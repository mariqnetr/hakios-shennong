#!/usr/bin/env python3
# strrefs.py file start_hex end_hex -> strings referenced (adrp+add) by code in [start, end)
import struct, sys
f, s, e = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
b = open(f, 'rb').read()
def cstr(o):
    if o >= len(b): return None
    t = b[o:b.find(b'\0', o)]
    return t.decode() if t and all(32 <= c < 127 or c in (9, 10, 13) for c in t) and len(t) > 3 else None
for off in range(s, e, 4):
    w = struct.unpack_from('<I', b, off)[0]
    if (w & 0x9F000000) == 0x90000000:
        rd = w & 31; imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        if imm & (1 << 20): imm -= 1 << 21
        page = (off & ~0xFFF) + (imm << 12)
        for k in range(1, 6):
            w2 = struct.unpack_from('<I', b, off + 4 * k)[0]
            if (w2 & 0xFFC00000) == 0x91000000 and ((w2 >> 5) & 31) == rd:
                t = cstr(page + ((w2 >> 10) & 0xFFF))
                if t: print(f'{off:06x}: {t[:100]!r}')
                break
