#!/bin/bash
# Build the iOS 16.5 images HackiOS boots on the phone, on a Mac, with Podman (or Docker):
#
#   cd ~/tmp/shennong && bash build-ios.sh
#
#   1. ios165/fetch.sh     downloads the root filesystem and the cryptexes of iOS 16.5 (iPhone X) straight
#                          from Apple's CDN (about 7.4 GB) and unpacks what the system is made of
#   2. userland/build.sh   REST=1, our programs from prebuilt/: root.apfs (small, travels with the kernel)
#                          and system.apfs (2.5 GB, uploaded once per loader session with `xl ramdisk`)
#                          The phone's firmware in build/fw-gpu/ (and other build/fw-*/) goes into
#                          root.apfs as fw/; after the first run only root.apfs is made again (seconds).
#
# The results go to build/hackios/userland/ -- where ./xl looks for them on the Mac.
#
# Unlike build-uefi.sh this runs in a NATIVE arm64 container: nothing here needs x86, and the two heavy
# parts (7-Zip unpacking a 4.7 GB APFS image, Python compressing 5.4 GB of files with zlib) would crawl
# under QEMU's emulation. The iOS files are unpacked into a volume inside the Podman VM (shennong-ios),
# not into the folder shared with the Mac: the Mac's filesystem does not tell upper from lower case, and
# iOS has names that differ only in case. A second run downloads nothing and rebuilds only what changed.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$HERE/port" ] || { echo "no port/ next to this script"; exit 1; }
mkdir -p "$HERE/build/out" "$HERE/build/hackios/userland" "$HERE/build/ios-image"
# the port's files, copied into the build directory
rm -rf "$HERE/build/port" && cp -a "$HERE/port" "$HERE/build/port"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then
	RT=podman
	podman machine start >/dev/null 2>&1 || true
else
	echo "need Docker Desktop or Podman"; exit 1
fi

cat > "$HERE/build/inner-ios.sh" <<'INNER'
#!/bin/bash
set -e
T0=$SECONDS; step() { echo "TIME: $1 -- $((SECONDS - T0)) s"; }
echo "VM: $(nproc) CPUs, $(free -g | awk '/^Mem:/{print $2}') GB RAM, $(df -h /ios | awk 'NR==2{print $4}') free in the volume"
[ "$(free -g | awk '/^Mem:/{print $2}')" -ge 5 ] || echo "WARNING: little RAM in the Podman VM; the image builder may run out (podman machine set --memory 8192)"
HACKIOS_COMMIT=d82c64a3a31cd27a0a0afbf6e213e467e44b1b4c
git config --global --add safe.directory '*'
git config --global user.name build; git config --global user.email build@localhost
cd /ios
# a tree of its own, inside the VM, with the same port files as the Mac's
if [ ! -d hackios ]; then
	git clone https://github.com/oosexgang/hackios-xnu-kernel.git hackios
	git -C hackios checkout -q $HACKIOS_COMMIT
fi
cd hackios
cp -a /work/port/. .
# the userland repository, checked out into the same tree (common/setup.sh)
common/setup.sh 2>&1 | grep -E "^setup: (cloning|hackios)" || true
[ -f userland/build.sh ] || { echo "the userland repository did not come"; exit 1; }
# userland/build.sh gives iOS this machine's time zone with systemd's timedatectl, which a container has
# not got: "zone=$(timedatectl ...)" then fails, and under set -e the build stops right there without a
# word. A stand-in that answers with the Mac's zone (HOST_TZ, passed in by the script outside).
printf '#!/bin/sh\necho "%s"\n' "${HOST_TZ:-UTC}" > /usr/local/bin/timedatectl && chmod +x /usr/local/bin/timedatectl
# the phone's own firmware (the vendor's, never in a repository), pulled from its Android into build/fw-*
# on the Mac: userland/build.sh takes ../fw-gpu and the like into root.apfs (fw/), where the kernel reads it
for d in /work/fw-*; do
	[ -d "$d" ] || continue
	rm -rf "$(basename "$d")" && cp -r "$d" . && echo "firmware: $(basename "$d"): $(ls "$d" | tr '\n' ' ')"
