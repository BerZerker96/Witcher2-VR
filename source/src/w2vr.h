#pragma once
// W2VR shared state (framework: AC2VR / DA2VR). Most of it lives on the game's render thread unless noted.
#include <windows.h>
#include <d3d9.h>
#include <string>
#include "stereo.h"

enum VrMode { VRMODE_STEREO = 0, VRMODE_HEAD = 1, VRMODE_SCREEN = 2 };

constexpr int kConstBase = 252;       // c252..c255 belong to the mod in every patched vertex shader

struct VrState {
    int mode = VRMODE_STEREO;
    float worldScale = 1.f;
    float fovScale = 1.f;
    bool positional = true;
    // camera
    bool decoupledPitch = false;      // remove the game camera's pitch (UEVR style): the head alone looks up / down
    float camOffset[3] = { 0, 0, 0 }; // metres: x right, y up, z forward (in the levelled camera frame)
    float camPitch = 0;               // the game camera's pitch (radians, from the last frame)
    bool camPitchValid = false;
    // HUD
    bool hudScaleAuto = true;         // HUD shrunk to the game's view inside the wider VR view
    float hudScale = 1.f;             // manual HUD scale (hudScaleAuto = false)
    float hudScaleEff = 1.f;          // applied this frame (auto fit or manual scale)
    float hudSize = 1.f;              // extra size factor (VR panel, live)
    float hudOffset[2] = { 0, 0 };    // HUD position offset in clip units (x right, y up; live)
    int hudMode = 0;                  // 0 panel (own head-locked quad, full resolution), 1 fitted into the eye image, 2 full size in the eye image

    // per frame (set at Present for the next frame)
    bool active = false;              // host connected and a game projection is known
    int eye = 0;                      // 0 / 1 (AFR stereo), 2 = centre (head mode)
    xrb::IpcPose pose = {0, 0, 0, 0, 0, 0, 1};
    GameProj mainProj;                // the game's main camera projection (last frame)
    GameProj outProj;                 // projection of the image sent to the headset
    UINT mainW = 0, mainH = 0;        // viewport size of the main camera's draws
    UINT mainVpX = 0, mainVpY = 0;    // viewport offset (letterbox / pillarbox)
    UINT mainRtW = 0, mainRtH = 0;    // render target the main camera draws into
    float content[4] = { 0, 0, 1, 1 };// where the 3D picture sits in the back buffer (x0, y0, x1, y1 in 0..1)
    int64_t poseTime = 0;
    // engine camera mode: the head pose is put on AC2's render camera (so the engine culls for the real eye); the
    // shaders then only remap the projection. engineCamFrame = Present count when the camera got the pose.
    bool engineCam = true;
    volatile unsigned engineCamFrame = 0, presentCount = 0;
    xrb::IpcPose engPose = {0, 0, 0, 0, 0, 0, 1}; int engEye = 0;      // pose / eye last put on the game camera
    bool drawEng = false; xrb::IpcPose drawPose = {0, 0, 0, 0, 0, 0, 1}; int drawEye = 0; GameProj drawProj;   // what this frame's 3D draws used

    // counters of the frame being rendered
    unsigned drawsXf = 0, drawsPatched = 0, drawsTotal = 0;
    unsigned drawsMain = 0;           // draws recognised as the main 3D camera (engine camera mode: not transformed)
    // statistics (reset by the periodic report)
    unsigned shadersPatched = 0, shadersSkipped = 0, shadersV1 = 0, reservedWrites = 0;
};
extern VrState g_vr;
extern std::wstring g_gameDir, g_modDir;
extern IDirect3DDevice9* g_dev;
extern volatile LONG g_inOwn;         // >0 while the mod itself draws (hooks pass through)

// d3d9hooks.cpp
void D3D9HooksInstall();
const char* ModeName(int m);
// trampolines the mod uses for its own calls
HRESULT CallSetVSConstF(IDirect3DDevice9* d, UINT reg, const float* data, UINT count);
HRESULT CallCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                          IDirect3DTexture9** tex, HANDLE* shared);
// called by d3d9hooks at Present: rolls the per-frame statistics over and picks the main camera
void StereoEndFrame();

