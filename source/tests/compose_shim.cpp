// Native (Linux) build of the engine-camera math for tests/check_camera.py.
#include "w2cammath.h"
using namespace w2cam;
extern "C" int w2_compose(const float* base, const float* pose, const float* fov, const float* ofov, int eye, const float* centre,
                          int decoupled, int positional, float ws, const float* off, float margin, unsigned bbw, unsigned bbh, int tag, float* out,
                          int fp, float fpDist, float fpRight, float fpUp, float fpNear) {
    Fields b, o; memcpy(&b, base, sizeof b);
    xrb::IpcPose p = { pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6] };
    xrb::IpcFov f = { fov[0], fov[1], fov[2], fov[3] }, of = { ofov[0], ofov[1], ofov[2], ofov[3] };
    ComposeOpts c = { decoupled != 0, positional != 0, ws, { off[0], off[1], off[2] }, margin, bbw, bbh, tag != 0, fp != 0, fpDist, fpRight, fpUp, fpNear };
    bool ok = ComposeEye(b, p, f, &of, eye, centre, c, o);
    memcpy(out, &o, sizeof o);
    return ok;
}
extern "C" void w2_euler_roundtrip(const float* e, float* o) { M3 m; EulerToM(e, m); MToEuler(m, o); }
