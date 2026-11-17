// D3D9 layer.
//  * Direct3DCreate9 -> Direct3DCreate9Ex and CreateDevice -> CreateDeviceEx, so the mod can create textures shared
//    with the 64-bit XrBridgeHost. D3D9Ex does not allow D3DPOOL_MANAGED: those resources are created in
//    D3DPOOL_DEFAULT (textures as D3DUSAGE_DYNAMIC so the game can still lock them).
//  * Every vertex shader is patched at creation (vspatch.cpp); before each draw with a patched shader the mod uploads
//    the clip-space eye matrix to c252..c255 - enabled only for draws of the game's main camera (the perspective
//    matrix the shader reads matches the main projection, or the draw goes to the main scene render target).
//  * Present: hands the frame to xr.cpp.
#include <windows.h>
#include <d3d9.h>
#include <unordered_map>
#include <intrin.h>
#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>
#include "MinHook.h"
#include "log.h"
#include "config.h"
#include "w2vr.h"
#include "vspatch.h"
#include <string>
#include <cstdio>
#include <cstdlib>

IDirect3DDevice9* g_dev = nullptr;
volatile LONG g_inOwn = 0;
VrState g_vr;

// ---------------------------------------------------------------- config read once at startup
bool g_vanilla = false;
static bool g_cfgWindowed = true, g_cfgImmediate = true, g_cfgPatch = true, g_cfgEx = true, g_cfgNoDof = false, g_cfgForceSize = false;
static volatile LONG g_dofReplaced = 0;

// ---------------------------------------------------------------- trampolines
typedef IDirect3D9* (WINAPI* Create9Fn)(UINT);
typedef HRESULT (WINAPI* Create9ExFn)(UINT, IDirect3D9Ex**);
typedef HRESULT (STDMETHODCALLTYPE* CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE* ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT (STDMETHODCALLTYPE* SCPresentFn)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* CreateTextureFn)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateVolTexFn)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateCubeTexFn)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateVBFn)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateIBFn)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
typedef HRESULT (STDMETHODCALLTYPE* SetRTFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (STDMETHODCALLTYPE* DrawPrimFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* DrawIdxPrimFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* DrawPrimUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* DrawIdxPrimUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* CreateVSFn)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
typedef HRESULT(STDMETHODCALLTYPE* CreatePSFn)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
typedef HRESULT (STDMETHODCALLTYPE* SetVSFn)(IDirect3DDevice9*, IDirect3DVertexShader9*);
typedef HRESULT (STDMETHODCALLTYPE* SetVSConstFFn)(IDirect3DDevice9*, UINT, const float*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* SetRSFn)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* SetScissorFn)(IDirect3DDevice9*, const RECT*);

static Create9Fn oCreate9;
static Create9ExFn pCreate9Ex;
static CreateDeviceFn oCreateDevice;
static ResetFn oReset;
static PresentFn oPresent;
static SCPresentFn oSCPresent;
static CreateTextureFn oCreateTexture;
static CreateVolTexFn oCreateVolTex;
static CreateCubeTexFn oCreateCubeTex;
static CreateVBFn oCreateVB;
static CreateIBFn oCreateIB;
static SetRTFn oSetRT;
static DrawPrimFn oDrawPrim;
static DrawIdxPrimFn oDrawIdxPrim;
static DrawPrimUPFn oDrawPrimUP;
static DrawIdxPrimUPFn oDrawIdxPrimUP;
static CreateVSFn oCreateVS;
static CreatePSFn oCreatePS;
static SetVSFn oSetVS;
static SetVSConstFFn oSetVSConstF;
static SetRSFn oSetRS;
typedef HRESULT (STDMETHODCALLTYPE* SetViewportFn)(IDirect3DDevice9*, const D3DVIEWPORT9*);
static SetViewportFn oSetViewport;
static SetScissorFn oSetScissor;

HRESULT CallSetVSConstF(IDirect3DDevice9* d, UINT reg, const float* data, UINT count) {
    return oSetVSConstF ? oSetVSConstF(d, reg, data, count) : d->SetVertexShaderConstantF(reg, data, count);
}
HRESULT CallCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                          IDirect3DTexture9** tex, HANDLE* shared) {
    return oCreateTexture ? oCreateTexture(d, w, h, levels, usage, fmt, pool, tex, shared) : d->CreateTexture(w, h, levels, usage, fmt, pool, tex, shared);
}

static void** VT(void* obj) { return *(void***)obj; }
template <class F> static bool Hook(void* target, void* detour, F* orig, const char* name) {
    if (*orig) return true;
    MH_STATUS s = MH_CreateHook(target, detour, (void**)orig);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { LOGE("d3d9: hook %s failed (%d)", name, (int)s); return false; }
    MH_EnableHook(target);
    return true;
}

// ---------------------------------------------------------------- shader registry
struct ShaderInfo { VsPatchInfo info; int projReg = -1; unsigned noProjScans = 0, skipped = 0; };
static CRITICAL_SECTION g_shaderCs;
static std::unordered_map<IDirect3DVertexShader9*, std::unique_ptr<ShaderInfo>> g_shaders;
static ShaderInfo* g_curShader = nullptr;

// ---------------------------------------------------------------- vertex constant mirror
static float g_c[256][4];
static uint32_t g_constGen = 1;
static bool g_gameUsesReserved = false;

// ---------------------------------------------------------------- render target tracking
static UINT g_rtW = 0, g_rtH = 0, g_bbW = 0, g_bbH = 0;   // render target 0
static UINT g_vpW = 0, g_vpH = 0, g_vpX = 0, g_vpY = 0;    // viewport: the area the draws render to (the size key)
static D3DFORMAT g_rtFmt = D3DFMT_UNKNOWN;
static IDirect3DSurface9* g_bbSurf = nullptr;              // the swap chain's back buffer (no reference held)
static bool g_rtIsBB = true;
// The game draws gameplay frames (scene + HUD) into an intermediate target and copies it to the back buffer at the end
// (StretchRect, or a full-screen draw sampling it); menus / loading screens draw into the back buffer directly. The
// HUD panel takes 2D draws on either: on the intermediate ("final") target only after the scene has been composited
// into it this frame (a draw sampling a screen-size target, or a copy into it), so post-processing never counts as HUD.
static IDirect3DSurface9* g_curRT0 = nullptr;           // current render target 0 (no reference)
static IDirect3DSurface9* g_finalSrc = nullptr;         // last frame's source of the copy into the back buffer (compared only)
static IDirect3DSurface9* g_finalSrcNow = nullptr;      // this frame's
static bool g_rtIsFinal = false, g_finalComposited = false;
static IDirect3DSurface9* g_redirFrom = nullptr;        // the target a redirected HUD draw was meant for
static unsigned g_hudOnBB = 0, g_hudOnFinal = 0, g_finalFrames = 0, g_hudKeptOut = 0;
static IDirect3DBaseTexture9* g_stageTex[8] = {};          // textures bound to samplers 0..7 (no references)
static D3DMULTISAMPLE_TYPE g_bbMsType = D3DMULTISAMPLE_NONE;
// mirrors of the few states the HUD panel redirect changes (Get* fails on a pure device)
static D3DVIEWPORT9 g_vpFull = {}; static RECT g_scissor = {}; static bool g_scissorSet = false;
static DWORD g_rsAB = FALSE, g_rsSepAB = FALSE, g_rsSrcA = D3DBLEND_ONE, g_rsDstA = D3DBLEND_ZERO, g_rsOpA = D3DBLENDOP_ADD;
static bool g_redir = false; static unsigned g_redirDraws = 0, g_redirFrames = 0;
static void NoteBackBufferSurface(IDirect3DDevice9* d) {
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) { g_bbSurf = s; s->Release(); }
}
// per-frame breakdown of patched draws by target (logged with the periodic report)
struct TargetStat { UINT vpW, vpH, rtW, rtH; D3DFORMAT fmt; unsigned draws, proj, xf, hud; };
static TargetStat g_tstat[8]; static int g_tstatN = 0; static bool g_tstatLog = false, g_tstatArm = false;
static TargetStat* TStat() {
    for (int i = 0; i < g_tstatN; i++) { TargetStat& t = g_tstat[i]; if (t.vpW == g_vpW && t.vpH == g_vpH && t.rtW == g_rtW && t.rtH == g_rtH && t.fmt == g_rtFmt) return &t; }
    if (g_tstatN < 8) { g_tstat[g_tstatN] = { g_vpW, g_vpH, g_rtW, g_rtH, g_rtFmt, 0, 0, 0, 0 }; return &g_tstat[g_tstatN++]; }
    return nullptr;
}
void StereoLogTargetsNextFrame() { g_tstatArm = true; }

// ---------------------------------------------------------------- main camera statistics
struct ProjStat { GameProj p; UINT w, h; unsigned n; UINT vx, vy, rw, rh; };
static std::vector<ProjStat> g_stats;
static bool g_haveMain = false;
static std::vector<float> g_pitchZ, g_pitchY;   // camera pitch seen through main-camera draws (Z-up / Y-up world)
static int g_upAxis = 0;                        // 0 unknown, 1 Z-up, 2 Y-up
static int g_missFrames = 0;

static void StatAdd(const GameProj& p, UINT w, UINT h) {
    for (auto& s : g_stats) if (s.w == w && s.h == h && ProjClose(p, s.p, 0.005f)) { s.n++; return; }
    if (g_stats.size() < 64) g_stats.push_back({ p, w, h, 1, g_vpX, g_vpY, g_rtW, g_rtH });
}

void StereoEndFrame() {
    const ProjStat* best = nullptr;
    for (auto& s : g_stats) if (!best || s.n > best->n) best = &s;
    if (best && best->n >= 3) {
        if (!g_haveMain || !ProjClose(best->p, g_vr.mainProj, 0.002f) || best->w != g_vr.mainW || best->h != g_vr.mainH) {
            static DWORD lastLog = 0;
            if (!g_haveMain || GetTickCount() - lastLog > 2000) {
                lastLog = GetTickCount();
                LOGI("stereo: main camera p00 %.4f p11 %.4f (vfov %.1f deg) off %.3f %.3f, viewport %ux%u (%u draws)",
                     best->p.p00, best->p.p11, 2 * atanf(1.f / best->p.p11) * 57.2958f, best->p.p02, best->p.p12, best->w, best->h, best->n);
            }
        }
        g_vr.mainProj = best->p; g_vr.mainW = best->w; g_vr.mainH = best->h;
        g_vr.mainVpX = best->vx; g_vr.mainVpY = best->vy; g_vr.mainRtW = best->rw; g_vr.mainRtH = best->rh;
        // where the 3D picture ends up in the back buffer: the viewport on a back-buffer-shaped target, otherwise the
        // target fitted into the back buffer with its aspect kept (the game composites a 16:9 scene letterboxed)
        float c0 = 0, c1 = 0, c2 = 1, c3 = 1;
        if (g_bbW && g_bbH && best->rw && best->rh) {
            float bbA = (float)g_bbW / g_bbH, rtA = (float)best->rw / best->rh;
            if (fabsf(bbA / rtA - 1.f) < 0.01f) {
                c0 = (float)best->vx / best->rw; c1 = (float)best->vy / best->rh;
                c2 = (float)(best->vx + best->w) / best->rw; c3 = (float)(best->vy + best->h) / best->rh;
            } else {
                float vpA = (float)best->w / best->h;
                if (vpA > bbA) { float hh = bbA / vpA; c1 = (1 - hh) * 0.5f; c3 = 1 - c1; }
                else { float ww = vpA / bbA; c0 = (1 - ww) * 0.5f; c2 = 1 - c0; }
            }
        }
        if (fabsf(c0 - g_vr.content[0]) + fabsf(c1 - g_vr.content[1]) + fabsf(c2 - g_vr.content[2]) + fabsf(c3 - g_vr.content[3]) > 0.002f) {
            LOGI("stereo: 3D picture covers x %.3f..%.3f, y %.3f..%.3f of the %ux%u frame (viewport %ux%u at %u,%u on %ux%u)%s", c0, c2, c1, c3, g_bbW, g_bbH,
                 best->w, best->h, best->vx, best->vy, best->rw, best->rh, (c0 > 0.001f || c1 > 0.001f) ? " - the game letterboxes / pillarboxes" : "");
            g_vr.content[0] = c0; g_vr.content[1] = c1; g_vr.content[2] = c2; g_vr.content[3] = c3;
        }
        g_haveMain = true; g_missFrames = 0;
    } else if (g_haveMain && ++g_missFrames > 3) {
        g_haveMain = false;
        LOGI("stereo: no main camera (menu / loading / video)");
    }
    if (g_tstatLog && g_tstatN) {
        for (int i = 0; i < g_tstatN; i++) {
            const TargetStat& t = g_tstat[i];
            LOGI("stereo:   target viewport %ux%u on RT %ux%u fmt %d: %u patched draws, %u with a camera matrix, %u transformed, %u HUD",
                 t.vpW, t.vpH, t.rtW, t.rtH, (int)t.fmt, t.draws, t.proj, t.xf, t.hud);
        }
        g_tstatLog = false; g_tstatN = 0;
    } else if (g_tstatLog) { g_tstatLog = false; LOGI("stereo:   no patched draws this frame"); }
    if (g_tstatArm) { g_tstatArm = false; g_tstatLog = true; g_tstatN = 0; LOGI("stereo: draw targets of the next frame:"); }
    // camera pitch for decoupled pitch: median over this frame's main-camera draws; the world's up axis is the one
    // that gives plausible (upright) cameras for most draws
    if (g_pitchZ.size() + g_pitchY.size() >= 3) {
        if (!g_upAxis) {
            static int votesZ = 0, votesY = 0;
            if (g_pitchZ.size() > 2 * g_pitchY.size()) votesZ++; else if (g_pitchY.size() > 2 * g_pitchZ.size()) votesY++;
            if (votesZ >= 10) { g_upAxis = 1; LOGI("stereo: world is Z-up"); }
            else if (votesY >= 10) { g_upAxis = 2; LOGI("stereo: world is Y-up"); }
        }
        std::vector<float>& v = g_upAxis == 2 ? g_pitchY : g_pitchZ;
        if (g_upAxis && v.size() >= 3) {
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            g_vr.camPitch = v[v.size() / 2]; g_vr.camPitchValid = true;
        }
    } else g_vr.camPitchValid = false;
    g_pitchZ.clear(); g_pitchY.clear();
    g_stats.clear();
    g_vr.drawsXf = g_vr.drawsPatched = g_vr.drawsTotal = g_vr.drawsMain = 0;
}
bool StereoHaveMain() { return g_haveMain; }

// ---------------------------------------------------------------- per-draw eye transform
static uint32_t g_frameSerial = 1;     // bumped when the eye / pose changes (xr.cpp)
void StereoPoseChanged() { g_frameSerial++; }

// (fitted / full HUD modes only) AC2 lays its HUD out for a 16:9 area taken from the frame height; in a narrower frame without letterbox bars it
// sits to the right. hud_aspect_fix moves / shrinks it by how much narrower the frame is than 16:9 (calibrated at 5:4:
// size 0.91, x -0.26, y -0.05; 16:9: nothing). hud_size / hud_offset_x / _y fine-tune on top.
static bool g_cfgHudFix = false, g_cfgNoBars = true;
static bool g_cfgEngineCamStart = true;
static bool g_cfgAnalyseOnly = false;     // patch_shaders = auto with the engine camera and the HUD panel: shaders unchanged   // camera_mode = engine at start-up: relative-addressing shaders are not patched
void D3D9SetHudAspectFix(bool on) { g_cfgHudFix = on; }
static void HudAspectFix(float* s, float* x, float* y) {
    if (!g_cfgHudFix || !g_cfgNoBars || !g_bbW || !g_bbH) return;
    float r = (16.f / 9.f) / ((float)g_bbW / (float)g_bbH) - 1.f;   // 0 at 16:9, 0.42 at 5:4, 0.78 at 1:1
    if (r <= 0.f) return;
    *s = 1.f / (1.f + 0.234f * r); *x = -0.616f * r; *y = -0.118f * r;
}
// ---------------------------------------------------------------- shadow maps: depth bias
// The sun's cascades are three 1024 x 1024 maps in one depth-only 1024 x 3072 target (colour format FOURCC 'NULL');
// spot / cube shadows are depth-only square targets too. A headset's view is far wider than the flat game's (~104
// degrees vertically against ~60), so each cascade spreads its texels over much more ground and the game's own depth
// bias no longer covers them: lit surfaces shadow themselves at random (shadow acne - grainy dark specks in sunlight).
// The classic fix, applied only while drawing INTO those shadow maps (never to the main view's depth): extra
// slope-scaled and constant depth bias on top of the game's own (D3DRS_SLOPESCALEDEPTHBIAS / D3DRS_DEPTHBIAS). Live.
static bool g_rtIsShadow = false;
static float g_shSlope = 0.f, g_shConst = 0.f;   // added bias (shadow_slope_bias / shadow_depth_bias)
static bool g_shOff = false;                     // diagnostic: skip shadow-map draws (no shadows at all)
static DWORD g_rsSlope = 0, g_rsDepthBias = 0;   // the game's own values (float bits)
static bool g_biasSet = false, g_skipDraw = false;
static unsigned g_shadowDraws = 0;
void D3D9SetShadowBias(float slope, float constant, bool shadowsOff) { g_shSlope = slope; g_shConst = constant; g_shOff = shadowsOff; }
static inline float AsF(DWORD v) { float f; memcpy(&f, &v, 4); return f; }
static inline DWORD AsD(float f) { DWORD v; memcpy(&v, &f, 4); return v; }

static void EnforceSamplers(IDirect3DDevice9* d);
// ---------------------------------------------------------------- stage dump (diagnostics, 0.7.0)
// One frame, stage by stage: every time the game moves on from a full-size render target it has drawn into, a
// 1600 x 1200 crop of each bound target (MRT 0..3) is saved to W2VR\stages\ (colour | alpha). The files show at which
// stage of the frame (G-buffer colour, normals, lighting, post-processing) a wrong pixel first appears.
bool StageDumpSurface(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& rc, const std::wstring& path);
static int g_sdState = 0;   // 0 idle, 1 armed (starts at the next Present), 2 recording this frame
static unsigned g_sdSeq = 0, g_sdDraws = 0, g_sdRun = 0, g_sdSaved = 0;
void D3D9RequestStageDump() { if (!g_sdState) g_sdState = 1; }
static void StageDumpBound(IDirect3DDevice9* d) {
    InterlockedIncrement(&g_inOwn);
    for (DWORD i = 0; i < 4 && g_sdSeq < 60; i++) {
        IDirect3DSurface9* s = nullptr;
        if (FAILED(d->GetRenderTarget(i, &s)) || !s) continue;
        D3DSURFACE_DESC sd;
        if (SUCCEEDED(s->GetDesc(&sd)) && sd.Width == g_bbW && sd.Height == g_bbH && (sd.Usage & D3DUSAGE_RENDERTARGET)) {
            int cw = sd.Width < 1600 ? (int)sd.Width : 1600, ch = sd.Height < 1200 ? (int)sd.Height : 1200;
            RECT rc; rc.left = ((int)sd.Width - cw) / 2; rc.top = ((int)sd.Height - ch) * 3 / 4; rc.right = rc.left + cw; rc.bottom = rc.top + ch;
            wchar_t n[128]; swprintf(n, 128, L"\\stages\\run%u_%02u_rt%lu_fmt%u_after%udraws.png", g_sdRun, g_sdSeq, (unsigned long)i, (unsigned)sd.Format, g_sdDraws);
            bool ok = StageDumpSurface(d, s, rc, g_modDir + n);
            if (ok) g_sdSaved++;
            LOGI("stages: %02u target %lu fmt %u after %u draws%s", g_sdSeq, (unsigned long)i, (unsigned)sd.Format, g_sdDraws, ok ? "" : " (not saved: format / read-back)");
            g_sdSeq++;
        }
        s->Release();
    }
    InterlockedDecrement(&g_inOwn);
}


