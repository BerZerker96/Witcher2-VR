// Engine camera for The Witcher 2 (REDengine 2): every eye is rendered by the game's own render camera.
//
// All render-camera updates go through one matrix builder (CRenderCamera, witcher2.exe 2013-05-09 at 0xB067F5;
// found by signature after SteamStub has decrypted the code). It reads the camera's inputs and derives everything
// the renderer uses from them:
//     +0x00 position (world, Z up)       +0x10 Euler angles in degrees: roll, pitch, yaw (rows = R_y(roll) R_x(pitch) R_z(yaw))
//     +0x1C vertical fov (degrees)       +0x20 aspect   +0x24 zoom   +0x28 near   +0x2C far
//     +0x280 / +0x284 off-centre terms (projection row 2: x_c += -o280 * z, y_c += o284 * z)
//   ->  +0x40 world->view, +0x80 world->local, +0xC0 / +0x100 inverses, +0x180 projection, +0x140 its inverse,
//       +0x1C0 view-projection, +0x200 its inverse, +0x240..+0x26C frustum vectors
// (layout and conventions checked by emulating the exe's own code: the Euler order, the axis swap and the
// off-centre projection reproduce to 1e-7).
//
// The hook rewrites the INPUTS before the builder runs: position + eye offset, Euler angles of game camera x head
// rotation, and the eye's own asymmetric headset projection. So culling, shadows, deferred lighting, fog and the
// HUD's 3D markers are all built by the engine for the eye that is really rendered - no shader remap is needed.
// The second function (0xB067AE: +0x270 = 1 / tan(fov / 2), the screen-size / LOD scale) is kept at the game's own
// field of view, so the wider VR view does not lower distant detail.
//
// Anti-compounding: if a camera is rebuilt without the game writing new inputs, the inputs are still ours; the
// original values are used as the base again (the head pose is never applied twice).
//
// Eyes and the render thread: the game thread builds the camera, the render thread draws it a frame later. Each
// eye's projection is unique (asymmetric headset FOV, or a sub-percent tag when both eyes are symmetric), so a draw
// tells which eye's camera it uses (W2CamMatchDraw) and the frame is captured with exactly that eye's pose / FOV.
//
// Full stereo (stereo_method = full): both eyes from ONE game tick. The renderer's frame function (0xB15317,
// stdcall (renderer, frame, scene); called by RenderCommand_RenderScene::Execute at 0x9DC56A on the render thread)
// draws a CRenderFrame - its CRenderFrameInfo at frame+0x10 holds the render camera at frame+0x20 and the "present
// when done" flag at frame+0x2B4 (Present is the last step, 0xB15958 -> 0x9D5FF5 -> IDirect3DDevice9::Present).
// The engine itself renders one frame several times with a changed camera: the tiled "uber screenshot" (0xB15C00)
// sets the frame camera's off-centre terms (0xB0712C -> builder), undoes the scene's frame counter (scene+0x74,
// incremented per render at 0x9C0148) and calls 0xB15317 again. Full stereo does the same per eye: the game thread
// builds the head-centred camera (union of both eye views) plus the two exact eye cameras; on the render thread the
// main frame is drawn with the left-eye camera (no present, copied out as the left eye), then with the right-eye
// camera (presents; captured as the right eye). Animation, physics, particles and the clock are those of a single
// tick: no doubling in motion, no held clock, and the game simulates once per pair.
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "MinHook.h"
#include "log.h"
#include "config.h"
#include "w2vr.h"
#include "w2cammath.h"
using namespace w2cam;

