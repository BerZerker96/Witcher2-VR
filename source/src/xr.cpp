// VR output through XrBridge (64-bit XrBridgeHost.exe) with D3D9Ex shared textures (SYNC_SLOT_STATE).
//
// Stereo: full stereo (w2camera.cpp) draws both eyes inside one game frame - the first eye is copied out mid-frame
// (XrFullStereoMid), the second at Present. Same-tick / plain AER: the game renders one frame per eye (left, right,
// left, ...). At Present the back buffer is copied into that eye's texture; a
// slot of the shared ring gets both eyes (each with the pose and FOV it was rendered with) and is submitted once the
// GPU has finished the copies (event query). Frames without the game's 3D camera (menus, loading screens, videos)
// go to the flat virtual screen.
//
// Atlas layout per slot: left eye (0,0,W,H) | right eye (W,0,W,H), HUD / flat screen / menu panel (0,H,W,H).
#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <cmath>
#include <string>
#include <cctype>
#include <cstdlib>
#include "log.h"
#include "config.h"
#include "w2vr.h"
#include "vrkit_input.h"
#include "game.h"
#include "xrbridge/client.h"

void StereoPoseChanged();
void ScreenshotSave(IDirect3DDevice9* d, IDirect3DTexture9* left, IDirect3DTexture9* right, bool sbs);
void ScreenshotSaveHud(IDirect3DDevice9* d, IDirect3DTexture9* hud);
bool MenuIsOpen();
void StereoLogTargetsNextFrame();
bool StereoHaveMain();
void D3D9Report();

int W2CamLatestEye();
// ---------------------------------------------------------------- HUD panel (hud_mode = panel)
// The game's 2D HUD / menus are drawn into this target instead of the eye image (d3d9hooks.cpp), at full resolution,
// premultiplied alpha; it goes into the atlas's HUD rectangle and the host shows it as a head-locked quad whose angular
// size is the flat game's own view (so 3D-anchored markers sit where the flat game had them).
static IDirect3DTexture9* g_hudTex = nullptr; static IDirect3DSurface9* g_hudSurf = nullptr; static UINT g_hudW = 0, g_hudH = 0;
static unsigned g_hudDrawnFrame = 0; static bool g_hudFailed = false; static DWORD g_hudFailTick = 0;
static float g_hudPanelDist = 1.25f, g_hudPanelOffY = -0.05f, g_hudPanelW = 1.85f; static bool g_hudPanelFollow = true;
static float g_hudSentW = -1, g_hudSentD = -1, g_hudSentY = -9;
void XrNoteHudDraw() { g_hudDrawnFrame = g_vr.presentCount; }
IDirect3DSurface9* XrHudSurface(IDirect3DDevice9* d) {
    if (g_hudSurf) return g_hudSurf;
    if (g_hudFailed && GetTickCount() - g_hudFailTick < 5000) return nullptr;
    IDirect3DSurface9* bb = nullptr; D3DSURFACE_DESC bd;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return nullptr;
    bb->GetDesc(&bd); bb->Release();
    if (FAILED(CallCreateTexture(d, bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTex, nullptr)) || !g_hudTex) {
        g_hudFailed = true; g_hudFailTick = GetTickCount(); LOGW("xr: HUD panel target %ux%u failed - the HUD stays in the eye image", bd.Width, bd.Height); return nullptr;
    }
    g_hudTex->GetSurfaceLevel(0, &g_hudSurf); g_hudSurf->Release();   // the texture keeps it
    d->ColorFill(g_hudSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
    g_hudW = bd.Width; g_hudH = bd.Height; g_hudFailed = false;
    LOGI("xr: HUD panel target %ux%u", g_hudW, g_hudH);
    return g_hudSurf;
}
static void ReleaseHud() { if (g_hudTex) { g_hudTex->Release(); g_hudTex = nullptr; } g_hudSurf = nullptr; }
static bool HudHasContent() { return g_hudTex && g_vr.presentCount - g_hudDrawnFrame <= 2; }
static UINT g_w = 0, g_h = 0;          // eye capture size (= back buffer)
static xrb::IpcFov g_hfov[2] = {}; static bool g_hfovValid = false;
void XrSetHudPanel(float dist, float offY, float width, bool follow) { g_hudPanelDist = dist; g_hudPanelOffY = offY; g_hudPanelW = width; g_hudPanelFollow = follow; g_hudSentW = -1; }
static void UpdateHudPanelPlacement() {
    // width: the flat game's horizontal view at that distance (x hud_size), or a fixed width
    float w = g_hudPanelW;
    if (w <= 0.01f) {
        GameProj gp; float t = 1.f;   // half-tangent of the game's horizontal view (default 45 degrees)
        if (W2CamGameProj(&gp) && gp.p00 > 0.1f) t = 1.f / gp.p00;
        w = 2.f * g_hudPanelDist * t;
    }
    w *= g_vr.hudSize;
    {   // hud_panel_fit: keep the whole panel inside the clearly visible middle of the headset's view (any headset:
        // its per-eye FOV), so minimap / quick slots / subtitles at the HUD's edges are not out at the blurry lens edge
        static const bool fit = ConfigBool("hud_panel_fit", true);
        static const float share = ConfigFloat("hud_panel_fit_share", 0.70f);
        if (fit && g_hfovValid && g_hudPanelDist > 0.05f) {
            const float k = share < 0.3f ? 0.3f : share > 1.f ? 1.f : share;
            float inward = fminf(fabsf(g_hfov[0].right), fabsf(g_hfov[1].left));          // each eye's side towards the nose
            float vert = fminf(fminf(g_hfov[0].up, -g_hfov[0].down), fminf(g_hfov[1].up, -g_hfov[1].down));
            float aspect = (g_w && g_h) ? (float)g_w / (float)g_h : 16.f / 9.f;
            float maxW = 2.f * g_hudPanelDist * tanf(inward * k);
            float maxH = 2.f * (g_hudPanelDist * tanf(vert * k) - fabsf(g_hudPanelOffY));
            if (maxH > 0.05f && maxH * aspect < maxW) maxW = maxH * aspect;
            if (maxW > 0.2f && w > maxW) {
                static float logged = 0; if (fabsf(logged - maxW) > 0.02f) { logged = maxW; LOGI("xr: HUD panel %.2f m -> %.2f m to stay inside %.0f %% of the headset's view (hud_panel_fit)", w, maxW, k * 100.f); }
                w = maxW;
            }
        }
    }
    if (fabsf(w - g_hudSentW) > 0.005f || g_hudPanelDist != g_hudSentD || g_hudPanelOffY != g_hudSentY) {
        xrb::ClientSetHud(g_hudPanelDist, w, g_hudPanelOffY, g_hudPanelFollow);
        if (g_hudSentW < 0) LOGI("xr: HUD panel %.2f m wide at %.2f m (%s)", w, g_hudPanelDist, g_hudPanelFollow ? "head-locked" : "world-locked");
        g_hudSentW = w; g_hudSentD = g_hudPanelDist; g_hudSentY = g_hudPanelOffY;
    }
}
static const char* kHudModeName[3] = { "panel (own quad, full resolution)", "fitted into the eye image", "full size in the eye image" };
void MenuSetHudMode(int m);
void XrCycleHudMode() { int m = (g_vr.hudMode + 1) % 3; MenuSetHudMode(m); LOGI("xr: HUD %s", kHudModeName[m]); }

static bool g_xrInit = false;
static UINT g_ew = 0, g_eh = 0;        // eye rectangle in the atlas (edge fill: wider than the picture)
static UINT g_wantEw = 0, g_wantEh = 0;
static UINT g_aw = 0, g_ah = 0;        // atlas size
static bool g_edgeFill = true, g_aspectCrop = true;
static float g_edgeMaxSide = 8192.f;
static float g_efCal = 0; static int g_efCalN = 0, g_efBad = 0, g_efStable = 0; static bool g_efOff = false, g_efOk = false;
static bool g_efHeadMargin = true;
static std::string g_engineFovMode = "auto"; static float g_engineFovFixed = 1.f, g_engineFovMax = 4.f, g_engineFovMargin = 1.f;
static UINT g_hw = 0, g_hh = 0;        // HUD / flat screen / menu rectangle (at most 2560 wide; saves video memory)
static D3DFORMAT g_bbFmt = D3DFMT_UNKNOWN;
static D3DMULTISAMPLE_TYPE g_bbMs = D3DMULTISAMPLE_NONE;
static IDirect3DTexture9* g_eyeTex[3] = {};        // 0 left, 1 right, 2 flat screen
struct EyeRec { bool valid = false; xrb::IpcPose pose{}; xrb::IpcFov fov{}; int64_t time = 0; unsigned frame = 0; };
static EyeRec g_eyeRec[2];
struct Slot { IDirect3DTexture9* tex = nullptr; HANDLE share = nullptr; IDirect3DQuery9* query = nullptr; };
static Slot g_slots[xrb::kSlots];
struct Pending { bool used = false; xrb::IpcPose pose[2]; xrb::IpcFov fov[2]; int64_t time; uint32_t flags; DWORD tick; };
static Pending g_pending[xrb::kSlots];
static IDirect3DStateBlock9* g_sb = nullptr;
static volatile LONG g_recenterReq = 0;
static unsigned g_frame = 0, g_submitted = 0, g_busy = 0, g_noHost = 0, g_vrFrames = 0, g_flatFrames = 0, g_gpuLate = 0;
static DWORD g_reportTick = 0, g_fpsTick = 0; static unsigned g_fpsFrames = 0;
static bool g_mirror = true;
static int g_keyRecenter = VK_SPACE;
static int g_keyScreenshot = VK_DELETE, g_keyHud = VK_F7, g_keyFp = VK_END;
void MenuToggleFirstPerson();
static bool g_shotWanted = false, g_shotSbs = false;
static int VkFromName(const std::string& s, int def) {
    if (s.empty()) return def;
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return toupper((unsigned char)s[0]);
    static const struct { const char* n; int vk; } k[] = { { "delete", VK_DELETE }, { "end", VK_END }, { "home", VK_HOME }, { "insert", VK_INSERT },
        { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "f1", VK_F1 }, { "f2", VK_F2 }, { "f3", VK_F3 }, { "f4", VK_F4 }, { "f5", VK_F5 },
        { "f6", VK_F6 }, { "f7", VK_F7 }, { "f11", VK_F11 }, { "f12", VK_F12 }, { "pause", VK_PAUSE }, { "space", VK_SPACE }, { "f9", VK_F9 }, { "none", 0 }, { "scrolllock", VK_SCROLL }, { "printscreen", VK_SNAPSHOT } };
    std::string l; for (char ch : s) l += (char)tolower((unsigned char)ch);
    for (auto& e : k) if (l == e.n) return e.vk;
    if (l.size() > 2 && l[0] == '0' && l[1] == 'x') return (int)strtoul(l.c_str(), nullptr, 16);
    return def;
}
static float g_minXfDraws = 20;
static float g_fakeYaw = 0;

void XrRequestRecenter() { g_recenterReq = 1; }

// ---------------------------------------------------------------- frame limiter (VR panel > Engine > Frame limit)
// off | headset (the headset's refresh rate) | headset_x2 (twice it: in stereo each eye gets every second frame) |
// a number in fps. Paced on the real-time clock (speed-hack safe): sleep in 1 ms steps, then spin the last part.
static std::string g_limitMode = "headset";
static double g_limWaitMs = 0; static unsigned g_limN = 0, g_limWaited = 0;
void XrSetFrameLimit(const std::string& m) { g_limitMode = m; }
// ---------------------------------------------------------------- stereo_method = auto: follow the headset's smoothing
// Runtimes and headsets (Pimax Smart Smoothing, SteamVR Motion Smoothing, WMR / Quest ASW, OpenXR layers that do the
// same) drop to a fraction of the panel rate and synthesise the missing frames from the app's frame pairs. Same-tick AER
// renders one eye per frame, so to that synthesis one eye is a frame old and jumps every other frame. While the
// headset runs below its full refresh, full stereo (both eyes from the same game tick) gives it consistent pairs; at
// full refresh same-tick keeps one new eye picture per display frame. Switch after 1.5 s in the new state.
void SameTickSetMode(const std::string& m);
void W2FullStereoSetMethod(const std::string& m);
// Headset smoothing (VR panel > Engine, live): 2 = always (the game is paced at half the headset's rate with both eyes
// per tick, so the headset's smoothing runs all the time and never drops in or out), 1 = only when fps drops (full rate
// while the game holds it; half rate + both eyes while the headset has dropped), 0 = off (the game never follows).
static int g_smMode = 1; static bool g_smFellBack = false;
void XrSetSmoothing(const std::string& m) { int n = m == "off" ? 0 : m == "drops" ? 1 : 2; if (n != g_smMode) { g_smMode = n; g_smFellBack = false; LOGI("smoothing: headset smoothing %s", n == 2 ? "always" : n == 1 ? "only when fps drops" : "off"); } }
static int SmMode() { return g_smMode == 2 && g_smFellBack ? 1 : g_smMode; }
static bool g_stereoAuto = false; static int g_autoState = -1; static DWORD g_autoSince = 0;
static void ApplyStereoMethod(const std::string& m) { SameTickSetMode(m); W2FullStereoSetMethod(m); }
void XrSetStereoAuto(bool on) {
    // already automatic (the VR panel re-applies every setting on each change): keep the current method - resetting it
    // switched to same-tick and back to full stereo for a moment on every menu change
    if (on && g_stereoAuto && g_autoState >= 0) return;
    g_stereoAuto = on;
    if (on) { g_autoState = -1; g_autoSince = 0; ApplyStereoMethod("same_tick"); }
}
bool XrSmoothingActive() { return g_stereoAuto ? g_autoState == 1 : false; }
static void StereoAutoUpdate() {
    if (!g_stereoAuto && SmMode() != 2) return;
    static double maxHz = 0;
    int64_t ns = xrb::ClientDisplayPeriodNs();
    if (ns <= 0) return;
    double hz = 1e9 / (double)ns; if (hz > maxHz) maxHz = hz;
    int want = hz < maxHz * 0.75 ? 1 : 0;
    if (SmMode() == 2) want = 1; else if (SmMode() == 0) want = 0;
    DWORD now = GetTickCount();
    if (g_autoState < 0) { g_autoState = want; g_autoSince = now; ApplyStereoMethod(want ? "full" : "same_tick"); return; }
    if (want == g_autoState) { g_autoSince = now; return; }
    if (now - g_autoSince < 1500) return;
    g_autoState = want; g_autoSince = now;
    LOGI("stereo: headset at %.0f of %.0f Hz - %s", hz, maxHz, want ? "its frame smoothing is active: full stereo (both eyes from each game tick)"
                                                                 : "full refresh: same-tick (one new eye per display frame)");
    ApplyStereoMethod(want ? "full" : "same_tick");
}

double g_limLastSpare = 0;
static double FrameLimitHz() {
    if (g_limitMode == "off" || g_limitMode.empty()) return 0;
    if (g_limitMode == "headset" || g_limitMode == "headset_x2") {
        // the headset's full refresh: runtimes switch to half rate (motion smoothing: 72 Hz panel reported as 36 Hz)
        // when the game is slow - capping the game there would lock it at half rate, so the highest rate seen wins
        static double maxHz = 0;
        int64_t ns = xrb::ClientDisplayPeriodNs();
        double cur = ns > 0 ? 1e9 / (double)ns : 0.0;
        if (cur > maxHz) maxHz = cur;
        double hz = maxHz > 0 ? maxHz : 90.0;
        // smoothing_lock (0.6.1): while the headset's frame smoothing runs (Pimax Smart Smoothing, SteamVR Motion
        // Smoothing, ASW...: the runtime asks for frames at half rate), the game is paced at exactly that rate. Smoothing
        // builds the in-between frames from evenly spaced pairs; 40 - 60 uneven game frames per second at a 36 Hz request
        // gave it irregular gaps (wobble / judder). Released when the game would have more than 40 % of the period spare
        // for 3 s (it can hold the full rate again, and the runtime then switches smoothing off by itself).
        if (SmMode() == 2 && maxHz > 0) {
            // always: half rate, evenly paced. Smart Smoothing off in Pimax Play = the headset stays at its full rate and
            // would show every picture twice: noticed after 15 s, then "only when fps drops" for this session
            static DWORD fullSince = 0; DWORD now = GetTickCount();
            if (cur >= maxHz * 0.75 && g_vr.active) { if (!fullSince) fullSince = now; } else fullSince = 0;
            if (fullSince && now - fullSince > 15000) {
                g_smFellBack = true; fullSince = 0;
                LOGW("smoothing: the headset stayed at %.0f Hz for 15 s while the game sent %.0f fps - Smart Smoothing seems off in Pimax Play; using 'only when fps drops' for this session", cur, maxHz / 2);
            } else return g_limitMode == "headset_x2" ? maxHz : maxHz / 2;
        }
        const bool lockOn = SmMode() == 1;
        static bool locked = false; static DWORD spareSince = 0;
        bool smoothing = cur > 0 && maxHz > 0 && cur < maxHz * 0.75;
        if (lockOn && smoothing) {
            if (!locked) { locked = true; spareSince = 0; LOGI("frame limit: headset smoothing at %.0f Hz - the game is paced at %.0f fps (smoothing_lock)", cur, cur); }
            extern double g_limLastSpare;   // share of the last period the limiter waited
            DWORD now = GetTickCount();
            static DWORD freeUntil = 0;   // after a release: 10 s at full rate, so the runtime can leave smoothing
            if (freeUntil && (int)(now - freeUntil) < 0) { /* full rate */ }
            else {
                if (freeUntil) { freeUntil = 0; spareSince = 0; }
                // both eyes at half rate must leave more than half the period spare before one eye per full-rate frame can
                // hold (0.6.1 - 0.8.0 tried at 40 %: the game missed the full rate, the headset fell back, again and again)
                static DWORD lastTrial = 0;
                if (g_limLastSpare > 0.58) { if (!spareSince) spareSince = now; } else spareSince = 0;
                if (spareSince && now - spareSince > 5000 && (!lastTrial || now - lastTrial > 120000)) { lastTrial = now; freeUntil = now + 10000; spareSince = 0; LOGI("frame limit: the game has time to spare - trying the full %.0f Hz for 10 s", maxHz); }
                else hz = cur;
            }
        } else if (locked) { locked = false; LOGI("frame limit: headset back at %.0f Hz - paced at its full rate", cur); }
        return g_limitMode == "headset_x2" ? hz * 2 : hz;
    }
    double v = atof(g_limitMode.c_str());
    return v >= 10 ? v : 0;
}
void XrFrameLimit() {
    static bool period = false;
    double hz = FrameLimitHz();
    static LARGE_INTEGER next = {};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);   // 100 ns units (realtime.h)
    if (hz <= 0) { next.QuadPart = 0; return; }
    if (!period) { period = true; timeBeginPeriod(1); }
    LONGLONG step = (LONGLONG)(1e7 / hz);
    // late (the frame took longer than the period): start the schedule again from now instead of catching up -
    // catching up released the next frames with no wait at all, a burst then a gap (judder / hitches)
    if (next.QuadPart == 0 || now.QuadPart - next.QuadPart > step / 4) next.QuadPart = now.QuadPart;
    LONGLONG wait = next.QuadPart - now.QuadPart;
    if (wait > 20000) Sleep((DWORD)((wait - 15000) / 10000));             // all but the last 1.5 ms
    for (;;) { QueryPerformanceCounter(&now); if (now.QuadPart >= next.QuadPart) break; YieldProcessor(); }
    g_limN++; if (wait > 0) { g_limWaited++; g_limWaitMs += wait / 10000.0; }
    { double sp = step > 0 ? (double)(wait > 0 ? wait : 0) / (double)step : 0; g_limLastSpare = g_limLastSpare * 0.9 + sp * 0.1; }
    next.QuadPart += step;
}
void XrSetViewOptions(bool edgeFill, bool crop, const std::string& engineFov) {
    g_edgeFill = edgeFill; g_aspectCrop = crop;
    if (engineFov != g_engineFovMode) {
        g_engineFovMode = engineFov; g_engineFovFixed = (float)atof(engineFov.c_str());
        g_efOff = false; g_efBad = 0; g_efOk = false;   // re-check with the new setting
    }
}