// ---------------------------------------------------------------- G-buffer texture census (diagnostics, 0.9.4)
// During a stage-dump frame: which textures the draws into the G-buffer (3 render targets bound) read, by sampler -
// format, size, levels and how the mod holds them. Shows what the normal / height maps are.
struct CenKey { DWORD stage; D3DFORMAT fmt; UINT w, h, lv; int kind; bool operator==(const CenKey& o) const { return !memcmp(this, &o, sizeof o); } };
static CenKey g_cen[96]; static unsigned g_cenN[96], g_cenUsed = 0;
static IDirect3DBaseTexture9* g_cenTex[16] = {};
static void GbufCensusNote(DWORD stage, IDirect3DBaseTexture9* t) { g_cenTex[stage] = t; }
static IDirect3DTexture9* MtSys(IDirect3DTexture9* t, bool markDirty);
static void MtUpload(IDirect3DDevice9* d, IDirect3DTexture9* t);
extern volatile LONG g_mtAnyDirty;
static void GbufCensusDraw(IDirect3DDevice9* d) {
    for (DWORD st = 0; st < 16; st++) {
        IDirect3DBaseTexture9* t = g_cenTex[st]; if (!t) continue;
        CenKey k = {}; k.stage = st; k.lv = t->GetLevelCount();
        if (t->GetType() == D3DRTYPE_TEXTURE) { D3DSURFACE_DESC sd; if (FAILED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &sd))) continue; k.fmt = sd.Format; k.w = sd.Width; k.h = sd.Height;
            k.kind = (sd.Usage & D3DUSAGE_RENDERTARGET) ? 3 : MtSys((IDirect3DTexture9*)t, false) ? 2 : (sd.Usage & D3DUSAGE_DYNAMIC) ? 1 : 0; }
        else if (t->GetType() == D3DRTYPE_CUBETEXTURE) { D3DSURFACE_DESC sd; if (FAILED(((IDirect3DCubeTexture9*)t)->GetLevelDesc(0, &sd))) continue; k.fmt = sd.Format; k.w = sd.Width; k.h = 6; k.kind = 4; }
        else { k.kind = 5; }
        unsigned i = 0; for (; i < g_cenUsed; i++) if (g_cen[i] == k) break;
        if (i == g_cenUsed) { if (g_cenUsed >= 96) continue; g_cen[g_cenUsed++] = k; g_cenN[i] = 0; }
        g_cenN[i]++;
    }
}
static void GbufCensusReport() {
    if (!g_cenUsed) return;
    static const char* kind[] = { "video memory", "dynamic (mod)", "managed copy (mod)", "render target", "cube", "volume" };
    LOGI("gbuffer: textures read by the G-buffer draws (sampler: format size levels how - draws):");
    for (unsigned i = 0; i < g_cenUsed; i++) {
        const CenKey& k = g_cen[i]; char f[8]; DWORD v = (DWORD)k.fmt;
        if (v > 0xFFFF) { memcpy(f, &v, 4); f[4] = 0; } else snprintf(f, sizeof f, "%lu", (unsigned long)v);
        LOGI("gbuffer:   s%lu: %s %ux%u x%u %s - %u", (unsigned long)k.stage, f, k.w, k.h, k.lv, kind[k.kind], g_cenN[i]);
    }
    g_cenUsed = 0;
}
void NormalRepair(IDirect3DDevice9* d, IDirect3DSurface9* normals);
unsigned NormalRepairPasses();
static unsigned g_rtDraws = 0; static bool g_nrOn = true, g_nrDone = false;
static int g_tstView; static bool g_tstDecalsOff, g_rt2Bound; static void TestGrabLayer(IDirect3DDevice9* d); static void TestShowLayer(IDirect3DDevice9* d);

// ---- 1.0.1: GPU time per game frame (timestamp queries from the frame's first draw to its Present), read 4 frames later
// without waiting. Tells whether a slow-down is the GPU (GPU ms ~ frame ms) or the game's CPU side.
static IDirect3DQuery9* g_gtQ[8][2] = {}; static bool g_gtBegun[8] = {}; static unsigned g_gtIdx = 0; static UINT64 g_gtFreq = 0;
static double g_gtSum = 0, g_gtMax = 0; static unsigned g_gtN = 0; static bool g_gtOff = false, g_gtFrameOpen = false;
static void GpuTimeBegin(IDirect3DDevice9* d) {
    if (g_gtOff || g_gtFrameOpen) return;
    unsigned i = g_gtIdx & 7;
    if (!g_gtQ[i][0]) {
        if (!g_gtFreq) { IDirect3DQuery9* f = nullptr; if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f)) || !f) { g_gtOff = true; return; }
                         f->Issue(D3DISSUE_END); for (int k = 0; k < 2000 && f->GetData(&g_gtFreq, sizeof g_gtFreq, D3DGETDATA_FLUSH) == S_FALSE; k++) Sleep(0); f->Release();
                         if (!g_gtFreq) { g_gtOff = true; return; } }
        if (FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_gtQ[i][0])) || FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_gtQ[i][1]))) { g_gtOff = true; return; }
    }
    g_gtQ[i][0]->Issue(D3DISSUE_END); g_gtFrameOpen = true;
}
static void GpuTimeEnd() {
    if (g_gtOff || !g_gtFrameOpen) return;
    unsigned i = g_gtIdx & 7;
    g_gtQ[i][1]->Issue(D3DISSUE_END); g_gtBegun[i] = true; g_gtFrameOpen = false; g_gtIdx++;
    unsigned j = (g_gtIdx + 3) & 7;   // the pair issued 5 frames ago
    if (g_gtBegun[j]) {
        UINT64 a = 0, b = 0;
        if (g_gtQ[j][0]->GetData(&a, sizeof a, 0) == S_OK && g_gtQ[j][1]->GetData(&b, sizeof b, 0) == S_OK && b > a) {
            double ms = (double)(b - a) * 1000.0 / (double)g_gtFreq;
            if (ms < 1000) { g_gtSum += ms; g_gtN++; if (ms > g_gtMax) g_gtMax = ms; }
        }
        g_gtBegun[j] = false;
    }
}
void D3D9GpuTimeReport() {
    if (g_gtN) LOGI("gpu: %.2f ms per game frame on the graphics card (max %.2f; the frame's first draw to its Present)", g_gtSum / g_gtN, g_gtMax);
    else if (g_gtOff) LOGI("gpu: timestamp queries not available");
    g_gtSum = g_gtMax = 0; g_gtN = 0;
}

// ---- 1.0.4: where the CPU time of a frame goes (log line "cpu:"): the mod's work inside the game's draw / state calls,
// the real Present, the frame limiter's wait, and the rest (the game itself and the graphics driver)
static LONGLONG g_cpuQpf = 0; static double g_cpuHookMs = 0, g_cpuPresentMs = 0, g_cpuFrameMs = 0, g_cpuLimitMs = 0; static unsigned g_cpuFrames = 0;
static LONGLONG CpuNow() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart; }
static double CpuMs(LONGLONG a, LONGLONG b) { if (!g_cpuQpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_cpuQpf = f.QuadPart; } return (b - a) * 1000.0 / g_cpuQpf; }
// the mod's own work is timed with the CPU's cycle counter (a few ns per reading; QueryPerformanceCounter cost more than
// some of the hooks it measured), converted to ms with the counter's rate measured between reports
static unsigned long long g_hookTsc = 0;
struct HookTime { unsigned long long t0; HookTime() : t0(__rdtsc()) {} ~HookTime() { g_hookTsc += __rdtsc() - t0; } };
void D3D9CpuReport() {
    {   static unsigned long long lastTsc = 0; static LONGLONG lastQpc = 0;
        unsigned long long t = __rdtsc(); LONGLONG q = CpuNow();
        if (lastTsc && q > lastQpc) { double ms = CpuMs(lastQpc, q); if (ms > 0) g_cpuHookMs = g_hookTsc * ms / (double)(t - lastTsc); }
        lastTsc = t; lastQpc = q; g_hookTsc = 0; }
    if (g_cpuFrames) LOGI("cpu: %.2f ms per frame on the game's render thread: the mod's draw / state hooks %.2f, the real Present %.2f, the frame limit's wait %.2f, the game + driver %.2f",
                          g_cpuFrameMs / g_cpuFrames, g_cpuHookMs / g_cpuFrames, g_cpuPresentMs / g_cpuFrames, g_cpuLimitMs / g_cpuFrames,
                          (g_cpuFrameMs - g_cpuHookMs - g_cpuPresentMs - g_cpuLimitMs) / g_cpuFrames);
    g_cpuHookMs = g_cpuPresentMs = g_cpuFrameMs = g_cpuLimitMs = 0; g_cpuFrames = 0;
}
extern volatile LONG g_mbAnyDirty; static void MbUpload(IDirect3DDevice9* d);
static void PreDraw(IDirect3DDevice9* d) {
    HookTime ht_;
    if (!g_inOwn && !g_gtFrameOpen) GpuTimeBegin(d);
    if (g_inOwn) return;
    if (g_vanilla) { if (g_sdState == 2) g_sdDraws++; return; }
    if (g_mbAnyDirty) MbUpload(d);
    if (g_mtAnyDirty) { g_mtAnyDirty = 0; for (int st = 0; st < 8; st++) if (g_stageTex[st] && g_stageTex[st]->GetType() == D3DRTYPE_TEXTURE) MtUpload(d, (IDirect3DTexture9*)g_stageTex[st]); }
    if (g_sdState == 2) { IDirect3DSurface9* r2 = nullptr; if (SUCCEEDED(d->GetRenderTarget(2, &r2)) && r2) { GbufCensusDraw(d); r2->Release(); } }
    if (g_sdState == 2) g_sdDraws++;
    g_rtDraws++;
    EnforceSamplers(d);
    g_vr.drawsTotal++;
    if (g_tstDecalsOff && g_rt2Bound && g_rsAB) { g_skipDraw = true; return; }
    if (g_rtIsShadow) {
        g_shadowDraws++;
        if (g_shOff) { g_skipDraw = true; return; }
        if (g_shSlope > 0.f || g_shConst > 0.f) {
            oSetRS(d, D3DRS_SLOPESCALEDEPTHBIAS, AsD(AsF(g_rsSlope) + g_shSlope));
            oSetRS(d, D3DRS_DEPTHBIAS, AsD(AsF(g_rsDepthBias) + g_shConst));
            g_biasSet = true;
        }
    }
    ShaderInfo* s = g_curShader;
    if (!s || !s->info.analyzed) return;
    // a shader analysed but not patched (relative constant addressing in engine-camera mode) is still classified (main
    // 3D draw, eye, HUD redirect) - only the mod's constants / transform are left out
    const bool canWrite = s->info.patched;
    if (canWrite) g_vr.drawsPatched++;

    // perspective matrices this shader reads (cached per shader + constant state)
    static ShaderInfo* scanShader = nullptr; static uint32_t scanGen = 0;
    static int scanResult = 0, scanEye = -1; static GameProj scanProj, scanFirst; static bool scanFound = false;
    const bool w2cam = W2CamActive();
    if (scanShader != s || scanGen != g_constGen) {
        scanShader = s; scanGen = g_constGen; scanResult = 0; scanFound = false; scanEye = -1;
        const VsPatchInfo& vi = s->info;
        // 1.0.4: this scan ran for every draw (the game changes constants between draws) over every register the shader
        // reads - thousands of draws a frame on the game's render thread, more in combat. Now: the register where this
        // shader's projection was last found is tried first (the full scan only if that fails), and a shader that never
        // had one in 256 scans (2D / post-process) is re-scanned only every 64th time.
        int r0 = 0;
        if (s->projReg >= 0) {
            GameProj p;
            if (DetectProjection(&g_c[s->projReg][0], p)) r0 = s->projReg; else s->projReg = -1;
        }
        bool skipScan = false;
        if (s->projReg < 0 && s->noProjScans >= 256) { if ((++s->skipped & 63) != 0) skipScan = true; }
        if (!skipScan) for (int r = r0; r + 3 < kConstBase; r++) {
            if (!vi.reads(r) || !vi.reads(r + 1) || !vi.reads(r + 2) || !vi.reads(r + 3)) continue;
            GameProj p;
            if (!DetectProjection(&g_c[r][0], p)) continue;
            if (!scanFound) { scanFound = true; scanFirst = p; s->projReg = r; s->noProjScans = 0; }
            int ce = -1;
            if (w2cam && W2CamMatchDraw(p, &ce)) {   // one of the eye cameras the engine was given
                scanResult = 1; scanProj = p; scanEye = ce;
                break;
            }
            if (g_haveMain && ProjClose(p, g_vr.mainProj, 0.03f)) {
                scanResult = 1; scanProj = p;
                float pz, py;
                if (g_pitchZ.size() < 512 && PitchFromUp(p.upZ, &pz)) g_pitchZ.push_back(pz);
                if (g_pitchY.size() < 512 && PitchFromUp(p.upY, &py)) g_pitchY.push_back(py);
                break;
            }
            scanResult = 2;
            r += 3;
        }
        if (!skipScan && !scanFound && s->noProjScans < 100000) s->noProjScans++;
    }
    if (scanFound) StatAdd(scanResult == 1 ? scanProj : scanFirst, g_vpW, g_vpH);
    bool en = false; const GameProj* P = nullptr;
    if (scanResult == 1 && scanEye >= 0 && w2cam) {
        // engine camera: the engine itself rendered this eye (pose and projection) - nothing to transform
        g_vr.drawsMain++;
        if (!g_vr.drawEng) {
            xrb::IpcPose pp; GameProj pj;
            if (W2CamEyeRecord(scanEye, &pp, &pj)) { g_vr.drawEng = true; g_vr.drawPose = pp; g_vr.drawEye = scanEye; g_vr.drawProj = pj; }
        }
    } else if (g_vr.active && !w2cam && canWrite) {
        if (scanResult == 1) { en = true; P = &scanProj; }
        else if (scanResult == 0 && g_haveMain && g_vpW == g_vr.mainW && g_vpH == g_vr.mainH) { en = true; P = &g_vr.mainProj; }
    }
    float c[16];
    if (en) {
        static GameProj mP; static uint32_t mSerial = 0; static float m[12]; static float mLevel = 0;
        // engine camera mode: the game's own camera already carries this eye's head pose -> projection remap only
        bool engCam = g_vr.engineCam && g_vr.engineCamFrame && g_vr.presentCount + 1 - g_vr.engineCamFrame <= 1;
        if (engCam && !g_vr.drawEng) { g_vr.drawEng = true; g_vr.drawPose = g_vr.engPose; g_vr.drawEye = g_vr.engEye; }
        static bool mEng = false;
        if (mSerial != g_frameSerial || memcmp(&mP, P, sizeof mP) || mLevel != g_vr.camPitch || mEng != engCam) {
            mLevel = g_vr.camPitch; mEng = engCam;
            mP = *P; mSerial = g_frameSerial;
            if (engCam) {
                static const xrb::IpcPose id = { 0, 0, 0, 0, 0, 0, 1 }; static const float zero[3] = { 0, 0, 0 };
                EyeClipMatrix(mP, g_vr.outProj, id, 1.f, false, 0.f, zero, m);
            } else {
                float level = (g_vr.decoupledPitch && g_vr.camPitchValid) ? g_vr.camPitch : 0.f;
                static const float noOff[3] = { 0, 0, 0 };   // first person has its own offsets: the third-person one does not add
                EyeClipMatrix(mP, g_vr.outProj, g_vr.pose, g_vr.worldScale, g_vr.positional, level, g_vr.camOffset, m);
            }
        }
        memcpy(c, m, sizeof m);
        g_vr.drawsXf++;
    } else memset(c, 0, 48);
    // HUD: 2D draws into the back buffer that do not read a screen-size render target (that would be the scene
    // being composited), scaled around the centre to fit the game's view inside the wider VR view
    bool hud = false;
    bool hudOn = g_vr.hudMode == 0 || g_vr.hudScaleEff != 1.f || g_vr.hudSize != 1.f || g_vr.hudOffset[0] != 0.f || g_vr.hudOffset[1] != 0.f;
    // a draw sampling a screen-size render target is a composite (the scene / post-processing into this target); onto
    // the back buffer it also tells which target the game copies to the screen
    // ... and a draw sampling ANY screen-shaped render target (bloom, light shafts, scene copies at 1/2, 1/4, 1/8 ...
    // size) is part of the 3D picture (an additive glow over the frame), never HUD - else the HUD panel carries a faint
    // copy of the scene
    bool samplesBig = false, samplesScreen = false;
    if ((g_rtIsBB || g_rtIsFinal) && !scanFound) for (int st = 0; st < 8; st++) {
        IDirect3DBaseTexture9* t = g_stageTex[st];
        if (t && t->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC td;
            if (SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &td)) && (td.Usage & D3DUSAGE_RENDERTARGET) && g_bbH && td.Height &&
                td.Width * 16 >= g_bbW && fabsf(((float)td.Width / td.Height) / ((float)g_bbW / g_bbH) - 1.f) < 0.03f) samplesScreen = true;
            if (SUCCEEDED(((IDirect3DTexture9*)t)->GetLevelDesc(0, &td)) && (td.Usage & D3DUSAGE_RENDERTARGET) && td.Width * 2 >= g_bbW) {
                samplesBig = true;
                if (g_rtIsBB && td.Width == g_bbW && td.Height == g_bbH) {
                    IDirect3DSurface9* lvl = nullptr;
                    if (SUCCEEDED(((IDirect3DTexture9*)t)->GetSurfaceLevel(0, &lvl)) && lvl) { g_finalSrcNow = lvl; lvl->Release(); }
                }
            }
        }
    }
    if (samplesBig && g_rtIsFinal) g_finalComposited = true;
    const bool hudTarget = g_rtIsBB || (g_rtIsFinal && g_finalComposited);
    if (g_vr.active && hudTarget && hudOn && !scanFound && !samplesBig && !samplesScreen) hud = true;
    else if (g_vr.active && hudTarget && hudOn && !scanFound && samplesScreen && !samplesBig) g_hudKeptOut++;   // glow / scene copy   // 2D draws only (3D markers keep their stereo)
    IDirect3DSurface9* hudRT = nullptr;
    if (hud && g_vr.hudMode == 0 && g_bbMsType == D3DMULTISAMPLE_NONE && g_bbSurf && (hudRT = XrHudSurface(d))) {
        // HUD panel: this 2D draw goes into the HUD's own target at full size (shown as a head-locked quad);
        // alpha blended as premultiplied (colour over transparent black, alpha accumulated with ONE / INVSRCALPHA)
        g_redirFrom = g_curRT0 ? g_curRT0 : g_bbSurf;
        if (g_rtIsBB) g_hudOnBB++; else g_hudOnFinal++;
        oSetRT(d, 0, hudRT);
        if (g_vpFull.Width) oSetViewport(d, &g_vpFull);
        if (g_scissorSet && oSetScissor) oSetScissor(d, &g_scissor);
        if (g_rsAB) { oSetRS(d, D3DRS_SEPARATEALPHABLENDENABLE, TRUE); oSetRS(d, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
                      oSetRS(d, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA); oSetRS(d, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD); }
        g_redir = true; g_redirDraws++;
        XrNoteHudDraw();
        hud = false; en = false;
        memset(c, 0, 48);
    }
    if (hud) {   // eye transform off for this 2D draw; c252 = HUD size / position (live from the VR panel)
        float as = 1.f, ax = 0.f, ay = 0.f;
        HudAspectFix(&as, &ax, &ay);
        float sc = g_vr.hudScaleEff * g_vr.hudSize * as;
        c[0] = sc - 1.f; c[1] = sc - 1.f; c[2] = ax + g_vr.hudOffset[0]; c[3] = ay + g_vr.hudOffset[1];
        en = false;
    }
    c[12] = 1.f; c[13] = 1e-12f; c[14] = en ? 1.f : 0.f; c[15] = hud ? 1.f : 0.f;
    if (g_tstatLog) if (TargetStat* t = TStat()) { t->draws++; if (scanFound) t->proj++; if (en) t->xf++; if (hud) t->hud++; }
    if (canWrite) oSetVSConstF(d, kConstBase, c, 4);
}
bool D3D9GameWritesReserved() { return g_gameUsesReserved; }
static void PostDraw(IDirect3DDevice9* d) {
    HookTime ht_;
    g_skipDraw = false;
    if (g_biasSet) { g_biasSet = false; oSetRS(d, D3DRS_SLOPESCALEDEPTHBIAS, g_rsSlope); oSetRS(d, D3DRS_DEPTHBIAS, g_rsDepthBias); }
    if (g_redir) {   // back to the game's back buffer and its own states
        g_redir = false;
        oSetRT(d, 0, g_redirFrom ? g_redirFrom : g_bbSurf);
        if (g_vpFull.Width) oSetViewport(d, &g_vpFull);
        if (g_scissorSet && oSetScissor) oSetScissor(d, &g_scissor);
        if (g_rsAB) { oSetRS(d, D3DRS_SEPARATEALPHABLENDENABLE, g_rsSepAB); oSetRS(d, D3DRS_SRCBLENDALPHA, g_rsSrcA);
                      oSetRS(d, D3DRS_DESTBLENDALPHA, g_rsDstA); oSetRS(d, D3DRS_BLENDOPALPHA, g_rsOpA); }
    }
    if (g_gameUsesReserved && !g_inOwn && g_curShader && g_curShader->info.patched) oSetVSConstF(d, kConstBase, &g_c[kConstBase][0], 4);
}

