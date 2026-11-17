// Engine patches for The Witcher 2 (found by signature after SteamStub has decrypted the code).
//
// no_letterbox: since patch 1.1 the game forces its 3D picture to 16:9 (or an aspect from its table at 0xE46DB4:
// 16:9, 16:9, 4:3, 5:4, 16:10, 5:3, 48:9, 27:16) and fills the rest with black bars - the reason ultrawide / 16:10
// players needed Widescreen Fixer / Flawless Widescreen / the "39 8E E3 3F" hex edit. The viewport's set-size routine
// (0xAAEB8E) computes the bars with ftol into ebx (top) and edi (left), then sets the viewport rect
// (edi, ebx, w - 2 edi, h - 2 ebx). A near-square VR picture would be letterboxed to a 16:9 band (~40 % of the pixels
// lost). The two "mov reg, eax" after ftol become "xor reg, reg": the picture covers the whole frame at any aspect.
//
// no_dof (and motion blur / vignette / blur): depth of field off in gameplay AND cutscenes, whatever User.ini, the options menu or a cutscene asks for.
// Every frame the renderer builds its post-process mask (0x9BFFE8, esi = frame info, result in eax) by ANDing what
// the frame / environment / cutscene requests (frame +0x2798 DOF, +0x27A0 scatter DOF) with the settings object
// (getter 0x402254 -> [0x2580E28]; offsets from the config table at 0xB07714):
//     +0x54 AllowDOF -> 0x20 (gameplay DOF)   +0x5C AllowCutsceneDOF -> 0x40   +0x5D AllowScatterDOF -> 0x2000 (bokeh)
// There is no other path to the DOF passes. Each test is "cmp byte [eax+off], cl / je <bit = 0>": the je becomes a
// jmp, so the three DOF bits are never set. Toggled live from the panel (the bytes are swapped back and forth).
#include <windows.h>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "log.h"
#include "config.h"
#include "w2vr.h"
#include "MinHook.h"

namespace {
bool g_lbDone = false;
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
bool Write(uint8_t* at, const void* b, size_t n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, b, n); VirtualProtect(at, n, old, &old); FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}
} // namespace

namespace {
// post-process effects forced off in the renderer's mask (0x9BFFE8): each test "cmp byte [settings + off], cl / je"
// gets its je turned into a jmp, so the effect's bit is never set
struct EffectSite { const char* sig; int at; };
struct Effect { const char* name; EffectSite site[3]; int n; uint8_t* jcc[3]; bool found, off; };
Effect g_fx[W2FX_COUNT] = {
    { "depth of field (gameplay, cutscene and scatter)", { { "3A D1 74 0A 38 48 54 74 05 6A 20 5F", 7 },     // AllowDOF        -> 0x20
                                                           { "3A D1 74 0A 38 48 5C 74 05 6A 40 5A", 7 },     // AllowCutsceneDOF -> 0x40
                                                           { "38 48 5D 74 07 BA 00 20 00 00", 3 } }, 3 },    // AllowScatterDOF -> 0x2000
    { "motion blur", { { "38 48 62 74 07 BA 00 08 00 00", 3 } }, 1 },                                        // AllowMotionBlur -> 0x800
    { "vignette",    { { "38 48 5E 74 07 BA 80 00 00 00", 3 } }, 1 },                                        // AllowVignette   -> 0x80
    { "fullscreen blur", { { "38 48 53 74 05 6A 10 5A", 3 } }, 1 },                                          // AllowBlur       -> 0x10
    { "SSAO", { { "38 48 61 74 07 BA 00 04 00 00", 3 } }, 1 },                                                // AllowSSAO       -> 0x400
    { "sharpen", { { "38 48 5F 74 07 BA 00 01 00 00", 3 } }, 1 },                                             // AllowSharpen    -> 0x100
};
bool g_fxSearched = false;
// the renderer's second DOF path (0x9C0348): when the frame's normal post-processing is off (frame info +0x2767) but
// +0x278D is set, the mask is written as a constant 0x2060 = gameplay + cutscene + scatter DOF, bypassing every
// setting - patched to 0 with the DOF switch
uint8_t* g_dofOnlyImm = nullptr;
}