void XrSetMode(int mode) {
    if (g_vr.mode == mode) return;
    g_vr.mode = mode;
    g_eyeRec[0].valid = g_eyeRec[1].valid = false;
    LOGI("xr: mode -> %s", ModeName(mode));
}

static void ReleaseBlur();
void DeDither(IDirect3DDevice9* d, IDirect3DTexture9* eye);   // dedither.cpp
void DeDitherRelease();
unsigned DeDitherPasses();
static void ReleaseResources() {
    DeDitherRelease();
    for (auto& t : g_eyeTex) if (t) { t->Release(); t = nullptr; }
    for (int i = 0; i < xrb::kSlots; i++) {
        if (g_pending[i].used) { xrb::ClientCancelSlot(i); g_pending[i].used = false; }
        if (g_slots[i].query) g_slots[i].query->Release();
        if (g_slots[i].tex) g_slots[i].tex->Release();
        g_slots[i] = Slot{};
    }
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    ReleaseBlur();
    ReleaseHud();
    g_w = g_h = 0; g_ew = g_eh = 0;
    g_eyeRec[0].valid = g_eyeRec[1].valid = false;
}

void XrOnLostDevice() { ReleaseResources(); }

static bool g_fakeHmd = false;
static DWORD g_resFailTick = 0;
static bool EnsureResources(IDirect3DDevice9* d, const D3DSURFACE_DESC& bd) {
    UINT ew = g_wantEw ? g_wantEw : bd.Width, eh = g_wantEh ? g_wantEh : bd.Height;
    if (g_slots[0].tex && bd.Width == g_w && bd.Height == g_h && bd.Format == g_bbFmt && ew == g_ew && eh == g_eh) return true;
    if (g_resFailTick && GetTickCount() - g_resFailTick < 5000) return false;   // failed recently: retry every 5 s
    g_resFailTick = GetTickCount();
    ReleaseResources();
    UINT w = bd.Width, h = bd.Height;
    for (int i = 0; i < 3; i++) {
        HRESULT hr = CallCreateTexture(d, w, h, 1, D3DUSAGE_RENDERTARGET, bd.Format, D3DPOOL_DEFAULT, &g_eyeTex[i], nullptr);
        if (FAILED(hr)) { LOGE("xr: eye texture %ux%u fmt %d failed %08lx", w, h, (int)bd.Format, hr); ReleaseResources(); return false; }
    }
    UINT hw = w > 2560 ? 2560 : w, hh = (UINT)((uint64_t)h * hw / w) & ~1u;
    UINT aw = ew * 2 > hw ? ew * 2 : hw, ah = eh + hh;
    xrb::TextureSet ts;
    ts.atlasWidth = aw; ts.atlasHeight = ah; ts.dxgiFormat = 87;   // DXGI_FORMAT_B8G8R8A8_UNORM = D3DFMT_A8R8G8B8
    ts.sync = xrb::SYNC_SLOT_STATE;
    ts.eyeRect[0] = { 0, 0, (int)ew, (int)eh }; ts.eyeRect[1] = { (int)ew, 0, (int)ew, (int)eh }; ts.hudRect = { 0, (int)eh, (int)hw, (int)hh };
    for (int i = 0; i < xrb::kSlots; i++) {
        HANDLE sh = nullptr;
        HRESULT hr = CallCreateTexture(d, aw, ah, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_slots[i].tex, &sh);
        if (FAILED(hr) || (!sh && !g_fakeHmd)) { LOGE("xr: shared atlas %ux%u failed %08lx (handle %p)", aw, ah, hr, sh); ReleaseResources(); return false; }
        g_slots[i].share = sh;
        ts.handles[i] = (uint64_t)(uintptr_t)sh;
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_EVENT, &g_slots[i].query))) g_slots[i].query = nullptr;
    }
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sb))) g_sb = nullptr;
    xrb::ClientPublishTextures(ts);
    g_resFailTick = 0;
    g_w = w; g_h = h; g_hw = hw; g_hh = hh; g_ew = ew; g_eh = eh; g_aw = aw; g_ah = ah; g_bbFmt = bd.Format; g_bbMs = bd.MultiSampleType;
    LOGI("xr: %d shared atlases %ux%u (eyes %ux%u from a %ux%u picture, screen / menu %ux%u, back buffer fmt %d)", xrb::kSlots, aw, ah, ew, eh, w, h, hw, hh, (int)bd.Format);
    return true;
}