namespace {
// ---- state
typedef void (__stdcall* BuildFn)(uint8_t*);
BuildFn oBuild = nullptr;
bool g_installed = false, g_keepLod = true, g_lodHooked = false;
CRITICAL_SECTION g_cs;

struct Shared { xrb::IpcPose pose[2]; xrb::IpcFov fov[2]; bool valid; } g_sh = {};
UINT g_bbW = 0, g_bbH = 0;
float g_fovMargin = 1.0f; bool g_tagEyes = true;
volatile bool g_fp = false; float g_fpDist = 3.0f, g_fpRight = -0.10f, g_fpUp = 0.10f, g_fpNear = 0.18f;
bool g_stCamHold = true; Fields g_stBase; bool g_stBaseValid = false; unsigned g_stBasePair = 0;

struct Cand { uint8_t* cam; uintptr_t site; unsigned votes, frame, run; };
Cand g_cand[8] = {};
uint8_t* g_main = nullptr; uintptr_t g_mainSite = 0;
DWORD g_voteTick = 0, g_firstBuild = 0; unsigned g_switches = 0;
volatile unsigned g_mainFrame = 0; volatile bool g_mainEver = false;

struct Written { uint8_t* cam; Fields orig, wrote; bool valid; };
Written g_wr[6] = {}; int g_wrNext = 0;

struct EyeBuild { bool valid; xrb::IpcPose pose; GameProj proj, gameProj; unsigned frame; };
EyeBuild g_eb[3] = {};
int g_lastEye = 1, g_latest = -1; unsigned g_lastEyeFrame = ~0u;
float g_hudScale = 1.f;
volatile bool g_symProj = false;   // symmetric_projection
// full stereo
struct FsEntry { bool valid; Fields centre; Fields eye[2]; xrb::IpcPose pose[2]; unsigned frame; };
FsEntry g_fs[8] = {}; int g_fsNext = 0;
bool g_fsInstalled = false; volatile bool g_fsMethod = false;
typedef void (__stdcall* RenderFn)(void*, uint8_t*, uint8_t*);
RenderFn oRender = nullptr;
// 0xAA4167 (stdcall, frame info): info+0x2B0 = pixel -> clip x the camera's inverse view-projection (+0x200) - the
// "screen to world" matrix deferred lighting, shadows, SSAO and fog rebuild positions from depth with. The game
// calls it right after every build of the frame camera (0xAA430A); a pass with another camera needs it again.
typedef void (__stdcall* InfoFn)(uint8_t*);
InfoFn g_infoScreen = nullptr;
volatile LONG g_fsFrames = 0, g_fsOther = 0, g_fsMiss = 0;
volatile bool g_fsFailed = false; unsigned g_fsWatch = 0; LONG g_fsSeen = 0;
// the watchdog's fallback is per scene: it remembers the main camera it gave up on and tries full stereo again as soon
// as the game renders through another one (intro / cutscene -> gameplay, loading a save)
uint8_t* g_fsFailedMain = nullptr; unsigned g_fsRetries = 0; volatile LONG g_fsNear = 0; bool g_fsTolerant = true;
bool FsOn() { return g_fsInstalled && g_fsMethod && !g_fsFailed && g_vr.mode == VRMODE_STEREO; }
volatile LONG g_builds = 0, g_mainBuilds = 0, g_applied = 0, g_compound = 0, g_lodCalls = 0;
volatile bool g_appliedEver = false;

Written* WrFind(uint8_t* cam) { for (auto& w : g_wr) if (w.valid && w.cam == cam) return &w; return nullptr; }
Written* WrGet(uint8_t* cam) {
    if (Written* w = WrFind(cam)) return w;
    Written* w = &g_wr[g_wrNext++ % 6]; *w = Written{}; w->cam = cam; return w;
}

// cameras the mod wrote recently (main camera outputs, full-stereo eye cameras): a camera built holding exactly one of
// these is a COPY of a head-tracked camera (the frame info's camera, a renderer-local copy, an eye pass) - it carries
// the head pose already, so it must neither be composed again nor become the main camera
Fields g_outRing[32]; int g_outN = 0; volatile LONG g_copies = 0;
void NoteOut(const Fields& f) { g_outRing[g_outN++ & 31] = f; }
bool IsRecentOut(const Fields& f) {
    int n = g_outN < 32 ? g_outN : 32;
    for (int i = 0; i < n; i++) if (!memcmp(&g_outRing[i], &f, sizeof(Fields))) return true;
    return false;
}

void Vote(uint8_t* cam, uintptr_t site, bool match, unsigned pc) {
    int k = -1;
    for (int i = 0; i < 8; i++) if (g_cand[i].cam == cam) { k = i; break; }
    if (k < 0) { int lo = 0; for (int i = 1; i < 8; i++) if (g_cand[i].votes < g_cand[lo].votes) lo = i; k = lo; g_cand[k] = Cand{ cam, site, 0, 0, 0 }; }
    Cand& c = g_cand[k];
    c.site = site;
    if (match) {
        c.votes++;
        if (c.frame != pc) { c.run = c.frame + 1 == pc ? c.run + 1 : 1; c.frame = pc; }
        // camera cut: the main camera stopped being built and this one renders the main view for 2 frames
        if (cam != g_main && c.run >= 2) {
            bool mainGone = !g_main || pc - g_mainFrame >= 2;
            if (mainGone) {
                g_main = cam; g_mainSite = site; g_switches++;
                if (g_switches <= 20 || g_switches % 50 == 0) LOGI("camera: main render camera %p (call site %08x; switched at once, %u switches)", (void*)cam, (unsigned)site, g_switches);
            }
        }
    }
    DWORD now = GetTickCount();
    if (now - g_voteTick > 1000) {
        int best = 0; for (int i = 1; i < 8; i++) if (g_cand[i].votes > g_cand[best].votes) best = i;
        unsigned mainVotes = 0; for (auto& x : g_cand) if (x.cam == g_main) mainVotes = x.votes;
        // hysteresis: the main camera only changes when it is gone or clearly out-voted (no flapping between a camera
        // and something built from it)
        if (g_cand[best].votes >= 5 && g_cand[best].cam != g_main && (mainVotes < 5 || g_cand[best].votes * 2 > mainVotes * 3)) {
            g_main = g_cand[best].cam; g_mainSite = g_cand[best].site;
            LOGI("camera: main render camera %p (call site %08x, %u matching builds / s)", (void*)g_main, (unsigned)g_mainSite, g_cand[best].votes);
        }
        for (auto& x : g_cand) x.votes = 0;
        g_voteTick = now;
    }
}
} // namespace

