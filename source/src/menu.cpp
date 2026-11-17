// Settings (w2vr_config.txt) and the in-headset VR panel (vrkit, D3D9 backend). Insert or hold L3 + R3 opens it.
#include <windows.h>
#include <d3d9.h>
#include <string>
#include <cstdio>
#include "log.h"
#include "config.h"
#include "w2vr.h"
#include "game.h"
#include "version.h"
#include "vrkit_options.h"
#include "vrkit_menu_d3d9.h"
#include "xrbridge/client.h"

void D3D9SetHudAspectFix(bool on);
void D3D9SetShadowBias(float slope, float constant, bool shadowsOff);
void XrSetFrameLimit(const std::string& m);
void XrSetViewOptions(bool edgeFill, bool crop, const std::string& engineFov);
void XrSetSoftEdge(bool on);
void DeDitherSet(float strength, float range);
void D3D9SetTextureFiltering(bool on, float minBias, int aniso);
void XrSetStereoAuto(bool on);
void XrSetAutoZoom(bool on);
void XrCycleHudMode();
void XrSetHudPanel(float dist, float offY, float width, bool follow);
void W2CamSetFovMargin(float m);

struct VrSettings {
    std::string vrMode = "stereo";
    float worldScale = 1.f, fovMargin = 1.f;
    bool edgeFill = true, edgeSoft = true; float edgeMargin = 4.f; std::string cameraMode = "engine", frameLimit = "headset", smoothing = "always", tstView = "normal", stereoMethod = "aer", renderResolution = "headset", renderAspect = "headset";
    bool positional = true, decoupledPitch = true, keepLod = true;
    bool firstPerson = false; float fpDist = 3.f, fpRight = -0.1f, fpUp = 0.1f, fpNear = 0.18f;
    float camX = 0, camY = 0, camZ = 0;
    bool hudScaleAuto = true, hudAspectFix = false;
    std::string hudMode = "panel"; float hudPanelDist = 1.25f, hudPanelOffY = -0.05f, hudPanelW = 1.85f, hudTransp = 0.6f; bool hudPanelFollow = true;
    float hudScale = 0.6f, hudSize = 1.0f, hudX = 0.f, hudY = 0.f;
    float renderScale = 1.f;
    std::string screenshotMode = "eye";
    bool flatPaused = true;
    bool noSsao = false, symProj = true;
    float shSlope = 0.f, shConst = 0.f; bool shOff = false; bool noSharpen = true;
    float dedither = 0.5f, ditherRange = 0.35f;
    bool tstDecalsOff = false, tstNormalRepair = false, tstReupload = false;
    std::string tstBudget = "game"; bool tstBufDirect = false;
    bool texFilter = true; float texMinBias = 0.f; float texAniso = 16.f;
    bool iniTweaks = true, noDof = true, noMotionBlur = true, noVignette = true, noBlur = false;
    float screenDistance = 2.0f, screenWidth = 2.6f;
    float menuWidth = 0.6f, menuOpacity = 0.8f;
};
static VrSettings g_set;
static bool g_menuInit = false, g_menuFailed = false;
static float g_fps = 0;

static int ModeFromString(const std::string& m) { return m == "head" ? VRMODE_HEAD : m == "screen" ? VRMODE_SCREEN : VRMODE_STEREO; }

