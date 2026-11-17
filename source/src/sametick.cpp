// Same-tick AER for The Witcher 2, done natively in REDengine's main loop.
//
// Alternate-eye rendering draws one eye per game frame. In plain AER each eye shows a different moment, so anything
// that moves is in a different place in each eye (doubled edges in motion). Same tick: the game's time stands still
// for the second eye of each pair, so both eyes show the same moment of the world, like rendering both in one frame.
//
// Why not the Assassin's Creed method (freezing QueryPerformanceCounter): REDengine's main loop (0xAA5E2A) computes
//     dt = clock() - last;  dt = min(dt, 0.1);  dt = max(dt, 0.2667 * previous dt);  if (dt > 0) Tick(dt)
// A frozen clock gives dt = 0, the floor turns that into 27 % of the previous frame (the world still moves and the
// game runs fast), and a true 0 skips Tick - no frame, no camera build for the second eye. So the hook sits where the
// loop stores its final dt (0xAA5ED5) and decides it per frame:
//     first eye of a pair:  dt = real dt + what the second eye did not use   (game time stays exact)
//     second eye:           dt = 1e-5 s (the tick runs, the world does not move), its real dt is banked
// The engine clock (0xAC103A, REDengine's "seconds since start", 98 callers: timers, animation, effects) is held at the
// first eye's time during the second eye's frame - except for the main loop's own read (its return address), which
// keeps measuring real time. The loop also gives the camera hook a deterministic eye per game frame.
#include <windows.h>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "MinHook.h"
#include "w2vr.h"
#include "config.h"
#include "log.h"

extern "C" {
volatile uint8_t g_stHold = 0;     // clock stub: return g_stHeld (read by asm)
double g_stHeld = 0;
uintptr_t g_stLoopRet = 0;         // return address of the main loop's own clock read (always real)
void* g_oClock = nullptr;          // trampoline of the engine clock
uintptr_t g_stLoopBack = 0;        // main loop: continue at the jbe after the replaced instructions
void __cdecl w2vr_LoopHook(uint8_t* frame);
}
namespace {
constexpr float kEps = 1e-5f;
std::string g_mode = "same_tick";
bool g_installed = false, g_clockHooked = false, g_clockHold = true;
volatile unsigned g_loopCount = 0;   // game ticks since start (never reset)
volatile int g_eye = 1; volatile unsigned g_pair = 0; volatile bool g_heldFrame = false;
float g_bank = 0; double g_pairTime = 0;
volatile LONG g_frames = 0, g_held = 0, g_clockHeldReads = 0;
double g_sumGiven = 0, g_sumReal = 0;

uint8_t* FindUnique(const char* pattern, int* count) {
    std::vector<int> b;
    for (const char* s = pattern; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '?') { b.push_back(-1); s += 2; continue; }
        b.push_back((int)strtoul(std::string(s, 2).c_str(), nullptr, 16)); s += 2;
    }
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* hit = nullptr; int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p0 = base + sec[i].VirtualAddress; size_t len = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + b.size() <= len; k++) {
            if (p0[k] != (uint8_t)b[0]) continue;
            size_t j = 1;
            for (; j < b.size(); j++) if (b[j] >= 0 && p0[k + j] != (uint8_t)b[j]) break;
            if (j == b.size()) { if (!hit) hit = p0 + k; n++; }
        }
    }
    *count = n;
    return n == 1 ? hit : nullptr;
}
bool g_testForce = false;
// "full" runs as same tick whenever full stereo itself is not drawing (hook not found, or its watchdog gave up)
bool Active() { if (g_testForce) return true; return (g_mode == "same_tick" || (g_mode == "full" && !W2FullStereoOn())) && g_vr.mode == VRMODE_STEREO && W2CamActive() && !W2GameMenuOpen(); }
} // namespace