// ---------------------------------------------------------------- the hook
// ---------------------------------------------------------------- 0.8.0: the origin - CRenderFrameInfo's constructor
// Traced in the exe: everything that needs "the camera" in a game tick constructs a CRenderFrameInfo from the viewport
// (0xAA43A0: edi = info, arg = viewport; info+0x10 is its CRenderCamera, which the game's active camera component fills
// through CCameraComponent::CalcCamera 0xA84EDB -> Set 0xB071EE -> the matrix builder). It has six callers per tick:
// the renderer's frame, the HUD / 3D markers (0x9B667D), the viewport's own frame generation (0xAAEF03) and three
// more (0x9D1D61, 0xAAAAA5, 0xAAAE36). 0.1 - 0.7 picked ONE camera object by votes (address + call site + a
// projection match) and gave only that one the head pose, with whatever head pose was newest at each build. So only one
// of the six consumers saw the head-tracked view; the others worked with the game's own forward-looking camera, and
// two builds of one tick could get two different head poses.
// Now: every camera at info+0x10 of a frame info constructed this tick is the viewport's camera - exactly, no votes -
// and all of them get the same head pose, latched once per game tick.
extern "C" { void* g_w2InfoCtorOrig = nullptr; }
namespace {
struct InfoSlot { uint8_t* info; unsigned tick; };
InfoSlot g_infoRing[16]; volatile LONG g_infoNext = 0, g_infoCtors = 0, g_originCams = 0;
bool g_originOn = false;
bool InfoRingHas(const uint8_t* info, unsigned tick) {
    for (const InfoSlot& e : g_infoRing) if (e.info == info && (!tick || tick - e.tick <= 2)) return true;
    return false;
}
}
extern "C" void __cdecl w2vr_NoteFrameInfo(uint8_t* info) {
    InterlockedIncrement(&g_infoCtors);
    unsigned tick = SameTickLoopCount();
    for (InfoSlot& e : g_infoRing) if (e.info == info) { e.tick = tick; return; }
    InfoSlot& e = g_infoRing[(unsigned)InterlockedIncrement(&g_infoNext) % 16];
    e.tick = tick; e.info = info;
}
__asm__(".text\n.globl _W2InfoCtorThunk\n_W2InfoCtorThunk:\n  pushl %eax\n  pushl %ecx\n  pushl %edx\n  pushl %edi\n  call _w2vr_NoteFrameInfo\n  addl $4, %esp\n"
        "  popl %edx\n  popl %ecx\n  popl %eax\n  jmp *_g_w2InfoCtorOrig\n");
extern "C" void W2InfoCtorThunk();

