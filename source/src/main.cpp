// W2VR - The Witcher 2: Assassins of Kings VR mod. Loaded as xinput1_3.dll from the game's bin folder (witcher2.exe
// imports XINPUT1_3 ordinals 2 and 3). Framework: AC2VR / Dragon Age II VR (XrBridge host, vrkit menu, proxy loader,
// D3D9Ex shared textures, vertex shader patching). Witcher 2 specific: the head pose and the headset projection go
// onto REDengine's render camera (w2camera.cpp), User.ini is set up for VR (w2ini.cpp).
#include <windows.h>
#include <string>
#include "log.h"
#include "config.h"
#include "MinHook.h"
#include "version.h"
#include "w2vr.h"
#include "game.h"

void ProxyLoad(HMODULE self);
extern std::wstring g_proxyName;

std::wstring g_gameDir, g_modDir;
HMODULE g_self = nullptr;

extern LPTOP_LEVEL_EXCEPTION_FILTER g_gameCrashFilter;   // w2compat.cpp
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord; auto* c = ep->ContextRecord;
    HMODULE m = nullptr; char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &m)) GetModuleFileNameA(m, mod, MAX_PATH);
    const char* base = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
    LOGE("CRASH code %08lx at %p (%s+%lx)", er->ExceptionCode, er->ExceptionAddress, base, (unsigned long)((char*)er->ExceptionAddress - (char*)m));
    LOGE("  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    DWORD* sp = (DWORD*)c->Esp; int n = 0;
    for (int i = 0; i < 2048 && n < 24; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i]; HMODULE mm = nullptr; char mn[MAX_PATH];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &mm) && mm &&
            GetModuleFileNameA(mm, mn, MAX_PATH)) {
            const char* b = strrchr(mn, '\\') ? strrchr(mn, '\\') + 1 : mn;
            if (mm == GetModuleHandleW(nullptr) || mm == g_self || !_stricmp(b, "d3d9.dll")) { LOGE("  stack+%04x %s+%lx", i * 4, b, v - (DWORD)mm); n++; }
        }
    }
    LogFlushNow();
    if (g_gameCrashFilter) return g_gameCrashFilter(ep);   // then the game's own crash reporter
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI FlushThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) { Sleep(200); LogFlushDisk(); }
}