static void MbNoteDraw(INT64 firstVert, INT64 numVerts, INT64 firstIdx, INT64 numIdx); static INT64 PrimVerts(D3DPRIMITIVETYPE t, UINT pc);
static HRESULT STDMETHODCALLTYPE hDrawPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT b) {
    PreDraw(d); HRESULT hr = g_skipDraw ? D3D_OK : oDrawPrim(d, t, a, b); PostDraw(d);
    if (!g_inOwn) MbNoteDraw(a, PrimVerts(t, b), 0, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hDrawIdxPrim(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc) {
    PreDraw(d); HRESULT hr = g_skipDraw ? D3D_OK : oDrawIdxPrim(d, t, bv, mi, nv, si, pc); PostDraw(d);
    if (!g_inOwn) MbNoteDraw((INT64)bv + mi, nv, si, PrimVerts(t, pc));
    return hr;
}
static IDirect3DVertexBuffer9* g_strOrig[16] = {}; static IDirect3DIndexBuffer9* g_idxOrig = nullptr;   // (stall-free buffers, below)
static HRESULT STDMETHODCALLTYPE hDrawPrimUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT pc, const void* v, UINT st) {
    if (!g_inOwn) g_strOrig[0] = nullptr;   // D3D9 unbinds stream 0 after a UP draw
    PreDraw(d); HRESULT hr = g_skipDraw ? D3D_OK : oDrawPrimUP(d, t, pc, v, st); PostDraw(d); return hr;
}
static HRESULT STDMETHODCALLTYPE hDrawIdxPrimUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void* i, D3DFORMAT f, const void* v, UINT st) {
    if (!g_inOwn) { g_strOrig[0] = nullptr; g_idxOrig = nullptr; }   // D3D9 unbinds stream 0 and the indices after it
    PreDraw(d); HRESULT hr = g_skipDraw ? D3D_OK : oDrawIdxPrimUP(d, t, mi, nv, pc, i, f, v, st); PostDraw(d); return hr;
}

static HRESULT STDMETHODCALLTYPE hSetVSConstF(IDirect3DDevice9* d, UINT reg, const float* data, UINT count) {
    if (!g_inOwn && data && reg < 256) {
        UINT n = count; if (reg + n > 256) n = 256 - reg;
        memcpy(&g_c[reg][0], data, n * 16);
        g_constGen++;
        if (reg + n > (UINT)kConstBase) {
            if (!g_gameUsesReserved && !g_cfgAnalyseOnly) LOGW("stereo: the game writes c%u..c%u, which overlaps the mod's c%d..c255", reg, reg + n - 1, kConstBase);
            g_gameUsesReserved = true; g_vr.reservedWrites++;
        }
    }
    return oSetVSConstF(d, reg, data, count);
}

