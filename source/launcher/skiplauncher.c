// W2VR skip-launcher: stands in for the game's Launcher.exe (renamed Launcher_original.exe by the installer), so
// Steam / GOG "Play" goes straight into bin\witcher2.exe (and VR) without the launcher window.
// Hold SHIFT while starting to get the original launcher (Configurator, DLC, etc.).
// Started by Steam, so the Steam environment (SteamAppId) is inherited and the Steam exe's DRM is satisfied.
#include <windows.h>
#include <wchar.h>

static void Msg(const wchar_t* t) { MessageBoxW(NULL, t, L"Witcher 2 VR - skip launcher", MB_OK | MB_ICONWARNING); }

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmdArgs, int show) {
    wchar_t self[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], bin[MAX_PATH], orig[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy(dir, self); wchar_t* s = wcsrchr(dir, L'\\'); if (s) *s = 0;
    _snwprintf(orig, MAX_PATH, L"%s\\Launcher_original.exe", dir);
    _snwprintf(bin, MAX_PATH, L"%s\\bin", dir);
    _snwprintf(exe, MAX_PATH, L"%s\\bin\\witcher2.exe", dir);
    BOOL wantOrig = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const wchar_t* target = exe; const wchar_t* cwd = bin;
    if (wantOrig || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
        if (!wantOrig) { Msg(L"bin\\witcher2.exe was not found next to this Launcher.exe - starting the original launcher."); }
        if (GetFileAttributesW(orig) == INVALID_FILE_ATTRIBUTES) { Msg(L"Launcher_original.exe not found. Run W2VR_SkipLauncher_Uninstall.bat or verify the game files."); return 1; }
        target = orig; cwd = dir;
    }
    // command line: "target" + the arguments this launcher got (none normally)
    size_t n = wcslen(target) + (cmdArgs ? wcslen(cmdArgs) : 0) + 8;
    wchar_t* cl = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, n * sizeof(wchar_t));
    _snwprintf(cl, n, L"\"%s\"%s%s", target, (cmdArgs && *cmdArgs) ? L" " : L"", cmdArgs ? cmdArgs : L"");
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(target, cl, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) {
        wchar_t m[600]; _snwprintf(m, 600, L"Could not start %s (error %lu).", target, GetLastError()); Msg(m); return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);   // Steam shows the game as running until it exits
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
    return (int)code;
}