bool W2EngineSetEffectOff(int fx, bool off) {
    if (fx < 0 || fx >= W2FX_COUNT || !g_fx[fx].found) return false;
    Effect& e = g_fx[fx];
    uint8_t op = off ? 0xEB : 0x74;   // jmp / je (both short, same displacement)
    bool ok = true;
    for (int i = 0; i < e.n; i++) ok &= Write(e.jcc[i], &op, 1);
    if (fx == W2FX_DOF && g_dofOnlyImm) { uint32_t v = off ? 0u : 0x2060u; ok &= Write(g_dofOnlyImm, &v, 4); }
    if (ok && off != e.off) LOGI("engine: %s %s", e.name, off ? "forced OFF" : "back to the game's settings");
    e.off = off && ok;
    return ok;
}
bool W2EngineSetNoDof(bool on) { return W2EngineSetEffectOff(W2FX_DOF, on); }

// ---------------------------------------------------------------- the game's pause state (menus -> flat screen)
// The script natives IsPaused (0xAA72B1) and IsActivelyPaused (0xAA78D6) read the CGame object: +0x5C pause count,
// +0x60 "active pause" count, both void while the byte at 0x160868F is set. The engine keeps the game object at
// [0x1404950] (0x9A8B46: mov eax, [GGame]; cmp [eax+0x5C], 0; ...; cmp byte [0x160868F], 0). The pause menu,
// inventory, map, journal and character panels pause the game.
namespace {
uint8_t** g_gamePP = nullptr; const volatile uint8_t* g_pauseOff = nullptr;
bool g_flatPaused = true, g_flatActive = false, g_pauseFound = false;
}
bool W2GamePaused(bool* activePause) {
    if (activePause) *activePause = false;
    if (!g_pauseFound) return false;
    uint8_t* g = *g_gamePP;
    if (!g || IsBadReadPtr(g + 0x5C, 8)) return false;
    if (*g_pauseOff) return false;
    if (activePause) *activePause = *(volatile int*)(g + 0x60) > 0;
    return *(volatile int*)(g + 0x5C) > 0;
}
bool W2GameMenuOpen() {
    if (!g_flatPaused) return false;
    bool active = false;
    bool p = W2GamePaused(&active);
    bool open = p && (!active || g_flatActive);
    static bool last = false;
    if (open != last) { last = open; LOGI("game: %s", open ? "paused (menu / inventory / map) - flat screen" : "running - VR"); }
    return open;
}
void W2SetFlatWhenPaused(bool on) { g_flatPaused = on; }
bool W2EngineNoDof() { return g_fx[W2FX_DOF].off; }