static HRESULT STDMETHODCALLTYPE hCreateVS(IDirect3DDevice9* d, const DWORD* func, IDirect3DVertexShader9** out) {
    if (!func || !out || g_inOwn || !g_cfgPatch) return oCreateVS(d, func, out);
    auto si = std::make_unique<ShaderInfo>();
    std::vector<uint32_t> patched;
    HRESULT hr = E_FAIL;
    if (VsPatch((const uint32_t*)func, patched, kConstBase, si->info, !g_cfgEngineCamStart, g_cfgAnalyseOnly)) {
        hr = oCreateVS(d, (const DWORD*)patched.data(), out);
        if (FAILED(hr)) { si->info.patched = false; si->info.reason = "runtime rejected the patched shader"; LOGW("stereo: patched shader rejected (%08lx), using the original", hr); }
    }
    if (FAILED(hr)) hr = oCreateVS(d, func, out);
    if (SUCCEEDED(hr) && *out) {
        if (si->info.patched) g_vr.shadersPatched++;
        else {
            if (((si->info.version >> 8) & 0xFF) < 2) g_vr.shadersV1++; else g_vr.shadersSkipped++;
            static int logged = 0;
            if (!g_cfgAnalyseOnly && logged++ < 20) LOGI("stereo: vertex shader %08x not patched: %s", si->info.version, si->info.reason.c_str());
        }
        EnterCriticalSection(&g_shaderCs);
        g_shaders[*out] = std::move(si);
        LeaveCriticalSection(&g_shaderCs);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hSetVS(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    if (!g_inOwn) {
        ShaderInfo* info = nullptr;
        if (s) {
            EnterCriticalSection(&g_shaderCs);
            auto it = g_shaders.find(s);
            if (it != g_shaders.end()) info = it->second.get();
            LeaveCriticalSection(&g_shaderCs);
        }
        g_curShader = info;
    }
    return oSetVS(d, s);
}

typedef HRESULT (STDMETHODCALLTYPE* SetTextureFn)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
static SetTextureFn oSetTexture;

// ---------------------------------------------------------------- SetLOD of (former) managed textures
// IDirect3DBaseTexture9::SetLOD only works on D3DPOOL_MANAGED textures. REDengine's texture streaming uses it to keep
// the GPU off mip levels it has not uploaded yet; with D3D9Ex those textures live in D3DPOOL_DEFAULT, where SetLOD
// does nothing - the GPU then samples top mips that hold zeros or another (recycled) texture's pixels: garbage texels
// on surfaces that shimmer as the view (and so the mip) changes. Emulated: the LOD the game sets is remembered per
// texture and applied as D3DSAMP_MAXMIPLEVEL whenever that texture is bound.
static CRITICAL_SECTION g_lodCs;
static std::unordered_map<IDirect3DBaseTexture9*, DWORD> g_lod;
static DWORD g_stageLod[8] = {}; static bool g_ssDirty = true;   // sampler enforcement needed before the next draw
static unsigned g_lodCalls = 0, g_lodNonZero = 0, g_lodApplied = 0; static DWORD g_lodMax = 0;
static bool g_lodEmu = true;
static DWORD LodOf(IDirect3DBaseTexture9* t) {
    if (!t) return 0;
    EnterCriticalSection(&g_lodCs);
    auto it = g_lod.find(t); DWORD v = it == g_lod.end() ? 0 : it->second;
    LeaveCriticalSection(&g_lodCs);
    return v;
}

// ---------------------------------------------------------------- texture filtering for a headset
// Material textures (not render targets, with mip levels) get: the game's negative mip LOD bias raised to
// texture_lod_bias_min (0 by default: sharper-than-screen mips sparkle and crawl when the head moves), point mip
// filtering made trilinear, and anisotropic filtering (texture_anisotropy, 16 by default) where the game filters
// linearly. Render targets (post-processing, shadow maps, G-buffer) keep exactly the game's states. Only changes are
// sent: the device's actual state is tracked against what the game set.
typedef HRESULT (STDMETHODCALLTYPE* SetSSFn)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
static SetSSFn oSetSS;
static DWORD g_ssGame[8][14], g_ssDev[8][14];          // types 0..13 for samplers 0..7
static bool g_stageMat[8] = {};
static bool g_tfOn = true; static float g_tfMinBias = 0.f; static DWORD g_tfAniso = 16;
static unsigned g_tfNegBias = 0, g_tfPointMip = 0, g_tfChanged = 0; static float g_tfLowestBias = 0.f;
static void SSInitDefaults() {
    for (int s = 0; s < 8; s++) {
        for (int t = 0; t < 14; t++) g_ssGame[s][t] = g_ssDev[s][t] = 0;
        DWORD* g = g_ssGame[s];
        g[D3DSAMP_ADDRESSU] = g[D3DSAMP_ADDRESSV] = g[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        g[D3DSAMP_MAGFILTER] = g[D3DSAMP_MINFILTER] = D3DTEXF_POINT; g[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        g[D3DSAMP_MAXANISOTROPY] = 1;
        memcpy(g_ssDev[s], g, sizeof g_ssGame[s]);
    }
}
static HRESULT STDMETHODCALLTYPE hSetSS(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (!g_inOwn && s < 8 && (unsigned)t < 14) {
        g_ssGame[s][t] = v; g_ssDirty = true;
        if (t == D3DSAMP_MIPMAPLODBIAS) { float b; memcpy(&b, &v, 4); if (b < 0.f) { g_tfNegBias++; if (b < g_tfLowestBias) g_tfLowestBias = b; } }
        if (t == D3DSAMP_MIPFILTER && v == D3DTEXF_POINT) g_tfPointMip++;
        g_ssDev[s][t] = v;   // always forwarded (the game's own state cache already skips repeats)
    }   // the mod's own draws: not tracked - they restore the device from a state block afterwards
    return oSetSS(d, s, t, v);
}
static void NoteStageTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    bool mat = false;
    if (t && (t->GetType() == D3DRTYPE_TEXTURE || t->GetType() == D3DRTYPE_CUBETEXTURE) && t->GetLevelCount() > 1) {
        D3DSURFACE_DESC sd = {};
        HRESULT hr = t->GetType() == D3DRTYPE_TEXTURE ? ((IDirect3DTexture9*)t)->GetLevelDesc(0, &sd) : ((IDirect3DCubeTexture9*)t)->GetLevelDesc(0, &sd);
        mat = SUCCEEDED(hr) && !(sd.Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
    }
    g_stageMat[stage] = mat;
    g_stageLod[stage] = g_lodEmu ? LodOf(t) : 0;
}
static bool g_tfTestOff = false;   // diagnostics: the game's own filtering for a few seconds (second stage dump)
void D3D9TextureFilteringTest(bool off) { g_tfTestOff = off; g_ssDirty = true; }
static void EnforceSamplers(IDirect3DDevice9* d) {
    if (!oSetSS) return;
    if (!g_ssDirty) return;   // 1.0.5: nothing the result depends on changed since the last draw
    g_ssDirty = false;
    bool changed = false;
    for (int s = 0; s < 8; s++) {
        const DWORD* g = g_ssGame[s]; DWORD want[14]; memcpy(want, g, sizeof want);
        if (g_tfOn && !g_tfTestOff && g_stageMat[s]) {
            float b; memcpy(&b, &g[D3DSAMP_MIPMAPLODBIAS], 4);
            if (b < g_tfMinBias) memcpy(&want[D3DSAMP_MIPMAPLODBIAS], &g_tfMinBias, 4);
            if (g[D3DSAMP_MIPFILTER] == D3DTEXF_POINT) want[D3DSAMP_MIPFILTER] = D3DTEXF_LINEAR;
            if (g_tfAniso > 1 && (g[D3DSAMP_MINFILTER] == D3DTEXF_LINEAR || g[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC)) {
                want[D3DSAMP_MINFILTER] = D3DTEXF_ANISOTROPIC;
                want[D3DSAMP_MAXANISOTROPY] = g[D3DSAMP_MAXANISOTROPY] > g_tfAniso ? g[D3DSAMP_MAXANISOTROPY] : g_tfAniso;
            }
        }
        if (g_stageLod[s] > g[D3DSAMP_MAXMIPLEVEL]) { want[D3DSAMP_MAXMIPLEVEL] = g_stageLod[s]; g_lodApplied++; }
        static const D3DSAMPLERSTATETYPE ts[] = { D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXANISOTROPY, D3DSAMP_MAXMIPLEVEL };
        for (auto t : ts) if (g_ssDev[s][t] != want[t]) { oSetSS(d, s, t, want[t]); g_ssDev[s][t] = want[t]; changed = true; }
    }
    if (changed) g_tfChanged++;
}
void D3D9SetTextureFiltering(bool on, float minBias, int aniso) { g_ssDirty = true; g_tfOn = on; g_tfMinBias = minBias; g_tfAniso = aniso < 1 ? 1 : aniso > 16 ? 16 : (DWORD)aniso; }
extern volatile LONG g_cubeCount; static void CubeUploadIfDirty(IDirect3DDevice9* d, IDirect3DBaseTexture9* t);
extern volatile LONG g_mvCount; static void MvUpload(IDirect3DDevice9* d, IDirect3DBaseTexture9* t);
extern volatile LONG g_mtCount;
static void GbufCensusNote(DWORD stage, IDirect3DBaseTexture9* t);
static HRESULT STDMETHODCALLTYPE hSetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    if (stage < 8 && !g_inOwn) { g_stageTex[stage] = t; g_ssDirty = true; }
    if (stage < 8 && !g_inOwn) NoteStageTexture(stage, t);
    if (t && g_cubeCount) CubeUploadIfDirty(d, t);
    if (t && g_mtCount && t->GetType() == D3DRTYPE_TEXTURE) MtUpload(d, (IDirect3DTexture9*)t);
    if (t && g_mvCount && t->GetType() == D3DRTYPE_VOLUMETEXTURE) MvUpload(d, t);
    if (stage < 16 && !g_inOwn) GbufCensusNote(stage, t);
    return oSetTexture(d, stage, t);
}
static unsigned g_srFixed = 0;
// viewport: tracked only. (0.1.3/0.1.4 scaled viewports that exceed the target; the game sets a screen-size viewport
// on the shadow map before switching targets, and D3D9 rejecting it is what keeps the shadows right.)
static HRESULT STDMETHODCALLTYPE hSetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp) {
    if (vp && !g_inOwn && vp->X + vp->Width <= g_rtW && vp->Y + vp->Height <= g_rtH) { g_vpW = vp->Width; g_vpH = vp->Height; g_vpX = vp->X; g_vpY = vp->Y; g_vpFull = *vp; }
    return oSetViewport(d, vp);
}
typedef HRESULT (STDMETHODCALLTYPE* StretchRectFn)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
static StretchRectFn oStretchRect;
static HRESULT STDMETHODCALLTYPE hStretchRect(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t, const RECT* tr, D3DTEXTUREFILTERTYPE f) {
    if (!g_inOwn && t && s && s != t) {
        if (t == g_bbSurf) g_finalSrcNow = s;                     // the game's picture goes to the screen from s
        else if (t == g_finalSrc) g_finalComposited = true;       // the scene copied into the final target
    }
    D3DSURFACE_DESC td;
    if (!g_inOwn && tr && t && SUCCEEDED(t->GetDesc(&td)) && (tr->right > (LONG)td.Width || tr->bottom > (LONG)td.Height)) {
        RECT r = *tr;
        float sc = std::min((float)td.Width / tr->right, (float)td.Height / tr->bottom);
        r.left = (LONG)(r.left * sc); r.top = (LONG)(r.top * sc); r.right = std::min((LONG)(r.right * sc), (LONG)td.Width); r.bottom = std::min((LONG)(r.bottom * sc), (LONG)td.Height);
        if (g_srFixed++ < 5) LOGW("d3d9: StretchRect target %ldx%ld exceeds the %ux%u surface - scaled to %ldx%ld", tr->right - tr->left, tr->bottom - tr->top, td.Width, td.Height, r.right - r.left, r.bottom - r.top);
        return oStretchRect(d, s, sr, t, &r, f);
    }
    return oStretchRect(d, s, sr, t, tr, f);
}
static HRESULT STDMETHODCALLTYPE hSetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v) {
    if (!g_inOwn) switch (s) {
        case D3DRS_ALPHABLENDENABLE: g_rsAB = v; break;
        case D3DRS_SEPARATEALPHABLENDENABLE: g_rsSepAB = v; break;
        case D3DRS_SRCBLENDALPHA: g_rsSrcA = v; break;
        case D3DRS_DESTBLENDALPHA: g_rsDstA = v; break;
        case D3DRS_BLENDOPALPHA: g_rsOpA = v; break;
        case D3DRS_SLOPESCALEDEPTHBIAS: g_rsSlope = v; break;
        case D3DRS_DEPTHBIAS: g_rsDepthBias = v; break;
        default: break;
    }
    return oSetRS(d, s, v);
}
static HRESULT STDMETHODCALLTYPE hSetScissor(IDirect3DDevice9* d, const RECT* r) {
    if (r && !g_inOwn) { g_scissor = *r; g_scissorSet = true; }
    return oSetScissor(d, r);
}
static HRESULT STDMETHODCALLTYPE hSetRT(IDirect3DDevice9* d, DWORD idx, IDirect3DSurface9* surf) {
    if (g_sdState == 2 && !g_inOwn && g_sdDraws) { StageDumpBound(d); g_sdDraws = 0; }
    if (!g_inOwn) {
        // the game leaves the G-buffer (hundreds of draws into colour + specular + normals + depth): repair the normals once
        if (g_tstView && !g_nrDone && g_rtDraws > 300 && g_rt2Bound) TestGrabLayer(d);
        if (g_nrOn && !g_nrDone && g_rtDraws > 300) {
            IDirect3DSurface9* n2 = nullptr;
            if (SUCCEEDED(d->GetRenderTarget(2, &n2)) && n2) {
                D3DSURFACE_DESC nd;
                if (SUCCEEDED(n2->GetDesc(&nd)) && nd.Format == D3DFMT_A8R8G8B8 && nd.Width == g_bbW && nd.Height == g_bbH) { g_nrDone = true; NormalRepair(d, n2); }
                n2->Release();
            }
        }
        if (idx == 0) { g_rtDraws = 0; g_nrDone = false; }
    }
    if (idx == 2 && !g_inOwn) g_rt2Bound = surf != nullptr;
    if (idx == 0 && surf && !g_inOwn) {
        g_rtIsBB = surf == g_bbSurf;
        g_curRT0 = surf;
        D3DSURFACE_DESC sd;
        g_rtIsFinal = false;
        if (SUCCEEDED(surf->GetDesc(&sd))) {
            g_rtIsFinal = !g_rtIsBB && surf == g_finalSrc && sd.Width == g_bbW && sd.Height == g_bbH;
            // depth-only target that is not the screen's size: a shadow map (sun cascades, spot / cube shadows)
            g_rtIsShadow = sd.Format == (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L') && !(sd.Width == g_bbW && sd.Height == g_bbH);
            g_rtW = g_vpW = sd.Width; g_rtH = g_vpH = sd.Height; g_vpX = g_vpY = 0; g_rtFmt = sd.Format;
            g_vpFull = { 0, 0, sd.Width, sd.Height, 0.f, 1.f };
        }
    }
    return oSetRT(d, idx, surf);
}

// ---------------------------------------------------------------- managed pool redirection (D3D9Ex)
// D3D9Ex has no D3DPOOL_MANAGED. A managed 2D texture becomes a D3DPOOL_DEFAULT texture plus a D3DPOOL_SYSTEMMEM
// shadow: LockRect / UnlockRect go to the shadow and UnlockRect queues UpdateTexture (no CPU/GPU stall), which is what
// the managed pool does. The default texture is also D3DUSAGE_DYNAMIC so a lock through GetSurfaceLevel still works.
static unsigned g_convTex = 0, g_convBuf = 0, g_shadowLocks = 0, g_shadowFallback = 0, g_directTex = 0;
static bool g_directLock = true;
static CRITICAL_SECTION g_shadowCs;
static std::unordered_map<IDirect3DTexture9*, IDirect3DTexture9*> g_shadow;
static std::unordered_map<uint64_t, DWORD> g_lockFlags;   // flags of the open lock per texture level (render thread)
typedef HRESULT (STDMETHODCALLTYPE* TexLockFn)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* TexUnlockFn)(IDirect3DTexture9*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* TexDirtyFn)(IDirect3DTexture9*, const RECT*);
typedef ULONG (STDMETHODCALLTYPE* TexReleaseFn)(IDirect3DTexture9*);
static TexLockFn oTexLock; static TexUnlockFn oTexUnlock; static TexDirtyFn oTexDirty; static TexReleaseFn oTexRelease;
static IDirect3DTexture9* ShadowOf(IDirect3DTexture9* t) {
    EnterCriticalSection(&g_shadowCs);
    auto it = g_shadow.find(t);
    IDirect3DTexture9* s = it == g_shadow.end() ? nullptr : it->second;
    LeaveCriticalSection(&g_shadowCs);
    return s;
}

// ---------------------------------------------------------------- 0.9.4: exact managed-pool emulation (2D textures)
// D3D9Ex has no D3DPOOL_MANAGED. The managed pool keeps every texture in system memory, takes ALL writes there (texture
// LockRect, surface LockRect through GetSurfaceLevel - the path D3DX uses -, AddDirtyRect) and copies the whole texture
// to video memory before it is next drawn with. 0.1 - 0.9.3 approximated that (a dynamic video-memory texture locked
// directly, or a copy updated per unlock), and the G-buffer normals came out empty where the normal / height maps should
// be - textures read by the G-buffer pass with missing content. managed_mode = exact does what the managed pool does:
// a static video-memory texture + a system-memory copy; every write path goes to the copy; the copy is uploaded whole
// (UpdateTexture) when the texture is next bound or preloaded. Memory: like the real managed pool (one copy each side).
struct MTex { IDirect3DTexture9* sys; volatile LONG dirty; };
static std::unordered_map<IDirect3DTexture9*, MTex> g_mt;
static std::unordered_map<IDirect3DSurface9*, IDirect3DTexture9*> g_mtSurfOf;   // system-memory level surfaces handed out -> their texture
volatile LONG g_mtAnyDirty = 0;
typedef HRESULT (STDMETHODCALLTYPE* SurfUnlockFn)(IDirect3DSurface9*);
static SurfUnlockFn oSurfUnlock;
volatile LONG g_mtCount = 0;
static volatile LONG g_mtUploads = 0, g_mtSurf = 0, g_mtLocks = 0, g_mtFail = 0;
static int g_managedMode = 2;   // 2 exact, 1 direct (0.6.1 - 0.9.3), 0 copy per unlock (0.6.0)
typedef HRESULT (STDMETHODCALLTYPE* TexSurfFn)(IDirect3DTexture9*, UINT, IDirect3DSurface9**);
typedef void (STDMETHODCALLTYPE* PreLoadFn)(IDirect3DTexture9*);
static TexSurfFn oTexSurf; static PreLoadFn oTexPreLoad;
static IDirect3DTexture9* MtSys(IDirect3DTexture9* t, bool markDirty) {
    if (!g_mtCount) return nullptr;
    IDirect3DTexture9* r = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_mt.find(t); if (it != g_mt.end()) { r = it->second.sys; if (markDirty) { it->second.dirty = 1; g_mtAnyDirty = 1; } }
    LeaveCriticalSection(&g_shadowCs);
    return r;
}
static void MtUpload(IDirect3DDevice9* d, IDirect3DTexture9* t) {
    IDirect3DTexture9* s = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_mt.find(t); if (it != g_mt.end() && it->second.dirty) { s = it->second.sys; it->second.dirty = 0; s->AddRef(); }
    LeaveCriticalSection(&g_shadowCs);
    if (!s) return;
    s->AddDirtyRect(nullptr);
    InterlockedIncrement(&g_inOwn);
    HRESULT hr = d->UpdateTexture(s, t);
    InterlockedDecrement(&g_inOwn);
    s->Release();
    InterlockedIncrement(&g_mtUploads);
    if (FAILED(hr)) { InterlockedIncrement(&g_mtFail); static int n = 0; if (n++ < 5) LOGW("d3d9: managed texture upload failed %08lx", hr); }
}
static HRESULT STDMETHODCALLTYPE hMtLock(IDirect3DTexture9* t, UINT l, D3DLOCKED_RECT* r, const RECT* rc, DWORD f) {
    if (IDirect3DTexture9* s = MtSys(t, false)) { InterlockedIncrement(&g_mtLocks); return s->LockRect(l, r, rc, f & (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NO_DIRTY_UPDATE)); }
    return oTexLock(t, l, r, rc, f);
}
static HRESULT STDMETHODCALLTYPE hMtUnlock(IDirect3DTexture9* t, UINT l) {
    if (IDirect3DTexture9* s = MtSys(t, false)) { HRESULT hr = s->UnlockRect(l); MtSys(t, true); return hr; }   // dirty once written
    return oTexUnlock(t, l);
}
static HRESULT STDMETHODCALLTYPE hSurfUnlock(IDirect3DSurface9* sf) {
    HRESULT hr = oSurfUnlock(sf);
    IDirect3DTexture9* t = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_mtSurfOf.find(sf); if (it != g_mtSurfOf.end()) t = it->second;
    LeaveCriticalSection(&g_shadowCs);
    if (t) MtSys(t, true);   // written through the level surface (D3DX): dirty
    return hr;
}
static HRESULT STDMETHODCALLTYPE hMtSurf(IDirect3DTexture9* t, UINT l, IDirect3DSurface9** out) {
    if (!g_inOwn) if (IDirect3DTexture9* s = MtSys(t, true)) {
        InterlockedIncrement(&g_mtSurf);
        HRESULT hr = s->GetSurfaceLevel(l, out);
        if (SUCCEEDED(hr) && out && *out) {
            if (!oSurfUnlock) Hook(VT(*out)[14], (void*)hSurfUnlock, &oSurfUnlock, "Surface::UnlockRect");
            EnterCriticalSection(&g_shadowCs); g_mtSurfOf[*out] = t; LeaveCriticalSection(&g_shadowCs);
        }
        return hr;
    }
    return oTexSurf(t, l, out);
}
static void STDMETHODCALLTYPE hMtPreLoad(IDirect3DTexture9* t) {
    if (MtSys(t, false)) { IDirect3DDevice9* d = nullptr; if (SUCCEEDED(t->GetDevice(&d)) && d) { MtUpload(d, t); d->Release(); } return; }
    oTexPreLoad(t);
}
static void MtForget(IDirect3DTexture9* t) {
    IDirect3DTexture9* s = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_mt.find(t); if (it != g_mt.end()) { s = it->second.sys; g_mt.erase(it); InterlockedDecrement(&g_mtCount); }
    if (s) for (auto i = g_mtSurfOf.begin(); i != g_mtSurfOf.end();) { if (i->second == t) i = g_mtSurfOf.erase(i); else ++i; }
    LeaveCriticalSection(&g_shadowCs);
    if (s) s->Release();
}


// ---------------------------------------------------------------- texture tests (VR panel > Tests, live, 0.9.7)
// Not saved: every start is a normal run. view: 0 = normal picture, 1..3 = the G-buffer's colour / specular / normal
// target shown in place of the picture (so a wrong colour can be traced to a layer in the headset); decalsOff: draws
// that BLEND into the G-buffer (decals, blood, dirt layers) are skipped; reupload: every texture copied to video memory
// again (one shot); the others switch single mod features off.
static bool g_tstNoMtUpload = false;
static IDirect3DTexture9* g_tstTex = nullptr; static bool g_tstHave = false;
void D3D9SetTests(int view, bool decalsOff, bool normalRepair, bool reupload) {
    g_tstView = view < 0 || view > 3 ? 0 : view; g_tstDecalsOff = decalsOff; g_nrOn = normalRepair;
    if (reupload) {
        EnterCriticalSection(&g_shadowCs);
        for (auto& e : g_mt) e.second.dirty = 1;
        LeaveCriticalSection(&g_shadowCs);
        g_mtAnyDirty = 1;
        LOGI("tests: every texture marked for upload again (%ld textures)", (long)g_mtCount);
    }
    static int lastView = -1; static bool lastDec = false, lastNr = true;
    if (lastView != g_tstView || lastDec != g_tstDecalsOff || lastNr != g_nrOn)
        LOGI("tests: view %s, blended G-buffer draws (decals) %s, normal repair %s",
             g_tstView == 0 ? "normal" : g_tstView == 1 ? "G-buffer COLOUR" : g_tstView == 2 ? "G-buffer SPECULAR" : "G-buffer NORMALS",
             g_tstDecalsOff ? "SKIPPED" : "drawn", g_nrOn ? "on" : "off");
    lastView = g_tstView; lastDec = g_tstDecalsOff; lastNr = g_nrOn;
}
// at the end of the G-buffer: keep a copy of the chosen layer
static void TestGrabLayer(IDirect3DDevice9* d) {
    if (!g_tstView) return;
    IDirect3DSurface9* src = nullptr;
    if (FAILED(d->GetRenderTarget(g_tstView - 1, &src)) || !src) return;
    D3DSURFACE_DESC sd; src->GetDesc(&sd);
    D3DSURFACE_DESC td = {};
    if (g_tstTex) g_tstTex->GetLevelDesc(0, &td);
    if (!g_tstTex || td.Width != sd.Width || td.Height != sd.Height) {
        if (g_tstTex) { g_tstTex->Release(); g_tstTex = nullptr; }
        InterlockedIncrement(&g_inOwn);
        if (FAILED(CallCreateTexture(d, sd.Width, sd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tstTex, nullptr))) g_tstTex = nullptr;
        InterlockedDecrement(&g_inOwn);
    }
    if (g_tstTex) {
        IDirect3DSurface9* dst = nullptr; g_tstTex->GetSurfaceLevel(0, &dst);
        if (dst) { g_tstHave = SUCCEEDED(oStretchRect(d, src, nullptr, dst, nullptr, D3DTEXF_NONE)); dst->Release(); }
    }
    src->Release();
}
// at Present, before the eye picture is taken: the chosen layer replaces the picture
static void TestShowLayer(IDirect3DDevice9* d) {
    if (!g_tstView || !g_tstHave || !g_tstTex || !g_bbSurf) return;
    IDirect3DSurface9* s = nullptr; g_tstTex->GetSurfaceLevel(0, &s);
    if (s) { oStretchRect(d, s, nullptr, g_bbSurf, nullptr, D3DTEXF_POINT); s->Release(); }
    g_tstHave = false;
}
static HRESULT STDMETHODCALLTYPE hTexLock(IDirect3DTexture9* t, UINT l, D3DLOCKED_RECT* r, const RECT* rc, DWORD f) {
    if (g_mtCount && MtSys(t, false)) return hMtLock(t, l, r, rc, f);
    if (IDirect3DTexture9* s = ShadowOf(t)) {
        g_shadowLocks++;
        HRESULT hr = s->LockRect(l, r, rc, f & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK));
        if (SUCCEEDED(hr)) { EnterCriticalSection(&g_shadowCs); g_lockFlags[((uint64_t)(uintptr_t)t << 5) | l] = f; LeaveCriticalSection(&g_shadowCs); }
        return hr;
    }
    return oTexLock(t, l, r, rc, f);
}
// Only the level that was unlocked is copied to video memory. (0.1 - 0.5.9 copied the whole texture with
// UpdateTexture. The game fills many textures through D3DX - D3DXCreateTextureFromFileInMemory writes each mip level
// through IDirect3DSurface9::LockRect, straight into the video-memory texture, bypassing the system-memory copy - and
// streams other levels through IDirect3DTexture9::LockRect. Every such texture lock then overwrote the D3DX-written
// levels with the copy's stale / empty ones: garbage texels in the game's textures.)
static unsigned g_lvlUpdates = 0, g_lvlFail = 0;
static HRESULT STDMETHODCALLTYPE hTexUnlock(IDirect3DTexture9* t, UINT l) {
    if (g_mtCount && MtSys(t, false)) return hMtUnlock(t, l);
    if (IDirect3DTexture9* s = ShadowOf(t)) {
        HRESULT hr = s->UnlockRect(l);
        bool readOnly = false;
        EnterCriticalSection(&g_shadowCs);
        { auto it = g_lockFlags.find(((uint64_t)(uintptr_t)t << 5) | l); if (it != g_lockFlags.end()) { readOnly = (it->second & D3DLOCK_READONLY) != 0; g_lockFlags.erase(it); } }
        LeaveCriticalSection(&g_shadowCs);
        if (!readOnly) {
            IDirect3DDevice9* d = nullptr;
            if (SUCCEEDED(t->GetDevice(&d)) && d) {
                IDirect3DSurface9 *src = nullptr, *dst = nullptr;
                HRESULT u = E_FAIL;
                if (SUCCEEDED(s->GetSurfaceLevel(l, &src)) && SUCCEEDED(t->GetSurfaceLevel(l, &dst))) u = d->UpdateSurface(src, nullptr, dst, nullptr);
                if (src) src->Release();
                if (dst) dst->Release();
                if (FAILED(u)) {   // fall back to the old whole-texture copy
                    g_lvlFail++;
                    s->AddDirtyRect(nullptr);
                    u = d->UpdateTexture(s, t);
                    static int n = 0; if (FAILED(u) && n++ < 5) LOGW("d3d9: UpdateTexture for a managed texture failed %08lx", u);
                } else g_lvlUpdates++;
                d->Release();
            }
        }
        return hr;
    }
    return oTexUnlock(t, l);
}
static ULONG STDMETHODCALLTYPE hTexRelease(IDirect3DTexture9* t);
static HRESULT STDMETHODCALLTYPE hTexDirty(IDirect3DTexture9* t, const RECT* rc);
typedef DWORD (STDMETHODCALLTYPE* SetLodFn)(IDirect3DBaseTexture9*, DWORD);
typedef DWORD (STDMETHODCALLTYPE* GetLodFn)(IDirect3DBaseTexture9*);
static SetLodFn oTexSetLod, oCubeSetLod; static GetLodFn oTexGetLod, oCubeGetLod;
static bool g_lodTracked(IDirect3DBaseTexture9* t, bool* found, DWORD* v) {
    EnterCriticalSection(&g_lodCs);
    auto it = g_lod.find(t); *found = it != g_lod.end(); *v = *found ? it->second : 0;
    LeaveCriticalSection(&g_lodCs);
    return *found;
}
static DWORD SetLodCommon(IDirect3DBaseTexture9* t, DWORD lod, SetLodFn orig) {
    bool f; DWORD old;
    if (!g_lodTracked(t, &f, &old)) return orig(t, lod);
    DWORD n = t->GetLevelCount(); if (n && lod >= n) lod = n - 1;
    EnterCriticalSection(&g_lodCs); g_lod[t] = lod; LeaveCriticalSection(&g_lodCs);
    g_lodCalls++; if (lod) g_lodNonZero++; if (lod > g_lodMax) g_lodMax = lod;
    for (int s = 0; s < 8; s++) if (g_stageTex[s] == t) { g_stageLod[s] = g_lodEmu ? lod : 0; g_ssDirty = true; }   // bound right now
    return old;
}
static DWORD STDMETHODCALLTYPE hTexSetLod(IDirect3DBaseTexture9* t, DWORD lod) { return SetLodCommon(t, lod, oTexSetLod); }
static DWORD STDMETHODCALLTYPE hCubeSetLod(IDirect3DBaseTexture9* t, DWORD lod) { return SetLodCommon(t, lod, oCubeSetLod); }
static DWORD STDMETHODCALLTYPE hTexGetLod(IDirect3DBaseTexture9* t) { bool f; DWORD v; return g_lodTracked(t, &f, &v) ? v : oTexGetLod(t); }
static DWORD STDMETHODCALLTYPE hCubeGetLod(IDirect3DBaseTexture9* t) { bool f; DWORD v; return g_lodTracked(t, &f, &v) ? v : oCubeGetLod(t); }
static void LodForget(IDirect3DBaseTexture9* t) { EnterCriticalSection(&g_lodCs); g_lod.erase(t); LeaveCriticalSection(&g_lodCs); }
typedef ULONG (STDMETHODCALLTYPE* CubeReleaseFn)(IDirect3DCubeTexture9*);
static CubeReleaseFn oCubeRelease;
static void CubeForget(IDirect3DBaseTexture9* t);
static ULONG STDMETHODCALLTYPE hCubeRelease(IDirect3DCubeTexture9* t) { ULONG n = oCubeRelease(t); if (n == 0) { LodForget(t); CubeForget(t); } return n; }
static void LodTrack(IDirect3DBaseTexture9* t, bool cube) {
    void** v = VT(t);
    if (cube) { Hook(v[11], (void*)hCubeSetLod, &oCubeSetLod, "CubeTexture::SetLOD"); Hook(v[12], (void*)hCubeGetLod, &oCubeGetLod, "CubeTexture::GetLOD");
                Hook(v[2], (void*)hCubeRelease, &oCubeRelease, "CubeTexture::Release"); }
    else { Hook(v[11], (void*)hTexSetLod, &oTexSetLod, "Texture::SetLOD"); Hook(v[12], (void*)hTexGetLod, &oTexGetLod, "Texture::GetLOD");
           Hook(v[2], (void*)hTexRelease, &oTexRelease, "Texture::Release");
           Hook(v[21], (void*)hTexDirty, &oTexDirty, "Texture::AddDirtyRect"); }
    EnterCriticalSection(&g_lodCs); g_lod[t] = 0; LeaveCriticalSection(&g_lodCs);
}
static HRESULT STDMETHODCALLTYPE hTexDirty(IDirect3DTexture9* t, const RECT* rc) {
    if (IDirect3DTexture9* ms = MtSys(t, true)) return ms->AddDirtyRect(rc);
    if (IDirect3DTexture9* s = ShadowOf(t)) return s->AddDirtyRect(rc);
    { bool f; DWORD v; if (g_lodTracked(t, &f, &v)) return D3D_OK; }   // a former managed texture locked directly: nothing to track
    return oTexDirty(t, rc);
}
static ULONG STDMETHODCALLTYPE hTexRelease(IDirect3DTexture9* t) {
    ULONG n = oTexRelease(t);
    if (n == 0) {
        MtForget(t);
        IDirect3DTexture9* s = nullptr;
        EnterCriticalSection(&g_shadowCs);
        auto it = g_shadow.find(t);
        if (it != g_shadow.end()) { s = it->second; g_shadow.erase(it); }
        LeaveCriticalSection(&g_shadowCs);
        if (s) s->Release();
        LodForget(t);
    }
    return n;
}
static HRESULT STDMETHODCALLTYPE hCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) {
    if (!g_cfgEx) { HRESULT hr0 = oCreateTexture(d, w, h, l, u, f, p, t, sh); if (SUCCEEDED(hr0) && t && *t) LodForget(*t); return hr0; }   // a plain D3D9 device keeps its managed pool
    if (p == D3DPOOL_MANAGED && g_managedMode == 2) {
        bool dxt = f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
        UINT cw = w, ch = h;
        HRESULT hr = oCreateTexture(d, cw, ch, l, u, f, D3DPOOL_DEFAULT, t, sh);
        if (FAILED(hr) && dxt && ((w | h) & 3)) { cw = (w + 3) & ~3u; ch = (h + 3) & ~3u; hr = oCreateTexture(d, cw, ch, l, u, f, D3DPOOL_DEFAULT, t, sh); }
        IDirect3DTexture9* sys = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(oCreateTexture(d, cw, ch, (u & D3DUSAGE_AUTOGENMIPMAP) ? 1 : (*t)->GetLevelCount(), 0, f, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys) {   // autogen: the copy holds level 0, the runtime builds the rest
            void** v = VT(*t);
            Hook(v[2], (void*)hTexRelease, &oTexRelease, "Texture::Release");
            Hook(v[9], (void*)hMtPreLoad, &oTexPreLoad, "Texture::PreLoad");
            Hook(v[18], (void*)hMtSurf, &oTexSurf, "Texture::GetSurfaceLevel");
            Hook(v[19], (void*)hTexLock, &oTexLock, "Texture::LockRect");
            Hook(v[20], (void*)hTexUnlock, &oTexUnlock, "Texture::UnlockRect");
            Hook(v[21], (void*)hTexDirty, &oTexDirty, "Texture::AddDirtyRect");
            EnterCriticalSection(&g_shadowCs); g_mt[*t] = MTex{ sys, 0 }; LeaveCriticalSection(&g_shadowCs);
            InterlockedIncrement(&g_mtCount);
            g_convTex++; LodTrack(*t, false);
            return hr;
        }
        if (SUCCEEDED(hr) && *t) { (*t)->Release(); *t = nullptr; }
        static int nf = 0; if (nf++ < 5) LOGW("d3d9: managed texture %ux%u fmt %08x: exact emulation failed (%08lx) - dynamic texture instead", w, h, (unsigned)f, hr);
    }
    if (p == D3DPOOL_MANAGED) {
        HRESULT hr = oCreateTexture(d, w, h, l, u | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, t, sh);
        if (FAILED(hr)) hr = oCreateTexture(d, w, h, l, u, f, D3DPOOL_DEFAULT, t, sh);
        bool dxt = f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
        if (FAILED(hr) && dxt && ((w | h) & 3)) {
            // video memory needs DXT sizes in multiples of 4 (the managed pool hides that). Rounding up keeps the same
            // block layout at every mip level (a 2x16 and a 4x16 DXT texture are both one block wide), so the game's
            // locked writes land where it expects them.
            UINT w4 = (w + 3) & ~3u, h4 = (h + 3) & ~3u;
            hr = oCreateTexture(d, w4, h4, l, u | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, t, sh);
            if (FAILED(hr)) hr = oCreateTexture(d, w4, h4, l, u, f, D3DPOOL_DEFAULT, t, sh);
            static int n = 0; if (n++ < 5) LOGI("d3d9: managed DXT texture %ux%u created as %ux%u (%08lx)", w, h, w4, h4, hr);
            if (SUCCEEDED(hr)) { w = w4; h = h4; }
        }
        bool dyn = false;
        if (SUCCEEDED(hr)) { D3DSURFACE_DESC dd = {}; if (SUCCEEDED((*t)->GetLevelDesc(0, &dd))) dyn = (dd.Usage & D3DUSAGE_DYNAMIC) != 0; }
        if (SUCCEEDED(hr) && dyn && (g_directLock || g_managedMode == 2)) {
            // managed_direct_lock (0.6.1): the video-memory texture is dynamic, so the game and D3DX lock it directly -
            // one copy of every level, written by whoever writes it, and no system-memory duplicate in the 32-bit process
            g_convTex++; g_directTex++;
            LodTrack(*t, false);
            return hr;
        }
        if (SUCCEEDED(hr)) {
            g_convTex++;
            LodTrack(*t, false);
            IDirect3DTexture9* s = nullptr;
            if (!(u & D3DUSAGE_AUTOGENMIPMAP) &&
                SUCCEEDED(oCreateTexture(d, w, h, (*t)->GetLevelCount(), 0, f, D3DPOOL_SYSTEMMEM, &s, nullptr)) && s) {
                void** v = VT(*t);
                Hook(v[19], (void*)hTexLock, &oTexLock, "Texture::LockRect");
                Hook(v[20], (void*)hTexUnlock, &oTexUnlock, "Texture::UnlockRect");
                Hook(v[21], (void*)hTexDirty, &oTexDirty, "Texture::AddDirtyRect");
                Hook(v[2], (void*)hTexRelease, &oTexRelease, "Texture::Release");
                EnterCriticalSection(&g_shadowCs);
                g_shadow[*t] = s;
                LeaveCriticalSection(&g_shadowCs);
            } else { g_shadowFallback++; if (g_shadowFallback < 5) LOGW("d3d9: managed texture %ux%u fmt %08x has no system-memory copy", w, h, (unsigned)f); }
            return hr;
        }
        LOGW("d3d9: managed texture %ux%u fmt %08x could not be created in video memory (%08lx)", w, h, (unsigned)f, hr);
        return hr;
    }
    HRESULT hr = oCreateTexture(d, w, h, l, u, f, p, t, sh);
    if (SUCCEEDED(hr) && t && *t) LodForget(*t);
    return hr;
}

// ---------------------------------------------------------------- 0.9.5: exact managed emulation for volume textures
struct MVol { IDirect3DVolumeTexture9* sys; volatile LONG dirty; };
static std::unordered_map<IDirect3DBaseTexture9*, MVol> g_mv; volatile LONG g_mvCount = 0;
typedef HRESULT (STDMETHODCALLTYPE* VolLockFn)(IDirect3DVolumeTexture9*, UINT, D3DLOCKED_BOX*, const D3DBOX*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* VolUnlockFn)(IDirect3DVolumeTexture9*, UINT);
typedef HRESULT (STDMETHODCALLTYPE* VolLevelFn)(IDirect3DVolumeTexture9*, UINT, IDirect3DVolume9**);
typedef HRESULT (STDMETHODCALLTYPE* VolDirtyFn)(IDirect3DVolumeTexture9*, const D3DBOX*);
typedef ULONG (STDMETHODCALLTYPE* VolReleaseFn)(IDirect3DVolumeTexture9*);
static VolLockFn oVolLock; static VolUnlockFn oVolUnlock; static VolLevelFn oVolLevel; static VolDirtyFn oVolDirty; static VolReleaseFn oVolRelease;
static IDirect3DVolumeTexture9* MvSys(IDirect3DBaseTexture9* t, bool dirty) {
    if (!g_mvCount) return nullptr;
    IDirect3DVolumeTexture9* r = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_mv.find(t); if (it != g_mv.end()) { r = it->second.sys; if (dirty) it->second.dirty = 1; }
    LeaveCriticalSection(&g_shadowCs);
    return r;
}
static HRESULT STDMETHODCALLTYPE hVolLock(IDirect3DVolumeTexture9* t, UINT l, D3DLOCKED_BOX* b, const D3DBOX* bx, DWORD f) {
    if (IDirect3DVolumeTexture9* s = MvSys(t, false)) return s->LockBox(l, b, bx, f & (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NO_DIRTY_UPDATE));
    return oVolLock(t, l, b, bx, f);
}
static HRESULT STDMETHODCALLTYPE hVolUnlock(IDirect3DVolumeTexture9* t, UINT l) {
    if (IDirect3DVolumeTexture9* s = MvSys(t, false)) { HRESULT hr = s->UnlockBox(l); MvSys(t, true); return hr; }
    return oVolUnlock(t, l);
}
static HRESULT STDMETHODCALLTYPE hVolLevel(IDirect3DVolumeTexture9* t, UINT l, IDirect3DVolume9** out) {
    if (!g_inOwn) if (IDirect3DVolumeTexture9* s = MvSys(t, true)) return s->GetVolumeLevel(l, out);
    return oVolLevel(t, l, out);
}
static HRESULT STDMETHODCALLTYPE hVolDirty(IDirect3DVolumeTexture9* t, const D3DBOX* bx) {
    if (IDirect3DVolumeTexture9* s = MvSys(t, true)) return s->AddDirtyBox(bx);
    return oVolDirty(t, bx);
}
static ULONG STDMETHODCALLTYPE hVolRelease(IDirect3DVolumeTexture9* t) {
    ULONG n = oVolRelease(t);
    if (n == 0) {
        IDirect3DVolumeTexture9* s = nullptr;
        EnterCriticalSection(&g_shadowCs);
        auto it = g_mv.find(t); if (it != g_mv.end()) { s = it->second.sys; g_mv.erase(it); InterlockedDecrement(&g_mvCount); }
        LeaveCriticalSection(&g_shadowCs);
        if (s) s->Release();
    }
    return n;
}
static void MvUpload(IDirect3DDevice9* d, IDirect3DBaseTexture9* t) {
    IDirect3DVolumeTexture9* s = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_mv.find(t); if (it != g_mv.end() && it->second.dirty) { s = it->second.sys; it->second.dirty = 0; }
    LeaveCriticalSection(&g_shadowCs);
    if (!s) return;
    s->AddDirtyBox(nullptr);
    InterlockedIncrement(&g_inOwn); d->UpdateTexture(s, t); InterlockedDecrement(&g_inOwn);
}
static HRESULT STDMETHODCALLTYPE hCreateVolTex(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* sh) {
    if (!g_cfgEx) return oCreateVolTex(d, w, h, dp, l, u, f, p, t, sh);   // a plain D3D9 device keeps its managed pool
    if (p == D3DPOOL_MANAGED && g_managedMode == 2) {
        HRESULT hr = oCreateVolTex(d, w, h, dp, l, u, f, D3DPOOL_DEFAULT, t, sh);
        IDirect3DVolumeTexture9* sys = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(oCreateVolTex(d, w, h, dp, (*t)->GetLevelCount(), 0, f, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys) {
            void** v = VT(*t);
            Hook(v[2], (void*)hVolRelease, &oVolRelease, "VolumeTexture::Release");
            Hook(v[18], (void*)hVolLevel, &oVolLevel, "VolumeTexture::GetVolumeLevel");
            Hook(v[19], (void*)hVolLock, &oVolLock, "VolumeTexture::LockBox");
            Hook(v[20], (void*)hVolUnlock, &oVolUnlock, "VolumeTexture::UnlockBox");
            Hook(v[21], (void*)hVolDirty, &oVolDirty, "VolumeTexture::AddDirtyBox");
            EnterCriticalSection(&g_shadowCs); g_mv[*t] = MVol{ sys, 0 }; LeaveCriticalSection(&g_shadowCs);
            InterlockedIncrement(&g_mvCount); g_convTex++;
            static int n = 0; if (n++ < 4) LOGI("d3d9: managed volume texture %ux%ux%u fmt %u: video memory + system-memory copy", w, h, dp, (unsigned)f);
            return hr;
        }
        if (SUCCEEDED(hr) && *t) { (*t)->Release(); *t = nullptr; }
    }
    if (p == D3DPOOL_MANAGED) {
        HRESULT hr = oCreateVolTex(d, w, h, dp, l, u | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, t, sh);
        if (SUCCEEDED(hr)) { g_convTex++; return hr; }
        return oCreateVolTex(d, w, h, dp, l, u, f, D3DPOOL_DEFAULT, t, sh);
    }
    return oCreateVolTex(d, w, h, dp, l, u, f, p, t, sh);
}
// ---------------------------------------------------------------- 0.9.3: managed CUBE textures done properly
// Until now a managed cube texture became a D3DUSAGE_DYNAMIC one that the game / D3DX locked face by face, level by
// level. A lock of a dynamic texture may hand out fresh storage for the whole texture, so after filling 6 faces x N
// levels only part of it holds data. The game builds a cube with D3DXFillCubeTexture at start-up (exe 0x53E99E /
// 0x53EEF3 - a vector lookup cube used by its shaders); with holes in it, vectors looked up through it come back as
// zero: the empty G-buffer normals. Now: an ordinary video-memory cube plus a system-memory copy that takes every
// lock (texture or face surface) and is uploaded whole before the cube is next used - what the managed pool does.
struct CubeShadow { IDirect3DCubeTexture9* sys; bool dirty; };
static std::unordered_map<IDirect3DBaseTexture9*, CubeShadow> g_cube;
volatile LONG g_cubeCount = 0; static unsigned g_cubeUploads = 0, g_cubeLocks = 0; static bool g_cubeShadowOn = true;
typedef HRESULT (STDMETHODCALLTYPE* CubeLockFn)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* CubeUnlockFn)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT);
typedef HRESULT (STDMETHODCALLTYPE* CubeSurfFn)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, IDirect3DSurface9**);
typedef HRESULT (STDMETHODCALLTYPE* CubeDirtyFn)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, const RECT*);
static CubeLockFn oCubeLock; static CubeUnlockFn oCubeUnlock; static CubeSurfFn oCubeSurf; static CubeDirtyFn oCubeDirty;
static IDirect3DCubeTexture9* CubeSys(IDirect3DBaseTexture9* t, bool markDirty) {
    if (!g_cubeCount) return nullptr;
    IDirect3DCubeTexture9* r = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_cube.find(t); if (it != g_cube.end()) { r = it->second.sys; if (markDirty) it->second.dirty = true; }
    LeaveCriticalSection(&g_shadowCs);
    return r;
}
static HRESULT STDMETHODCALLTYPE hCubeLock(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT l, D3DLOCKED_RECT* r, const RECT* rc, DWORD fl) {
    if (IDirect3DCubeTexture9* s = CubeSys(t, !(fl & D3DLOCK_READONLY))) { g_cubeLocks++; return s->LockRect(f, l, r, rc, fl & (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NO_DIRTY_UPDATE)); }
    return oCubeLock(t, f, l, r, rc, fl);
}
static HRESULT STDMETHODCALLTYPE hCubeUnlock(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT l) {
    if (IDirect3DCubeTexture9* s = CubeSys(t, false)) return s->UnlockRect(f, l);
    return oCubeUnlock(t, f, l);
}
static HRESULT STDMETHODCALLTYPE hCubeSurf(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, UINT l, IDirect3DSurface9** out) {
    if (!g_inOwn) if (IDirect3DCubeTexture9* s = CubeSys(t, true)) return s->GetCubeMapSurface(f, l, out);   // D3DX writes through face surfaces
    return oCubeSurf(t, f, l, out);
}
static HRESULT STDMETHODCALLTYPE hCubeDirty(IDirect3DCubeTexture9* t, D3DCUBEMAP_FACES f, const RECT* rc) {
    if (IDirect3DCubeTexture9* s = CubeSys(t, true)) return s->AddDirtyRect(f, rc);
    return oCubeDirty(t, f, rc);
}
static void CubeUploadIfDirty(IDirect3DDevice9* d, IDirect3DBaseTexture9* t) {
    IDirect3DCubeTexture9* s = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_cube.find(t); if (it != g_cube.end() && it->second.dirty) { s = it->second.sys; it->second.dirty = false; }
    LeaveCriticalSection(&g_shadowCs);
    if (!s) return;
    for (int f = 0; f < 6; f++) s->AddDirtyRect((D3DCUBEMAP_FACES)f, nullptr);
    HRESULT hr = d->UpdateTexture(s, t);
    g_cubeUploads++;
    if (FAILED(hr)) { static int n = 0; if (n++ < 5) LOGW("d3d9: cube texture upload failed %08lx", hr); }
}
static void CubeForget(IDirect3DBaseTexture9* t) {
    IDirect3DCubeTexture9* s = nullptr;
    EnterCriticalSection(&g_shadowCs);
    auto it = g_cube.find(t); if (it != g_cube.end()) { s = it->second.sys; g_cube.erase(it); InterlockedDecrement(&g_cubeCount); }
    LeaveCriticalSection(&g_shadowCs);
    if (s) s->Release();
}
static HRESULT STDMETHODCALLTYPE hCreateCubeTex(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* sh) {
    if (!g_cfgEx) return oCreateCubeTex(d, e, l, u, f, p, t, sh);
    if (p == D3DPOOL_MANAGED) {
        if (g_cubeShadowOn && !(u & D3DUSAGE_AUTOGENMIPMAP)) {
            HRESULT hr = oCreateCubeTex(d, e, l, u, f, D3DPOOL_DEFAULT, t, sh);
            IDirect3DCubeTexture9* sys = nullptr;
            if (SUCCEEDED(hr) && SUCCEEDED(oCreateCubeTex(d, e, (*t)->GetLevelCount(), 0, f, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys) {
                g_convTex++; LodTrack(*t, true);
                void** v = VT(*t);
                Hook(v[18], (void*)hCubeSurf, &oCubeSurf, "CubeTexture::GetCubeMapSurface");
                Hook(v[19], (void*)hCubeLock, &oCubeLock, "CubeTexture::LockRect");
                Hook(v[20], (void*)hCubeUnlock, &oCubeUnlock, "CubeTexture::UnlockRect");
                Hook(v[21], (void*)hCubeDirty, &oCubeDirty, "CubeTexture::AddDirtyRect");
                EnterCriticalSection(&g_shadowCs); g_cube[*t] = CubeShadow{ sys, false }; LeaveCriticalSection(&g_shadowCs);
                InterlockedIncrement(&g_cubeCount);
                static int n = 0; if (n++ < 4) LOGI("d3d9: managed cube texture %u px, %lu levels, fmt %u: video memory + system-memory copy", e, (unsigned long)(*t)->GetLevelCount(), (unsigned)f);
                return hr;
            }
            if (SUCCEEDED(hr) && *t) { (*t)->Release(); *t = nullptr; }
        }
        HRESULT hr = oCreateCubeTex(d, e, l, u | D3DUSAGE_DYNAMIC, f, D3DPOOL_DEFAULT, t, sh);
        if (SUCCEEDED(hr)) { g_convTex++; LodTrack(*t, true); return hr; }
        return oCreateCubeTex(d, e, l, u, f, D3DPOOL_DEFAULT, t, sh);
    }
    HRESULT hr = oCreateCubeTex(d, e, l, u, f, p, t, sh);
    if (SUCCEEDED(hr) && t && *t) LodForget(*t);
    return hr;
}
// ---------------------------------------------------------------- 1.0.2: managed vertex / index buffers without GPU stalls
// D3D9Ex has no managed pool, so the game's managed buffers became plain video-memory buffers. Locking one of those
// while the GPU still draws from it makes the CPU WAIT for the GPU (the managed pool never does: it writes a system-memory
// copy). Witcher 2 rewrites some managed buffers every frame in combat - blood, hit effects, damage numbers / animated
// HUD pieces - and each such lock stalled the frame: 14 - 26 fps with the GPU only 12 ms busy.
// Now: a buffer is locked straight as before until it has been drawn ("cold": no stall possible). The first write-lock
// after it has been drawn makes it "hot": it gets a system-memory copy that takes all locks (never waits) and a dynamic
// twin that is refilled with DISCARD (fresh memory, never waits) before the next draw and bound in its place.
struct MBuf { bool ib = false; UINT size = 0; DWORD usage = 0, fmt = 0; bool used = false, hot = false, dirty = false, noHot = false, needWhole = true;
               UINT dMin = 0, dMax = 0, uMin = 0, uMax = 0; unsigned discFrame = 0, discN = 0;
               bool rshadow = false; UINT wO = 0, wN = 0; bool wOpen = false;
               uint8_t* shadow = nullptr; IDirect3DResource9* twin = nullptr; };
// 1.0.3: refills copy only what was written. A written range the GPU has not read since the twin's last DISCARD goes in
// with NOOVERWRITE (no copy of the rest, no new memory); only an overlap takes a DISCARD and a whole copy. 1.0.2 copied
// the whole buffer with DISCARD on every refill: a large buffer rewritten in many small pieces per frame (combat
// effects) meant hundreds of MB copied and renamed per frame - slower, and the driver ran short of memory.
static std::unordered_map<void*, MBuf> g_mb; static CRITICAL_SECTION g_mbCs; static bool g_mbCsInit = false;
static std::vector<void*> g_mbDirty; volatile LONG g_mbAnyDirty = 0;
static bool g_mbExact = true; static thread_local int t_mbOwn = 0;   // the mod creating / filling its own twins (this thread only)
static size_t g_mbShadowBytes = 0; static const size_t kMbShadowMax = 256u << 20; static unsigned g_mbRShadows = 0, g_mbRLocks = 0;
static unsigned g_mbPartialWo = 0, g_mbWaits = 0; static double g_mbWaitMs = 0; static unsigned g_mbHot = 0, g_mbUploads = 0, g_mbReadbacks = 0, g_mbLocks = 0; static double g_mbUploadMB = 0, g_mbSlowMs = 0;
static UINT g_strOff[16] = {}, g_strStride[16] = {}; static bool g_strTwin[16] = {};
static bool g_idxTwin = false;
typedef HRESULT (STDMETHODCALLTYPE* BufLockFn)(void*, UINT, UINT, void**, DWORD);
typedef HRESULT (STDMETHODCALLTYPE* BufUnlockFn)(void*);
typedef ULONG (STDMETHODCALLTYPE* BufReleaseFn)(void*);
static BufLockFn oVbLock, oIbLock; static BufUnlockFn oVbUnlock, oIbUnlock; static BufReleaseFn oVbRelease, oIbRelease;
typedef HRESULT (STDMETHODCALLTYPE* SetStrmFn)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* SetIdxFn)(IDirect3DDevice9*, IDirect3DIndexBuffer9*);
static SetStrmFn oSetStrm; static SetIdxFn oSetIdx;
static double MbNowMs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
static MBuf* MbFind(void* b) { auto it = g_mb.find(b); return it == g_mb.end() ? nullptr : &it->second; }
static bool MbMakeHot(void* b, MBuf& m, bool needOld) {
    IDirect3DDevice9* d = g_dev; if (!d) return false;
    if (m.rshadow && m.shadow) { needOld = false; m.rshadow = false; g_mbShadowBytes -= m.size; }   // the read copy is already current
    else { m.shadow = (uint8_t*)malloc(m.size ? m.size : 1); if (!m.shadow) return false; memset(m.shadow, 0, m.size); }
    if (needOld) {   // a partial write: the rest of the buffer must survive - read it once (one wait, then never again)
        void* p = nullptr; double t0 = MbNowMs();
        BufLockFn lk = m.ib ? oIbLock : oVbLock; BufUnlockFn ul = m.ib ? oIbUnlock : oVbUnlock;
        if (SUCCEEDED(lk(b, 0, 0, &p, D3DLOCK_READONLY)) && p) { memcpy(m.shadow, p, m.size); ul(b); g_mbReadbacks++; }
        g_mbSlowMs += MbNowMs() - t0;
    }
    HRESULT hr;
    DWORD use = (m.usage & ~(DWORD)D3DUSAGE_WRITEONLY) | D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
    t_mbOwn++;
    if (m.ib) hr = d->CreateIndexBuffer(m.size, use, (D3DFORMAT)m.fmt, D3DPOOL_DEFAULT, (IDirect3DIndexBuffer9**)&m.twin, nullptr);
    else hr = d->CreateVertexBuffer(m.size, use, m.fmt, D3DPOOL_DEFAULT, (IDirect3DVertexBuffer9**)&m.twin, nullptr);
    t_mbOwn--;
    if (FAILED(hr) || !m.twin) { free(m.shadow); m.shadow = nullptr; m.twin = nullptr; return false; }
    m.hot = true; g_mbHot++;
    return true;
}
static HRESULT BufLock(bool ib, void* b, UINT o, UINT n, void** pp, DWORD f) {
    BufLockFn lk = ib ? oIbLock : oVbLock;
    if (t_mbOwn || !pp) return lk(b, o, n, pp, f);
    EnterCriticalSection(&g_mbCs);
    MBuf* m = MbFind(b);
    if (m && !m->hot && !m->noHot && m->used && !(f & D3DLOCK_READONLY) && g_mbExact) {
        bool whole = o == 0 && (n == 0 || n >= m->size);
        // a partial write to a write-only buffer: its old contents cannot be read back reliably - left as it was
        if (!whole && (m->usage & D3DUSAGE_WRITEONLY)) { g_mbPartialWo++; m = nullptr; }
        else if (!MbMakeHot(b, *m, !whole)) m = nullptr;
    }
    // 1.0.5: a buffer the game READS (blood / hit decals are cut out of the meshes they land on: the game reads those
    // meshes' vertices and indices) gets a system-memory copy, as the managed pool kept. Read straight from video
    // memory, every vertex fetch went over the bus from uncached memory - combat with blood fell to 30 fps.
    if (m && !m->hot && !m->rshadow && (f & D3DLOCK_READONLY) && g_mbExact && g_mbShadowBytes + m->size <= kMbShadowMax) {
        uint8_t* sh = (uint8_t*)malloc(m->size ? m->size : 1);
        void* p = nullptr;
        if (sh) {
            BufUnlockFn ul = ib ? oIbUnlock : oVbUnlock;
            double t0 = MbNowMs();
            if (SUCCEEDED(lk(b, 0, 0, &p, D3DLOCK_READONLY)) && p) { memcpy(sh, p, m->size); ul(b); m->shadow = sh; m->rshadow = true; g_mbShadowBytes += m->size; g_mbRShadows++; }
            else free(sh);
            g_mbSlowMs += MbNowMs() - t0;
        }
    }
    if (m && m->rshadow && !m->hot) {
        if (o > m->size) o = 0;
        *pp = m->shadow + o; g_mbRLocks++;
        if (!(f & D3DLOCK_READONLY)) { m->wOpen = true; m->wO = o; m->wN = (n == 0 || o + n > m->size) ? m->size - o : n; }
        LeaveCriticalSection(&g_mbCs);
        return D3D_OK;
    }
    if (m && m->hot) {
        if (o > m->size) o = 0;
        *pp = m->shadow + o; g_mbLocks++;
        if (!(f & D3DLOCK_READONLY)) {
            UINT e = (n == 0 || o + n > m->size) ? m->size : o + n;
            if (!m->dirty) { m->dirty = true; m->dMin = o; m->dMax = e; g_mbDirty.push_back(b); g_mbAnyDirty = 1; }
            else { if (o < m->dMin) m->dMin = o; if (e > m->dMax) m->dMax = e; }
        }
        LeaveCriticalSection(&g_mbCs);
        return D3D_OK;
    }
    bool timed = m && m->used;   // drawn before and still locked directly: this lock may wait for the GPU
    LeaveCriticalSection(&g_mbCs);
    if (!timed) return lk(b, o, n, pp, f);   // cold: never drawn yet, nothing to wait for
    double t0 = MbNowMs(); HRESULT hr = lk(b, o, n, pp, f); double ms = MbNowMs() - t0;
    g_mbWaits++; g_mbWaitMs += ms;
    return hr;
}
static HRESULT BufUnlock(bool ib, void* b) {
    if (!t_mbOwn && g_mbCsInit) {
        EnterCriticalSection(&g_mbCs); MBuf* m = MbFind(b); bool hot = m && m->hot;
        if (m && m->rshadow && !hot) {   // a write into the copy: the written range goes to the video-memory buffer too
            if (m->wOpen && m->wN) {
                void* p = nullptr; BufLockFn lk = ib ? oIbLock : oVbLock; BufUnlockFn ul = ib ? oIbUnlock : oVbUnlock;
                if (SUCCEEDED(lk(b, m->wO, m->wN, &p, 0)) && p) { memcpy(p, m->shadow + m->wO, m->wN); ul(b); }
            }
            m->wOpen = false;
            LeaveCriticalSection(&g_mbCs);
            return D3D_OK;
        }
        LeaveCriticalSection(&g_mbCs);
        if (hot) return D3D_OK;
    }
    return (ib ? oIbUnlock : oVbUnlock)(b);
}
static ULONG BufRelease(bool ib, void* b) {
    ULONG n = (ib ? oIbRelease : oVbRelease)(b);
    if (n == 0 && g_mbCsInit) {
        EnterCriticalSection(&g_mbCs);
        auto it = g_mb.find(b);
        if (it != g_mb.end()) {
            if (it->second.rshadow) g_mbShadowBytes -= it->second.size;
            if (it->second.shadow) free(it->second.shadow);
            if (it->second.twin) it->second.twin->Release();
            g_mb.erase(it);
            for (auto& x : g_mbDirty) if (x == b) x = nullptr;
        }
        LeaveCriticalSection(&g_mbCs);
    }
    return n;
}
static HRESULT STDMETHODCALLTYPE hVbLock(void* b, UINT o, UINT n, void** p, DWORD f) { return BufLock(false, b, o, n, p, f); }
static HRESULT STDMETHODCALLTYPE hIbLock(void* b, UINT o, UINT n, void** p, DWORD f) { return BufLock(true, b, o, n, p, f); }
static HRESULT STDMETHODCALLTYPE hVbUnlock(void* b) { return BufUnlock(false, b); }
static HRESULT STDMETHODCALLTYPE hIbUnlock(void* b) { return BufUnlock(true, b); }
static ULONG STDMETHODCALLTYPE hVbRelease(void* b) { return BufRelease(false, b); }
static ULONG STDMETHODCALLTYPE hIbRelease(void* b) { return BufRelease(true, b); }
// before a draw: refill the twins of buffers written since, and bind twins wherever their buffer is bound
static unsigned g_mbFrame = 0, g_mbDiscards = 0, g_mbPartials = 0, g_mbDemoted = 0; static double g_mbFrameMB = 0, g_mbMaxFrameMB = 0;
// a buffer that still needs a whole DISCARD copy many times a frame goes back to the direct path (as before 1.0.2)
static void MbDemote(IDirect3DDevice9* d, void* b, MBuf& m) {
    BufLockFn lk = m.ib ? oIbLock : oVbLock; BufUnlockFn ul = m.ib ? oIbUnlock : oVbUnlock;
    void* p = nullptr;
    if (SUCCEEDED(lk(b, 0, 0, &p, 0)) && p) { memcpy(p, m.shadow, m.size); ul(b); }
    for (int s = 0; s < 16; s++) if (g_strOrig[s] == b && g_strTwin[s]) { oSetStrm(d, s, (IDirect3DVertexBuffer9*)b, g_strOff[s], g_strStride[s]); g_strTwin[s] = false; }
    if (g_idxOrig == b && g_idxTwin) { oSetIdx(d, (IDirect3DIndexBuffer9*)b); g_idxTwin = false; }
    free(m.shadow); m.shadow = nullptr; m.twin->Release(); m.twin = nullptr;
    m.hot = false; m.noHot = true; m.dirty = false; g_mbDemoted++;
}
static void MbUpload(IDirect3DDevice9* d) {
    if (!g_mbAnyDirty) return;
    g_mbAnyDirty = 0;
    EnterCriticalSection(&g_mbCs);
    t_mbOwn++;
    for (void* b : g_mbDirty) {
        if (!b) continue;
        MBuf* m = MbFind(b); if (!m || !m->hot || !m->dirty) continue;
        bool overlap = m->uMax > m->uMin && m->dMin < m->uMax && m->dMax > m->uMin;
        bool whole = m->needWhole || overlap || m->dMax <= m->dMin;
        if (whole) {
            if (m->discFrame != g_mbFrame) { m->discFrame = g_mbFrame; m->discN = 0; }
            if (++m->discN > 8 && m->size > 512 * 1024) { MbDemote(d, b, *m); continue; }
        }
        UINT o = whole ? 0 : m->dMin, n = whole ? m->size : m->dMax - m->dMin;
        DWORD fl = whole ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE;
        void* p = nullptr;
        HRESULT hr = m->ib ? ((IDirect3DIndexBuffer9*)m->twin)->Lock(o, whole ? 0 : n, &p, fl) : ((IDirect3DVertexBuffer9*)m->twin)->Lock(o, whole ? 0 : n, &p, fl);
        if (SUCCEEDED(hr) && p) {
            memcpy(p, m->shadow + o, n);
            if (m->ib) ((IDirect3DIndexBuffer9*)m->twin)->Unlock(); else ((IDirect3DVertexBuffer9*)m->twin)->Unlock();
            g_mbUploads++; g_mbUploadMB += n / 1048576.0; g_mbFrameMB += n / 1048576.0;
            if (whole) { g_mbDiscards++; m->uMin = m->uMax = 0; m->needWhole = false; } else g_mbPartials++;
        }
        m->dirty = false;
    }
    g_mbDirty.clear();
    for (int s = 0; s < 16; s++) if (g_strOrig[s] && !g_strTwin[s]) { MBuf* m = MbFind(g_strOrig[s]); if (m && m->hot) { oSetStrm(d, s, (IDirect3DVertexBuffer9*)m->twin, g_strOff[s], g_strStride[s]); g_strTwin[s] = true; } }
    if (g_idxOrig && !g_idxTwin) { MBuf* m = MbFind(g_idxOrig); if (m && m->hot) { oSetIdx(d, (IDirect3DIndexBuffer9*)m->twin); g_idxTwin = true; } }
    t_mbOwn--;
    LeaveCriticalSection(&g_mbCs);
}
// after a draw: which bytes of the bound twins the GPU may now be reading (until their next DISCARD)
static DWORD g_strFreq[16] = {};
static void MbUse(MBuf* m, INT64 a, INT64 e) {
    if (!m) return;
    if (a < 0) a = 0;
    if (e > (INT64)m->size || e <= a) { a = 0; e = m->size; }
    if (m->uMax <= m->uMin) { m->uMin = (UINT)a; m->uMax = (UINT)e; }
    else { if ((UINT)a < m->uMin) m->uMin = (UINT)a; if ((UINT)e > m->uMax) m->uMax = (UINT)e; }
}
static void MbNoteDraw(INT64 firstVert, INT64 numVerts, INT64 firstIdx, INT64 numIdx) {
    if (!g_mbCsInit) return;
    bool any = g_idxTwin; for (int s = 0; s < 16 && !any; s++) any = g_strTwin[s];
    if (!any) return;
    EnterCriticalSection(&g_mbCs);
    for (int s = 0; s < 16; s++) {
        if (!g_strTwin[s] || !g_strOrig[s]) continue;
        MBuf* m = MbFind(g_strOrig[s]); if (!m || !m->hot) continue;
        UINT st = g_strStride[s];
        if (!st || (g_strFreq[s] & 0x80000000u) || numVerts <= 0) MbUse(m, 0, m->size);   // instance data / unknown: all of it
        else MbUse(m, (INT64)g_strOff[s] + firstVert * st, (INT64)g_strOff[s] + (firstVert + numVerts) * st);
    }
    if (g_idxTwin && g_idxOrig && numIdx > 0) {
        MBuf* m = MbFind(g_idxOrig);
        if (m && m->hot) { int isz = m->fmt == D3DFMT_INDEX32 ? 4 : 2; MbUse(m, firstIdx * isz, (firstIdx + numIdx) * isz); }
    }
    LeaveCriticalSection(&g_mbCs);
}
static INT64 PrimVerts(D3DPRIMITIVETYPE t, UINT pc) {
    switch (t) { case D3DPT_POINTLIST: return pc; case D3DPT_LINELIST: return 2LL * pc; case D3DPT_LINESTRIP: return pc + 1LL;
                 case D3DPT_TRIANGLELIST: return 3LL * pc; case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return pc + 2LL; default: return 0; }
}
typedef HRESULT (STDMETHODCALLTYPE* SetFreqFn)(IDirect3DDevice9*, UINT, UINT);
static SetFreqFn oSetFreq;
static HRESULT STDMETHODCALLTYPE hSetFreq(IDirect3DDevice9* d, UINT s, UINT f) { if (s < 16 && !g_inOwn) g_strFreq[s] = f; return oSetFreq(d, s, f); }
static HRESULT STDMETHODCALLTYPE hSetStrm(IDirect3DDevice9* d, UINT s, IDirect3DVertexBuffer9* vb, UINT off, UINT stride) {
    if (g_inOwn || s >= 16 || !g_mbCsInit) return oSetStrm(d, s, vb, off, stride);
    g_strOrig[s] = vb; g_strOff[s] = off; g_strStride[s] = stride; g_strTwin[s] = false;
    if (vb) {
        EnterCriticalSection(&g_mbCs);
        MBuf* m = MbFind(vb); IDirect3DVertexBuffer9* use = vb;
        if (m) { m->used = true; if (m->hot) { use = (IDirect3DVertexBuffer9*)m->twin; g_strTwin[s] = true; if (m->dirty) g_mbAnyDirty = 1; } }
        LeaveCriticalSection(&g_mbCs);
        return oSetStrm(d, s, use, off, stride);
    }
    return oSetStrm(d, s, vb, off, stride);
}
static HRESULT STDMETHODCALLTYPE hSetIdx(IDirect3DDevice9* d, IDirect3DIndexBuffer9* ib) {
    if (g_inOwn || !g_mbCsInit) return oSetIdx(d, ib);
    g_idxOrig = ib; g_idxTwin = false;
    if (ib) {
        EnterCriticalSection(&g_mbCs);
        MBuf* m = MbFind(ib); IDirect3DIndexBuffer9* use = ib;
        if (m) { m->used = true; if (m->hot) { use = (IDirect3DIndexBuffer9*)m->twin; g_idxTwin = true; if (m->dirty) g_mbAnyDirty = 1; } }
        LeaveCriticalSection(&g_mbCs);
        return oSetIdx(d, use);
    }
    return oSetIdx(d, ib);
}
static void MbRegister(void* b, bool ib, UINT size, DWORD usage, DWORD fmt) {
    if (!b) return;
    if (!g_mbCsInit) { InitializeCriticalSection(&g_mbCs); g_mbCsInit = true; }
    void** v = VT(b);
    if (ib) { if (!oIbLock) { Hook(v[11], (void*)hIbLock, &oIbLock, "IndexBuffer::Lock"); Hook(v[12], (void*)hIbUnlock, &oIbUnlock, "IndexBuffer::Unlock"); Hook(v[2], (void*)hIbRelease, &oIbRelease, "IndexBuffer::Release"); } }
    else    { if (!oVbLock) { Hook(v[11], (void*)hVbLock, &oVbLock, "VertexBuffer::Lock"); Hook(v[12], (void*)hVbUnlock, &oVbUnlock, "VertexBuffer::Unlock"); Hook(v[2], (void*)hVbRelease, &oVbRelease, "VertexBuffer::Release"); } }
    EnterCriticalSection(&g_mbCs);
    MBuf m; m.ib = ib; m.size = size; m.usage = usage; m.fmt = fmt; g_mb[b] = m;
    LeaveCriticalSection(&g_mbCs);
}
unsigned g_mbFrameExt() { if (g_mbFrameMB > g_mbMaxFrameMB) g_mbMaxFrameMB = g_mbFrameMB; g_mbFrameMB = 0; return ++g_mbFrame; }
void D3D9SetBuffersDirect(bool direct) { static int last = -1; g_mbExact = !direct && ConfigStr("managed_buffers", "exact") != "direct"; if (last != (int)g_mbExact) { last = g_mbExact; LOGI("buffers: %s", g_mbExact ? "stall-free (buffers rewritten after drawing get a copy)" : "direct (Tests: locks may wait for the GPU)"); } }
void D3D9BufferReport() {
    size_t n = 0; if (g_mbCsInit) { EnterCriticalSection(&g_mbCs); n = g_mb.size(); LeaveCriticalSection(&g_mbCs); }
    LOGI("buffers: %u managed buffers, %u rewritten after drawing (hot, never wait): %u writes, %u refills (%.1f MB), %u one-time reads (%.1f ms), %u partial writes to write-only buffers left waiting",
         (unsigned)n, g_mbHot, g_mbLocks, g_mbUploads, g_mbUploadMB, g_mbReadbacks, g_mbSlowMs, g_mbPartialWo);
    LOGI("buffers: %u read from a system-memory copy (as the managed pool did; %u buffers copied so far, %.0f MB)", g_mbRLocks, g_mbRShadows, g_mbShadowBytes / 1048576.0);
    g_mbRLocks = 0;
    LOGI("buffers: %u direct locks of already-drawn buffers took %.1f ms in total%s; refills %u in place + %u whole, at most %.1f MB in one frame, %u buffers sent back to the direct path",
         g_mbWaits, g_mbWaitMs, g_mbExact ? "" : " (managed_buffers = direct)", g_mbPartials, g_mbDiscards, g_mbMaxFrameMB, g_mbDemoted);
    g_mbPartials = g_mbDiscards = 0; g_mbMaxFrameMB = 0;
    g_mbWaits = 0; g_mbWaitMs = 0;
    g_mbLocks = g_mbUploads = g_mbReadbacks = g_mbPartialWo = 0; g_mbUploadMB = g_mbSlowMs = 0;
}
static HRESULT STDMETHODCALLTYPE hCreateVB(IDirect3DDevice9* d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** b, HANDLE* sh) {
    if (!g_cfgEx || t_mbOwn) return oCreateVB(d, len, u, fvf, p, b, sh);
    bool man = p == D3DPOOL_MANAGED;
    if (man) { g_convBuf++; p = D3DPOOL_DEFAULT; }
    HRESULT hr = oCreateVB(d, len, u, fvf, p, b, sh);
    if (man && SUCCEEDED(hr) && b && *b) MbRegister(*b, false, len, u, fvf);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hCreateIB(IDirect3DDevice9* d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** b, HANDLE* sh) {
    if (!g_cfgEx || t_mbOwn) return oCreateIB(d, len, u, f, p, b, sh);
    bool man = p == D3DPOOL_MANAGED;
    if (man) { g_convBuf++; p = D3DPOOL_DEFAULT; }
    HRESULT hr = oCreateIB(d, len, u, f, p, b, sh);
    if (man && SUCCEEDED(hr) && b && *b) MbRegister(*b, true, len, u, (DWORD)f);
    return hr;
}

// ---------------------------------------------------------------- present / reset
// render size: the game renders at its window size (window.cpp tells it the size chosen here), so any aspect ratio and
// resolution works, including sizes the game's options do not offer
static float g_cfgAspect = 0, g_cfgRenderScale = 1.f; static UINT g_cfgMaxSide = 8192;
bool XrHeadsetEyeSize(unsigned* w, unsigned* h);
static bool g_cfgResHeadset = true;
static void RenderSize(UINT& w, UINT& h) {
    if (!w || !h) return;
    // the longer side: the headset's longer per-eye side (else the game's), x render_scale; the shape: render_aspect,
    // else the headset eye's, else the game's (same rule as w2ini.cpp)
    unsigned rw = 0, rh = 0;
    bool hs = g_cfgResHeadset && XrHeadsetEyeSize(&rw, &rh) && rw && rh;
    float aspect = g_cfgAspect > 0 ? g_cfgAspect : hs ? (float)rw / rh : (float)w / h;
    float side = (float)(hs ? (rh > rw ? rh : rw) : (h > w ? h : w)) * g_cfgRenderScale, fw, fh;
    if (aspect >= 1.f) { fw = side; fh = side / aspect; } else { fh = side; fw = side * aspect; }
    float mx = (float)g_cfgMaxSide;
    if (fw > mx) { fh *= mx / fw; fw = mx; }
    if (fh > mx) { fw *= mx / fh; fh = mx; }
    UINT nw = ((UINT)(fw + 2.f)) & ~3u, nh = ((UINT)(fh + 2.f)) & ~3u;
    if (nw != w || nh != h) LOGI("d3d9: render size %ux%u -> %ux%u (aspect %.3f, scale %.2f)", w, h, nw, nh, aspect, g_cfgRenderScale);
    w = nw; h = nh;
}
static void ApplyPresentParams(D3DPRESENT_PARAMETERS& p) {
    // The Witcher 2 sizes its own render targets from User.ini [Viewport] (w2ini.cpp writes the headset size there
    // before the game reads it), so the back buffer is normally left as the game asks; force_render_size overrides it
    if (g_cfgForceSize) RenderSize(p.BackBufferWidth, p.BackBufferHeight);
    if (g_cfgWindowed && !p.Windowed) { p.Windowed = TRUE; p.FullScreen_RefreshRateInHz = 0; }
    {   // windowed with size 0 = "the client rect": give the full resolution explicitly (never a monitor-capped rect);
        // a mismatch with what User.ini says is logged (the game's own size wins unless force_render_size)
        unsigned rw, rh;
        if (p.Windowed && W2IniResolution(&rw, &rh)) {
            if (!p.BackBufferWidth || !p.BackBufferHeight) { p.BackBufferWidth = rw; p.BackBufferHeight = rh; LOGI("d3d9: windowed back buffer size 0 -> %ux%u", rw, rh); }
            else if (p.BackBufferWidth != rw || p.BackBufferHeight != rh) LOGW("d3d9: the game asks for %ux%u, User.ini says %ux%u", p.BackBufferWidth, p.BackBufferHeight, rw, rh);
        }
    }
    if (g_cfgImmediate) p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    else if (p.Windowed && p.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) p.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
}
static void NoteBackBuffer(const D3DPRESENT_PARAMETERS& p) {
    g_bbW = p.BackBufferWidth; g_bbH = p.BackBufferHeight; g_rtW = g_vpW = g_bbW; g_rtH = g_vpH = g_bbH; g_rtFmt = p.BackBufferFormat;
    g_bbMsType = p.MultiSampleType; g_vpFull = { 0, 0, g_bbW, g_bbH, 0.f, 1.f };
    W2CamSetBackBuffer(g_bbW, g_bbH);
    LOGI("d3d9: back buffer %ux%u fmt %d x%u, %s, ms %d, interval %08lx", p.BackBufferWidth, p.BackBufferHeight, (int)p.BackBufferFormat,
         p.BackBufferCount, p.Windowed ? "windowed" : "fullscreen", (int)p.MultiSampleType, p.PresentationInterval);
}

static DWORD g_presentTid = 0;
static int g_presentDepth = 0;
static void OnPresent(IDirect3DDevice9* d) {
    if (g_presentDepth++ > 0) return;
    GpuTimeEnd();
    { extern unsigned g_mbFrameExt(); g_mbFrameExt(); }
    if (g_tstView) { InterlockedIncrement(&g_inOwn); TestShowLayer(d); InterlockedDecrement(&g_inOwn); }
    if (g_sdState == 2) {
        if (g_sdDraws) StageDumpBound(d);
        g_sdState = 0;
        LOGI("stages: frame %u recorded - %u pictures in W2VR\\stages (send that folder)", g_sdRun, g_sdSaved);
        GbufCensusReport();
    } else if (g_sdState == 1) {
        g_sdState = 2; g_sdSeq = 0; g_sdDraws = 0; g_sdSaved = 0; g_sdRun++;
        CreateDirectoryW((g_modDir + L"\\stages").c_str(), nullptr);
    }
    InterlockedIncrement(&g_inOwn);
    XrOnPresent(d);
    g_vr.presentCount++;   // the next frame: its camera update marks engineCamFrame
    InterlockedDecrement(&g_inOwn);
    // the game's next draws: the render target after Present is the back buffer
    g_rtW = g_vpW = g_bbW; g_rtH = g_vpH = g_bbH; g_rtIsBB = true; g_rtIsFinal = false; g_rtIsShadow = false; g_curRT0 = g_bbSurf;
    g_finalSrc = g_finalSrcNow; g_finalSrcNow = nullptr; g_finalComposited = false;
    if (g_finalSrc) g_finalFrames++;
    if (g_redirDraws) g_redirFrames++;
    g_redirDraws = 0;
}
void XrFrameLimit();
static LONGLONG g_cpuLastPresent = 0;
static void CpuFrameTick() { LONGLONG n = CpuNow(); if (g_cpuLastPresent) { double ms = CpuMs(g_cpuLastPresent, n); if (ms < 1000) { g_cpuFrameMs += ms; g_cpuFrames++; } } g_cpuLastPresent = n; }
static HRESULT STDMETHODCALLTYPE hPresent(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    CpuFrameTick();
    { HookTime ht_; OnPresent(d); }
    if (g_presentDepth == 1) { LONGLONG l0 = CpuNow(); XrFrameLimit(); g_cpuLimitMs += CpuMs(l0, CpuNow()); }
    LONGLONG p0 = CpuNow();
    HRESULT hr = oPresent(d, a, b, w, r);
    g_cpuPresentMs += CpuMs(p0, CpuNow());
    g_presentDepth = 0;
    if (hr == S_PRESENT_OCCLUDED) hr = D3D_OK;
    return hr;
}
static HRESULT STDMETHODCALLTYPE hSCPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    IDirect3DDevice9* d = nullptr;
    bool outer = g_presentDepth == 0;
    if (outer && SUCCEEDED(sc->GetDevice(&d)) && d) { if (d == g_dev) { CpuFrameTick(); { HookTime ht_; OnPresent(d); } LONGLONG l0 = CpuNow(); XrFrameLimit(); g_cpuLimitMs += CpuMs(l0, CpuNow()); } d->Release(); }
    LONGLONG p0 = CpuNow();
    HRESULT hr = oSCPresent(sc, a, b, w, r, f);
    if (outer) g_cpuPresentMs += CpuMs(p0, CpuNow());
    if (outer) g_presentDepth = 0;
    if (hr == S_PRESENT_OCCLUDED) hr = D3D_OK;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hReset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    InterlockedIncrement(&g_inOwn);
    XrOnLostDevice();
    MenuOnLostDevice();
    InterlockedDecrement(&g_inOwn);
    if (!pp) return oReset(d, pp);
    D3DPRESENT_PARAMETERS p = *pp;
    ApplyPresentParams(p);
    HRESULT hr = oReset(d, &p);
    LOGI("d3d9: Reset -> %08lx", hr);
    if (SUCCEEDED(hr)) {
        SSInitDefaults(); for (auto& m : g_stageMat) m = false;   // Reset puts every sampler state back to its default
        pp->BackBufferWidth = p.BackBufferWidth; pp->BackBufferHeight = p.BackBufferHeight;
        pp->BackBufferFormat = p.BackBufferFormat; pp->BackBufferCount = p.BackBufferCount;
        NoteBackBuffer(p);
        NoteBackBufferSurface(d);
        if (p.Windowed) WindowFitClient(p.hDeviceWindow, p.BackBufferWidth, p.BackBufferHeight);
    }
    g_curShader = nullptr;
    return hr;
}

// ---------------------------------------------------------------- depth of field off (cutscenes)
// AC II / Brotherhood / Revelations build their depth of field from one pixel shader (programs "DepthOfField" /
// "DepthOfFieldQuadratic", bytecode embedded in the exe) that mixes the sharp picture (sampler "Source"), the blurred
// one ("BlurredSource") and the depth ("DepthTexture"). In VR the blur sits on the eyes' focus. The shader is known
// by its constant table and replaced by one that passes "Source" through at the same coordinates - the post chain
// stays as it is, the picture is just sharp.
static const char* CtabFind(const DWORD* f, const char* want, int* reg) {
    // walk comment blocks; return non-null if a CTAB constant named `want` exists (its register index in *reg)
    if (!f || (f[0] & 0xFFFF0000) != 0xFFFF0000) return nullptr;
    for (const DWORD* t = f + 1; *t != 0x0000FFFF && t < f + 8192; ) {
        DWORD tok = *t;
        if ((tok & 0xFFFF) == 0xFFFE) {
            DWORD n = tok >> 16;
            const uint8_t* c = (const uint8_t*)(t + 1);
            if (n >= 8 && !memcmp(c, "CTAB", 4)) {
                const uint8_t* b = c + 4; size_t len = n * 4 - 4;
                uint32_t cnt, info; memcpy(&cnt, b + 12, 4); memcpy(&info, b + 16, 4);
                for (uint32_t k = 0; k < cnt && info + k * 20 + 20 <= len; k++) {
                    uint32_t nm; uint16_t rset, ridx; memcpy(&nm, b + info + k * 20, 4); memcpy(&rset, b + info + k * 20 + 4, 2); memcpy(&ridx, b + info + k * 20 + 6, 2);
                    if (nm < len && !strncmp((const char*)b + nm, want, len - nm) && strlen(want) == strnlen((const char*)b + nm, len - nm)) { if (reg) *reg = ridx; return want; }
                }
                return nullptr;
            }
            t += 1 + n; continue;
        }
        // instruction length (SM2+ tokens carry it in bits 24..27)
        t += 1 + ((tok >> 24) & 0xF);
    }
    return nullptr;
}
static bool IsDofShader(const DWORD* f, int* srcReg, int* coord) {
    if (!f || (f[0] & 0xFFFFFF00) != 0xFFFF0300) return false;   // ps_3_0
    int s = -1;
    if (!CtabFind(f, "Source", &s) || !CtabFind(f, "BlurredSource", nullptr) || !CtabFind(f, "DepthTexture", nullptr) ||
        !CtabFind(f, "g_blurConstants", nullptr) || !CtabFind(f, "g_PlaneConstants", nullptr) || s < 0 || s > 15) return false;
    // the input coordinate register: the first dcl_texcoord0 input (v#)
    int v = -1;
    for (const DWORD* t = f + 1; *t != 0x0000FFFF && t < f + 8192; ) {
        DWORD tok = *t;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + (tok >> 16); continue; }
        if ((tok & 0xFFFF) == 0x1F && (t[1] & 0x1F) == 5 && ((t[1] >> 16) & 0xF) == 0) {
            DWORD r = t[2]; int rt = (int)(((r >> 28) & 7) | ((r >> 8) & 0x18));
            if (rt == 1) { v = (int)(r & 0x7FF); break; }
        }
        t += 1 + ((tok >> 24) & 0xF);
    }
    if (v < 0 || v > 9) return false;
    *srcReg = s; *coord = v;
    return true;
}
static volatile LONG g_psOk = 0, g_psFail = 0;
static HRESULT STDMETHODCALLTYPE hCreatePS(IDirect3DDevice9* d, const DWORD* func, IDirect3DPixelShader9** out) {
    int s, v;
    if (g_cfgNoDof && !g_inOwn && func && out && IsDofShader(func, &s, &v)) {
        const DWORD pass[] = {
            0xFFFF0300,
            0x0200001F, 0x80000005, 0x90030000 | (DWORD)v,          // dcl_texcoord0 v#.xy
            0x0200001F, 0x90000000, 0xA00F0800 | (DWORD)s,          // dcl_2d s# (Source)
            0x03000042, 0x800F0000, 0x90E40000 | (DWORD)v, 0xA0E40800 | (DWORD)s,   // texld r0, v#, s#
            0x02000001, 0x800F0800, 0x80E40000,                     // mov oC0, r0
            0x0000FFFF };
        HRESULT hr = oCreatePS(d, pass, out);
        if (SUCCEEDED(hr)) {
            LONG n = InterlockedIncrement(&g_dofReplaced);
            if (n <= 4) LOGI("engine: depth of field shader replaced by a sharp pass-through (Source s%d, texcoord v%d)", s, v);
            return hr;
        }
        LOGW("engine: depth of field pass-through rejected (%08lx) - the game's shader is used", hr);
    }
    HRESULT hr = oCreatePS(d, func, out);
    if (!g_inOwn) { if (SUCCEEDED(hr)) InterlockedIncrement(&g_psOk); else { LONG n = InterlockedIncrement(&g_psFail); if (n <= 5) LOGW("d3d9: the game's pixel shader %08lx was REJECTED (%08lx) - materials using it draw wrong", func ? (unsigned)func[0] : 0u, hr); } }
    return hr;
}
int D3D9DofReplaced() { return (int)g_dofReplaced; }

