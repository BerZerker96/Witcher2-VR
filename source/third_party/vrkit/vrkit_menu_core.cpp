#include "vrkit_menu_core.h"
#include "vrkit_options.h"
#include <windows.h>
#include <vector>
#include <algorithm>
#include "imgui.h"

namespace vrkit { namespace menu { namespace core {

namespace {
Settings g_s;
ImGuiContext* g_im = nullptr;
ImFont* g_fBody = nullptr; ImFont* g_fTitle = nullptr; ImFont* g_fTab = nullptr;
bool g_open = false;
int g_page = 0, g_sel = 0;
LARGE_INTEGER g_qpf, g_last;

std::vector<int> PageItems(int page) {
    auto& opts = Options::Get().All();
    auto pages = Options::Get().Pages();
    std::vector<int> r;
    if (pages.empty()) return r;
    page = (std::min)(page, (int)pages.size() - 1);
    for (int k = 0; k < (int)opts.size(); ++k)
        if (opts[k].page == pages[page] && (!opts[k].visible || opts[k].visible())) r.push_back(k);
    return r;
}

void Close() {
    g_open = false;
    input::SetBlocking(false);
    if (Options::Get().Dirty() && !g_s.configPath.empty()) {
        if (Options::Get().Save(g_s.configPath)) Log("vrkit: settings saved");
    }
}

void Navigate(uint32_t b) {
    auto pages = Options::Get().Pages();
    if (pages.empty()) return;
    if (b & input::PagePrev) { g_page = (g_page + (int)pages.size() - 1) % (int)pages.size(); g_sel = 0; }
    if (b & input::PageNext) { g_page = (g_page + 1) % (int)pages.size(); g_sel = 0; }
    auto items = PageItems(g_page);
    if (items.empty()) return;
    int n = (int)items.size();
    if (b & input::Up) g_sel = (g_sel + n - 1) % n;
    if (b & input::Down) g_sel = (g_sel + 1) % n;
    g_sel = (std::min)(g_sel, n - 1);
    Option& o = Options::Get().All()[items[g_sel]];
    if (b & input::Left) Options::Get().Step(o, -1);
    if (b & input::Right) Options::Get().Step(o, +1);
    if (b & input::Accept) Options::Get().Activate(o);
    if (b & input::Reset) Options::Get().Reset(o);
}
} // namespace

bool InitImGui(const Settings& s) {
    if (g_im) return true;
    g_s = s;
    input::Configure(s.input);
    input::InstallHooks();
    IMGUI_CHECKVERSION();
    g_im = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_im);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)s.texW, (float)s.texH);
    // a clear UI font from Windows at the requested size; ImGui's built-in font as fallback
    float os = s.fontOversample > 0 ? s.fontOversample : 1.f;
    ImFont* f = nullptr;
    wchar_t win[MAX_PATH]; char path[MAX_PATH * 2];
    if (GetWindowsDirectoryW(win, MAX_PATH)) {
        for (const wchar_t* name : { L"\\Fonts\\segoeui.ttf", L"\\Fonts\\arial.ttf" }) {
            std::wstring p = std::wstring(win) + name;
            WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, path, sizeof(path), nullptr, nullptr);
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && (f = io.Fonts->AddFontFromFileTTF(path, s.fontPx * os))) break;
        }
    }
    if (!f) { ImFontConfig fc; fc.SizePixels = s.fontPx * os; f = io.Fonts->AddFontDefault(&fc); }
    g_fBody = g_fTitle = g_fTab = f;
    if (s.theme == 1 && GetWindowsDirectoryW(win, MAX_PATH)) {
        // serif faces for the title and the page names (Palatino / Georgia / Constantia ship with Windows)
        for (const wchar_t* name : { L"\\Fonts\\palab.ttf", L"\\Fonts\\georgiab.ttf", L"\\Fonts\\constanb.ttf", L"\\Fonts\\pala.ttf", L"\\Fonts\\georgia.ttf" }) {
            std::wstring p = std::wstring(win) + name;
            if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, path, sizeof(path), nullptr, nullptr);
            ImFont* t = io.Fonts->AddFontFromFileTTF(path, s.fontPx * 1.45f * os);
            ImFont* b = io.Fonts->AddFontFromFileTTF(path, s.fontPx * 1.02f * os);
            if (t && b) { g_fTitle = t; g_fTab = b; }
            break;
        }
    }
    io.FontGlobalScale = 1.f / os;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 14.f; st.WindowBorderSize = 0.f; st.WindowPadding = ImVec2(0, 0);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.035f, 0.045f, s.backgroundAlpha);   // dark, semi-transparent
    QueryPerformanceFrequency(&g_qpf); QueryPerformanceCounter(&g_last);
    return true;
}