done
echo "time zone for iOS: $(timedatectl)"
# userland/build.sh copies drawable_test without checking for it, unlike every program beside it, and
# prebuilt/userland has none: a PREBUILT build (ours: no cross toolchain here) stops there. The same
# guard its neighbours have. (Idempotent: the line no longer matches once changed.)
sed -i 's#^\(\s*\)cp obj/hello_dyn obj/svc_test obj/surf_test obj/drawable_test obj/rwtouch obj/mg_test root/bin/$#\1cp obj/hello_dyn obj/svc_test obj/surf_test obj/rwtouch obj/mg_test root/bin/\n\1[ -f obj/drawable_test ] \&\& cp obj/drawable_test root/bin/   \# (prebuilt/ has none)#' userland/build.sh
# shennong's vibration motor: HyperOS taps from ../fw-haptic (get-haptic.sh: rtpN.bin, read by
# bsd/dev/shennong_haptic.c), which userland/build.sh, knowing only redwood's aw8697_haptic.bin, leaves out:
# put in beside that one. (Idempotent: added once.)
grep -q 'fw-haptic/rtp' userland/build.sh || sed -i 's#^\(\s*\)install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/$#&\nfi\nif ls ../fw-haptic/rtp*.bin >/dev/null 2>\&1; then\n\1mkdir -p root/fw\n\1install -m 0644 ../fw-haptic/rtp*.bin root/fw/   \# (shennong: HyperOS taps, get-haptic.sh)#' userland/build.sh
# shennong's cameras' colour tuning, made beforehand out of HyperOS's tuning files (their newer format, which
# tools/camtuning.py does not read) and kept as build/fw-camera/camera-<sensor>.isp: put into root/fw/ as they are,
# under the name libredwood-media looks for (the wide camera is its sensor 1: camera-s5khm2sd.isp). (Idempotent.)
grep -q 'fw-camera/camera-\*\.isp' userland/build.sh || sed -i 's|^# \.\.\. and with ADSP=1 the image|if ls ../fw-camera/camera-*.isp >/dev/null 2>\&1; then mkdir -p root/fw; install -m 0644 ../fw-camera/camera-*.isp root/fw/; fi   # (shennong: made beforehand)\n&|' userland/build.sh
# init built from userland/src/init.c with tools' change for var_apfs=BLOCK (the data volume on the flash, APFS written
# by the kernel) -- build/init-var-apfs, made by ~/tmp/apfsk/init/build-init.sh, source beside it. Without
# var_apfs it does what the prebuilt one does. (Idempotent: the line is added once.)
if [ -f /work/init-var-apfs ]; then
	cp /work/init-var-apfs init-var-apfs
	grep -q 'init-var-apfs' userland/build.sh || sed -i 's#^cp obj/init root/sbin/init$#&\n[ -f ../init-var-apfs ] \&\& cp ../init-var-apfs root/sbin/init   \# (build-ios.sh: var_apfs)#' userland/build.sh
	echo "init: build/init-var-apfs (var_apfs=BLOCK: /private/var on the flash)"
fi
# Files' "On My iPhone" and "Save to Files": their extensions' programs, too big for select-system.py's limit
# (build/keep-big.py says more). The first build after this downloads and extracts the root filesystem again.
[ -f /work/keep-big.py ] && python3 /work/keep-big.py ios165/select-system.py
step "trees"
# 1. iOS 16.5 out of Apple's IPSW (resumes: what is done is marked .done-*). First a quick look that
# Apple still serves that IPSW, so that a missing file says so at once instead of after 20 retries.
if [ ! -f ios165/.done-cryptex-app ]; then
	URL=$(python3 -c "import re;s=open('ios165/fetch-from-ipsw.py').read();m=re.search(r'URL = \((.*?)\)',s,re.S);print(eval('('+m.group(1)+')'))")
	code=$(curl -s -o /dev/null -w '%{http_code}' -r 0-0 "$URL" || true)
	case "$code" in
	200|206) echo "Apple CDN: the iOS 16.5 IPSW is there ($code)" ;;
	*) echo "IPSW-UNREACHABLE: Apple's CDN answered '$code' for $URL"; exit 1 ;;
	esac
