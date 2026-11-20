#!/usr/bin/env python3
"""
Extract the DXE drivers from a Qualcomm uefi.img (ELF with a UEFI firmware volume), the way
UEFIReader does, without .NET: walks FV -> FFS files -> sections, unpacks LZMA-compressed
volumes, and writes each driver's PE32 / DEPEX / RAW sections plus an index in FV order and the
APRIORI list.

usage: fvextract.py uefi.img outdir
"""
import gzip, json, lzma, os, struct, sys, uuid

LZMA_GUID = uuid.UUID('EE4E5898-3914-4259-9D6E-DC7BD79403CF')
GZIP_GUID = uuid.UUID('1D301FE9-BE79-4353-91C2-D23BC959AE0C')   # Qualcomm's gzip volumes
APRIORI_DXE = uuid.UUID('FC510EE7-FFDC-11D4-BD41-0080C73C8881')
FFS_TYPES = {1: 'RAW', 2: 'FREEFORM', 3: 'SEC', 4: 'PEI_CORE', 5: 'DXE_CORE', 6: 'PEIM', 7: 'DRIVER',
             8: 'COMBINED', 9: 'APPLICATION', 0xA: 'MM', 0xB: 'FV_IMAGE', 0xF0: 'PAD'}
SEC_TYPES = {0x01: 'COMPRESSION', 0x02: 'GUID_DEFINED', 0x03: 'DISPOSABLE', 0x10: 'PE32', 0x11: 'PIC', 0x12: 'TE',
             0x13: 'DXE_DEPEX', 0x14: 'VERSION', 0x15: 'UI', 0x16: 'COMPAT16', 0x17: 'FV_IMAGE',
             0x18: 'FREEFORM_SUBTYPE_GUID', 0x19: 'RAW', 0x1B: 'PEI_DEPEX', 0x1C: 'MM_DEPEX'}

def guid(b):
    return uuid.UUID(bytes_le=bytes(b))

def sections(data, out, depth=0):
    p = 0
    while p + 4 <= len(data):
        size = data[p] | data[p + 1] << 8 | data[p + 2] << 16
        typ = data[p + 3]
        hdr = 4
        if size == 0xFFFFFF:
            size, = struct.unpack_from('<I', data, p + 4); hdr = 8
        if size < hdr:
            break
        body = data[p + hdr:p + size]
        if typ == 0x02:                      # GUID-defined: LZMA or Qualcomm gzip
            g = guid(body[:16]); off, attr = struct.unpack_from('<HH', body, 16)
            inner = data[p + off:p + size]
            if g == LZMA_GUID:
                inner = lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(inner)
            elif g == GZIP_GUID:
                inner = gzip.decompress(inner)
            sections(inner, out, depth + 1)
        elif typ == 0x17:                    # nested firmware volume
            out.setdefault('fv', []).append(body)
        else:
            out.setdefault(SEC_TYPES.get(typ, hex(typ)), []).append(body)
        p = (p + size + 3) & ~3

def walk_fv(fv, files):
    if fv[40:44] != b'_FVH':
        raise ValueError('not a firmware volume')
    fvlen, = struct.unpack_from('<Q', fv, 32)
    hlen, = struct.unpack_from('<H', fv, 48)
    ext, = struct.unpack_from('<H', fv, 52)
    p = hlen
    if ext:                                   # extended header
        esize, = struct.unpack_from('<I', fv, ext + 16)
        p = (ext + esize + 7) & ~7
    while p + 24 <= min(len(fv), fvlen):
        name = fv[p:p + 16]
        if fv[p:p + 24] == b'\xff' * 24:      # free space (a pad file may also be named all-FF)
            break
        ftype = fv[p + 18]; attr = fv[p + 19]
        size = fv[p + 20] | fv[p + 21] << 8 | fv[p + 22] << 16
        hdr = 24
        if attr & 0x01 or size == 0xFFFFFF or size == 0:   # FFS3 large file
            size, = struct.unpack_from('<Q', fv, p + 24); hdr = 32
        body = fv[p + hdr:p + size]
        g = guid(name)
        if ftype == 0xF0:
            pass
        elif ftype in (1, 2) and g == APRIORI_DXE or ftype in (1,) and False:
            files.append({'guid': str(g).upper(), 'type': FFS_TYPES.get(ftype, hex(ftype)), 'raw': body, 'secs': {}})
        else:
            secs = {}
            try:
                if ftype == 1:
                    secs['RAW'] = [body]
                else:
                    sections(body, secs)
            except Exception as e:
                secs['error'] = [str(e).encode()]
            for inner in secs.pop('fv', []):
                walk_fv(inner, files)
            files.append({'guid': str(g).upper(), 'type': FFS_TYPES.get(ftype, hex(ftype)), 'secs': secs})
        p = (p + size + 7) & ~7

def main():
    img, outdir = sys.argv[1], sys.argv[2]
    d = open(img, 'rb').read()
    fvs = []
    i = d.find(b'_FVH')
    while i >= 0:
        fvs.append(d[i - 40:]); i = d.find(b'_FVH', i + 4)
    files = []
    for fv in fvs[:1]:
        walk_fv(fv, files)
    os.makedirs(outdir, exist_ok=True)
    index = []
    for f in files:
        secs = f['secs']
        ui = secs.get('UI', [b''])[0].decode('utf-16-le').rstrip('\0') if secs.get('UI') else ''
        name = ui or f['guid']
        entry = {'guid': f['guid'], 'type': f['type'], 'name': name}
        if f['guid'] == str(APRIORI_DXE).upper():
            raw = (secs.get('RAW') or [f.get('raw', b'')])[0]
            entry['apriori'] = [str(guid(raw[k:k + 16])).upper() for k in range(0, len(raw) - 15, 16)]
        dname = os.path.join(outdir, name.replace('/', '_'))
        written = []
        for kind, ext in (('PE32', '.efi'), ('TE', '.te'), ('DXE_DEPEX', '.depex'), ('RAW', '.raw')):
            for n, blob in enumerate(secs.get(kind, [])):
                os.makedirs(dname, exist_ok=True)
                fn = os.path.join(dname, name + (f'_{n}' if n else '') + ext)
                open(fn, 'wb').write(blob); written.append(os.path.basename(fn))
        entry['files'] = written
        index.append(entry)
    json.dump(index, open(os.path.join(outdir, 'index.json'), 'w'), indent=1)
    for e in index:
        print(f"{e['type']:<12} {e['name']:<40} {e['guid']}  {' '.join(e['files'])}")

main()