void ShutdownImGui() {
    if (g_im) { ImGui::DestroyContext(g_im); g_im = nullptr; }
    g_open = false;
}

const Settings& Get() { return g_s; }

bool UpdateInput() {
    if (!g_im) return false;
    static int rehook = 0;
    if ((++rehook % 300) == 1) input::InstallHooks();   // the game may load XInput late
    uint32_t b = input::Poll();
    if (b & input::Toggle) { if (g_open) Close(); else { g_open = true; input::SetBlocking(true); } b &= ~input::Toggle; }
    else if (g_open && (b & input::Back)) { Close(); return false; }
    if (!g_open) return false;
    Navigate(b);
    ImGui::SetCurrentContext(g_im);
    return true;
}

static void BuildDefault();
static void BuildWitcher();
void BuildFrame() { if (g_s.theme == 1) BuildWitcher(); else BuildDefault(); }

static void BuildDefault() {
    ImGui::SetCurrentContext(g_im);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    io.DeltaTime = (std::max)(0.001f, (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart); g_last = now;
    io.DisplaySize = ImVec2((float)g_s.texW, (float)g_s.texH);
    ImGui::NewFrame();
    const float W = (float)g_s.texW, H = (float)g_s.texH, pad = g_s.fontPx * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##vrkit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    // theme: near-black panel, crimson accents
    const ImU32 cText = IM_COL32(238, 234, 232, 255), cDim = IM_COL32(150, 140, 140, 255), cAcc = IM_COL32(255, 92, 82, 255),
                cSel = IM_COL32(140, 18, 22, 210), cWarn = IM_COL32(255, 190, 90, 255), cRed = IM_COL32(200, 28, 34, 255),
                cRedDark = IM_COL32(70, 10, 14, 235), cLine = IM_COL32(150, 24, 30, 200);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // title band
    float bandH = g_s.fontPx * 1.9f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, bandH), IM_COL32(120, 12, 18, 245), IM_COL32(30, 4, 6, 235), IM_COL32(30, 4, 6, 235), IM_COL32(120, 12, 18, 245));
    dl->AddRectFilled(ImVec2(0, bandH - 4), ImVec2(W, bandH), cRed);
    float y = (bandH - g_s.fontPx) * 0.5f - 2;
    dl->AddText(ImVec2(pad, y), IM_COL32(255, 255, 255, 255), g_s.title.c_str());
    if (g_s.statusLine) { std::string st = g_s.statusLine(); ImVec2 ts = ImGui::CalcTextSize(st.c_str()); dl->AddText(ImVec2(W - pad - ts.x, y), IM_COL32(235, 190, 190, 255), st.c_str()); }
    y = bandH + g_s.fontPx * 0.45f;
    // page tabs: red pills
    auto pages = Options::Get().Pages();
    float x = pad;
    for (int p = 0; p < (int)pages.size(); ++p) {
        ImVec2 ts = ImGui::CalcTextSize(pages[p].c_str());
        ImVec2 a(x - 14, y - 6), b(x + ts.x + 14, y + ts.y + 6);
        if (p == g_page) { dl->AddRectFilled(a, b, cRed, 8.f); dl->AddText(ImVec2(x, y), IM_COL32(255, 255, 255, 255), pages[p].c_str()); }
        else { dl->AddRectFilled(a, b, cRedDark, 8.f); dl->AddRect(a, b, cLine, 8.f, 0, 2.f); dl->AddText(ImVec2(x, y), cDim, pages[p].c_str()); }
        x += ts.x + 40;
    }
    y += g_s.fontPx * 1.6f;
    dl->AddLine(ImVec2(pad, y - 8), ImVec2(W - pad, y - 8), cLine, 2.f);
    // rows (scrolled so the selection stays visible)
    auto items = PageItems(g_page);
    float rowH = g_s.fontPx * 1.35f, listTop = y, listBottom = H - g_s.fontPx * 5.2f;
    int visibleRows = (std::max)(1, (int)((listBottom - listTop) / rowH));
    int first = (std::max)(0, (std::min)(g_sel - visibleRows / 2, (int)items.size() - visibleRows));
    auto& opts = Options::Get().All();
    for (int r = first; r < (int)items.size() && r < first + visibleRows; ++r) {
        Option& o = opts[items[r]];
        float ry = listTop + (r - first) * rowH;
        if (r == g_sel) {
            dl->AddRectFilled(ImVec2(pad * 0.5f, ry - 4), ImVec2(W - pad * 0.5f, ry + rowH - 6), cSel, 6.f);
            dl->AddRectFilled(ImVec2(pad * 0.5f, ry - 4), ImVec2(pad * 0.5f + 7, ry + rowH - 6), cAcc, 3.f);
        }
        dl->AddText(ImVec2(pad, ry), cText, o.label.c_str());
        std::string v = Options::Get().DisplayText(o);
        bool adjustable = o.type != OptType::Action;
        std::string shown = o.type == OptType::Action ? "[ run ]" : (r == g_sel && adjustable ? "<  " + v + "  >" : v);
        ImVec2 vs = ImGui::CalcTextSize(shown.c_str());
        float vx = W - pad - vs.x;
        if (o.needsRestart) {
            const char* rs = "restart";
            ImVec2 rsz = ImGui::CalcTextSize(rs);
            dl->AddText(ImVec2(vx - rsz.x - 24, ry), cWarn, rs);
        }
        dl->AddText(ImVec2(vx, ry), r == g_sel ? cAcc : cText, shown.c_str());
    }
    if (items.empty()) dl->AddText(ImVec2(pad, listTop), cDim, "(nothing on this page)");
    // help for the selected option + controls
    float hy = H - g_s.fontPx * 4.6f;
    dl->AddLine(ImVec2(pad, hy - 10), ImVec2(W - pad, hy - 10), cLine, 2.f);
    if (!items.empty()) {
        const Option& o = opts[items[(std::min)(g_sel, (int)items.size() - 1)]];
        std::string h = o.help;
        if (!o.key.empty()) h += (h.empty() ? "" : "   ") + std::string("[") + o.key + "]";
        ImGui::SetCursorScreenPos(ImVec2(pad, hy));
        ImGui::PushTextWrapPos(W - pad);
        ImGui::PushStyleColor(ImGuiCol_Text, cDim);
        ImGui::TextWrapped("%s", h.c_str());
        ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    }
    const char* keys = "A: change   Y: undo   LB/RB: page   B: close";
    dl->AddText(ImVec2(pad, H - g_s.fontPx * 1.4f), cDim, keys);
    ImGui::End();
    ImGui::Render();
}