// ---------------------------------------------------------------- 0.9.9: hide D3D9Ex from everyone but the mod
// The Witcher 2 (or code in its process) asks the device whether it is an IDirect3DDevice9Ex. Answered yes, metallic
// / reflective materials (swords, helmets, eyes, Geralt's medallion, wet blood) take a path that writes the raw values of
// the game's axis-lookup cube (exe 0x53F470: 0 / 0.5 / 1 per face -> teal, purple, green, blue, pink) into the colour
// buffer. ReShade hit exactly this: 3.0.4 answered yes for a plain device, users saw "green / red / pink tint on swords,
// helmets, metal parts of Geralt's armour, puddles"; 3.0.6 stopped answering yes and it was gone. The mod's device IS a
// 9Ex device (needed for the headset's shared textures), so it answered yes all along. Now only the mod sees 9Ex.
typedef HRESULT (STDMETHODCALLTYPE* QIFn)(IUnknown*, REFIID, void**);
static QIFn oDevQI, oScQI; static volatile LONG g_exHidden = 0; static bool g_hideEx = true;
static bool FromMod(void* ret) {
    HMODULE me = nullptr, them = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&FromMod, &me);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &them);
    if (me && me == them) return true;
    // the Direct3D runtime's own calls (system d3d9.dll) never reached ReShade's wrapper either; with another D3D9 layer
    // in the game folder (ReShade, DXVK, ...) "d3d9.dll" may be that layer, so check the path
    if (!them) return false;
    static wchar_t win[MAX_PATH] = L""; if (!win[0]) GetWindowsDirectoryW(win, MAX_PATH);   // System32 or SysWOW64 (32-bit game)
    wchar_t p[MAX_PATH]; if (!GetModuleFileNameW(them, p, MAX_PATH)) return false;
    size_t n = wcslen(win), l = wcslen(p);
    return _wcsnicmp(p, win, n) == 0 && l > 9 && _wcsicmp(p + l - 9, L"\\d3d9.dll") == 0;
}
static HRESULT HideEx(QIFn o, IUnknown* self, REFIID riid, void** out, const IID& exIid, const char* what, void* ret) {
    if (g_hideEx && out && IsEqualIID(riid, exIid) && !g_inOwn && !FromMod(ret)) {
        *out = nullptr;
        LONG n = InterlockedIncrement(&g_exHidden);
        if (n <= 3) {
            HMODULE m = nullptr; wchar_t name[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m) && m) GetModuleFileNameW(m, name, MAX_PATH);
            LOGI("d3d9: %s asked for by %ls (+0x%lx) - answered 'not supported' (hide_d3d9ex)", what, wcsrchr(name, L'\\') ? wcsrchr(name, L'\\') + 1 : name,
                 m ? (unsigned long)((uint8_t*)ret - (uint8_t*)m) : 0ul);
        }
        return E_NOINTERFACE;
    }
    return o(self, riid, out);
}
static HRESULT STDMETHODCALLTYPE hDevQI(IUnknown* s, REFIID r, void** o) { return HideEx(oDevQI, s, r, o, __uuidof(IDirect3DDevice9Ex), "IDirect3DDevice9Ex", __builtin_return_address(0)); }
static HRESULT STDMETHODCALLTYPE hScQI(IUnknown* s, REFIID r, void** o) { return HideEx(oScQI, s, r, o, __uuidof(IDirect3DSwapChain9Ex), "IDirect3DSwapChain9Ex", __builtin_return_address(0)); }

