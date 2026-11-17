# End-to-end check of the W2VR engine camera against witcher2.exe's own render-camera builder (0xB067F5), run in an
# x86 emulator on the (locally decrypted) exe. For random game cameras and head poses:
#   W2VR math (native build of w2cammath.h) -> camera inputs -> the game's builder -> world->view / projection,
# compared with the eye view derived independently: world->view of the game camera, then the eye pose in that view.
import sys, math, random, ctypes, struct
import os; HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools'))
import numpy as np
from emu import mk, call, wf, rf
lib = ctypes.CDLL(os.path.join(HERE, 'build', 'libw2compose.so'))
F = ctypes.c_float * 16
mu = mk(); C = 0x10090000
def build(f):   # f = pos3 eul3 fov aspect zoom near ox oy
    mu.mem_write(C, b'\0' * 0x300)
    wf(mu, C, list(f[0:3]) + [1]); wf(mu, C + 0x10, f[3:6]); wf(mu, C + 0x1c, (f[6], f[7], f[8], f[9], 3000.0)); wf(mu, C + 0x280, (f[10], f[11]))
    call(mu, 0xb067f5, {}, stack_args=(C,))
    g = lambda o: np.array(rf(mu, C + o, 16), dtype=np.float64).reshape(4, 4)
    return g(0x40), g(0x180), g(0x140), g(0x100)
def qmat(q):
    x, y, z, w = q
    return np.array([[1-2*(y*y+z*z), 2*(x*y-w*z), 2*(x*z+w*y)], [2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x)], [2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y)]])
def qeuler(yaw, pitch, roll):  # OpenXR: yaw about +Y, pitch about +X, roll about -Z (forward)
    def ax(a, v): s = math.sin(a/2); return np.array([v[0]*s, v[1]*s, v[2]*s, math.cos(a/2)])
    def mul(a, b):
        ax_, ay, az, aw = a; bx, by, bz, bw = b
        return np.array([aw*bx+ax_*bw+ay*bz-az*by, aw*by-ax_*bz+ay*bw+az*bx, aw*bz+ax_*by-ay*bx+az*bw, aw*bw-ax_*bx-ay*by-az*bz])
    return mul(mul(ax(yaw, (0, 1, 0)), ax(pitch, (1, 0, 0))), ax(roll, (0, 0, -1)))
def arr(v): return (ctypes.c_float * len(v))(*v)
random.seed(7); worst = {'view': 0, 'proj': 0, 'cover': 0, 'vp': 0}
S = np.diag([1, 1, -1])
N = int(sys.argv[1]) if len(sys.argv) > 1 else 300
for i in range(N):
    base = [random.uniform(-500, 500), random.uniform(-500, 500), random.uniform(-20, 50),
            random.uniform(-8, 8), random.uniform(-55, 55), random.uniform(-180, 180),
            random.uniform(40, 75), random.choice([1.0, 1.25, 1.333, 1.778]), 1.0, 0.1, 0.0, 0.0]
    decoupled = i % 2
    q = qeuler(math.radians(random.uniform(-80, 80)), math.radians(random.uniform(-60, 60)), math.radians(random.uniform(-25, 25)))
    p = [random.uniform(-0.4, 0.4), random.uniform(-0.3, 0.3), random.uniform(-0.4, 0.4)]
    eye = i % 3 if i % 5 else 1
    fovs = [(-0.94, 0.87, 0.85, -0.96), (-0.87, 0.94, 0.85, -0.96)] if i % 4 else [(-0.9, 0.9, 0.9, -0.9)] * 2
    fv = fovs[eye if eye < 2 else 0]; ofv = fovs[(eye ^ 1) if eye < 2 else 1]
    bbw, bbh = random.choice([(2000, 2000), (1832, 1920), (2064, 2096)])
    out = (ctypes.c_float * 12)()
    ok = lib.w2_compose(arr(base), arr(p + list(q)), arr(fv), arr(ofv), eye, arr([0, 0, 0]), decoupled, 1, ctypes.c_float(1.0), arr([0, 0, 0]),
                        ctypes.c_float(1.0), bbw, bbh, 1, out, 0, ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0))
    assert ok
    out = list(out)
    V_out, P_out, IP_out, IV_out = build(out)
    g = list(base)
    if decoupled: g[3] = 0.0; g[4] = 0.0
    V_g, _, _, _ = build(g)
    Re = S @ qmat(q) @ S; pe = S @ np.array(p)
    T = np.eye(4); T[3, :3] = -pe
    M = np.eye(4); M[:3, :3] = Re
    V_exp = V_g @ T @ M
    rot_err = np.abs(V_out[:3, :3] - V_exp[:3, :3]).max()
    tr_err = np.abs(V_out[3, :3] - V_exp[3, :3]).max() / (1 + np.abs(V_exp[3, :3]).max())
    worst['view'] = max(worst['view'], rot_err, tr_err * 10)
    # projection: every corner of this eye's headset FOV is inside the frustum, the tighter axis exactly on the edge
    L, R, U, D = [math.tan(a) for a in fv]
    nd = []
    for tx in (L, R):
        for ty in (U, D):
            c = np.array([tx, ty, 1.0, 1.0]) @ P_out; nd.append((c[0] / c[3], c[1] / c[3]))
    mx = max(max(abs(a), abs(b)) for a, b in nd)
    worst['cover'] = max(worst['cover'], mx - 1.0)        # > 0 would cut the headset view
    tight = min(max(abs(a) for a, b in nd), max(abs(b) for a, b in nd))
    # square pixels: frustum tangent extents have the back buffer's shape
    ext_x = 2 / P_out[0, 0]; ext_y = 2 / P_out[1, 1]
    worst['proj'] = max(worst['proj'], abs((ext_x / ext_y) / (bbw / bbh) - 1))
    # the engine's own inverses (deferred lighting reconstructs positions with them) stay exact with the off-centre terms
    worst['vp'] = max(worst['vp'], np.abs(P_out @ IP_out - np.eye(4)).max(), np.abs((V_out @ IV_out - np.eye(4))[:3, :3]).max())
