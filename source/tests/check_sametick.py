# Runs the compiled same-tick stubs (build/xinput1_3.dll) in an emulator:
#  * main-loop stub: frame pairs give dt = real + banked (first eye) / 1e-5 (second eye), game time stays exact,
#    [esi+0x10] and xmm1 get the new dt, the loop's comisd flags and xmm0, xmm2..7 survive, esp is unchanged
#  * engine-clock stub: real time normally, the held time during the second eye - but real for the loop's own read
import sys, struct
sys.argv = sys.argv[:1]
import os; HERE = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(HERE, 'check_dll_hook.py')).read().split("P = 0x10070000")[0]
exec(src)    # mu with the game + the DLL mapped, exp (exports), syms, run()
from unicorn.x86_const import *
CODE = 0x10060000   # inside the scratch area emu.py maps
ORIGCLK = CODE + 0x000; mu.mem_write(ORIGCLK, b'\xd9\xe8\xc3')                       # "real clock": fld1; ret (1.0)
CALLER = CODE + 0x100                                                                # call stub_clock; fstp [OUT]; hlt
OUT = CODE + 0x800
def caller_at(addr):
    rel = (syms['_w2vr_stub_clock'] - (addr + 5)) & 0xffffffff
    mu.mem_write(addr, b'\xe8' + struct.pack('<I', rel) + b'\xdd\x1d' + struct.pack('<I', OUT) + b'\xf4')
caller_at(CALLER); LOOPREAD = CODE + 0x200; caller_at(LOOPREAD)
BACK = CODE + 0x300   # where the loop continues: jbe -> eax=2 else eax=1
mu.mem_write(BACK, b'\x76\x06\xb8\x01\x00\x00\x00\xf4\xb8\x02\x00\x00\x00\xf4')
run(exp[203], (ORIGCLK, LOOPREAD + 5, BACK))
def clock(at):
    mu.reg_write(UC_X86_REG_ESP, 0x100f0000); mu.emu_start(at, 0, count=100)
    return struct.unpack('<d', mu.mem_read(OUT, 8))[0]
G = syms['_g_stHold']; HELD = syms['_g_stHeld']
E = 0x10050000; S = 0x10052000; mu.mem_map(E - 0x1000, 0x4000) if False else None
fails = []
def loop_frame(real_dt, t1, cf):
    mu.mem_write(E, b'\0' * 0x40); mu.mem_write(E + 0x0C, struct.pack('<f', real_dt)); mu.mem_write(E + 0x18, struct.pack('<d', t1))
    mu.mem_write(S, b'\0' * 0x20)
    for k in range(8): mu.reg_write(UC_X86_REG_XMM0 + k, int.from_bytes(struct.pack('<4f', 100 + k, 200 + k, 300 + k, 400 + k), 'little'))
    mu.reg_write(UC_X86_REG_ESI, S); mu.reg_write(UC_X86_REG_ESP, E)
    mu.reg_write(UC_X86_REG_EFLAGS, 0x202 | (1 if cf else 0))   # CF set -> jbe taken
    mu.emu_start(syms['_w2vr_stub_loop'], 0, count=2_000_000)
    if mu.reg_read(UC_X86_REG_ESP) != E: fails.append('esp changed')
    if mu.reg_read(UC_X86_REG_EAX) != (2 if cf else 1): fails.append('flags not preserved')
    for k in (0, 2, 3, 4, 5, 6, 7):
        v = struct.unpack('<4f', mu.reg_read(UC_X86_REG_XMM0 + k).to_bytes(16, 'little'))
        if v[0] != 100 + k or v[3] != 400 + k: fails.append('xmm%d clobbered' % k)
    x1 = struct.unpack('<f', (mu.reg_read(UC_X86_REG_XMM1) & 0xffffffff).to_bytes(4, 'little'))[0]
    st = struct.unpack('<f', mu.mem_read(S + 0x10, 4))[0]
    if abs(x1 - st) > 1e-9: fails.append('xmm1 / [esi+0x10] differ')
    return st
real = [0.011, 0.012, 0.010, 0.013, 0.011, 0.012, 0.010]   # ends on a first-eye frame (pays out the last bank)
t = 100.0; given = []; holds = []; clocks = []
for i, r in enumerate(real):
    t += r
    d = loop_frame(r, t, i % 2 == 0)
    given.append(d)
    holds.append(mu.mem_read(G, 1)[0])
    clocks.append((clock(CALLER), clock(LOOPREAD)))
print('frames real dt  ', real)
print('frames given dt ', [round(x, 6) for x in given])
print('held for 2nd eye', holds, ' clock other/loop read', [(round(a, 4), round(b, 4)) for a, b in clocks])
if abs(sum(given) - sum(real)) > 1e-5: fails.append('game time not conserved: %.6f vs %.6f' % (sum(given), sum(real)))
if any(given[i] > 1e-4 for i in (1, 3, 5)): fails.append('second eye moved the world')
if holds != [0, 1, 0, 1, 0, 1, 0]: fails.append('hold pattern %s' % holds)
for i, (a, b) in enumerate(clocks):
    if i % 2 == 1:   # held: other callers get the first eye's time, the loop's own read stays real
        want = 100.0 + sum(real[:i]) + 1e-5
        if abs(a - want) > 1e-4 or abs(b - 1.0) > 1e-9: fails.append('held clock wrong in frame %d (%f, %f)' % (i, a, b))
    elif abs(a - 1.0) > 1e-9: fails.append('clock held outside the second eye (frame %d)' % i)
print('RESULT:', 'FAIL ' + '; '.join(fails) if fails else 'PASS'); sys.exit(1 if fails else 0)
