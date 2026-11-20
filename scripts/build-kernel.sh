#!/bin/bash
# Build the HackiOS kernel (XNU xnu-8796.121.2 with HackiOS's changes and the shennong port) on a
# Mac, with Podman (or Docker):
#
#   cd ~/tmp/shennong && bash build-kernel.sh
#
# What it does, in a NATIVE arm64 Linux container (the kernel builds on Linux with stock clang):
#   1. theos's pieces the build needs: the iOS cctools (ld64, nm, strip...) for Linux arm64 and
#      sdks/iPhoneOS16.5.sdk -- nothing else of theos
#   2. its own HackiOS tree with port/ (next to this script) copied over it
#   3. xnu/apply.sh (Apple's XNU + HackiOS's changes), xnu-deps/setup.sh, ./xnu-build.sh -k
#   4. the kernel goes where ./xl looks for it on the Mac:
#      build/hackios/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
#      (./xl takes it from there instead of prebuilt/ by itself; delete it to go back to prebuilt/)
#   5. the HackiOS loader (xnuloader/loader.efi) too, and a boot image with it:
#      build/out/shennong-xnu.img = the UEFI of build/out/shennong-boot-debug.img (build-uefi.sh) + this
#      loader -- `fastboot boot` that one. The UEFI is not rebuilt here (that is build-uefi.sh's job).
#
# Everything lives in a volume inside the Podman VM (shennong-kernel): XNU's tree has names that differ
# only in case, which the Mac's filesystem does not tell apart. A second run downloads nothing and only
# rebuilds; after a change in port/ the kernel tree is set up again (a full build, minutes).
# The full log is build/kernel-build.log. Nothing is written to the phone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$HERE/port" ] || { echo "no port/ next to this script"; exit 1; }
mkdir -p "$HERE/build/out" "$HERE/build/kernel-image"
# the port's files, copied into the build directory
rm -rf "$HERE/build/port" && cp -a "$HERE/port" "$HERE/build/port"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then
	RT=podman
	podman machine start >/dev/null 2>&1 || true
else
	echo "need Docker Desktop or Podman"; exit 1
fi

cat > "$HERE/build/inner-kernel.sh" <<'INNER'
#!/bin/bash
set -e -o pipefail
T0=$SECONDS; step() { echo "TIME: $1 -- $((SECONDS - T0)) s"; }
echo "VM: $(nproc) CPUs, $(free -g | awk '/^Mem:/{print $2}') GB RAM, $(df -h /k | awk 'NR==2{print $4}') free in the volume;" \
	"$(clang --version | head -1)"
HACKIOS_COMMIT=d82c64a3a31cd27a0a0afbf6e213e467e44b1b4c
git config --global --add safe.directory '*'
git config --global user.name build; git config --global user.email build@localhost
cd /k

# 1. theos: the Linux cctools and the iOS 16.5 SDK (the two things xnu-deps/setup.sh asks for)
export THEOS=/k/theos
mkdir -p $THEOS/toolchain $THEOS/sdks
if [ ! -x $THEOS/toolchain/linux/iphone/bin/ld ]; then
	echo "kernel: downloading theos's iOS toolchain for Linux $(uname -m)"
	curl -fsSL --retry 3 "https://github.com/L1ghtmann/llvm-project/releases/latest/download/iOSToolchain-$(uname -m).tar.xz" |
		tar -xJ -C $THEOS/toolchain/
fi
v=$($THEOS/toolchain/linux/iphone/bin/ld -v 2>&1 || true)
case "$v" in
*ld64*|*PROGRAM:ld*) echo "kernel: linker: $(echo "$v" | head -1)" ;;
*) echo "KERNEL-FAILED: the cctools linker does not run here: $(echo "$v" | head -3)"; exit 1 ;;
esac
if [ ! -d $THEOS/sdks/iPhoneOS16.5.sdk/usr/include ]; then
	echo "kernel: fetching iPhoneOS16.5.sdk (only that one) from github.com/theos/sdks"
	rm -rf /k/theos-sdks
	git clone -q --filter=blob:none --no-checkout --depth 1 https://github.com/theos/sdks /k/theos-sdks
	git -C /k/theos-sdks sparse-checkout set --no-cone /iPhoneOS16.5.sdk/
	git -C /k/theos-sdks checkout -q
	rm -rf $THEOS/sdks/iPhoneOS16.5.sdk
	mv /k/theos-sdks/iPhoneOS16.5.sdk $THEOS/sdks/
	rm -rf /k/theos-sdks
