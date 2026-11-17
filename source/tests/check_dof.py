# The renderer's per-frame post-process mask (0x9BFFE8) run in an emulator with every effect requested by the frame
# and allowed by the settings. W2VR's effect patches (w2engine.cpp: the effect's je -> jmp, signatures read from the
# source) are applied one effect at a time: each must clear exactly its own bits (DOF: 0x20 gameplay, 0x40 cutscene,
# 0x2000 scatter; motion blur 0x800; vignette 0x80; blur 0x10; SSAO 0x400) and leave every other bit as the game had it.
import sys, struct, os, re
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools'))
from emu import mk, call
from w2 import find
from unicorn.x86_const import *
src = open(os.path.join(HERE, '..', 'src', 'w2engine.cpp')).read()
table = src[src.index('Effect g_fx[W2FX_COUNT] = {'):src.index('bool g_fxSearched')]
effects = []
for m in re.finditer(r'\{ "([^"]+)",\s+\{(.*?)\}, (\d) \}', table, re.S):
    sites = re.findall(r'\{ "([0-9A-F ?]+)", (\d+) \}', m.group(2))
    effects.append((m.group(1), [(s, int(o)) for s, o in sites]))
BITS = {'depth of field (gameplay, cutscene and scatter)': 0x2060, 'motion blur': 0x800, 'vignette': 0x80, 'fullscreen blur': 0x10, 'SSAO': 0x400, 'sharpen': 0x100}
def run(patch_sites):
    mu = mk()
    for s, o in patch_sites:
        h = find(s); assert len(h) == 1, (s, h)
        assert mu.mem_read(h[0] + o, 1) == b'\x74', 'not a je at %s +%d' % (s, o)
        mu.mem_write(h[0] + o, b'\xeb')
    FRAME, SET = 0x10080000, 0x10084000
    mu.mem_write(FRAME, b'\x01' * 0x2800)           # the frame asks for every effect (incl. cutscene DOF)
    mu.mem_write(SET, b'\x01' * 0x100)              # every User.ini Allow* on
    mu.mem_write(0x2580e28, struct.pack('<I', SET))
    call(mu, 0x9bffe8, {UC_X86_REG_ESI: FRAME})
    return mu.reg_read(UC_X86_REG_EAX)
fails = []
base = run([])
print('mask with every effect on: %05x' % base)
if len(effects) != 6: fails.append('effect table parse: %d entries' % len(effects))
for name, sites in effects:
    m = run(sites); want = BITS.get(name, -1)
    cleared = base & ~m
    print('  %-48s sites %d  mask %05x  cleared %05x' % (name, len(sites), m, cleared))
    if base & want != want: fails.append('%s: the unpatched game does not set its bits' % name)
    if cleared != want: fails.append('%s: cleared %05x, expected %05x' % (name, cleared, want))
allm = run([x for _, s in effects for x in s])
if allm != base & ~0x2DF0: fails.append('all together: %05x' % allm)
print('all six together: %05x (DOF, motion blur, vignette, blur, SSAO, sharpen off; bloom, shafts, AA, rain, fog kept)' % allm)
# the renderer's second DOF path (0x9C0311..0x9C0352): frame post-processing off (+0x2767 = 0), +0x278D set -> the
# mask is written as the constant 0x2060 (all DOF). The DOF switch patches that constant (signature from the source).
m = re.search(r'FindUnique\("(C7 83 DC 00 00 00 60 20 00 00)", &c\); if \(h\) g_dofOnlyImm = h \+ (\d+);', src)
def dof_only(patched):
    mu = mk()
    if patched:
        h = find(m.group(1)); assert len(h) == 1
        mu.mem_write(h[0] + int(m.group(2)), struct.pack('<I', 0))
    OUT, INFO = 0x10080000, 0x10084000
    mu.mem_write(OUT, b'\0' * 0x100); mu.mem_write(INFO, b'\0' * 0x2800)
    mu.mem_write(OUT + 8, struct.pack('<I', INFO)); mu.mem_write(INFO + 0x278D, b'\x01')
    mu.mem_write(OUT + 0xDC, struct.pack('<I', 0xFFFFFFFF))
    mu.reg_write(UC_X86_REG_EBX, OUT); mu.reg_write(UC_X86_REG_ESP, 0x100F0000)
    mu.emu_start(0x9C0311, 0x9C0352, timeout=2_000_000, count=100)
    return struct.unpack('<I', mu.mem_read(OUT + 0xDC, 4))[0]
if not m: fails.append('DOF-only patch not in the source')
else:
    u, pt = dof_only(False), dof_only(True)
    print('DOF-only path (post-processing off, +0x278D set): mask unpatched %05x, W2VR %05x' % (u, pt))
    if u != 0x2060: fails.append('the unpatched DOF-only path gives %05x' % u)
    if pt != 0: fails.append('DOF-only path still gives %05x' % pt)
print('RESULT:', 'FAIL ' + '; '.join(fails) if fails else 'PASS'); sys.exit(1 if fails else 0)
