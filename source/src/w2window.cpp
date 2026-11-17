// The game window for VR (The Witcher 2 creates it with CreateWindowExW and supports windowed mode itself).
//  * D3D: d3d9hooks.cpp sets Windowed = TRUE (force_windowed), w2ini.cpp sets User.ini Fullscreen off.
//  * The window's client area is the game's real resolution - the headset's per-eye size and shape - even when that is
//    larger than the monitor (window_scale = 1, the default). Windows normally caps a window at the screen size
//    through WM_GETMINMAXINFO, and it sends that message from inside CreateWindowExW, before a window can be
//    subclassed; the game then resizes its viewport to the clamped client rect (witcher2.exe 0x9D22F8: GetClientRect
//    -> viewport resize). So a WH_CBT hook subclasses the game's window at HCBT_CREATEWND - before the first
//    WM_GETMINMAXINFO - lifts the size cap, and sets the creation size to the exact client size. The process is made
//    DPI-aware, so display scaling does not stretch the window and its pixels stay 1:1.
//  * window_scale = auto shrinks the window to fit the monitor instead (D3D scales the picture into it; the headset
//    still gets the full frame). In both cases the game is told its window has its own resolution (GetClientRect,
//    WM_SIZE) from the first moment, and mouse positions are scaled back (ScreenToClient).
//  * Mouse: positions are mapped from the part of the window that is on the monitor to the whole game picture
//    (mouse_fit_screen), so a window taller / wider than the screen still has every menu button in reach - the
//    cursor cannot leave the monitor, but the headset shows the whole picture.
//  * Focus (keep_running_unfocused): clicking the SteamVR / desktop window must not pause or minimise the game.
// The game's imports are patched in its import table (DllMain-safe).
#include <windows.h>
#include <string>
#include <cstdlib>
#include "log.h"
#include "config.h"
#include "w2vr.h"

bool PatchImport(HMODULE mod, const char* dll, void* target, void* repl);   // d3d9hooks.cpp

namespace {
bool g_force = true, g_borderless = false, g_keepRunning = true, g_sizing = false, g_mouseFit = true;
float g_windowScale = 1;   // 0 = auto (fit the monitor), 1 = native (the game's resolution, larger than the screen if need be)
HHOOK g_cbt = nullptr;
HWND g_wnd = nullptr; WNDPROC g_gameProc = nullptr;
UINT g_fakeW = 0, g_fakeH = 0;     // the game's resolution (= back buffer)
UINT g_realW = 0, g_realH = 0;     // the client area it really has
const UINT WM_W2VR_REFIT = WM_APP + 0x3A;

typedef HWND (WINAPI* CreateWindowExWFn)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI* ShowWindowFn)(HWND, int);
typedef BOOL (WINAPI* RectFn)(HWND, LPRECT);
typedef BOOL (WINAPI* PointFn)(HWND, LPPOINT);
CreateWindowExWFn oCreateWindowExW; ShowWindowFn oShowWindow; RectFn oGetClientRect; PointFn oScreenToClient;

// a mouse position in the real client area -> the game's resolution. The visible client rect is the client area
// clipped to its monitor; it is stretched over the whole game picture.
void MapMouse(HWND h, LONG* x, LONG* y) {
    if (!g_fakeW || !g_realW || !g_realH) return;
    double vx0 = 0, vy0 = 0, vx1 = g_realW, vy1 = g_realH;
    if (g_mouseFit) {
        POINT o = { 0, 0 }; ClientToScreen(h, &o);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            const RECT& m = mi.rcMonitor;
            if (m.left - o.x > vx0) vx0 = m.left - o.x;
            if (m.top - o.y > vy0) vy0 = m.top - o.y;
            if (m.right - o.x < vx1) vx1 = m.right - o.x;
            if (m.bottom - o.y < vy1) vy1 = m.bottom - o.y;
            if (vx1 - vx0 < 16 || vy1 - vy0 < 16) { vx0 = vy0 = 0; vx1 = g_realW; vy1 = g_realH; }   // off screen: plain scale
        }
    }
    *x = (LONG)((*x - vx0) * g_fakeW / (vx1 - vx0)); *y = (LONG)((*y - vy0) * g_fakeH / (vy1 - vy0));
}