# first person: identity head pose, eye at the orbit pivot (game camera + dist along its pitched view + right + up), near raised
fp_err = 0
for i in range(60):
    base = [random.uniform(-300, 300), random.uniform(-300, 300), random.uniform(0, 30), 0.0, random.uniform(-50, 50), random.uniform(-180, 180), 55.0, 1.0, 1.0, 0.1, 0.0, 0.0]
    out = (ctypes.c_float * 12)()
    lib.w2_compose(arr(base), arr([0, 0, 0, 0, 0, 0, 1]), arr((-0.9, 0.9, 0.9, -0.9)), arr((-0.9, 0.9, 0.9, -0.9)), 2, arr([0, 0, 0]), 1, 1, ctypes.c_float(1),
                   arr([0, 0, 0]), ctypes.c_float(1), 2000, 2000, 0, out, 1, ctypes.c_float(3.0), ctypes.c_float(-0.1), ctypes.c_float(0.1), ctypes.c_float(0.18))
    out = list(out)
    V, _, _, IV = build(out)
    eyepos = IV[3, :3]
    y, pt = math.radians(base[5]), math.radians(base[4])
    fwd = np.array([-math.sin(y) * math.cos(pt), math.cos(y) * math.cos(pt), math.sin(pt)]); right = np.array([math.cos(y), math.sin(y), 0])
    want = np.array(base[:3]) + fwd * 3.0 + right * -0.1 + np.array([0, 0, 0.1])
    fp_err = max(fp_err, np.abs(eyepos - want).max() / (1 + np.abs(want).max()), abs(out[9] - 0.18))
    lv = V[:3, :3]   # decoupled: the eye looks level (view z axis has no world-up component)
    fp_err = max(fp_err, abs(lv[2, 2]))
worst['fp'] = fp_err
print('first person: pivot / level view / near (max err): %.2e' % fp_err)
print('cases', N)
print('eye view vs independent derivation (max err):  %.2e' % worst['view'])
print('headset FOV corners outside the frustum (NDC): %.2e  (<= 0 + eye tag means covered)' % worst['cover'])
print('pixel shape error vs back buffer:              %.2e' % worst['proj'])
print('engine inverse proj / view - identity:          %.2e' % worst['vp'])
bad = worst.get('fp', 0) > 1e-5 or worst['view'] > 2e-3 or worst['cover'] > 0.01 or worst['proj'] > 1e-3 or worst['vp'] > 1e-4
print('RESULT:', 'FAIL' if bad else 'PASS')
sys.exit(1 if bad else 0)