// ---------------------------------------------------------------- "witcher" theme
// Dark leather-black panel, a double frame (dark bronze outside, gold hairline inside) with diamond corner studs and
// small ornaments in the middle of each edge, the title in a serif crimson, page names in gold, On/Off as red
// switches. Colours are linear-ish sRGB for the headset quad.
namespace {
inline ImU32 C(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 edge) {
    ImVec2 p[4] = { ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y) };
    dl->AddConvexPolyFilled(p, 4, fill); dl->AddPolyline(p, 4, edge, ImDrawFlags_Closed, 1.5f);
}
// corner piece: an L of two lines with a stud at the corner and a small tick
void Corner(ImDrawList* dl, ImVec2 c, float sx, float sy, float len, ImU32 gold, ImU32 bronze) {
    // stud on the frame corner, an inner gold bracket with a bronze echo, and a small diamond inside the bracket
    Diamond(dl, c, 11.f, C(150, 26, 24), gold);
    ImVec2 i(c.x + sx * 24, c.y + sy * 24);
    dl->AddLine(i, ImVec2(i.x + sx * len, i.y), gold, 2.5f);
    dl->AddLine(i, ImVec2(i.x, i.y + sy * len), gold, 2.5f);
    dl->AddLine(ImVec2(i.x + sx * 8, i.y + sy * 8), ImVec2(i.x + sx * len * 0.55f, i.y + sy * 8), bronze, 2.f);
    dl->AddLine(ImVec2(i.x + sx * 8, i.y + sy * 8), ImVec2(i.x + sx * 8, i.y + sy * len * 0.55f), bronze, 2.f);
    Diamond(dl, ImVec2(i.x + sx * len, i.y), 4.5f, gold, gold);
    Diamond(dl, ImVec2(i.x, i.y + sy * len), 4.5f, gold, gold);
    Diamond(dl, ImVec2(i.x + sx * 20, i.y + sy * 20), 5.5f, C(150, 26, 24), gold);
}
// a horizontal rule that thins out towards the ends, with a diamond in the middle
void Rule(ImDrawList* dl, float x0, float x1, float y, ImU32 gold, ImU32 bronze) {
    float m = (x0 + x1) * 0.5f;
    dl->AddRectFilledMultiColor(ImVec2(x0, y - 1), ImVec2(m, y + 1), C(0, 0, 0, 0), gold, gold, C(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(m, y - 1), ImVec2(x1, y + 1), gold, C(0, 0, 0, 0), C(0, 0, 0, 0), gold);
    Diamond(dl, ImVec2(m, y), 6.f, bronze, gold);
}
} // namespace