extern "C" void __cdecl w2vr_BuildHook(uint8_t* cam, uintptr_t ret, uintptr_t callerEbp) {
    if (!cam) { oBuild(cam); return; }
    InterlockedIncrement(&g_builds);
    EnterCriticalSection(&g_cs);
    Fields cur; ReadF(cam, cur);
    Written* wr = WrFind(cam);
    Fields base = cur;
    // anti-compounding per group (position / rotation / projection): a group still holding what we wrote goes back to
    // the game's value; a group the game has set since (e.g. only the fov, by a setter) is taken as new
    bool ours = false;
    if (wr) {
        bool gp_ = !memcmp(cur.pos, wr->wrote.pos, 12), ge = !memcmp(cur.eul, wr->wrote.eul, 12);
        if (gp_) memcpy(base.pos, wr->orig.pos, 12);
        if (ge) memcpy(base.eul, wr->orig.eul, 12);
        ours = gp_ || ge;
        float* bf = &base.fov; const float* cf = &cur.fov; const float* wf = &wr->wrote.fov; const float* of = &wr->orig.fov;
        for (int k = 0; k < 6; k++) if (!memcmp(cf + k, wf + k, 4)) { bf[k] = of[k]; ours = true; }   // fov, aspect, zoom, near, off-centre x / y
        if (ours) InterlockedIncrement(&g_compound);
    }
    // a copy of a camera the mod composed: leave it exactly as it is (no second head pose, no vote)
    const bool copy = !ours && IsRecentOut(cur);
    if (copy) InterlockedIncrement(&g_copies);
    bool persp = !copy && Finite(base) && Perspective(base);
    unsigned pc = g_vr.presentCount;
    bool apply = false;
    Fields out = base;
    if (persp) {
        if (!g_firstBuild) g_firstBuild = GetTickCount();
        // the site: where the builder was called from, and that caller's caller (the frame pointer of Set & co.)
        uintptr_t site = ret;
        if (callerEbp && !IsBadReadPtr((void*)(callerEbp + 4), 4)) site ^= *(uintptr_t*)(callerEbp + 4) << 1;
        GameProj gp = FieldsProj(base);
        const GameProj& mp = g_vr.mainProj;
        bool match = false;
        if (mp.valid()) {
            match = ProjClose(gp, mp, 0.02f);
            if (!match && cam == g_main)   // the main camera carries our eye projections: the shaders see those
                for (int e = 0; e < 3 && !match; e++) if (g_eb[e].valid && ProjClose(g_eb[e].proj, mp, 0.01f)) match = true;
        } else if (GetTickCount() - g_firstBuild > 5000) {
            // the shaders never showed a main 3D camera (constants not recognised): the perspective camera built most
            // often with a plausible gameplay fov wins
            match = base.fov > 20.f && base.fov < 120.f;
        }
        bool origin = false;
        if (g_originOn) {   // the viewport's camera, by construction (a default-constructed camera - all zero - is not yet it)
            static const float zero[6] = {};
            origin = InfoRingHas(cam - 0x10, SameTickLoopCount()) && (memcmp(base.pos, zero, 12) || memcmp(base.eul, zero, 12));
            if (origin) { g_main = cam; InterlockedIncrement(&g_originCams); }
        } else Vote(cam, site, match, pc);
        if (g_originOn ? origin : cam == g_main) {
            InterlockedIncrement(&g_mainBuilds);
            g_mainFrame = pc; g_mainEver = true;
            apply = g_installed && g_vr.engineCam && g_sh.valid && g_vr.mode != VRMODE_SCREEN && !W2GameMenuOpen();   // menu: the game's own camera
        }
        if (apply) {
            // one head pose per game tick for every camera of that tick (0.8.0)
            static unsigned latchTick = 0; static xrb::IpcPose latchPose[2]; static xrb::IpcFov latchFov[2];
            const unsigned tickNow = g_originOn ? SameTickLoopCount() : 0;
            if (!tickNow || tickNow != latchTick) { latchTick = tickNow; latchPose[0] = g_sh.pose[0]; latchPose[1] = g_sh.pose[1]; latchFov[0] = g_sh.fov[0]; latchFov[1] = g_sh.fov[1]; }
            const xrb::IpcPose* const shPose = latchPose; const xrb::IpcFov* const shFov = latchFov;
            // symmetric eye view: each eye's field of view widened to a centred one that holds it
            xrb::IpcFov fv[2] = { shFov[0], shFov[1] };
            if (g_symProj) for (auto& f : fv) {
                float h = fmaxf(-f.left, f.right), v = fmaxf(f.up, -f.down);
                f.left = -h; f.right = h; f.up = v; f.down = -v;
            }
            int eye;
            int loopEye = SameTickFrameEye();   // -1: the game loop is not hooked
            const bool full = FsOn();
            if (full) eye = 2;   // the frame camera: head centre, both eyes' views (the render thread draws the eyes)
            else if (g_vr.mode == VRMODE_STEREO) {
                if (loopEye >= 0) eye = loopEye;   // one eye per game frame, paired with the same-tick hold
                else { if (pc != g_lastEyeFrame) { g_lastEyeFrame = pc; g_lastEye ^= 1; } eye = g_lastEye; }
            } else eye = 2;
            // same tick: the second eye of a pair uses the game camera of the first (the camera moves with raw mouse
            // input even while the game's time stands still)
            if (!full && g_vr.mode == VRMODE_STEREO && g_stCamHold && SameTickOn()) {
                unsigned pair = SameTickPair();
                if (eye == 0) { g_stBase = base; g_stBaseValid = true; g_stBasePair = pair; }
                else if (eye == 1 && g_stBaseValid && g_stBasePair == pair) base = g_stBase;
            }
            xrb::IpcPose pose = shPose[eye == 2 ? 0 : eye];
            xrb::IpcFov fov = fv[eye == 2 ? 0 : eye];
            if (eye == 2) {   // head-tracked mono: centre position, the union of both eyes' views
                const xrb::IpcPose& a = shPose[0]; const xrb::IpcPose& b = shPose[1];
                pose.px = (a.px + b.px) * 0.5f; pose.py = (a.py + b.py) * 0.5f; pose.pz = (a.pz + b.pz) * 0.5f;
                fov = { fminf(fv[0].left, fv[1].left), fmaxf(fv[0].right, fv[1].right),
                        fmaxf(fv[0].up, fv[1].up), fminf(fv[0].down, fv[1].down) };
            }
            ComposeOpts co = { g_vr.decoupledPitch, g_vr.positional, g_vr.worldScale, { g_vr.camOffset[0], g_vr.camOffset[1], g_vr.camOffset[2] },
                               g_fovMargin, g_bbW, g_bbH, g_tagEyes && !full, g_fp, g_fpDist, g_fpRight, g_fpUp, g_fpNear };
            const xrb::IpcPose& a0 = shPose[0]; const xrb::IpcPose& a1 = shPose[1];
            float centre[3] = { (a0.px + a1.px) * 0.5f, (a0.py + a1.py) * 0.5f, (a0.pz + a1.pz) * 0.5f };
            bool okc = ComposeEye(base, pose, fov, eye < 2 ? &fv[eye ^ 1] : nullptr, eye, centre, co, out);
            if (okc && full) {   // both eye cameras for this tick, keyed by the frame camera's inputs
                const bool dupFs = g_fsNext && !memcmp(&g_fs[(g_fsNext - 1) % 8].centre, &out, sizeof(Fields));   // the same tick's camera again
                FsEntry& fe = dupFs ? g_fs[(g_fsNext - 1) % 8] : g_fs[g_fsNext++ % 8];
                fe.valid = false; fe.centre = out; fe.frame = pc;
                bool ok2 = true;
                for (int e = 0; e < 2 && ok2; e++) {
                    ok2 = ComposeEye(base, shPose[e], fv[e], &fv[e ^ 1], e, centre, co, fe.eye[e]);
                    fe.pose[e] = shPose[e];
                    if (ok2) { EyeBuild& eb = g_eb[e]; eb.valid = true; eb.pose = shPose[e]; eb.proj = FieldsProj(fe.eye[e]); eb.gameProj = gp; eb.frame = pc; NoteOut(fe.eye[e]); }
                }
                fe.valid = ok2;
            }
            if (okc) {
                EyeBuild& b = g_eb[eye];
                b.valid = true; b.pose = pose; b.proj = FieldsProj(out); b.gameProj = gp; b.frame = pc;
                g_latest = eye;
                g_hudScale = fminf(1.f, fminf(b.proj.p00 / gp.p00, b.proj.p11 / gp.p11));
                InterlockedIncrement(&g_applied); g_appliedEver = true;
            } else { out = base; apply = false; }
        }
    }
    if (apply) {
        Written* w = wr ? wr : WrGet(cam);
        w->cam = cam; w->orig = base; w->wrote = out; w->valid = true;
        WriteF(cam, out);
        NoteOut(out);
    } else if (ours) {
        WriteF(cam, base);   // not ours this time: give the game its own values back
        wr->valid = false;
    }
    LeaveCriticalSection(&g_cs);
    oBuild(cam);
}

