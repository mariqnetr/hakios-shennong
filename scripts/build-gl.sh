#!/bin/bash
# The iOS images WITH Mesa (userland GL=1): CoreAnimation then draws with OpenGL ES on the Adreno 750
# when the kernel keeps it (boot-arg gpu=1). On a Mac, with Podman (or Docker):
#
#   cd ~/tmp/shennong && bash build-gl.sh
#
# build-ios.sh takes our userland programs from prebuilt/userland, because its container has no cross
# toolchain, and prebuilt/ has no Mesa. This builds the userland from its sources instead, in a container
# that has both volumes: shennong-ios (the iOS files and the HackiOS tree with the userland, made by
# build-ios.sh) and shennong-kernel (theos: the cctools and the iOS 16.5 SDK, fetched by build-kernel.sh).
# Run those two first, once. Mesa (tools/mk-mesa.sh: Mesa 25.0.7 with HackiOS's changes, softpipe and
# freedreno) is downloaded and built the first time: some minutes. The images go where build-ios.sh puts
# them (build/hackios/userland/); `./xl ramdisk` afterwards, since system.apfs is made again too.
# Logs: build/gl.log, build/userland-gl-trace.log, and Mesa's own as build/mesa-*.log when it fails.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
[ -d "$HERE/port" ] || { echo "no port/ next to this script"; exit 1; }
mkdir -p "$HERE/build/hackios/userland" "$HERE/build/gl-image"
# the port's files, copied into the build directory
rm -rf "$HERE/build/port" && cp -a "$HERE/port" "$HERE/build/port"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then
	RT=podman
	podman machine start >/dev/null 2>&1 || true
else
	echo "need Docker Desktop or Podman"; exit 1
fi

cat > "$HERE/build/inner-gl.sh" <<'INNER'
#!/bin/bash
set -e
T0=$SECONDS; step() { echo "TIME: $1 -- $((SECONDS - T0)) s"; }
echo "VM: $(nproc) CPUs, $(free -g | awk '/^Mem:/{print $2}') GB RAM, $(df -h /ios | awk 'NR==2{print $4}') free in the volume"
git config --global --add safe.directory '*'
git config --global user.name build; git config --global user.email build@localhost
export THEOS=/k/theos
CCTOOLS=$THEOS/toolchain/linux/iphone/bin
[ -x $CCTOOLS/ld ] && [ -d $THEOS/sdks/iPhoneOS16.5.sdk/usr/include ] ||
	{ echo "GL-FAILED: no theos in the shennong-kernel volume: run bash build-kernel.sh first"; exit 1; }
[ -f /ios/hackios/userland/build.sh ] && [ -f /ios/hackios/ios165/.done-cryptex-app ] ||
	{ echo "GL-FAILED: no iOS files or userland in the shennong-ios volume: run bash build-ios.sh first"; exit 1; }
cd /ios/hackios
# the port files, as build-ios.sh copies them
cp -a /work/port/. .
printf '#!/bin/sh\necho "%s"\n' "${HOST_TZ:-UTC}" > /usr/local/bin/timedatectl && chmod +x /usr/local/bin/timedatectl
for d in /work/fw-*; do
	[ -d "$d" ] || continue
	rm -rf "$(basename "$d")" && cp -r "$d" . && echo "firmware: $(basename "$d"): $(ls "$d" | tr '\n' ' ')"
done
# shennong's Wi-Fi: the WCN7850's firmware from ../fw-kiwi (the phone's modem partition, image/kiwi; read by
# bsd/dev/shennong_wlan.c), which userland/build.sh, knowing only redwood's ../fw-wifi, leaves out: put in
# after ../fw-gpu's. (Idempotent: added once.)
grep -q 'fw-kiwi' userland/build.sh || sed -i 's#^\(\s*\)cp \.\./fw-gpu/\* root/fw/$#&\nfi\nif [ -d ../fw-kiwi ]; then\n\1mkdir -p root/fw\n\1cp ../fw-kiwi/* root/fw/   \# (shennong: the WCN7850 firmware, shennong_wlan.c)#' userland/build.sh
# shennong's cameras' colour tuning, made beforehand out of HyperOS's tuning files (their newer format, which
# tools/camtuning.py does not read) and kept as build/fw-camera/camera-<sensor>.isp: put into root/fw/ as they are,
# under the name libredwood-media looks for (the wide camera is its sensor 1: camera-s5khm2sd.isp). (Idempotent.)
grep -q 'fw-camera/camera-\*\.isp' userland/build.sh || sed -i 's|^# \.\.\. and with ADSP=1 the image|if ls ../fw-camera/camera-*.isp >/dev/null 2>\&1; then mkdir -p root/fw; install -m 0644 ../fw-camera/camera-*.isp root/fw/; fi   # (shennong: made beforehand)\n&|' userland/build.sh
# the cross toolchain where userland/build.sh and tools/mk-mesa.sh look for it (xnu-deps/host/bin, as
# xnu-deps/setup.sh links it for the kernel): the Mach-O linker and its companions from theos
mkdir -p xnu-deps/host/bin
for tool in nm otool strip lipo libtool nmedit ctf_insert codesign_allocate install_name_tool; do
	ln -sfn $CCTOOLS/$tool xnu-deps/host/bin/$tool
