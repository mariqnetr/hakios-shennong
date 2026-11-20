#!/bin/bash
# GPU Stress: builds build/trollstore/GPUStress.ipa (ad-hoc signed; tsboot installs it through TrollStore at boot)
set -e
cd "$(dirname "$0")"
rm -rf out && mkdir -p out/Payload/GPUStress.app
xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -fobjc-arc -O2 main.m -o out/Payload/GPUStress.app/GPUStress \
	-framework UIKit -framework QuartzCore -framework Foundation -framework CoreGraphics
plutil -convert binary1 -o out/Payload/GPUStress.app/Info.plist Info.plist
codesign -f -s - out/Payload/GPUStress.app
(cd out && zip -qr GPUStress.ipa Payload)
mkdir -p ../build/trollstore && cp out/GPUStress.ipa ../build/trollstore/
echo "GPUSTRESS-OK: build/trollstore/GPUStress.ipa ($(stat -f %z out/GPUStress.ipa) bytes)"
