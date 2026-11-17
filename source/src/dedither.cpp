// De-dither: an edge-preserving 3 x 3 filter on each eye's picture, right after it is copied out of the back buffer.
//
// The Witcher 2 renders several things with screen-door (ordered / noise) dithering instead of blending: soft and
// "transparent" shadows (shadowPCF with interleaved samples), decal and terrain layer blends, hair, LOD cross-fades. On a
// monitor the game hides it with Ubersampling (the frame drawn 4 times with sub-pixel offsets and averaged - PCUber.ini
// has UberSampling=2); without it players have reported "grid shadows" and a "mesh-type pattern over textures" since
// 2011. In a headset, at 3420 x 2668 per eye magnified by the lenses, that pattern is the grain on lit surfaces.
// Ubersampling costs 4x per eye, which VR cannot afford.
//
// The filter averages each pixel with its 8 neighbours, weighted by distance and by how close their brightness is
// RELATIVE to the pixel's own (dither steps are proportional to brightness): dither steps (10 - 40 %) are smoothed,
// real edges (silhouettes, texture contours: several times that) are kept. Measured on a W2VR screenshot: the
// 1-pixel checker energy halves, the sharpest edge keeps 88 % of its contrast.
//
// The shaders are precompiled (vs_3_0 / ps_3_0 bytecode below, HLSL source kept next to it). 0.4.8 compiled them at
// run time with the game's d3dx9_39.dll: the D3DX compiler is not thread-safe and the game compiles its own shaders
// on its loader thread while a save loads - loading a save crashed.
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <cmath>
#include "log.h"
#include <d3dcommon.h>

extern volatile LONG g_inOwn;
HRESULT CallCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                          IDirect3DTexture9** tex, HANDLE* shared);