static void InstallDeviceHooks(IDirect3DDevice9* d) {
    void** v = VT(d);
    Hook(v[16], (void*)hReset, &oReset, "Reset");
    Hook(v[17], (void*)hPresent, &oPresent, "Present");
    Hook(v[23], (void*)hCreateTexture, &oCreateTexture, "CreateTexture");
    Hook(v[24], (void*)hCreateVolTex, &oCreateVolTex, "CreateVolumeTexture");
    Hook(v[25], (void*)hCreateCubeTex, &oCreateCubeTex, "CreateCubeTexture");
    g_mbExact = ConfigStr("managed_buffers", "exact") != "direct";
    if (g_cfgEx && !g_vanilla) { Hook(v[100], (void*)hSetStrm, &oSetStrm, "SetStreamSource"); Hook(v[104], (void*)hSetIdx, &oSetIdx, "SetIndices"); Hook(v[102], (void*)hSetFreq, &oSetFreq, "SetStreamSourceFreq"); }
    Hook(v[26], (void*)hCreateVB, &oCreateVB, "CreateVertexBuffer");
    Hook(v[27], (void*)hCreateIB, &oCreateIB, "CreateIndexBuffer");
    Hook(v[37], (void*)hSetRT, &oSetRT, "SetRenderTarget");
    Hook(v[47], (void*)hSetViewport, &oSetViewport, "SetViewport");
    Hook(v[34], (void*)hStretchRect, &oStretchRect, "StretchRect");
    Hook(v[65], (void*)hSetTexture, &oSetTexture, "SetTexture");
    SSInitDefaults(); Hook(v[69], (void*)hSetSS, &oSetSS, "SetSamplerState");
    Hook(v[81], (void*)hDrawPrim, &oDrawPrim, "DrawPrimitive");
    Hook(v[82], (void*)hDrawIdxPrim, &oDrawIdxPrim, "DrawIndexedPrimitive");
    Hook(v[83], (void*)hDrawPrimUP, &oDrawPrimUP, "DrawPrimitiveUP");
    Hook(v[84], (void*)hDrawIdxPrimUP, &oDrawIdxPrimUP, "DrawIndexedPrimitiveUP");
    Hook(v[91], (void*)hCreateVS, &oCreateVS, "CreateVertexShader");
    Hook(v[92], (void*)hSetVS, &oSetVS, "SetVertexShader");
    Hook(v[106], (void*)hCreatePS, &oCreatePS, "CreatePixelShader");
    Hook(v[94], (void*)hSetVSConstF, &oSetVSConstF, "SetVertexShaderConstantF");
    Hook(v[57], (void*)hSetRS, &oSetRS, "SetRenderState");
    Hook(v[75], (void*)hSetScissor, &oSetScissor, "SetScissorRect");
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(d->GetSwapChain(0, &sc)) && sc) { Hook(VT(sc)[3], (void*)hSCPresent, &oSCPresent, "SwapChain::Present"); sc->Release(); }
    g_hideEx = ConfigBool("hide_d3d9ex", true);
    if (g_cfgEx && !g_vanilla) {
        Hook(VT(d)[0], (void*)hDevQI, &oDevQI, "Device::QueryInterface");
        IDirect3DSwapChain9* sc2 = nullptr;
        if (SUCCEEDED(d->GetSwapChain(0, &sc2)) && sc2) { Hook(VT(sc2)[0], (void*)hScQI, &oScQI, "SwapChain::QueryInterface"); sc2->Release(); }
    }
    LOGI("d3d9: device hooks installed");
}


