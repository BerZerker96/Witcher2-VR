#pragma once
// Clip-space stereo / head tracking math. Engine-independent: the game's projection is recovered from the
// view-projection matrices it uploads as vertex shader constants, and the head / eye pose is applied as a 3x4
// matrix on the clip-space position (see vspatch.h).
//
// Canonical view space (recovered from the clip position itself): x right, y up, z forward, w_clip = z.
// Game projection:  x_c = p00 x + p02 z,  y_c = p11 y + p12 z,  w_c = z   (z_c is left as the game made it).
#include "xrbridge/ipc.h"

struct GameProj {
    float p00 = 0, p02 = 0, p11 = 0, p12 = 0;
    // filled by DetectProjection: the world's up axis seen from the camera (canonical view space), for a Z-up and for a
    // Y-up world (only one is right; the object's world matrix rotates around the up axis, so this is the camera's)
    float upZ[3] = {}, upY[3] = {};
    bool valid() const { return p00 > 0 && p11 > 0; }
};
// pitch of the camera from an up vector (radians, positive = looking up); false if the vector is not plausible
bool PitchFromUp(const float up[3], float* pitch);

// 4 consecutive vec4 constant registers -> perspective projection, if they hold a (view-)projection matrix with a
// rigid view part. Tries both register layouts (registers = rows of clip = M * v, or = columns).
bool DetectProjection(const float regs[16], GameProj& out);
bool ProjClose(const GameProj& a, const GameProj& b, float relTol);

// Rows x, y, w of the clip-space transform for one eye, applied to q = (x_c, y_c, 1, w_c):
//   in  = the projection the game used for this draw, out = the projection of the image sent to the headset,
//   eye = eye pose relative to the recentered reference (OpenXR convention, metres).
//   levelPitch = the game camera's pitch to remove (decoupled pitch, 0 = keep), offset = camera offset in metres
//   (x right, y up, z forward; applied in the levelled camera frame).
void EyeClipMatrix(const GameProj& in, const GameProj& out, const xrb::IpcPose& eye, float worldScale,
                   bool positional, float levelPitch, const float offset[3], float m[12]);
// FOV of an image rendered with this projection (radians, OpenXR convention)
xrb::IpcFov ProjToFov(const GameProj& p);
// FOV of the whole image when the projection p covers only the rectangle c (x0, y0, x1, y1 in 0..1, y down)
xrb::IpcFov ProjToImageFov(const GameProj& p, const float c[4]);
// extend the narrower side of p so that its tangent extents have the given width / height ratio (square pixels)
GameProj MatchAspect(const GameProj& p, float aspect, bool crop = false);   // crop: stay inside p (never show what the game culled)
// out projection = game projection widened by fovScale (>= 1), clamped to the headset's FOV
GameProj WidenProj(const GameProj& p, float fovScale, const xrb::IpcFov& headset);