namespace {
const char kVs[] =
    "void main(float4 p : POSITION, float2 t : TEXCOORD0, out float4 op : POSITION, out float2 ot : TEXCOORD0) { op = p; ot = t; }\n";
// c0 = (1/w, 1/h, 1/range, strength)
const char kPs[] =
    "sampler2D s : register(s0);\n"
    "float4 k : register(c0);\n"
    "float L(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)) + 0.05; }\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 c = tex2D(s, uv).rgb; float l = L(c); float il = k.z / l;\n"
    "  float3 acc = c; float ws = 1;\n"
    "  #define TAP(X, Y, SW) { float3 n = tex2D(s, uv + float2(X, Y) * k.xy).rgb; float r = (L(n) - l) * il; float w = SW * exp(-r * r); acc += n * w; ws += w; }\n"
    "  TAP(-1, 0, 0.6065) TAP(1, 0, 0.6065) TAP(0, -1, 0.6065) TAP(0, 1, 0.6065)\n"
    "  TAP(-1, -1, 0.3679) TAP(1, -1, 0.3679) TAP(-1, 1, 0.3679) TAP(1, 1, 0.3679)\n"
    "  return float4(lerp(c, acc / ws, k.w), 1);\n"
    "}\n";

// vkd3d-compiler -x hlsl -p vs_3_0 / ps_3_0 -b d3dbc (the strings above)
const DWORD kVsCode[] = {
    0xfffe0300, 0x0010fffe, 0x42415443, 0x0000001c, 0x0000001c, 0xfffe0300, 0x00000000, 0x0000001c,
    0x00000000, 0x00000000, 0x33646b76, 0x68732d64, 0x72656461, 0x312e3220, 0x69672820, 0x38642074,
    0x32393138, 0xab002932, 0x0200001f, 0x80000000, 0x900f0000, 0x0200001f, 0x80000005, 0x900f0001,
    0x0200001f, 0x80000000, 0xe00f0000, 0x0200001f, 0x80000005, 0xe0030001, 0x02000001, 0xe00f0000,
    0x90e40000, 0x02000001, 0xe0030001, 0x90040001, 0x0000ffff };

const DWORD kPsCode[] = {
    0xffff0300, 0x0024fffe, 0x42415443, 0x0000001c, 0x0000006c, 0xffff0300, 0x00000002, 0x0000001c,
    0x00000000, 0x00000000, 0x00000044, 0x00000002, 0x00000001, 0x00000048, 0x00000000, 0x00000058,
    0x00000003, 0x00000001, 0x0000005c, 0x00000000, 0xabab006b, 0x00030001, 0x00040001, 0x00000001,
    0x00000001, 0xabab0073, 0x000c0004, 0x00010001, 0x00000001, 0x00000001, 0x33646b76, 0x68732d64,
    0x72656461, 0x312e3220, 0x69672820, 0x38642074, 0x32393138, 0xab002932, 0x05000051, 0xa00f0001,
    0x3e991687, 0x3f1645a2, 0x3de978d5, 0x00000000, 0x05000051, 0xa00f0002, 0x3d4ccccd, 0x00000000,
    0x00000000, 0x00000000, 0x05000051, 0xa00f0003, 0xbf800000, 0x00000000, 0x00000000, 0x00000000,
    0x05000051, 0xa00f0004, 0x3fb8aa3b, 0x00000000, 0x00000000, 0x00000000, 0x05000051, 0xa00f0005,
    0x3f1b4396, 0x00000000, 0x00000000, 0x00000000, 0x05000051, 0xa00f0006, 0x3f800000, 0x00000000,
    0x00000000, 0x00000000, 0x05000051, 0xa00f0007, 0x3f800000, 0x00000000, 0x00000000, 0x00000000,
    0x05000051, 0xa00f0008, 0x00000000, 0xbf800000, 0x00000000, 0x00000000, 0x05000051, 0xa00f0009,
    0x00000000, 0x3f800000, 0x00000000, 0x00000000, 0x05000051, 0xa00f000a, 0xbf800000, 0xbf800000,
    0x00000000, 0x00000000, 0x05000051, 0xa00f000b, 0x3ebc5d64, 0x00000000, 0x00000000, 0x00000000,
    0x05000051, 0xa00f000c, 0x3f800000, 0xbf800000, 0x00000000, 0x00000000, 0x05000051, 0xa00f000d,
    0xbf800000, 0x3f800000, 0x00000000, 0x00000000, 0x0200001f, 0x80000005, 0x90030000, 0x0200001f,
    0x90000000, 0xa00f0800, 0x03000042, 0x800f0000, 0x90040000, 0xa0e40800, 0x03000008, 0x80010001,
    0x80240000, 0xa0240001, 0x03000002, 0x80010001, 0x80000001, 0xa0000002, 0x02000006, 0x80020001,
    0x80000001, 0x03000005, 0x80020001, 0xa0aa0000, 0x80550001, 0x03000005, 0x800c0001, 0xa0400000,
    0xa0400003, 0x03000002, 0x800c0001, 0x90400000, 0x80e00001, 0x03000042, 0x800f0002, 0x80ae0001,
    0xa0e40800, 0x03000008, 0x80040001, 0x80240002, 0xa0240001, 0x03000002, 0x80040001, 0x80aa0001,
    0xa0000002, 0x03000002, 0x80040001, 0x80aa0001, 0x81000001, 0x03000005, 0x80040001, 0x80aa0001,
    0x80550001, 0x03000005, 0x80040001, 0x81aa0001, 0x80aa0001, 0x03000005, 0x80040001, 0x80aa0001,
    0xa0000004, 0x0200000e, 0x80040001, 0x80aa0001, 0x03000005, 0x80040001, 0x80aa0001, 0xa0000005,
    0x03000005, 0x80070002, 0x80240002, 0x80aa0001, 0x03000002, 0x80070002, 0x80240000, 0x80240002,
    0x03000002, 0x80040001, 0x80aa0001, 0xa0000006, 0x03000005, 0x80030003, 0xa0040000, 0xa0040007,
    0x03000002, 0x80030003, 0x90040000, 0x80040003, 0x03000042, 0x800f0003, 0x80040003, 0xa0e40800,
    0x03000008, 0x80080001, 0x80240003, 0xa0240001, 0x03000002, 0x80080001, 0x80ff0001, 0xa0000002,
    0x03000002, 0x80080001, 0x80ff0001, 0x81000001, 0x03000005, 0x80080001, 0x80ff0001, 0x80550001,
    0x03000005, 0x80080001, 0x81ff0001, 0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000004,
    0x0200000e, 0x80080001, 0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000005, 0x03000005,
    0x80070003, 0x80240003, 0x80ff0001, 0x03000002, 0x80070002, 0x80240002, 0x80240003, 0x03000002,
    0x80040001, 0x80aa0001, 0x80ff0001, 0x03000005, 0x80030003, 0xa0040000, 0xa0040008, 0x03000002,
    0x80030003, 0x90040000, 0x80040003, 0x03000042, 0x800f0003, 0x80040003, 0xa0e40800, 0x03000008,
    0x80080001, 0x80240003, 0xa0240001, 0x03000002, 0x80080001, 0x80ff0001, 0xa0000002, 0x03000002,
    0x80080001, 0x80ff0001, 0x81000001, 0x03000005, 0x80080001, 0x80ff0001, 0x80550001, 0x03000005,
    0x80080001, 0x81ff0001, 0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000004, 0x0200000e,
    0x80080001, 0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000005, 0x03000005, 0x80070003,
    0x80240003, 0x80ff0001, 0x03000002, 0x80070002, 0x80240002, 0x80240003, 0x03000002, 0x80040001,
    0x80aa0001, 0x80ff0001, 0x03000005, 0x80030003, 0xa0040000, 0xa0040009, 0x03000002, 0x80030003,
    0x90040000, 0x80040003, 0x03000042, 0x800f0003, 0x80040003, 0xa0e40800, 0x03000008, 0x80080001,
    0x80240003, 0xa0240001, 0x03000002, 0x80080001, 0x80ff0001, 0xa0000002, 0x03000002, 0x80080001,
    0x80ff0001, 0x81000001, 0x03000005, 0x80080001, 0x80ff0001, 0x80550001, 0x03000005, 0x80080001,
    0x81ff0001, 0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000004, 0x0200000e, 0x80080001,
    0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000005, 0x03000005, 0x80070003, 0x80240003,
    0x80ff0001, 0x03000002, 0x80070002, 0x80240002, 0x80240003, 0x03000002, 0x80040001, 0x80aa0001,
    0x80ff0001, 0x03000005, 0x80030003, 0xa0040000, 0xa004000a, 0x03000002, 0x80030003, 0x90040000,
    0x80040003, 0x03000042, 0x800f0003, 0x80040003, 0xa0e40800, 0x03000008, 0x80080001, 0x80240003,
    0xa0240001, 0x03000002, 0x80080001, 0x80ff0001, 0xa0000002, 0x03000002, 0x80080001, 0x80ff0001,
    0x81000001, 0x03000005, 0x80080001, 0x80ff0001, 0x80550001, 0x03000005, 0x80080001, 0x81ff0001,
    0x80ff0001, 0x03000005, 0x80080001, 0x80ff0001, 0xa0000004, 0x0200000e, 0x80080001, 0x80ff0001,
    0x03000005, 0x80080001, 0x80ff0001, 0xa000000b, 0x03000005, 0x80070003, 0x80240003, 0x80ff0001,
    0x03000002, 0x80070002, 0x80240002, 0x80240003, 0x03000002, 0x80040001, 0x80aa0001, 0x80ff0001,
    0x03000005, 0x80030003, 0xa0040000, 0xa004000c, 0x03000002, 0x80030003, 0x90040000, 0x80040003,
    0x03000042, 0x800f0003, 0x80040003, 0xa0e40800, 0x03000008, 0x80080001, 0x80240003, 0xa0240001,
    0x03000002, 0x80080001, 0x80ff0001, 0xa0000002, 0x03000002, 0x80080001, 0x80ff0001, 0x81000001,
    0x03000005, 0x80080001, 0x80ff0001, 0x80550001, 0x03000005, 0x80080001, 0x81ff0001, 0x80ff0001,
    0x03000005, 0x80080001, 0x80ff0001, 0xa0000004, 0x0200000e, 0x80080001, 0x80ff0001, 0x03000005,
    0x80080001, 0x80ff0001, 0xa000000b, 0x03000005, 0x80070003, 0x80240003, 0x80ff0001, 0x03000002,
    0x80070002, 0x80240002, 0x80240003, 0x03000002, 0x80040001, 0x80aa0001, 0x80ff0001, 0x03000005,
    0x80030003, 0xa0040000, 0xa004000d, 0x03000002, 0x80030003, 0x90040000, 0x80040003, 0x03000042,
    0x800f0003, 0x80040003, 0xa0e40800, 0x03000008, 0x80080001, 0x80240003, 0xa0240001, 0x03000002,
    0x80080001, 0x80ff0001, 0xa0000002, 0x03000002, 0x80080001, 0x80ff0001, 0x81000001, 0x03000005,
    0x80080001, 0x80ff0001, 0x80550001, 0x03000005, 0x80080001, 0x81ff0001, 0x80ff0001, 0x03000005,
    0x80080001, 0x80ff0001, 0xa0000004, 0x0200000e, 0x80080001, 0x80ff0001, 0x03000005, 0x80080001,
    0x80ff0001, 0xa000000b, 0x03000005, 0x80070003, 0x80240003, 0x80ff0001, 0x03000002, 0x80070002,
    0x80240002, 0x80240003, 0x03000002, 0x80040001, 0x80aa0001, 0x80ff0001, 0x03000002, 0x80030003,
    0x90040000, 0xa0040000, 0x03000042, 0x800f0003, 0x80040003, 0xa0e40800, 0x03000008, 0x80080001,
    0x80240003, 0xa0240001, 0x03000002, 0x80080001, 0x80ff0001, 0xa0000002, 0x03000002, 0x80010001,
    0x80ff0001, 0x81000001, 0x03000005, 0x80010001, 0x80000001, 0x80550001, 0x03000005, 0x80010001,
    0x81000001, 0x80000001, 0x03000005, 0x80010001, 0x80000001, 0xa0000004, 0x0200000e, 0x80010001,
    0x80000001, 0x03000005, 0x80010001, 0x80000001, 0xa000000b, 0x03000005, 0x80070003, 0x80240003,
    0x80000001, 0x03000002, 0x80070002, 0x80240002, 0x80240003, 0x03000002, 0x80010001, 0x80aa0001,
    0x80000001, 0x02000006, 0x80020001, 0x80000001, 0x02000006, 0x80040001, 0x80000001, 0x02000006,
    0x80080001, 0x80000001, 0x03000005, 0x80070001, 0x80240002, 0x80790001, 0x03000002, 0x80070001,
    0x80240001, 0x81240000, 0x03000005, 0x80070001, 0xa0ff0000, 0x80240001, 0x03000002, 0x80070000,
    0x80240000, 0x80240001, 0x02000001, 0x80080000, 0xa0000006, 0x02000001, 0x800f0800, 0x80e40000,
    0x0000ffff };

IDirect3DVertexShader9* g_vs = nullptr; IDirect3DPixelShader9* g_ps = nullptr; IDirect3DVertexDeclaration9* g_decl = nullptr;
IDirect3DTexture9* g_tmp = nullptr; UINT g_tw = 0, g_th = 0; D3DFORMAT g_tf = D3DFMT_UNKNOWN;
IDirect3DStateBlock9* g_sb = nullptr;
float g_strength = 1.f, g_range = 0.35f;
bool g_failed = false;
unsigned g_passes = 0;

// The shaders are compiled with the system's d3dcompiler_47.dll (thread-safe, unlike D3DX; present on Windows 8.1+ and
// in most game folders). The precompiled vkd3d bytecode (0.4.8 - 0.5.8) was rejected by the D3D9 runtime, so the
// filter never ran; it stays only as the last fallback.
typedef HRESULT (WINAPI* D3DCompileFn)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);
struct Blob { ID3DBlob* b = nullptr; ~Blob() { if (b) b->Release(); } };
bool CompileBoth(IDirect3DDevice9* d) {
    static const wchar_t* dlls[] = { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" };
    D3DCompileFn fn = nullptr; const wchar_t* used = L"";
    for (auto n : dlls) { HMODULE m = LoadLibraryW(n); if (m && (fn = (D3DCompileFn)GetProcAddress(m, "D3DCompile"))) { used = n; break; } }
    if (!fn) { LOGW("de-dither: no d3dcompiler DLL found"); return false; }
    Blob vb, pb, e1, e2;
    HRESULT h1 = fn(kVs, strlen(kVs), "dedither_vs", nullptr, nullptr, "main", "vs_3_0", 0, 0, &vb.b, &e1.b);
    HRESULT h2 = fn(kPs, strlen(kPs), "dedither_ps", nullptr, nullptr, "main", "ps_3_0", 0, 0, &pb.b, &e2.b);
    if (FAILED(h1) || FAILED(h2)) {
        LOGW("de-dither: %ls could not compile the filter (%08lx / %08lx): %s", used, h1, h2,
             e2.b ? (const char*)e2.b->GetBufferPointer() : e1.b ? (const char*)e1.b->GetBufferPointer() : "");
        return false;
    }
    HRESULT c1 = d->CreateVertexShader((const DWORD*)vb.b->GetBufferPointer(), &g_vs);
    HRESULT c2 = d->CreatePixelShader((const DWORD*)pb.b->GetBufferPointer(), &g_ps);
    if (FAILED(c1) || FAILED(c2)) { LOGW("de-dither: compiled shaders rejected (%08lx / %08lx)", c1, c2); return false; }
    LOGI("de-dither: filter compiled with %ls", used);
    return true;
}
bool EnsureShaders(IDirect3DDevice9* d) {
    if (g_ps && g_vs && g_decl) return true;
    if (g_failed) return false;
    g_failed = true;   // until it works
    bool ok = CompileBoth(d);
    if (!ok) {
        if (g_vs) { g_vs->Release(); g_vs = nullptr; }
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        HRESULT c1 = d->CreateVertexShader(kVsCode, &g_vs), c2 = d->CreatePixelShader(kPsCode, &g_ps);
        ok = SUCCEEDED(c1) && SUCCEEDED(c2);
        if (!ok) LOGW("de-dither: built-in bytecode rejected too (%08lx / %08lx)", c1, c2);
    }
    static const D3DVERTEXELEMENT9 el[] = {
        { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
    ok = ok && SUCCEEDED(d->CreateVertexDeclaration(el, &g_decl));
    if (!ok) {
        LOGW("de-dither: shaders could not be created - filter off");
        if (g_vs) { g_vs->Release(); g_vs = nullptr; }
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        if (g_decl) { g_decl->Release(); g_decl = nullptr; }
        return false;
    }
    g_failed = false;
    LOGI("de-dither: filter ready (strength %.2f, range %.2f)", g_strength, g_range);
    return true;
}
} // namespace

void DeDitherSet(float strength, float range) {
    g_strength = strength < 0.f ? 0.f : strength > 1.f ? 1.f : strength;
    g_range = range < 0.05f ? 0.05f : range > 2.f ? 2.f : range;
}
float DeDitherStrength() { return g_strength; }
unsigned DeDitherPasses() { unsigned n = g_passes; g_passes = 0; return n; }

// device lost / reset: the default-pool temporary and the state block go; shaders and the declaration survive
void DeDitherRelease() {
    if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    g_tw = g_th = 0;
}

// filters the eye texture in place (it is a render-target texture the size of the back buffer)
void DeDither(IDirect3DDevice9* d, IDirect3DTexture9* eye) {
    if (!d || !eye || g_strength <= 0.001f) return;
    struct Own { Own() { InterlockedIncrement(&g_inOwn); } ~Own() { InterlockedDecrement(&g_inOwn); } } own;   // the mod's own calls
    if (!EnsureShaders(d)) return;
    D3DSURFACE_DESC ed; if (FAILED(eye->GetLevelDesc(0, &ed))) return;
    if (!g_tmp || g_tw != ed.Width || g_th != ed.Height || g_tf != ed.Format) {
        if (g_tmp) { g_tmp->Release(); g_tmp = nullptr; }
        if (FAILED(CallCreateTexture(d, ed.Width, ed.Height, 1, D3DUSAGE_RENDERTARGET, ed.Format, D3DPOOL_DEFAULT, &g_tmp, nullptr)) || !g_tmp) {
            static int n = 0; if (n++ < 3) LOGW("de-dither: %ux%u temporary texture failed", ed.Width, ed.Height);
            g_tmp = nullptr; return;
        }
        g_tw = ed.Width; g_th = ed.Height; g_tf = ed.Format;
    }
    if (!g_sb && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sb))) g_sb = nullptr;
    IDirect3DSurface9 *es = nullptr, *ts = nullptr;
    eye->GetSurfaceLevel(0, &es); g_tmp->GetSurfaceLevel(0, &ts);
    if (!es || !ts || FAILED(d->StretchRect(es, nullptr, ts, nullptr, D3DTEXF_NONE))) { if (es) es->Release(); if (ts) ts->Release(); return; }
    if (g_sb) g_sb->Capture();
    IDirect3DSurface9* rt[4] = {}; IDirect3DSurface9* ds = nullptr;
    D3DVIEWPORT9 vpSave = {}; d->GetViewport(&vpSave);   // setting render target 0 resets the viewport: restored last
    for (int i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
    d->GetDepthStencilSurface(&ds);
    d->SetRenderTarget(0, es);
    for (int i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, nullptr);
    d->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, ed.Width, ed.Height, 0.f, 1.f }; d->SetViewport(&vp);
    const DWORD rs[][2] = {
        { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE },
        { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
        { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_FOGENABLE, FALSE } };
    for (auto& r : rs) d->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
    d->SetTexture(0, g_tmp);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    d->SetVertexDeclaration(g_decl); d->SetVertexShader(g_vs); d->SetPixelShader(g_ps);
    const float c0[4] = { 1.f / ed.Width, 1.f / ed.Height, 1.f / g_range, g_strength };
    d->SetPixelShaderConstantF(0, c0, 1);
    // clip space, shifted half a pixel so each pixel samples its own texel (D3D9 pixel centres)
    const float hx = 1.f / ed.Width, hy = 1.f / ed.Height;
    struct V { float x, y, z, w, u, v; } q[4] = {
        { -1.f - hx,  1.f + hy, 0.5f, 1.f, 0.f, 0.f }, { 1.f - hx,  1.f + hy, 0.5f, 1.f, 1.f, 0.f },
        { -1.f - hx, -1.f + hy, 0.5f, 1.f, 0.f, 1.f }, { 1.f - hx, -1.f + hy, 0.5f, 1.f, 1.f, 1.f } };
    // at Present the game's scene is closed (draws outside BeginScene / EndScene fail); between the full-stereo passes
    // it may still be open - then BeginScene fails and the draw goes into the open scene
    const bool began = SUCCEEDED(d->BeginScene());
    HRESULT hr = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    if (began) d->EndScene();
    if (FAILED(hr)) { static int n = 0; if (n++ < 3) LOGW("de-dither: draw failed %08lx", hr); }
    else g_passes++;
    if (g_sb) g_sb->Apply();
    for (int i = 0; i < 4; i++) { if (i == 0 || rt[i]) d->SetRenderTarget(i, rt[i]); if (rt[i]) rt[i]->Release(); }
    d->SetDepthStencilSurface(ds); if (ds) ds->Release();
    d->SetViewport(&vpSave);
    es->Release(); ts->Release();
}


// ---------------------------------------------------------------- 0.9.0: G-buffer normal repair
// Found with the stage dump (0.8.0): the game's normal target (MRT 2 of the G-buffer, A8R8G8B8, view-space normal * 0.5
// + 0.5, alpha = material) holds a ZERO vector in about 60 % of all pixels and shortened vectors in most others; only
// the direction of the non-zero ones is right. The lighting passes do not normalise: a zero normal is unlit - the dark
// specks on every lit surface (they follow the texture detail and crawl with the view). The colour target is clean.
// This pass runs once when the game leaves the G-buffer: normals are renormalised, and a pixel without a usable one
// takes the direction of its neighbours of the same material (up to 4 pixels away).
namespace {
const char kNrPs[] =
    "sampler2D s : register(s0);\n"
    "float4 k : register(c0);\n"
    "float3 tap(float2 uv, float2 o, float a, float w) {\n"
    "  float4 t = tex2Dlod(s, float4(uv + o * k.xy, 0, 0)); float3 v = t.rgb * 2 - 1; float l = length(v);\n"
    "  return (abs(t.a - a) < 0.006 && l > 0.08 && l < 1.15) ? v * (w / l) : float3(0, 0, 0);\n"
    "}\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float4 c = tex2Dlod(s, float4(uv, 0, 0)); float3 n = c.rgb * 2 - 1; float l = length(n);\n"
    "  if (l > 1.15) return c;\n"                                   // not a normal (sky / cleared)
    "  if (l > 0.6) return float4(n / l * 0.5 + 0.5, c.a);\n"       // usable: full length
    "  float3 acc = n;\n"
    "  acc += tap(uv, float2(1, 0), c.a, 1) + tap(uv, float2(-1, 0), c.a, 1) + tap(uv, float2(0, 1), c.a, 1) + tap(uv, float2(0, -1), c.a, 1);\n"
    "  acc += tap(uv, float2(1, 1), c.a, .7) + tap(uv, float2(-1, 1), c.a, .7) + tap(uv, float2(1, -1), c.a, .7) + tap(uv, float2(-1, -1), c.a, .7);\n"
    "  acc += tap(uv, float2(2, 0), c.a, .5) + tap(uv, float2(-2, 0), c.a, .5) + tap(uv, float2(0, 2), c.a, .5) + tap(uv, float2(0, -2), c.a, .5);\n"
    "  acc += tap(uv, float2(2, 2), c.a, .35) + tap(uv, float2(-2, 2), c.a, .35) + tap(uv, float2(2, -2), c.a, .35) + tap(uv, float2(-2, -2), c.a, .35);\n"
    "  acc += tap(uv, float2(4, 0), c.a, .25) + tap(uv, float2(-4, 0), c.a, .25) + tap(uv, float2(0, 4), c.a, .25) + tap(uv, float2(0, -4), c.a, .25);\n"
    "  float la = length(acc);\n"
    "  if (la < 0.05) return c;\n"
    "  return float4(acc / la * 0.5 + 0.5, c.a);\n"
    "}\n";
IDirect3DPixelShader9* g_nrPs = nullptr; IDirect3DTexture9* g_nrTmp = nullptr; UINT g_nrW = 0, g_nrH = 0; bool g_nrFailed = false; unsigned g_nrPasses = 0;
bool EnsureNr(IDirect3DDevice9* d) {
    if (g_nrPs) return true;
    if (g_nrFailed || !EnsureShaders(d)) return false;
    g_nrFailed = true;
    static const wchar_t* dlls[] = { L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll" };
    D3DCompileFn fn = nullptr;
    for (auto n : dlls) { HMODULE m = LoadLibraryW(n); if (m && (fn = (D3DCompileFn)GetProcAddress(m, "D3DCompile"))) break; }
    if (!fn) return false;
    Blob pb, e;
    HRESULT h = fn(kNrPs, strlen(kNrPs), "normal_repair", nullptr, nullptr, "main", "ps_3_0", 0, 0, &pb.b, &e.b);
    if (FAILED(h)) { LOGW("normals: repair shader did not compile (%08lx): %s", h, e.b ? (const char*)e.b->GetBufferPointer() : ""); return false; }
    if (FAILED(d->CreatePixelShader((const DWORD*)pb.b->GetBufferPointer(), &g_nrPs))) { g_nrPs = nullptr; return false; }
    g_nrFailed = false;
    LOGI("normals: G-buffer normal repair ready");
    return true;
}
} // namespace
unsigned NormalRepairPasses() { unsigned n = g_nrPasses; g_nrPasses = 0; return n; }
void NormalRepairRelease() { if (g_nrTmp) { g_nrTmp->Release(); g_nrTmp = nullptr; } }
void NormalRepair(IDirect3DDevice9* d, IDirect3DSurface9* es) {
    if (!d || !es) return;
    struct Own { Own() { InterlockedIncrement(&g_inOwn); } ~Own() { InterlockedDecrement(&g_inOwn); } } own;
    if (!EnsureNr(d)) return;
    D3DSURFACE_DESC ed; if (FAILED(es->GetDesc(&ed))) return;
    if (!g_nrTmp || g_nrW != ed.Width || g_nrH != ed.Height) {
        if (g_nrTmp) { g_nrTmp->Release(); g_nrTmp = nullptr; }
        if (FAILED(CallCreateTexture(d, ed.Width, ed.Height, 1, D3DUSAGE_RENDERTARGET, ed.Format, D3DPOOL_DEFAULT, &g_nrTmp, nullptr)) || !g_nrTmp) { g_nrTmp = nullptr; g_nrFailed = true; return; }
        g_nrW = ed.Width; g_nrH = ed.Height;
    }
    if (!g_sb && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_sb))) g_sb = nullptr;
    IDirect3DSurface9* ts = nullptr; g_nrTmp->GetSurfaceLevel(0, &ts);
    if (!ts) return;
    if (g_sb) g_sb->Capture();
    IDirect3DSurface9* rt[4] = {}; IDirect3DSurface9* ds = nullptr;
    D3DVIEWPORT9 vpSave = {}; d->GetViewport(&vpSave);
    for (int i = 0; i < 4; i++) d->GetRenderTarget(i, &rt[i]);
    d->GetDepthStencilSurface(&ds);
    for (int i = 3; i >= 1; i--) if (rt[i]) d->SetRenderTarget(i, nullptr);
    d->SetRenderTarget(0, es);
    d->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp = { 0, 0, ed.Width, ed.Height, 0.f, 1.f }; d->SetViewport(&vp);
    const DWORD rs[][2] = {
        { D3DRS_ZENABLE, FALSE }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE }, { D3DRS_ALPHABLENDENABLE, FALSE },
        { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_STENCILENABLE, FALSE }, { D3DRS_SCISSORTESTENABLE, FALSE }, { D3DRS_CLIPPLANEENABLE, 0 },
        { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_SRGBWRITEENABLE, FALSE }, { D3DRS_FILLMODE, D3DFILL_SOLID }, { D3DRS_FOGENABLE, FALSE } };
    for (auto& r : rs) d->SetRenderState((D3DRENDERSTATETYPE)r[0], r[1]);
    d->SetTexture(0, g_nrTmp);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    d->SetVertexDeclaration(g_decl); d->SetVertexShader(g_vs); d->SetPixelShader(g_nrPs);
    const float c0[4] = { 1.f / ed.Width, 1.f / ed.Height, 0, 0 };
    d->SetPixelShaderConstantF(0, c0, 1);
    const float hx = 1.f / ed.Width, hy = 1.f / ed.Height;
    struct V { float x, y, z, w, u, v; } q[4] = {
        { -1.f - hx,  1.f + hy, 0.5f, 1.f, 0.f, 0.f }, { 1.f - hx,  1.f + hy, 0.5f, 1.f, 1.f, 0.f },
        { -1.f - hx, -1.f + hy, 0.5f, 1.f, 0.f, 1.f }, { 1.f - hx, -1.f + hy, 0.5f, 1.f, 1.f, 1.f } };
    // three rounds: each fills holes up to 4 pixels deep from their rim (measured on a dumped frame: 61 % of the
    // pixels without a normal before, under 0.5 % after); pixels that already have one leave the shader at once
    const bool began = SUCCEEDED(d->BeginScene());
    HRESULT hr = S_OK;
    for (int round = 0; round < 3 && SUCCEEDED(hr); round++) {
        d->SetTexture(0, nullptr);
        hr = d->StretchRect(es, nullptr, ts, nullptr, D3DTEXF_NONE);
        d->SetTexture(0, g_nrTmp);
        if (SUCCEEDED(hr)) hr = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    }
    if (began) d->EndScene();
    if (SUCCEEDED(hr)) g_nrPasses++; else { static int n = 0; if (n++ < 3) LOGW("normals: repair draw failed %08lx", hr); }
    if (g_sb) g_sb->Apply();
    d->SetRenderTarget(0, rt[0]);
    for (int i = 1; i < 4; i++) if (rt[i]) d->SetRenderTarget(i, rt[i]);
    for (int i = 0; i < 4; i++) if (rt[i]) rt[i]->Release();
    d->SetDepthStencilSurface(ds); if (ds) ds->Release();
    d->SetViewport(&vpSave);
    ts->Release();
}
