#!/bin/bash
# TrollStore on the phone, from the next `bash build-ios.sh` on (root.apfs only: seconds, no system.apfs rebuild).
#   ~/tmp/shennong/sideload/enable.sh     on
#   ~/tmp/shennong/sideload/enable.sh off off again
# What goes in: build/boot-bin/tsboot (run first at boot: makes persona 99, installs TrollStore once into
# /var/containers on the flash, re-registers its apps on every later boot) and build/trollstore/ (TrollStore.tar and
# its root helper; build-ios.sh puts that directory in /redwood/share). README.md beside this says the rest.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/../build
if [ "$1" = off ]; then
	rm -rf "$B/trollstore" "$B/boot-bin/tsboot"
	echo "TrollStore: off for the next build (what is installed on the phone's flash stays there)"
	exit 0
fi
grep -q '/work/trollstore' "$HERE/../build-ios.sh" || { echo "build-ios.sh has no TrollStore line: see README.md"; exit 1; }
mkdir -p "$B/trollstore" "$B/boot-bin"
cp "$HERE/trollstore/TrollStore.tar" "$HERE/trollstore/trollstorehelper" "$B/trollstore/"
cp "$HERE/tsboot" "$B/boot-bin/tsboot"
chmod 755 "$B/trollstore/trollstorehelper" "$B/boot-bin/tsboot"
echo "TrollStore: on for the next build -- bash build-ios.sh, then the usual run (no XL_CMDLINE changes)"
