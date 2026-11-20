#!/usr/bin/env python3
"""
Take one partition image out of an Android OTA zip that lies on a web server, without downloading
the zip: the zip's directory, payload.bin's header and manifest, and then only the data of that
partition's operations are fetched with HTTP range requests (a few MB for odm, of a 6 GB zip).

  ota-part.py URL PARTITION OUT.img [URL2 ...]   (more URLs: mirrors of the same file, tried in turn)

Formats: the zip (APPNOTE, with ZIP64), payload.bin (update_engine: "CrAU", version 2, manifest
DeltaArchiveManifest in protobuf; update_metadata.proto). Operations of a full OTA: REPLACE,
REPLACE_BZ, REPLACE_XZ, ZERO/DISCARD, REPLACE_ZSTD (needs the zstandard module). The image is
checked against the sha256 the manifest gives for it.
"""
import bz2, hashlib, lzma, struct, sys, urllib.request

def fetch(url, start, length):
    request = urllib.request.Request(url, headers={'Range': f'bytes={start}-{start + length - 1}', 'User-Agent': 'curl/8'})
    with urllib.request.urlopen(request, timeout=120) as answer:
        if answer.status != 206:        # the whole file would come: not read
            raise IOError(f'the server does not do range requests (status {answer.status})')
        if length < (1 << 20):
            data = answer.read()
        else:                           # a big one: say how far it has come
            parts, got = [], 0
            while True:
                piece = answer.read(1 << 20)
                if not piece:
                    break
                parts.append(piece)
                got += len(piece)
                print(f'\r  {got >> 20} of {length >> 20} MB', end='', flush=True)
            print()
            data = b''.join(parts)
    if len(data) != length:
        raise IOError(f'asked for {length} bytes at {start}, got {len(data)}')
    return data

def size_of(url):
    request = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'curl/8'})
    with urllib.request.urlopen(request, timeout=60) as answer:
        return int(answer.headers['Content-Length'])

# ---- the zip: where payload.bin's bytes are ----------------------------------------------------------
def payload_offset(url, total):
    tail = fetch(url, max(0, total - 65536 - 22), min(total, 65536 + 22))
    at = tail.rfind(b'PK\x05\x06')
    if at < 0:
        raise ValueError('no end of central directory: not a zip')
    entries, cd_size, cd_offset = struct.unpack_from('<HII', tail, at + 10)
    if cd_offset == 0xffffffff or entries == 0xffff:            # ZIP64
        loc = tail.rfind(b'PK\x06\x07', 0, at)
        eocd64, = struct.unpack_from('<Q', tail, loc + 8)
        rec = fetch(url, eocd64, 56)
        entries, cd_size, cd_offset = struct.unpack_from('<QQQ', rec, 32)
    cd = fetch(url, cd_offset, cd_size)
    p = 0
    while p < len(cd):
        if cd[p:p + 4] != b'PK\x01\x02':
            break
        method, = struct.unpack_from('<H', cd, p + 10)
        csize, usize = struct.unpack_from('<II', cd, p + 20)
        nlen, xlen, clen = struct.unpack_from('<HHH', cd, p + 28)
        local, = struct.unpack_from('<I', cd, p + 42)
        name = cd[p + 46:p + 46 + nlen].decode()
        extra = cd[p + 46 + nlen:p + 46 + nlen + xlen]
        if 0xffffffff in (csize, usize, local):                 # sizes and offset in the ZIP64 extra field
            q = 0
            while q + 4 <= len(extra):
                tag, sz = struct.unpack_from('<HH', extra, q)
                if tag == 1:
                    vals, r = [], q + 4
                    for v in (usize, csize, local):
                        if v == 0xffffffff:
                            vals.append(struct.unpack_from('<Q', extra, r)[0]); r += 8
                        else:
                            vals.append(v)
                    usize, csize, local = vals
                q += 4 + sz
        if name == 'payload.bin':
            if method != 0:
                raise ValueError('payload.bin is compressed in the zip (method %d)' % method)
            head = fetch(url, local, 30)
            n2, x2 = struct.unpack_from('<HH', head, 26)
            return local + 30 + n2 + x2, usize
        p += 46 + nlen + xlen + clen
    raise ValueError('no payload.bin in the zip')

# ---- protobuf, the little of it needed ----------------------------------------------------------------
def varint(b, p):
    v = s = 0
    while True:
        c = b[p]; p += 1
        v |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            return v, p

