#!/bin/bash
# The Metal driver (hakios-metal, ~/tmp/hakios-metal, branch shennong-a750) for the phone, built on a Mac with Podman
# (or Docker), in the container build-gl.sh made (shennong-gl) and against the volumes it reads:
#
#   cd ~/tmp/shennong && bash build-metal.sh            # the probe, stages 1 to 8 (no draws yet on the A750)
#   PROBE_UNTIL=1000 bash build-metal.sh                # every stage
#
# What it does, inside:
#   1. hakios's AIR front end (hakios-metal/window-server/tools/mesa/air, newer than the hackios tree's
#      tools/mesa/air) compiled into its own libair.a against the iOS Mesa 25.0.7 build that build-gl.sh made
#      (shennong-ios: xnu-deps/build/mesa) -- ir3, NIR and freedreno's layout come from that build as they are
#   2. libhakios-metal.a (the Makefile's rule, MESA=1) and hakios-metal-probe for iOS 16.5, the shader compiler in it
#   3. the probe goes to build/boot-bin/, so that the next build-gl.sh (or build-ios.sh) puts it in /redwood/bin and
#      the image runs it first at boot; then `./xl ramdisk` and `./xl run` as usual, and its "probe:" lines are in the log
#
# Nothing in the volumes is changed: the front end, the driver and the probe are built in the container's /tmp.
# The log is build/metal.log. Run build-gl.sh once before (it makes the image and Mesa).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HM=${HM:-$HOME/tmp/hakios-metal}
[ -f "$HM/Makefile" ] || { echo "no hakios-metal at $HM (HM=)"; exit 1; }
mkdir -p "$HERE/build/boot-bin" "$HERE/build/metal"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then
	RT=podman
	podman machine start >/dev/null 2>&1 || true
else
	echo "need Docker Desktop or Podman"; exit 1
fi
$RT image inspect shennong-gl >/dev/null 2>&1 || { echo "build-metal: no shennong-gl image: run bash build-gl.sh once first"; exit 1; }

cat > "$HERE/build/inner-metal.sh" <<'INNER'
#!/bin/bash
set -e -o pipefail
T0=$SECONDS; step() { echo "TIME: $1 -- $((SECONDS - T0)) s"; }
export THEOS=/k/theos
H=/ios/hackios
M=$H/xnu-deps/build/mesa
SRC=$M/mesa-25.0.7
B=$M/build-softpipe-freedreno
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
[ -d "$SDK/usr/include" ] || { echo "METAL-FAILED: no iOS SDK in shennong-kernel (bash build-kernel.sh)"; exit 1; }
[ -f "$B/src/freedreno/ir3/libfreedreno_ir3.a" ] || { echo "METAL-FAILED: no iOS Mesa build in shennong-ios (bash build-gl.sh)"; exit 1; }
[ -x "$H/xnu-deps/host/bin/xnu-clang" ] || { echo "METAL-FAILED: no cross compiler in shennong-ios (bash build-gl.sh)"; exit 1; }
export PATH=$PATH:/usr/lib/llvm-20/bin
CC="$H/xnu-deps/host/bin/xnu-clang -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0"
IOSCXX="$H/xnu-deps/host/bin/xnu-clang++ -arch arm64 -isysroot $SDK -miphoneos-version-min=16.0"
# Mesa's -D set for this build, off the ir3.c.o line (as hakios-metal's tools/compiler.mk reads it)
DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' $B/build.ninja | grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
[ -n "$DEFS" ] || { echo "METAL-FAILED: no -D set in $B/build.ninja"; exit 1; }