// ---------------------------------------------------------------- the game's dither textures (screen-door patterns)
// The renderer builds five small procedural textures at start-up with D3DXFillTexture (exe 0x53ECE0, slots at
// 0x1626630 + i * 0x244). Two of them are the screen-door patterns behind the grain in the headset:
//  - 0x53EF90: a 16 x 16 Bayer ordered-dither matrix (x / y bits interleaved -> threshold 1..255): the alpha threshold
//    of dithered blends - decal and ground layers, LOD cross-fades, "transparent" shadows;
//  - 0x53F140: 16 x 16 rotation angles in the same Bayer order ((cos + 1) / 2, (sin + 1) / 2): the per-pixel rotation of the
//    shadow filter's sample pattern (interleaved PCF).
// On a monitor Ubersampling (UberSampling >= 2: the frame drawn N x N times with sub-pixel camera offsets, 0xB14A8A)
// averages the patterns away; in a headset they are the grain. Their fill callbacks are swapped (the push before the
// call, 0x5424D6 / 0x542587) for ones that give every texel one value: the Bayer texel nearest 0.5 (a clean 50 %
// alpha test instead of a screen door) and the first rotation (one fixed shadow-filter pattern). Start-up only.
namespace {
typedef void (WINAPI* FillCb)(float* out, const float* tc, const float* ts, void* data);
FillCb g_oBayer = nullptr, g_oRot = nullptr;
float g_bayerConst[4], g_rotConst[4]; bool g_bayerInit = false, g_rotInit = false;
volatile LONG g_bayerTexels = 0, g_rotTexels = 0;
bool g_ditherPatched = false;
void WINAPI ConstBayer(float* out, const float* tc, const float* ts, void* data) {
    if (!g_bayerInit) {
        float best = 9.f;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            float c[2] = { (x + 0.5f) / 16.f, (y + 0.5f) / 16.f }, o[4];
            g_oBayer(o, c, ts, data);
            if (fabsf(o[0] - 0.5f) < best) { best = fabsf(o[0] - 0.5f); memcpy(g_bayerConst, o, 16); }
        }
        g_bayerInit = true;
    }
    memcpy(out, g_bayerConst, 16);
    InterlockedIncrement(&g_bayerTexels);
}
// remove_dither = fine: the 16 x 16 pattern folded to its 2 x 2 core - texel (x, y) takes the value of (x & 1, y & 1).
// The half split of every 2 x 2 block (what the game's colour decode and 50 % blends read) is unchanged, so no colour
// shift; the screen door becomes a regular 2-pixel grid (4 levels) the dedither filter averages away cleanly.
float g_fine[4][4]; bool g_fineInit = false; bool g_bayerFine = false;
void WINAPI FineBayer(float* out, const float* tc, const float* ts, void* data) {
    if (!g_fineInit) {
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) { float c[2] = { (x + 0.5f) / 16.f, (y + 0.5f) / 16.f }; g_oBayer(g_fine[y * 2 + x], c, ts, data); }
        g_fineInit = true;
    }
    int x = (int)floorf(tc[0] * 16.f) & 1, y = (int)floorf(tc[1] * 16.f) & 1;
    memcpy(out, g_fine[y * 2 + x], 16);
    InterlockedIncrement(&g_bayerTexels);
}
void WINAPI ConstRot(float* out, const float* tc, const float* ts, void* data) {
    if (!g_rotInit) { float c[2] = { 0.5f / 16.f, 0.5f / 16.f }; g_oRot(g_rotConst, c, ts, data); g_rotInit = true; }
    memcpy(out, g_rotConst, 16);
    InterlockedIncrement(&g_rotTexels);
}
// the one "push imm32" of a callback's address in the code
uint8_t* FindPush(const void* target) {
    uint8_t pat[5] = { 0x68 }; uint32_t a = (uint32_t)(uintptr_t)target; memcpy(pat + 1, &a, 4);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* hit = nullptr; int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* p0 = base + sec[i].VirtualAddress; size_t len = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 5 <= len; k++) if (p0[k] == 0x68 && !memcmp(p0 + k, pat, 5)) { hit = p0 + k; n++; }
    }
    return n == 1 ? hit : nullptr;
}
void PatchDitherTextures(const char* when) {
    if (g_ditherPatched) return;
    std::string rd = ConfigStr("remove_dither", "false");
    for (auto& ch : rd) ch = (char)tolower((unsigned char)ch);
    g_bayerFine = rd == "fine";
    bool wantBayer = g_bayerFine || rd == "true" || rd == "uniform" || rd == "1" || rd == "on", wantRot = ConfigBool("shadow_dither_uniform", false);   // was remove_shadow_dither (on in 0.5.0 - 0.6.1). Off: blood has had the wrong colour ever since that patch went in, so the texture is very likely read by more than the shadow filter
    if (!wantBayer && !wantRot) { g_ditherPatched = true; LOGI("engine: the game's dither textures left as they are (remove_dither = false)"); return; }
    int n1 = 0, n2 = 0;
    uint8_t* bayer = FindUnique("55 8B EC 83 EC 34 8B 4D 0C D9 7D 0E 0F B7 45 0E D9 01 D9 05 ?? ?? ?? ?? 0D 00 0C 00 00 DC C9 89 45 D8", &n1);
    uint8_t* rot = FindUnique("55 8B EC 83 EC 30 8B 4D 0C D9 7D 0E 0F B7 45 0E D9 01 D9 05 ?? ?? ?? ?? 0D 00 0C 00 00 89 45 D4 DC C9", &n2);
    if (!bayer || !rot) { if (!strcmp(when, "device")) { g_ditherPatched = true; LOGW("engine: dither texture code not found (%d / %d matches)", n1, n2); } return; }
    uint8_t* pb = FindPush(bayer); uint8_t* pr = FindPush(rot);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    g_ditherPatched = true;
    if (wantBayer && pb) { g_oBayer = (FillCb)bayer; uint32_t v = (uint32_t)(uintptr_t)&ConstBayer; if (g_bayerFine) v = (uint32_t)(uintptr_t)&FineBayer; Write(pb + 1, &v, 4); }
    if (wantRot && pr) { g_oRot = (FillCb)rot; uint32_t v = (uint32_t)(uintptr_t)&ConstRot; Write(pr + 1, &v, 4); }
    LOGI("engine: screen-door dither removed at the source - Bayer threshold texture %s, shadow-filter rotation texture %s (exe+%lx / exe+%lx, %s)",
         wantBayer ? (pb ? (g_bayerFine ? "folded to a fine 2x2 grid" : "made uniform") : "push NOT found") : "kept", wantRot ? (pr ? "made uniform" : "push NOT found") : "kept",
         (unsigned long)(bayer - base), (unsigned long)(rot - base), when);
}

} // namespace

