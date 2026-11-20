#!/usr/bin/env python3
"""mksparse.py RAW OUT: RAW as an Android sparse image (4 KB blocks; zero blocks as FILL 0, so they are
written as zeros, the rest as RAW), for `fastboot flash` of an image bigger than the bootloader takes in
one download. Checked by expanding it back in memory against RAW's sha256."""
import hashlib, struct, sys
BS = 4096
raw, out = sys.argv[1], sys.argv[2]
data = open(raw, 'rb').read()
if len(data) % BS:
    data += bytes(BS - len(data) % BS)
n = len(data) // BS
zero = bytes(BS)
chunks = []                      # (kind, first, count)
i = 0
while i < n:
    z = data[i * BS:(i + 1) * BS] == zero
    j = i + 1
    while j < n and (data[j * BS:(j + 1) * BS] == zero) == z and j - i < 65536:
        j += 1
    chunks.append(('fill' if z else 'raw', i, j - i))
    i = j
with open(out, 'wb') as f:
    f.write(struct.pack('<IHHHHIIII', 0xed26ff3a, 1, 0, 28, 12, BS, n, len(chunks), 0))
    for kind, first, count in chunks:
        if kind == 'raw':
            f.write(struct.pack('<HHII', 0xCAC1, 0, count, 12 + count * BS))
            f.write(data[first * BS:(first + count) * BS])
        else:
            f.write(struct.pack('<HHIII', 0xCAC2, 0, count, 16, 0))
# expand it back and compare
s = open(out, 'rb').read()
magic, _, _, hs, cs, bs, total, nch, _ = struct.unpack_from('<IHHHHIIII', s, 0)
p, outb = hs, bytearray()
for _ in range(nch):
    t, _, cnt, tot = struct.unpack_from('<HHII', s, p)
    body = s[p + cs:p + tot]
    outb += body if t == 0xCAC1 else bytes(cnt * BS)
    p += tot
assert hashlib.sha256(outb).digest() == hashlib.sha256(data).digest() and total == n
print('%s: %d blocks, %d chunks, %d MB; expands to the same bytes as %s' % (out, n, nch, len(s) >> 20, raw))