// window.cpp
void WindowHooksInstall();
void WindowFitClient(HWND hwnd, UINT w, UINT h);    // nullptr = the game window
void WindowNoteDeviceWindow(HWND h);
bool PatchImport(HMODULE mod, const char* dll, void* target, void* repl);   // d3d9hooks.cpp
// sametick.cpp: same-tick alternate-eye rendering
void SameTickInstall();
void SameTickSetMode(const std::string& m);
bool SameTickAvailable();
void SameTickNextFrame(bool stereoActive, int nextEye);
void SameTickCamera(const float* m);
bool SameTickHeld();
bool SameTickOn();          // same tick is running (VR stereo + engine camera + stereo_method = same_tick)
void SameTickReport();
int SameTickFrameEye();      // eye the current game frame renders (native loop hook), -1 if not hooked
unsigned SameTickPair();     // counts eye pairs
unsigned SameTickLoopCount();   // game ticks (0 = loop not hooked)
void SameTickInstallNative(const char* when);   // W2: main-loop dt + engine clock (after SteamStub decrypted the code)

// xr.cpp
void XrOnPresent(IDirect3DDevice9* dev);
void XrOnLostDevice();
void XrReport();
void XrRequestRecenter();
void XrSetMode(int mode);

// w2camera.cpp: the head pose on REDengine's render camera (CRenderCamera matrix builder)
void W2CamInstall(const char* when);
void W2FullStereoInstall(const char* when);           // hook the renderer's frame function (0xB15317): both eyes per tick
void W2FullStereoSetMethod(const std::string& m);      // "full" turns it on (VR stereo only)
bool W2FullStereoOn();
bool W2FullStereoAvailable();
bool W2CamActive();                                   // the engine camera carries the eye poses (no shader remap)
bool W2CamMatchDraw(const GameProj& p, int* eye);     // a draw's projection is one of the eye cameras written
bool W2CamEyeRecord(int eye, xrb::IpcPose* pose, GameProj* proj);   // what the camera of that eye was built with
float W2CamHudScale();                                // game view / VR view (tangent ratio), 1 if unknown
bool W2CamMainLive();                                 // the main render camera was built in the last frames
void W2CamOnPresent(const xrb::IpcPose poses[2], const xrb::IpcFov fov[2], bool valid);
void W2CamSetBackBuffer(UINT w, UINT h);
void W2CamReport();
void W2CamSetFirstPerson(bool on, float dist, float right, float up, float nearZ);
void W2CamSetSymmetric(bool on);   // symmetric_projection
bool W2CamGameProj(GameProj* p);   // the game's own projection of the main camera (before the VR eye projection)
// w2engine.cpp
void W2EngineInstall(const char* when);
void W2EngineEarly();        // patches that need no decrypted code (before the game starts)
void W2InputReport();
void W2WindowToFront();
bool W2IniResolution(unsigned* w, unsigned* h);
void D3D9RequestStageDump();
// xr.cpp: the HUD panel's render target (back-buffer size), nullptr if not available
IDirect3DSurface9* XrHudSurface(IDirect3DDevice9* d);
void XrNoteHudDraw();
bool W2CamFirstPerson();
// w2window.cpp
HWND WindowGame();
// w2ini.cpp: Documents\Witcher 2\Config\User.ini tweaks (before the game reads it)
enum { W2FX_DOF = 0, W2FX_MOTION_BLUR, W2FX_VIGNETTE, W2FX_BLUR, W2FX_SSAO, W2FX_SHARPEN, W2FX_COUNT };
bool W2EngineSetEffectOff(int fx, bool off);   // post effect forced off in the renderer (code patch, live)
bool W2EngineSetNoDof(bool on);
bool W2GamePaused(bool* activePause);   // the game's pause count (menus, inventory, map ...)
bool W2GameMenuOpen();                  // paused by a menu and flat_when_paused: show the flat screen
void W2SetFlatWhenPaused(bool on);   // depth of field off everywhere (code patch, live)
bool W2EngineNoDof();
void W2EngineDitherReport();     // once: the dither textures were built uniform
bool W2EngineDitherRemoved();
void W2IniApply();
void W2CompatInstall();   // many-core start-up hang fix (w2compat.cpp)
bool W2IniResolution(unsigned* w, unsigned* h);   // the windowed resolution the game will use (after the tweaks)

// menu.cpp
void SettingsLoad();
void SettingsApply();
IDirect3DTexture9* MenuUpdate(IDirect3DDevice9* dev, bool* open);
void MenuOnLostDevice();
void MenuNoteFps(float fps);
