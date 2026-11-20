#!/bin/bash
# Build the shennong UEFI (Mu-Silicium + HackiOS port) and the HackiOS boot image on a Mac,
# inside an x86-64 Ubuntu container (Docker Desktop or Podman). Run it from ~/tmp/shennong:
#
#   cd ~/tmp/shennong && bash build-uefi.sh
#   cd ~/tmp/shennong && MODE=DEBUG bash build-uefi.sh     # UEFI that prints its log on the screen
#
# Everything (sources, ~6 GB) stays in ~/tmp/shennong/build; results land in ~/tmp/shennong/build/out:
#   Mu-shennong.img      Mu-Silicium UEFI alone: `fastboot boot` it first -- the UEFI should show on screen
#   shennong-boot.img    the same UEFI with the HackiOS loader inside (common/build.sh)
# The full log is build/build.log. Nothing is written to the phone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$HERE/port" ] || { echo "no port/ next to this script"; exit 1; }
mkdir -p "$HERE/build/out"
# the port's files, copied into the build directory
rm -rf "$HERE/build/port" && cp -a "$HERE/port" "$HERE/build/port"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then
	RT=podman
	podman machine inspect >/dev/null 2>&1 || podman machine init --cpus 6 --memory 8192
	podman machine start >/dev/null 2>&1 || true
else
	echo "need Docker Desktop or Podman:  brew install podman   (or install Docker Desktop)"; exit 1
fi
echo "build-uefi: using $RT; log: $HERE/build/build.log"

cat > "$HERE/build/inner.sh" <<'INNER'
#!/bin/bash
set -ex
export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
HACKIOS_COMMIT=d82c64a3a31cd27a0a0afbf6e213e467e44b1b4c
cd /work
T0=$SECONDS; step() { set +x; echo "TIME: $1 -- $((SECONDS - T0)) s"; set -x; }
# how x86 code runs here: Rosetta is fast, QEMU several times slower
set +x; echo "X86-EMULATION: $(grep -m1 -o -E 'rosetta|qemu[^ /]*' /proc/self/maps || echo 'none (native)')"; set -x
# packages come with the shennong-build image (built once, kept by the container runtime)
git config --global --add safe.directory '*'
git config --global user.name build; git config --global user.email build@localhost

# HackiOS with the shennong port
if [ ! -d hackios ]; then
	git clone https://github.com/oosexgang/hackios-xnu-kernel.git hackios
	git -C hackios checkout -q $HACKIOS_COMMIT
fi
cd hackios
cp -a /work/port/. .

# uefi/apply.sh, but tolerant: one test-only submodule (cmocka, on gitlab) is not needed to build
# Submodules recursively, except OpenSSL's own (krb5, pyca, fuzz corpora, wycheproof...) and cmocka:
# tests and fuzzing only, gigabytes, not needed to build
init_subs() {
	local d=$1 p
	[ -f "$d/.gitmodules" ] || return 0
	for p in $(git -C "$d" config -f .gitmodules --get-regexp '\.path$' | awk '{print $2}'); do
		case "$d/$p" in */openssl/*|*/cmocka*) continue;; esac
		git -C "$d" submodule update --init --jobs 8 -- "$p" || { echo "submodule $d/$p failed"; continue; }
		init_subs "$d/$p"
	done
}
if [ ! -f Mu-Silicium/.port-applied ]; then
	# a clone left by an interrupted run is reused, not downloaded again
	[ -d Mu-Silicium/.git ] || git clone https://github.com/Project-Silicium/Mu-Silicium.git Mu-Silicium
	git -C Mu-Silicium checkout -q -f "$(cat uefi/BASE)"
	init_subs Mu-Silicium
	for s in Binaries Common/Mu Common/Mu_OEM_Sample Mu_Basecore Silicium-ACPI; do
		[ -n "$(ls -A Mu-Silicium/$s 2>/dev/null)" ] || { echo "submodule $s missing"; exit 1; }
	done
	git -C Mu-Silicium apply --whitespace=nowarn "$PWD/uefi/mu-silicium.patch"
	touch Mu-Silicium/.port-applied