// called from the main loop with its frame (esp at the hook): [+0x0C] float dt (final, clamped), [+0x18] double clock
extern "C" void __cdecl w2vr_LoopHook(uint8_t* frame) {
    float* dt = (float*)(frame + 0x0C);
    double t1; memcpy(&t1, frame + 0x18, 8);
    InterlockedIncrement(&g_frames);
    g_loopCount++;
    g_eye ^= 1;
    if (!Active()) {
        g_stHold = 0; g_heldFrame = false;
        if (g_bank > 0) { float d = *dt + g_bank; *dt = d > 0.15f ? 0.15f : d; g_bank = 0; }
        if (g_eye == 0) g_pair++;
        return;
    }
    float real = *dt;
    g_sumReal += real;
    if (g_eye == 1) {   // second eye: the world stands still
        g_bank += real - kEps;   // what the frozen frame did not use, paid out with the next first eye
        *dt = kEps;
        g_stHeld = g_pairTime + kEps;
        g_stHold = g_clockHold ? 1 : 0;
        g_heldFrame = true;
        InterlockedIncrement(&g_held);
    } else {            // first eye: real time again, plus what the second eye did not use
        g_stHold = 0; g_heldFrame = false;
        float d = real + g_bank;
        if (d < kEps) d = kEps;
        if (d > 0.15f) d = 0.15f;
        *dt = d; g_bank = 0; g_pairTime = t1; g_pair++;
    }
    g_sumGiven += *dt;
}

__asm__(".intel_syntax noprefix\n"
        // engine clock: held value during the second eye's frame, except for the main loop's own read
        ".globl _w2vr_stub_clock\n"
        "_w2vr_stub_clock:\n"
        "  cmp byte ptr [_g_stHold], 0\n"
        "  je 1f\n"
        "  push eax\n"
        "  mov eax, dword ptr [esp+4]\n"
        "  cmp eax, dword ptr [_g_stLoopRet]\n"
        "  pop eax\n"
        "  je 1f\n"
        "  fld qword ptr [_g_stHeld]\n"
        "  ret\n"
        "1:\n"
        "  jmp dword ptr [_g_oClock]\n"
        // main loop dt (replaces movss xmm1,[esp+0Ch]; movss [esi+10h],xmm1 - the flags of the comisd before it and
        // xmm0..7 are preserved for the loop)
        ".globl _w2vr_stub_loop\n"
        "_w2vr_stub_loop:\n"
        "  pushfd\n"
        "  pushad\n"
        "  sub esp, 128\n"
        "  movups xmmword ptr [esp], xmm0\n"
        "  movups xmmword ptr [esp+16], xmm1\n"
        "  movups xmmword ptr [esp+32], xmm2\n"
        "  movups xmmword ptr [esp+48], xmm3\n"
        "  movups xmmword ptr [esp+64], xmm4\n"
        "  movups xmmword ptr [esp+80], xmm5\n"
        "  movups xmmword ptr [esp+96], xmm6\n"
        "  movups xmmword ptr [esp+112], xmm7\n"
        "  lea eax, [esp+164]\n"            // the loop's esp: 128 + 32 (pushad) + 4 (pushfd)
        "  push eax\n"
        "  call _w2vr_LoopHook\n"
        "  add esp, 4\n"
        "  movups xmm0, xmmword ptr [esp]\n"
        "  movups xmm1, xmmword ptr [esp+16]\n"
        "  movups xmm2, xmmword ptr [esp+32]\n"
        "  movups xmm3, xmmword ptr [esp+48]\n"
        "  movups xmm4, xmmword ptr [esp+64]\n"
        "  movups xmm5, xmmword ptr [esp+80]\n"
        "  movups xmm6, xmmword ptr [esp+96]\n"
        "  movups xmm7, xmmword ptr [esp+112]\n"
        "  add esp, 128\n"
        "  popad\n"
        "  popfd\n"
        "  movss xmm1, dword ptr [esp+0x0C]\n"
        "  movss dword ptr [esi+0x10], xmm1\n"
        "  jmp dword ptr [_g_stLoopBack]\n"
        ".att_syntax\n");
extern "C" void w2vr_stub_clock();
extern "C" void w2vr_stub_loop();

