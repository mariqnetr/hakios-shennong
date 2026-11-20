#!/usr/bin/env python3
"""Minimal FDT -> dts-like text (no dtc needed). usage: fdtdump.py file.dtb > out.dts"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, total, off_struct, off_str, off_rsv, ver, lcv, bcpu, sz_str, sz_struct = struct.unpack('>10I', d[:40])
assert magic == 0xd00dfeed, hex(magic)
def s(o):
    e = d.index(b'\0', off_str + o); return d[off_str + o:e].decode()
def fmt(name, v):
    if not v: return ''
    if v[-1:] == b'\0' and all(32 <= c < 127 or c == 0 for c in v) and b'\0\0' not in v[:-1] and v[0] != 0:
        return ' = ' + ', '.join('"%s"' % x.decode() for x in v[:-1].split(b'\0'))
    if len(v) % 4 == 0:
        return ' = <' + ' '.join('0x%x' % x for x in struct.unpack('>%dI' % (len(v)//4), v)) + '>'
    return ' = [' + ' '.join('%02x' % c for c in v) + ']'
out = []; p = off_struct; depth = 0
print('/memreserve/:')
r = off_rsv
while True:
    a, l = struct.unpack('>QQ', d[r:r+16]); r += 16
    if not a and not l: break
    print('/memreserve/ 0x%x 0x%x;' % (a, l))
while True:
    tok, = struct.unpack('>I', d[p:p+4]); p += 4
    if tok == 1:
        e = d.index(b'\0', p); n = d[p:e].decode() or '/'; p = (e + 4) & ~3
        print('\t' * depth + n + ' {'); depth += 1
    elif tok == 2:
        depth -= 1; print('\t' * depth + '};')
    elif tok == 3:
        ln, no = struct.unpack('>II', d[p:p+8]); p += 8
        v = d[p:p+ln]; p = (p + ln + 3) & ~3
        print('\t' * depth + s(no) + fmt(s(no), v) + ';')
    elif tok == 4: continue
    elif tok == 9: break