static const char* kDefaultConfig =
    "# " GAME_NAME " (" GAME_SHORT ") settings. Lines: key = value. Delete this file to get the defaults back.\n"
    "# Most settings are also in the VR panel (Insert / hold L3 + R3) and are saved from there.\n"
    "\n"
    "# ---- View\n"
    "# vr_mode: stereo (3D), head (head-tracked single image), screen (flat virtual screen). F8 cycles them.\n"
    "vr_mode = stereo\n"
    "# world size in VR (1.0 = real scale; larger = the world looks smaller)\n"
    "world_scale = 1.00\n"
    "# camera_mode: engine = each eye is rendered by REDengine's own render camera (correct culling, shadows and\n"
    "# lighting for every eye); shader = fallback that turns the view in the vertex shaders\n"
    "camera_mode = engine\n"
    "positional_tracking = true\n"
    "# fov_margin: render a little more than the headset shows (1.00 - 1.20), headroom for reprojection\n"
    "fov_margin = 1.00\n"
    "# symmetric_projection: each eye rendered with a centred view wide enough to hold the headset's off-centre view.\n"
    "# The game's screen-space effects (SSAO and its blur, fog, light shafts) assume a centred view - off-centre eye\n"
    "# views (Pimax: +-0.2) give grainy dark specks and halos. The render size grows to keep the pixel density.\n"
    "symmetric_projection = true\n"
    "# eye_tag: on headsets whose eyes have identical views, the two eye cameras get a tiny difference so the mod can\n"
    "# tell which eye the render thread drew (the FOV sent to the headset includes it, nothing is misplaced)\n"
    "eye_tag = true\n"
    "edge_fill = true\n"
    "edge_fill_soft = true\n"
    "# edge_fill_margin: degrees beyond the headset view filled with the stretched edge (no black when the headset re-projects)\n"
    "edge_fill_margin = 4\n"
    "\n"
    "# ---- Camera\n"
    "# decoupled_pitch: the game camera stays level, you look up / down with your head only (comfort)\n"
    "decoupled_pitch = true\n"
    "# camera offset in metres: x right, y up, z forward (e.g. z 1.5 moves the view closer to Geralt)\n"
    "camera_offset_x = 0.00\n"
    "camera_offset_y = 0.00\n"
    "camera_offset_z = 0.00\n"
    "# first person (End toggles): the view moves to Geralt's head - the pivot of the game's orbit camera, first_person_distance\n"
    "# metres along its view axis (Enhanced Camera's first-person template uses 3.0); right / up shift it, near clips his head\n"
    "first_person = false\n"
    "first_person_key = end\n"
    "first_person_distance = 3.00\n"
    "first_person_right = -0.10\n"
    "first_person_up = 0.10\n"
    "first_person_near = 0.18\n"
    "# no_letterbox: the game forces a 16:9 picture with black bars on other aspects (the cause of the community's\n"
    "# ultrawide hex fixes); off with this, the near-square VR picture uses the whole frame\n"
    "no_letterbox = true\n"
    "# engine_keep_lod: the engine's screen-size / detail scale stays at the game's own fov (no lower detail from the wide view)\n"
    "engine_keep_lod = true\n"
    "\n"
    "# ---- HUD and menus (Scaleform)\n"
    "# hud_mode: panel = the game's HUD and menus are drawn into their own target and shown on a head-locked panel at full\n"
    "# resolution (sharp text), as large as the flat game's view so 3D markers line up; fitted = drawn into the eye image,\n"
    "# shrunk to the flat game's view; full = into the eye image at full size. F7 cycles.\n"
    "hud_mode = panel\n"
    "hud_panel_distance = 1.25\n"
    "hud_panel_offset_y = -0.05\n"
    "# hud_panel_width: 0 = the flat game's view at that distance; else metres\n"
    "hud_panel_width = 1.85\n"
    "hud_panel_follow_head = true\n"
    "# hud_transparency: how see-through the game's HUD and UI are in the headset (0 = solid, 0.6 = 60 %). Live: VR panel > HUD\n"
    "hud_transparency = 0.60\n"
    "# hud_panel_fit: the HUD panel is shrunk to stay inside hud_panel_fit_share of the headset's own view (its FOV),\n"
    "# so minimap, quick slots and subtitles at the HUD's edges are not out at the blurry edge of the lenses\n"
    "hud_panel_fit = true\n"
    "hud_panel_fit_share = 0.70\n"
    "# fitted mode: the HUD is shrunk to cover what the flat game showed\n"
    "hud_scale_auto = true\n"
    "hud_scale = 0.60\n"
    "hud_size = 1.00\n"
    "hud_offset_x = 0.000\n"
    "hud_offset_y = 0.000\n"
    "hud_aspect_fix = false\n"
    "hud_toggle_key = f7\n"
    "\n"
    "# ---- Display (restart the game after changing)\n"
    "# render_resolution: headset = User.ini [Viewport] set to the headset's per-eye resolution and shape, asked from\n"
    "# the headset at every start (any OpenXR headset: resolution, aspect and per-eye FOV detected, a new headset is\n"
    "# picked up on its first launch); game = leave the game's resolution\n"
    "render_resolution = headset\n"
    "# headset_detect: ask the headset before the game starts (waits up to headset_detect_ms,\n"
    "# 0 = 5 s, or 15 s the first time; headset off = the last values)\n"
    "headset_detect = true\n"
    "headset_detect_ms = 0\n"
    "render_scale = 1.00\n"
    "# hide_d3d9ex: the mod's device is a D3D9Ex device (for the headset's shared textures); answered 'yes, 9Ex' the game\n"
    "# draws metal, eyes and blood in rainbow cube colours (same bug ReShade 3.0.4 caused). true = the game only sees plain D3D9.\n"
    "hide_d3d9ex = true\n"
    "# managed_buffers: exact = buffers the game rewrites after drawing get a copy and never make the frame wait for the GPU\n"
    "# (combat: blood, hit effects, damage numbers); direct = the old handling\n"
    "managed_buffers = exact\n"
    "# vr_game_antialias: the game's own anti-aliasing (a post-process edge blur that softens the picture). false = off\n"
    "vr_game_antialias = false\n"
    "# render_aspect: headset (the eye's shape), or W:H such as 1:1\n"
    "render_aspect = headset\n"
    "# render_max_side: upper limit for the longer side (D3D9 handles 8192)\n"
    "render_max_side = 8192\n"
    "# force_render_size: also force that size on the D3D back buffer (only if the game ignores User.ini)\n"
    "force_render_size = false\n"
    "# flat_when_paused: the pause menu, inventory, map, journal and character panels (the game is paused) are shown on\n"
    "# the flat virtual screen; VR returns when the game runs again. flat_on_active_pause: also during the game's\n"
    "# 'active pause' (left in VR by default)\n"
    "flat_when_paused = true\n"
    "flat_on_active_pause = false\n"
    "# fix_many_core_hang: the game hangs on its medallion splash on CPUs with 32+ logical processors (its CPU topology\n"
    "# loop never ends); the mod makes it take its own early exit - no affinity restriction, all cores kept\n"
    "fix_many_core_hang = true\n"
    "# recenter_key: Home by default (Space is dodge and F9 quickload in The Witcher 2)\n"
    "recenter_key = home\n"
    "screenshot_key = delete\n"
    "screenshot_mode = eye\n"
    "screenshot_square = true\n"
    "\n"
    "# ---- User.ini (Documents\\Witcher 2\\Config\\User.ini; original kept as User.ini.w2vr_backup)\n"
    "user_ini_tweaks = true\n"
    "# disable_depth_of_field: gameplay, cutscene and scatter DOF forced off in the renderer (and in User.ini)\n"
    "disable_depth_of_field = true\n"
    "disable_motion_blur = true\n"
    "disable_vignette = true\n"
    "disable_blur = false\n"
    "# shadow_slope_bias / shadow_depth_bias: extra depth bias while the game draws its shadow maps (only there). The\n"
    "# wide VR view stretches the sun's 1024 x 1024 shadow cascades over far more ground than in the flat game, and the\n"
    "# game's own bias stops covering it: lit surfaces shadow themselves at random (grainy dark specks in sunlight).\n"
    "# Live in the panel (Engine); 0 = the game's own\n"
    "shadow_slope_bias = 0.0\n"
    "shadow_depth_bias = 0.0\n"
    "# disable_ssao: screen-space ambient occlusion off (on by default: symmetric_projection keeps it clean)\n"
    "disable_ssao = false\n"
    "# disable_sharpen: the game's sharpen filter (AllowSharpen) - in a headset it gives grainy dark specks on bright,\n"
    "# detailed surfaces and dark halos along edges. Live renderer switch (and User.ini)\n"
    "disable_sharpen = false\n"
    "# remove_dither: OFF by default - the game also decodes colours with its 16x16 Bayer texture (made uniform, blood\n"
    "# and some highlights turn green). When on: alpha threshold of decal / ground blends, LOD fades,\n"
    "# soft shadows) is built uniform - the screen-door grain is gone at the source. remove_shadow_dither: the same\n"
    "# for the shadow filter's per-pixel rotation texture. Start-up only.\n"
    "# remove_dither = fine (test option): the pattern folded to a 2x2 grid that keeps the colour decode intact\n"
    "# (same half split per 2x2 block); the grain becomes a fine regular grid the dedither filter removes.\n"
    "remove_dither = false\n"
    "# shadow_dither_uniform: (was remove_shadow_dither) off - blood had the wrong colour ever since it was on\n"
    "shadow_dither_uniform = false\n"
    "# dedither: extra edge-preserving smoothing filter on each eye picture (0 = off, 1 = full). Not needed with\n"
    "# remove_dither; dedither_range = relative brightness difference still treated as dither.\n"
    "dedither = 0.0\n"
    "dedither_range = 0.35\n"
    "# texture_filtering_vr: material textures (not render targets) - the game's negative mip LOD bias raised to\n"
    "# texture_lod_bias_min, point mip filtering made trilinear, anisotropic filtering (texture_anisotropy). Stops fine\n"
    "# texture detail sparkling and crawling when the head moves. Live in the VR panel (Engine)\n"
    "texture_filtering_vr = true\n"
    "# managed_lod_emulation: the game's texture streaming limits each texture's sharpest mip level with SetLOD, which\n"
    "# does nothing with the D3D9Ex device the VR mod needs - not-yet-loaded mip levels (garbage) were drawn. Emulated.\n"
    "managed_lod_emulation = true\n"
    "# vr_texture_budget: TextureMemoryBudget (MB) raised to at least this for the headset-size frame (its render targets\n"
    "# take hundreds of MB the game's 100 - 600 MB budget never planned for; too little = broken / black textures). 0 = off\n"
    "vr_texture_budget = 600\n"
    "# vr_texture_quality: max = full-resolution textures every start (TextureDownscale 0, sizes repaired); keep = User.ini as is\n"
    "vr_texture_quality = max\n"
    "# managed_direct_lock: the game's textures live once, in video memory, and are written there directly (no system-memory\n"
    "# copy that could go out of step, and half the memory in the 32-bit game). false = the old copy-and-upload path\n"
    "managed_direct_lock = true\n"
    "# smoothing_lock: while the headset's frame smoothing runs (Pimax Smart Smoothing, SteamVR Motion Smoothing, ASW),\n"
    "# pace the game at exactly the rate the headset asks for - even frame spacing for the synthesis\n"
    "smoothing_lock = true\n"
    "texture_lod_bias_min = 0.0\n"
    "texture_anisotropy = 16\n"
    "# full_stereo_tolerant: full stereo also takes a frame whose camera the game nudged after the build (shake, cutscene\n"
    "# touch-ups: within 10 cm / 1 degree). false = only bit-identical frames (0.4.7 behaviour)\n"
    "full_stereo_tolerant = true\n"
    "ini_vsync_off = true\n"
    "\n"
    "# ---- Frame pacing\n"
    "# frame_limit (live in the VR panel): off, headset (the headset's refresh rate), headset_x2, or a number in fps\n"
    "frame_limit = headset\n"
    "# stereo_method: full = full stereo - the renderer draws BOTH eyes from every game tick (one simulation, two eye\n"
    "# cameras; falls back to same_tick by itself if the renderer hook is not reached); same_tick = alternate eyes with\n"
    "# the game's time standing still for the second eye; aer = plain alternate eyes, a frame apart\n"
    "# auto = follow the headset: same_tick at its full refresh, full stereo while its frame smoothing / reprojection\n"
    "# runs it at a reduced rate (consistent eye pairs for the synthesised frames)\n"
    "stereo_method = auto\n"
    "# same_tick_clock: also hold REDengine's clock (timers, animation, effects) for the second eye\n"
    "same_tick_clock = true\n"
    "# same_tick_camera: the second eye reuses the first eye's game camera (raw mouse input still turns it)\n"
    "same_tick_camera = true\n"
    "\n"
    "# ---- Flat screen (menus, loading screens, videos) and the VR panel\n"
    "screen_distance = 2.0\n"
    "screen_width = 2.6\n"
    "menu_width = 0.60\n"
    "menu_opacity = 0.80\n"
    "\n"
    "# ---- Start-up options (restart the game after changing)\n"
    "# force_windowed: User.ini Fullscreen off, D3D windowed, window styles - the game always runs in a window\n"
    "force_windowed = true\n"
    "window_mode = bordered\n"
    "# window_scale: 1 = native - the window's client is the real VR resolution and shape, larger than the monitor if\n"
    "# need be (no screen-size cap); auto = shrink the window to fit the monitor (the headset still gets the full frame)\n"
    "window_scale = 1\n"
    "# mouse_fit_screen: with a window larger than the monitor, the part on screen is mapped to the whole picture, so\n"
    "# every menu button stays in reach of the mouse\n"
    "mouse_fit_screen = true\n"
    "# dpi_aware: Windows display scaling does not stretch the window\n"
    "dpi_aware = true\n"
    "keep_running_unfocused = true\n"
    "present_immediate = true\n"
    "mirror_right_eye = true\n"
    "d3d9ex = true\n"
    "# block_nvidia_stereo: keep the game's built-in NVIDIA 3D Vision path off (it would stack on the VR eyes)\n"
    "block_nvidia_stereo = true\n"
    "# patch_shaders: auto = vertex shaders are analysed (draw classification, HUD panel) and only patched when needed\n"
    "# (camera_mode = shader, or hud_mode fitted / full); otherwise the game's shaders run unchanged. true / false\n"
    "patch_shaders = auto\n"
    "\n"
    "# ---- 0.7.0\n"
    "# pad_without_focus: the game reads the pad only after DirectInput (keyboard / mouse), and gives up when that is lost\n"
    "# because its window is not in front - the pad then does nothing. A lost device now reports 'nothing pressed' instead.\n"
    "pad_without_focus = true\n"
    "# window_to_front: bring the game window to the front once when VR starts (keyboard / mouse need it)\n"
    "window_to_front = true\n"
    "# hud_native_aspect: the game lays its HUD out for the frame's real aspect ratio instead of 16:9 (edges were cut off)\n"
    "hud_native_aspect = true\n"
    "# diag_stage_dump: once per session (and on F10) one frame is saved stage by stage to W2VR\\stages - for diagnosis\n"
    "diag_stage_dump = false\n"
    "# camera_origin: the head pose is applied at the origin - every camera the viewport hands out in a game tick (the\n"
    "# renderer's frame, HUD markers, visibility ...) gets the same head-tracked view, one pose per tick. false = the old\n"
    "# way: one camera picked by votes\n"
    "camera_origin = true\n"
    "# managed_mode: exact = textures emulate Direct3D's managed pool exactly (every write to a system-memory copy, the\n"
    "# whole texture uploaded before use); direct / copy = the older approximations\n"
    "managed_mode = exact\n"
    "# diag_vanilla: true = a comparison run of the untouched game (no VR): F10 saves the frame's stages\n"
    "diag_vanilla = false\n"
    "# normal_repair: the game's G-buffer normals arrive with about 60 % of the pixels empty (unlit: the dark specks);\n"
    "# they are renormalised and filled from their neighbours before the lighting runs. false = off\n"
    "normal_repair = false\n"
    "# smoothing_follow: false = the game always runs at its own full rate (one eye per frame), even while the headset\n"
    "# has dropped to half rate; true = switch to both-eyes-per-frame at the headset's half rate (0.6.1 - 0.9.0)\n"
    "# headset_smoothing (live: VR panel > Engine): always / drops / off\n"
    "headset_smoothing = drops\n"
    "\n"
    "# keys: Insert = VR panel, End = first person, Home = recentre, F8 = stereo / head / screen, F7 = HUD mode,\n"
    "# Delete = screenshot, F10 = save diagnostic images\n";

