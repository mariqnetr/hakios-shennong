#!/usr/bin/env python3
"""
The CS35L43 amplifiers' firmware (Cirrus's speaker protection, from the phone's odm/firmware) turned into a
plain list of register writes for each amplifier, which bsd/dev/shennong_speaker.c plays over I2C (amp_fw=1):

  amp-fw.py ODM_FIRMWARE_DIR REGS_DUMP_B OUT_DIR

- cs35l43-dsp1-spk-prot.wmfw: its blocks, at the HALO core's memories as Linux's cs_dsp puts them
  (cs_dsp_halo_region_to_reg): PM packed 0x3800000 + 5 x, XM packed (0x2000000 + 3 x) & ~3,
  YM packed (0x2c00000 + 3 x) & ~3, XM unpacked 0x2800000 + 4 x, YM unpacked 0x3400000 + 4 x
- {B,T}-cs35l43-dsp1-spk-prot.bin: each block at its algorithm's base plus its offset (cs_dsp_load_coeff); the
  algorithms' bases are the firmware's own table (its header in XM, read here out of Android's regmap dump of
  the running amplifier, build/android-capture/cs35l43/regs-2-0040.txt)

Output: OUT_DIR/amp-B.wr and amp-T.wr: records of [u32 register][u32 bytes][bytes, padded to 4], little-endian
header words, the bytes as they go over the bus (most significant first, as the files have them).
"""
import struct, sys, os

fwdir, dump, out = sys.argv[1], sys.argv[2], sys.argv[3]

def region(typ, x):
    if typ == 0x10: return 0x3800000 + 5 * x
    if typ == 0x11: return (0x2000000 + 3 * x) & ~3
    if typ == 0x12: return (0x2c00000 + 3 * x) & ~3
    if typ == 0x05: return 0x2800000 + 4 * x
    if typ == 0x06: return 0x3400000 + 4 * x
    return None

recs = []
b = open(os.path.join(fwdir, 'cs35l43-dsp1-spk-prot.wmfw'), 'rb').read()
off = struct.unpack('<I', b[4:8])[0]
while off + 8 <= len(b):
    w0, ln = struct.unpack('<II', b[off:off + 8])
    typ, o = w0 >> 24, w0 & 0xffffff
    data = b[off + 8:off + 8 + ln]
    reg = region(typ, o)
    if reg is not None and ln:
        recs.append((reg, data))
    off += 8 + ln

r = {int(a, 16): int(v, 16) for a, v in (l.strip().split(':') for l in open(dump))}
w = [r[0x2800000 + 4 * i] for i in range(10 + 6 * 64)]
algs = {}
for i in range(w[9]):
    a = w[10 + 6 * i:16 + 6 * i]
    algs[a[0]] = (a[2], a[4])

for side in 'BT':
    out_recs = list(recs)
    c = open(os.path.join(fwdir, '%s-cs35l43-dsp1-spk-prot.bin' % side), 'rb').read()
    off = struct.unpack('<I', c[4:8])[0]
    skipped = 0
    while off + 20 <= len(c):
        o16, t16, aid, ver, sr, ln = struct.unpack('<HHIIII', c[off:off + 20])
        data = c[off + 20:off + 20 + ln]
        off += 20 + ((ln + 3) & ~3)
        if t16 in (0x05, 0x06, 0x10, 0x11, 0x12):
            if aid not in algs:
                skipped += 1
                continue
            xm, ym = algs[aid]
            base = xm if t16 in (0x05, 0x10, 0x11) else ym
            out_recs.append((region(t16, base) + o16, data))
        elif t16 >> 8 in (0xfe, 0xff, 0xfc):
            continue
        else:
            skipped += 1
    with open(os.path.join(out, 'amp-%s.wr' % side), 'wb') as f:
        for reg, data in out_recs:
            pad = data + b'\0' * ((-len(data)) % 4)
            f.write(struct.pack('<II', reg, len(data)) + pad)
    print('%s: %d writes, %d bytes, %d blocks skipped' % (side, len(out_recs), sum(len(d) for _, d in out_recs), skipped))
