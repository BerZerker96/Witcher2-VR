# Runs the COMPILED W2VR hook (build/xinput1_3.dll: asm stub -> C++ -> the game's builder) inside an x86 emulator with
# the decrypted witcher2.exe mapped, at all four stack alignments, and compares the camera the game ends up with
# against the native build of the same math. Also checks the LOD stub (+0x270 at the game's fov, registers kept).
import sys, math, struct, ctypes, random
import os; HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools')); sys.argv = sys.argv[:1]
import numpy as np, pefile
from unicorn import *
from unicorn.x86_const import *
src = open(os.path.join(HERE, 'check_camera.py')).read().split("random.seed(7)")[0]
exec(src)                                    # mu (game mapped), build(), lib (native compose), qeuler(), arr()
DLL = os.path.join(HERE, '..', 'build', 'xinput1_3.dll')
pe = pefile.PE(DLL); IBD = pe.OPTIONAL_HEADER.ImageBase
mu.mem_map(IBD, (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF)
for s in pe.sections: mu.mem_write(IBD + s.VirtualAddress, s.get_data()[:max(s.Misc_VirtualSize, 0) or len(s.get_data())])
mu.mem_map(0x7FFE0000, 0x1000)               # KUSER_SHARED_DATA (realtime.h clock)
STUB = 0x7F000000; mu.mem_map(STUB, 0x10000)
asm = {   # tiny native implementations
 'EnterCriticalSection': b'\xc2\x04\x00', 'LeaveCriticalSection': b'\xc2\x04\x00',
 'IsBadReadPtr': b'\x31\xc0\xc2\x08\x00',
 'memcpy': b'\x56\x57\x8b\x7c\x24\x0c\x8b\x74\x24\x10\x8b\x4c\x24\x14\x89\xf8\xf3\xa4\x5f\x5e\xc3',
 # memcmp: repe cmpsb; eax = 0 if equal else (byte diff sign)
 'memcmp': b'\x56\x57\x8b\x74\x24\x0c\x8b\x7c\x24\x10\x8b\x4c\x24\x14\x31\xc0\xe3\x0c\xf3\xa6\x74\x08\x0f\xb6\x46\xff\x0f\xb6\x4f\xff\x29\xc8\x5f\x5e\xc3',
 'tan': b'\xdd\x44\x24\x04\xd9\xf2\xdd\xd8\xc3',     # fld qword [esp+4]; fptan; fstp st0; ret
}
k = 0; missing = []
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in e.imports:
        a = STUB + k * 0x40; k += 1
        nm = imp.name.decode() if imp.name else '#%d' % imp.ordinal
        mu.mem_write(a, asm.get(nm, b'\xcc'))
        mu.mem_write(imp.address, struct.pack('<I', a))
        if nm not in asm: missing.append((a, nm))
miss = dict(missing)
exp = {x.ordinal: IBD + x.address for x in pe.DIRECTORY_ENTRY_EXPORT.symbols}
import subprocess
syms = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run(['i686-w64-mingw32-nm', DLL], capture_output=True, text=True).stdout.splitlines() if len(l.split()) == 3}
STUB_BUILD, STUB_LOD = syms['_w2vr_stub_build'], syms['_w2vr_stub_lod']
def run(addr, args=(), regs=None, align=0):
    sp = 0x100f0000 - align; RET = 0x10000000; mu.mem_write(RET, b'\xf4')
    for a in reversed(args): sp -= 4; mu.mem_write(sp, struct.pack('<I', a))
    sp -= 4; mu.mem_write(sp, struct.pack('<I', RET))
    mu.reg_write(UC_X86_REG_ESP, sp)
    for r, v in (regs or {}).items(): mu.reg_write(r, v)
    try: mu.emu_start(addr, RET, timeout=10_000_000, count=5_000_000)
    except UcError as ex:
        eip = mu.reg_read(UC_X86_REG_EIP); raise SystemExit('emulation fault %s at %08x (%s)' % (ex, eip, miss.get(eip & ~0x3f, '?')))
    return sp + 4 + 4 * len(args)
def setup(cam, poses, fovs, decoupled):
    f = []
    for p in poses: f += p
    run(exp[201], (0xB067F5, cam, 0x10070000, 0x10071000, 0, decoupled))
