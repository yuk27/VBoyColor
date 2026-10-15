#!/usr/bin/env bash
# Builds VBoyColorVR and VBoyColor for Linux ARM64 (Steam Frame) in Docker,
# then packages them into build-frame/VBoyColorVR-linux-arm64/.
# Needs a running Docker whose containers are arm64 (Apple Silicon / Colima).
# Usage: tools/steam_frame/build-linux-arm64.sh [extra cmake configure args]
set -euo pipefail
cd "$(dirname "$0")/../.."

IMAGE=vboycolor-frame-build

docker build -t "$IMAGE" -f tools/steam_frame/linux-arm64.Dockerfile tools/steam_frame

# build-frame/ is the build dir (gitignored). The FetchContent downloads live in
# a named volume, so wiping build-frame/ doesn't refetch everything.
# -j3: glslang needs about a GB per compile job and the Colima VM is small.
docker run --rm \
    -v "$PWD":/src \
    -v vboycolor-frame-deps:/deps \
    "$IMAGE" bash -euo pipefail -c '
        cmake -S . -B build-frame -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DGLFW_BUILD_WAYLAND=OFF -DFETCHCONTENT_BASE_DIR=/deps "$@"
        cmake --build build-frame --target VBoyColorVR VBoyColor -j3
        tools/steam_frame/package.sh
    ' _ "$@"