def fields(b):
    p = 0
    while p < len(b):
        key, p = varint(b, p)
        f, t = key >> 3, key & 7
        if t == 0:
            v, p = varint(b, p)
        elif t == 1:
            v = b[p:p + 8]; p += 8
        elif t == 2:
            n, p = varint(b, p); v = b[p:p + n]; p += n
        elif t == 5:
            v = b[p:p + 4]; p += 4
        else:
            raise ValueError('protobuf wire type %d' % t)
        yield f, v

def manifest_partition(manifest, wanted):
    block = 4096
    names = []
    for f, v in fields(manifest):
        if f == 3:
            block = v
    for f, v in fields(manifest):
        if f != 13:
            continue
        name, ops, size, digest = None, [], None, None
        for g, w in fields(v):
            if g == 1:
                name = w.decode()
            elif g == 8:
                op = {'type': 0, 'offset': 0, 'length': 0, 'dst': []}
                for h, x in fields(w):
                    if h == 1: op['type'] = x
                    elif h == 2: op['offset'] = x
                    elif h == 3: op['length'] = x
                    elif h == 6:
                        e = dict(fields(x)); op['dst'].append((e.get(1, 0), e.get(2, 0)))
                ops.append(op)
            elif g == 7:
                info = dict(fields(w)); size, digest = info.get(1), info.get(2)
        names.append(name)
        if name == wanted:
            return block, ops, size, digest
    raise ValueError('no partition %r in the payload; it has: %s' % (wanted, ' '.join(n for n in names if n)))

# ---- all of it ---------------------------------------------------------------------------------------
def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    urls, part, out = [sys.argv[1]] + sys.argv[4:], sys.argv[2], sys.argv[3]
    for url in urls:
        try:
            total = size_of(url)
            break
        except Exception as e:
            print(f'{url}: {e}')
    else:
        sys.exit('no mirror answers')
    print(f'{url}: {total} bytes')
    base, psize = payload_offset(url, total)
    head = fetch(url, base, 24)
    if head[:4] != b'CrAU':
        sys.exit('payload.bin does not start with CrAU')
    version, msize = struct.unpack_from('>QQ', head, 4)
    sigsize, = struct.unpack_from('>I', head, 20) if version >= 2 else (0,)
    hsize = 24 if version >= 2 else 20
    manifest = fetch(url, base + hsize, msize)
    data = base + hsize + msize + sigsize
    block, ops, size, digest = manifest_partition(manifest, part)
    print(f'payload.bin at {base} ({psize} bytes), version {version}; {part}: {len(ops)} operations, {size} bytes')
    print(f'fetching {part}\'s data: {(max((o["offset"] + o["length"] for o in ops), default=0) - min((o["offset"] for o in ops if o["length"]), default=0)) >> 20} MB')
    lo = min((o['offset'] for o in ops if o['length']), default=0)
    hi = max((o['offset'] + o['length'] for o in ops if o['length']), default=0)
    blob = fetch(url, data + lo, hi - lo) if hi > lo else b''
    print(f'fetched {hi - lo} bytes of its data')
    image = bytearray(size if size else sum(n for o in ops for _, n in o['dst']) * block)
    for o in ops:
        raw = blob[o['offset'] - lo:o['offset'] - lo + o['length']]
        t = o['type']
        if t == 0:
            pass
        elif t == 1:
            raw = bz2.decompress(raw)
        elif t == 8:
            raw = lzma.decompress(raw)
        elif t in (6, 7):
            raw = bytes(sum(n for _, n in o['dst']) * block)
        elif t == 14:
            import zstandard
            raw = zstandard.ZstdDecompressor().decompress(raw, max_output_size=sum(n for _, n in o['dst']) * block)
        else:
            sys.exit(f'operation type {t} is not one of a full OTA\'s')
        at = 0
        for start, n in o['dst']:
            chunk = raw[at:at + n * block]
            image[start * block:start * block + len(chunk)] = chunk
            at += n * block
    if size:
        del image[size:]
    if digest is not None:
        got = hashlib.sha256(image).digest()
        if got != digest:
            sys.exit(f'{part}: sha256 {got.hex()} is not the manifest\'s {digest.hex()}')
        print(f'{part}: sha256 {got.hex()} as the manifest says')
    open(out, 'wb').write(image)
    print(f'{out}: {len(image)} bytes')

main()
