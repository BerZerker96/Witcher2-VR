# Full stereo (stereo_method = full), with the COMPILED W2VR DLL and the game's own camera builder in an emulator:
#  1. the game thread builds the main camera through the hook: it becomes the head-centred camera, and both eye
#     cameras of that tick are prepared;
#  2. the render hook (the renderer's frame function 0xB15317, here with a recording stand-in for the renderer) gets
#     a frame holding that camera at +0x20 and must draw it twice: left eye with present off, then right eye with the
#     game's present flag and the scene's frame counter undone once (as the engine's uber screenshot does);
#  3. each pass's view / projection must equal the native build of that eye's camera, and the frame camera must be
#     restored afterwards.
import sys, os, math, struct, ctypes, random
HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'check_dll_hook.py')).read().split("P = 0x10070000")[0]
exec(src)
P = 0x10070000; FR = 0x100A0000; SC = 0x100A4000; LOG = 0x100A6000
random.seed(5); worst = 0; worstS = 0; fails = []
for i in range(10):
    base = [random.uniform(-300, 300), random.uniform(-300, 300), random.uniform(0, 30), random.uniform(-5, 5), random.uniform(-40, 40), random.uniform(-180, 180), 55.0, 1.778, 1.0, 0.1, 0.0, 0.0]
    dec = i % 2
    q0 = list(qeuler(math.radians(random.uniform(-60, 60)), math.radians(random.uniform(-30, 30)), math.radians(random.uniform(-10, 10))))
    p0 = [-0.032 + random.uniform(-.2, .2), random.uniform(-.1, .1), random.uniform(-.2, .2)]; p1 = [p0[0] + 0.064, p0[1], p0[2]]
    fv = [-0.94, 0.87, 0.85, -0.96, -0.87, 0.94, 0.85, -0.96] if i % 3 else [-0.9, 0.9, 0.9, -0.9] * 2   # also symmetric eyes (tagged)
    wf(mu, P, p0 + q0 + p1 + q0); wf(mu, P + 0x1000, fv)
    mu.mem_write(C, b'\0' * 0x300)
    wf(mu, C, base[0:3] + [1]); wf(mu, C + 0x10, base[3:6]); wf(mu, C + 0x1c, (base[6], base[7], base[8], base[9], 3000.0)); wf(mu, C + 0x280, (0, 0))
    run(exp[201], (0xB067F5, C, P, P + 0x1000, 0, dec))       # W2VR_TestSetup: stereo
    hook = None
    run(exp[205], (LOG, 0xAA4167)); hook = mu.reg_read(UC_X86_REG_EAX)  # W2VR_TestFullStereo -> the render hook
    run(STUB_BUILD, (C,))                                       # game thread: the main camera build
    # the render thread: a frame holding the camera as built, present on, scene frame counter 41
    mu.mem_write(FR, b'\0' * 0x400); mu.mem_write(FR + 0x20, bytes(mu.mem_read(C, 0x290))); mu.mem_write(FR + 0x2B4, b'\x01')
    mu.mem_write(FR + 0x2B8, struct.pack('<II', 2000, 2000))     # render size (info +0x2A8 / +0x2AC)
    # the copy rule: the head-tracked camera copied to another address and rebuilt must come out unchanged
    C2 = 0x100A8000; mu.mem_write(C2, bytes(mu.mem_read(C, 0x290))); before2 = bytes(mu.mem_read(C2, 0x290))
    run(STUB_BUILD, (C2,))
    if bytes(mu.mem_read(C2, 0x40)) != before2[:0x40] or bytes(mu.mem_read(C2 + 0x280, 8)) != before2[0x280:0x288]: fails.append('a copy of the head-tracked camera was composed again')
    mu.mem_write(SC, b'\0' * 0x100); mu.mem_write(SC + 0x74, struct.pack('<i', 41)); mu.mem_write(LOG, b'\0' * 4 * 52 * 4)
    before = bytes(mu.mem_read(FR + 0x20, 0x2E0))
    run(hook, (0, FR, SC))
    logs = [rf(mu, LOG + k * 208, 52) for k in range(3)]
    if os.environ.get("DBG"): print([l[:4] for l in logs], rf(mu, FR + 0x20 + 0x1c, 4))
    if logs[2][4:20] != (0.0,) * 16: fails.append('more than two passes')
    if bytes(mu.mem_read(FR + 0x20, 0x2E0)) != before: fails.append('frame camera / screen matrix not restored')
    if mu.mem_read(FR + 0x2B4, 1) != b'\x01': fails.append('present flag not restored')
    for eye in (0, 1):
        L = logs[eye]
        if L[0] != (0.0 if eye == 0 else 1.0): fails.append('pass %d present flag %g' % (eye, L[0]))
        if L[1] != (41.0 if eye == 0 else 40.0): fails.append('pass %d scene frame counter %g' % (eye, L[1]))
        if L[2] != eye or L[3] != 1: fails.append('pass %d draws not preset to the eye' % eye)
        pp = (p0 + q0) if eye == 0 else (p1 + q0); f1 = fv[0:4] if eye == 0 else fv[4:8]; f2 = fv[4:8] if eye == 0 else fv[0:4]
        out = (ctypes.c_float * 12)()
        lib.w2_compose(arr(base), arr(pp), arr(f1), arr(f2), eye, arr([0, 0, 0]), dec, 1, ctypes.c_float(1), arr([0, 0, 0]), ctypes.c_float(1), 2000, 2000, 0, out, 0, ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0))
        V, Pm, _, _ = build(list(out))
        # the screen-to-world matrix this pass used = the game's 0xAA4167 for that eye's camera (reference frame info)
        REF = 0x100AC000; mu.mem_write(REF, b'\0' * 0x300)
        wf(mu, REF + 0x10, list(out[0:3]) + [1]); wf(mu, REF + 0x20, list(out[3:6])); wf(mu, REF + 0x2c, (out[6], out[7], out[8], out[9], 3000.0)); wf(mu, REF + 0x290, (out[10], out[11]))
        run(0xB067F5, (REF + 0x10,)); mu.mem_write(REF + 0x2A8, struct.pack('<II', 2000, 2000)); run(0xAA4167, (REF,))
        want = np.array(rf(mu, REF + 0x2B0, 16)); gotS = np.array(L[36:52])
        sw = np.abs(gotS - want).max() / (1 + np.abs(want).max())
        if not np.isfinite(sw) or sw > 1e-5 or not np.abs(want).max() > 0: fails.append('pass %d screen-to-world matrix differs (%.2e)' % (eye, sw))
        worstS = max(worstS, sw)
        got = np.array(L[4:20]).reshape(4, 4); gotP = np.array(L[20:36]).reshape(4, 4)
        worst = max(worst, np.abs(got[:3] - V[:3]).max(), np.abs(got[3] - V[3]).max() / (1 + np.abs(V[3]).max()), np.abs(gotP - Pm).max())
    # the two passes really are different eyes: separation along the head's right axis ~ 64 mm
    v0 = np.array(logs[0][4:20]).reshape(4, 4); v1 = np.array(logs[1][4:20]).reshape(4, 4)
    e0 = -v0[3, :3] @ np.linalg.inv(v0[:3, :3]); e1 = -v1[3, :3] @ np.linalg.inv(v1[:3, :3])
    sep = np.linalg.norm(e0 - e1)
    if not 0.055 < sep < 0.075: fails.append('eye separation %.3f m' % sep)
print('frames drawn for both eyes: 10, max difference pass vs native eye camera %.2e, screen-to-world matrix vs the game %.2e, last eye separation %.1f mm' % (worst, worstS, sep * 1000))
if worst > 2e-5: fails.append('eye cameras differ')
print('RESULT:', 'FAIL ' + '; '.join(sorted(set(fails))) if fails else 'PASS'); sys.exit(1 if fails else 0)
