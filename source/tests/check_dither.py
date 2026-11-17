# The game's screen-door dither textures (w2engine.cpp PatchDitherTextures): runs the exe's own D3DXFillTexture
# callbacks in the emulator for all 16 x 16 texels and checks what the mod relies on:
#  - 0x53EF90 is a 16 x 16 ordered (Bayer) dither matrix: 256 distinct thresholds, spread so every 2 x 2 block covers
#    the range (a screen-door pattern), and one texel sits at ~0.5 (the uniform value the mod uses);
#  - 0x53F140 is a per-texel rotation ((cos + 1) / 2, (sin + 1) / 2), varying between neighbours (interleaved PCF);
#  - the mod's signatures find each callback once and each has exactly one "push imm32" (the patched site).
import sys, os, re, struct, math
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools'))
from emu import mk, call, wf, rf
from w2 import IB, pe
from unicorn.x86_const import *

src = open(os.path.join(HERE, '..', 'src', 'w2engine.cpp')).read()
sigs = re.findall(r'FindUnique\("(55 8B EC 83 EC 3[04][^"]*)"', src)
assert len(sigs) == 2, sigs
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
mem = pe.get_memory_mapped_image()
lo, hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
def find(sig):
    pat = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in sig.split()), re.S)
    hits = [IB + lo + m.start() for m in pat.finditer(mem[lo:hi])]
    return hits
fails = 0
addr = {}
for name, sig in zip(('bayer', 'rotation'), sigs):
    h = find(sig)
    pushes = mem[lo:hi].count(b'\x68' + struct.pack('<I', h[0])) if len(h) == 1 else -1
    print(f'{name}: signature matches {len(h)} ({", ".join(hex(x) for x in h)}), push sites {pushes}')
    if len(h) != 1 or pushes != 1: fails += 1
    else: addr[name] = h[0]

mu = mk()
OUT, TC, TS = 0x10010000, 0x10010100, 0x10010200
def texel(fn, x, y):
    wf(mu, TC, [(x + 0.5) / 16, (y + 0.5) / 16]); wf(mu, TS, [1 / 16, 1 / 16]); wf(mu, OUT, [-9, -9, -9, -9])
    call(mu, fn, {}, (OUT, TC, TS, 0))
    return rf(mu, OUT, 4)

if 'bayer' in addr:
    B = [[texel(addr['bayer'], x, y) for x in range(16)] for y in range(16)]
    v = [[B[y][x][0] for x in range(16)] for y in range(16)]
    flat = [round(a * 255) for r in v for a in r]
    same = all(B[y][x][0] == B[y][x][c] for y in range(16) for x in range(16) for c in range(4))
    distinct = len(set(flat))
    spread = min(max(v[y][x], v[y][x+1], v[y+1][x], v[y+1][x+1]) - min(v[y][x], v[y][x+1], v[y+1][x], v[y+1][x+1])
                 for y in range(0, 16, 2) for x in range(0, 16, 2))
    best = min((abs(a - 0.5), a) for r in v for a in r)[1]
    print(f'bayer: values {min(flat)}..{max(flat)} / 255, distinct {distinct}, all channels equal {same}, '
          f'min range inside each 2x2 block {spread:.3f}, texel nearest 0.5 = {best:.4f}')
    print('  first rows:', [flat[i] for i in range(8)], [flat[16 + i] for i in range(8)])
    if distinct < 250 or not same or spread < 0.45 or abs(best - 0.5) > 0.01: fails += 1
    # remove_dither = fine: every texel takes the value of texel (x & 1, y & 1). The 0.5 split (which pixels of
    # each 2 x 2 block are above / below half - what the colour decode and 50 % blends see) must stay identical.
    fine = [[v[y & 1][x & 1] for x in range(16)] for y in range(16)]
    keep = all((v[y][x] >= 0.5) == (fine[y][x] >= 0.5) for y in range(16) for x in range(16))
    lvl = sorted(round(fine[y][x] * 255) for y in range(2) for x in range(2))
    print(f'fine 2x2: levels {lvl} / 255, 0.5 split identical to the original for all 256 texels: {keep}')
    if not keep or len(set(lvl)) != 4: fails += 1
if 'rotation' in addr:
    R = [[texel(addr['rotation'], x, y) for x in range(16)] for y in range(16)]
    ang = [math.atan2(R[y][x][1] * 2 - 1, R[y][x][0] * 2 - 1) for y in range(16) for x in range(16)]
    unit = max(abs((R[y][x][0] * 2 - 1) ** 2 + (R[y][x][1] * 2 - 1) ** 2 - 1) for y in range(16) for x in range(16))
    nb = sum(abs(R[0][x][0] - R[0][x + 1][0]) for x in range(15)) / 15
    print(f'rotation: {len(set(round(a, 3) for a in ang))} distinct angles, |(2r-1, 2g-1)| = 1 within {unit:.4f}, '
          f'mean neighbour change {nb:.3f}, texel (0,0) = {[round(c, 4) for c in R[0][0]]}')
    if unit > 0.01 or nb < 0.05: fails += 1
print('RESULT:', 'PASS' if not fails else f'FAIL ({fails})')
sys.exit(1 if fails else 0)