// 0xB067AE replacement: +0x270 = 1 / max(tan(fov / 2), 0.0005) from the game's own fov
extern "C" { float g_w2LodTan = 0; }
extern "C" void __cdecl w2vr_LodHook(uint8_t* cam) {
    float fov; memcpy(&fov, cam + kFov, 4);
    InterlockedIncrement(&g_lodCalls);
    if (g_keepLod) {
        EnterCriticalSection(&g_cs);
        if (Written* w = WrFind(cam)) { Fields cur; ReadF(cam, cur); if (!memcmp(&cur.fov, &w->wrote.fov, 4)) fov = w->orig.fov; }
        LeaveCriticalSection(&g_cs);
    }
    float t = tanf(fov * 0.0055555557f * 3.14159274f * 0.5f);
    if (!(t > 0.0005f)) t = 0.0005f;
    float v = 1.f / t;
    memcpy(cam + kLod, &v, 4);
    g_w2LodTan = t;
}

// stubs: the builder (stdcall, 1 argument, returns with ret 4) and the LOD function (esi = camera, plain ret)
__asm__(".intel_syntax noprefix\n"
        ".globl _w2vr_stub_build\n"
        "_w2vr_stub_build:\n"
        "  mov eax, dword ptr [esp]\n"        // return address in the caller
        "  push ebp\n"                        // the caller's frame pointer (its own caller's return at [ebp+4])
        "  push eax\n"
        "  push dword ptr [esp+12]\n"         // camera
        "  call _w2vr_BuildHook\n"
        "  add esp, 12\n"
        "  ret 4\n"
        ".globl _w2vr_stub_lod\n"
        "_w2vr_stub_lod:\n"
        "  pushad\n"
        "  push esi\n"
        "  call _w2vr_LodHook\n"
        "  add esp, 4\n"
        "  popad\n"
        "  movss xmm0, dword ptr [_g_w2LodTan]\n"
        "  movss xmm1, dword ptr [esi+0x270]\n"
        "  ret\n"
        ".att_syntax\n");
extern "C" void w2vr_stub_build();
extern "C" void w2vr_stub_lod();

namespace {
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
}

void W2CamInstall(const char* when) {
    static bool csInit = false;
    if (!csInit) { InitializeCriticalSection(&g_cs); csInit = true; }
    if (g_installed) return;
    if (ConfigStr("camera_mode", "engine") == "shader") { if (!strcmp(when, "device")) LOGI("camera: camera_mode = shader - the engine camera is not used"); return; }
    g_keepLod = ConfigBool("engine_keep_lod", true);
    g_fovMargin = ConfigFloat("fov_margin", 1.0f);
    g_tagEyes = ConfigBool("eye_tag", true);
    g_stCamHold = ConfigBool("same_tick_camera", true);
    int n1 = 0, n2 = 0;
    uint8_t* b = FindUnique("55 8B EC 83 E4 F0 81 EC 54 01 00 00 0F 57 C0 F3 0F 10 0D ?? ?? ?? ?? 53 8B 5D 08 56 57 8D 44 24 60 50", &n1);
    uint8_t* l = FindUnique("F3 0F 10 46 1C F3 0F 59 05 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
                            "F3 0F 10 0D ?? ?? ?? ?? 0F 2F C8 72 03 0F 28 C1 F3 0F 10 0D ?? ?? ?? ?? F3 0F 5E C8 F3 0F 11 8E 70 02 00 00 C3", &n2);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!b) {
        // DllMain time: SteamStub has not decrypted the code yet - the device creation call finds it
        if (!strcmp(when, "device")) LOGE("camera: render camera builder not found (%d matches) - this witcher2.exe build differs; VR needs camera_mode = shader", n1);
        return;
    }
    MH_STATUS s = MH_CreateHook(b, (void*)w2vr_stub_build, (void**)&oBuild);
    if (s == MH_OK) s = MH_EnableHook(b);
    if (s != MH_OK) { LOGE("camera: hook at exe+%lx failed (%d)", (unsigned long)(b - base), (int)s); return; }
    g_installed = true;
    if (ConfigBool("camera_origin", true)) {   // the origin: CRenderFrameInfo's constructor (see above)
        int nc = 0;
        uint8_t* c = FindUnique("55 8B EC 51 53 8B 5D 08 56 8D 47 10 89 1F E8 ?? ?? ?? ?? 8B 43 28 89 87 A0 02 00 00", &nc);
        if (c && MH_CreateHook(c, (void*)W2InfoCtorThunk, &g_w2InfoCtorOrig) == MH_OK && MH_EnableHook(c) == MH_OK) {
            g_originOn = true;
            LOGI("camera: origin hooked - CRenderFrameInfo constructor at exe+%lx: every viewport camera of a game tick gets the same head pose (camera_origin)", (unsigned long)(c - base));
        } else LOGW("camera: origin not hooked (%d matches) - the main camera is picked by votes as before", nc);
    }
    if (l && g_keepLod) {
        uint8_t j[5] = { 0xE9 }; int32_t rel = (int32_t)((uintptr_t)w2vr_stub_lod - ((uintptr_t)l + 5)); memcpy(j + 1, &rel, 4);
        DWORD old;
        if (VirtualProtect(l, 5, PAGE_EXECUTE_READWRITE, &old)) { memcpy(l, j, 5); VirtualProtect(l, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), l, 5); g_lodHooked = true; }
    }
    LOGI("camera: REDengine render camera builder hooked at exe+%lx (%s); detail scale kept at the game's fov: %s (exe+%lx, %d matches)",
         (unsigned long)(b - base), when, g_lodHooked ? "yes" : "no", l ? (unsigned long)(l - base) : 0ul, n2);
}

