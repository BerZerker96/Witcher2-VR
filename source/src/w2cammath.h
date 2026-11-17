// Pure math of the Witcher 2 engine camera (no Windows): shared by w2camera.cpp and the offline test that runs it
// against the game's own camera builder in an emulator (tests/check_camera.py).
#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>
#include "stereo.h"

namespace w2cam {
constexpr int kPos = 0x00, kEuler = 0x10, kFov = 0x1C, kAspect = 0x20, kZoom = 0x24, kNear = 0x28, kLod = 0x270, kOffX = 0x280, kOffY = 0x284;
constexpr float kDeg = 57.29577951f, kRad = 0.01745329252f;

struct Fields { float pos[3], eul[3], fov, aspect, zoom, nearZ, ox, oy; };   // nearZ = +0x28 (follows zoom)
inline void ReadF(const uint8_t* c, Fields& f) {
    memcpy(f.pos, c + kPos, 12); memcpy(f.eul, c + kEuler, 12);
    memcpy(&f.fov, c + kFov, 16);   // fov, aspect, zoom, near
    memcpy(&f.ox, c + kOffX, 4); memcpy(&f.oy, c + kOffY, 4);
}
inline void WriteF(uint8_t* c, const Fields& f) {
    memcpy(c + kPos, f.pos, 12); memcpy(c + kEuler, f.eul, 12);
    memcpy(c + kFov, &f.fov, 16);
    memcpy(c + kOffX, &f.ox, 4); memcpy(c + kOffY, &f.oy, 4);
}
inline bool Finite(const Fields& f) {
    const float* v = &f.pos[0];
    for (int i = 0; i < 12; i++) if (!std::isfinite(v[i]) || fabsf(v[i]) > 1e7f) return false;
    return true;
}
inline bool Perspective(const Fields& f) { return f.fov > 1.f && f.fov < 175.f && f.aspect > 0.1f && f.aspect < 10.f && f.zoom > 0.01f && f.zoom < 100.f; }
inline GameProj FieldsProj(const Fields& f) {
    GameProj p;
    float t = tanf(f.fov * 0.5f * kRad); if (t < 0.0005f) t = 0.0005f;
    p.p11 = f.zoom / t; p.p00 = p.p11 / f.aspect; p.p02 = -f.ox; p.p12 = f.oy;
    return p;
}

// ---- rotation helpers: rows = local right, forward, up in world (Z up), row-vector convention as in the engine
typedef float M3[3][3];
inline void Mul3(const M3 a, const M3 b, M3 o) { M3 t; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]; memcpy(o, t, sizeof t); }
inline void Ry(float a, M3 m) { float c = cosf(a * kRad), s = sinf(a * kRad); M3 r = { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } }; memcpy(m, r, sizeof r); }
inline void Rx(float a, M3 m) { float c = cosf(a * kRad), s = sinf(a * kRad); M3 r = { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } }; memcpy(m, r, sizeof r); }
inline void Rz(float a, M3 m) { float c = cosf(a * kRad), s = sinf(a * kRad); M3 r = { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } }; memcpy(m, r, sizeof r); }
inline void EulerToM(const float e[3], M3 m) { M3 a, b, c, ab; Ry(e[0], a); Rx(e[1], b); Rz(e[2], c); Mul3(a, b, ab); Mul3(ab, c, m); }
inline void MToEuler(const M3 m, float e[3]) {
    const float* f = m[1];
    float sp = f[2] > 1.f ? 1.f : f[2] < -1.f ? -1.f : f[2];
    float pitch = asinf(sp) * kDeg;
    float yaw = (fabsf(f[0]) + fabsf(f[1]) > 1e-6f) ? atan2f(-f[0], f[1]) * kDeg : 0.f;
    M3 rx, rz, r0; Rx(pitch, rx); Rz(yaw, rz); Mul3(rx, rz, r0);
    // m = Ry(roll) r0  ->  Ry(roll) = m r0^T ; Ry[0] = (cos, 0, -sin)
    float q00 = m[0][0] * r0[0][0] + m[0][1] * r0[0][1] + m[0][2] * r0[0][2];
    float q02 = m[0][0] * r0[2][0] + m[0][1] * r0[2][1] + m[0][2] * r0[2][2];
    float roll = atan2f(-q02, q00) * kDeg;
    e[0] = roll; e[1] = pitch; e[2] = yaw;
}
// OpenXR head rotation (x right, y up, -z forward) as rows (right, forward, up) in the camera's local frame
inline void HeadRows(const xrb::IpcPose& q, M3 h) {
    float x = q.qx, y = q.qy, z = q.qz, w = q.qw;
    float R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
                      { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
                      { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) } };   // columns = head axes
    auto conv = [](float vx, float vy, float vz, float* o) { o[0] = vx; o[1] = -vz; o[2] = vy; };   // xr -> (r, f, u)
    conv(R[0][0], R[1][0], R[2][0], h[0]);            // right   = col 0
    conv(-R[0][2], -R[1][2], -R[2][2], h[1]);         // forward = -col 2
    conv(R[0][1], R[1][1], R[2][1], h[2]);            // up      = col 1
}