void SettingsLoad() {
    g_set.vrMode = ConfigStr("vr_mode", "stereo");
    g_set.worldScale = ConfigFloat("world_scale", 1.0f);
    g_set.fovMargin = ConfigFloat("fov_margin", 1.0f);
    g_set.edgeFill = ConfigBool("edge_fill", true);
    g_set.edgeSoft = ConfigBool("edge_fill_soft", true);
    g_set.edgeMargin = ConfigFloat("edge_fill_margin", 4.f);
    g_set.cameraMode = ConfigStr("camera_mode", "engine");
    g_set.frameLimit = ConfigStr("frame_limit", "headset");
    g_set.smoothing = ConfigStr("headset_smoothing", "drops");
    g_set.stereoMethod = ConfigStr("stereo_method", "auto");
    g_set.renderResolution = ConfigStr("render_resolution", "headset");
    g_set.renderAspect = ConfigStr("render_aspect", "headset");
    g_set.positional = ConfigBool("positional_tracking", true);
    g_set.decoupledPitch = ConfigBool("decoupled_pitch", true);
    g_set.keepLod = ConfigBool("engine_keep_lod", true);
    g_set.firstPerson = ConfigBool("first_person", false);
    g_set.fpDist = ConfigFloat("first_person_distance", 3.0f);
    g_set.fpRight = ConfigFloat("first_person_right", -0.10f);
    g_set.fpUp = ConfigFloat("first_person_up", 0.10f);
    g_set.fpNear = ConfigFloat("first_person_near", 0.18f);
    g_set.camX = ConfigFloat("camera_offset_x", 0.f);
    g_set.camY = ConfigFloat("camera_offset_y", 0.f);
    g_set.camZ = ConfigFloat("camera_offset_z", 0.f);
    g_set.hudScaleAuto = ConfigBool("hud_scale_auto", true);
    g_set.hudMode = ConfigStr("hud_mode", "panel");
    g_set.hudPanelDist = ConfigFloat("hud_panel_distance", 1.25f);
    g_set.hudPanelOffY = ConfigFloat("hud_panel_offset_y", -0.05f);
    g_set.hudPanelW = ConfigFloat("hud_panel_width", 1.85f);
    g_set.hudPanelFollow = ConfigBool("hud_panel_follow_head", true);
    g_set.hudTransp = ConfigFloat("hud_transparency", 0.6f);
    g_set.hudScale = ConfigFloat("hud_scale", 0.6f);
    g_set.hudSize = ConfigFloat("hud_size", 1.0f);
    g_set.hudAspectFix = ConfigBool("hud_aspect_fix", false);
    g_set.hudX = ConfigFloat("hud_offset_x", 0.f);
    g_set.hudY = ConfigFloat("hud_offset_y", 0.f);
    g_set.renderScale = ConfigFloat("render_scale", 1.0f);
    g_set.screenshotMode = ConfigStr("screenshot_mode", "eye");
    g_set.iniTweaks = ConfigBool("user_ini_tweaks", true);
    g_set.noDof = ConfigBool("disable_depth_of_field", true);
    g_set.noMotionBlur = ConfigBool("disable_motion_blur", true);
    g_set.noVignette = ConfigBool("disable_vignette", true);
    g_set.noBlur = ConfigBool("disable_blur", false);
    g_set.noSsao = ConfigBool("disable_ssao", false);
    g_set.symProj = ConfigBool("symmetric_projection", true);
    g_set.shSlope = ConfigFloat("shadow_slope_bias", 0.f);
    g_set.shConst = ConfigFloat("shadow_depth_bias", 0.f);
    g_set.noSharpen = ConfigBool("disable_sharpen", false);
    g_set.dedither = ConfigFloat("dedither", 0.f);
    g_set.texFilter = ConfigBool("texture_filtering_vr", true);
    g_set.texMinBias = ConfigFloat("texture_lod_bias_min", 0.f);
    g_set.texAniso = (float)ConfigInt("texture_anisotropy", 16);
    g_set.ditherRange = ConfigFloat("dedither_range", 0.35f);
    g_set.flatPaused = ConfigBool("flat_when_paused", true);
    g_set.screenDistance = ConfigFloat("screen_distance", 2.0f);
    g_set.screenWidth = ConfigFloat("screen_width", 2.6f);
    g_set.menuWidth = ConfigFloat("menu_width", 0.6f);
    g_set.menuOpacity = ConfigFloat("menu_opacity", 0.8f);
}