// ---------------------------------------------------------------- full stereo: the render-thread side
void XrFullStereoMid(int eye, const xrb::IpcPose& pose, const GameProj& proj);   // xr.cpp: copy the back buffer out as this eye
void XrFullStereoPass(int eye, const xrb::IpcPose& pose, const GameProj& proj);  // xr.cpp: the next draws are this eye
namespace {
constexpr int kFrameInfo = 0x10, kFrameCam = 0x20, kFramePresent = 0x2B4, kSceneFrameNo = 0x74;
constexpr int kSaveBytes = 0x2E0;   // frame +0x20 .. +0x300: the camera, size / flags and the screen-to-world matrix (+0x2C0)
void __stdcall hRender(void* r, uint8_t* frame, uint8_t* scene) {
    if (!FsOn() || !frame) { oRender(r, frame, scene); return; }
    uint8_t* cam = frame + kFrameCam;
    Fields cur; ReadF(cam, cur);
    FsEntry fe; bool found = false;
    EnterCriticalSection(&g_cs);
    for (int k = 0; k < 8 && !found; k++) { const FsEntry& x = g_fs[(g_fsNext - 1 - k) & 7]; if (x.valid && !memcmp(&x.centre, &cur, sizeof(Fields))) { fe = x; found = true; } }
    // not bit-identical: the game adjusted the frame camera after the build (camera shake, a cutscene's own fov /
    // aspect touch-up). The newest entry within a few cm / a fraction of a degree is the same view.
    if (!found && g_fsTolerant && Perspective(cur))
        for (int k = 0; k < 3 && !found; k++) {
            const FsEntry& x = g_fs[(g_fsNext - 1 - k) & 7];
            if (!x.valid || g_vr.presentCount - x.frame > 3) continue;
            float dp = 0, de = 0;
            for (int i = 0; i < 3; i++) { dp = fmaxf(dp, fabsf(x.centre.pos[i] - cur.pos[i])); float d = fabsf(x.centre.eul[i] - cur.eul[i]); d = fminf(d, fabsf(360.f - d)); de = fmaxf(de, d); }
            if (dp < 0.10f && de < 1.0f && fabsf(x.centre.fov / cur.fov - 1.f) < 0.02f && fabsf(x.centre.aspect / cur.aspect - 1.f) < 0.02f) { fe = x; found = true; InterlockedIncrement(&g_fsNear); }
        }
    LeaveCriticalSection(&g_cs);
    if (!found) { InterlockedIncrement(Perspective(cur) ? &g_fsMiss : &g_fsOther); oRender(r, frame, scene); return; }
    static uint8_t save[kSaveBytes];   // render thread only
    memcpy(save, cam, kSaveBytes);
    const uint8_t present = frame[kFramePresent];
    for (int e = 0; e < 2; e++) {
        WriteF(cam, fe.eye[e]);
        oBuild(cam);                                   // the game's own builder (unhooked): every derived matrix + frustum
        memcpy(cam + kLod, save + kLod, 4);            // detail / LOD scale of the frame camera
        g_infoScreen(frame + kFrameInfo);              // the screen-to-world matrix of THIS eye (lighting, shadows, SSAO)
        if (e == 1 && scene) --*(volatile int*)(scene + kSceneFrameNo);   // same frame again (as the uber screenshot does)
        frame[kFramePresent] = e == 1 ? present : 0;   // only the last pass presents
        XrFullStereoPass(e, fe.pose[e], FieldsProj(fe.eye[e]));
        oRender(r, frame, scene);
        if (e == 0) XrFullStereoMid(0, fe.pose[0], FieldsProj(fe.eye[0]));
    }
    memcpy(cam, save, kSaveBytes);
    frame[kFramePresent] = present;
    InterlockedIncrement(&g_fsFrames); InterlockedIncrement(&g_fsSeen);
}
}
void W2FullStereoSetMethod(const std::string& m) {
    bool on = m == "full";
    if (on && !g_fsMethod) g_fsFailed = false;   // chosen again: try again
    if (on != g_fsMethod) LOGI("full stereo: %s%s", on ? "ON - both eyes rendered from each game tick" : "off", on && !g_fsInstalled ? " (waiting for the renderer hook)" : "");
    g_fsMethod = on;
}
bool W2FullStereoOn() { return FsOn(); }
bool W2FullStereoAvailable() { return g_fsInstalled; }
void W2FullStereoInstall(const char* when) {
    g_fsTolerant = ConfigBool("full_stereo_tolerant", true);
    if (g_fsInstalled || !g_installed) return;
    int n = 0, m = 0;
    uint8_t* f = FindUnique("55 8B EC 83 E4 F0 81 EC E4 01 00 00 53 56 57 E8 ?? ?? ?? ?? 8B 45 0C 8B 75 08 83 C0 10 8B 18", &n);
    // the present test inside it: cmp byte [frame info + 0x2A4], 0 (frame info = frame + 0x10)
    uint8_t* t = FindUnique("8B 44 24 1C 80 B8 A4 02 00 00 00 74 0E 80 7C 24 16 00", &m);
    auto* base = (uint8_t*)GetModuleHandleW(nullptr);
    int k = 0;
    g_infoScreen = (InfoFn)FindUnique("53 8B DC 51 51 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC D8 00 00 00 8B 43 08 F3 0F 10 05 ?? ?? ?? ?? DB 80 AC 02 00 00", &k);
    if (!g_infoScreen) { if (!strcmp(when, "device")) LOGW("full stereo: frame screen-to-world function not found (%d matches) - full stereo off (lighting would not match the eyes)", k); return; }
    if (!f || !t || t < f || t > f + 0x800) { if (!strcmp(when, "device")) LOGW("full stereo: renderer frame function not found (%d / %d matches) - use same_tick or aer", n, m); return; }
    MH_STATUS s = MH_CreateHook(f, (void*)hRender, (void**)&oRender);
    if (s == MH_OK) s = MH_EnableHook(f);
    if (s != MH_OK) { LOGE("full stereo: hook at exe+%lx failed (%d)", (unsigned long)(f - base), (int)s); return; }
    g_fsInstalled = true;
    LOGI("full stereo: renderer frame function hooked at exe+%lx (%s) - %s", (unsigned long)(f - base), when, g_fsMethod ? "active in stereo" : "available (stereo_method = full)");
}
void W2FullStereoReport() {
    if (!g_fsInstalled || !g_fsMethod) return;
    LOGI("full stereo: %ld frames drawn for both eyes (%ld matched within tolerance), %ld other 3D frames, %ld main-looking frames not matched%s",
         InterlockedExchange(&g_fsFrames, 0), InterlockedExchange(&g_fsNear, 0), InterlockedExchange(&g_fsOther, 0), InterlockedExchange(&g_fsMiss, 0),
         g_fsFailed ? " - same tick for this scene (tried again on the next camera change)" : "");
}