fi
# KEEP=1: rootfs.dmg stays in the volume, and a change of the file selection downloads nothing (4.7 GB otherwise)
KEEP=1 ios165/fetch.sh
step "iOS files"
# 2. the images: what common/build.sh does in its "images" step for this device (REST=1 and the
# profile's USERLAND_VARS, empty for shennong), called directly: common/build.sh does not stop on a
# failed userland/build.sh, and that one stops silently on a failed test (set -e). Traced, so a failure
# shows the line it stopped at.
# OSLOG_JOBS="label ..." (bash build-ios.sh with it set): those daemons' os_log goes to the console too, beside the
# window server's (userland/build.sh OSLOG, mkjobs.py: OS_ACTIVITY_DT_MODE), e.g. com.apple.powerd for Battery Health
# build/boot-bin/*: our own programs, put in /redwood/bin (userland/build.sh EXTRA) and run first at boot by the job
# com.hakios.demo (rc.jobs: a copy of the chosen one with them in front), e.g. bh_notify for Battery Health
if ls /work/boot-bin/* >/dev/null 2>&1; then
	chmod +x /work/boot-bin/*
	export EXTRA="$(ls /work/boot-bin/* | tr '\n' ' ')"
	{ for f in /work/boot-bin/*; do echo "/redwood/bin/$(basename "$f")"; done; echo "/redwood/bin/mg_test HomeButtonType JwLB44/jEB8aFDpXQ16Tuw ArtworkDeviceSubType oPeik/9e8lQWMszEjbPzng DeviceSupportsFaceID main-screen-class main-screen-width"; cat "userland/${RC_JOBS:-rc.jobs}"; } > userland/rc.jobs.shennong
	# (the mg_test line: what MobileGestalt says about the home button and the artwork -- gestures, status bar, Dynamic Island)
	export RC_JOBS=rc.jobs.shennong
	echo "run at boot first: $EXTRA"
fi
if [ -n "${OSLOG_JOBS:-}" ]; then export OSLOG="com.apple.backboardd $OSLOG_JOBS"; echo "os_log on the console: $OSLOG"; else unset OSLOG; fi
# TrollStore (sideload/enable.sh puts build/trollstore there): TrollStore.tar and its root helper in /redwood/share/trollstore,
# where build/boot-bin/tsboot finds them at boot to install TrollStore onto the flash (sideload/README.md). Without the
# directory nothing changes.
if [ -d /work/trollstore ]; then
	export EXTRA_SHARE="${EXTRA_SHARE:+$EXTRA_SHARE }/work/trollstore"
	echo "TrollStore: /redwood/share/trollstore ($(ls /work/trollstore | tr '\n' ' '))"
fi
if ! REST=1 bash -x userland/build.sh > /work/userland-trace.log 2>&1; then
	echo "USERLAND-BUILD-FAILED: the last lines of build/userland-trace.log:"
	tail -25 /work/userland-trace.log | cut -c1-220
	exit 1
fi
grep -E "^(build\.sh|mkapfs\.py|  )" /work/userland-trace.log | tail -8 || true
step "images"
for f in system.apfs root.apfs; do
	[ -s userland/$f ] || { echo "userland/$f was not made"; exit 1; }
	# copied only when changed: system.apfs is 2.5 GB
	cmp -s userland/$f /work/hackios/userland/$f || cp userland/$f /work/hackios/userland/$f
done
ls -la /work/hackios/userland/
step "total"
echo IOS-BUILD-OK
INNER
chmod +x "$HERE/build/inner-ios.sh"

cat > "$HERE/build/ios-image/Containerfile" <<'IMG'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y python3 git curl ca-certificates 7zip gcc libc6-dev make \
	&& rm -rf /var/lib/apt/lists/*
IMG
echo "build-ios: preparing the build image (native arm64, once)"
$RT build --platform linux/arm64 -t shennong-ios "$HERE/build/ios-image" 2>&1 \
	| tee "$HERE/build/ios-image.log" | grep -E "^STEP|^COMMIT|rror" || true
$RT image inspect shennong-ios >/dev/null 2>&1 || { echo "build-ios: image build FAILED, see build/ios-image.log"; exit 1; }

echo "build-ios: log in build/ios.log (the download is about 7.4 GB)"
# the Mac's time zone, for iOS (/etc/localtime -> /var/db/timezone/zoneinfo/Europe/Moscow, say)
HOST_TZ=$(readlink /etc/localtime 2>/dev/null | sed -E 's#.*/zoneinfo/##')
$RT run --rm --platform linux/arm64 -e HOST_TZ="${HOST_TZ:-UTC}" -e RC_JOBS="${RC_JOBS:-}" -e OSLOG_JOBS="${OSLOG_JOBS:-}" -v "$HERE/build:/work" -v shennong-ios:/ios shennong-ios /work/inner-ios.sh 2>&1 \
	| tee "$HERE/build/ios.log"
grep -q IOS-BUILD-OK "$HERE/build/ios.log" && echo "build-ios: done -> $HERE/build/hackios/userland" \
	|| { echo "build-ios: FAILED, see build/ios.log"; exit 1; }