// the projection for one eye: its headset FOV (tangents), widened by fov_margin and to the back buffer's shape
// (square pixels), plus the eye tag when both eyes would otherwise be identical
inline GameProj EyeProj(const xrb::IpcFov& f, int eye, const xrb::IpcFov* other, float fovMargin, unsigned bbW, unsigned bbH, bool tagEyes) {
    float L = tanf(f.left), R = tanf(f.right), U = tanf(f.up), D = tanf(f.down);
    float m = fovMargin < 1.f ? 1.f : fovMargin > 1.6f ? 1.6f : fovMargin;
    float cx = (R + L) * 0.5f, cy = (U + D) * 0.5f, hw = (R - L) * 0.5f * m, hh = (U - D) * 0.5f * m;
    if (bbW && bbH) {
        float A = (float)bbW / (float)bbH;
        if (hw / hh < A) hw = hh * A; else hh = hw / A;
    }
    if (hw > 11.f) hw = 11.f;   // < 170 degrees
    if (hh > 11.f) hh = 11.f;
    GameProj p;
    p.p00 = 1.f / hw; p.p11 = 1.f / hh; p.p02 = -cx / hw; p.p12 = -cy / hh;
    if (tagEyes && eye < 2 && other) {
        float oL = tanf(other->left), oR = tanf(other->right);
        float op02 = -((oR + oL) * 0.5f) / ((oR - oL) * 0.5f * m);
        if (fabsf(p.p02 - op02) < 0.012f) {
            // both eyes alike (e.g. symmetric eye views): the left eye's view is widened by 1 %, the right's by 2 % -
            // both stay centred (no off-centre shift that screen-space effects would trip over) and the pixels square.
            // The FOV sent to the headset is taken from this very projection, so nothing is misplaced.
            float t = eye == 0 ? 1.01f : 1.02f;
            p.p00 /= t; p.p11 /= t;
        }
    }
    return p;
}


struct ComposeOpts { bool decoupledPitch, positional; float worldScale; float camOffset[3]; float fovMargin; unsigned bbW, bbH; bool tagEyes;
                     // first person: the orbit camera's pivot is fpDist ahead along its (pitched) view axis, i.e. where
                     // Enhanced Camera's first-person template (cameraFurther = -3) puts it; fpRight / fpUp shift it (metres)
                     bool firstPerson; float fpDist, fpRight, fpUp, fpNear; };
// base camera inputs + one eye (pose / fov in OpenXR convention; centre = the head centre for rotation-only tracking)
inline bool ComposeEye(const Fields& base, const xrb::IpcPose& pose, const xrb::IpcFov& fov, const xrb::IpcFov* otherFov, int eye,
                       const float centre[3], const ComposeOpts& o, Fields& out) {
    out = base;
    M3 Rg, Rb, H, Rf;
    EulerToM(base.eul, Rg);
    if (o.decoupledPitch) Rz(base.eul[2], Rb); else memcpy(Rb, Rg, sizeof Rb);
    HeadRows(pose, H);
    Mul3(H, Rb, Rf);
    MToEuler(Rf, out.eul);
    float bpos[3] = { base.pos[0], base.pos[1], base.pos[2] };
    if (o.firstPerson) {   // Rg rows: right, forward (pitched), up of the game camera
        for (int i = 0; i < 3; i++) bpos[i] += Rg[1][i] * o.fpDist + Rb[0][i] * o.fpRight;
        bpos[2] += o.fpUp;
        if (out.nearZ < o.fpNear) out.nearZ = o.fpNear;   // clips the inside of Geralt's head
    }
    float ws = o.worldScale, lr = 0, lf = 0, lu = 0;
    if (o.positional) { lr = pose.px * ws; lf = -pose.pz * ws; lu = pose.py * ws; }
    else if (centre) { lr = (pose.px - centre[0]) * ws; lf = -(pose.pz - centre[2]) * ws; lu = (pose.py - centre[1]) * ws; }
    lr += o.camOffset[0]; lu += o.camOffset[1]; lf += o.camOffset[2];
    for (int i = 0; i < 3; i++) out.pos[i] = bpos[i] + lr * Rb[0][i] + lf * Rb[1][i] + lu * Rb[2][i];
    GameProj ep = EyeProj(fov, eye, otherFov, o.fovMargin, o.bbW, o.bbH, o.tagEyes);
    out.zoom = 1.f;
    out.fov = 2.f * atanf(1.f / ep.p11) * kDeg;
    out.aspect = ep.p11 / ep.p00;
    out.ox = -ep.p02; out.oy = ep.p12;
    return Finite(out) && Perspective(out);
}
} // namespace w2cam
