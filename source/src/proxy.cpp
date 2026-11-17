// xinput1_3.dll proxy: the real DLL is loaded from the system folder and every export jumps to it.
// The Witcher 2 (witcher2.exe) imports XINPUT1_3 ordinals 2 (XInputGetState) and 3 (XInputSetState).
#include <windows.h>
#include <string>
#include <cwctype>

extern "C" { FARPROC g_px[12] = {}; }

struct ProxyName { const char* name; WORD ordinal; };
static const ProxyName kXInput[] = {
    {"XInputGetState",0},{"XInputSetState",0},{"XInputGetCapabilities",0},{"XInputEnable",0},
    {"XInputGetDSoundAudioDeviceGuids",0},{"XInputGetBatteryInformation",0},{"XInputGetKeystroke",0},
    {nullptr,100},{nullptr,101},{nullptr,102},{nullptr,103} };

std::wstring g_proxyName;

void ProxyLoad(HMODULE self) {
    wchar_t me[MAX_PATH]; GetModuleFileNameW(self, me, MAX_PATH);
    std::wstring n = me; n = n.substr(n.find_last_of(L'\\') + 1);
    for (auto& c : n) c = (wchar_t)towlower(c);
    g_proxyName = n;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE real = LoadLibraryW((std::wstring(sys) + L"\\xinput1_3.dll").c_str());
    if (!real) real = LoadLibraryW((std::wstring(sys) + L"\\xinput1_4.dll").c_str());   // Windows without the DirectX runtime
    if (!real) return;
    for (int i = 0; i < 11; i++) {
        const ProxyName& p = kXInput[i];
        g_px[i] = p.name ? GetProcAddress(real, p.name) : GetProcAddress(real, MAKEINTRESOURCEA(p.ordinal));
    }
}

// export stubs: jmp [g_px + i*4]; a function the system DLL lacks returns ERROR_DEVICE_NOT_CONNECTED (1167)
#define PX(i) __asm__(".globl _Proxy_" #i "\n_Proxy_" #i ":\n  movl _g_px+" #i "*4, %eax\n  testl %eax, %eax\n  jz 1f\n  jmp *%eax\n1:\n  movl $1167, %eax\n  ret $" PXARGS_##i "\n");
#define PXARGS_0 "8"
#define PXARGS_1 "8"
#define PXARGS_2 "12"
#define PXARGS_3 "4"
#define PXARGS_4 "12"
#define PXARGS_5 "12"
#define PXARGS_6 "12"
#define PXARGS_7 "8"
#define PXARGS_8 "12"
#define PXARGS_9 "4"
#define PXARGS_10 "4"
PX(0) PX(1) PX(2) PX(3) PX(4) PX(5) PX(6) PX(7) PX(8) PX(9) PX(10)