fi
step "theos"

# 2. a HackiOS tree of its own, with port/ copied over it
if [ ! -d hackios ]; then
	git clone -q https://github.com/oosexgang/hackios-xnu-kernel.git hackios
	git -C hackios checkout -q $HACKIOS_COMMIT
fi
cd hackios
cp -a /work/port/. .
# the Mac's tree too, where ./xl runs (xl and the device profile are there): the same files, the same
# bookkeeping as build-uefi.sh's. Not fatal: the kernel does not need it.
(
	cd /work/hackios 2>/dev/null || exit 0
	cp -a /work/port/. .
)
step "port files"

# 3. the kernel tree: made by xnu/apply.sh; made again when the kernel's files have changed
XNU=xnu-src/xnu-8796.121.2
HASH=$( { sha1sum xnu/redwood.patch; find xnu/files -type f -print0 | sort -z | xargs -0 sha1sum; } | sha1sum | cut -c1-40)
if [ -d $XNU/.git ] && [ "$(cat $XNU/.git/port-hash 2>/dev/null)" != "$HASH" ]; then
	echo "kernel: the kernel's changes are new: setting its tree up again (the build objects stay)"
	git -C $XNU checkout -q -- .
	git -C $XNU clean -fdq            # BUILD/ is in .git/info/exclude: kept
	git -C $XNU apply --whitespace=nowarn "$PWD/xnu/redwood.patch"
	cp -a xnu/files/. $XNU/
	echo "$HASH" > $XNU/.git/port-hash
fi
if [ ! -d $XNU/.git ]; then
	xnu/apply.sh
	echo "$HASH" > $XNU/.git/port-hash
fi
xnu-deps/setup.sh 2>&1 | grep -E "^setup: " || true
[ -f xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/local/lib/kernel/libpthread_kext.a ] ||
	{ echo "KERNEL-FAILED: xnu-deps/setup.sh did not finish (see its logs in xnu-deps/ inside the volume)"; exit 1; }
step "kernel tree and dependencies"

# 3b. the Xiaomi 14 Pro's Wi-Fi driver, ath12k of Linux on FreeBSD's LinuxKPI (tools/mk-ath12k.sh), from
# the two archives in build/ (linux-7.0.14.tar.xz, freebsd-15.1-src.txz); linked into the kernel when
# it builds. Its own log: build/ath12k-build.log. Without the archives the kernel is built without it.
unset LX_OBJECT
if [ -f /work/linux-7.0.14.tar.xz ] && [ -f /work/freebsd-15.1-src.txz ] && [ -f tools/mk-ath12k.sh ]; then
	mkdir -p xnu-deps/build/ath11k
	for f in linux-7.0.14.tar.xz freebsd-15.1-src.txz; do
		[ -e xnu-deps/build/ath11k/$f ] || ln -sf /work/$f xnu-deps/build/ath11k/$f
	done
	if PATH=/usr/lib/llvm-20/bin:$PATH bash tools/mk-ath12k.sh > /work/ath12k-build.log 2>&1; then
		export LX_OBJECT=$PWD/xnu-deps/build/ath12k/out/ath12k.o
		echo "kernel: $(tail -1 /work/ath12k-build.log)"
	else
		echo "kernel: WARNING: ath12k did not build, the kernel goes without it; the end of build/ath12k-build.log:"
		tail -8 /work/ath12k-build.log | cut -c1-240
	fi
	step "ath12k"
fi

# 4. the build: make ends with an error by design (export checks after the kernel is linked: -k is
# required, see xnu/README.md) -- what counts is a kernel newer than this run
K=$XNU/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
# the makefiles ask xcrun (xnu-deps/host/bin/xcrun: host/bin, then /usr/bin) for dsymutil, which makes the
# kernel's .dSYM before it is stripped; Ubuntu's LLVM has it only as dsymutil-20 (Arch: /usr/bin/dsymutil)
[ -x /usr/bin/dsymutil ] || ln -sf "$(ls /usr/lib/llvm-*/bin/dsymutil 2>/dev/null | tail -1)" /usr/bin/dsymutil 2>/dev/null || true
[ -x /usr/bin/dsymutil ] || { echo "KERNEL-FAILED: no dsymutil (LLVM's) in the image"; exit 1; }
echo "kernel: $(/usr/bin/dsymutil --version 2>&1 | grep -m1 -i version)"
touch /tmp/kernel-start
./xnu-build.sh -j$(nproc) -k > /work/kernel-build.log 2>&1 || true
# a kernel from this run; or one already up to date (no source newer than it)
if [ -f $K ] && { [ $K -nt /tmp/kernel-start ] ||
	[ -z "$(find $XNU -path $XNU/BUILD -prune -o -type f -newer $K -print -quit)" ]; }; then
	:
else
	echo "KERNEL-FAILED: no new kernel; the first errors of build/kernel-build.log:"
	grep -n -E "error:|Error [0-9]|\*\*\*" /work/kernel-build.log | head -20 | cut -c1-240
	exit 1
fi
step "build"
ls -la $K
grep -c "syna: touch screen ready" $K >/dev/null && echo "kernel: the Synaptics touch driver is in it" ||
	echo "kernel: WARNING: no Synaptics touch driver in this kernel"
mkdir -p /work/hackios/$(dirname $K)
cp $K /work/hackios/$K
cp $K /work/out/

# 5. the loader, with this device's payload address, and the boot image with it
# where this phone's darwin is: build/phones/shennong.darwin, made by common/flash/darwinid.py from-gpt
DID=; [ -f /work/phones/shennong.darwin ] && DID=/work/phones/shennong.darwin
( set -a; . common/devices/shennong.conf; set +a; DARWIN_ID=$DID PAYLOAD_BASE=$PAYLOAD_BASE xnuloader/build.sh ) > /work/loader-build.log 2>&1 ||
	{ echo "KERNEL-FAILED: the loader did not build, see build/loader-build.log"; tail -5 /work/loader-build.log; exit 1; }
mkdir -p /work/hackios/xnuloader
cp xnuloader/loader.efi /work/hackios/xnuloader/loader.efi
python3 xnuloader/mkpayload.py -o /tmp/usb-payload.bin loader.efi=xnuloader/loader.efi --usb > /dev/null
if [ -f /work/out/shennong-boot-debug.img ]; then
	python3 tools/repack-xnuimg.py /work/out/shennong-boot-debug.img /tmp/usb-payload.bin /work/out/shennong-xnu.img
else
	echo "kernel: WARNING: no build/out/shennong-boot-debug.img (MODE=DEBUG bash build-uefi.sh makes it): no boot image made"
fi
step "total"
echo KERNEL-BUILD-OK
INNER
chmod +x "$HERE/build/inner-kernel.sh"

# Ubuntu 24.04 for theos's toolchain (built against its libraries), with clang 20 as /usr/bin/clang
# (the build calls /usr/bin/clang; HackiOS was tested with a recent clang, 24.04's default is 18).
cat > "$HERE/build/kernel-image/Containerfile" <<'IMG'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y build-essential git curl ca-certificates python3 perl xz-utils \
	flex bison m4 autoconf automake libtool gawk clang-20 lld-20 llvm-20 libncurses-dev libtinfo6 libxml2 uuid-dev \
	&& ln -sf /usr/bin/clang-20 /usr/bin/clang && ln -sf /usr/bin/clang++-20 /usr/bin/clang++ \
	&& ln -sf /usr/lib/llvm-20/bin/dsymutil /usr/bin/dsymutil \
	&& ln -sf /usr/bin/ld.lld-20 /usr/bin/ld.lld && ln -sf /usr/bin/lld-link-20 /usr/bin/lld-link \
	&& rm -rf /var/lib/apt/lists/*
IMG
echo "build-kernel: preparing the build image (native arm64, once)"
$RT build --platform linux/arm64 -t shennong-kernel "$HERE/build/kernel-image" 2>&1 \
	| tee "$HERE/build/kernel-image.log" | grep -E "^STEP|^COMMIT|rror" || true
$RT image inspect shennong-kernel >/dev/null 2>&1 || { echo "build-kernel: image build FAILED, see build/kernel-image.log"; exit 1; }

echo "build-kernel: log in build/kernel.log (the compiler's own output: build/kernel-build.log)"
$RT run --rm --platform linux/arm64 -v "$HERE/build:/work" -v shennong-kernel:/k shennong-kernel /work/inner-kernel.sh 2>&1 \
	| tee "$HERE/build/kernel.log"
grep -q KERNEL-BUILD-OK "$HERE/build/kernel.log" &&
	echo "build-kernel: done -> build/hackios/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837" ||
	{ echo "build-kernel: FAILED, see build/kernel.log"; exit 1; }