void SameTickInstall() { g_mode = ConfigStr("stereo_method", "auto"); g_clockHold = ConfigBool("same_tick_clock", true); }
void SameTickInstallNative(const char* when) {
    if (g_installed) return;
    int n1 = 0, n2 = 0, n3 = 0;
    uint8_t* loop = FindUnique("F3 0F 10 4C 24 0C F3 0F 11 4E 10 76 26 0F 57 D2 66 0F 28 D8", &n1);
    uint8_t* read = FindUnique("E8 ?? ?? ?? ?? DD 5C 24 18 F2 0F 10 44 24 18 DD 44 24 18 F2 0F 5C 44 24 10", &n2);
    uint8_t* clk = FindUnique("55 8B EC 51 51 8D 45 F8 50 FF 15 ?? ?? ?? ?? 32 D2 38 15 ?? ?? ?? ?? 74 08 DD 05", &n3);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!loop) { if (!strcmp(when, "device")) LOGW("same tick: main loop not found (%d matches) - plain alternate-eye rendering", n1); return; }
    g_stLoopBack = (uintptr_t)(loop + 11);
    uint8_t j[11] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    int32_t rel = (int32_t)((uintptr_t)w2vr_stub_loop - ((uintptr_t)loop + 5)); memcpy(j + 1, &rel, 4);
    DWORD old;
    if (!VirtualProtect(loop, 11, PAGE_EXECUTE_READWRITE, &old)) { LOGE("same tick: cannot write the main loop"); return; }
    memcpy(loop, j, 11); VirtualProtect(loop, 11, old, &old); FlushInstructionCache(GetCurrentProcess(), loop, 11);
    g_installed = true;
    if (clk && read) {
        g_stLoopRet = (uintptr_t)(read + 5);
        if (MH_CreateHook(clk, (void*)w2vr_stub_clock, &g_oClock) == MH_OK && MH_EnableHook(clk) == MH_OK) g_clockHooked = true;
    }
    LOGI("same tick: main loop dt at exe+%lx, engine clock at exe+%lx %s (%s) - stereo method %s", (unsigned long)(loop - base),
         clk ? (unsigned long)(clk - base) : 0ul, g_clockHooked ? "held for the second eye" : "NOT hooked", when, g_mode.c_str());
}
void SameTickSetMode(const std::string& m) {
    if (m != g_mode) LOGI("same tick: stereo method %s", m.c_str());
    g_mode = m;
}
bool SameTickAvailable() { return g_installed; }
int SameTickFrameEye() { return g_installed ? g_eye : -1; }
unsigned SameTickPair() { return g_pair; }
unsigned SameTickLoopCount() { return g_installed ? g_loopCount + 1 : 0; }   // 0: the game loop is not hooked
bool SameTickHeld() { return g_installed && g_heldFrame; }
bool SameTickOn() { return g_installed && Active(); }   // "full" (full stereo) is not same tick
void SameTickNextFrame(bool, int) {}
void SameTickCamera(const float*) {}
void SameTickReport() {
    if (!g_installed) return;
    LONG f = InterlockedExchange(&g_frames, 0), h = InterlockedExchange(&g_held, 0);
    LOGI("same tick: %s, %ld game frames (%ld held for the second eye), game time given %.2f s for %.2f s real%s", Active() ? "active" : (g_mode == "aer" ? "off (aer)" : W2FullStereoOn() ? "off (full stereo drawing both eyes)" : "waiting for VR stereo"),
         f, h, g_sumGiven, g_sumReal, g_clockHooked ? "" : ", engine clock not held");
    g_sumGiven = g_sumReal = 0;
}

// test export (tests/check_sametick.py): wire the stubs to test code instead of the game
extern "C" __declspec(dllexport) void __stdcall W2VR_TestSameTickSetup(void* clockOrig, uintptr_t loopRet, uintptr_t loopBack) {
    g_oClock = clockOrig; g_stLoopRet = loopRet; g_stLoopBack = loopBack; g_installed = true; g_testForce = true; g_clockHold = true;
    g_eye = 1; g_bank = 0; g_pairTime = 0; g_stHold = 0; g_sumGiven = g_sumReal = 0;
}
extern "C" __declspec(dllexport) int __stdcall W2VR_TestSameTickEye() { return g_eye; }
