#!/usr/bin/env python3
"""
Put another payload (a new loader.efi, say) into a boot image that mkxnuimg.py made, keeping its UEFI,
boot shim and device tree as they are -- for a device whose UEFI is built somewhere else (shennong's,
in an x86 container) when only the loader has changed.

  kernel = gzip(BootShim + UEFI FD + "XNUP" container) + DTB      (see mkxnuimg.py)

The image is an Android boot image of header version 3 or 4 (pages of 4096 bytes: header, kernel,
ramdisk, and for version 4 the boot signature). Only the kernel changes; everything else is copied.

usage: repack-xnuimg.py base.img payload.bin out.img
"""
import gzip, struct, sys, zlib

PAGE = 4096


def pages(n):
    return (n + PAGE - 1) // PAGE * PAGE


base, payload_path, out = sys.argv[1:4]
img = open(base, 'rb').read()
if img[:8] != b'ANDROID!':
    sys.exit(f'{base}: not an Android boot image')
kernel_size, ramdisk_size = struct.unpack_from('<II', img, 8)
version, = struct.unpack_from('<I', img, 40)
if version not in (3, 4):
    sys.exit(f'{base}: boot image header version {version}; only 3 and 4 are handled')
signature_size = struct.unpack_from('<I', img, 1580)[0] if version == 4 else 0

kernel = img[PAGE:PAGE + kernel_size]
z = zlib.decompressobj(16 + zlib.MAX_WBITS)
blob = z.decompress(kernel)
dtb = z.unused_data
if dtb[:4] != b'\xd0\x0d\xfe\xed':
    sys.exit('no device tree after the compressed kernel')

# the payload is the "XNUP" container that ends the blob (padded to 16 bytes with zeros); the FD before
# it may hold the same four letters (the UEFI's XnuLoader looks for them), so the one that fits is taken
where = None
at = blob.find(b'XNUP')
while at >= 0:
    size, = struct.unpack_from('<I', blob, at + 4)
    end = at + 8 + size
    if end <= len(blob) and len(blob) - end < 16 and not any(blob[end:]):
        where = at
    at = blob.find(b'XNUP', at + 1)
if where is None:
    sys.exit(f'{base}: no payload at the end of its kernel')

payload = open(payload_path, 'rb').read()
if payload[:4] != b'XNUP':
    sys.exit(f'{payload_path}: not an XNUP container (xnuloader/mkpayload.py)')
new_blob = blob[:where] + payload
new_blob += b'\0' * (-len(new_blob) % 16)
new_kernel = gzip.compress(new_blob, 6, mtime=0) + dtb

header = bytearray(img[:PAGE])
struct.pack_into('<I', header, 8, len(new_kernel))
rest_at = PAGE + pages(kernel_size)                     # ramdisk and signature, copied as they are
rest = img[rest_at:rest_at + pages(ramdisk_size) + pages(signature_size)]
data = bytes(header) + new_kernel + b'\0' * (pages(len(new_kernel)) - len(new_kernel)) + rest
open(out, 'wb').write(data)
print(f'{out}: {len(data)} bytes; payload {len(blob) - where} -> {len(payload)} bytes '
      f'(UEFI and shim {where} bytes, device tree {len(dtb)} bytes kept)')
