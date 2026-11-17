# The view-projection REDengine builds for an eye (+0x1C0, row-vector) as a shader would see it - as rows (row_major)
# or transposed (HLSL column_major default), with and without an object world matrix - must give back exactly the
# projection W2VR wrote for that eye (DetectProjection, the per-draw eye identification in d3d9hooks.cpp).
import sys, math, random, ctypes
import os; HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools')); sys.argv = sys.argv[:1]
import numpy as np
src = open(os.path.join(HERE, 'check_camera.py')).read().split("random.seed(7)")[0]
exec(src)
det = ctypes.CDLL(os.path.join(HERE, 'build', 'libw2detect.so'))
random.seed(3); worst = 0; fails = 0; n = 0
for i in range(200):
    base = [random.uniform(-300, 300), random.uniform(-300, 300), random.uniform(0, 30), 0, random.uniform(-40, 40), random.uniform(-180, 180), 55, 1.0, 1.0, 0.1, 0, 0]
    q = qeuler(math.radians(random.uniform(-60, 60)), math.radians(random.uniform(-40, 40)), 0)
    fv = (-0.94, 0.87, 0.85, -0.96) if i % 2 else (-0.87, 0.94, 0.85, -0.96)
    out = (ctypes.c_float * 12)()
    lib.w2_compose(arr(base), arr([0.03, 0, 0] + list(q)), arr(fv), arr((-0.87, 0.94, 0.85, -0.96)), i % 2, arr([0, 0, 0]), 1, 1, ctypes.c_float(1), arr([0, 0, 0]), ctypes.c_float(1), 2000, 2000, 1, out, 0, ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0))
    out = list(out)
    V, P, _, _ = build(out)
    want = (1 / math.tan(math.radians(out[6]) / 2) / out[7], -out[10], 1 / math.tan(math.radians(out[6]) / 2), out[11])
    # object world matrix: rotation about Z + translation near the camera (unit scale, as most meshes)
    a = random.uniform(0, 6.28); W = np.eye(4); W[:2, :2] = [[math.cos(a), math.sin(a)], [-math.sin(a), math.cos(a)]]; W[3, :3] = np.array(base[:3]) + np.random.uniform(-20, 20, 3)
    for M in (V @ P, W @ V @ P):
        for regs in (M.T, M):   # column_major upload (registers = columns) / row_major
            r = (ctypes.c_float * 16)(*[float(x) for x in regs.flatten()]); o = (ctypes.c_float * 4)()
            n += 1
            if not det.w2_detect(r, o): fails += 1; continue
            worst = max(worst, max(abs(o[k] - want[k]) for k in range(4)))
print('uploads tested', n, 'not recognised', fails, 'max projection error %.2e' % worst)
ok = fails == 0 and worst < 1e-3
print('RESULT:', 'PASS' if ok else 'FAIL'); sys.exit(0 if ok else 1)
