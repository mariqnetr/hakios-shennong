#!/usr/bin/env python3
"""keep-big.py select-system.py: the file-provider and document-manager extensions' executables go into the
system image, whatever their size. They are 5-9 MB, over the 4 MB limit for a file under the framework
directories, so their bundles were there without the programs: no "On My iPhone" in Files (the
com.apple.FileProvider.LocalStorage extension could not start), no "Save to Files", no document picker.
Idempotent: called on every build-ios.sh (a change makes fetch.sh choose and extract again)."""
import sys
p = sys.argv[1]
s = open(p).read()
if 'KEEP_BIG' in s:
    sys.exit(0)
D = 'System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/'
const = "KEEP_BIG = ('System/Library/Frameworks/FileProvider.framework/PlugIns/LocalStorageFileProvider.appex/LocalStorageFileProvider',\n"
for n in ('com.apple.DocumentManager.Service', 'SaveToFiles', 'RecentsAvocado', 'RecentsAppPopover'):
    const += f"            '{D}{n}.appex/{n}',\n"
const = const.rstrip(',\n') + ")\n"
a = "SMALL = 4 << 20\n"
b = "    if path.startswith(FRAMEWORKS):\n"
assert s.count(a) == 1 and s.count(b) == 1
s = s.replace(a, a + const).replace(b, b + "        if path in KEEP_BIG:\n            return True\n")
open(p, 'w').write(s)
print('keep-big: select-system.py changed: the system image gets the file extensions\' programs')
