#!/usr/bin/env python3
"""camtuning.py for HyperOS's newer Chromatix files (Parameter Parser V3.6: 0x3c-byte symbols, trigger trees):
the wide OVX9000's colour matrix (cc13, no flash, by lux index and CCT), lens shading (lsc50 at f/1.42, the
whole 4:3 grid: the kernel's mode 1, 4096x3072; CROP=1 its middle 3/4 of rows, for the 16:9 mode 9) and AWB reference lights, as libredwood-media's RWCISP02.

usage: camtuning36.py TUNED.bin OUT.isp"""
import struct, sys
from collections import OrderedDict
d = open(sys.argv[1], 'rb').read()
assert d.startswith(b'QTI Chromatix Header')
n = struct.unpack_from('<I', d, 0xa4)[0]
e = [struct.unpack_from('<3I', d, 0xa8 + 12 * i) for i in range(n)]
t, pool = e[0][1], e[1][1]
L, S = [], {}
a = t
while a + 0x3c <= pool:
    sid = struct.unpack_from('<I', d, a)[0]
    nm = d[a + 4:a + 0x24].split(b'\0')[0].decode('latin1')
    f = struct.unpack_from('<6i', d, a + 0x24)
    S[sid] = (nm, d[pool + f[3]:pool + f[3] + f[4]])
    L.append(sid)
    a += 0x3c

def walk(sid, path=()):
    v = S[sid][1]
    out = []
    for k in range(len(v) // 24):
        lo, hi, nc, cs, nl, ls = struct.unpack_from('<2f4I', v, 24 * k)
        p = path + ((lo, hi),)
        out += walk(cs, p) if nc else [(p, S[ls][1])]
    return out

def tree(g, floats):
    out = struct.pack('<I', len(g))
    for (lo, hi), r in g.items():
        out += struct.pack('<2fI', lo, hi, len(r))
        for (c0, c1), m in r:
            out += struct.pack('<2f%df' % floats, c0, c1, *m)
    return out

# the colour matrix: the first cc13 tree with the most leaves (the others: defaults of one identity matrix)
roots = [s for s in L if S[s][0] == 'mod_cc13_trigger_data']
root = max(roots, key=lambda s: (len(walk(s)), -roots.index(s)))
cc = OrderedDict()
for p, v in walk(root):
    if p[2] == (0.0, 0.0):
        cc.setdefault(p[3], []).append((p[4], struct.unpack('<9f', v[:36])))

import os
CROP = os.environ.get('CROP') == '1'


def crop(grid):
    if not CROP:
        return list(grid)
    out = []
    for r in range(13):
        y = (0.125 + 0.75 * r / 12) * 12
        y0 = int(y); y1 = min(y0 + 1, 12); w = y - y0
        out += [grid[y0 * 17 + c] * (1 - w) + grid[y1 * 17 + c] * w for c in range(17)]
    return out

ls = OrderedDict()
for p, v in walk(next(s for s in L if S[s][0] == 'mod_lsc50_trigger_data')):
    if abs(p[0][0] - 1.41) > 0.01 or p[3] != (0.0, 0.0):
        continue
    f = struct.unpack('<884f', v[:884 * 4])
    ls.setdefault(p[4], []).append((p[5], sum((crop(f[k * 221:(k + 1) * 221]) for k in range(4)), [])))

v = S[next(s for s in L if S[s][0] == 'refPtV1')][1]
f = struct.unpack('<%df' % (len(v) // 4), v)
pts = [(f[i], f[i + 1]) for i in range(6, len(f) - 1, 2) if f[i] > 0 and f[i + 1] > 0]
CCTS = (10000, 7500, 6500, 5000, 4150, 4000, 3800, 2850, 2300, 1900)
assert len(pts) == 10
out = bytearray(b'RWCISP02') + tree(cc, 9) + tree(ls, 884) + struct.pack('<I', 10)
for (rg, bg), c in zip(pts, CCTS):
    out += struct.pack('<3f', rg, bg, c)
open(sys.argv[2], 'wb').write(out)
print('%s: %d matrices, %d shadings, 10 lights, %d bytes' % (sys.argv[2], sum(map(len, cc.values())), sum(map(len, ls.values())), len(out)))