// ---------------------------------------------------------------- own drawing into an atlas (fixed function)
struct QuadV { float x, y, z, rhw, u, v; };
enum QuadMode { QUAD_OPAQUE, QUAD_PREMUL, QUAD_PREMUL_FADE };
// hud_transparency (VR panel > HUD, live): 0 = the game's HUD / UI as drawn, 0.6 = 60 % see-through (premultiplied: colour and
// alpha scaled together)
static float g_hudTransp = 0.6f;
void XrSetHudTransparency(float t) { g_hudTransp = t < 0.f ? 0.f : t > 0.95f ? 0.95f : t; }
static IDirect3DSurface9* g_saveRT[4] = {}; static IDirect3DSurface9* g_saveDS = nullptr;

static void BeginOwn(IDirect3DDevice9* d, IDirect3DTexture9* atlas) {
    if (g_sb) g_sb->Capture();
    for (int i = 0; i < 4; i++) { g_saveRT[i] = nullptr; d->GetRenderTarget(i, &g_saveRT[i]); }
    d->GetDepthStencilSurface(&g_saveDS);
    IDirect3DSurface9* s = nullptr; atlas->GetSurfaceLevel(0, &s);
    d->SetRenderTarget(0, s); if (s) s->Release();
    for (int i = 1; i < 4; i++) if (g_saveRT[i]) d->SetRenderTarget(i, nullptr);
    d->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, g_aw, g_ah, 0.f, 1.f }; d->SetViewport(&vp);
    d->SetVertexShader(nullptr); d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    const DWORD rs[][2] = {
        { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_CULLMODE, D3DCULL_NONE },
        { D3DRS_LIGHTING, FALSE }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_SCISSORTESTENABLE, FALSE },
        { D3DRS_CLIPPLANEENABLE, 0 }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SRGBWRITEENABLE, FALSE },
        { D3DRS_SEPARATEALPHABLENDENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_TEXTUREFACTOR, 0xFFFFFFFF },
        { D3DRS_MULTISAMPLEANTIALIAS, FALSE } };
    for (auto& r : rs) d->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0); d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (int i = 1; i < 8; i++) d->SetTexture(i, nullptr);
}
static void Quad(IDirect3DDevice9* d, IDirect3DTexture9* t, float x, float y, float w, float h, QuadMode m) {
    d->SetTexture(0, t);
    if (m == QUAD_OPAQUE) {
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    } else {
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        if (m == QUAD_PREMUL_FADE && g_hudTransp > 0.001f) {
            DWORD k = (DWORD)((1.f - g_hudTransp) * 255.f + 0.5f);
            d->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(k, k, k, k));
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        }
    }
    float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x + w - 0.5f, y1 = y + h - 0.5f;
    QuadV v[4] = { { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 }, { x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadV));
    if (m == QUAD_PREMUL_FADE) { d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT); }
}
// textured rectangle with its own texture coordinates (opaque)
static void QuadUV(IDirect3DDevice9* d, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
    QuadV v[4] = { { x0, y0, 0, 1, u0, v0 }, { x1, y0, 0, 1, u1, v0 }, { x0, y1, 0, 1, u0, v1 }, { x1, y1, 0, 1, u1, v1 } };
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadV));
}
// ---- soft edge fill: the area outside the picture is the picture's edge stretched outward from a strongly blurred copy
// (soft, no hard lines); at the seam a short fade from the sharp edge into the blur keeps the join invisible.
static bool g_softEdge = true;
void XrSetSoftEdge(bool on) { g_softEdge = on; }
static IDirect3DTexture9* g_blur[2][8] = {}; static UINT g_blurW[8] = {}, g_blurH[8] = {}; static int g_blurN = 0; static bool g_blurOk[2] = {};
static void ReleaseBlur() {
    for (auto& e : g_blur) for (auto& t : e) if (t) { t->Release(); t = nullptr; }
    g_blurN = 0; g_blurOk[0] = g_blurOk[1] = false;
}
// halve until about 70 px wide: each linear half-size copy is a 2 x 2 box filter, together a wide smooth blur
static bool EnsureBlur(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt) {
    UINT bw = w / 2, bh = h / 2;
    if (g_blurN && g_blurW[0] == bw && g_blurH[0] == bh) return true;
    ReleaseBlur();
    int n = 0;
    while (n < 8 && bw >= 4 && bh >= 4) {
        g_blurW[n] = bw; g_blurH[n] = bh;
        for (int e = 0; e < 2; e++)
            if (FAILED(CallCreateTexture(d, bw, bh, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &g_blur[e][n], nullptr))) { ReleaseBlur(); return false; }
        n++;
        if (bw <= 80 || bw * 48 <= w) break;   // about 1/48 of the picture wide (at most 80 px)
        bw /= 2; bh /= 2;
    }
    g_blurN = n;
    return n > 0;
}
static void BuildBlur(IDirect3DDevice9* d, int e, IDirect3DSurface9* src, const RECT* r) {
    if (!g_blurN) return;
    IDirect3DSurface9* prev = src; const RECT* pr = r; bool ok = true;
    for (int i = 0; i < g_blurN && ok; i++) {
        IDirect3DSurface9* s = nullptr;
        g_blur[e][i]->GetSurfaceLevel(0, &s);
        ok = s && SUCCEEDED(d->StretchRect(prev, pr, s, nullptr, D3DTEXF_LINEAR));
        if (prev != src) prev->Release();
        prev = s; pr = nullptr;
    }
    if (prev && prev != src) prev->Release();
    g_blurOk[e] = ok;
}
struct QuadVC { float x, y, z, rhw; DWORD c; float u, v; };
static void QuadVCs(IDirect3DDevice9* d, const QuadVC* v) {   // 4 vertices, strip order: x0y0, x1y0, x0y1, x1y1
    QuadVC q[4]; memcpy(q, v, sizeof q);
    for (auto& x : q) { x.x -= 0.5f; x.y -= 0.5f; }
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVC));
}
// The output field of view of one eye: the picture's own (cf), or with edge fill the union with the headset's (hf).
// 1.0.1: plus edge_fill_margin degrees beyond the headset's view on every side. The headset only shows that band when it
// re-projects an older frame (fps below the refresh rate, combat, Smart Smoothing): turning the head then pulls the
// frame's edge into view, which was black. Now it holds the picture's edge stretched outward (soft), so no black shows.
static float g_edgeMarginDeg = 4.f;
void XrSetEdgeMargin(float deg) { g_edgeMarginDeg = deg < 0.f ? 0.f : deg > 20.f ? 20.f : deg; }
static xrb::IpcFov OutFov(const xrb::IpcFov& cf, const xrb::IpcFov* hf) {
    if (!hf) return cf;
    const float m = g_edgeMarginDeg * 0.01745329f, lim = 80.f * 0.01745329f;
    xrb::IpcFov h = { fmaxf(hf->left - m, -lim), fminf(hf->right + m, lim), fminf(hf->up + m, lim), fmaxf(hf->down - m, -lim) };
    return { fminf(cf.left, h.left), fmaxf(cf.right, h.right), fmaxf(cf.up, h.up), fminf(cf.down, h.down) };
}
// ---- auto zoom: when the game's picture is narrower than the headset (a narrow cutscene shot the engine could not
// widen enough, or a camera cut caught for a frame or two), the picture is enlarged until it covers the headset view,
// so no edge fill shows. Eased in / out; 1 (off) whenever the picture already covers the view (normal gameplay).
static bool g_autoZoom = true; static float g_zoom = 1.f; static DWORD g_zoomTick = 0, g_zoomLog = 0;
void XrSetAutoZoom(bool on) { g_autoZoom = on; }
static float NeedZoom(const xrb::IpcFov& cf, const xrb::IpcFov& hf) {
    float z = 1.f;
    auto r = [&](float h, float c) { float th = tanf(fabsf(h)), tc = tanf(fabsf(c)); return tc > 1e-3f ? th / tc : 1.f; };
    z = fmaxf(z, r(hf.up, cf.up)); z = fmaxf(z, r(hf.down, cf.down)); z = fmaxf(z, r(hf.left, cf.left)); z = fmaxf(z, r(hf.right, cf.right));
    return z;
}
static xrb::IpcFov ZoomFov(const xrb::IpcFov& f, float z) {
    if (z <= 1.0001f) return f;
    return { atanf(tanf(f.left) * z), atanf(tanf(f.right) * z), atanf(tanf(f.up) * z), atanf(tanf(f.down) * z) };
}
static int g_zoomNarrow = 0;
static void UpdateZoom(const xrb::IpcFov& cf0, const xrb::IpcFov& cf1) {
    DWORD now = GetTickCount(); float dt = g_zoomTick ? (now - g_zoomTick) / 1000.f : 0.f; g_zoomTick = now;
    if (dt > 0.1f) dt = 0.1f;
    float need = 1.f;
    if (g_autoZoom && g_hfovValid && g_efOk && g_vr.engineCam) {   // only with the engine view working (it normally covers the headset)
        need = fmaxf(NeedZoom(cf0, g_hfov[0]), NeedZoom(cf1, g_hfov[1]));
        need = need > 1.02f ? fminf(need * 1.01f, 4.f) : 1.f;   // a hair more, so the very edge never shows
    }
    // camera cuts: for 1 - 2 frames the new shot is not widened yet - ignore such spikes (zooming in and straight out
    // again was the abrupt pumping); a narrow picture that stays (3+ frames) is zoomed
    g_zoomNarrow = need > 1.f ? g_zoomNarrow + 1 : 0;
    float target = g_zoomNarrow >= 3 ? need : 1.f;
    // glide: in over ~0.15 s, out over ~0.6 s (exponential ease)
    float tau = target > g_zoom ? 0.15f : 0.6f;
    g_zoom += (target - g_zoom) * (1.f - expf(-dt / tau));
    if (fabsf(g_zoom - 1.f) < 0.002f && target == 1.f) g_zoom = 1.f;
    if (g_zoom > 1.05f && now - g_zoomLog > 5000) {
        g_zoomLog = now;
        LOGI("view: auto zoom x%.2f - the game's picture is narrower than the headset (narrow cutscene shot?)", target > 1.f ? target : g_zoom);
    }
}
// One eye into its atlas rectangle: the 3D picture (content rect of the captured back buffer) placed by angle, and with
// edge fill the rest of the rectangle filled by repeating the picture's outermost rows / columns / corner pixels.
static void EyeQuad(IDirect3DDevice9* d, IDirect3DTexture9* t, float ex, const xrb::IpcFov& cf, const xrb::IpcFov& of) {
    d->SetTexture(0, t);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    const float* c = g_vr.content;
    float px0 = c[0] * g_w, px1 = c[2] * g_w, py0 = c[1] * g_h, py1 = c[3] * g_h;
    if (px1 - px0 < 2 || py1 - py0 < 2) { px0 = 0; py0 = 0; px1 = (float)g_w; py1 = (float)g_h; }
    float u0 = px0 / g_w, u1 = px1 / g_w, v0 = py0 / g_h, v1 = py1 / g_h;
    float ue0 = (floorf(px0) + 0.5f) / g_w, ue1 = (ceilf(px1) - 0.5f) / g_w, ve0 = (floorf(py0) + 0.5f) / g_h, ve1 = (ceilf(py1) - 0.5f) / g_h;
    float tl = tanf(of.left), tr = tanf(of.right), tu = tanf(of.up), td = tanf(of.down);
    if (tr - tl < 1e-4f || tu - td < 1e-4f) { QuadUV(d, ex, 0, ex + g_ew, (float)g_eh, u0, v0, u1, v1); return; }
    auto X = [&](float a) { return ex + g_ew * (tanf(a) - tl) / (tr - tl); };
    auto Y = [&](float a) { return g_eh * (tu - tanf(a)) / (tu - td); };
    float xs[4] = { ex, X(cf.left), X(cf.right), ex + g_ew }, ys[4] = { 0, Y(cf.up), Y(cf.down), (float)g_eh };
    for (int i = 1; i < 3; i++) { xs[i] = fminf(fmaxf(xs[i], xs[0]), xs[3]); ys[i] = fminf(fmaxf(ys[i], ys[0]), ys[3]); }
    int e = (t == g_eyeTex[1]) ? 1 : 0;   // the eye whose picture this is (head-tracked mono shows eye 0 twice)
    bool soft = g_edgeFill && g_softEdge && g_blurN && g_blurOk[e] && xs[2] - xs[1] > 8 && ys[2] - ys[1] > 8;
    if (soft) {
        // picture-relative coordinates: 0..1 across the picture, beyond it on the borders (mirrored by the sampler)
        auto PU = [&](float x) { return (x - xs[1]) / (xs[2] - xs[1]); };
        auto PV = [&](float y) { return (y - ys[1]) / (ys[2] - ys[1]); };
        // same brightness as the picture everywhere: the fill reads as one seamless image (no darkening)
        auto Shade = [&](float, float) { return D3DCOLOR_ARGB(255, 255, 255, 255); };
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        // 1. the borders from the blurred picture, stretched straight outward (clamped at its edge, no mirroring): the
        //    picture's edge colours continue softly to the edge of the view.
        auto Sat = [&](float p) { return p; };
        d->SetTexture(0, g_blur[e][g_blurN - 1]);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) {
                if (r == 1 && k == 1) continue;
                float x0 = xs[k], x1 = xs[k + 1], y0 = ys[r], y1 = ys[r + 1];
                if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) continue;
                int nx = k == 1 ? 1 : 10, ny = r == 1 ? 1 : 10;   // subdivide along the outward direction(s)
                for (int j = 0; j < ny; j++)
                    for (int i = 0; i < nx; i++) {
                        float a0 = x0 + (x1 - x0) * i / nx, a1 = x0 + (x1 - x0) * (i + 1) / nx;
                        float b0 = y0 + (y1 - y0) * j / ny, b1 = y0 + (y1 - y0) * (j + 1) / ny;
                        QuadVC v[4] = { { a0, b0, 0, 1, Shade(a0, b0), Sat(PU(a0)), Sat(PV(b0)) }, { a1, b0, 0, 1, Shade(a1, b0), Sat(PU(a1)), Sat(PV(b0)) },
                                        { a0, b1, 0, 1, Shade(a0, b1), Sat(PU(a0)), Sat(PV(b1)) }, { a1, b1, 0, 1, Shade(a1, b1), Sat(PU(a1)), Sat(PV(b1)) } };
                        QuadVCs(d, v);
                    }
            }
        // 2. at the seam: the sharp picture's edge, stretched, fading into the blur over a short band (only when the
        //    picture fills its texture - the clamp then holds exactly the picture's edge pixels)
        bool full = u0 < 0.002f && v0 < 0.002f && u1 > 0.998f && v1 > 0.998f;
        if (full) {
            d->SetTexture(0, t);
            d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            float fx = (xs[2] - xs[1]) * 0.035f, fy = (ys[2] - ys[1]) * 0.035f;   // band: 3.5 % of the picture
            float bx0 = fmaxf(xs[0], xs[1] - fx), bx1 = fminf(xs[3], xs[2] + fx), by0 = fmaxf(ys[0], ys[1] - fy), by1 = fminf(ys[3], ys[2] + fy);
            auto C = [&](float a, float x, float y) { DWORD g = Shade(x, y) & 0xFF; return D3DCOLOR_ARGB((int)(a * 255.f), g, g, g); };
            auto Band = [&](float x0, float y0, float x1, float y1, float a00, float a10, float a01, float a11) {
                if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;
                QuadVC v[4] = { { x0, y0, 0, 1, C(a00, x0, y0), PU(x0), PV(y0) }, { x1, y0, 0, 1, C(a10, x1, y0), PU(x1), PV(y0) },
                                { x0, y1, 0, 1, C(a01, x0, y1), PU(x0), PV(y1) }, { x1, y1, 0, 1, C(a11, x1, y1), PU(x1), PV(y1) } };
                QuadVCs(d, v);
            };
            Band(bx0, ys[1], xs[1], ys[2], 0, 1, 0, 1);           // left
            Band(xs[2], ys[1], bx1, ys[2], 1, 0, 1, 0);           // right
            Band(xs[1], by0, xs[2], ys[1], 0, 0, 1, 1);           // top
            Band(xs[1], ys[2], xs[2], by1, 1, 1, 0, 0);           // bottom
            Band(bx0, by0, xs[1], ys[1], 0, 0, 0, 1);             // corners: opaque only at the picture's corner
            Band(xs[2], by0, bx1, ys[1], 0, 0, 1, 0);
            Band(bx0, ys[2], xs[1], by1, 0, 1, 0, 0);
            Band(xs[2], ys[2], bx1, by1, 1, 0, 0, 0);
            d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }
        // back to the plain opaque state for the picture itself
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        d->SetTexture(0, t);
        QuadUV(d, xs[1], ys[1], xs[2], ys[2], u0, v0, u1, v1);
        return;
    }
    float us[3][2] = { { ue0, ue0 }, { u0, u1 }, { ue1, ue1 } }, vs[3][2] = { { ve0, ve0 }, { v0, v1 }, { ve1, ve1 } };
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            if (!g_edgeFill && (r != 1 || k != 1)) continue;
            QuadUV(d, xs[k], ys[r], xs[k + 1], ys[r + 1], us[k][0], vs[r][0], us[k][1], vs[r][1]);
        }
}
// atlas eye size that keeps the picture's pixel density over the output field of view
static void UpdateWantedEyeSize(const xrb::IpcFov& cf) {
    if (!g_w || !g_h) return;
    const float* c = g_vr.content;
    float cw = (c[2] - c[0]) * g_w, ch = (c[3] - c[1]) * g_h;
    if (cw < 16 || ch < 16) return;
    float sx = 1.f, sy = 1.f;
    if (g_edgeFill && g_hfovValid) {
        float ctx = tanf(cf.right) - tanf(cf.left), cty = tanf(cf.up) - tanf(cf.down);
        for (int e = 0; e < 2; e++) {
            xrb::IpcFov o = OutFov(cf, &g_hfov[e]);
            if (ctx > 1e-4f) sx = fmaxf(sx, (tanf(o.right) - tanf(o.left)) / ctx);
            if (cty > 1e-4f) sy = fmaxf(sy, (tanf(o.up) - tanf(o.down)) / cty);
        }
    }
    float w = cw * sx, h = ch * sy, m = fmaxf(w, h);
    if (m > g_edgeMaxSide) { w *= g_edgeMaxSide / m; h *= g_edgeMaxSide / m; }
    UINT nw = ((UINT)w + 15) & ~15u, nh = ((UINT)h + 15) & ~15u;
    if (nw < 64) nw = 64;
    if (nh < 64) nh = 64;
    // only follow real changes (> 8% for 1 s): every change re-creates the shared textures
    static unsigned since = 0;
    UINT cw0 = g_wantEw ? g_wantEw : g_w, ch0 = g_wantEh ? g_wantEh : g_h;
    bool differ = fabsf((float)nw / cw0 - 1.f) > 0.08f || fabsf((float)nh / ch0 - 1.f) > 0.08f;
    if (!differ) { since = 0; return; }
    if (++since < 60) return;
    since = 0;
    LOGI("xr: eye image %ux%u -> %ux%u (picture %.0fx%.0f, view %.2f x %.2f of the picture%s)", cw0, ch0, nw, nh, cw, ch, sx, sy, g_edgeFill ? ", edge fill" : "");
    g_wantEw = nw; g_wantEh = nh;
}
static void EndOwn(IDirect3DDevice9* d) {
    for (int i = 0; i < 4; i++) { if (i == 0 || g_saveRT[i]) d->SetRenderTarget(i, g_saveRT[i]); if (g_saveRT[i]) g_saveRT[i]->Release(); g_saveRT[i] = nullptr; }
    d->SetDepthStencilSurface(g_saveDS); if (g_saveDS) { g_saveDS->Release(); g_saveDS = nullptr; }
    if (g_sb) g_sb->Apply();
}
// menu centred in the HUD rect at menuWidth of its width
static void MenuQuad(IDirect3DDevice9* d, IDirect3DTexture9* menu, float menuWidth) {
    D3DSURFACE_DESC md; menu->GetLevelDesc(0, &md);
    float w = g_hw * menuWidth, h = w * md.Height / md.Width;
    if (h > g_hh * 0.98f) { h = g_hh * 0.98f; w = h * md.Width / md.Height; }
    Quad(d, menu, (g_hw - w) * 0.5f, g_eh + (g_hh - h) * 0.5f, w, h, QUAD_PREMUL);
}

