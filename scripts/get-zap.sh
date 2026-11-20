#!/bin/bash
# The GPU's zap shader of this very phone's HyperOS build, which HyperOS does not hand to adb:
# taken out of Xiaomi's own full OTA of that build, without downloading its 6 GB. ota-part.py fetches
# the zip's directory, the payload's manifest and only the odm partition's data (a few MB) with range
# requests, checks it against the manifest's sha256; the odm image is then opened in a container.
#
#   cd ~/tmp/shennong && bash get-zap.sh
#
# Result: build/fw-gpu/gen70900_zap.mbn (then bash build-ios.sh puts it into the small image).
# The build is the phone's: ro.build.version.incremental OS3.0.308.0.WNBCNXM (getprop.txt).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
VER=OS3.0.308.0.WNBCNXM
FILE=shennong-ota_full-OS3.0.308.0.WNBCNXM-user-16.0-704ee35814.zip
URLS="https://bigota.d.miui.com/$VER/$FILE https://bkt-sgp-miui-ota-update-alisgp.oss-ap-southeast-1.aliyuncs.com/$VER/$FILE https://cdnorg.d.miui.com/$VER/$FILE"
[ -f "$HERE/ota-part.py" ] || { echo "no ota-part.py next to this script"; exit 1; }
mkdir -p "$HERE/build/fw-gpu" "$HERE/build/zap"
cp "$HERE/ota-part.py" "$HERE/build/zap/"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then RT=podman; podman machine start >/dev/null 2>&1 || true
else echo "need Docker Desktop or Podman"; exit 1; fi
# the image build-ios.sh made (Ubuntu 24.04 with python3), or plain Ubuntu
IMG=shennong-ios
$RT image inspect $IMG >/dev/null 2>&1 || IMG=docker.io/library/ubuntu:24.04

$RT run --rm --platform linux/arm64 -e URLS="$URLS" -v "$HERE/build:/work" $IMG bash -c '
set -e
cd /work/zap
echo "installing erofs-utils in the container (a minute)"
apt-get update -qq >/dev/null && apt-get install -y -qq python3 ca-certificates erofs-utils e2fsprogs >/dev/null 2>&1
first=${URLS%% *}; rest=${URLS#* }
echo "asking Xiaomi'"'"'s servers for the OTA'"'"'s directory and the odm partition"
python3 -I -u ota-part.py "$first" odm odm.img $rest
rm -rf odm
if [ "$(od -An -tx4 -j1024 -N4 odm.img | tr -d " ")" = "e0f5e1e2" ]; then
	echo "odm.img: EROFS"; mkdir odm && fsck.erofs --extract=odm odm.img >/dev/null
elif [ "$(od -An -tx2 -j1080 -N2 odm.img | tr -d " ")" = "ef53" ]; then
	echo "odm.img: ext4"; mkdir odm && debugfs -R "rdump / odm" odm.img >/dev/null 2>&1
else
	echo "odm.img: neither EROFS nor ext4"; exit 1
fi
find odm -iname "*zap*" -exec ls -l {} \;
zap=$(find odm -name gen70900_zap.mbn | head -1)
[ -n "$zap" ] || { echo "no gen70900_zap.mbn in odm"; exit 1; }
[ "$(head -c4 "$zap" | od -An -c | tr -d " ")" = "177ELF" ] || { echo "$zap is not an ELF file"; exit 1; }
cp "$zap" /work/fw-gpu/gen70900_zap.mbn
sha256sum /work/fw-gpu/gen70900_zap.mbn
echo ZAP-OK
'
ls -l "$HERE/build/fw-gpu/"