void SettingsApply() {
    g_vr.worldScale = g_set.worldScale;
    g_vr.fovScale = 1.f;
    g_vr.positional = g_set.positional;
    g_vr.decoupledPitch = g_set.decoupledPitch;
    g_vr.engineCam = g_set.cameraMode != "shader";
    W2CamSetFovMargin(g_set.fovMargin);
    W2CamSetFirstPerson(g_set.firstPerson, g_set.fpDist, g_set.fpRight, g_set.fpUp, g_set.fpNear);
    W2EngineSetNoDof(g_set.noDof);   // live: the post-process mask patch (no-op until the code is found)
    W2EngineSetEffectOff(W2FX_MOTION_BLUR, g_set.noMotionBlur);
    W2EngineSetEffectOff(W2FX_VIGNETTE, g_set.noVignette);
    W2EngineSetEffectOff(W2FX_BLUR, g_set.noBlur);
    W2EngineSetEffectOff(W2FX_SSAO, g_set.noSsao);
    W2EngineSetEffectOff(W2FX_SHARPEN, g_set.noSharpen);
    W2CamSetSymmetric(g_set.symProj);
    D3D9SetShadowBias(g_set.shSlope, g_set.shConst, g_set.shOff);
    DeDitherSet(g_set.dedither, g_set.ditherRange);
    D3D9SetTextureFiltering(g_set.texFilter, g_set.texMinBias, (int)(g_set.texAniso + 0.5f));
    XrSetFrameLimit(g_set.frameLimit);
    { extern void XrSetSmoothing(const std::string&); XrSetSmoothing(g_set.smoothing); }
    {   extern void D3D9SetTests(int, bool, bool, bool);
        int v = g_set.tstView == "colour" ? 1 : g_set.tstView == "specular" ? 2 : g_set.tstView == "normals" ? 3 : 0;
        D3D9SetTests(v, g_set.tstDecalsOff, g_set.tstNormalRepair, g_set.tstReupload);
        g_set.tstReupload = false;   // one shot
        { extern void D3D9SetBuffersDirect(bool); D3D9SetBuffersDirect(g_set.tstBufDirect); }
        { extern void W2SetTextureBudgetTest(int); W2SetTextureBudgetTest(g_set.tstBudget == "game" ? 0 : atoi(g_set.tstBudget.c_str())); }
    }
    if (g_set.stereoMethod == "auto") XrSetStereoAuto(true);   // follows the headset's smoothing (xr.cpp)
    else { XrSetStereoAuto(false); SameTickSetMode(g_set.stereoMethod); W2FullStereoSetMethod(g_set.stereoMethod); }
    g_vr.camOffset[0] = g_set.camX; g_vr.camOffset[1] = g_set.camY; g_vr.camOffset[2] = g_set.camZ;
    g_vr.hudScaleAuto = g_set.hudScaleAuto;
    g_vr.hudMode = g_set.hudMode == "fitted" ? 1 : g_set.hudMode == "full" ? 2 : 0;
    XrSetHudPanel(g_set.hudPanelDist, g_set.hudPanelOffY, g_set.hudPanelW, g_set.hudPanelFollow);
    { extern void XrSetHudTransparency(float); XrSetHudTransparency(g_set.hudTransp); }
    g_vr.hudScale = g_set.hudScale;
    g_vr.hudSize = g_set.hudSize;
    D3D9SetHudAspectFix(g_set.hudAspectFix);
    g_vr.hudOffset[0] = g_set.hudX; g_vr.hudOffset[1] = g_set.hudY;
    W2SetFlatWhenPaused(g_set.flatPaused);
    XrSetMode(ModeFromString(g_set.vrMode));
    XrSetViewOptions(g_set.edgeFill, true, "off");
    XrSetSoftEdge(g_set.edgeSoft);
    { extern void XrSetEdgeMargin(float); XrSetEdgeMargin(g_set.edgeMargin); }
    XrSetAutoZoom(false);
    xrb::ClientSetMenu(g_set.screenDistance, g_set.screenWidth);
}

void MenuNoteFps(float fps) { g_fps = fps; }
void MenuSetHudMode(int m) { static const char* n[3] = { "panel", "fitted", "full" }; g_set.hudMode = n[m < 0 || m > 2 ? 0 : m]; SettingsApply(); }
void MenuToggleFirstPerson() { g_set.firstPerson = !g_set.firstPerson; SettingsApply(); }
bool MenuIsOpen() { return g_menuInit && !g_menuFailed && vrkit::menu9::IsOpen(); }

