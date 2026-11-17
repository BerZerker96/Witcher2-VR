// Start-up fix: The Witcher 2 hangs on its medallion splash on CPUs with many logical processors.
//
// witcher2.exe 0x930A6E (called once at start-up from 0x93209E) reads the CPU topology:
//     GetProcessAffinityMask(proc, &procMask, &sysMask);
//     if (procMask != sysMask) return 8;                 // 0x930AAD: cmp / je - the early exit
//     for (bit = 1; bit <= sysMask; bit = 1 << ++n) {    // 0x930ACF .. 0x930BC4
//         SetThreadAffinityMask(thread, bit); cpuid ...; strcat(topology string at 0x1404BF8, ...);
//     }
// In a 32-bit process a machine with 32+ logical processors has sysMask = 0xFFFFFFFF, which no bit can exceed, and
// x86 wraps 1 << 32 back to 1 - the loop never ends (and the string it appends to keeps growing). That is why the
// community fix is "start /affinity FF": any affinity restriction makes procMask != sysMask and takes the early exit.
//
// Here only that one call is answered differently: when the caller is the topology routine (its exact bytes follow
// the call: mov eax,[ebp-8]; cmp eax,[ebp-4]; je +7; mov al,8) and the machine has more than 16 logical processors,
// the reported process mask has its highest bit cleared - the routine takes its own early exit, exactly the path the
// affinity workaround leads to, while the game keeps every core (its real affinity is not changed).
#include <windows.h>
#include <intrin.h>
#include <cstring>
#include "log.h"
#include "config.h"

bool PatchImport(HMODULE mod, const char* dll, void* target, void* repl);   // d3d9hooks.cpp

namespace {
typedef BOOL (WINAPI* GpamFn)(HANDLE, PDWORD_PTR, PDWORD_PTR);
GpamFn oGpam = nullptr;
volatile LONG g_faked = 0;
int g_minCpus = 17;
const uint8_t kTopologyTail[] = { 0x8B, 0x45, 0xF8, 0x3B, 0x45, 0xFC, 0x74, 0x07, 0xB0, 0x08 };

int Bits(DWORD_PTR m) { int n = 0; while (m) { n += (int)(m & 1); m >>= 1; } return n; }

BOOL WINAPI hGpam(HANDLE proc, PDWORD_PTR procMask, PDWORD_PTR sysMask) {
    BOOL ok = oGpam(proc, procMask, sysMask);
    const uint8_t* ret = (const uint8_t*)__builtin_return_address(0);
    if (ok && procMask && sysMask && *procMask == *sysMask && Bits(*sysMask) >= g_minCpus &&
        !IsBadReadPtr(ret, sizeof kTopologyTail) && !memcmp(ret, kTopologyTail, sizeof kTopologyTail)) {
        DWORD_PTR top = *sysMask;
        DWORD_PTR hi = 1; while (top >>= 1) hi <<= 1;   // highest set bit
        *procMask &= ~hi;
        if (InterlockedIncrement(&g_faked) == 1)
            LOGI("compat: %d logical processors - the game's CPU topology loop (0x930A6E) would never end on this machine; it takes its own early exit (the 'start /affinity' fix, without losing any core)",
                 Bits(*sysMask));
    }
    return ok;
}
}


// ---- crash log: the game installs its own unhandled-exception filter (its crash reporter) over the mod's, and that
// one exits without a word in w2vr.log. Its call is intercepted: the mod's filter stays installed, logs the crash
// (module + offset, registers, stack) and then hands the exception to the game's filter.
LPTOP_LEVEL_EXCEPTION_FILTER g_gameCrashFilter = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hSetUef(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_gameCrashFilter;
    g_gameCrashFilter = f;
    static int n = 0; if (n++ < 3) LOGI("compat: the game's crash handler chained behind the mod's crash log");
    return prev;
}

void W2CompatInstall() {
    {   void* r = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetUnhandledExceptionFilter");
        if (r) PatchImport(GetModuleHandleW(nullptr), "kernel32.dll", r, (void*)hSetUef); }
    if (!ConfigBool("fix_many_core_hang", true)) { LOGI("compat: many-core start-up fix off (fix_many_core_hang = false)"); return; }
    g_minCpus = ConfigInt("many_core_threshold", 17);
    SYSTEM_INFO si; GetSystemInfo(&si);
    DWORD_PTR pm = 0, sm = 0; GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    void* real = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessAffinityMask");
    if (real && PatchImport(GetModuleHandleW(nullptr), "kernel32.dll", real, (void*)hGpam)) {
        oGpam = (GpamFn)real;
        LOGI("compat: many-core start-up fix ready (%lu processors visible, %d in the system mask%s)", si.dwNumberOfProcessors, Bits(sm),
             Bits(sm) >= g_minCpus ? " - the fix will apply" : " - not needed on this machine");
    } else LOGW("compat: could not redirect GetProcessAffinityMask - on 32+ thread CPUs use W2VR_Launch.bat");
}
