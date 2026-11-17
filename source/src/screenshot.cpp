// Screenshot of the VR view (Delete): the eye images exactly as they were sent to the headset, saved as PNG next to the
// game exe (AC2VR_screenshot_<date>_<time>.png). screenshot_mode = eye (left eye) or sbs (left | right side by side).
// The GPU read-back happens on the render thread; PNG encoding runs on a worker thread.
#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <cstdio>
#include "log.h"
#include "w2vr.h"
#include "game.h"
#include <cmath>
#include <cstring>
bool g_screenshotSquare = true;   // screenshot_square
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

namespace {
struct Job { std::wstring path; std::vector<unsigned char> rgb; int w, h; };

DWORD WINAPI Encode(LPVOID p) {
    Job* j = (Job*)p;
    stbi_write_png_compression_level = 5;
    char path[MAX_PATH * 3]; WideCharToMultiByte(CP_UTF8, 0, j->path.c_str(), -1, path, sizeof path, nullptr, nullptr);
    FILE* f = _wfopen(j->path.c_str(), L"wb");
    int ok = 0;
    if (f) {
        ok = stbi_write_png_to_func([](void* ctx, void* data, int size) { fwrite(data, 1, size, (FILE*)ctx); }, f, j->w, j->h, 3, j->rgb.data(), j->w * 3);
        fclose(f);
    }
    if (ok) LOGI("screenshot: saved %s (%dx%d)", path, j->w, j->h);
    else LOGE("screenshot: could not write %s", path);
    delete j;
    return 0;
}

// copy one render-target texture into rgb at column x0 (rgb row width = rowW pixels)
bool ReadBack(IDirect3DDevice9* d, IDirect3DTexture9* t, std::vector<unsigned char>& rgb, int x0, int rowW, int* w, int* h) {
    IDirect3DSurface9 *src = nullptr, *sys = nullptr;
    D3DSURFACE_DESC sd;
    if (!t || FAILED(t->GetSurfaceLevel(0, &src)) || !src) return false;
    src->GetDesc(&sd);
    bool ok = false;
    if (sd.Format == D3DFMT_X8R8G8B8 || sd.Format == D3DFMT_A8R8G8B8) {
        if (SUCCEEDED(d->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(d->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                if (rgb.empty()) rgb.resize((size_t)rowW * sd.Height * 3);
                for (UINT y = 0; y < sd.Height; y++) {
                    const unsigned char* s = (const unsigned char*)lr.pBits + y * lr.Pitch;
                    unsigned char* o = &rgb[((size_t)y * rowW + x0) * 3];
                    for (UINT x = 0; x < sd.Width; x++) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o += 3; s += 4; }
                }
                sys->UnlockRect();
                ok = true;
            }
        }
    } else LOGW("screenshot: unsupported back buffer format %d", (int)sd.Format);
    *w = sd.Width; *h = sd.Height;
    if (sys) sys->Release();
    src->Release();
    return ok;
}
} // namespace

// left / right: the eye textures (right may be null: one image); sbs: side by side
void ScreenshotSave(IDirect3DDevice9* d, IDirect3DTexture9* left, IDirect3DTexture9* right, bool sbs) {
    if (!left) { LOGW("screenshot: no image yet"); return; }
    D3DSURFACE_DESC ld; left->GetLevelDesc(0, &ld);
    bool two = sbs && right;
    int rowW = two ? (int)ld.Width * 2 : (int)ld.Width;
    Job* j = new Job();
    int w = 0, h = 0;
    bool ok = ReadBack(d, left, j->rgb, 0, rowW, &w, &h);
    if (ok && two) ok = ReadBack(d, right, j->rgb, w, rowW, &w, &h);
    if (!ok) { delete j; LOGE("screenshot: read-back failed"); return; }
    j->w = rowW; j->h = h;
    // the VR view at 1:1: the square in the middle of the 3D picture (letterbox bars, if any, cut off), per eye
    if (g_screenshotSquare) {
        const float* c = g_vr.content;
        float cx0 = c[0] * w, cx1 = c[2] * w, cy0 = c[1] * h, cy1 = c[3] * h;
        if (cx1 - cx0 < 16 || cy1 - cy0 < 16) { cx0 = 0; cx1 = (float)w; cy0 = 0; cy1 = (float)h; }
        int side = (int)fminf(cx1 - cx0, cy1 - cy0);
        int sx = (int)((cx0 + cx1 - side) * 0.5f), sy = (int)((cy0 + cy1 - side) * 0.5f);
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx + side > w) side = w - sx;
        if (sy + side > h) side = h - sy;
        int n = two ? 2 : 1;
        std::vector<unsigned char> out((size_t)side * n * side * 3);
        for (int e = 0; e < n; e++)
            for (int y = 0; y < side; y++)
                memcpy(&out[((size_t)y * side * n + (size_t)e * side) * 3], &j->rgb[((size_t)(sy + y) * rowW + e * w + sx) * 3], (size_t)side * 3);
        j->rgb.swap(out); j->w = side * n; j->h = side;
    }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[96];
    swprintf(name, 96, L"\\" GAME_WSHOT L"_screenshot_%04u-%02u-%02u_%02u-%02u-%02u%ls.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, two ? L"_sbs" : L"");
    j->path = g_gameDir + name;
    HANDLE t = CreateThread(nullptr, 0, Encode, j, 0, nullptr);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    else Encode(j);
}

