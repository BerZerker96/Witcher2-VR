#pragma once
// D3D9 vertex shader bytecode patcher (SM 2.0 / 2.x / 3.0).
//
// Every write to the position output (oPos in vs_2_x, the dcl_position o# register in vs_3_0) is redirected into
// a new temp register rP (the tail's scratch registers reuse the shader's own temps, which are dead by then). Before the end of the main program a short tail is appended that applies the per-eye
// clip-space transform the mod uploads to c[base .. base+3]:
//
//     q        = (rP.x, rP.y, 1, rP.w)
//     t.x/y/w  = dot(q, c[base+0/1/2])          (view-space head/eye transform folded into the game projection)
//     t.z      = rP.z * t.w / rP.w               (keep the game's NDC depth: no new clipping, same depth order)
//     m        = c[base+3].z * ((rP.w - c[base+3].x)^2 > c[base+3].y)   (enabled and not an ortho / w == 1 draw)
//     o        = rP + m * (t - rP)
//     o.xy    += rP.xy * (1 - m) * c[base+3].w       (HUD scale around the screen centre for the vertices left alone)
//     oPos     = o
//
// c[base+3] = (1, eps, enable, hudScale - 1). With enable = 0 and hudScale = 1 the shader outputs exactly what the
// game computed.
#include <cstdint>
#include <vector>
#include <string>

struct VsPatchInfo {
    bool patched = false;
    bool analyzed = false;      // pass 1 done: constRead / relConst are valid even when not patched
    uint32_t version = 0;       // e.g. 0xFFFE0300
    int posReg = -1;            // vs_3_0 output register of POSITION0 (-1 = oPos)
    int maxTemp = -1, maxConst = -1;
    bool relConst = false;      // reads constants with relative addressing (skinning palettes ...)
    int temps[3] = { -1, -1, -1 };
    uint32_t constRead[8] = {};   // bit set of constant registers read without relative addressing
    bool reads(int r) const { return r >= 0 && r < 256 && (constRead[r >> 5] >> (r & 31) & 1); }
    std::string reason;         // why it was not patched
};

// base = first of the 4 constant registers used by the tail (252 by default).
// allowRel = false: shaders that index constants relatively (they may read c<base>..) are analysed, not patched
// analyseOnly: pass 1 only (info.analyzed), the shader stays the game's own
bool VsPatch(const uint32_t* in, std::vector<uint32_t>& out, int base, VsPatchInfo& info, bool allowRel = true, bool analyseOnly = false);
// size in DWORDs of a shader (up to and including the end token), 0 if malformed
size_t VsSizeDwords(const uint32_t* in, size_t maxDwords = 1u << 20);