rm -rf /tmp/hm /tmp/air && cp -a /hm /tmp/hm && mkdir -p /tmp/air/obj
cp /hm/window-server/tools/mesa/air/*.c /hm/window-server/tools/mesa/air/*.h /tmp/air/
INC="-I/tmp/air -I$SRC/src -I$SRC/include -I$SRC/src/compiler -I$SRC/src/compiler/nir -I$SRC/src/gallium/include \
	-I$B/src -I$B/src/compiler -I$B/src/compiler/nir"
# 1. the front end: libair's sources as its meson.build lists them
for f in air_bitcode air_layout air_metallib air_module air_print air_stitch air_to_nir air_intrinsics; do
	$CC -std=c11 -O2 -fvisibility=hidden -Wno-override-init $DEFS $INC -c /tmp/air/$f.c -o /tmp/air/obj/$f.o \
		|| { echo "METAL-FAILED: the AIR front end: $f.c"; exit 1; }
done
rm -f /tmp/air/libair.a && llvm-ar --format=darwin rcs /tmp/air/libair.a /tmp/air/obj/*.o
echo "metal: libair.a from hakios's front end, $(ls /tmp/air/obj | wc -l) objects"
step "front end"

# 2. the driver and the probe. The front end's headers first (hakios's nir_air.h, not the volume's older copy in
# $SRC/src/compiler/air), tools/compiler.mk's include list otherwise; libair.a is the one just made.
cd /tmp/hm
MESAINC="-isystem /tmp/air -isystem $SRC/src -isystem $SRC/include -isystem $SRC/src/freedreno -isystem $SRC/src/freedreno/common \
	-isystem $SRC/src/compiler -isystem $SRC/src/compiler/nir -isystem $SRC/src/gallium/include \
	-isystem $B/src -isystem $B/src/compiler -isystem $B/src/compiler/nir -isystem $B/src/freedreno \
	-isystem $B/src/freedreno/common -isystem $B/src/freedreno/isa"
ARCH="src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a src/freedreno/isa/libir3decode.a \
	src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a src/freedreno/fdl/libfreedreno_layout.a \
	src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a"
LIBS="/tmp/air/libair.a $(for a in $ARCH; do printf '%s ' $B/$a; done) -lz -lm"
FLAGS="-DPROBE_UNTIL=${PROBE_UNTIL:-8} -Wno-missing-field-initializers"
make out/ios/libhakios-metal.a MESA=1 HAKIOS=$H SDK=$SDK IOS_CXX="$IOSCXX" \
	IOS_AR="llvm-ar --format=darwin rcs" IOS_MESA_BUILD=$B IOS_MESA_SRC=$SRC IOS_LIBAIR=/tmp/air/libair.a \
	HAKIOS_METAL_IOS_MESA_CXXFLAGS="$DEFS $MESAINC" HAKIOS_METAL_IOS_MESA_LIBS="$LIBS" \
	IOS_CXXFLAGS_EXTRA="$FLAGS" 2>&1 | grep -v '^\s*/ios\|^cd out' | tail -30
[ -f out/ios/libhakios-metal.a ] || { echo "METAL-FAILED: no libhakios-metal.a"; exit 1; }
# the probe as the Makefile links it (its rule would remake the bitcode fixtures, which need a host Mesa: they are in git)
$IOSCXX -std=c++17 -O2 -Wall -Wextra -Werror $FLAGS -DHAKIOS_METAL_MESA $DEFS $MESAINC -Iinclude -o out/ios/hakios-metal-probe \
	tools/probe.cpp out/ios/libhakios-metal.a $LIBS -framework IOKit -framework CoreFoundation -framework IOSurface 2>&1 | tail -30
[ -f out/ios/hakios-metal-probe ] || { echo "METAL-FAILED: no probe"; exit 1; }
step "driver"
# 3. the Metal plug-in (hakios's window-server/userland/src/HakiosMetalCA.m: the device, CoreAnimation's window server
# drawn through it) over the same library, as a bundle named RedwoodMetal: the name the image's
# /System/Library/Extensions/RedwoodMetal.bundle link and its /redwood/lib bundle already have, so that only the small
# image changes (build-gl.sh puts it there, with HakiosMetalDevice its principal class). No ARC (the file says why);
# native_bridge_metal_* (Metal on Vulkan, metal_m2v=1) are weak and not linked: looked up at load, absent here.
rm -f out/ios/RedwoodMetal
$IOSCXX -x objective-c -fno-objc-arc -O2 -Wno-unused-parameter -Wno-deprecated-declarations -Iinclude -c \
	/hm/window-server/userland/src/HakiosMetalCA.m -o out/ios/HakiosMetalCA.o 2>&1 | { grep -E 'error' || true; } | head -30
[ -f out/ios/HakiosMetalCA.o ] || { echo "METAL-FAILED: HakiosMetalCA.m"; exit 1; }
$IOSCXX -bundle -o out/ios/RedwoodMetal out/ios/HakiosMetalCA.o out/ios/libhakios-metal.a $LIBS -lobjc \
	-framework Foundation -framework Metal -framework IOKit -framework IOSurface -framework CoreFoundation \
	-Wl,-undefined,dynamic_lookup 2>&1 | { grep -v 'warning' || true; } | tail -20
[ -f out/ios/RedwoodMetal ] || { echo "METAL-FAILED: the plug-in did not link"; exit 1; }
step "plug-in"
cp out/ios/RedwoodMetal /work/metal/RedwoodMetal
cp out/ios/hakios-metal-probe /work/boot-bin/hakios-metal-probe
cp out/ios/hakios-metal-probe out/ios/libhakios-metal.a /work/metal/
ls -la /work/boot-bin/hakios-metal-probe /work/metal/
echo METAL-BUILD-OK
INNER
chmod +x "$HERE/build/inner-metal.sh"

echo "build-metal: log in build/metal.log"
$RT run --rm --platform linux/arm64 -e PROBE_UNTIL="${PROBE_UNTIL:-}" -v "$HERE/build:/work" -v "$HM:/hm:ro" \
	-v shennong-ios:/ios -v shennong-kernel:/k shennong-gl /work/inner-metal.sh 2>&1 | tee "$HERE/build/metal.log"
grep -q METAL-BUILD-OK "$HERE/build/metal.log" && echo "build-metal: done -> build/boot-bin/hakios-metal-probe (now bash build-gl.sh, ./xl ramdisk, ./xl run)" \
	|| { echo "build-metal: FAILED, see build/metal.log"; exit 1; }