// the HUD panel's picture (premultiplied, shown over black), full frame: W2VR_screenshot_<time>_hud.png
void ScreenshotSaveHud(IDirect3DDevice9* d, IDirect3DTexture9* hud) {
    if (!hud) return;
    D3DSURFACE_DESC hd; hud->GetLevelDesc(0, &hd);
    Job* j = new Job();
    int w = 0, h = 0;
    if (!ReadBack(d, hud, j->rgb, 0, (int)hd.Width, &w, &h)) { delete j; LOGE("screenshot: HUD read-back failed"); return; }
    j->w = w; j->h = h;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t name[96];
    swprintf(name, 96, L"\\" GAME_WSHOT L"_screenshot_%04u-%02u-%02u_%02u-%02u-%02u_hud.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    j->path = g_gameDir + name;
    HANDLE t = CreateThread(nullptr, 0, Encode, j, 0, nullptr);
    if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    else Encode(j);
}

// ---------------------------------------------------------------- stage dump (diagnostics)
// A crop of any render target as PNG: colour on the left half, the alpha channel (grey) on the right half.
namespace {
float HalfToFloat(unsigned short h) {
    int e = (h >> 10) & 31, m = h & 1023; float v;
    if (e == 0) v = ldexpf((float)m, -24); else if (e == 31) v = m ? 0.f : 65504.f; else v = ldexpf((float)(m + 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}
unsigned char Tone(float v) { if (!(v > 0.f)) return 0; v = v / (1.f + v); v = sqrtf(v) * 1.4142f; return (unsigned char)(v >= 1.f ? 255 : v * 255.f); }
unsigned char Unit(float v) { return (unsigned char)(v <= 0.f ? 0 : v >= 1.f ? 255 : v * 255.f + 0.5f); }
}
bool StageDumpSurface(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& rc, const std::wstring& path) {
    D3DSURFACE_DESC sd; if (!src || FAILED(src->GetDesc(&sd))) return false;
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    D3DFORMAT f = sd.Format;
    bool known = f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8 || f == D3DFMT_A16B16G16R16F || f == D3DFMT_G16R16F || f == D3DFMT_R32F ||
                 f == D3DFMT_A2R10G10B10 || f == D3DFMT_A2B10G10R10 || f == D3DFMT_A16B16G16R16;
    if (!known || cw < 8 || ch < 8) return false;
    IDirect3DSurface9 *small = nullptr, *sys = nullptr; bool ok = false;
    if (SUCCEEDED(d->CreateRenderTarget(cw, ch, f, D3DMULTISAMPLE_NONE, 0, FALSE, &small, nullptr)) &&
        SUCCEEDED(d->CreateOffscreenPlainSurface(cw, ch, f, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(d->StretchRect(src, &rc, small, nullptr, D3DTEXF_NONE)) && SUCCEEDED(d->GetRenderTargetData(small, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            Job* j = new Job(); j->w = cw * 2; j->h = ch; j->rgb.resize((size_t)cw * 2 * ch * 3); j->path = path;
            for (int y = 0; y < ch; y++) {
                const unsigned char* s = (const unsigned char*)lr.pBits + (size_t)y * lr.Pitch;
                unsigned char* o = &j->rgb[(size_t)y * cw * 2 * 3]; unsigned char* oa = o + (size_t)cw * 3;
                for (int x = 0; x < cw; x++, o += 3, oa += 3) {
                    unsigned char r = 0, g = 0, b = 0, a = 0;
                    if (f == D3DFMT_A8R8G8B8 || f == D3DFMT_X8R8G8B8) { b = s[0]; g = s[1]; r = s[2]; a = s[3]; s += 4; }
                    else if (f == D3DFMT_A16B16G16R16F) { const unsigned short* h = (const unsigned short*)s; r = Tone(HalfToFloat(h[0])); g = Tone(HalfToFloat(h[1])); b = Tone(HalfToFloat(h[2])); a = Unit(HalfToFloat(h[3])); s += 8; }
                    else if (f == D3DFMT_A16B16G16R16) { const unsigned short* h = (const unsigned short*)s; r = h[0] >> 8; g = h[1] >> 8; b = h[2] >> 8; a = h[3] >> 8; s += 8; }
                    else if (f == D3DFMT_G16R16F) { const unsigned short* h = (const unsigned short*)s; r = Unit(HalfToFloat(h[0]) * 0.5f + 0.5f); g = Unit(HalfToFloat(h[1]) * 0.5f + 0.5f); b = 128; a = 255; s += 4; }
                    else if (f == D3DFMT_R32F) { float v; memcpy(&v, s, 4); r = g = b = Unit(log2f(1.f + fabsf(v)) / 12.f); a = Unit(v - floorf(v)); s += 4; }
                    else { unsigned v; memcpy(&v, s, 4); unsigned c0 = v & 1023, c1 = (v >> 10) & 1023, c2 = (v >> 20) & 1023; a = (unsigned char)((v >> 30) * 85);
                           if (f == D3DFMT_A2R10G10B10) { b = c0 >> 2; g = c1 >> 2; r = c2 >> 2; } else { r = c0 >> 2; g = c1 >> 2; b = c2 >> 2; } s += 4; }
                    o[0] = r; o[1] = g; o[2] = b; oa[0] = oa[1] = oa[2] = a;
                }
            }
            sys->UnlockRect();
            HANDLE t = CreateThread(nullptr, 0, Encode, j, 0, nullptr);
            if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); } else Encode(j);
            ok = true;
        }
    }
    if (small) small->Release();
    if (sys) sys->Release();
    return ok;
}