// ---------------------------------------------------------------- diagnostics: downscaled BMP of a surface
static void DumpSurface(IDirect3DDevice9* d, IDirect3DSurface9* src, const wchar_t* name) {
    D3DSURFACE_DESC sd; if (!src || FAILED(src->GetDesc(&sd))) return;
    UINT w = sd.Width, h = sd.Height;
    while (w > 1920) { w = (w + 1) / 2; h = (h + 1) / 2; }
    IDirect3DSurface9 *small = nullptr, *sys = nullptr;
    HRESULT hr = d->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &small, nullptr);
    if (SUCCEEDED(hr)) hr = d->StretchRect(src, nullptr, small, nullptr, D3DTEXF_LINEAR);
    if (SUCCEEDED(hr)) hr = d->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = d->GetRenderTargetData(small, sys);
    D3DLOCKED_RECT lr;
    double sum = 0; unsigned alpha0 = 0;
    if (SUCCEEDED(hr) && SUCCEEDED(hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        FILE* f = _wfopen((g_modDir + L"\\" + name).c_str(), L"wb");
        BITMAPFILEHEADER bh = { 0x4d42, (DWORD)(54 + w * h * 4), 0, 0, 54 };
        BITMAPINFOHEADER bi = { 40, (LONG)w, -(LONG)h, 1, 32 };
        if (f) { fwrite(&bh, 14, 1, f); fwrite(&bi, 40, 1, f); }
        for (UINT y = 0; y < h; y++) {
            const DWORD* row = (const DWORD*)((const char*)lr.pBits + y * lr.Pitch);
            if (f) fwrite(row, 4, w, f);
            for (UINT i = 0; i < w; i++) { DWORD p = row[i]; sum += ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255); if (!(p >> 24)) alpha0++; }
        }
        if (f) fclose(f);
        sys->UnlockRect();
    }
    char nm[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, MAX_PATH, nullptr, nullptr);
    if (SUCCEEDED(hr)) LOGI("diag: %s (%ux%u fmt %d -> %ux%u): mean brightness %.1f / 255, %.0f%% alpha 0", nm, sd.Width, sd.Height, (int)sd.Format, w, h,
                            sum / (3.0 * w * h), 100.0 * alpha0 / (w * h));
    else LOGW("diag: %s failed %08lx", nm, hr);
    if (sys) sys->Release();
    if (small) small->Release();
}
static int g_dumpWanted = 0;
static bool g_dumpSentPending = false;          // bit 1: next flat frame, bit 2: next 3D frame, 4: any (F10)
static unsigned g_dumpId = 0;