static HRESULT STDMETHODCALLTYPE hCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                               D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (!pp || !out) return oCreateDevice(self, adapter, type, wnd, flags, pp, out);
    // 0.9.6: the game asks for a PURE device (D3DCREATE_PUREDEVICE, flags 0x56): no Get* calls for state and no reliable
    // state-block capture. Every pass the mod adds inside the frame (eye capture, HUD redirect, normal repair, VR panel)
    // saves and restores the game's state through exactly those - on a pure device they are not supported, so the game's
    // next draws could run with the mod's blend / depth / shader-constant state (suspected: decals and some materials in the
    // wrong colours - green blood, normal-map colours on the sword pommel - differently from run to run).
    if (!g_vanilla && (flags & D3DCREATE_PUREDEVICE) && ConfigBool("strip_pure_device", true)) {
        flags &= ~(DWORD)D3DCREATE_PUREDEVICE;
        LOGI("d3d9: pure device flag removed (the mod's passes need the device to report and restore its state)");
    }
    IDirect3D9Ex* ex = nullptr;
    D3DPRESENT_PARAMETERS p = *pp;
    ApplyPresentParams(p);
    HRESULT hr = E_FAIL;
    if (g_cfgEx && SUCCEEDED(self->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&ex)) && ex) {
        D3DDISPLAYMODEEX mode = { sizeof(mode), p.BackBufferWidth, p.BackBufferHeight, p.FullScreen_RefreshRateInHz, p.BackBufferFormat, D3DSCANLINEORDERING_PROGRESSIVE };
        IDirect3DDevice9Ex* dex = nullptr;
        hr = ex->CreateDeviceEx(adapter, type, wnd, flags, &p, p.Windowed ? nullptr : &mode, &dex);
        ex->Release();
        LOGI("d3d9: CreateDeviceEx(adapter %u, flags %08lx) -> %08lx", adapter, flags, hr);
        if (SUCCEEDED(hr)) *out = dex;
    }
    if (FAILED(hr) && g_vanilla) {   // the comparison run: the game's own plain D3D9 device, with the stage-dump hooks only
        hr = oCreateDevice(self, adapter, type, wnd, flags, &p, out);
        LOGI("d3d9: plain D3D9 device for the vanilla comparison (flags %08lx) -> %08lx", flags, hr);
        if (FAILED(hr)) return hr;
    }
    if (FAILED(hr)) {
        LOGE("d3d9: no D3D9Ex device - VR output disabled, the game runs normally");
        p = *pp;
        return oCreateDevice(self, adapter, type, wnd, flags, pp, out);
    }
    pp->BackBufferWidth = p.BackBufferWidth; pp->BackBufferHeight = p.BackBufferHeight;
    pp->BackBufferFormat = p.BackBufferFormat; pp->BackBufferCount = p.BackBufferCount;
    NoteBackBuffer(p);
    {   HWND dw = p.hDeviceWindow ? p.hDeviceWindow : wnd;
        WindowNoteDeviceWindow(dw);
        if (p.Windowed) WindowFitClient(dw, p.BackBufferWidth, p.BackBufferHeight); }
    if (g_dev) LOGW("d3d9: a second device was created - VR follows the newest one");
    g_dev = *out;
    InstallDeviceHooks(*out);
    NoteBackBufferSurface(*out);
    if (g_vanilla) {   // diag_vanilla: the game untouched (its own device type, managed pool, camera, renderer) - F10 dumps stages
        LOGI("diag: VANILLA comparison run - no VR, no camera / engine / texture changes; press F10 in gameplay, then send W2VR\\stages");
        return hr;
    }
    W2CamInstall("device");   // SteamStub has decrypted the game's code by now
    SameTickInstallNative("device");
    W2FullStereoInstall("device");
    W2EngineInstall("device");
    return hr;
}