bool W2CamActive() {
    return g_installed && g_vr.engineCam && g_mainEver && g_vr.presentCount - g_mainFrame <= 4 && g_appliedEver;
}
bool W2CamMainLive() { return g_installed && g_mainEver && g_vr.presentCount - g_mainFrame <= 4; }
bool W2CamMatchDraw(const GameProj& p, int* eye) {
    if (!g_installed) return false;
    // the newest build first (with the render thread a frame behind, the other eye may be the one drawn)
    int order[4] = { g_latest, 0, 1, 2 };
    for (int k = 0; k < 4; k++) {
        int e = order[k]; if (e < 0 || (k && e == g_latest)) continue;
        const EyeBuild& b = g_eb[e];
        if (!b.valid || g_vr.presentCount - b.frame > 6) continue;
        if (fabsf(p.p00 / b.proj.p00 - 1.f) < 0.004f && fabsf(p.p11 / b.proj.p11 - 1.f) < 0.004f &&
            fabsf(p.p02 - b.proj.p02) < 0.003f && fabsf(p.p12 - b.proj.p12) < 0.003f) { *eye = e; return true; }
    }
    return false;
}
bool W2CamEyeRecord(int eye, xrb::IpcPose* pose, GameProj* proj) {
    if (eye < 0 || eye > 2 || !g_eb[eye].valid) return false;
    EnterCriticalSection(&g_cs);
    *pose = g_eb[eye].pose; *proj = g_eb[eye].proj;
    LeaveCriticalSection(&g_cs);
    return true;
}
int W2CamLatestEye() { return g_latest; }
float W2CamHudScale() { return g_hudScale > 0.05f ? g_hudScale : 1.f; }
void W2CamOnPresent(const xrb::IpcPose poses[2], const xrb::IpcFov fov[2], bool valid) {
    // full stereo watchdog: VR stereo with the head pose applied for ~2 s but the renderer never drew both eyes -> same tick
    if (g_fsFailed && g_fsMethod && g_main && g_main != g_fsFailedMain && W2CamMainLive()) {
        // another scene camera: full stereo gets a fresh chance (the fallback was for the camera it failed with)
        g_fsFailed = false; g_fsWatch = 0; InterlockedExchange(&g_fsSeen, 0); g_fsRetries++;
        if (g_fsRetries <= 20) LOGI("full stereo: main camera changed (%p) - trying full stereo again", (void*)g_main);
    }
    if (FsOn() && valid && g_appliedEver && W2CamMainLive()) {
        if (InterlockedExchange(&g_fsSeen, 0) > 0) g_fsWatch = 0;
        else if (++g_fsWatch > 90) {
            g_fsFailed = true; g_fsFailedMain = g_main;
            LOGW("full stereo: the frames of camera %p never reached the renderer hook (%ld unmatched 3D frames) - same-tick AER until the camera changes", (void*)g_main, (long)g_fsMiss);
        }
    } else g_fsWatch = 0;
    EnterCriticalSection(&g_cs);
    if (valid) { g_sh.pose[0] = poses[0]; g_sh.pose[1] = poses[1]; g_sh.fov[0] = fov[0]; g_sh.fov[1] = fov[1]; }
    g_sh.valid = valid;
    LeaveCriticalSection(&g_cs);
}
void W2CamSetBackBuffer(UINT w, UINT h) { g_bbW = w; g_bbH = h; }
void W2CamSetFovMargin(float m) { g_fovMargin = m; }
void W2CamSetSymmetric(bool on) {
    if (on != g_symProj) LOGI("camera: eye views %s", on ? "symmetric (centred, widened to hold the headset's view)" : "as the headset's (off-centre)");
    g_symProj = on;
}
void W2CamSetFirstPerson(bool on, float dist, float right, float up, float nearZ) {
    if (on != g_fp) LOGI("camera: first person %s (pivot %.2f m ahead, right %.2f, up %.2f, near plane %.2f m)", on ? "ON" : "off", dist, right, up, nearZ);
    g_fpDist = dist; g_fpRight = right; g_fpUp = up; g_fpNear = nearZ; g_fp = on;
}
bool W2CamFirstPerson() { return g_fp; }
bool W2CamGameProj(GameProj* p) {
    for (int e : { g_latest, 0, 1, 2 }) if (e >= 0 && g_eb[e].valid && g_eb[e].gameProj.valid()) { *p = g_eb[e].gameProj; return true; }
    return false;
}
void W2CamReport() {
    if (!g_installed) return;
    W2FullStereoReport();
    if (g_originOn) LOGI("camera origin: %ld frame infos constructed, %ld viewport cameras head-tracked (all with one pose per game tick)",
                         InterlockedExchange(&g_infoCtors, 0), InterlockedExchange(&g_originCams, 0));
    LOGI("camera: %ld builds (main %ld, head pose applied %ld, rebuilt without new input %ld, copies of head-tracked cameras left alone %ld), detail-scale calls %ld, main %p, HUD fit %.2f",
         InterlockedExchange(&g_builds, 0), InterlockedExchange(&g_mainBuilds, 0), InterlockedExchange(&g_applied, 0), InterlockedExchange(&g_compound, 0),
         InterlockedExchange(&g_copies, 0), InterlockedExchange(&g_lodCalls, 0), (void*)g_main, g_hudScale);
}

