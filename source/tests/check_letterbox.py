# The game's viewport set-size routine (0xAAEB8E) run in an emulator with a VR-shaped frame: unpatched it letterboxes
# the picture to 16:9; with W2VR's no_letterbox bytes (from w2engine.cpp: xor ebx,ebx / xor edi,edi after ftol) the
# viewport rect is the whole frame. Also 21:9 (pillarbox) and 16:9 (no bars either way).
import sys, struct, os
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools'))
from emu import mk, call, wf, rf
from w2 import find
from unicorn.x86_const import *
def run(w, h, patched):
    mu = mk()
    top = find('E8 ?? ?? ?? ?? 8B D8 EB 30 F3 0F 10 4D 08')[0]; left = find('E8 ?? ?? ?? ?? 8B F8 8B 56 20 8B 06 8D 0C 1B')[0]
    fn = find('55 8B EC 51 8B 45 08 56 8B F1 80 7E 24 00')[0]
    # the CRT's floor / ftol dispatch through pointers the runtime fills in at start-up: x87 stand-ins
    floor_ = bytes.fromhex('dd442404' '83ec04' 'd93c24' '668b0424' '660d0004' '6625fff7' '6689442402' 'd96c2402' 'd9fc' 'd92c24' '83c404' 'c3')
    ftol_ = bytes.fromhex('83ec08' 'd93c24' '668b0424' '660d000c' '6689442402' 'd96c2402' 'db5c2404' 'd92c24' '8b442404' '83c408' 'c3')
    mu.mem_write(0x6718f0, floor_); mu.mem_write(0x673f80, ftol_)
    if patched: mu.mem_write(top + 5, b'\x33\xdb'); mu.mem_write(left + 5, b'\x33\xff')
    OBJ, VT, REC, OUT = 0x10080000, 0x10081000, 0x10082000, 0x10083000
    mu.mem_write(OBJ, b'\0' * 0x200); mu.mem_write(OBJ, struct.pack('<I', VT)); mu.mem_write(OBJ + 0x24, b'\x01')
    wf(mu, OBJ + 0x90, (16 / 9,))
    mu.mem_write(VT + 8, struct.pack('<I', REC))
    # recorder (thiscall, 4 args): copy them to OUT, ret 16
    mu.mem_write(REC, bytes.fromhex('8b442404a3' + struct.pack('<I', OUT).hex() + '8b442408a3' + struct.pack('<I', OUT + 4).hex() +
                                    '8b44240ca3' + struct.pack('<I', OUT + 8).hex() + '8b442410a3' + struct.pack('<I', OUT + 12).hex() + 'c21000'))
    call(mu, fn, {UC_X86_REG_ECX: OBJ}, stack_args=(w, h))
    return struct.unpack('<4i', mu.mem_read(OUT, 16))   # x, y, width, height
fails = []
for w, h in [(2064, 2096), (3440, 1440), (1920, 1080)]:
    a = run(w, h, False); b = run(w, h, True)
    print('%4dx%-4d  game: x %4d y %4d %4dx%-4d   W2VR: x %d y %d %dx%d' % (w, h, *a, *b))
    if b != (0, 0, w, h): fails.append('%dx%d not full frame' % (w, h))
    if (w, h) == (2064, 2096) and a[1] == 0: fails.append('expected the unpatched game to letterbox')
print('RESULT:', 'FAIL ' + '; '.join(fails) if fails else 'PASS'); sys.exit(1 if fails else 0)
