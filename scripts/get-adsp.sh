#!/bin/bash
# The audio DSP's firmware of this very phone's HyperOS build (adsp.mdt, adsp.bNN and adsp_dtb.*, about
# 20 MB), taken out of Xiaomi's full OTA of that build without downloading its 6 GB, as get-zap.sh does
# for the GPU: ota-part.py fetches only the modem partition's data (NON-HLOS, a FAT image, a few hundred
# MB) and checks it against the manifest's sha256; the files are taken from its image/ directory.
#
#   cd ~/tmp/shennong && bash get-adsp.sh
#
# Result: build/fw-audio/ (then bash build-ios.sh / build-gl.sh put it into the system image's /fw, where
# bsd/dev/redwood_adsp.c reads it).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
VER=OS3.0.308.0.WNBCNXM
FILE=shennong-ota_full-OS3.0.308.0.WNBCNXM-user-16.0-704ee35814.zip
URLS="https://bigota.d.miui.com/$VER/$FILE https://bkt-sgp-miui-ota-update-alisgp.oss-ap-southeast-1.aliyuncs.com/$VER/$FILE https://cdnorg.d.miui.com/$VER/$FILE"
[ -f "$HERE/ota-part.py" ] || { echo "no ota-part.py next to this script"; exit 1; }
mkdir -p "$HERE/build/fw-audio" "$HERE/build/adsp"
cp "$HERE/ota-part.py" "$HERE/build/adsp/"

if command -v docker >/dev/null && docker info >/dev/null 2>&1; then RT=docker
elif command -v podman >/dev/null; then RT=podman; podman machine start >/dev/null 2>&1 || true
else echo "need Docker Desktop or Podman"; exit 1; fi
IMG=shennong-ios
$RT image inspect $IMG >/dev/null 2>&1 || IMG=docker.io/library/ubuntu:24.04

$RT run --rm --platform linux/arm64 -e URLS="$URLS" -v "$HERE/build:/work" $IMG bash -c '
set -e
cd /work/adsp
echo "installing mtools in the container (a minute)"
apt-get update -qq >/dev/null && apt-get install -y -qq python3 ca-certificates mtools >/dev/null 2>&1
first=${URLS%% *}; rest=${URLS#* }
if [ ! -s modem.img ]; then
	echo "asking Xiaomi'"'"'s servers for the OTA'"'"'s directory and the modem partition"
	python3 -I -u ota-part.py "$first" modem modem.img $rest
fi
export MTOOLS_SKIP_CHECK=1
mdir -i modem.img ::/image | grep -i adsp || { echo "no adsp files in the modem partition'"'"'s image/"; exit 1; }
rm -rf /work/fw-audio/adsp*
for f in $(mdir -b -i modem.img ::/image | grep -i "/adsp[._]" ); do
	mcopy -n -i modem.img "::${f#::}" /work/fw-audio/ 2>/dev/null || mcopy -n -i modem.img "$f" /work/fw-audio/
done
cd /work/fw-audio
for f in *; do lower=$(echo "$f" | tr A-Z a-z); [ "$f" = "$lower" ] || mv "$f" "$lower"; done
[ "$(head -c4 adsp.mdt | od -An -c | tr -d " ")" = "177ELF" ] || { echo "adsp.mdt is not an ELF file"; exit 1; }
ls -l /work/fw-audio | head -80
du -sh /work/fw-audio
echo ADSP-OK
'