// ---------------------------------------------------------------- host link
// starts XrBridgeHost (once): also used at start-up (XrHeadsetDetect), before the game reads User.ini
static bool g_hostStarted = false;
static void StartHost() {
    if (g_hostStarted) return;
    g_hostStarted = true;
    xrb::ClientSettings s;
    s.instanceName = GAME_SHORT;
    s.appName = GAME_NAME;
    s.hostExePath = g_gameDir + L"\\XrBridgeHost.exe";
    s.hostLogPath = g_modDir + L"\\xrhost.log";
    s.menuDistance = ConfigFloat("screen_distance", 2.0f); s.menuWidth = ConfigFloat("screen_width", 2.6f);
    s.hudDistance = ConfigFloat("hud_panel_distance", 1.25f); s.hudWidth = 1.6f;
    s.hudOffsetY = ConfigFloat("hud_panel_offset_y", -0.05f); s.hudFollowHead = ConfigBool("hud_panel_follow_head", true); s.hudPremultiplied = true;
    xrb::ClientInit(s);
}
// 1.0.1: which OpenXR API layers the headset host will load (OpenXR Toolkit, Pimax / Varjo layers, ...). The host uses
// the standard Khronos loader, so every enabled implicit layer runs as with any other OpenXR game; this only lists them.
static void LogXrLayers() {
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        HKEY k = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i++) {
            wchar_t name[1024]; DWORD nl = 1024, type = 0, val = 1, vl = sizeof val;
            if (RegEnumValueW(k, i, name, &nl, nullptr, &type, (BYTE*)&val, &vl) != ERROR_SUCCESS) break;
            const wchar_t* f = wcsrchr(name, L'\\'); f = f ? f + 1 : name;
            LOGI("xr: OpenXR layer %ls - %s", f, type == REG_DWORD && val == 0 ? "enabled (loaded by the headset host)" : "disabled in the registry");
        }
        RegCloseKey(k);
    }
}
static void InitXr() {
    LogXrLayers();
    g_xrInit = true;
    SettingsLoad();
    g_mirror = ConfigBool("mirror_right_eye", true);
    g_minXfDraws = (float)ConfigInt("min_3d_draws", 20);
    g_fakeHmd = ConfigBool("debug_fake_hmd", false); g_fakeYaw = ConfigFloat("debug_fake_head_yaw", 0.f);
    g_keyScreenshot = VkFromName(ConfigStr("screenshot_key", "delete"), VK_DELETE);
    g_keyRecenter = VkFromName(ConfigStr("recenter_key", "home"), VK_HOME);
    g_keyHud = VkFromName(ConfigStr("hud_toggle_key", "f7"), VK_F7);
    g_keyFp = VkFromName(ConfigStr("first_person_key", "end"), VK_END);
    g_shotSbs = ConfigStr("screenshot_mode", "eye") == "sbs";
    { extern bool g_screenshotSquare; g_screenshotSquare = ConfigBool("screenshot_square", true); }
    g_edgeFill = ConfigBool("edge_fill", true);
    g_softEdge = ConfigBool("edge_fill_soft", true);
    g_edgeMarginDeg = ConfigFloat("edge_fill_margin", 4.f); XrSetEdgeMargin(g_edgeMarginDeg);
    g_autoZoom = ConfigBool("auto_zoom", true);
    g_aspectCrop = ConfigStr("aspect_fit", "crop") != "extend";
    g_edgeMaxSide = (float)ConfigInt("eye_max_side", 8192);   // 1.0.1: 4096 would shrink a 4352-wide eye (softer)
    if (g_edgeMaxSide < 512) g_edgeMaxSide = 512;
    g_engineFovMode = ConfigStr("engine_fov", "auto");
    g_engineFovFixed = (float)atof(g_engineFovMode.c_str());
    g_engineFovMax = ConfigFloat("engine_fov_max", 24.f);   // narrow cutscene shots (telephoto, 8-15 deg) need up to about x10-20
    g_engineFovMargin = ConfigFloat("engine_fov_margin", 1.0f);
    g_efHeadMargin = ConfigBool("engine_fov_head_margin", true);
    StartHost();
    SettingsApply();
    LOGI("xr: host launched, mode %s, world scale %.2f, fov scale %.2f", ModeName(g_vr.mode), g_vr.worldScale, g_vr.fovScale);
}

// headset's recommended per-eye size, for render_resolution = headset (asked at device creation). The last size is
// kept in AC2VR\headset_size.txt so later launches need not wait for the headset.
static unsigned g_recW = 0, g_recH = 0; static bool g_recLive = false;
bool XrHeadsetEyeSize(unsigned* w, unsigned* h) {
    static bool tried = false;
    std::wstring cache = g_modDir + L"\\headset_size.txt";
    if (!tried) {
        tried = true;
        if (FILE* f = _wfopen(cache.c_str(), L"r")) { if (fscanf(f, "%u %u", &g_recW, &g_recH) != 2) g_recW = g_recH = 0; fclose(f); }
        if (g_recW) LOGI("xr: headset eye size %ux%u (from the last run)", g_recW, g_recH);
        if (!g_xrInit) InitXr();
        unsigned a = 0, b = 0;
        if (xrb::ClientWaitRecommendedSize(&a, &b, g_recW ? 1500 : (unsigned)ConfigInt("headset_wait_ms", 10000))) {
            g_recLive = true;
            if (a != g_recW || b != g_recH) {
                g_recW = a; g_recH = b;
                if (FILE* f = _wfopen(cache.c_str(), L"w")) { fprintf(f, "%u %u\n", a, b); fclose(f); }
            }
        } else if (!g_recW) LOGW("xr: the headset did not report its resolution - using the game's resolution");
    }
    if (!g_recW || !g_recH) return false;
    *w = g_recW; *h = g_recH;
    return true;
}
static void RecSizeLater() {   // the host came up after the device: remember the size for the next launch
    if (g_recLive || !xrb::ClientHostConnected()) return;
    unsigned a = 0, b = 0;
    if (xrb::ClientWaitRecommendedSize(&a, &b, 0)) {
        g_recLive = true;
        if (a != g_recW || b != g_recH) {
            LOGI("xr: headset eye size %ux%u (used from the next launch with render_resolution = headset)", a, b);
            if (FILE* f = _wfopen((g_modDir + L"\\headset_size.txt").c_str(), L"w")) { fprintf(f, "%u %u\n", a, b); fclose(f); }
        }
        g_recW = a; g_recH = b;
    }
}

// symmetric eye view: how much wider (x, y) the centred view is than the headset's own - stored after the size in
// headset_size.txt ("w h rx ry") so the next launch renders that much larger (same pixel density)
static void SaveSymmetricRatio() {
    if (!g_recW || !g_recH || !xrb::ClientHostConnected()) return;
    xrb::IpcPose p[2]; xrb::IpcFov f[2]; int64_t t;
    if (!xrb::ClientGetLatestPoses(p, f, &t)) return;
    float rx = 1, ry = 1;
    for (int e = 0; e < 2; e++) {
        float l = tanf(-f[e].left), r = tanf(f[e].right), u = tanf(f[e].up), d = tanf(-f[e].down);
        if (l + r <= 0 || u + d <= 0) return;
        rx = fmaxf(rx, 2.f * fmaxf(l, r) / (l + r)); ry = fmaxf(ry, 2.f * fmaxf(u, d) / (u + d));
    }
    static float savedX = 0, savedY = 0;
    if (fabsf(rx - savedX) < 0.005f && fabsf(ry - savedY) < 0.005f) return;
    if (FILE* fl = _wfopen((g_modDir + L"\\headset_size.txt").c_str(), L"w")) {
        fprintf(fl, "%u %u %.4f %.4f\n", g_recW, g_recH, rx, ry); fclose(fl);
        savedX = rx; savedY = ry;
        LOGI("xr: headset eye %ux%u, a centred view is %.3f x %.3f as wide / tall (used for symmetric_projection from the next launch)", g_recW, g_recH, rx, ry);
    }
}