// test export: the Euler round trip (used by the offline check against the exe's own code)
extern "C" __declspec(dllexport) int __stdcall W2VR_TestCameraMath(float* e) {
    M3 m; EulerToM(e, m); float o[3]; MToEuler(m, o); M3 m2; EulerToM(o, m2);
    float err = 0; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) err = fmaxf(err, fabsf(m[i][j] - m2[i][j]));
    e[0] = o[0]; e[1] = o[1]; e[2] = o[2];
    return err < 1e-4f ? 1 : 0;
}

// test export (tests/check_dll_hook.py runs the compiled hook in an emulator against the game's own builder)
extern "C" __declspec(dllexport) void __stdcall W2VR_TestSetup(void* builder, uint8_t* mainCam, const float* poses14, const float* fov8, int mode, int decoupled) {
    oBuild = (BuildFn)builder; g_installed = true; g_keepLod = true;
    g_vr.engineCam = true; g_vr.mode = mode; g_vr.worldScale = 1.f; g_vr.positional = true; g_vr.decoupledPitch = decoupled != 0;
    g_vr.camOffset[0] = g_vr.camOffset[1] = g_vr.camOffset[2] = 0; g_vr.presentCount = 100;
    for (int e = 0; e < 2; e++) {
        const float* p = poses14 + e * 7; g_sh.pose[e] = { p[0], p[1], p[2], p[3], p[4], p[5], p[6] };
        const float* f = fov8 + e * 4; g_sh.fov[e] = { f[0], f[1], f[2], f[3] };
    }
    g_sh.valid = true; g_main = mainCam; g_mainEver = true; g_voteTick = 0; g_bbW = 2000; g_bbH = 2000; g_fovMargin = 1.f; g_tagEyes = true;
}
extern "C" __declspec(dllexport) int __stdcall W2VR_TestLastEye(float* proj4) {
    int e = g_latest; if (e < 0) return -1;
    proj4[0] = g_eb[e].proj.p00; proj4[1] = g_eb[e].proj.p02; proj4[2] = g_eb[e].proj.p11; proj4[3] = g_eb[e].proj.p12;
    return e;
}

// test export (tests/check_fullstereo.py): full stereo with a recording stand-in for the renderer; returns the hook
namespace { float* g_tlog = nullptr; int g_tn = 0; }
static void __stdcall TestRender(void*, uint8_t* frame, uint8_t* scene) {
    if (!g_tlog || g_tn >= 4) return;
    float* o = g_tlog + g_tn++ * 52;
    o[0] = frame[0x2B4]; o[1] = scene ? (float)*(int*)(scene + 0x74) : -1.f; o[2] = g_vr.drawEye; o[3] = g_vr.drawEng;
    memcpy(o + 4, frame + 0x20 + 0x40, 64); memcpy(o + 20, frame + 0x20 + 0x180, 64); memcpy(o + 36, frame + 0x10 + 0x2B0, 64);
}
extern "C" __declspec(dllexport) void* __stdcall W2VR_TestFullStereo(float* log, void* infoFn) {
    g_tlog = log; g_tn = 0; oRender = TestRender; g_fsInstalled = true; g_fsMethod = true; g_fsFailed = false; g_infoScreen = (InfoFn)infoFn;
    return (void*)hRender;
}
