#include "stereo.h"
#include <cmath>
#include <algorithm>

namespace {
inline float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool TestRows(const float r[4][4], GameProj& out) {
    const float* a3 = r[3];
    float n3 = Dot3(a3, a3);
    if (fabsf(n3 - 1.f) > 0.02f) return false;          // w row = view z axis (unit, rigid view)
    float x0[3], y0[3], z0[3];
    float p02 = Dot3(r[0], a3) / n3, p12 = Dot3(r[1], a3) / n3, a = Dot3(r[2], a3) / n3;
    for (int i = 0; i < 3; i++) { x0[i] = r[0][i] - p02 * a3[i]; y0[i] = r[1][i] - p12 * a3[i]; z0[i] = r[2][i] - a * a3[i]; }
    float p00 = sqrtf(Dot3(x0, x0)), p11 = sqrtf(Dot3(y0, y0));
    if (p00 < 0.15f || p00 > 10.f || p11 < 0.15f || p11 > 10.f) return false;
    if (fabsf(Dot3(x0, y0)) > 0.01f * p00 * p11) return false;   // view x / y axes orthogonal
    if (sqrtf(Dot3(z0, z0)) > 0.01f * (fabsf(a) + 1e-3f) + 1e-4f) return false;   // depth row along view z
    if (fabsf(a) < 1e-4f) return false;
    out.p00 = p00; out.p02 = p02; out.p11 = p11; out.p12 = p12;
    // view rows in object space: x0 / p00, y0 / p11, a3 (unit) -> image of the world up axis in view space
    float s3 = 1.f / sqrtf(n3);
    out.upZ[0] = x0[2] / p00; out.upZ[1] = y0[2] / p11; out.upZ[2] = a3[2] * s3;
    out.upY[0] = x0[1] / p00; out.upY[1] = y0[1] / p11; out.upY[2] = a3[1] * s3;
    return true;
}

struct M4 { float m[4][4]; };
M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; }
    return r;
}
} // namespace

bool DetectProjection(const float regs[16], GameProj& out) {
    float r[4][4], t[4][4];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { r[i][j] = regs[i * 4 + j]; t[j][i] = regs[i * 4 + j]; }
    for (int i = 0; i < 16; i++) if (!std::isfinite(regs[i])) return false;
    return TestRows(r, out) || TestRows(t, out);
}

bool ProjClose(const GameProj& a, const GameProj& b, float tol) {
    return fabsf(a.p00 - b.p00) <= tol * b.p00 && fabsf(a.p11 - b.p11) <= tol * b.p11 &&
           fabsf(a.p02 - b.p02) <= tol && fabsf(a.p12 - b.p12) <= tol;
}

bool PitchFromUp(const float u[3], float* pitch) {
    float n = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (fabsf(n - 1.f) > 0.05f || u[1] < 0.2f || fabsf(u[0]) > 0.5f) return false;   // camera upright-ish, little roll
    // up = (0, cos p, sin p) for a camera pitched by p (looking up: the world's up axis tilts towards the view direction)
    *pitch = atan2f(u[2], u[1]);
    return true;
}

