#!/bin/bash
# Build the init of the iOS image (init-mg/init.c: build/init-var-apfs.c + mg_reset=1) in the kernel's
# container, put it at build/init-var-apfs (the old one stays as build/init-var-apfs.before-mg) for
# build-ios.sh to take.  cd ~/tmp/shennong && bash build-init.sh && bash build-ios.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker; else RT=podman; podman machine start >/dev/null 2>&1 || true; fi
$RT run --rm --platform linux/arm64 -v "$HERE/init-mg:/out" -v shennong-kernel:/k shennong-kernel bash /out/build-init.sh
[ -f "$HERE/build/init-var-apfs.before-mg" ] || cp "$HERE/build/init-var-apfs" "$HERE/build/init-var-apfs.before-mg"
cp "$HERE/init-mg/init.bin" "$HERE/build/init-var-apfs"
cp "$HERE/init-mg/init.c" "$HERE/build/init-var-apfs.c"
echo "init: build/init-var-apfs (with mg_reset=1)"
