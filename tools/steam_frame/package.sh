#!/usr/bin/env bash
# Assembles build-frame/VBoyColorVR-linux-arm64/ from a finished build-frame/
# (the same contents as the CI's Linux package, plus the VR app).
set -euo pipefail
cd "$(dirname "$0")/../.."

out=build-frame/VBoyColorVR-linux-arm64
rm -rf "$out"
mkdir -p "$out/roms"
for exe in VBoyColorVR VBoyColor; do
    cp build-frame/$exe "$out/"
    strip "$out/$exe"
done
cp -r build-frame/fonts build-frame/icons build-frame/logo build-frame/colorpacks build-frame/icon.png "$out/"
cp assets/PlaceRomsHere.txt "$out/roms/"
cp -r assets/steam "$out/steam"
cp platform/desktop/install-to-steam.py "$out/"
cp -r README.md LICENSE THIRD-PARTY-NOTICES.md licenses "$out/"
echo "Packaged $out"
