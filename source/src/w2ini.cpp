// User.ini tweaks for VR (Documents\Witcher 2\Config\User.ini), applied in DllMain - before the game reads the file.
//  * [Viewport] Width / Height: the game renders at the headset's per-eye resolution (from W2VR\headset_size.txt,
//    which the mod writes once the headset has reported it - so the very first VR launch still uses the old size).
//  * [Rendering] Fullscreen off (VR runs windowed), VSync off (the mod paces frames to the headset). Nothing else:
//    quality and effect settings stay as the user set them (effects are switched in the renderer instead).
// The original file is copied once to User.ini.w2vr_backup. Existing values keep their style (true/false or 1/0).
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>
#include "log.h"
#include "config.h"
#include "w2vr.h"
bool XrHeadsetDetect(unsigned timeoutMs);   // xr.cpp

namespace {
std::wstring DocumentsDir() {
    // registry instead of shell32 (DllMain: loader lock)
    wchar_t raw[MAX_PATH * 2] = {}, out[MAX_PATH * 2] = {};
    DWORD sz = sizeof raw, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", L"Personal",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, raw, &sz) == ERROR_SUCCESS) {
        if (ExpandEnvironmentStringsW(raw, out, MAX_PATH * 2)) return out;
        return raw;
    }
    if (GetEnvironmentVariableW(L"USERPROFILE", out, MAX_PATH)) return std::wstring(out) + L"\\Documents";
    return L"";
}
struct Line { std::string text; };
std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string Trim(const std::string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }

bool ReadAll(const std::wstring& p, std::string& out, bool* utf16) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return false;
    std::vector<char> b; char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
    fclose(f);
    *utf16 = b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE;
    if (*utf16) {
        std::wstring w((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
        int m = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        out.assign(m, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], m, nullptr, nullptr);
    } else out.assign(b.begin(), b.end());
    return true;
}
bool WriteAll(const std::wstring& p, const std::string& s, bool utf16) {
    FILE* f = _wfopen(p.c_str(), L"wb"); if (!f) return false;
    if (utf16) {
        int m = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(m, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], m);
        fwrite("\xFF\xFE", 1, 2, f); fwrite(w.data(), 2, w.size(), f);
    } else fwrite(s.data(), 1, s.size(), f);
    fclose(f); return true;
}

struct Ini {
    std::vector<std::string> lines; bool crlf = true; int changes = 0; std::string log;
    void Parse(const std::string& s) {
        crlf = s.find("\r\n") != std::string::npos;
        size_t a = 0;
        while (a <= s.size()) { size_t e = s.find('\n', a); if (e == std::string::npos) e = s.size(); std::string l = s.substr(a, e - a); if (!l.empty() && l.back() == '\r') l.pop_back(); lines.push_back(l); a = e + 1; if (e == s.size()) break; }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
    }
    std::string Text() const { std::string o; for (auto& l : lines) { o += l; o += crlf ? "\r\n" : "\n"; } return o; }
    // section range [begin, end) of lines after its header; -1 if missing
    int Section(const std::string& sec, int* end) const {
        int start = -1;
        for (int i = 0; i < (int)lines.size(); i++) {
            std::string t = Trim(lines[i]);
            if (t.size() > 2 && t[0] == '[') {
                if (start >= 0) { *end = i; return start; }
                if (Lower(t) == "[" + Lower(sec) + "]") start = i + 1;
            }
        }
        *end = (int)lines.size(); return start;
    }
    // value style of booleans already in the file
    bool DigitsStyle() const {
        int t = 0, d = 0;
        for (auto& l : lines) { size_t e = l.find('='); if (e == std::string::npos) continue; std::string v = Lower(Trim(l.substr(e + 1))); if (v == "true" || v == "false") t++; else if (v == "0" || v == "1") d++; }
        return d > t;
    }
    void Set(const std::string& sec, const std::string& key, const std::string& value, bool isBool) {
        int end, s = Section(sec, &end);
        if (s < 0) { lines.push_back("[" + sec + "]"); s = (int)lines.size(); end = s; }
        for (int i = s; i < end; i++) {
            size_t e = lines[i].find('='); if (e == std::string::npos) continue;
            if (Lower(Trim(lines[i].substr(0, e))) != Lower(key)) continue;
            std::string old = Trim(lines[i].substr(e + 1)), v = value;
            if (isBool) { bool on = value == "true"; std::string lo = Lower(old); v = (lo == "0" || lo == "1") ? (on ? "1" : "0") : (on ? "true" : "false"); if (lo == Lower(v)) return; }
            else if (old == v) return;
            lines[i] = lines[i].substr(0, e + 1) + v;
            changes++; log += " " + key + "=" + v + " (was " + old + ")";
            return;
        }
        std::string v = value;
        if (isBool && DigitsStyle()) v = value == "true" ? "1" : "0";
        lines.insert(lines.begin() + end, key + "=" + v);
        changes++; log += " " + key + "=" + v + " (added)";
    }
};