static void Init() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_gameDir = exe; g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L'\\'));
    g_modDir = g_gameDir + L"\\" GAME_WDIR;
    CreateDirectoryW(g_modDir.c_str(), nullptr);
    LogInit((g_gameDir + L"\\" GAME_WFILE L".log").c_str());
    std::wstring cfg = g_gameDir + L"\\" GAME_WFILE L"_config.txt";
    if (GetFileAttributesW(cfg.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(cfg.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, kDefaultConfig, (DWORD)strlen(kDefaultConfig), &w, nullptr); CloseHandle(f); }
    }
    {   // once (0.9.1): render_scale 1.00 -> 0.90 in an existing config file (asked for; 19 % fewer pixels)
        std::wstring mark = g_modDir + L"\\scale_091.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            size_t p = t.find("\nrender_scale = 1.00");
            if (p != std::string::npos) { t.replace(p, 20, "\nrender_scale = 0.90"); if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); } }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("render_scale set to 0.90 once (W2VR 0.9.1)\n", f); fclose(f); }
        }
    }
    {   // once (1.0.5): texture budget 1024 -> 600, a value the game's Configurator knows ("VeryHighBudget")
        std::wstring mark = g_modDir + L"\\budget_105.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            size_t p = t.find("\nvr_texture_budget = 1024");
            if (p != std::string::npos) { t.replace(p, 25, "\nvr_texture_budget = 600"); if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); } }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("texture budget set to 600 once (W2VR 1.0.5)\n", f); fclose(f); }
        }
    }
    {   // once (1.0.3): edge fill margin 6 -> 4 degrees (smaller eye images for the headset: less video memory)
        std::wstring mark = g_modDir + L"\\margin_103.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            size_t p = t.find("\nedge_fill_margin = 6");
            if (p != std::string::npos) { t.replace(p, 21, "\nedge_fill_margin = 4"); if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); } }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("edge fill margin set to 4 once (W2VR 1.0.3)\n", f); fclose(f); }
        }
    }
    {   // once (1.0.1): smooth edge fill back on (it only fills the area around the picture, never the picture itself)
        std::wstring mark = g_modDir + L"\\edge_101.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            size_t p = t.find("\nedge_fill_soft = false");
            if (p != std::string::npos) { t.replace(p, 24, "\nedge_fill_soft = true"); if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); } }
        }
        if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("smooth edge fill set once (W2VR 1.0.1)\n", f); fclose(f); }
    }
    {   // once (1.0.0, asked for): sharp + fast defaults - every frame real (smoothing only when fps drops), no soft edge
        // fill, no diagnostic stage recordings
        std::wstring mark = g_modDir + L"\\perf_100.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            auto setv = [&](const char* key, const char* val) {
                std::string k = std::string("\n") + key + " = "; size_t p = t.find(k);
                if (p == std::string::npos) return;
                size_t v = p + k.size(), e = t.find_first_of("\r\n", v); if (e == std::string::npos) e = t.size();
                t.replace(v, e - v, val); };
            setv("headset_smoothing", "drops"); setv("diag_stage_dump", "false");
            if (!t.empty()) if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("sharp / fast defaults set once (W2VR 1.0.0)\n", f); fclose(f); }
        }
    }
    {   // once (0.9.8, asked for): render_scale back to 1.00
        std::wstring mark = g_modDir + L"\\scale_098.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            size_t p = t.find("\nrender_scale = ");
            if (p != std::string::npos) { size_t e = t.find_first_of("\r\n", p + 16); if (e == std::string::npos) e = t.size(); t.replace(p + 16, e - p - 16, "1.00"); if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); } }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("render_scale set to 1.00 once (W2VR 0.9.8)\n", f); fclose(f); }
        }
    }
    {   // once (0.9.3, asked for): de-dither filter off, the game's sharpen on
        std::wstring mark = g_modDir + L"\\fx_093.txt";
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string t; if (FILE* f = _wfopen(cfg.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n); fclose(f); }
            auto setKey = [&](const char* key, const char* val) {
                std::string k = std::string("\n") + key + " = "; size_t p = t.find(k);
                if (p == std::string::npos) { t += std::string(key) + " = " + val + "\r\n"; return; }
                size_t e = t.find_first_of("\r\n", p + k.size()); if (e == std::string::npos) e = t.size();
                t.replace(p + k.size(), e - p - k.size(), val);
            };
            if (!t.empty()) {
                setKey("dedither", "0.00"); setKey("disable_sharpen", "false");
                if (FILE* f = _wfopen(cfg.c_str(), L"wb")) { fwrite(t.data(), 1, t.size(), f); fclose(f); }
            }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("dedither 0, sharpen on - set once (W2VR 0.9.3)\n", f); fclose(f); }
        }
    }
    ConfigLoad(cfg.c_str());
    char pn[64]; WideCharToMultiByte(CP_UTF8, 0, g_proxyName.c_str(), -1, pn, 64, nullptr, nullptr);
    char en[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, exe, -1, en, MAX_PATH, nullptr, nullptr);
    LOGI(GAME_SHORT " %s loaded as %s (pid %lu) in %s", AC2VR_VERSION, pn, GetCurrentProcessId(), en);
    SetUnhandledExceptionFilter(CrashFilter);
    if (MH_Initialize() != MH_OK) LOGE("MH_Initialize failed");
    W2IniApply();                  // before the game reads User.ini
    W2EngineEarly();               // the HUD's aspect table (plain data)
    if (!ConfigBool("diag_vanilla", false)) W2CamInstall("startup");       // an unpacked exe: hooked now; the Steam exe (SteamStub): at device creation
    W2CompatInstall();             // many-core start-up hang (before the game's WinMain reads the CPU topology)
    WindowHooksInstall();          // before the game's WinMain creates its window
    D3D9HooksInstall();            // and Direct3DCreate9 is redirected in time
    SameTickInstall();
    {   // The Witcher 2 has native NVIDIA 3D Vision support (NvAPI stereo handle, stereo-parameter texture, convergence
        // per camera); with a 3D Vision driver it would duplicate every draw on top of the VR eyes. block_nvidia_stereo:
        // the game's own load of nvapi.dll fails, so its stereo path never starts (the VR mod does not need NvAPI).
        extern bool W2BlockNvapiInstall(); if (ConfigBool("block_nvidia_stereo", true)) W2BlockNvapiInstall();
    }
    {   // the 6DOF head-tracking ASI hooks the same camera: two mods turning one camera do not mix
        wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH); std::wstring d = p; d = d.substr(0, d.find_last_of(L'\\'));
        if (GetFileAttributesW((d + L"\\TheWitcher2-HeadTracking.asi").c_str()) != INVALID_FILE_ATTRIBUTES)
            LOGW("TheWitcher2-HeadTracking.asi is installed next to the game: remove it (or its dinput8.dll loader) while playing in VR");
    }
    CreateThread(nullptr, 0, FlushThread, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        ProxyLoad(h);
        wchar_t mx[64]; wsprintfW(mx, L"Local\\" GAME_WDIR L"_%lu", GetCurrentProcessId());
        HANDLE m = CreateMutexW(nullptr, TRUE, mx);
        if (m && GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
        // only the game: other programs in bin load xinput1_3.dll too (Configurator.exe for its pad settings). There
        // the mod is a plain XInput forwarder - no hooks, no log, no User.ini (0.5.1 and earlier hooked the
        // Configurator's window and controller and overwrote w2vr.log).
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* bn = wcsrchr(exe, L'\\'); bn = bn ? bn + 1 : exe;
        if (_wcsicmp(bn, L"witcher2.exe") != 0) return TRUE;
        Init();
    } else if (reason == DLL_PROCESS_DETACH) {
        LogFlushNow();   // the last buffered lines
    }
    return TRUE;
}