static void MenuInit(IDirect3DDevice9* dev) {
    g_menuInit = true;
    vrkit::SetLog([](const char* m) { LOGI("%s", m); });
    auto& o = vrkit::Options::Get();
    auto apply = [] { SettingsApply(); };

    // View
    auto& m = o.AddChoice("View", "vr_mode", "VR mode", &g_set.vrMode, { "stereo", "head", "screen" },
                          "Stereo: the game renders the eyes in turn. Head: one head-tracked image. Screen: flat virtual screen. Also F8.", apply);
    m.choiceLabels = { "Stereo 3D", "Head-tracked mono", "Flat screen" };
    auto& stm = o.AddChoice("View", "stereo_method", "Stereo method", &g_set.stereoMethod, { "auto", "full", "same_tick", "aer" },
                           "Full stereo: the renderer draws both eyes from each game tick (one simulation, two eye views - like the engine's own tiled screenshots). Same tick: the game's time stands still for the second eye. AER: the eyes a frame apart.", apply);
    stm.choiceLabels = { "Automatic (follows the headset's smoothing)", "Full stereo (both eyes per tick)", "Same-tick AER", "Alternate eye (AER)" };
    o.AddFloat("View", "world_scale", "World scale", &g_set.worldScale, 0.25f, 4.f, 0.05f, 2,
               "1.0 = real size. Larger values make the world look smaller (wider eye separation).", apply);
    auto& cm = o.AddChoice("View", "camera_mode", "Head tracking on", &g_set.cameraMode, { "engine", "shader" },
                           "Engine: REDengine's own render camera renders each eye (correct culling, shadows, lighting). Shader: fallback.", apply);
    cm.choiceLabels = { "Game camera (recommended)", "Shaders (fallback)" };
    o.AddFloat("View", "fov_margin", "Render margin", &g_set.fovMargin, 1.f, 1.3f, 0.01f, 2,
               "Renders a little beyond the headset's view (headroom when the headset reprojects). 1.00 = exactly the headset's view.", apply);
    o.AddBool("View", "positional_tracking", "Positional tracking", &g_set.positional,
              "Off: only head rotation moves the view (the eye separation stays).", apply);
    o.AddBool("View", "symmetric_projection", "Symmetric eye view", &g_set.symProj,
              "Each eye is rendered with a centred view wide enough to hold the headset's off-centre view. Screen-space effects (SSAO and its blur, fog, light shafts) assume a centred view - with it they are clean. The render size grows to keep the sharpness (from the next launch). Live.", apply);
    o.AddBool("View", "edge_fill", "Edge fill", &g_set.edgeFill, "No black at the edge of the view: the picture's edge is stretched outward, smooth, around it. Shows when the headset re-projects an older frame (low fps, turning the head) or the picture is narrower than the view.", apply);
    o.AddBool("View", "edge_fill_soft", "Edge fill: smooth", &g_set.edgeSoft, "On: the stretched edge is blurred and fades in smoothly. Off: the edge pixels are repeated (streaks). The picture itself stays sharp either way.", apply);
    o.AddFloat("View", "edge_fill_margin", "Edge fill: extra margin (deg)", &g_set.edgeMargin, 0.f, 20.f, 1.f, 0,
               "How far beyond the headset's view the stretched edge reaches on every side. More hides black better when fps drops; 0 = only gaps inside the view.", apply);
    o.AddBool("View", "flat_when_paused", "Menus on the flat screen", &g_set.flatPaused,
              "Pause menu, inventory, map, journal, character: whenever the game pauses, the headset shows the game on the flat virtual screen; VR comes back when the game runs again.", apply);
    o.AddAction("View", "Recenter view", [] { XrRequestRecenter(); }, "Puts the view in front of you (also Home - recenter_key).");

    // Camera
    o.AddBool("Camera", "first_person", "First person (End)", &g_set.firstPerson,
              "Moves the view to Geralt's head: the pivot of the game's orbit camera (as Enhanced Camera's first-person template). Also End.", apply);
    o.AddFloat("Camera", "first_person_distance", "First person: distance", &g_set.fpDist, 0.f, 6.f, 0.05f, 2,
               "Metres from the game camera to Geralt's head along its view (3.0 for the standard exploration camera; less if you zoomed it in).", apply);
    o.AddFloat("Camera", "first_person_right", "First person: right", &g_set.fpRight, -1.f, 1.f, 0.01f, 2, "Metres. The game camera sits over Geralt's shoulder.", apply);
    o.AddFloat("Camera", "first_person_up", "First person: up", &g_set.fpUp, -1.f, 1.f, 0.01f, 2, "Metres, to eye height.", apply);
    o.AddFloat("Camera", "first_person_near", "First person: near clip", &g_set.fpNear, 0.05f, 0.5f, 0.01f, 2,
               "Metres in front of the eye where drawing starts: clips the inside of Geralt's head and hair.", apply);
    o.AddBool("Camera", "decoupled_pitch", "Decoupled pitch", &g_set.decoupledPitch,
              "The game camera stays level; you look up and down with your head only. Recommended against motion sickness.", apply);
    o.AddFloat("Camera", "camera_offset_x", "Camera offset X (right)", &g_set.camX, -3.f, 3.f, 0.05f, 2, "Metres. Moves the view right (+) / left (-).", apply);
    o.AddFloat("Camera", "camera_offset_y", "Camera offset Y (up)", &g_set.camY, -3.f, 3.f, 0.05f, 2, "Metres. Moves the view up (+) / down (-).", apply);
    o.AddFloat("Camera", "camera_offset_z", "Camera offset Z (forward)", &g_set.camZ, -5.f, 5.f, 0.05f, 2,
               "Metres. Moves the view forward (+) towards Geralt / back (-).", apply);
    o.AddAction("Camera", "Reset camera offsets", [] { g_set.camX = g_set.camY = g_set.camZ = 0; SettingsApply(); }, "Back to the game's camera position.");
    o.AddBool("Camera", "engine_keep_lod", "Keep the game's detail level", &g_set.keepLod,
              "The engine's screen-size / detail scale stays at the game's own field of view, so the wider VR view does not lower distant detail.").needsRestart = true;

    // HUD
    auto& hm = o.AddChoice("HUD", "hud_mode", "HUD / menus shown", &g_set.hudMode, { "panel", "fitted", "full" },
                           "Panel: the game's HUD and menus on their own head-locked panel at full resolution (sharp text), sized to the flat game's view so markers line up. Fitted / full: drawn into the eye image (they need the shader patch: restart the game after choosing them). F7 cycles.", apply);
    hm.choiceLabels = { "Floating panel (sharp)", "In view, fitted", "In view, full size" };
    o.AddFloat("HUD", "hud_panel_distance", "Panel distance", &g_set.hudPanelDist, 0.6f, 5.f, 0.05f, 2,
               "Metres. The panel keeps its angular size (it is the flat game's view), so this only changes where your eyes focus.", apply);
    o.AddFloat("HUD", "hud_panel_offset_y", "Panel height", &g_set.hudPanelOffY, -1.f, 1.f, 0.01f, 2, "Metres up (+) / down (-) from eye level.", apply);
    o.AddFloat("HUD", "hud_panel_width", "Panel width (0 = game view)", &g_set.hudPanelW, 0.f, 6.f, 0.05f, 2,
               "0: as wide as the flat game's view at that distance (markers line up). Any other value: fixed width in metres.", apply);
    o.AddFloat("HUD", "hud_transparency", "HUD / UI transparency", &g_set.hudTransp, 0.f, 0.9f, 0.05f, 2,
               "How see-through the game's HUD and UI are in the headset (0 = solid, 0.6 = 60 % see-through). Live.", apply);
    o.AddBool("HUD", "hud_panel_follow_head", "Panel follows the head", &g_set.hudPanelFollow,
              "On: the panel stays in front of your eyes. Off: it stays in front of the recentred direction.", apply);
    o.AddBool("HUD", "hud_scale_auto", "HUD fits the game view", &g_set.hudScaleAuto,
              "Shrinks the HUD to the area the flat game showed, so it sits in front of you instead of at the edges of the headset view.", apply);
    o.AddFloat("HUD", "hud_scale", "HUD scale", &g_set.hudScale, 0.2f, 1.5f, 0.01f, 2,
               "Manual HUD size (when 'HUD fits the game view' is off). 1.0 = the game's size.", apply);
    o.AddFloat("HUD", "hud_size", "HUD size (fine)", &g_set.hudSize, 0.3f, 2.f, 0.01f, 2, "Extra size on top of the scale above. Live.", apply);
    o.AddFloat("HUD", "hud_offset_x", "HUD position X", &g_set.hudX, -1.f, 1.f, 0.005f, 3, "Moves the whole HUD left (-) / right (+). Live.", apply);
    o.AddFloat("HUD", "hud_offset_y", "HUD position Y", &g_set.hudY, -1.f, 1.f, 0.005f, 3, "Moves the whole HUD down (-) / up (+). Live.", apply);
    o.AddBool("HUD", "hud_aspect_fix", "Centre a 16:9 HUD in a square picture", &g_set.hudAspectFix,
              "Only if the HUD sits off-centre with a square render size (calibrated for Assassin's Creed; try it). Live.", apply);
    o.AddAction("HUD", "Cycle HUD mode (F7)", [] { XrCycleHudMode(); }, "Floating panel / fitted / full size.");
    o.AddAction("HUD", "Reset HUD position / size", [] { g_set.hudSize = 1.0f; g_set.hudX = 0.f; g_set.hudY = 0.f; SettingsApply(); }, "Back to the defaults.");

    // Display (start-up)
    auto& rr = o.AddChoice("Display", "render_resolution", "Resolution source", &g_set.renderResolution, { "headset", "game" },
                           "Headset: User.ini [Viewport] is set to the headset's per-eye resolution at start-up. Game: left as it is.");
    rr.choiceLabels = { "Headset", "Game options" }; rr.needsRestart = true;
    o.AddFloat("Display", "render_scale", "Render resolution", &g_set.renderScale, 0.5f, 2.f, 0.05f, 2,
               "Multiplies the headset's resolution. Costs performance and video memory.").needsRestart = true;
    auto& ra = o.AddChoice("Display", "render_aspect", "Aspect ratio", &g_set.renderAspect, { "headset", "1:1", "5:4", "4:3", "16:9" },
                           "Shape of the game's image. Headset = the eye's own shape.");
    ra.choiceLabels = { "Headset eye", "1:1", "5:4", "4:3", "16:9" }; ra.needsRestart = true;
    auto& sm = o.AddChoice("Display", "screenshot_mode", "Screenshot (Delete)", &g_set.screenshotMode, { "eye", "sbs" },
                           "Saved as PNG next to the game exe: one eye, or both eyes side by side.");
    sm.choiceLabels = { "Left eye", "Both eyes (SBS)" };

    // Engine (User.ini, start-up)
    o.AddBool("Engine", "user_ini_tweaks", "Set up User.ini for VR", &g_set.iniTweaks,
              "Windowed, VSync off and the headset resolution, written to Documents\\Witcher 2\\Config\\User.ini (backup kept). Quality settings are never touched; the effects below are switched in the renderer.").needsRestart = true;
    o.AddBool("Engine", "disable_depth_of_field", "Depth of field off", &g_set.noDof, "Gameplay, cutscene and bokeh (scatter) focus blur - forced off in the renderer, so cutscenes cannot turn it back on. Live.", apply);
    o.AddBool("Engine", "disable_motion_blur", "Motion blur off", &g_set.noMotionBlur, "Recommended in VR. Live (renderer patch).", apply);
    o.AddBool("Engine", "disable_vignette", "Vignette off", &g_set.noVignette, "Dark screen corners do not belong in a headset. Live (renderer patch).", apply);
    o.AddBool("Engine", "disable_ssao", "SSAO off", &g_set.noSsao,
              "Screen-space ambient occlusion. Keep it with 'Symmetric eye view' on (it assumes a centred view: off-centre eye views give grainy specks and halos). Live (renderer patch).", apply);
    o.AddBool("Engine", "disable_sharpen", "Sharpen off", &g_set.noSharpen,
              "The game's sharpen filter boosts fine detail and edges: in a headset (the image is resampled again by the compositor) it shows as grainy dark specks on bright, detailed surfaces and dark halos along edges. Live (renderer patch).", apply);
    o.AddBool("Engine", "texture_filtering_vr", "Texture filtering for VR", &g_set.texFilter,
              "Material textures (not render targets): the game's negative mip LOD bias raised to the minimum below, point mip filtering made trilinear, anisotropic filtering. Removes the sparkle / crawl of fine texture detail when you move your head. Live.", apply);
    o.AddFloat("Engine", "texture_lod_bias_min", "Texture LOD bias (minimum)", &g_set.texMinBias, -1.f, 1.f, 0.05f, 2,
               "0 = never sharper than the screen can show. Higher = softer, calmer textures; lower = sharper, more shimmer. Live.", apply);
    o.AddFloat("Engine", "texture_anisotropy", "Anisotropic filtering", &g_set.texAniso, 1.f, 16.f, 1.f, 0,
               "Keeps ground and walls sharp at grazing angles. 1 = off. Live.", apply);
    o.AddFloat("Engine", "dedither", "De-dither filter", &g_set.dedither, 0.f, 1.f, 0.05f, 2,
               "The Witcher 2 draws soft shadows and blended decal / ground layers as a pixel dither pattern that its Ubersampling averages away on a monitor - in the headset it is the grain on lit surfaces. This edge-preserving filter smooths it in each eye's picture. 0 = off, 1 = full. Live.", apply);
    o.AddFloat("Engine", "dedither_range", "De-dither: contrast kept", &g_set.ditherRange, 0.1f, 1.f, 0.05f, 2,
               "Brightness difference (relative) still treated as dither. Higher smooths more, lower keeps more fine detail. Live.", apply);
    o.AddFloat("Engine", "shadow_slope_bias", "Shadow acne fix (slope)", &g_set.shSlope, 0.f, 8.f, 0.1f, 1,
               "Extra slope-scaled depth bias while the game draws its shadow maps. The wide VR view stretches the sun's shadow maps over more ground than the game's own bias covers: grainy dark specks on sunlit surfaces. Raise until they are gone; too much detaches shadows from their objects. 0 = the game's own. Live.", apply);
    o.AddFloat("Engine", "shadow_depth_bias", "Shadow acne fix (constant)", &g_set.shConst, 0.f, 0.005f, 0.0001f, 4,
               "Extra constant depth bias for the shadow maps (with the slope bias above). Live.", apply);
    o.AddBool("Engine", "shadow_maps_off_test", "Test: shadow maps off", &g_set.shOff,
              "Diagnostic, not saved as a setting you want: the shadow maps are not drawn (no shadows). If the grain disappears, it is shadow acne - tune the two values above. Live.", apply);
    o.AddBool("Engine", "disable_blur", "Fullscreen blur off", &g_set.noBlur, "The blur behind menus and some effects. Live (renderer patch).", apply);
    // ---- Tests (live; not kept between starts): find which layer / feature a wrong texture colour comes from
    auto& tv = o.AddChoice("Tests", "test_view", "Show layer", &g_set.tstView, { "normal", "colour", "specular", "normals" },
                           "Shows one layer of the game's G-buffer in place of the picture: Colour = the textures' colours before lighting, Specular, Normals = surface directions (normal / height maps). If green / blue patches are already in Colour, they come from the textures or decals; if Colour is right, they come from lighting.", apply);
    tv.choiceLabels = { "Normal picture", "G-buffer: colour", "G-buffer: specular", "G-buffer: normals" };
    o.AddBool("Tests", "test_decals_off", "Hide decals (blended layers)", &g_set.tstDecalsOff,
              "Skips every draw that blends into the G-buffer: blood, dirt, decals, terrain blend layers. If the green blood disappears, it is drawn by a decal.", apply);
    o.AddBool("Tests", "test_normal_repair", "Normal repair", &g_set.tstNormalRepair,
              "The mod's fill-in of empty surface normals (0.9.0). Off: what the game itself writes.", apply);
    o.AddBool("Tests", "test_reupload", "Re-upload all textures now", &g_set.tstReupload,
              "Copies every game texture to the graphics card again (one shot). If a wrong texture fixes itself, its video-memory copy was stale.", apply);
    o.AddBool("Tests", "test_buffers_direct", "Old buffer handling (slower)", &g_set.tstBufDirect,
              "Compare: buffers the game rewrites after drawing (blood, hit effects, animated HUD pieces) are locked directly again, which makes the frame wait for the graphics card. Off = the stall-free 1.0.2 handling. Only buffers that become 'hot' after switching are affected.", apply);
    auto& tb = o.AddChoice("Tests", "test_budget", "Texture memory budget", &g_set.tstBudget, { "game", "1536", "2048", "3072" },
                           "The game loads a texture's sharp mips only while its texture memory fits this budget; over it, textures stay on blurry low mips or flat placeholder colours. If the sword / eyes get their real texture with a higher value, the budget was the cause. Live.", apply);
    tb.choiceLabels = { "Game's value", "1536 MB", "2048 MB", "3072 MB" };
    auto& hsm = o.AddChoice("Engine", "headset_smoothing", "Headset smoothing", &g_set.smoothing, { "always", "drops", "off" },
                           "Always: the game runs at half the headset's rate with both eyes per tick, so the headset's smoothing (Pimax Smart Smoothing) is on all the time. Only when fps drops: every frame is real while the game holds the full rate. Off: the game never follows the headset. Smart Smoothing must be on in Pimax Play.", apply);
    hsm.choiceLabels = { "Always", "Only when fps drops", "Off" };
    auto& fl = o.AddChoice("Engine", "frame_limit", "Frame limit", &g_set.frameLimit,
                           { "off", "headset", "headset_x2", "30", "45", "60", "72", "90", "120" },
                           "Caps the game's frame rate (live). Headset x2: each eye gets the full headset rate in stereo.", apply);
    fl.choiceLabels = { "Off (uncapped)", "Headset refresh", "Headset refresh x2", "30 fps", "45 fps", "60 fps", "72 fps", "90 fps", "120 fps" };

    // Screen
    o.AddFloat("Screen", "screen_distance", "Screen distance", &g_set.screenDistance, 1.f, 10.f, 0.1f, 1,
               "Flat screen: menus, loading screens, videos, and Flat screen mode.", apply);
    o.AddFloat("Screen", "screen_width", "Screen width", &g_set.screenWidth, 1.f, 10.f, 0.1f, 1, "Metres.", apply);
    o.AddFloat("Screen", "menu_width", "This panel's size", &g_set.menuWidth, 0.3f, 1.f, 0.05f, 2, "Width of this panel.");
    o.AddFloat("Screen", "menu_opacity", "This panel's opacity", &g_set.menuOpacity, 0.4f, 1.f, 0.02f, 2, "Applied at the next start.").needsRestart = true;

    vrkit::menu::Settings s;
    s.title = "THE WITCHER 2  VR";
    s.subtitle = "v" AC2VR_VERSION;
    s.theme = 1;   // dark panel, bronze / gold frame, crimson title, gold pages, red toggles
    s.configPath = g_gameDir + L"\\" GAME_WFILE L"_config.txt";
    s.backgroundAlpha = g_set.menuOpacity;
    s.statusLine = [] {
        char b[160];
        snprintf(b, sizeof b, "%.0f fps  |  %s%s%s", g_fps, ModeName(g_vr.mode), W2FullStereoOn() ? " full stereo" : SameTickOn() ? " same-tick" : "",
                 W2CamFirstPerson() ? "  |  first person" : (g_vr.active || g_vr.mode == VRMODE_SCREEN ? "" : " (flat now)"));
        return std::string(b);
    };
    s.input.keyboardToggleVk = VK_INSERT;
    bool ok = vrkit::menu9::Init(dev, s);
    if (!ok) g_menuFailed = true;
    LOGI("menu: %s", ok ? "ready - Insert or hold L3+R3" : "init FAILED");
}