// ---------------------------------------------------------------- 0.7.0: the HUD's aspect ratio
// The viewport keeps an aspect ratio of its own (+0x90, from the table at 0xE46DB4: 16:9, 16:9, 4:3, 5:4, 16:10, 5:3,
// 48:9, 27:16; default at 0xE46DB0) and lays the Scaleform HUD out on a stage 1280 wide and 1280 / aspect high (+0xA0,
// 0xAAED18). With the letterbox patched out the picture has the headset's aspect (about 1.63) but the HUD was still
// laid out for 16:9 and scaled to the frame's height: wider than the frame, its left / right edges cut off. Every
// table entry (and the default) becomes the frame's real aspect, so the game lays the HUD out for the picture it draws.
namespace {
bool g_aspectDone = false;
void PatchAspectTable(const char* when) {
    if (g_aspectDone || !ConfigBool("hud_native_aspect", true)) return;
    unsigned w = 0, h = 0;
    if (!W2IniResolution(&w, &h) || !w || !h) return;
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    float* tab = (float*)(base + 0xA46DB4); float* def = (float*)(base + 0xA46DB0); float* stage = (float*)(base + 0xA46DAC);
    static const float expect[7] = { 1.7778f, 1.3333f, 1.25f, 1.6f, 1.6667f, 5.3333f, 1.6875f };   // table[0..6]; the default sits just before it
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(stage, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return;
    bool ok = fabsf(*stage - 1280.f) < 0.5f && fabsf(*def - 1.7778f) < 0.001f;
    for (int i = 0; i < 7 && ok; i++) ok = fabsf(tab[i] - expect[i]) < 0.001f;
    if (!ok) { if (!strcmp(when, "device")) { g_aspectDone = true; LOGW("engine: aspect table not found at exe+a46db4 - the HUD keeps its 16:9 layout"); } return; }
    float a = (float)w / (float)h, t[13]; for (float& v : t) v = a;   // the default + all 12 table entries (the last 7: multi-monitor)
    if (fabsf(tab[11] - 2.4f) > 0.001f) { if (!strcmp(when, "device")) { g_aspectDone = true; LOGW("engine: aspect table has another layout - left alone"); } return; }
    if (Write((uint8_t*)def, t, sizeof t)) {
        g_aspectDone = true;
        LOGI("engine: the game's aspect table set to the frame's %.4f (%ux%u, %s) - HUD stage 1280 x %.0f instead of 1280 x 720 (hud_native_aspect)", a, w, h, when, 1280.f / a);
    }
}
} // namespace
void W2EngineEarly() { if (ConfigBool("diag_vanilla", false)) return; PatchAspectTable("startup"); }

// ---------------------------------------------------------------- 0.7.0: input - the pad without window focus
// The game's input poll (0x9D2817, eax = input manager) reads the keyboard and mouse through DirectInput (exclusive,
// foreground) and only then the pad (XInputGetState, user 0). When a DirectInput read fails because the window is not
// the foreground window (DIERR_INPUTLOST / DIERR_NOTACQUIRED), it re-acquires and RETURNS (0x9D2B47 -> 0x9D2D81): the
// pad is never read. In a headset the game window is often not the foreground window (the headset's software, the
// desktop, a click anywhere else), and with keep_running_unfocused the game does not pause - it just ignores the pad.
// A lost DirectInput device now reports "nothing pressed" instead of an error, so the poll goes on to the pad.
extern "C" { void* g_w2PollOrig = nullptr; uint8_t* g_w2InputMgr = nullptr; }
__asm__(".text\n.globl _W2PollThunk\n_W2PollThunk:\n  movl %eax, _g_w2InputMgr\n  jmp *_g_w2PollOrig\n");
extern "C" void W2PollThunk();
namespace {
typedef HRESULT (WINAPI* DI8CreateFn)(HINSTANCE, DWORD, REFIID, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE* DICreateDevFn)(void*, REFGUID, void**, void*);
typedef HRESULT (STDMETHODCALLTYPE* DIGetStateFn)(void*, DWORD, void*);
typedef HRESULT (STDMETHODCALLTYPE* DIGetDataFn)(void*, DWORD, void*, DWORD*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* DIAcquireFn)(void*);
DI8CreateFn oDI8Create; DICreateDevFn oDICreateDev; DIGetStateFn oDIGetState; DIGetDataFn oDIGetData;
bool g_diMask = true, g_diImport = false, g_pollHooked = false;
volatile LONG g_diReads = 0, g_diLost = 0, g_diDevices = 0;
bool DiLost(HRESULT hr) { return hr == (HRESULT)0x8007001E || hr == (HRESULT)0x8007000C || hr == E_ACCESSDENIED; }
HRESULT STDMETHODCALLTYPE hDIGetState(void* dev, DWORD cb, void* data) {
    HRESULT hr = oDIGetState(dev, cb, data);
    InterlockedIncrement(&g_diReads);
    if (g_diMask && DiLost(hr)) {
        ((DIAcquireFn)(*(void***)dev)[7])(dev);
        hr = oDIGetState(dev, cb, data);
        if (FAILED(hr)) { InterlockedIncrement(&g_diLost); if (data && cb) memset(data, 0, cb); return S_OK; }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hDIGetData(void* dev, DWORD cbObj, void* rg, DWORD* inOut, DWORD flags) {
    HRESULT hr = oDIGetData(dev, cbObj, rg, inOut, flags);
    InterlockedIncrement(&g_diReads);
    if (g_diMask && DiLost(hr)) {
        ((DIAcquireFn)(*(void***)dev)[7])(dev);
        if (inOut) *inOut = 0;
        InterlockedIncrement(&g_diLost);
        return S_OK;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE hDICreateDev(void* di, REFGUID g, void** out, void* outer) {
    HRESULT hr = oDICreateDev(di, g, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void** v = *(void***)*out;
        if (!oDIGetState && MH_CreateHook(v[9], (void*)hDIGetState, (void**)&oDIGetState) == MH_OK) MH_EnableHook(v[9]);
        if (!oDIGetData && MH_CreateHook(v[10], (void*)hDIGetData, (void**)&oDIGetData) == MH_OK) MH_EnableHook(v[10]);
        if (InterlockedIncrement(&g_diDevices) <= 2) LOGI("input: DirectInput device created - a lost device (window not in front) no longer stops the pad");
    }
    return hr;
}
HRESULT WINAPI hDI8Create(HINSTANCE inst, DWORD ver, REFIID riid, void** out, void* outer) {
    HRESULT hr = oDI8Create(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && !oDICreateDev) {
        void** v = *(void***)*out;
        if (MH_CreateHook(v[3], (void*)hDICreateDev, (void**)&oDICreateDev) == MH_OK) MH_EnableHook(v[3]);
    }
    return hr;
}
void InputInstall(const char* when) {
    g_diMask = ConfigBool("pad_without_focus", true);
    if (!g_diImport) {
        HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (!m) m = LoadLibraryW(L"dinput8.dll");
        void* real = m ? (void*)GetProcAddress(m, "DirectInput8Create") : nullptr;
        if (real && PatchImport(GetModuleHandleW(nullptr), "dinput8.dll", real, (void*)hDI8Create)) {
            oDI8Create = (DI8CreateFn)real; g_diImport = true;
            LOGI("input: game import of DirectInput8Create redirected (%s)", when);
        } else if (!strcmp(when, "device")) LOGW("input: DirectInput8Create import not found - the pad needs the game window in front");
    }
    if (!g_pollHooked) {
        int n = 0;
        uint8_t* p = FindUnique("55 8B EC 81 EC E4 06 00 00 53 56 8B F0 8B 86 34 08 00 00", &n);
        if (p && MH_CreateHook(p, (void*)W2PollThunk, &g_w2PollOrig) == MH_OK && MH_EnableHook(p) == MH_OK) {
            g_pollHooked = true;
            LOGI("input: the game's input poll found at exe+%lx (%s)", (unsigned long)(p - (uint8_t*)GetModuleHandleW(nullptr)), when);
        } else if (!strcmp(when, "device")) LOGW("input: input poll not found (%d matches)", n);
    }
}
} // namespace
HWND W2GameWindow();
void W2InputReport() {
    HWND g_wnd = W2GameWindow();
    HWND fg = GetForegroundWindow();
    char title[64] = ""; if (fg && fg != g_wnd) GetWindowTextA(fg, title, sizeof title);
    DWORD mode = 99; bool kb = false, mouse = false;
    if (g_w2InputMgr && !IsBadReadPtr(g_w2InputMgr + 0x828, 16)) { mode = *(DWORD*)(g_w2InputMgr + 0x834); kb = *(void**)(g_w2InputMgr + 0x82c) != nullptr; mouse = *(void**)(g_w2InputMgr + 0x830) != nullptr; }
    LOGI("input: game window %s%s%s; game input mode %lu (3 = gameplay capture, 0 = menus / none; 99 = not polled yet), keyboard %s, mouse %s; DirectInput reads %ld, lost and masked %ld",
         fg == g_wnd ? "in front" : "NOT in front (", fg == g_wnd ? "" : title, fg == g_wnd ? "" : ")", (unsigned long)mode, kb ? "yes" : "no", mouse ? "yes" : "no",
         InterlockedExchange(&g_diReads, 0), InterlockedExchange(&g_diLost, 0));
}
// once, when VR starts: the game window to the front (the headset's software often takes it at start-up)
void W2WindowToFront() {
    HWND g_wnd = W2GameWindow();
    static bool done = false; if (done || !g_wnd) return; done = true;
    if (!ConfigBool("window_to_front", true) || GetForegroundWindow() == g_wnd) return;
    INPUT in[2] = {}; in[0].type = in[1].type = INPUT_KEYBOARD; in[0].ki.wVk = in[1].ki.wVk = VK_F24; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));   // an input event of our own lets this process take the foreground
    BOOL ok = SetForegroundWindow(g_wnd); SetActiveWindow(g_wnd); SetFocus(g_wnd);
    LOGI("window: game window brought to the front: %s", ok && GetForegroundWindow() == g_wnd ? "yes" : "no (click the game window once if the keyboard does not respond)");
}
void W2EngineInstall(const char* when) {
    PatchAspectTable(when);
    InputInstall(when);
    PatchDitherTextures(when);
    if (!g_pauseFound) {
        int n = 0, m = 0;
        uint8_t* p = FindUnique("A1 ?? ?? ?? ?? 83 78 5C 00 8B 4D 08 53 57 7E ?? 80 3D ?? ?? ?? ?? 00", &n);
        uint8_t* q = FindUnique("55 8B EC 8B 45 08 FF 40 18 83 79 5C 00 7E ?? 80 3D", &m);   // IsPaused: same field
        if (p && q && *(uint32_t*)(p + 18) == *(uint32_t*)(q + 17)) {
            g_gamePP = *(uint8_t***)(p + 1); g_pauseOff = *(uint8_t**)(p + 18); g_pauseFound = true;
            g_flatPaused = ConfigBool("flat_when_paused", true); g_flatActive = ConfigBool("flat_on_active_pause", false);
            LOGI("engine: game pause state found (game object at %p, %s) - menus %s", (void*)g_gamePP, when, g_flatPaused ? "switch to the flat screen" : "stay in VR");
        } else if (!strcmp(when, "device")) LOGW("engine: game pause state not found (%d / %d matches) - menus stay in VR", n, m);
    }
    if (!g_fxSearched) {
        auto* base = (uint8_t*)GetModuleHandleW(nullptr);
        static const char* key[W2FX_COUNT] = { "disable_depth_of_field", "disable_motion_blur", "disable_vignette", "disable_blur", "disable_ssao", "disable_sharpen" };
        static const bool def[W2FX_COUNT] = { true, true, true, false, false, true };
        int found = 0;
        { int c = 0; uint8_t* h = FindUnique("C7 83 DC 00 00 00 60 20 00 00", &c); if (h) g_dofOnlyImm = h + 6;
          LOGI("engine: DOF-only post-process path %s", h ? "found (forced DOF mask 0x2060 - patched with the DOF switch)" : "not found"); }
        for (int f = 0; f < W2FX_COUNT; f++) {
            Effect& e = g_fx[f]; bool all = true;
            for (int i = 0; i < e.n; i++) { int c = 0; uint8_t* h = FindUnique(e.site[i].sig, &c); if (!h) { all = false; break; } e.jcc[i] = h + e.site[i].at; }
            if (all && e.n) {
                // every site sits in the one mask function (0x9BFFE8 .. 0x9C011D)
                uint8_t* lo = e.jcc[0]; uint8_t* hi = e.jcc[0];
                for (int i = 1; i < e.n; i++) { if (e.jcc[i] < lo) lo = e.jcc[i]; if (e.jcc[i] > hi) hi = e.jcc[i]; }
                all = hi - lo < 0x200 && *lo == 0x74;
            }
            e.found = all;
            if (all) { found++; W2EngineSetEffectOff(f, ConfigBool(key[f], def[f])); }
        }
        if (found) { g_fxSearched = true; LOGI("engine: post-process mask found (exe+%lx): %d of %d effects can be forced off (%s)", (unsigned long)(g_fx[0].found ? g_fx[0].jcc[0] - base : 0), found, W2FX_COUNT, when); }
        else if (!strcmp(when, "device")) { g_fxSearched = true; LOGW("engine: post-process mask not found - DOF / motion blur / vignette only off through User.ini"); }
    }
    if (!g_lbDone && ConfigBool("no_letterbox", true)) {
        int n1 = 0, n2 = 0;
        uint8_t* top = FindUnique("E8 ?? ?? ?? ?? 8B D8 EB 30 F3 0F 10 4D 08", &n1);              // ftol; mov ebx, eax
        uint8_t* left = FindUnique("E8 ?? ?? ?? ?? 8B F8 8B 56 20 8B 06 8D 0C 1B", &n2);           // ftol; mov edi, eax
        if (top && left) {
            static const uint8_t xbx[2] = { 0x33, 0xDB }, xdi[2] = { 0x33, 0xFF };   // xor ebx, ebx / xor edi, edi
            bool ok = Write(top + 5, xbx, 2) && Write(left + 5, xdi, 2);
            g_lbDone = ok;
            auto* base = (uint8_t*)GetModuleHandleW(nullptr);
            LOGI("engine: no letterbox / pillarbox - the 3D picture covers the whole frame at any aspect (exe+%lx, exe+%lx, %s)%s",
                 (unsigned long)(top + 5 - base), (unsigned long)(left + 5 - base), when, ok ? "" : " - write FAILED");
        } else if (!strcmp(when, "device")) LOGW("engine: letterbox code not found (%d %d matches) - non-16:9 pictures get black bars", n1, n2);
    }
}

// 0: not patched / not yet built; texels written through the replaced callbacks (16 x 16 = 256 each when it worked)
void W2EngineDitherReport() {
    static bool done = false;
    if (done || !g_ditherPatched || (!g_oBayer && !g_oRot)) return;
    LONG b = g_bayerTexels, r = g_rotTexels;
    static int reports = 0;
    if (!b && !r && ++reports == 3) { done = true; LOGW("engine: the dither textures were built before the patch (no texels through it) - the grain stays; set dedither = 1 for the smoothing filter"); return; }
    if (b || r) { done = true; LOGI("engine: dither textures built uniform (%ld + %ld texels; Bayer value %.3f, rotation %.3f %.3f)", b, r, g_bayerConst[0], g_rotConst[0], g_rotConst[1]); }
}
bool W2EngineDitherRemoved() { return g_bayerTexels > 0; }

// ---- texture streaming budget (0.9.8): the streamer (0x9d5493 / 0x9d5807) loads a texture's full mips only while
// used texture memory (stats block 0x237e9f4.., +0x10 = bytes in use) + the new texture fits in
// settings+0x44 (TextureMemoryBudget, MB) << 20. Over budget it keeps the low mip / placeholder.
static uint8_t* W2Settings() {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    uint8_t** pp = (uint8_t**)(base + 0x2180E28);
    if (IsBadReadPtr(pp, 4) || !*pp || IsBadWritePtr(*pp + 0x44, 4)) return nullptr;
    return *pp;
}
static int g_budgetOrig = -1;
void W2SetTextureBudgetTest(int mb) {   // 0 = the game's own value
    uint8_t* s = W2Settings(); if (!s) return;
    int* p = (int*)(s + 0x44);
    if (g_budgetOrig < 0) g_budgetOrig = *p;
    int want = mb > 0 ? mb : g_budgetOrig;
    if (*p != want) { LOGI("engine: TextureMemoryBudget %d -> %d MB (Tests, live)", *p, want); *p = want; }
}
void W2TextureBudgetReport() {
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    uint8_t* s = W2Settings();
    uint32_t* st = (uint32_t*)(base + 0x1F7E9F4);
    if (IsBadReadPtr(st, 32)) return;
    int budget = s ? *(int*)(s + 0x44) : -1;
    LOGI("textures: budget %d MB, streamer stats %u %u %u %u %u %u %u %u (in use ~ %.0f MB)", budget,
         st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[4] / 1048576.0);
}
