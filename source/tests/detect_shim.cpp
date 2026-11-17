#include "stereo.h"
extern "C" int w2_detect(const float* regs, float* out) { GameProj p; if (!DetectProjection(regs, p)) return 0; out[0]=p.p00; out[1]=p.p02; out[2]=p.p11; out[3]=p.p12; return 1; }