DWORD StyleFor(DWORD style) {
    style &= ~(WS_POPUP | WS_MAXIMIZE | WS_MINIMIZE | WS_OVERLAPPEDWINDOW);
    return style | (g_borderless ? WS_POPUP : (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX));
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_keepRunning) {
        switch (msg) {
        case WM_ACTIVATEAPP: if (!wp) return 0; break;
        case WM_ACTIVATE: if (LOWORD(wp) == WA_INACTIVE) return DefWindowProcW(h, msg, wp, lp); break;
        case WM_KILLFOCUS: return DefWindowProcW(h, msg, wp, lp);
        case WM_SIZE: if (wp == SIZE_MINIMIZED) return 0; break;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE || (wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER) return 0;
            break;
        }
    }
    if (g_force) {
        if (msg == WM_W2VR_REFIT) { if (g_fakeW) WindowFitClient(h, g_fakeW, g_fakeH); return 0; }
        if (msg == WM_GETMINMAXINFO) {   // no screen-size cap: the window may be larger than the monitor
            LRESULT r = CallWindowProcW(g_gameProc, h, msg, wp, lp);
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = 32767; mm->ptMaxSize.x = mm->ptMaxSize.y = 32767;
            return r;
        }
        if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MAXIMIZE) return 0;
        if (msg == WM_SIZE && wp != SIZE_MINIMIZED) {
            if (wp == SIZE_MAXIMIZED && !g_sizing) PostMessageW(h, WM_W2VR_REFIT, 0, 0);
            g_realW = LOWORD(lp); g_realH = HIWORD(lp);
            if (g_fakeW) { wp = SIZE_RESTORED; lp = MAKELPARAM(g_fakeW, g_fakeH); }   // the game sees its resolution
        }
        if (g_fakeW && g_realW && (msg == WM_MOUSEMOVE || (msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) || msg == WM_MOUSEWHEEL)) {
            if (msg != WM_MOUSEWHEEL) {   // client coordinates in the game's resolution (wheel: screen coordinates)
                LONG x = (short)LOWORD(lp), y = (short)HIWORD(lp);
                MapMouse(h, &x, &y);
                lp = MAKELPARAM((WORD)(short)x, (WORD)(short)y);
            }
        }
        if (msg == WM_WINDOWPOSCHANGING) {
            auto* p = (WINDOWPOS*)lp;
            if (!(p->flags & SWP_NOZORDER) && p->hwndInsertAfter == HWND_TOPMOST) p->hwndInsertAfter = HWND_NOTOPMOST;
        }
    }
    return CallWindowProcW(g_gameProc, h, msg, wp, lp);
}

void Adopt(HWND h) {
    if (!h || h == g_wnd) return;
    if (g_wnd && g_gameProc && IsWindow(g_wnd)) { SetWindowLongW(g_wnd, GWL_WNDPROC, (LONG)(LONG_PTR)g_gameProc); LOGI("window: %p released (the device uses another window)", (void*)g_wnd); }
    g_wnd = h;
    g_gameProc = (WNDPROC)SetWindowLongW(h, GWL_WNDPROC, (LONG)(LONG_PTR)WndProc);
    LOGI("window: game window %p adopted", (void*)h);
}

// window size for a client of cw x ch with these styles, and where it goes: centred if it fits the monitor's work area,
// else at its top-left corner (title bar on screen, the rest past the bottom / right edge)
void WindowRectFor(UINT cw, UINT ch, DWORD st, DWORD ex, HMONITOR mon, RECT* out) {
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    RECT r = { 0, 0, (LONG)cw, (LONG)ch }; AdjustWindowRectEx(&r, st, FALSE, ex);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    *out = { x, y, x + ww, y + wh };
}
UINT ScaledClient(UINT v) { return g_windowScale > 0 ? (UINT)(v * g_windowScale + 0.5f) : v; }

// WH_CBT during the game's CreateWindowExW: subclass at HCBT_CREATEWND (before WM_GETMINMAXINFO / WM_NCCREATE) and
// give the window its full size
LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND && !g_wnd) {
        HWND h = (HWND)wp; auto* cw = (CBT_CREATEWNDW*)lp;
        CREATESTRUCTW* cs = cw->lpcs;
        if (cs && !cs->hwndParent && !(cs->style & WS_CHILD)) {
            Adopt(h);
            if (g_fakeW && g_fakeH && g_windowScale > 0) {
                RECT r; WindowRectFor(ScaledClient(g_fakeW), ScaledClient(g_fakeH), cs->style, cs->dwExStyle, MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &r);
                cs->x = r.left; cs->y = r.top; cs->cx = r.right - r.left; cs->cy = r.bottom - r.top;
                g_realW = ScaledClient(g_fakeW); g_realH = ScaledClient(g_fakeH);
                LOGI("window: created with client %ux%u (game resolution %ux%u) at %d,%d", g_realW, g_realH, g_fakeW, g_fakeH, r.left, r.top);
            }
        }
    }
    return CallNextHookEx(g_cbt, code, wp, lp);
}