static void BuildWitcher() {
    ImGui::SetCurrentContext(g_im);
    ImGuiIO& io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    io.DeltaTime = (std::max)(0.001f, (float)(now.QuadPart - g_last.QuadPart) / (float)g_qpf.QuadPart); g_last = now;
    io.DisplaySize = ImVec2((float)g_s.texW, (float)g_s.texH);
    ImGui::NewFrame();
    const float W = (float)g_s.texW, H = (float)g_s.texH, fp = g_s.fontPx;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##vrkitw", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    const int A = (int)(g_s.backgroundAlpha * 255.f + 0.5f);
    const ImU32 gold = C(212, 175, 55), goldDim = C(150, 122, 52), bronze = C(110, 82, 38), parch = C(226, 214, 190), parchDim = C(150, 138, 118),
                red = C(196, 30, 36), redHi = C(232, 58, 52), redDeep = C(92, 12, 16), ink = C(14, 11, 10, A), ink2 = C(30, 22, 18, A);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // panel: dark leather gradient, vignette at the edges
    const float m = 10.f;   // outer margin
    dl->AddRectFilledMultiColor(ImVec2(m, m), ImVec2(W - m, H - m), ink2, ink2, ink, ink);
    dl->AddRectFilledMultiColor(ImVec2(m, m), ImVec2(W - m, m + 90), C(0, 0, 0, A / 3), C(0, 0, 0, A / 3), C(0, 0, 0, 0), C(0, 0, 0, 0));
    // frame: bronze band, gold hairline inside it, corner pieces and edge studs
    dl->AddRect(ImVec2(m, m), ImVec2(W - m, H - m), bronze, 6.f, 0, 6.f);
    dl->AddRect(ImVec2(m + 9, m + 9), ImVec2(W - m - 9, H - m - 9), gold, 2.f, 0, 1.5f);
    const float cl = 64.f, ci = m + 4;
    Corner(dl, ImVec2(ci, ci), 1, 1, cl, gold, bronze);           Corner(dl, ImVec2(W - ci, ci), -1, 1, cl, gold, bronze);
    Corner(dl, ImVec2(ci, H - ci), 1, -1, cl, gold, bronze);      Corner(dl, ImVec2(W - ci, H - ci), -1, -1, cl, gold, bronze);
    Diamond(dl, ImVec2(W * 0.5f, m + 2), 9.f, C(150, 26, 24), gold); Diamond(dl, ImVec2(W * 0.5f, H - m - 2), 9.f, C(150, 26, 24), gold);
    Diamond(dl, ImVec2(m + 2, H * 0.5f), 7.f, bronze, gold);        Diamond(dl, ImVec2(W - m - 2, H * 0.5f), 7.f, bronze, gold);

    const float pad = fp * 1.4f;
    // title: serif crimson with a dark shadow, centred; subtitle / status in parchment below
    float y = m + fp * 0.55f;
    {
        ImGui::PushFont(g_fTitle);
        const char* t = g_s.title.c_str();
        ImVec2 ts = ImGui::CalcTextSize(t);
        float tx = (W - ts.x) * 0.5f;
        dl->AddText(ImVec2(tx + 3, y + 3), C(0, 0, 0, 200), t);
        dl->AddText(ImVec2(tx, y), red, t);
        dl->AddText(ImVec2(tx, y - 1), C(255, 255, 255, 18), t);   // faint highlight
        y += ts.y;
        ImGui::PopFont();
    }
    {
        std::string sub = g_s.subtitle;
        if (g_s.statusLine) { std::string st = g_s.statusLine(); sub = sub.empty() ? st : sub + "   \xC2\xB7   " + st; }
        if (!sub.empty()) { ImVec2 ss = ImGui::CalcTextSize(sub.c_str()); dl->AddText(ImVec2((W - ss.x) * 0.5f, y), parchDim, sub.c_str()); y += ss.y; }
    }
    y += fp * 0.35f;
    Rule(dl, pad, W - pad, y, gold, bronze);
    y += fp * 0.55f;
    // page names in gold (serif); the current one bright with a red diamond and an underline
    auto pages = Options::Get().Pages();
    ImGui::PushFont(g_fTab);
    float total = 0; std::vector<float> wv;
    for (auto& p : pages) { float w = ImGui::CalcTextSize(p.c_str()).x; wv.push_back(w); total += w; }
    float gap = pages.size() > 1 ? (std::min)(fp * 2.2f, (W - 2 * pad - total) / (pages.size() - 1)) : 0;
    float x = (W - (total + gap * (pages.size() ? pages.size() - 1 : 0))) * 0.5f;
    float tabH = ImGui::CalcTextSize("Ag").y;
    for (int p = 0; p < (int)pages.size(); ++p) {
        bool cur = p == g_page;
        if (cur) {
            dl->AddRectFilledMultiColor(ImVec2(x - 16, y - 4), ImVec2(x + wv[p] + 16, y + tabH + 6), C(120, 14, 18, 120), C(120, 14, 18, 120), C(40, 6, 8, 0), C(40, 6, 8, 0));
            dl->AddLine(ImVec2(x - 6, y + tabH + 6), ImVec2(x + wv[p] + 6, y + tabH + 6), gold, 2.5f);
            Diamond(dl, ImVec2(x + wv[p] * 0.5f, y + tabH + 6), 5.f, red, gold);
        }
        dl->AddText(ImVec2(x + 2, y + 2), C(0, 0, 0, 180), pages[p].c_str());
        dl->AddText(ImVec2(x, y), cur ? gold : goldDim, pages[p].c_str());
        if (p + 1 < (int)pages.size()) Diamond(dl, ImVec2(x + wv[p] + gap * 0.5f, y + tabH * 0.55f), 3.5f, bronze, bronze);
        x += wv[p] + gap;
    }
    ImGui::PopFont();
    y += tabH + fp * 0.8f;
    // rows
    auto items = PageItems(g_page);
    auto& opts = Options::Get().All();
    float rowH = fp * 1.38f, listTop = y, listBottom = H - m - fp * 4.9f;
    int visibleRows = (std::max)(1, (int)((listBottom - listTop) / rowH));
    int first = (std::max)(0, (std::min)(g_sel - visibleRows / 2, (int)items.size() - visibleRows));
    for (int r = first; r < (int)items.size() && r < first + visibleRows; ++r) {
        Option& o = opts[items[r]];
        float ry = listTop + (r - first) * rowH;
        bool sel = r == g_sel;
        if (sel) {
            dl->AddRectFilledMultiColor(ImVec2(pad * 0.6f, ry - 5), ImVec2(W - pad * 0.6f, ry + rowH - 7), C(120, 16, 20, 170), C(60, 8, 10, 60), C(60, 8, 10, 60), C(120, 16, 20, 170));
            dl->AddLine(ImVec2(pad * 0.6f, ry - 5), ImVec2(W - pad * 0.6f, ry - 5), C(212, 175, 55, 90), 1.f);
            dl->AddLine(ImVec2(pad * 0.6f, ry + rowH - 7), ImVec2(W - pad * 0.6f, ry + rowH - 7), C(212, 175, 55, 90), 1.f);
            Diamond(dl, ImVec2(pad * 0.6f, ry + rowH * 0.5f - 6), 7.f, red, gold);
        } else if ((r & 1) == 0) dl->AddRectFilled(ImVec2(pad * 0.6f, ry - 5), ImVec2(W - pad * 0.6f, ry + rowH - 7), C(255, 240, 220, 7));
        dl->AddText(ImVec2(pad, ry), sel ? C(250, 242, 228) : parch, o.label.c_str());
        float right = W - pad;
        if (o.type == OptType::Bool) {
            // switch: red when on (knob right), dark when off
            float sh = fp * 0.78f, sw = sh * 2.1f, sy = ry + (fp - sh) * 0.5f + 2, sx = right - sw;
            bool on = o.b && *o.b;
            dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh), on ? red : C(44, 36, 32), sh * 0.5f);
            dl->AddRect(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh), on ? redHi : bronze, sh * 0.5f, 0, 1.5f);
            float kx = on ? sx + sw - sh * 0.5f : sx + sh * 0.5f;
            dl->AddCircleFilled(ImVec2(kx, sy + sh * 0.5f), sh * 0.38f, on ? C(250, 236, 220) : C(120, 108, 96), 20);
            const char* lab = on ? "ON" : "OFF";
            ImVec2 ls = ImGui::CalcTextSize(lab);
            dl->AddText(ImVec2(sx - ls.x - 14, ry), on ? redHi : parchDim, lab);
            right = sx - ls.x - 14;
        } else {
            std::string v = Options::Get().DisplayText(o);
            std::string shown = o.type == OptType::Action ? "run" : v;
            ImVec2 vs = ImGui::CalcTextSize(shown.c_str());
            float vx = right - vs.x - (sel && o.type != OptType::Action ? fp * 0.9f : 0);
            if (o.type == OptType::Action) {   // a small red-bordered button
                dl->AddRectFilled(ImVec2(vx - 14, ry - 2), ImVec2(right + 2, ry + vs.y + 2), sel ? redDeep : C(40, 10, 12), 4.f);
                dl->AddRect(ImVec2(vx - 14, ry - 2), ImVec2(right + 2, ry + vs.y + 2), sel ? redHi : C(120, 30, 30), 4.f, 0, 1.5f);
                dl->AddText(ImVec2(vx - 6, ry), sel ? C(255, 235, 225) : parch, shown.c_str());
            } else {
                if (sel) {   // gold arrows around the value
                    dl->AddTriangleFilled(ImVec2(vx - fp * 0.9f, ry + fp * 0.5f), ImVec2(vx - fp * 0.45f, ry + fp * 0.22f), ImVec2(vx - fp * 0.45f, ry + fp * 0.78f), gold);
                    dl->AddTriangleFilled(ImVec2(right, ry + fp * 0.5f), ImVec2(right - fp * 0.45f, ry + fp * 0.22f), ImVec2(right - fp * 0.45f, ry + fp * 0.78f), gold);
                }
                dl->AddText(ImVec2(vx, ry), sel ? C(250, 236, 200) : parch, shown.c_str());
            }
            right = vx - (sel ? fp * 1.1f : 0);
        }
        if (o.needsRestart) {
            const char* rs = "restart";
            ImVec2 rsz = ImGui::CalcTextSize(rs);
            dl->AddText(ImVec2(right - rsz.x - 22, ry), C(200, 150, 70), rs);
        }
    }
    if ((int)items.size() > visibleRows) {   // scroll marks
        if (first > 0) Diamond(dl, ImVec2(W * 0.5f, listTop - 10), 5.f, gold, gold);
        if (first + visibleRows < (int)items.size()) Diamond(dl, ImVec2(W * 0.5f, listBottom - 2), 5.f, gold, gold);
    }
    if (items.empty()) dl->AddText(ImVec2(pad, listTop), parchDim, "(nothing on this page)");
    // help and controls
    float hy = H - m - fp * 4.6f;
    Rule(dl, pad, W - pad, hy - 12, gold, bronze);
    if (!items.empty()) {
        const Option& o = opts[items[(std::min)(g_sel, (int)items.size() - 1)]];
        std::string h = o.help;
        if (!o.key.empty()) h += (h.empty() ? "" : "   ") + std::string("[") + o.key + "]";
        ImGui::SetCursorScreenPos(ImVec2(pad + fp * 1.2f, hy));   // clear of the corner brackets
        ImGui::PushTextWrapPos(W - pad - fp * 1.2f);
        ImGui::PushStyleColor(ImGuiCol_Text, parchDim);
        ImGui::TextWrapped("%s", h.c_str());
        ImGui::PopStyleColor(); ImGui::PopTextWrapPos();
    }
    const char* keys = "A  change     Y  undo     LB / RB  page     B  close";
    ImVec2 ks = ImGui::CalcTextSize(keys);
    dl->AddText(ImVec2((W - ks.x) * 0.5f, H - m - fp * 1.55f), goldDim, keys);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::Render();
}

bool IsOpen() { return g_open; }
void SetOpen(bool open) { if (open == g_open) return; if (open) { g_open = true; input::SetBlocking(true); } else Close(); }

} } } // namespace vrkit::menu::core