// Start-up headset detection (any OpenXR headset): before the game reads User.ini the host is started and asked for
// the headset's own per-eye resolution and per-eye field of view. Both go to headset_size.txt ("w h rx ry"), from which
// w2ini.cpp sets the game's resolution and aspect ratio - so a different headset is picked up on its first launch
// instead of the one after. Waits at most headset_detect_ms (the headset off / runtime missing: the last values kept).
bool XrHeadsetDetect(unsigned timeoutMs) {
    StartHost();
    DWORD t0 = GetTickCount();
    unsigned a = 0, b = 0;
    if (!xrb::ClientWaitRecommendedSize(&a, &b, timeoutMs)) { LOGW("xr: start-up headset detection: no answer from the headset in %u ms - using the last known size", timeoutMs); return false; }
    unsigned prevW = 0, prevH = 0; float prx = 1, pry = 1;
    if (FILE* f = _wfopen((g_modDir + L"\\headset_size.txt").c_str(), L"r")) { if (fscanf(f, "%u %u %f %f", &prevW, &prevH, &prx, &pry) < 2) prevW = prevH = 0; fclose(f); }
    // the eye FOV arrives once the OpenXR session runs (a few frames after the size)
    xrb::IpcPose p[2]; xrb::IpcFov fv[2]; int64_t t; bool haveFov = false;
    while (GetTickCount() - t0 < timeoutMs + 3000) {
        if (xrb::ClientGetLatestPoses(p, fv, &t)) { haveFov = true; break; }
        Sleep(50);
    }
    float rx = 1, ry = 1;
    if (haveFov) {
        for (int e = 0; e < 2; e++) {
            float l = tanf(-fv[e].left), r = tanf(fv[e].right), u = tanf(fv[e].up), d = tanf(-fv[e].down);
            if (l + r <= 0 || u + d <= 0) { haveFov = false; break; }
            rx = fmaxf(rx, 2.f * fmaxf(l, r) / (l + r)); ry = fmaxf(ry, 2.f * fmaxf(u, d) / (u + d));
        }
    }
    if (!haveFov) { rx = (prevW == a && prevH == b) ? prx : 1.f; ry = (prevW == a && prevH == b) ? pry : 1.f; }
    g_recW = a; g_recH = b; g_recLive = true;
    if (FILE* f = _wfopen((g_modDir + L"\\headset_size.txt").c_str(), L"w")) { fprintf(f, "%u %u %.4f %.4f\n", a, b, rx, ry); fclose(f); }
    if (haveFov) {
        const float k = 57.29578f;
        LOGI("xr: headset detected: %ux%u per eye, FOV L %.1f/%.1f/%.1f/%.1f R %.1f/%.1f/%.1f/%.1f deg (left/right/up/down), centred view x %.3f / %.3f%s",
             a, b, fv[0].left * k, fv[0].right * k, fv[0].up * k, fv[0].down * k, fv[1].left * k, fv[1].right * k, fv[1].up * k, fv[1].down * k, rx, ry,
             (prevW && (prevW != a || prevH != b)) ? " - a different headset than last time: resolution and aspect updated" : "");
    } else LOGI("xr: headset detected: %ux%u per eye (its FOV not reported in time - the centred-view ratio from the last run)", a, b);
    return true;
}

static void PollPending(IDirect3DDevice9* d) {
    for (int i = 0; i < xrb::kSlots; i++) {
        Pending& p = g_pending[i];
        if (!p.used) continue;
        HRESULT hr = g_slots[i].query ? g_slots[i].query->GetData(nullptr, 0, D3DGETDATA_FLUSH) : S_OK;
        if (hr == S_FALSE && GetTickCount() - p.tick < 500) continue;
        if (hr == S_FALSE) g_gpuLate++;
        xrb::ClientSubmitSlot(i, p.pose, p.fov, p.time, p.flags);
        p.used = false; g_submitted++;
    }
}

static void Keys() {
    // (no fixed F9 recentre: F9 is quickload and Space is dodge in The Witcher 2 - recenter_key, default Home)
    static bool f8 = false, f10 = false;
    bool k8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0, k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (k10 && !f10) { g_dumpWanted |= 4; D3D9RequestStageDump(); LOGI("diag: F10 - dumping the next frame (and its stages to W2VR\\stages)"); }
    f10 = k10;
    static bool fShot = false, fFp = false;
    bool kShot = (GetAsyncKeyState(g_keyScreenshot) & 0x8000) != 0, kFp = g_keyHud && (GetAsyncKeyState(g_keyHud) & 0x8000) != 0;
    if (kShot && !fShot && !MenuIsOpen()) g_shotWanted = true;   // Delete is "undo" while the VR menu is open
    static DWORD dbgShot = ConfigInt("debug_screenshot_ms", 0) ? GetTickCount() + ConfigInt("debug_screenshot_ms", 0) : 0;
    if (dbgShot && GetTickCount() > dbgShot) { dbgShot = 0; g_shotWanted = true; }
    if (kFp && !fFp && !MenuIsOpen()) XrCycleHudMode();
    {   static bool fFp1 = false;
        bool k = g_keyFp && (GetAsyncKeyState(g_keyFp) & 0x8000) != 0;
        if (k && !fFp1 && !MenuIsOpen()) MenuToggleFirstPerson();
        fFp1 = k;
    }
    fShot = kShot; fFp = kFp;
    if (k8 && !f8) XrSetMode((g_vr.mode + 1) % 3);
    f8 = k8;
    {   // recenter key (default Home). Not while the VR panel is open.
        static bool fRc = false;
        bool kRc = g_keyRecenter && (GetAsyncKeyState(g_keyRecenter) & 0x8000) != 0;
        if (kRc && !fRc && !MenuIsOpen()) XrRequestRecenter();
        fRc = kRc;
    }
}

static xrb::IpcFov FovUnion(const xrb::IpcFov f[2]) {
    return { fminf(f[0].left, f[1].left), fmaxf(f[0].right, f[1].right), fmaxf(f[0].up, f[1].up), fminf(f[0].down, f[1].down) };
}

// ---------------------------------------------------------------- engine FOV controller
// auto: the engine's own vertical view is widened until it covers the headset's, so the engine culls for (and renders)
// the whole VR view. The relation between the game's fov value and the projection the shaders get is measured first
// (calibration at scale 1), then checked after widening; if the projection does not follow, the feature turns off.
static float g_efLastK = 1.f; static LONG g_efLastCalls = 0;
static float g_efHoldYaw = 0, g_efHoldPitch = 0; static DWORD g_efHoldTick = 0;
static float g_efCenterT = 0;   // headset half-tan (vertical, for the picture's aspect) the picture is sized to
// how far the head looks away from the game camera (radians, horizontal / vertical); the engine has to cull for that
static void HeadAngles(const xrb::IpcPose& q, float* yaw, float* pitch) {
    float x = q.qx, y = q.qy, z = q.qz, w = q.qw;
    // forward (0,0,-1) rotated
    float fx = -(2 * (x * z + w * y)), fy = -(2 * (y * z - w * x)), fz = -(1 - 2 * (x * x + y * y));
    *yaw = atan2f(fabsf(fx), -fz);
    *pitch = asinf(fminf(1.f, fabsf(fy)));
}
// (the engine FOV controller of the Assassin's Creed builds is not needed: the Witcher 2 engine camera is given the
// headset's own per-eye projection, w2camera.cpp)

// ---------------------------------------------------------------- frame-end step timing (finds stalls)
static double g_secMs[10]; static const char* g_secName[10] = { "keys", "menu", "capture", "poll", "compose", "screenshot", "mirror", "camera stats", "poses", "rest" };
static LARGE_INTEGER g_secT;
static inline void SecStart() { QueryPerformanceCounter(&g_secT); for (auto& v : g_secMs) v = 0; }
static inline void Sec(int i) { LARGE_INTEGER n; QueryPerformanceCounter(&n); g_secMs[i] += (n.QuadPart - g_secT.QuadPart) / 10000.0; g_secT = n; }

static double g_ownMs = 0, g_ownMaxMs = 0; static unsigned g_ownN = 0, g_drawsSum = 0;
// full stereo (w2camera.cpp, render thread = the D3D thread): the eye of the next draws, and the first eye's copy-out
extern IDirect3DDevice9* g_dev;
static volatile LONG g_fsMidN = 0;
void XrFullStereoPass(int eye, const xrb::IpcPose& pose, const GameProj& proj) {
    // preset what the draws are matched to (first match wins): the pass's exact eye pose and projection
    g_vr.drawEng = true; g_vr.drawEye = eye; g_vr.drawPose = pose; g_vr.drawProj = proj;
}
void XrFullStereoMid(int eye, const xrb::IpcPose& pose, const GameProj& proj) {
    IDirect3DDevice9* d = g_dev;
    if (!d || !g_eyeTex[eye]) return;
    struct Own { Own() { InterlockedIncrement(&g_inOwn); } ~Own() { InterlockedDecrement(&g_inOwn); } } own;   // the mod's own calls
    IDirect3DSurface9* bb = nullptr, *dst = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    g_eyeTex[eye]->GetSurfaceLevel(0, &dst);
    HRESULT hr = dst ? d->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE) : E_FAIL;
    if (SUCCEEDED(hr)) DeDither(d, g_eyeTex[eye]);   // the game's screen-door dither (shadows, decal blends) smoothed
    if (SUCCEEDED(hr) && g_edgeFill && g_softEdge) {
        D3DSURFACE_DESC ed; dst->GetDesc(&ed);
        const float* c = g_vr.content;
        RECT cr = { (LONG)(c[0] * ed.Width), (LONG)(c[1] * ed.Height), (LONG)(c[2] * ed.Width + 0.5f), (LONG)(c[3] * ed.Height + 0.5f) };
        if (cr.right - cr.left < 16 || cr.bottom - cr.top < 16) cr = { 0, 0, (LONG)ed.Width, (LONG)ed.Height };
        if (EnsureBlur(d, (UINT)(cr.right - cr.left), (UINT)(cr.bottom - cr.top), ed.Format)) BuildBlur(d, eye, dst, &cr);
    }
    if (dst) dst->Release();
    bb->Release();
    if (SUCCEEDED(hr)) { g_eyeRec[eye] = { true, pose, ProjToFov(proj), g_vr.poseTime, g_frame }; InterlockedIncrement(&g_fsMidN); }
    // the second pass draws the HUD again: start it transparent so it is not doubled
    if (g_hudSurf) d->ColorFill(g_hudSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
}