IDirect3DTexture9* MenuUpdate(IDirect3DDevice9* dev, bool* open) {
    *open = false;
    if (!dev || g_menuFailed) return nullptr;
    if (!g_menuInit) MenuInit(dev);
    if (!vrkit::menu9::IsOpen()) g_set.vrMode = ModeName(g_vr.mode);
    static bool debugOpen = ConfigBool("debug_menu_open", false);
    if (debugOpen) { debugOpen = false; vrkit::menu9::SetOpen(true); }
    bool wasOpen = vrkit::menu9::IsOpen();
    *open = vrkit::menu9::Update();
    static bool dumped = !ConfigBool("debug_dump_menu", false);
    if (*open && !dumped && vrkit::menu9::Texture()) {   // debug: AC2VR\menu.bmp
        dumped = true;
        IDirect3DSurface9 *s = nullptr, *sys = nullptr; D3DSURFACE_DESC sd;
        vrkit::menu9::Texture()->GetSurfaceLevel(0, &s); s->GetDesc(&sd);
        if (SUCCEEDED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(dev->GetRenderTargetData(s, sys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                FILE* f = _wfopen((g_modDir + L"\\menu.bmp").c_str(), L"wb");
                if (f) {
                    BITMAPFILEHEADER bh = { 0x4d42, (DWORD)(54 + sd.Width * sd.Height * 4), 0, 0, 54 };
                    BITMAPINFOHEADER bi = { 40, (LONG)sd.Width, -(LONG)sd.Height, 1, 32 };
                    fwrite(&bh, 14, 1, f); fwrite(&bi, 40, 1, f);
                    for (UINT y = 0; y < sd.Height; y++) fwrite((char*)lr.pBits + y * lr.Pitch, 4, sd.Width, f);
                    fclose(f);
                }
                sys->UnlockRect();
            }
        }
        if (sys) sys->Release();
        if (s) s->Release();
        LOGI("menu: dumped to W2VR\\menu.bmp");
    }
    if (*open != wasOpen) LOGI("menu: %s", *open ? "opened" : "closed");
    return *open ? vrkit::menu9::Texture() : nullptr;
}

void MenuOnLostDevice() { if (g_menuInit && !g_menuFailed) vrkit::menu9::OnLostDevice(); }