static IDirect3D9* WINAPI hCreate9(UINT sdk) {
    if (g_cfgEx && pCreate9Ex) {
        IDirect3D9Ex* ex = nullptr;
        HRESULT hr = pCreate9Ex(sdk, &ex);
        if (SUCCEEDED(hr) && ex) {
            Hook(VT(ex)[16], (void*)hCreateDevice, &oCreateDevice, "IDirect3D9::CreateDevice");
            LOGI("d3d9: Direct3DCreate9(%u) -> Direct3DCreate9Ex", sdk);
            return ex;
        }
        LOGW("d3d9: Direct3DCreate9Ex failed %08lx", hr);
    }
    return oCreate9(sdk);
}

bool PatchImport(HMODULE mod, const char* dll, void* target, void* repl) {
    auto* base = (BYTE*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    bool done = false;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)(base + imp->Name), dll)) continue;
        for (auto* t = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); t->u1.Function; t++) {
            if ((void*)t->u1.Function != target) continue;
            DWORD old;
            if (VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                t->u1.Function = (ULONG_PTR)repl;
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
                done = true;
            }
        }
    }
    return done;
}

const char* ModeName(int m) { return m == VRMODE_HEAD ? "head" : m == VRMODE_SCREEN ? "screen" : "stereo"; }

void D3D9HooksInstall() {
    InitializeCriticalSection(&g_shaderCs);
    InitializeCriticalSection(&g_shadowCs);
    InitializeCriticalSection(&g_lodCs);
    g_lodEmu = ConfigBool("managed_lod_emulation", true);
    g_directLock = ConfigBool("managed_direct_lock", true);
    { std::string m = ConfigStr("managed_mode", "exact"); g_managedMode = m == "direct" ? 1 : m == "copy" ? 0 : 2; if (g_managedMode == 0) g_directLock = false; }
    g_nrOn = ConfigBool("normal_repair", false);   // kept as a safety net behind the cube texture fix (pixels with a good normal leave it at once)
    g_cubeShadowOn = ConfigBool("managed_cube_copy", true);
    g_cfgWindowed = ConfigBool("force_windowed", true);
    g_cfgImmediate = ConfigBool("present_immediate", true);
    g_cfgEngineCamStart = ConfigStr("camera_mode", "engine") != "shader";
    {   // patch_shaders: auto = analyse every vertex shader (draw classification, HUD panel) but only patch it when the
        // patch has work to do - the shader camera fallback, or a HUD drawn into the eye image (fitted / full)
        std::string ps = ConfigStr("patch_shaders", "auto");
        bool hudPanel = ConfigStr("hud_mode", "panel") == "panel";
        g_cfgPatch = ps != "false" && ps != "0";
        g_cfgAnalyseOnly = ps == "auto" && g_cfgEngineCamStart && hudPanel;
        LOGI("d3d9: vertex shaders %s", !g_cfgPatch ? "not touched (patch_shaders = false: no draw classification)" :
             g_cfgAnalyseOnly ? "analysed, left unchanged (engine camera + HUD panel)" : "patched (shader camera / HUD in the eye image)");
    }
    g_cfgNoDof = ConfigBool("dof_shader_passthrough", false);   // Assassin's Creed shader names; Witcher 2: User.ini AllowDOF
    g_cfgForceSize = ConfigBool("force_render_size", false);
    g_vanilla = ConfigBool("diag_vanilla", false);
    g_cfgEx = ConfigBool("d3d9ex", true) && !g_vanilla;
    {
        std::string a = ConfigStr("render_aspect", "headset");
        float x = 0, y = 0;
        if (sscanf(a.c_str(), "%f:%f", &x, &y) == 2 && x > 0 && y > 0) g_cfgAspect = x / y;
        else if (a != "native" && a != "headset") { float v = (float)atof(a.c_str()); if (v > 0.2f && v < 5.f) g_cfgAspect = v; }
        g_cfgRenderScale = ConfigFloat("render_scale", 1.0f);
        if (g_cfgRenderScale < 0.25f) g_cfgRenderScale = 0.25f;
        if (g_cfgRenderScale > 3.f) g_cfgRenderScale = 3.f;
        g_cfgMaxSide = (UINT)ConfigInt("render_max_side", 8192);
        g_cfgHudFix = ConfigBool("hud_aspect_fix", false);   // AC2-calibrated; Witcher 2's Scaleform HUD lays out by itself
        g_cfgNoBars = ConfigBool("no_letterbox", true);
        g_cfgResHeadset = ConfigStr("render_resolution", "headset") != "game";
        LOGI("d3d9: render aspect %s, render scale %.2f, max side %u, resolution from the %s", g_cfgAspect > 0 ? a.c_str() : "native", g_cfgRenderScale, g_cfgMaxSide,
             g_cfgResHeadset ? "headset" : "game's settings");
    }
    HMODULE m = GetModuleHandleW(L"d3d9.dll");
    if (!m) m = LoadLibraryW(L"d3d9.dll");
    if (!m) { LOGE("d3d9: d3d9.dll not found"); return; }
    void* c9 = (void*)GetProcAddress(m, "Direct3DCreate9");
    pCreate9Ex = (Create9ExFn)GetProcAddress(m, "Direct3DCreate9Ex");
    if (!c9) { LOGE("d3d9: Direct3DCreate9 not exported"); return; }
    // the game imports Direct3DCreate9 statically: patch its import table (a plain memory write, safe inside DllMain);
    // inline hook only if the import was not found
    if (PatchImport(GetModuleHandleW(nullptr), "d3d9.dll", c9, (void*)hCreate9)) { oCreate9 = (Create9Fn)c9; LOGI("d3d9: game import of Direct3DCreate9 redirected"); }
    else Hook(c9, (void*)hCreate9, &oCreate9, "Direct3DCreate9");
    LOGI("d3d9: hooks ready (windowed %d, immediate %d, shader patch %d, ex %d)", g_cfgWindowed, g_cfgImmediate, g_cfgPatch, g_cfgEx);
}

unsigned D3D9HudPanelFrames() { unsigned n = g_redirFrames; g_redirFrames = 0; return n; }
void D3D9Report() {
    LOGI("d3d9: shaders patched %u, not patched %u (+%u vs_1_x), managed textures %u (shadowed %u, locks %u in 10 s), managed buffers %u, reserved-constant writes %u",
         g_vr.shadersPatched, g_vr.shadersSkipped, g_vr.shadersV1, g_convTex, g_convTex - g_shadowFallback - g_directTex, g_shadowLocks, g_convBuf, g_vr.reservedWrites);
    LOGI("d3d9: HUD panel draws: %u from the back buffer, %u from the game's final target (copied to the screen in %u frames); %u 2D draws of the 3D picture (glow / scene copies) kept out",
         g_hudOnBB, g_hudOnFinal, g_finalFrames, g_hudKeptOut);
    g_hudOnBB = g_hudOnFinal = g_finalFrames = g_hudKeptOut = 0;
    D3D9GpuTimeReport();
    D3D9CpuReport();
    D3D9BufferReport();
    LOGI("d3d9: pixel shaders created %ld, rejected %ld", (long)g_psOk, (long)g_psFail);
    LOGI("d3d9: 9Ex interface hidden from the game %ld times%s", (long)g_exHidden, g_hideEx ? "" : " (hide_d3d9ex = false: not hidden)");
    LOGI("d3d9: managed textures (exact managed-pool emulation): %ld, %ld locks, %ld surface writes, %ld uploads, %ld failed",
         (long)g_mtCount, (long)InterlockedExchange(&g_mtLocks, 0), (long)InterlockedExchange(&g_mtSurf, 0), (long)InterlockedExchange(&g_mtUploads, 0), (long)g_mtFail);
    LOGI("d3d9: managed cube textures %ld (system-memory copy): %u locks, %u uploads", (long)g_cubeCount, g_cubeLocks, g_cubeUploads);
    { extern void W2TextureBudgetReport(); W2TextureBudgetReport(); }
    LOGI("normals: G-buffer normals repaired in %u frames%s", NormalRepairPasses(), g_nrOn ? "" : " (normal_repair = false)");
    LOGI("d3d9: managed textures: %u locked directly in video memory (managed_direct_lock), others via a system-memory copy: %u single-level uploads, %u whole-texture fallbacks",
         g_directTex, g_lvlUpdates, g_lvlFail);
    g_lvlUpdates = g_lvlFail = 0;
    LOGI("d3d9: SetLOD (texture streaming) %s: %u calls, %u with a level > 0 (highest %lu), applied in %u sampler states",
         g_lodEmu ? "emulated" : "NOT emulated (managed_lod_emulation = false)", g_lodCalls, g_lodNonZero, (unsigned long)g_lodMax, g_lodApplied);
    g_lodCalls = g_lodNonZero = g_lodApplied = 0;
    LOGI("d3d9: texture filtering %s (anisotropy %u, LOD bias >= %.2f): the game set a negative LOD bias %u times (lowest %.2f), point mip filtering %u times; %u draws adjusted",
         g_tfOn ? "on" : "off", g_tfAniso, g_tfMinBias, g_tfNegBias, g_tfLowestBias, g_tfPointMip, g_tfChanged);
    g_tfNegBias = g_tfPointMip = g_tfChanged = 0;
    LOGI("d3d9: shadow maps: %u draws, extra depth bias slope %.2f constant %.5f%s", g_shadowDraws, g_shSlope, g_shConst, g_shOff ? " - SHADOW MAPS OFF (diagnostic)" : "");
    g_shadowDraws = 0;
    g_shadowLocks = 0;
}