bool RenderSizeFromHeadset(unsigned* w, unsigned* h) {
    if (ConfigBool("headset_detect", true)) {   // ask the headset now (writes headset_size.txt); any OpenXR headset
        bool cached = GetFileAttributesW((g_modDir + L"\\headset_size.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
        int ms = ConfigInt("headset_detect_ms", 0); if (ms <= 0) ms = cached ? 5000 : 15000;
        XrHeadsetDetect((unsigned)ms);
    }
    unsigned rw = 0, rh = 0; float rx = 1, ry = 1;
    if (FILE* f = _wfopen((g_modDir + L"\\headset_size.txt").c_str(), L"r")) {
        int n = fscanf(f, "%u %u %f %f", &rw, &rh, &rx, &ry);
        if (n < 2) rw = rh = 0;
        if (n < 4 || !(rx >= 1.f && rx < 3.f) || !(ry >= 1.f && ry < 3.f)) rx = ry = 1;   // (wide, very asymmetric eyes reach ~2.x)
        fclose(f);
    }
    if (!rw || !rh) return false;
    if (ConfigBool("symmetric_projection", true) && (rx > 1.f || ry > 1.f)) {   // the centred view is wider: same pixel density
        LOGI("ini: symmetric eye view - render size x %.3f / %.3f (%ux%u -> %ux%u)", rx, ry, rw, rh, (unsigned)(rw * rx), (unsigned)(rh * ry));
        rw = (unsigned)(rw * rx + 0.5f); rh = (unsigned)(rh * ry + 0.5f);
    }
    std::string a = ConfigStr("render_aspect", "headset");
    float aspect = (float)rw / (float)rh, x = 0, y = 0;
    if (a != "headset" && sscanf(a.c_str(), "%f:%f", &x, &y) == 2 && x > 0 && y > 0) aspect = x / y;
    float sc = ConfigFloat("render_scale", 1.0f); if (sc < 0.25f) sc = 0.25f; if (sc > 3.f) sc = 3.f;
    // the longer side keeps the headset's longer side (x render_scale), the other follows the aspect
    float side = (float)(rh > rw ? rh : rw) * sc, fw, fh;
    if (aspect >= 1.f) { fw = side; fh = side / aspect; } else { fh = side; fw = side * aspect; }
    float mx = (float)ConfigInt("render_max_side", 8192);
    if (fw > mx) { fh *= mx / fw; fw = mx; }
    if (fh > mx) { fw *= mx / fh; fh = mx; }
    // nearest multiple of 4 (the headset's own sizes are, so at render_scale 1 this is its exact size and ratio)
    *w = ((unsigned)(fw + 2.f)) & ~3u; *h = ((unsigned)(fh + 2.f)) & ~3u;
    return *w >= 320 && *h >= 240;
}
unsigned g_resW = 0, g_resH = 0;   // the resolution the game will run at (from User.ini after the tweaks)
unsigned IniInt(const Ini& ini, const char* sec, const char* key) {
    int end, s = ini.Section(sec, &end);
    if (s < 0) return 0;
    for (int i = s; i < end; i++) {
        size_t e = ini.lines[i].find('='); if (e == std::string::npos) continue;
        if (Lower(Trim(ini.lines[i].substr(0, e))) == Lower(key)) return (unsigned)strtoul(Trim(ini.lines[i].substr(e + 1)).c_str(), nullptr, 10);
    }
    return 0;
}
} // namespace

bool W2IniResolution(unsigned* w, unsigned* h) { if (!g_resW || !g_resH) return false; *w = g_resW; *h = g_resH; return true; }

void W2IniApply() {
    if (!ConfigBool("user_ini_tweaks", true)) { LOGI("ini: user_ini_tweaks = false - User.ini left alone"); return; }
    std::wstring doc = DocumentsDir();
    std::wstring path = ConfigStr("user_ini_path", "").empty() ? doc + L"\\Witcher 2\\Config\\User.ini" : L"";
    if (path.empty()) { std::string p = ConfigStr("user_ini_path", ""); path.assign(p.begin(), p.end()); }
    std::string text; bool utf16 = false;
    char pn[MAX_PATH * 2]; WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, pn, sizeof pn, nullptr, nullptr);
    if (!ReadAll(path, text, &utf16)) { LOGW("ini: %s not found - start the game once without the mod (or set user_ini_path)", pn); return; }
    std::wstring bak = path + L".w2vr_backup";
    if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES) { CopyFileW(path.c_str(), bak.c_str(), TRUE); LOGI("ini: original saved as User.ini.w2vr_backup"); }
    Ini ini; ini.Parse(text);
    if (ConfigBool("force_windowed", true)) ini.Set("Rendering", "Fullscreen", "false", true);
    if (ConfigBool("ini_vsync_off", true)) ini.Set("Rendering", "VSync", "false", true);
    if (!ConfigBool("disable_sharpen", false)) ini.Set("Rendering", "AllowSharpen", "true", true);
    if (!ConfigBool("vr_game_antialias", false)) ini.Set("Rendering", "AllowAntialias", "0", true);   // its edge blur softens the headset picture   // asked for: the game's sharpen on
    // Repair (not a preference): sizes of 0 are not valid values - the game then caps textures / shadow maps at its
    // minimum (smeared textures). The Configurator writes them when it cannot match the file to its lists.
    {
        auto get = [&](const Ini& f, const char* k) { int e, st = f.Section("Rendering", &e); if (st < 0) return std::string();
            for (int i = st; i < e; i++) { size_t q = f.lines[i].find('='); if (q != std::string::npos && Lower(Trim(f.lines[i].substr(0, q))) == Lower(k)) return Trim(f.lines[i].substr(q + 1)); }
            return std::string(); };
        auto num = [](const std::string& v) { return v.empty() ? -1.0 : atof(v.c_str()); };
        unsigned sq = IniInt(ini, "Rendering", "ShadowQuality");
        struct Fix { const char* k; double bad; const char* good; } fixes[] = {
            { "MaxTextureSize", 0, "2048" }, { "MaxAtlasTextureSize", 0, "4096" },
            { "MaxCubeShadowSize", 0, sq >= 3 ? "2048" : "1024" }, { "MaxSpotShadowSize", 0, sq >= 3 ? "2048" : "1024" } };
        // 0.6.2: also tiny values (the Configurator writes list positions such as 1 when it cannot match the file): a size
        // below 256 is never meant - MaxTextureSize=1 shrank every texture to a smear
        for (auto& f : fixes) {
            double v = num(get(ini, f.k));
            if (v >= 0 && v < 256) { LOGW("ini: %s=%s is not a valid size - set to %s", f.k, get(ini, f.k).c_str(), f.good); ini.Set("Rendering", f.k, f.good, false); }
        }
        // vr_texture_quality = max (default): full-resolution textures. The Configurator resets TextureDownscale when it
        // saves (and in the headset low-resolution textures are blur); keep = leave what the file says
        if (ConfigStr("vr_texture_quality", "max") == "max") {
            for (const char* k : { "TextureDownscale", "AtlasTextureDownscale", "DetailTextureDownscale" }) {
                std::string v = get(ini, k);
                if (!v.empty() && num(v) > 0) { LOGI("ini: %s %s -> 0 (full resolution; vr_texture_quality = keep to leave it)", k, v.c_str()); ini.Set("Rendering", k, "0", false); }
            }
        }
        // vr_texture_budget (MB): the headset's frame is ~4352 x 2668 - several times a monitor's - and its render targets
        // take hundreds of MB that the game's 100 - 600 MB TextureMemoryBudget was sized without. With too little budget
        // the texture streamer runs dry (Nexus reports: black / broken textures on RTX 30/40, fixed with 2048 - 4096).
        // Only raised, never lowered; 0 = leave User.ini alone.
        {
            // 0.9.4: 1024 - with exact managed-pool emulation every texture also has a copy inside the 32-bit game (as with
            // the real managed pool), so a 2048 MB budget could run the game out of address space. 2048 set by 0.6.1 - 0.9.3 is lowered.
            // 1.0.5: 600 (the Configurator's "VeryHighBudget"). The Configurator only knows 100 / 200 / 300 / 400 / 600: with
            // 1024 it showed "VerySmallBudget" and wrote 100 back on OK. The game uses under 300 MB in VR (log), 600 is plenty.
            int want = ConfigInt("vr_texture_budget", 600);
            {   double c = num(get(ini, "TextureMemoryBudget"));
                bool known = c == 100 || c == 200 || c == 300 || c == 400 || c == 600;
                if (want > 0 && c > want && !known) { ini.Set("Rendering", "TextureMemoryBudget", std::to_string(want), false); LOGI("ini: TextureMemoryBudget %.0f -> %d MB (a value the Configurator can show)", c, want); } }
            double cur = num(get(ini, "TextureMemoryBudget"));
            if (want > 0 && cur >= 0 && cur < want) {
                ini.Set("Rendering", "TextureMemoryBudget", std::to_string(want), false);
                LOGI("ini: TextureMemoryBudget %.0f -> %d MB for the headset-size frame (vr_texture_budget; 0 = off)", cur, want);
            }
        }
        // once (0.5.2): texture quality, decals and rain back to the original User.ini (User.ini.w2vr_backup, saved
        // before the mod's first change) wherever that was better - the values lost to the Configurator
        std::wstring mark = g_modDir + L"\\ini_restored_052.txt";
        std::string btxt; bool b16 = false;
        if (GetFileAttributesW(mark.c_str()) == INVALID_FILE_ATTRIBUTES && ReadAll(path + L".w2vr_backup", btxt, &b16)) {
            Ini bak; bak.Parse(btxt);
            struct R { const char* k; bool higherBetter; } rs[] = { { "TextureDownscale", false }, { "AtlasTextureDownscale", false }, { "DetailTextureDownscale", false },
                { "TextureMemoryBudget", true }, { "MaxTextureSize", true }, { "AllowDecals", true }, { "AllowRain", true }, { "MeshDistanceScale", true } };
            for (auto& r : rs) {
                std::string bv = get(bak, r.k), cv = get(ini, r.k);
                if (bv.empty()) continue;
                double b = num(bv), c = num(cv);
                if (cv.empty() || (r.higherBetter ? b > c : b < c)) ini.Set("Rendering", r.k, bv, false);
            }
            if (FILE* f = _wfopen(mark.c_str(), L"w")) { fputs("User.ini quality restored once from User.ini.w2vr_backup (W2VR 0.5.2). Delete this file to do it again.\n", f); fclose(f); }
        }
    }
    // Quality and effect settings are never written: depth of field, motion blur, vignette and sharpen are forced off
    // in the renderer (w2engine.cpp, live), so User.ini keeps exactly what you chose in the Configurator / in-game
    // options. Only what VR needs is written: windowed, VSync off, the render resolution.
    {   // what the game will use (diagnosis: the Configurator saves whatever it shows - see the README)
        auto v = [&](const char* k) { int e, st = ini.Section("Rendering", &e); if (st < 0) return std::string("-");
            for (int i = st; i < e; i++) { size_t q = ini.lines[i].find('='); if (q != std::string::npos && Lower(Trim(ini.lines[i].substr(0, q))) == Lower(k)) return Trim(ini.lines[i].substr(q + 1)); }
            return std::string("-"); };
        LOGI("ini: quality the game will use (User.ini; the mod only repairs invalid 0 sizes and, once, restores your original from the backup): TextureDownscale=%s TextureMemoryBudget=%s MaxTextureSize=%s ShadowQuality=%s ShadowedLights=%s "
             "MaxCubeShadowSize=%s MaxSpotShadowSize=%s AllowDecals=%s AllowRain=%s AllowSSAO=%s AllowAntialias=%s AllowBloom=%s AllowShafts=%s UberSampling=%s MeshDistanceScale=%s",
             v("TextureDownscale").c_str(), v("TextureMemoryBudget").c_str(), v("MaxTextureSize").c_str(), v("ShadowQuality").c_str(), v("ShadowedLights").c_str(),
             v("MaxCubeShadowSize").c_str(), v("MaxSpotShadowSize").c_str(), v("AllowDecals").c_str(), v("AllowRain").c_str(), v("AllowSSAO").c_str(),
             v("AllowAntialias").c_str(), v("AllowBloom").c_str(), v("AllowShafts").c_str(), v("UberSampling").c_str(), v("MeshDistanceScale").c_str());
        std::string td = v("TextureDownscale"), dc = v("AllowDecals"), rn = v("AllowRain");
        if ((td != "0" && td != "-") || dc == "0" || dc == "0.0" || rn == "0")
            LOGW("ini: User.ini has reduced quality (TextureDownscale=%s, AllowDecals=%s, AllowRain=%s) - set them in the Configurator: TextureDownscale NoDownscale, AllowDecals HIGH, AllowRain Enabled",
                 td.c_str(), dc.c_str(), rn.c_str());
    }
    unsigned w = 0, h = 0;
    if (ConfigStr("render_resolution", "headset") == "headset") {
        if (RenderSizeFromHeadset(&w, &h)) {
            // the engine reads [Viewport] Width / Height into both its fullscreen and windowed sizes (witcher2.exe
            // 0xB07727..0xB0788A); FullScreenWidth / ... / WindowedHeight are a legacy fallback read only when Width or
            // Height is missing - written too, so both agree
            ini.Set("Viewport", "Width", std::to_string(w), false); ini.Set("Viewport", "Height", std::to_string(h), false);
            ini.Set("Viewport", "WindowedWidth", std::to_string(w), false); ini.Set("Viewport", "WindowedHeight", std::to_string(h), false);
        }
        else LOGI("ini: headset resolution not known yet - this launch keeps the game's resolution; the next one uses the headset's");
    }
    g_resW = IniInt(ini, "Viewport", "Width"); g_resH = IniInt(ini, "Viewport", "Height");   // what the game uses
    if (!g_resW || !g_resH) { g_resW = IniInt(ini, "Viewport", "WindowedWidth"); g_resH = IniInt(ini, "Viewport", "WindowedHeight"); }
    if (g_resW && g_resH) LOGI("ini: the game will run windowed at %ux%u", g_resW, g_resH);
    if (!ini.changes) { LOGI("ini: User.ini already set up for VR%s", w ? (" (" + std::to_string(w) + "x" + std::to_string(h) + ")").c_str() : ""); return; }
    if (WriteAll(path, ini.Text(), utf16)) LOGI("ini: User.ini updated:%s", ini.log.c_str());
    else LOGW("ini: could not write %s (read-only?)", pn);
}