HWND WINAPI hCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    // the game's viewport window (class "W2ViewportClass"); helper windows (input, audio, overlays) are left alone
    bool cls_ok = cls && !IS_INTRESOURCE(cls) && !_wcsicmp(cls, L"W2ViewportClass");
    bool main = !g_wnd && !parent && !(style & WS_CHILD) && (cls_ok || (w >= 640 && h >= 400 && w != CW_USEDEFAULT));
    if (main && g_force) {
        DWORD ns = StyleFor(style), nex = ex & ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
        if (!g_borderless) nex |= WS_EX_APPWINDOW;
        LOGI("window: game window %dx%d style %08lx ex %08lx -> style %08lx ex %08lx", w, h, style, ex, ns, nex);
        style = ns; ex = nex;
    }
    if (main && g_force && !g_cbt) g_cbt = SetWindowsHookExW(WH_CBT, CbtProc, nullptr, GetCurrentThreadId());
    HWND r = oCreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (g_cbt && (r || !main)) { UnhookWindowsHookEx(g_cbt); g_cbt = nullptr; }
    if (main && r) {
        Adopt(r);
        if (g_fakeW && g_windowScale > 0) {   // in case the CBT hook was not available: size it now (no cap any more)
            RECT cr; oGetClientRect(r, &cr);
            if ((UINT)(cr.right - cr.left) != ScaledClient(g_fakeW) || (UINT)(cr.bottom - cr.top) != ScaledClient(g_fakeH)) WindowFitClient(r, g_fakeW, g_fakeH);
        }
    }
    return r;
}
BOOL WINAPI hShowWindow(HWND h, int cmd) {
    if (h && h == g_wnd && g_keepRunning && (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) return TRUE;
    if (h && h == g_wnd && g_force && cmd == SW_MAXIMIZE) cmd = SW_RESTORE;
    return oShowWindow(h, cmd);
}
BOOL WINAPI hGetClientRect(HWND h, LPRECT r) {
    BOOL ok = oGetClientRect(h, r);
    if (ok && r && g_force && h && h == g_wnd && g_fakeW) { r->left = r->top = 0; r->right = (LONG)g_fakeW; r->bottom = (LONG)g_fakeH; }
    return ok;
}
BOOL WINAPI hScreenToClient(HWND h, LPPOINT p) {
    BOOL ok = oScreenToClient(h, p);
    if (ok && p && g_force && h && h == g_wnd && g_fakeW && g_realW && g_realH) MapMouse(h, &p->x, &p->y);
    return ok;
}
} // namespace

