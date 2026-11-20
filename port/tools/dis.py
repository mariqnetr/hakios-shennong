#!/usr/bin/env python3
# dis.py file start_hex length_hex  -> addressed aarch64 disassembly via llvm-mc
import glob, subprocess, sys
f, start, ln = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
b = open(f, 'rb').read()
mc = sorted(glob.glob('/usr/bin/llvm-mc*'))[0]
for o in range(start, start + ln, 4):
    w = b[o:o + 4]
    r = subprocess.run([mc, '--disassemble', '-triple=aarch64'], input=' '.join('0x%02x' % x for x in w),
                       capture_output=True, text=True).stdout
    ins = [l.strip() for l in r.splitlines() if l.strip() and not l.strip().startswith('.text')]
    print(f'{o:06x}: {w[::-1].hex()}  {ins[0] if ins else "?"}')
