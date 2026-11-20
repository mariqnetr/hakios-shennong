#!/usr/bin/env python3
"""
Lay out drivers extracted by fvextract.py as a Mu-Silicium Binaries/<device> tree (the layout UEFIReader
writes): <Pkg>/<path>/<Module>.{efi,depex,inf} and RawFiles/<name>. Module paths come from each
driver's debug path. A reference tree of the same SoC (giulia) supplies .inf templates where the
module exists there; FILE_GUID always comes from this device's own volume.

Also rewrites DXE.inc / APRIORI.inc / RAW.inc of the device package: entries of the reference
lists are kept in order when this device has the module, OEM modules of the reference device are
dropped, and this device's own extra modules are added (commented out -- OEM drivers are not
needed to boot and are left for the porter to decide).

usage: mkbinaries.py extracted_dir ref_binaries_dir out_binaries_dir device ref_device pkg_include_dir
"""
import json, os, re, shutil, struct, sys

ext, ref, out, dev, refdev, inc = sys.argv[1:7]
index = json.load(open(os.path.join(ext, 'index.json')))
os.makedirs(out, exist_ok=True)

def pe_subsystem(b):
    pe, = struct.unpack_from('<I', b, 0x3C)
    return struct.unpack_from('<H', b, pe + 24 + 68)[0]

MODTYPE = {10: 'UEFI_APPLICATION', 11: 'DXE_DRIVER', 12: 'DXE_RUNTIME_DRIVER'}
mods = {}            # module name -> (relative dir, ui name, guid)
raws = {}            # ui name -> file
for e in index:
    files = e['files']; ui = e['name']; src = os.path.join(ext, ui)
    efi = [f for f in files if f.endswith('.efi')]
    if efi:
        b = open(os.path.join(src, efi[0]), 'rb').read()
        # CodeView debug path: RSDS (PDB 7) or NB10 (PDB 2, what Qualcomm's clang builds carry)
        m = re.search(rb'RSDS.{20}([ -~]+?)\x00', b, re.S) or re.search(rb'NB10.{12}([ -~]+?)\x00', b, re.S)
        pm = re.search(r'/AARCH64/(.+)/([^/]+)/DEBUG/[^/]+\.dll$', m.group(1).decode()) if m else None
        if not pm:
            print('skip (no debug path):', ui); continue
        rel, mod = pm.group(1), pm.group(2)
        d = os.path.join(out, rel); os.makedirs(d, exist_ok=True)
        shutil.copy(os.path.join(src, efi[0]), os.path.join(d, mod + '.efi'))
        dep = [f for f in files if f.endswith('.depex')]
        if dep:
            shutil.copy(os.path.join(src, dep[0]), os.path.join(d, mod + '.depex'))
        refinf = os.path.join(ref, rel, mod + '.inf')
        if os.path.exists(refinf):
            t = open(refinf).read()
            t = re.sub(r'(FILE_GUID\s*=\s*)\S+', lambda m: m.group(1) + e['guid'], t)
            # the reference may use Silicium-patched binaries (X.patched.efi); this device's are unpatched
            t = re.sub(r'\|(\w+)\.patched\.efi\|', r'|\1.efi|', t)
            if not dep:
                t = re.sub(r'.*DXE_DEPEX\|.*\n', '', t)
        else:
            mt = MODTYPE.get(pe_subsystem(b), 'DXE_DRIVER')
            t = (f"[Defines]\n  INF_VERSION                    = 0x0001001B\n  BASE_NAME                      = {ui}\n"
                 f"  FILE_GUID                      = {e['guid']}\n  MODULE_TYPE                    = {mt}\n"
                 f"  VERSION_STRING                 = 1.0\n  ENTRY_POINT                    = EfiEntry\n\n"
                 f"[Binaries.AARCH64]\n" + (f"  DXE_DEPEX|{mod}.depex|*\n" if dep else '') +
                 f"  PE32|{mod}.efi|*\n\n[Depex]\n  TRUE\n")
        open(os.path.join(d, mod + '.inf'), 'w').write(t)
        mods[mod] = (rel, ui, e['guid'])
    else:
        raw = [f for f in files if f.endswith('.raw')]
        if raw and e['type'] == 'FREEFORM' and not re.fullmatch(r'[0-9A-F-]{36}', ui):
            os.makedirs(os.path.join(out, 'RawFiles'), exist_ok=True)
            shutil.copy(os.path.join(src, raw[0]), os.path.join(out, 'RawFiles', ui))
            raws[ui] = raw[0]

# FV order of the drivers (the order DXE.inc should follow) and the APRIORI list
order = [e for e in index if e['files'] and any(f.endswith('.efi') for f in e['files'])]
apriori = next((e.get('apriori') for e in index if e.get('apriori')), [])
by_guid = {g: m for m, (r, u, g) in mods.items()}

def fix_list(path):
    """Keep reference entries this device has, drop the rest, append this device's extras."""
    lines = open(path).read().split('\n')
    kept, dropped, seen = [], [], set()
    for l in lines:
        m = re.match(r'(\s*)INF (?:RuleOverride = \S+ )?Binaries/[^/]+/(.+)/([^/]+)\.inf\s*$', l)
        if m:
            rel, mod = m.group(2), m.group(3)
            if mod in mods and mods[mod][0] == rel:
                kept.append(l); seen.add(mod)
            else:
                dropped.append(mod)
            continue
        kept.append(l)
    return kept, dropped, seen

report = {'modules': len(mods), 'raw_files': sorted(raws), 'lists': {}}
for name in ('DXE.inc', 'APRIORI.inc'):
    p = os.path.join(inc, name)
    kept, dropped, seen = fix_list(p)
    extra = [m for m in mods if m not in seen]
    if name == 'DXE.inc' and extra:
        kept.append(f'\n  # {dev} OEM / extra modules not in {refdev}\'s list -- not needed to boot, left out:')
        for m in extra:
            kept.append(f'  #INF Binaries/{dev}/{mods[m][0]}/{m}.inf')
    open(p, 'w').write('\n'.join(kept))
    report['lists'][name] = {'dropped': dropped, 'extras': extra if name == 'DXE.inc' else []}

# RAW.inc: keep entries whose raw file exists
p = os.path.join(inc, 'RAW.inc')
t = open(p).read()
blocks = re.split(r'(?=  FILE FREEFORM)', t)
out_blocks, raw_dropped = [], []
for b in blocks:
    m = re.search(r'RawFiles/([^\s]+)', b)
    if m and m.group(1) not in raws:
        raw_dropped.append(m.group(1))
        # keep surrounding !if/!endif lines that may live in this block
        keep = [l for l in b.split('\n') if l.strip().startswith('!')]
        out_blocks.append('\n'.join(keep) + ('\n' if keep else ''))
    else:
        out_blocks.append(b)
open(p, 'w').write(''.join(out_blocks))
report['raw_inc_dropped'] = raw_dropped
json.dump(report, open(os.path.join(out, '..', f'{dev}-binaries-report.json'), 'w'), indent=1)
print(json.dumps({k: v for k, v in report.items() if k != 'apriori_device'}, indent=1))