void EyeClipMatrix(const GameProj& in, const GameProj& out, const xrb::IpcPose& e, float ws, bool positional,
                   float levelPitch, const float offset[3], float m[12]) {
    // q = (x_c, y_c, 1, w_c) -> view point (x, y, z, 1)
    M4 pin{};
    pin.m[0][0] = 1.f / in.p00; pin.m[0][3] = -in.p02 / in.p00;
    pin.m[1][1] = 1.f / in.p11; pin.m[1][3] = -in.p12 / in.p11;
    pin.m[2][3] = 1.f;
    pin.m[3][2] = 1.f;
    // eye pose: OpenXR (x right, y up, -z forward) -> canonical (z forward): S R S, S p with S = diag(1, 1, -1)
    float qx = e.qx, qy = e.qy, qz = e.qz, qw = e.qw;
    float R[3][3] = {
        { 1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw) },
        { 2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw) },
        { 2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy) } };
    const float S[3] = { 1, 1, -1 };
    float Rc[3][3], pc[3] = { e.px * ws, e.py * ws, -e.pz * ws };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rc[i][j] = S[i] * R[i][j] * S[j];
    if (!positional) pc[0] = pc[1] = pc[2] = 0;
    if (offset) { pc[0] += offset[0]; pc[1] += offset[1]; pc[2] += offset[2]; }
    // view of the eye: v' = Rc^T (v - pc)
    M4 ev{};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) ev.m[i][j] = Rc[j][i];
        ev.m[i][3] = -(Rc[0][i] * pc[0] + Rc[1][i] * pc[1] + Rc[2][i] * pc[2]);
    }
    ev.m[3][3] = 1.f;
    // view -> clip x, y, w of the output projection (row 2 = w)
    M4 po{};
    po.m[0][0] = out.p00; po.m[0][2] = out.p02;
    po.m[1][1] = out.p11; po.m[1][2] = out.p12;
    po.m[2][2] = 1.f;
    po.m[3][3] = 1.f;
    // decoupled pitch: rotate the game camera's view about its x axis so that it looks level (v_level = Rx(p) v)
    M4 lv{};
    float cp = cosf(levelPitch), sp = sinf(levelPitch);
    lv.m[0][0] = 1; lv.m[1][1] = cp; lv.m[1][2] = sp; lv.m[2][1] = -sp; lv.m[2][2] = cp; lv.m[3][3] = 1;
    M4 r = Mul(po, Mul(ev, Mul(lv, pin)));
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) m[i * 4 + j] = r.m[i][j];
}

xrb::IpcFov ProjToFov(const GameProj& p) {
    xrb::IpcFov f;
    f.left = atanf((-1.f - p.p02) / p.p00); f.right = atanf((1.f - p.p02) / p.p00);
    f.up = atanf((1.f - p.p12) / p.p11);    f.down = atanf((-1.f - p.p12) / p.p11);
    return f;
}

GameProj WidenProj(const GameProj& p, float s, const xrb::IpcFov& hs) {
    GameProj o = p;
    if (s <= 1.001f) return o;
    float tl = tanf(-hs.left), tr = tanf(hs.right), tu = tanf(hs.up), td = tanf(-hs.down);
    float hmaxX = std::max(tl, tr), hmaxY = std::max(tu, td);
    float gx = (1.f + fabsf(p.p02)) / p.p00, gy = (1.f + fabsf(p.p12)) / p.p11;   // game half-extent (tangent)
    if (hmaxX > 0 && hmaxY > 0) s = std::min(s, std::max(1.f, std::min(hmaxX / gx, hmaxY / gy)));
    o.p00 = p.p00 / s; o.p11 = p.p11 / s;
    return o;
}

xrb::IpcFov ProjToImageFov(const GameProj& p, const float c[4]) {
    float L = (-1.f - p.p02) / p.p00, R = (1.f - p.p02) / p.p00, U = (1.f - p.p12) / p.p11, D = (-1.f - p.p12) / p.p11;
    float cw = c[2] - c[0], ch = c[3] - c[1];
    if (cw <= 0.01f || ch <= 0.01f) return ProjToFov(p);
    float l = L - (R - L) * c[0] / cw, r = L + (R - L) * (1.f - c[0]) / cw;
    float u = U + (D - U) * (0.f - c[1]) / ch, d = U + (D - U) * (1.f - c[1]) / ch;
    xrb::IpcFov f; f.left = atanf(l); f.right = atanf(r); f.up = atanf(u); f.down = atanf(d);
    return f;
}

GameProj MatchAspect(const GameProj& p, float aspect, bool crop) {
    GameProj o = p;
    if (!(aspect > 0.05f) || !p.valid()) return o;
    float tx = 1.f / p.p00, ty = 1.f / p.p11;      // half extents (tangent units)
    if (tx / ty > aspect * 1.001f) { if (crop) o.p00 = 1.f / (ty * aspect); else o.p11 = 1.f / (tx / aspect); }   // too wide
    else if (tx / ty < aspect / 1.001f) { if (crop) o.p11 = 1.f / (tx / aspect); else o.p00 = 1.f / (ty * aspect); }   // too tall
    return o;
}