void XrOnPresentImpl(IDirect3DDevice9* d);
void XrOnPresent(IDirect3DDevice9* d) {
    extern bool g_vanilla;
    if (g_vanilla) {   // only F10 (stage dump); nothing else runs
        static bool f10 = false; bool k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (k10 && !f10) { D3D9RequestStageDump(); LOGI("diag: F10 - dumping the next frame's stages (vanilla)"); }
        f10 = k10; return;
    }
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    g_drawsSum += g_vr.drawsTotal;
    XrOnPresentImpl(d);
    QueryPerformanceCounter(&b);
    double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    if (ms > 40.0) {   // a stall: say which step took the time (at most every 2 s)
        static DWORD last = 0; DWORD now = GetTickCount();
        if (now - last > 2000) {
            last = now;
            char buf[400]; int o = 0;
            for (int i = 0; i < 10; i++) if (g_secMs[i] >= 1.0) o += snprintf(buf + o, sizeof buf - o, " %s %.0f", g_secName[i], g_secMs[i]);
            LOGW("xr: frame-end work took %.0f ms:%s", ms, buf);
        }
    }
    g_ownMs += ms; if (ms > g_ownMaxMs) g_ownMaxMs = ms; g_ownN++;
}
void XrOnPresentImpl(IDirect3DDevice9* d) {
    StereoAutoUpdate();
    if (!g_xrInit) InitXr();
    SecStart();
    g_frame++; g_fpsFrames++;
    DWORD now = GetTickCount();
    if (now - g_fpsTick >= 1000) { MenuNoteFps(g_fpsFrames * 1000.f / (now - g_fpsTick + 1)); g_fpsFrames = 0; g_fpsTick = now; }
    Keys();
    Sec(0);
    if (g_recenterReq) { g_recenterReq = 0; xrb::ClientRequestRecenter(); LOGI("xr: recenter"); }

    // what this frame was rendered as
    // engine camera: the eye / pose / projection the game camera really carried during this frame's draws
    bool engFrame = g_vr.drawEng; g_vr.drawEng = false;
    xrb::IpcPose engPose = g_vr.drawPose; GameProj engProj = g_vr.drawProj; int engEye = g_vr.drawEye;
    static bool shaderMainEver = false; if (StereoHaveMain()) shaderMainEver = true;
    static const bool noShaderFallback = ConfigBool("engine_no_shader_fallback", true);
    if (!engFrame && W2CamActive() && !shaderMainEver && noShaderFallback && W2CamLatestEye() >= 0) {
        // the game's shader constants were never recognised: take the camera built last (assumes no render-thread lag)
        int e = W2CamLatestEye();
        if (W2CamEyeRecord(e, &engPose, &engProj)) { engFrame = true; engEye = e; }
    }
    const bool wasVR = g_vr.active && (g_vr.drawsXf >= (unsigned)g_minXfDraws || (engFrame && (g_vr.drawsMain >= (unsigned)g_minXfDraws || !shaderMainEver)));
    const int eye = engFrame ? engEye : g_vr.eye;
    const xrb::IpcPose pose = engFrame ? engPose : g_vr.pose;
    // the projection covers the 3D picture's rectangle (content) of the back buffer: that rectangle is what goes out
    const xrb::IpcFov fov = engFrame ? ProjToFov(engProj) : ProjToFov(g_vr.outProj);
    const xrb::IpcFov flatFov = ProjToImageFov(g_vr.outProj, g_vr.content);
    const int64_t ptime = g_vr.poseTime;

    bool menuOpen = false;
    IDirect3DTexture9* menu = MenuUpdate(d, &menuOpen);
    Sec(1);

    bool host = xrb::ClientHostConnected();
    static DWORD hostSince = 0; static bool autoFlat = false, auto3d = false;
    if (host && !hostSince) hostSince = now;
    bool diagOn = host || g_fakeHmd;
    // automatic diagnostic images (GPU read-back + file write on the render thread = a hitch): only with diag_auto_dump
    static const bool autoDump = ConfigBool("diag_auto_dump", false);
    static const unsigned dbgDumpFrame = (unsigned)ConfigInt("debug_dump_frame", 0);   // tests: dump this frame
    if (dbgDumpFrame && g_frame == dbgDumpFrame) g_dumpWanted |= 4;
    if (autoDump && diagOn && hostSince && now - hostSince > 3000 && !autoFlat) { autoFlat = true; g_dumpWanted |= 1; }
    if (autoDump && diagOn && hostSince && wasVR && !auto3d && now - hostSince > 3000) { auto3d = true; g_dumpWanted |= 2; }
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC bd = {};
    if ((host || g_fakeHmd) && SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) bb->GetDesc(&bd);
    if (g_fakeHmd && !host && !hostSince) hostSince = now;   // debug: exercise the capture / dump path without a headset
    if (!host) g_noHost++;
    if (bb && EnsureResources(d, bd)) {
        static bool luidChecked = false;
        if (!luidChecked && host && xrb::ClientAdapterLuid()) {
            luidChecked = true;
            IDirect3D9* d3d = nullptr; IDirect3D9Ex* ex = nullptr; D3DDEVICE_CREATION_PARAMETERS cp;
            if (SUCCEEDED(d->GetDirect3D(&d3d)) && d3d) {
                if (SUCCEEDED(d->GetCreationParameters(&cp)) && SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&ex)) && ex) {
                    LUID l; if (SUCCEEDED(ex->GetAdapterLUID(cp.AdapterOrdinal, &l))) {
                        uint64_t mine = ((uint64_t)(uint32_t)l.HighPart << 32) | l.LowPart, hs = xrb::ClientAdapterLuid();
                        if (hs != mine) LOGW("xr: the game renders on a different GPU than the headset (LUID %llx vs %llx) - the host cannot open the frames",
                                             (unsigned long long)mine, (unsigned long long)hs);
                        else LOGI("xr: game and headset use the same GPU (LUID %llx)", (unsigned long long)mine);
                    }
                    ex->Release();
                }
                d3d->Release();
            }
        }
        // 1. capture this frame
        int target = wasVR ? (eye == 1 ? 1 : 0) : 2;
        IDirect3DSurface9* dst = nullptr;
        g_eyeTex[target]->GetSurfaceLevel(0, &dst);
        HRESULT hr = d->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
        if (SUCCEEDED(hr) && target < 2) DeDither(d, g_eyeTex[target]);   // 3D eye pictures only (menus / flat screen stay as drawn)
        if (SUCCEEDED(hr) && target < 2 && g_edgeFill && g_softEdge && dst) {   // soft edge fill: blur this eye's new picture
            D3DSURFACE_DESC ed; dst->GetDesc(&ed);
            const float* c = g_vr.content;
            RECT cr = { (LONG)(c[0] * ed.Width), (LONG)(c[1] * ed.Height), (LONG)(c[2] * ed.Width + 0.5f), (LONG)(c[3] * ed.Height + 0.5f) };
            if (cr.right - cr.left < 16 || cr.bottom - cr.top < 16) cr = { 0, 0, (LONG)ed.Width, (LONG)ed.Height };
            if (EnsureBlur(d, (UINT)(cr.right - cr.left), (UINT)(cr.bottom - cr.top), ed.Format)) BuildBlur(d, target, dst, &cr);
        }
        bool dumpNow = (g_dumpWanted & 4) || ((g_dumpWanted & 1) && !wasVR) || ((g_dumpWanted & 2) && wasVR);
        if (dumpNow) {
            g_dumpWanted &= wasVR ? ~6 : ~5;
            g_dumpId++;
            wchar_t n[64]; swprintf(n, 64, L"dump%u_%ls_backbuffer.bmp", g_dumpId, wasVR ? L"3d" : L"flat");
            DumpSurface(d, bb, n);
            LOGI("diag: dump %u (%s frame, eye %d, %u transformed draws, host %s)", g_dumpId, wasVR ? "3D" : "flat", eye, g_vr.drawsXf,
                 xrb::ClientHostVisible() ? "visible" : "connected, not visible");
            StereoLogTargetsNextFrame();
            g_dumpSentPending = true;
        }
        if (dst) dst->Release();
        if (FAILED(hr)) { static int n = 0; if (n++ < 5) LOGE("xr: back buffer copy failed %08lx", hr); }
        if (wasVR) {
            g_vrFrames++;
            if (W2FullStereoOn() && eye == 1 && g_eyeRec[0].valid && g_eyeRec[0].frame + 1 >= g_frame) g_eyeRec[0].frame = g_frame;   // same tick
            if (eye == 2) { g_eyeRec[0] = { true, pose, fov, ptime, g_frame }; g_eyeRec[1].valid = false; }
            else g_eyeRec[eye] = { true, pose, fov, ptime, g_frame };
        } else { g_flatFrames++; g_eyeRec[0].valid = g_eyeRec[1].valid = false; }
        if (wasVR) UpdateWantedEyeSize(fov);

        // 2. submit finished slots, then build a new one
        Sec(2);
        PollPending(d);
        Sec(3);
        bool stereoReady = g_vr.mode == VRMODE_STEREO && g_eyeRec[0].valid && g_eyeRec[1].valid &&
                           (g_frame - g_eyeRec[0].frame) <= 2 && (g_frame - g_eyeRec[1].frame) <= 2;
        bool headReady = g_vr.mode == VRMODE_HEAD && wasVR && g_eyeRec[0].valid;
        bool flat = !wasVR;
        if (stereoReady || headReady || flat) {
            int slot = xrb::ClientAcquireSlot();
            if (slot < 0 && g_fakeHmd && !host) { if (g_dumpSentPending) slot = 0; else slot = -2; }   // debug: compose without a host
            if (slot >= 0 && g_pending[slot].used) { xrb::ClientCancelSlot(slot); slot = -1; }
            if (slot == -2) {}
            else if (slot < 0) g_busy++;
            else {
                Pending& p = g_pending[slot];
                uint32_t flags = 0;
                if (SUCCEEDED(d->BeginScene())) {
                    BeginOwn(d, g_slots[slot].tex);
                    if (flat) {
                        Quad(d, g_eyeTex[2], 0, (float)g_eh, (float)g_hw, (float)g_hh, QUAD_OPAQUE);
                        if (HudHasContent()) Quad(d, g_hudTex, 0, (float)g_eh, (float)g_hw, (float)g_hh, QUAD_PREMUL_FADE);   // its UI went to the HUD target
                        if (menu) MenuQuad(d, menu, ConfigFloat("menu_width", 0.6f));
                        flags = xrb::SLOT_MENU_MODE | xrb::SLOT_HUD_VISIBLE;
                        p.pose[0] = p.pose[1] = xrb::IpcPose{ 0, 0, 0, 0, 0, 0, 1 };
                        p.fov[0] = p.fov[1] = flatFov;
                        p.time = ptime;
                    } else {
                        IDirect3DTexture9* l = g_eyeTex[0], *r = stereoReady ? g_eyeTex[1] : g_eyeTex[0];
                        xrb::IpcFov cf0 = g_eyeRec[0].fov, cf1 = stereoReady ? g_eyeRec[1].fov : g_eyeRec[0].fov;
                        UpdateZoom(cf0, cf1);
                        cf0 = ZoomFov(cf0, g_zoom); cf1 = ZoomFov(cf1, g_zoom);
                        xrb::IpcFov of0 = OutFov(cf0, g_edgeFill && g_hfovValid ? &g_hfov[0] : nullptr);
                        xrb::IpcFov of1 = OutFov(cf1, g_edgeFill && g_hfovValid ? &g_hfov[1] : nullptr);
                        EyeQuad(d, l, 0, cf0, of0);
                        EyeQuad(d, r, (float)g_ew, cf1, of1);
                        p.pose[0] = g_eyeRec[0].pose; p.fov[0] = of0;
                        p.pose[1] = stereoReady ? g_eyeRec[1].pose : g_eyeRec[0].pose;
                        p.fov[1] = of1;
                        p.time = stereoReady ? (g_eyeRec[0].time > g_eyeRec[1].time ? g_eyeRec[0].time : g_eyeRec[1].time) : g_eyeRec[0].time;
                        bool hudPanel = HudHasContent();
                        if (menu || hudPanel) {
                            D3DRECT rc = { 0, (LONG)g_eh, (LONG)g_hw, (LONG)(g_eh + g_hh) };
                            d->Clear(1, &rc, D3DCLEAR_TARGET, 0, 1.f, 0);
                            if (hudPanel) Quad(d, g_hudTex, 0, (float)g_eh, (float)g_hw, (float)g_hh, QUAD_PREMUL_FADE);
                            if (menu) MenuQuad(d, menu, ConfigFloat("menu_width", 0.6f));
                            flags = xrb::SLOT_HUD_VISIBLE;
                        }
                    }
                    EndOwn(d);
                    d->EndScene();
                    if (g_dumpSentPending) {
                        g_dumpSentPending = false;
                        IDirect3DSurface9* as = nullptr; g_slots[slot].tex->GetSurfaceLevel(0, &as);
                        wchar_t n[64]; swprintf(n, 64, L"dump%u_%ls_sent_to_headset.bmp", g_dumpId, flat ? L"flat" : L"3d");
                        DumpSurface(d, as, n);
                        if (as) as->Release();
                        xrb::ClientRequestDump(g_dumpId);
                        LOGI("diag: asked the host to save what it receives (" GAME_SHORT "\\host_%u_*.bmp, see xrhost.log)", g_dumpId);
                    }
                    if (g_slots[slot].query) g_slots[slot].query->Issue(D3DISSUE_END);
                    if (host) { p.flags = flags; p.used = true; p.tick = GetTickCount(); }
                } else xrb::ClientCancelSlot(slot);
            }
        }
        Sec(4);
        if (g_shotWanted) {
            g_shotWanted = false;
            if (wasVR && g_eyeRec[0].valid) ScreenshotSave(d, g_eyeTex[0], g_vr.mode == VRMODE_STEREO && g_eyeRec[1].valid ? g_eyeTex[1] : nullptr, g_shotSbs);
            else ScreenshotSave(d, g_eyeTex[2], nullptr, false);
            if (HudHasContent()) ScreenshotSaveHud(d, g_hudTex);
        }
        // monitor: keep showing the right eye instead of flickering between the eyes
        Sec(5);
        if (g_mirror && wasVR && eye == 0 && g_eyeRec[1].valid && g_bbMs == D3DMULTISAMPLE_NONE) {
            IDirect3DSurface9* src = nullptr; g_eyeTex[1]->GetSurfaceLevel(0, &src);
            d->StretchRect(src, nullptr, bb, nullptr, D3DTEXF_NONE);
            if (src) src->Release();
        }
    }
    if (bb) bb->Release();

    if (g_hudSurf) d->ColorFill(g_hudSurf, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));   // the next frame's HUD starts transparent
    if (host && g_vr.hudMode == 0) UpdateHudPanelPlacement();
    // 3. statistics -> main camera; choose the next frame's eye and pose
    Sec(6);
    StereoEndFrame();
    Sec(7);
    xrb::IpcPose poses[2]; xrb::IpcFov hfov[2]; int64_t t = 0;
    bool havePose = host && xrb::ClientGetLatestPoses(poses, hfov, &t);
    Sec(8);
    if (!havePose && g_fakeHmd) {   // debug: synthetic headset (eyes +-32 mm, head turned by debug_fake_head_yaw)
        float a = g_fakeYaw * 0.0174533f, s = sinf(a * 0.5f), co = cosf(a * 0.5f);
        for (int e = 0; e < 2; e++) {
            float x = e ? 0.032f : -0.032f;
            poses[e] = xrb::IpcPose{ x * cosf(a), 0, -x * sinf(a), 0, s, 0, co };
            hfov[e] = xrb::IpcFov{ -0.8f, 0.8f, 0.8f, -0.8f };
        }
        t = 0; havePose = true;
    }
    if (havePose) { g_hfov[0] = hfov[0]; g_hfov[1] = hfov[1]; g_hfovValid = true; }
    W2CamOnPresent(poses, hfov, havePose);
    // a game menu (pause / inventory / map ...) shows the flat screen: the frame is the game's normal picture then
    g_vr.active = havePose && g_vr.mode != VRMODE_SCREEN && !W2GameMenuOpen() && (StereoHaveMain() || W2CamMainLive());
    SameTickNextFrame(false, 0);   // never leave the game clock held (no pose, flat mode ...); set again below
    if (havePose) {
        if (g_vr.mode == VRMODE_STEREO) {
            g_vr.eye = eye == 0 ? 1 : 0;
            g_vr.pose = poses[g_vr.eye];
        } else {
            g_vr.eye = 2;
            g_vr.pose = poses[0];
            g_vr.pose.px = (poses[0].px + poses[1].px) * 0.5f;
            g_vr.pose.py = (poses[0].py + poses[1].py) * 0.5f;
            g_vr.pose.pz = (poses[0].pz + poses[1].pz) * 0.5f;
        }
        g_vr.poseTime = t;
        SameTickNextFrame(g_vr.mode == VRMODE_STEREO && g_vr.active, g_vr.eye);
        if (W2CamActive()) g_vr.outProj = g_vr.mainProj;   // the engine renders the headset view itself
        else if (g_vr.mainProj.valid()) {
            // wider view, then the shape of the 3D picture (square pixels: a 1:1 picture gets the extra height)
            const float* c = g_vr.content;
            float aspect = (g_w && g_h) ? ((c[2] - c[0]) * g_w) / ((c[3] - c[1]) * g_h) : 0.f;
            if (g_efOk && g_engineFovFixed < 0.99f && g_efCenterT > 0.05f && aspect > 0.05f) {   // (fov_scale / aspect_fit do not apply then)
                // the engine renders (and culls) a view wider than the headset's so the head can turn inside it; the
                // picture itself is the headset's view (full resolution where the eyes are), cut from inside it
                GameProj o = g_vr.mainProj;
                float ty = g_efCenterT, tx = ty * aspect;
                float iy = 1.f / g_vr.mainProj.p11, ix = 1.f / g_vr.mainProj.p00;
                o.p11 = 1.f / fminf(ty, iy); o.p00 = 1.f / fminf(tx, ix);
                g_vr.outProj = MatchAspect(o, aspect, true);
            } else
                g_vr.outProj = MatchAspect(WidenProj(g_vr.mainProj, g_vr.fovScale, FovUnion(hfov)), aspect, g_aspectCrop && (g_vr.fovScale <= 1.001f || g_efOk));
        } else g_vr.outProj = g_vr.mainProj;
    }
    // HUD scale: auto = the ratio between the game's view and the wider VR view
    if (g_vr.hudMode == 2) g_vr.hudScaleEff = 1.f;
    else if (g_vr.hudScaleAuto) {
        // ratio of the headset view to the game's own view: the HUD covers what the flat game showed
        if (W2CamActive()) g_vr.hudScaleEff = W2CamHudScale();
        else g_vr.hudScaleEff = (g_vr.mainProj.valid() && g_vr.outProj.valid())
            ? fminf(1.f, fminf(g_vr.outProj.p00 / g_vr.mainProj.p00, g_vr.outProj.p11 / g_vr.mainProj.p11)) : 1.f;
    }
    else g_vr.hudScaleEff = g_vr.hudScale;
    if (g_vr.hudScaleEff > 0.999f && g_vr.hudScaleEff < 1.001f) g_vr.hudScaleEff = 1.f;
    StereoPoseChanged();
    Sec(9);
    if (now - g_reportTick >= 10000) { g_reportTick = now; RecSizeLater(); SaveSymmetricRatio(); XrReport(); W2CamReport(); }
}

