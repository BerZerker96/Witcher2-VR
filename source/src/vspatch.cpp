#include "vspatch.h"

namespace {
enum : uint32_t {
    OP_MOV = 1, OP_ADD = 2, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_DP4 = 9, OP_SLT = 12,
    OP_RET = 28, OP_LABEL = 30, OP_DCL = 31, OP_DEFB = 47, OP_DEFI = 48, OP_DEF = 81,
    OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};
enum : uint32_t { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_OUTPUT = 6 };
constexpr uint32_t kRel = 1u << 13;

inline uint32_t RegType(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
inline uint32_t RegNum(uint32_t t) { return t & 0x7FF; }
inline uint32_t Param(uint32_t type, uint32_t num) {
    return 0x80000000u | (num & 0x7FF) | ((type & 7) << 28) | ((type & 0x18) << 8);
}
inline uint32_t Dst(uint32_t type, uint32_t num, uint32_t mask) { return Param(type, num) | (mask << 16); }
inline uint32_t Src(uint32_t type, uint32_t num, uint32_t swz = 0xE4, bool neg = false) {
    return Param(type, num) | (swz << 16) | ((neg ? 1u : 0u) << 24);
}
inline uint32_t Ins(uint32_t op, uint32_t nParams) { return op | (nParams << 24); }
constexpr uint32_t X = 1, Y = 2, Z = 4, W = 8, XYZW = 15;
constexpr uint32_t SWZ_X = 0x00, SWZ_Y = 0x55, SWZ_Z = 0xAA, SWZ_W = 0xFF;
} // namespace

size_t VsSizeDwords(const uint32_t* in, size_t maxDwords) {
    if (!in || (in[0] & 0xFFFF0000u) != 0xFFFE0000u) return 0;
    size_t i = 1;
    while (i < maxDwords) {
        uint32_t t = in[i];
        uint32_t op = t & 0xFFFF;
        if (t == OP_END) return i + 1;
        if (op == OP_COMMENT) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        if ((in[0] & 0xFF00) < 0x0200) return 0;   // vs_1_x has no length field: not handled
        i += 1 + ((t >> 24) & 0xF);
    }
    return 0;
}

bool VsPatch(const uint32_t* in, std::vector<uint32_t>& out, int base, VsPatchInfo& info, bool allowRel, bool analyseOnly) {
    info = VsPatchInfo{};
    info.version = in[0];
    uint32_t major = (in[0] >> 8) & 0xFF;
    if ((in[0] & 0xFFFF0000u) != 0xFFFE0000u) { info.reason = "not a vertex shader"; return false; }
    if (major < 2) { info.reason = "vs_1_x"; return false; }
    size_t n = VsSizeDwords(in);
    if (!n) { info.reason = "malformed"; return false; }
    const uint32_t minor = in[0] & 0xFF;
    const int maxTemps = (major >= 3 || minor >= 1) ? 32 : 12;   // vs_2_0: 12; vs_2_x / vs_3_0: 32

    // pass 1: find the position output, temps / constants in use, the insertion point
    size_t insertAt = n - 1;              // before END by default
    size_t lastRet = 0; bool sawLabel = false;
    for (size_t i = 1; i < n - 1;) {
        uint32_t t = in[i], op = t & 0xFFFF;
        if (op == OP_COMMENT) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
        uint32_t len = (t >> 24) & 0xF;
        if (op == OP_DCL && len >= 2) {
            uint32_t usage = in[i + 1] & 0x1F, uidx = (in[i + 1] >> 16) & 0xF, d = in[i + 2];
            if (major >= 3 && RegType(d) == RT_OUTPUT && usage == 0 /*POSITION*/ && uidx == 0) {
                if (((d >> 16) & 0xF) != XYZW) { info.reason = "partial position declaration"; return false; }
                info.posReg = (int)RegNum(d);
            }
        } else if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB) {
            if (RegType(in[i + 1]) == RT_CONST && (int)RegNum(in[i + 1]) > info.maxConst) info.maxConst = (int)RegNum(in[i + 1]);
            if (op == OP_DEF && RegType(in[i + 1]) == RT_CONST && (int)RegNum(in[i + 1]) >= base) { info.reason = "def in reserved constants"; return false; }
        } else {
            if (op == OP_LABEL && !sawLabel) { sawLabel = true; if (lastRet) insertAt = lastRet; }
            if (op == OP_RET && !sawLabel) lastRet = i;
            for (uint32_t k = 1; k <= len; k++) {
                uint32_t p = in[i + k];
                uint32_t rt = RegType(p), rn = RegNum(p);
                if (rt == RT_TEMP && (int)rn > info.maxTemp) info.maxTemp = (int)rn;
                if (rt == RT_CONST) {
                    if ((int)rn > info.maxConst) info.maxConst = (int)rn;
                    if (p & kRel) info.relConst = true;
                    else if (k > 1 && rn < 256) info.constRead[rn >> 5] |= 1u << (rn & 31);
                }
                if (k == 1 && (p & kRel) && (rt == RT_OUTPUT || rt == RT_RASTOUT)) { info.reason = "relative output write"; return false; }
                // a relative-addressed source in SM2+ is followed by its address token: skip it
                if (k > 1 && (p & kRel) && major >= 2) k++;
            }
        }
        i += 1 + len;
    }
    info.analyzed = true;
    if (analyseOnly) { info.reason = "analysed only (engine camera, HUD on the panel: the game's shader is used unchanged)"; return false; }
    if (sawLabel && !lastRet) { info.reason = "subroutines without main ret"; return false; }
    if (major >= 3 && info.posReg < 0) { info.reason = "no POSITION output"; return false; }
    if (info.maxConst >= base && !info.relConst) { info.reason = "uses reserved constants"; return false; }
    // skinning palettes / instance arrays (c[a0 + n]) can reach the reserved registers: the game's data there would
    // be overwritten by the tail's constants before every draw
    if (info.relConst && !allowRel) { info.reason = "relative constant addressing (game data may sit in the reserved registers)"; return false; }
    // rP must be a new temp; the tail's two scratch registers can be any other temps, because the tail runs at the very
    // end of the main program where none of the shader's own temps are read again.
    if (info.maxTemp + 1 >= maxTemps) { info.reason = "no free temp register"; return false; }
    const uint32_t rP = info.maxTemp + 1;
    const uint32_t rT = rP >= 2 ? 0 : rP + 1, rU = rP >= 2 ? 1 : rP + 2;
    if ((int)rU >= maxTemps) { info.reason = "no free temp register"; return false; }
    info.temps[0] = rP; info.temps[1] = rT; info.temps[2] = rU;
    const uint32_t posType = major >= 3 ? RT_OUTPUT : RT_RASTOUT, posNum = major >= 3 ? (uint32_t)info.posReg : 0;