// client area w x h (the back buffer), shrunk to fit the monitor when it is larger (window_scale = auto)
void WindowFitClient(HWND hwnd, UINT w, UINT h) {
    if (!g_force || !w || !h) return;
    if (!hwnd) hwnd = g_wnd;
    if (!hwnd) return;
    if (!g_wnd) Adopt(hwnd);
    if (hwnd != g_wnd) return;
    g_fakeW = w; g_fakeH = h;
    if (IsZoomed(hwnd)) oShowWindow(hwnd, SW_RESTORE);
    DWORD st = (DWORD)GetWindowLongW(hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (st & WS_POPUP && !g_borderless) { st = StyleFor(st) | WS_VISIBLE; SetWindowLongW(hwnd, GWL_STYLE, (LONG)st); }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    float sc = 1.f;
    if (g_windowScale > 0) sc = g_windowScale;
    else {   // auto: fit 85 % of the monitor
        MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi); const RECT& wa = mi.rcWork;
        RECT fr = { 0, 0, 100, 100 }; AdjustWindowRectEx(&fr, st, FALSE, ex);
        float aw = 0.85f * (wa.right - wa.left) - (fr.right - fr.left - 100), ah = 0.85f * (wa.bottom - wa.top) - (fr.bottom - fr.top - 100);
        if (w > aw || h > ah) sc = aw / w < ah / h ? aw / w : ah / h;
    }
    UINT cw = (UINT)(w * sc + 0.5f), ch = (UINT)(h * sc + 0.5f);
    if (cw < 160) cw = 160;
    if (ch < 90) ch = 90;
    RECT r; WindowRectFor(cw, ch, st, ex, mon, &r);
    int x = r.left, y = r.top, ww = r.right - r.left, wh = r.bottom - r.top;
    g_sizing = true;
    SetWindowPos(hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    g_sizing = false;
    {   RECT got; oGetClientRect(hwnd, &got);   // what Windows really gave (should be the full size: no cap)
        if ((UINT)(got.right - got.left) != cw || (UINT)(got.bottom - got.top) != ch)
            LOGW("window: asked for client %ux%u, Windows gave %ldx%ld - the game still renders %ux%u, D3D scales it into the window", cw, ch, got.right - got.left, got.bottom - got.top, w, h);
        else cw = got.right - got.left, ch = got.bottom - got.top; }
    g_realW = cw; g_realH = ch;
    LOGI("window: client %ux%u at %d,%d (game resolution %ux%u, scale %.2f%s)", cw, ch, x, y, w, h, sc, sc == 1.f ? ", native" : "");
}

HWND WindowGame() { return g_wnd; }

// the game's LoadLibraryA("nvapi.dll") fails: its NVIDIA 3D Vision path (0x82AEE0) never starts
typedef HMODULE (WINAPI* LoadLibraryAFn)(LPCSTR);
static LoadLibraryAFn oLoadLibraryA;
static HMODULE WINAPI hLoadLibraryA(LPCSTR name) {
    if (name) {
        const char* b = strrchr(name, '\\'); b = b ? b + 1 : name;
        if (!_stricmp(b, "nvapi.dll")) { static bool logged = false; if (!logged) { logged = true; LOGI("stereo: the game's NVIDIA 3D Vision (nvapi.dll) kept off - block_nvidia_stereo"); } SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }
    }
    return oLoadLibraryA(name);
}
bool W2BlockNvapiInstall() {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    void* real = (void*)GetProcAddress(k, "LoadLibraryA");
    oLoadLibraryA = (LoadLibraryAFn)real;
    bool ok = real && PatchImport(GetModuleHandleW(nullptr), "kernel32.dll", real, (void*)hLoadLibraryA);
    LOGI("stereo: game import of LoadLibraryA %s (nvapi.dll blocked for the game)", ok ? "redirected" : "NOT found");
    return ok;
}
void WindowNoteDeviceWindow(HWND h) { Adopt(h); }

void WindowHooksInstall() {
    g_force = ConfigBool("force_windowed", true);
    g_borderless = ConfigStr("window_mode", "bordered") == "borderless";
    g_keepRunning = ConfigBool("keep_running_unfocused", true);
    g_mouseFit = ConfigBool("mouse_fit_screen", true);
    {   std::string ws = ConfigStr("window_scale", "1");
        if (ws == "auto") g_windowScale = 0.f;   // fit the monitor
        else { g_windowScale = ws == "native" ? 1.f : (float)atof(ws.c_str()); if (g_windowScale < 0.05f || g_windowScale > 4.f) g_windowScale = 1.f; } }
    // the game is told its resolution from the first moment (its viewport follows GetClientRect)
    { unsigned rw, rh; if (g_force && W2IniResolution(&rw, &rh)) { g_fakeW = rw; g_fakeH = rh; g_realW = ScaledClient(rw); g_realH = ScaledClient(rh); } }
    // DPI-aware: Windows display scaling must not stretch the window (1 window pixel = 1 picture pixel)
    if (ConfigBool("dpi_aware", true)) {
        typedef BOOL (WINAPI* CtxFn)(HANDLE); typedef BOOL (WINAPI* AwFn)();
        bool ok = false;
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (auto f = (CtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext")) ok = f((HANDLE)-4) != FALSE;   // PER_MONITOR_AWARE_V2
        if (!ok) if (auto f = (AwFn)GetProcAddress(u32, "SetProcessDPIAware")) ok = f() != FALSE;
        LOGI("window: DPI-aware %s", ok ? "yes" : "no (already set or unsupported)");
    }
    HMODULE exe = GetModuleHandleW(nullptr), u = GetModuleHandleW(L"user32.dll");
    if (!u) u = LoadLibraryW(L"user32.dll");
    struct { const char* name; void* hook; void** orig; } t[] = {
        { "CreateWindowExW", (void*)hCreateWindowExW, (void**)&oCreateWindowExW },
        { "ShowWindow", (void*)hShowWindow, (void**)&oShowWindow },
        { "GetClientRect", (void*)hGetClientRect, (void**)&oGetClientRect },
        { "ScreenToClient", (void*)hScreenToClient, (void**)&oScreenToClient },
    };
    int n = 0;
    for (auto& e : t) {
        void* real = (void*)GetProcAddress(u, e.name);
        if (!real) continue;
        *e.orig = real;
        if (PatchImport(exe, "user32.dll", real, e.hook)) n++;
    }
    LOGI("window: %d of 4 user32 imports redirected (windowed %d, %s, keep running unfocused %d, window %s, game resolution %ux%u)", n, g_force,
         g_borderless ? "borderless" : "bordered", g_keepRunning, g_windowScale == 0 ? "fits the monitor" : g_windowScale == 1.f ? "native size (may exceed the monitor)" : "scaled", g_fakeW, g_fakeH);
}
HWND W2GameWindow() { return g_wnd; }   // w2engine.cpp (input report, window to front)