fi
# the port's own files: copied every run, so a change in port/ reaches Mu-Silicium too. -a keeps their
# dates: a plain copy made every file look new to EDK2, which then rebuilt the whole firmware each time
cp -a uefi/files/. Mu-Silicium/
# what hakios-uefi changes in Silicium's own files: the name the firmware gives itself
sed -i 's/"Project Silicium for %a %a\\n"/"HakiOS UEFI (Mu-Silicium) for %a %a\\n"/' Mu-Silicium/Silicon/Silicium/SiliciumPkg/Sec/Sec.c
sed -i 's/PcdFirmwareVendor|L"Project Silicium"/PcdFirmwareVendor|L"HakiOS"/' Mu-Silicium/Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc
# no 3 s wait in the boot manager before the loader starts (the loader's own menu is where one chooses)
sed -i 's/gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|3/gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut|0/' Mu-Silicium/Silicon/Silicium/SiliciumPkg/SiliciumPkg.dsc.inc
step "port files"

# the Python tools live in a volume inside the VM (/venv): thousands of small files read on every build
[ -x /venv/bin/python3 ] || python3 -m venv /venv
. /venv/bin/activate
REQ=$(sha1sum Mu-Silicium/pip-requirements.txt | cut -c1-40)
[ "$(cat /venv/.req 2>/dev/null)" = "$REQ" ] || { pip install -q -r Mu-Silicium/pip-requirements.txt && echo "$REQ" > /venv/.req; }
step "python tools"

MODE=${MODE:-RELEASE}
# 1. the UEFI (Mu-Silicium's own boot image, no HackiOS payload)
( cd Mu-Silicium && python3 build_uefi.py -d shennong -r $MODE )
step "UEFI build"
ls -la Mu-Silicium/Build/shennongPkg/${MODE}_CLANGPDB/FV/SILICIUM_UEFI.fd
if [ $MODE = DEBUG ]; then
	# DEBUG: the UEFI prints its log on the screen (FrameBufferSerialPortLib) -- for finding where it stops
	cp Mu-Silicium/Mu-shennong-0.img /work/out/Mu-shennong-0-debug.img
	# and the HackiOS loader on top of that DEBUG UEFI: the UEFI's log and the loader's screen together.
	# (common/build.sh takes the RELEASE UEFI, so the steps are done here, with the profile's addresses.)
	( set -a; . common/devices/shennong.conf; set +a
	  PAYLOAD_BASE=$PAYLOAD_BASE xnuloader/build.sh
	  python3 xnuloader/mkpayload.py -o out/usb-payload.bin loader.efi=xnuloader/loader.efi --usb
	  UEFI_FD=$PWD/Mu-Silicium/Build/shennongPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd UEFI_TARGET=shennong \
	  FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE DTB= \
		python3 mkxnuimg.py -p out/usb-payload.bin -o /work/out/shennong-boot-debug.img )
else
	cp Mu-Silicium/Mu-shennong-0.img /work/out/
	# 2. HackiOS loader + boot image for shennong (prebuilt kernel; no iOS images needed for this)
	SKIP="kernel images" common/build.sh shennong
	cp out/shennong-boot.img /work/out/
fi
ls -la /work/out
step "total"
echo BUILD-OK
INNER
chmod +x "$HERE/build/inner.sh"

# The build tools go into an image once: later runs reuse it and download nothing again
# (Mu-Silicium's setup_env.sh apt list, plus what HackiOS's loader and boot image need).
mkdir -p "$HERE/build/image"      # its own empty folder: the build context is sent to the VM
cat > "$HERE/build/image/Containerfile" <<'IMG'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y pip git mono-devel build-essential lld uuid-dev nasm \
	gcc-aarch64-linux-gnu python3 python3-git python3-pip gettext locales gnupg ca-certificates python3-venv \
	git-core clang llvm curl && rm -rf /var/lib/apt/lists/*
IMG
echo "build-uefi: preparing the build image (only the first time takes long)"
$RT build --platform linux/amd64 -t shennong-build "$HERE/build/image" 2>&1 \
	| tee "$HERE/build/image.log" | grep -E "^STEP|^Successfully|^COMMIT|rror" || true
$RT image exists shennong-build 2>/dev/null || $RT image inspect shennong-build >/dev/null 2>&1 \
	|| { echo "build-uefi: image build FAILED, see build/image.log"; exit 1; }

# EDK2's build tree (tens of thousands of small files) and the Python tools stay in volumes inside the
# VM: the folder shared with the Mac is slow at that. The first run after this change builds everything once.
$RT run --rm --platform linux/amd64 -e MODE="${MODE:-RELEASE}" -v "$HERE/build:/work" \
	-v shennong-edk2-build:/work/hackios/Mu-Silicium/Build -v shennong-venv:/venv \
	shennong-build /work/inner.sh 2>&1 | tee "$HERE/build/build.log"
grep -q BUILD-OK "$HERE/build/build.log" && echo "build-uefi: done -> $HERE/build/out" || { echo "build-uefi: FAILED, see build/build.log"; exit 1; }