void XrReport() {
    LOGI("xr: host %s%s, frames %u (3D %u, flat %u), submitted %u, ring busy %u, gpu late %u, no host %u | mode %s, main camera %s",
         xrb::ClientHostConnected() ? "connected" : "not connected", xrb::ClientHostConnected() ? (xrb::ClientHostVisible() ? " (headset showing the app)" : " (headset NOT showing the app)") : "", g_frame, g_vrFrames, g_flatFrames, g_submitted, g_busy, g_gpuLate, g_noHost,
         ModeName(g_vr.mode), StereoHaveMain() ? "yes" : "no");
    {   // once in gameplay (VR active for two reports): which targets the frame's draws go to (HUD diagnosis)
        static int liveReports = 0;
        if (g_vr.active && W2CamMainLive()) { if (++liveReports == 2) { StereoLogTargetsNextFrame(); static const bool sd = ConfigBool("diag_stage_dump", false); if (sd) D3D9RequestStageDump(); }
            // second recording (run 2), ten seconds later, with the game's own texture filtering: shows whether the mod's
            // filtering has any part in the missing normals
            static const bool sd2 = ConfigBool("diag_stage_dump", false);
            extern void D3D9TextureFilteringTest(bool);
            if (sd2 && liveReports == 3) { D3D9TextureFilteringTest(true); }
            if (sd2 && liveReports == 4) { D3D9RequestStageDump(); }
            if (sd2 && liveReports == 5) { D3D9TextureFilteringTest(false); }
        } else liveReports = 0;
        if (g_vr.active) W2WindowToFront();
    }
    if (g_ownN) { unsigned dd = DeDitherPasses();
                  LOGI("xr: %.1f game fps, %.0f draws per frame, mod work at Present %.2f ms avg (max %.2f), de-dither %u eye pictures",
                     g_ownN / 10.f, (double)g_drawsSum / g_ownN, g_ownMs / g_ownN, g_ownMaxMs, dd); }
    g_ownMs = g_ownMaxMs = 0; g_ownN = g_drawsSum = 0;
    SameTickReport();
    { char b[256]; vrkit::input::Report(b, sizeof b); LOGI("%s", b); }
    W2InputReport();
    W2EngineDitherReport();
    {   // what sets the frame rate: the cap, or the GPU / game (the limiter then never waits)
        int64_t ns = xrb::ClientDisplayPeriodNs();
        double hz = FrameLimitHz();
        if (hz > 0)
            LOGI("frame limit: %s = %.0f fps (headset %s), waited in %u of %u frames, %.2f ms avg%s", g_limitMode.c_str(), hz,
                 ns > 0 ? (std::to_string((int)(1e9 / ns + 0.5)) + " Hz").c_str() : "refresh not reported, 90 Hz assumed", g_limWaited, g_limN,
                 g_limWaited ? g_limWaitMs / g_limWaited : 0.0, g_limN && g_limWaited * 10 < g_limN ? " - the GPU / game is the limit, not the cap" : "");
        else LOGI("frame limit: off (headset %s)", ns > 0 ? (std::to_string((int)(1e9 / ns + 0.5)) + " Hz").c_str() : "refresh not reported");
        g_limN = g_limWaited = 0; g_limWaitMs = 0;
    }
    if (*xrb::ClientHostError()) LOGW("xr: host error: %s", xrb::ClientHostError());
    D3D9Report();
    g_vrFrames = g_flatFrames = g_submitted = g_busy = g_gpuLate = g_noHost = 0;
}