P = 0x10070000
random.seed(11); worst = 0; n = 0
for align in (0, 4, 8, 12):
    for i in range(12):
        base = [random.uniform(-300, 300), random.uniform(-300, 300), random.uniform(0, 30), random.uniform(-5, 5), random.uniform(-40, 40), random.uniform(-180, 180), 55.0, 1.778, 1.0, 0.1, 0.0, 0.0]
        dec = i % 2
        q0 = list(qeuler(math.radians(random.uniform(-60, 60)), math.radians(random.uniform(-40, 40)), math.radians(random.uniform(-15, 15))))
        p0 = [-0.032 + random.uniform(-.2, .2), random.uniform(-.1, .1), random.uniform(-.2, .2)]
        p1 = [p0[0] + 0.064, p0[1], p0[2]]
        fv = [-0.94, 0.87, 0.85, -0.96, -0.87, 0.94, 0.85, -0.96]
        wf(mu, P, p0 + q0 + p1 + q0); wf(mu, P + 0x1000, fv)
        mu.mem_write(C, b'\0' * 0x300)
        wf(mu, C, base[0:3] + [1]); wf(mu, C + 0x10, base[3:6]); wf(mu, C + 0x1c, (base[6], base[7], base[8], base[9], 3000.0)); wf(mu, C + 0x280, (0, 0))
        run(exp[201], (0xB067F5, C, P, P + 0x1000, 0, dec))          # W2VR_TestSetup (resets the eye toggle state each time)
        esp_after = run(STUB_BUILD, (C,), align=align)
        if mu.reg_read(UC_X86_REG_ESP) != esp_after: raise SystemExit('stub did not pop its argument (ret 4)')
        got = np.array(rf(mu, C + 0x40, 16)).reshape(4, 4); gotP = np.array(rf(mu, C + 0x180, 16)).reshape(4, 4)
        snap = bytes(mu.mem_read(C, 0x300))      # the camera as the game holds it after the hooked build
        eyebuf = 0x10072000; eye = None
        run(exp[202], (eyebuf,)); eye = mu.reg_read(UC_X86_REG_EAX)
        # native: same math for that eye -> builder -> expected
        pp = (p0 + q0) if eye == 0 else (p1 + q0); f1 = fv[0:4] if eye == 0 else fv[4:8]; f2 = fv[4:8] if eye == 0 else fv[0:4]
        out = (ctypes.c_float * 12)()
        lib.w2_compose(arr(base), arr(pp), arr(f1), arr(f2), eye, arr([0, 0, 0]), dec, 1, ctypes.c_float(1), arr([0, 0, 0]), ctypes.c_float(1), 2000, 2000, 1, out, 0, ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0), ctypes.c_float(0))
        V, Pm, _, _ = build(list(out))
        worst = max(worst, np.abs(got[:3] - V[:3]).max(), np.abs(got[3] - V[3]).max() / (1 + np.abs(V[3]).max()), np.abs(gotP - Pm).max()); n += 1
        # second build without new game input (anti-compounding): the same camera again
        mu.mem_write(C, snap)
        run(STUB_BUILD, (C,), align=align)
        again = np.array(rf(mu, C + 0x40, 16)).reshape(4, 4)
        if np.abs(again - got).max() > 1e-4: raise SystemExit('head pose applied twice on a rebuild (align %d)' % align)
    # LOD stub: +0x270 from the game's own fov, general registers unchanged
    regs = {UC_X86_REG_ESI: C, UC_X86_REG_EBX: 0x11111111, UC_X86_REG_EDI: 0x22222222, UC_X86_REG_EBP: 0x100f8000, UC_X86_REG_EAX: 0x33333333}
    run(STUB_LOD, (), regs=regs, align=align)
    lod = rf(mu, C + 0x270, 1)[0]
    want = 1 / math.tan(math.radians(55.0) / 2)
    if abs(lod - want) > 1e-3 or mu.reg_read(UC_X86_REG_EBX) != 0x11111111 or mu.reg_read(UC_X86_REG_EDI) != 0x22222222 or mu.reg_read(UC_X86_REG_EAX) != 0x33333333:
        raise SystemExit('LOD stub wrong: %f (want %f) or registers changed' % (lod, want))
print('hook runs (all 4 stack alignments):', n, ' max difference compiled vs native (rotation / projection abs, translation rel) %.2e' % worst)
print('anti-compounding rebuild: OK   LOD stub: +0x270 = %.4f (game fov 55) OK' % lod)
print('RESULT:', 'PASS' if worst < 2e-5 else 'FAIL'); sys.exit(0 if worst < 2e-5 else 1)
