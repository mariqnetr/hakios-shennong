#!/bin/bash
# HyperOS's own haptic waveforms of this very phone (odm/firmware/N_name_P_RTP.bin: what Xiaomi's vibrator
# HAL plays through RichTap, signed 8-bit samples at 24 kHz) into build/fw-haptic/ as rtpN.bin, those that
# fit the haptics module's FIFO (2048 samples). They come from the odm partition get-zap.sh already took out
# of the OTA (build/zap/odm). build-ios.sh then puts build/fw-haptic/ into root.apfs (fw/), where
# bsd/dev/shennong_haptic.c reads them (haptic=4: rtp183, rtp181, rtp166 by default). The vendor's files,
# never in a repository.
#
#   cd ~/tmp/shennong && bash get-haptic.sh && bash build-ios.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/build/zap/odm/firmware"
[ -d "$SRC" ] || { echo "no build/zap/odm/firmware: run bash get-zap.sh first (it takes the odm partition out of the OTA)"; exit 1; }
OUT="$HERE/build/fw-haptic"
rm -rf "$OUT" && mkdir -p "$OUT"
n=0
for f in "$SRC"/*_RTP.bin; do
	b=$(basename "$f")
	num=${b%%_*}
	size=$(wc -c < "$f" | tr -d ' ')
	[ "$size" -gt 0 ] && [ "$size" -le 2048 ] || continue
	[ -e "$OUT/rtp$num.bin" ] && continue
	cp "$f" "$OUT/rtp$num.bin"
	n=$((n + 1))
done
echo "build/fw-haptic: $n of HyperOS's waveforms as rtpN.bin ($(du -sk "$OUT" | cut -f1) KB); now: bash build-ios.sh"
for want in 183 181 166; do
	[ -f "$OUT/rtp$want.bin" ] && echo "  rtp$want.bin: $(ls "$SRC"/${want}_*_RTP.bin | xargs -n1 basename), $(wc -c < "$OUT/rtp$want.bin" | tr -d ' ') samples" || echo "  rtp$want.bin: NOT THERE"
done
