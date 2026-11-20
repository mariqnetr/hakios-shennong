#!/bin/bash
# The Xiaomi 14 Pro's ambient light sensor for iOS's auto-brightness: als/shennong_als.c, a library that goes into
# backboardd and gives CoreBrightness a light sensor fed from the kernel's kern.redwood.als_mlux.
#
#   cd ~/tmp/shennong && bash build-als.sh && bash build-gl.sh
#
# Compiled with the cross compiler build-gl.sh has set up in the shennong-ios volume (run build-gl.sh once first);
# the library goes to build/als/libshennong-als.dylib, and build-gl.sh then puts it into the image as
# /redwood/share/als/libshennong-als.dylib and has backboardd started with it. Nothing is written to the phone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$HERE/build/als"
cp "$HERE/als/shennong_als.c" "$HERE/als/shennong_motion.c" "$HERE/build/als/"
if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then RT=podman; podman machine start >/dev/null 2>&1 || true
else echo "need Docker Desktop or Podman"; exit 1; fi
$RT image inspect shennong-gl >/dev/null 2>&1 || { echo "build-als: no shennong-gl image: run bash build-gl.sh once first"; exit 1; }
$RT run --rm --platform linux/arm64 -v "$HERE/build:/work" -v shennong-ios:/ios -v shennong-kernel:/k shennong-gl bash -c '
set -e
H=/ios/hackios
SDK=/k/theos/sdks/iPhoneOS16.5.sdk
[ -x $H/xnu-deps/host/bin/xnu-clang ] || { echo "ALS-FAILED: no cross compiler (bash build-gl.sh first)"; exit 1; }
export PATH=$H/xnu-deps/host/bin:$PATH
xnu-clang -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0 -O2 -Wall -Wextra -Werror -dynamiclib \
	-install_name /redwood/share/als/libshennong-als.dylib -framework CoreFoundation -framework IOKit -lobjc \
	-o /work/als/libshennong-als.dylib /work/als/shennong_als.c /work/als/shennong_motion.c
ls -la /work/als/libshennong-als.dylib
echo ALS-BUILD-OK
' 2>&1 | tee "$HERE/build/als.log"
grep -q ALS-BUILD-OK "$HERE/build/als.log" && echo "build-als: done -> build/als/libshennong-als.dylib (now bash build-gl.sh)" \
	|| { echo "build-als: FAILED, see build/als.log"; exit 1; }