    // pass 2: copy with position writes redirected, tail inserted at insertAt
    out.clear(); out.reserve(n + 64);
    out.push_back(in[0]);
    int redirected = 0;
    auto tail = [&] {
        const uint32_t c0 = base, c1 = base + 1, c2 = base + 2, c3 = base + 3;
        auto emit = [&](std::initializer_list<uint32_t> l) { for (uint32_t v : l) out.push_back(v); };
        emit({ Ins(OP_MOV, 2), Dst(RT_TEMP, rU, XYZW), Src(RT_TEMP, rP) });
        emit({ Ins(OP_MOV, 2), Dst(RT_TEMP, rU, Z), Src(RT_CONST, c3, SWZ_X) });
        emit({ Ins(OP_DP4, 3), Dst(RT_TEMP, rT, X), Src(RT_TEMP, rU), Src(RT_CONST, c0) });
        emit({ Ins(OP_DP4, 3), Dst(RT_TEMP, rT, Y), Src(RT_TEMP, rU), Src(RT_CONST, c1) });
        emit({ Ins(OP_DP4, 3), Dst(RT_TEMP, rT, W), Src(RT_TEMP, rU), Src(RT_CONST, c2) });
        emit({ Ins(OP_RCP, 2), Dst(RT_TEMP, rU, X), Src(RT_TEMP, rP, SWZ_W) });
        emit({ Ins(OP_MUL, 3), Dst(RT_TEMP, rU, X), Src(RT_TEMP, rU, SWZ_X), Src(RT_TEMP, rT, SWZ_W) });
        emit({ Ins(OP_MUL, 3), Dst(RT_TEMP, rT, Z), Src(RT_TEMP, rP, SWZ_Z), Src(RT_TEMP, rU, SWZ_X) });
        emit({ Ins(OP_ADD, 3), Dst(RT_TEMP, rU, Y), Src(RT_TEMP, rP, SWZ_W), Src(RT_CONST, c3, SWZ_X, true) });
        emit({ Ins(OP_MUL, 3), Dst(RT_TEMP, rU, Y), Src(RT_TEMP, rU, SWZ_Y), Src(RT_TEMP, rU, SWZ_Y) });
        emit({ Ins(OP_SLT, 3), Dst(RT_TEMP, rU, Y), Src(RT_CONST, c3, SWZ_Y), Src(RT_TEMP, rU, SWZ_Y) });
        emit({ Ins(OP_MUL, 3), Dst(RT_TEMP, rU, Y), Src(RT_TEMP, rU, SWZ_Y), Src(RT_CONST, c3, SWZ_Z) });
        emit({ Ins(OP_ADD, 3), Dst(RT_TEMP, rT, XYZW), Src(RT_TEMP, rT), Src(RT_TEMP, rP, 0xE4, true) });
        emit({ Ins(OP_MAD, 4), Dst(RT_TEMP, rT, XYZW), Src(RT_TEMP, rT), Src(RT_TEMP, rU, SWZ_Y), Src(RT_TEMP, rP) });
        // HUD (c3.w = 1; the eye transform is off for such draws, so c0 holds (sx-1, sy-1, tx, ty)): vertices the eye
        // transform left alone get xy += xy * (sx-1, sy-1) + (tx, ty) * w - size and position of the HUD
        emit({ Ins(OP_ADD, 3), Dst(RT_TEMP, rU, X), Src(RT_CONST, c3, SWZ_X), Src(RT_TEMP, rU, SWZ_Y, true) });
        emit({ Ins(OP_MUL, 3), Dst(RT_TEMP, rU, X), Src(RT_TEMP, rU, SWZ_X), Src(RT_CONST, c3, SWZ_W) });
        emit({ Ins(OP_MUL, 3), Dst(RT_TEMP, rU, Z | W), Src(RT_CONST, c0, 0xEE), Src(RT_TEMP, rP, SWZ_W) });
        emit({ Ins(OP_MAD, 4), Dst(RT_TEMP, rU, Z | W), Src(RT_TEMP, rP, 0x44), Src(RT_CONST, c0, 0x44), Src(RT_TEMP, rU, 0xE4) });
        emit({ Ins(OP_MAD, 4), Dst(RT_TEMP, rT, X | Y), Src(RT_TEMP, rU, 0xEE), Src(RT_TEMP, rU, SWZ_X), Src(RT_TEMP, rT) });
        emit({ Ins(OP_MOV, 2), Dst(posType, posNum, XYZW), Src(RT_TEMP, rT) });
    };
    for (size_t i = 1; i < n;) {
        if (i == insertAt) tail();
        uint32_t t = in[i], op = t & 0xFFFF;
        if (t == OP_END) { out.push_back(t); break; }
        if (op == OP_COMMENT) {
            size_t len = 1 + ((t >> 16) & 0x7FFF);
            out.insert(out.end(), in + i, in + i + len); i += len; continue;
        }
        uint32_t len = (t >> 24) & 0xF;
        out.push_back(t);
        for (uint32_t k = 1; k <= len; k++) {
            uint32_t p = in[i + k];
            if (k == 1 && op != OP_DCL && op != OP_DEF && op != OP_DEFI && op != OP_DEFB &&
                RegType(p) == posType && RegNum(p) == posNum) {
                p = (p & ~0x70001FFFu & ~0x00001800u) | (rP & 0x7FF);   // type TEMP (0), number rP, mask/modifiers kept
                redirected++;
            }
            out.push_back(p);
        }
        i += 1 + len;
    }
    if (!redirected) { info.reason = "position never written"; out.clear(); return false; }
    info.patched = true;
    return true;
}