done
ln -sfn $CCTOOLS/ld xnu-deps/host/bin/ld64
ln -sfn xnu-clang xnu-deps/host/bin/xnu-clang++
# tools/mk-mesa.sh strips the library with llvm-strip and then renames it with install_name_tool; LLVM 20's
# llvm-strip leaves a dylib that the cctools' install_name_tool calls malformed (two sections overlapping).
# The cctools' own strip takes the same arguments (-x -o OUT IN) and keeps the file to their liking.
printf '#!/bin/sh\nexec %s/strip "$@"\n' $CCTOOLS > xnu-deps/host/bin/llvm-strip && chmod +x xnu-deps/host/bin/llvm-strip
# llvm-ar, llvm-ranlib, llvm-strip for Mesa (after everything else: host/bin's nm and strip are the Mach-O ones)
export PATH=$PATH:/usr/lib/llvm-20/bin
echo "toolchain: $(xnu-deps/host/bin/ld64 -v 2>&1 | head -1); $(clang --version | head -1); meson $(meson --version)"
step "trees"
# the images, compiled (no PREBUILT) and with Mesa
# build/boot-bin/* (our programs, run first at boot) and OSLOG_JOBS (daemons' os_log on the console): as in build-ios.sh
if ls /work/boot-bin/* >/dev/null 2>&1; then
	chmod +x /work/boot-bin/*
	export EXTRA="$(ls /work/boot-bin/* | tr '\n' ' ')"
	{ for f in /work/boot-bin/*; do echo "/redwood/bin/$(basename "$f")"; done; cat "userland/${RC_JOBS:-rc.jobs}"; } > userland/rc.jobs.shennong
	export RC_JOBS=rc.jobs.shennong
	echo "run at boot first: $EXTRA"
fi
if [ -n "${OSLOG_JOBS:-}" ]; then export OSLOG="com.apple.backboardd $OSLOG_JOBS"; echo "os_log on the console: $OSLOG"; else unset OSLOG; fi
# an app installed by TrollStore that dies on launch: libredwood-app.dylib says its last words on the console (inject_app.c).
# userland/ is not in git, so changed here as the other fixes are. (Idempotent.)
sed -i 's|^/redwood/lib/libredwood-app.dylib Sileo$|/redwood/lib/libredwood-app.dylib Sileo Swiftgram|' userland/build.sh
grep -q 'libredwood-app.dylib Sileo Swiftgram' userland/build.sh || { echo "GL-FAILED: the app library's INJECT line was not found"; exit 1; }
# TrollStore (sideload/enable.sh puts build/trollstore there), as in build-ios.sh: /redwood/share/trollstore. Without the directory nothing changes.
if [ -d /work/trollstore ]; then
	export EXTRA_SHARE="${EXTRA_SHARE:+$EXTRA_SHARE }/work/trollstore"
	echo "TrollStore: /redwood/share/trollstore ($(ls /work/trollstore | tr '\n' ' '))"
fi
# hakios-metal's Metal plug-in (build-metal.sh: HakiosMetalCA.m over the A750's driver) in place of redwoodmetal's, as
# the same bundle RedwoodMetal with HakiosMetalDevice its principal class; QuartzCore's default.metallib for it
# (build/ca-share, hakios's tools/ca-metallib.py) in /redwood/share/ca-share, where the plug-in looks. Inert unless the
# kernel names the plug-in (boot-arg metal_plugin=1) or metal_props does. (Idempotent: added once.)
grep -q 'hakios-metal plug-in' userland/build.sh || sed -i 's#^\(\s*\)cp src/metal_plugin.plist root/lib/RedwoodMetal.bundle/Info.plist$#&\n\1if [ -f /work/metal/RedwoodMetal ]; then   \# (shennong: the hakios-metal plug-in)\n\1\tcp /work/metal/RedwoodMetal root/lib/RedwoodMetal.bundle/RedwoodMetal\n\1\tsed -i "s|<string>RedwoodMetalDevice</string>|<string>HakiosMetalDevice</string>|" root/lib/RedwoodMetal.bundle/Info.plist\n\1fi#' userland/build.sh
grep -q 'hakios-metal plug-in' userland/build.sh || { echo "GL-FAILED: the RedwoodMetal bundle's lines were not found in userland/build.sh"; exit 1; }
if [ -f /work/ca-share/QuartzCore-default.metallib ]; then
	export EXTRA_SHARE="${EXTRA_SHARE:+$EXTRA_SHARE }/work/ca-share"
	echo "Metal: QuartzCore-default.metallib in /redwood/share/ca-share"
fi
# init with var_apfs, mg_reset, mg_dump (build-init.sh -> build/init-var-apfs), as build-ios.sh puts it in
[ -f /work/init-var-apfs ] && cp /work/init-var-apfs init-var-apfs && echo "init: build/init-var-apfs"
cp /work/inject_bt.c userland/src/; cp /work/bluetool.c userland/src/; cp /work/com.apple.BlueTool.plist userland/; python3 /work/fix-userland-bt.py
# shennong's ambient light sensor for iOS's auto-brightness (build-als.sh -> build/als/libshennong-als.dylib): in
# /redwood/share/als, and backboardd started with it beside libredwood.dylib. Without the library nothing changes. (Idempotent.)
if [ -f /work/als/libshennong-als.dylib ]; then
	export EXTRA_SHARE="${EXTRA_SHARE:+$EXTRA_SHARE }/work/als"
	grep -q 'libshennong-als' userland/build.sh || sed -i 's#^/redwood/lib/libredwood-tweaks.dylib Preferences"}$#/redwood/share/als/libshennong-als.dylib backboardd\n&#' userland/build.sh
	grep -q 'libshennong-als' userland/build.sh || { echo "GL-FAILED: the INJECT list was not found in userland/build.sh"; exit 1; }
	echo "light sensor: /redwood/share/als/libshennong-als.dylib in backboardd"
fi
if ! REST=1 GL=1 bash -x userland/build.sh > /work/userland-gl-trace.log 2>&1; then
	echo "GL-BUILD-FAILED: the last lines of build/userland-gl-trace.log:"
	tail -30 /work/userland-gl-trace.log | cut -c1-240
	M=xnu-deps/build/mesa
	for f in meson-setup.log ninja.log; do [ -f $M/$f ] && cp $M/$f /work/mesa-$f; done
	[ -f $M/build-softpipe-freedreno/meson-logs/meson-log.txt ] && cp $M/build-softpipe-freedreno/meson-logs/meson-log.txt /work/mesa-meson-log.txt
	ls /work/mesa-*.log /work/mesa-*.txt 2>/dev/null || true
	exit 1
fi
grep -E "^(build\.sh|mk-mesa\.sh|mkapfs\.py|  )" /work/userland-gl-trace.log | tail -10 || true
step "images"
ls -la userland/root/lib/libOSMesa.8.dylib userland/root/lib/libredwood-gl.dylib ||
	{ echo "GL-FAILED: the image has no Mesa"; exit 1; }
for f in system.apfs root.apfs; do
	[ -s userland/$f ] || { echo "userland/$f was not made"; exit 1; }
	cmp -s userland/$f /work/hackios/userland/$f || cp userland/$f /work/hackios/userland/$f
done
ls -la /work/hackios/userland/
step "total"
echo GL-BUILD-OK
INNER
chmod +x "$HERE/build/inner-gl.sh"

# Ubuntu 24.04 (theos's toolchain is built against its libraries) with clang 20 as /usr/bin/clang (the
# compiler xnu-clang calls), LLVM's tools, what Mesa's build needs and what the image builder needs
cat > "$HERE/build/gl-image/Containerfile" <<'IMG'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y build-essential git curl ca-certificates python3 python3-venv perl xz-utils \
	flex bison m4 clang-20 lld-20 llvm-20 libxml2 uuid-dev libtinfo6 7zip meson ninja-build pkg-config \
	&& ln -sf /usr/bin/clang-20 /usr/bin/clang && ln -sf /usr/bin/clang++-20 /usr/bin/clang++ \
	&& rm -rf /var/lib/apt/lists/*
IMG
echo "build-gl: preparing the build image (native arm64, once)"
$RT build --platform linux/arm64 -t shennong-gl "$HERE/build/gl-image" 2>&1 \
	| tee "$HERE/build/gl-image.log" | grep -E "^STEP|^COMMIT|rror" || true
$RT image inspect shennong-gl >/dev/null 2>&1 || { echo "build-gl: image build FAILED, see build/gl-image.log"; exit 1; }

echo "build-gl: log in build/gl.log (the first time Mesa is downloaded and built: some minutes)"
HOST_TZ=$(readlink /etc/localtime 2>/dev/null | sed -E 's#.*/zoneinfo/##')
# ALLOW: jobs userland/mkjobs.py would deny, let through. By default duetexpertd and mobileassetd: the wallpaper
# gallery (Settings > Wallpaper > Add New Wallpaper) is filled by duetexpertd, which needs mobileassetd (18 November).
$RT run --rm --platform linux/arm64 -e HOST_TZ="${HOST_TZ:-UTC}" -e RC_JOBS="${RC_JOBS:-}" -e OSLOG_JOBS="${OSLOG_JOBS:-}" -e ALLOW="${ALLOW-duetexpertd mobileassetd}" -e SYSTEM="${SYSTEM:-}" -v "$HERE/build:/work" -v shennong-ios:/ios \
	-v shennong-kernel:/k shennong-gl /work/inner-gl.sh 2>&1 | tee "$HERE/build/gl.log"
grep -q GL-BUILD-OK "$HERE/build/gl.log" && echo "build-gl: done -> $HERE/build/hackios/userland (now ./xl ramdisk)" \
	|| { echo "build-gl: FAILED, see build/gl.log"; exit 1; }
