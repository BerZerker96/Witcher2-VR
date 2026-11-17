# Many-core start-up fix (w2compat.cpp): the hook recognises the game's CPU topology routine by the bytes that follow
# its GetProcessAffinityMask call. Checked against witcher2.exe: those bytes occur exactly once, right after
# "call [IAT GetProcessAffinityMask]" in the topology routine, and they are its procMask != sysMask early exit.
# The loop the fix avoids is also emulated: with a full 32-bit mask it never ends; with the faked mask it is skipped.
import sys, os, re, struct
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(HERE, 'tools'))
from w2 import find, rd, pe
src = open(os.path.join(HERE, '..', 'src', 'w2compat.cpp')).read()
tail = bytes(int(x, 16) for x in re.search(r'kTopologyTail\[\] = \{([^}]*)\}', src).group(1).replace(' ', '').split(','))
fails = []
hits = find(' '.join('%02x' % b for b in tail))
iat = None
pe.parse_data_directories()
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports:
        if i.name == b'GetProcessAffinityMask': iat = i.address
print('caller bytes %s: %d match(es) %s; GetProcessAffinityMask IAT %s' % (tail.hex(' '), len(hits), [hex(h) for h in hits], hex(iat) if iat else None))
if len(hits) != 1: fails.append('the caller bytes are not unique')
else:
    h = hits[0]
    if rd(h - 6, 6) != b'\xff\x15' + struct.pack('<I', iat): fails.append('not right after call [GetProcessAffinityMask]')
    if not (0x930A6E <= h < 0x930B00): fails.append('not in the topology routine')
# the loop: bit = 1 << n (x86 shl masks the count to 5 bits) while bit <= sysMask
def loop_iterations(sysmask, limit=1000):
    n, bit, it = 0, 1, 0
    while bit <= sysmask and it < limit:
        it += 1; n += 1; bit = (1 << (n & 31)) & 0xFFFFFFFF
        if bit == 0: break
    return it
for cpus in (8, 16, 24, 31, 32):
    m = (1 << cpus) - 1 if cpus < 32 else 0xFFFFFFFF
    it = loop_iterations(m)
    print('  %2d logical processors: topology loop %s' % (cpus, 'NEVER ENDS (hang)' if it >= 1000 else '%d iterations' % it))
    if cpus == 32 and it < 1000: fails.append('the 32-processor hang was not reproduced')
    if cpus < 32 and it >= 1000: fails.append('%d processors should not hang' % cpus)
print('RESULT:', 'FAIL ' + '; '.join(fails) if fails else 'PASS'); sys.exit(1 if fails else 0)
