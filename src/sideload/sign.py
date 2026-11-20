#!/usr/bin/env python3
"""
sign.py: ad-hoc code signature with entitlements for a thin arm64 Mach-O, without ldid (what `ldid -S<ents>` does).

    python3 sign.py BINARY ENTITLEMENTS.plist IDENTIFIER

The binary must already have an LC_CODE_SIGNATURE at the end of __LINKEDIT (ld64.lld -adhoc_codesign): it is
replaced. Blobs: CodeDirectory (SHA-256, 4 KiB pages, version 0x20400 with the main-binary exec segment),
an empty requirements set, and the entitlements as XML -- which is what this port's kernel reads
(libkern/amfi/miniamfi_entitlements.cpp).
"""
import hashlib, struct, sys

LC_SEGMENT_64, LC_CODE_SIGNATURE = 0x19, 0x1d
PAGE = 4096


def be(fmt, *a):
    return struct.pack('>' + fmt, *a)


def main(path, ents_path, ident):
    data = bytearray(open(path, 'rb').read())
    ents = open(ents_path, 'rb').read()
    magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', data, 0)
    assert magic == 0xfeedfacf, 'not a thin 64-bit Mach-O'
    off = 32
    linkedit = cs = text = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].rstrip(b'\0')
            if name == b'__LINKEDIT':
                linkedit = off
            elif name == b'__TEXT':
                text = off
        elif cmd == LC_CODE_SIGNATURE:
            cs = off
        off += size
    assert linkedit and cs and text, 'need __TEXT, __LINKEDIT and LC_CODE_SIGNATURE'
    dataoff, _old = struct.unpack_from('<II', data, cs + 8)
    del data[dataoff:]
    code_limit = dataoff

    reqs = be('III', 0xfade0c01, 12, 0)
    ent_blob = be('II', 0xfade7171, 8 + len(ents)) + ents
    n_special = 5
    n_code = (code_limit + PAGE - 1) // PAGE
    ident_b = ident.encode() + b'\0'
    header_len = 88  # CodeDirectory up to and including execSegFlags (version 0x20400)
    ident_off = header_len
    hash_off = ident_off + len(ident_b) + n_special * 32
    cd_len = hash_off + n_code * 32

    # the signature's final size is known before its contents: fix the load commands first, then hash
    blobs_len_guess = 12 + 3 * 8 + cd_len + len(reqs) + len(ent_blob)
    sig_size = (blobs_len_guess + 15) & ~15
    struct.pack_into('<II', data, cs + 8, dataoff, sig_size)
    le_fileoff, le_filesize = struct.unpack_from('<QQ', data, linkedit + 40)
    new_filesize = dataoff + sig_size - le_fileoff
    struct.pack_into('<Q', data, linkedit + 48, new_filesize)  # filesize
    vmsize = (new_filesize + 0x3fff) & ~0x3fff
    struct.pack_into('<Q', data, linkedit + 32, max(vmsize, struct.unpack_from('<Q', data, linkedit + 32)[0]))
    text_fileoff, text_filesize = struct.unpack_from('<QQ', data, text + 40)

    special = [b'\0' * 32] * n_special  # slots 1..5
    special[2 - 1] = hashlib.sha256(reqs).digest()
    special[5 - 1] = hashlib.sha256(ent_blob).digest()
    pages = b''.join(hashlib.sha256(bytes(data[i:min(i + PAGE, code_limit)])).digest()
                     for i in range(0, code_limit, PAGE))
    cd = be('IIIIIIIIIBBBBIIIIQQQQ',
            0xfade0c02, cd_len, 0x20400, 0x2,  # adhoc
            hash_off, ident_off, n_special, n_code, code_limit,
            32, 2, 0, 12,  # hashSize, SHA-256, platform, 4 KiB pages
            0, 0, 0, 0,  # spare2, scatterOffset, teamOffset, spare3
            0, text_fileoff, text_filesize, 1)  # codeLimit64, execSeg base/limit, CS_EXECSEG_MAIN_BINARY
    assert len(cd) == header_len, len(cd)
    cd += ident_b + b''.join(reversed(special)) + pages
    assert len(cd) == cd_len

    entries = [(0, cd), (2, reqs), (5, ent_blob)]
    index_len = 12 + 8 * len(entries)
    body, index, pos = b'', b'', index_len
    for slot, blob in entries:
        index += be('II', slot, pos)
        body += blob
        pos += len(blob)
    sb = be('III', 0xfade0cc0, pos, len(entries)) + index + body
    sb += b'\0' * (sig_size - len(sb))
    data += sb
    open(path, 'wb').write(data)
    print(f'{path}: signed as {ident}, {n_code} pages, entitlements {len(ents)} bytes')


if __name__ == '__main__':
    main(*sys.argv[1:4])
