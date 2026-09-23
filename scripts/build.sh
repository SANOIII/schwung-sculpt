#!/usr/bin/env bash
# Build Sculpt for Ableton Move (aarch64) and package it for Schwung.
#
# Uses Docker for cross-compilation unless CROSS_PREFIX is set
# (e.g. CROSS_PREFIX=aarch64-linux-gnu- on a machine with the toolchain,
#  or CROSS_PREFIX= "" to build natively on an ARM64 Linux box).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="schwung-sculpt-builder"
ID="sculpt"

if [ -z "${CROSS_PREFIX+x}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Sculpt build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX-aarch64-linux-gnu-}"
cd "$REPO_ROOT"
echo "=== Building Sculpt (prefix: '${CROSS_PREFIX}') ==="

mkdir -p build "dist/$ID"

ARCH_FLAGS=""
if [ -n "$CROSS_PREFIX" ]; then ARCH_FLAGS="-march=armv8-a -mtune=cortex-a72"; fi

${CROSS_PREFIX}gcc -std=gnu11 -O3 -ffast-math -shared -fPIC $ARCH_FLAGS \
    -fomit-frame-pointer -DNDEBUG -Wall -Wno-format-truncation \
    src/dsp/sculpt.c \
    -o build/dsp.so \
    -Isrc/dsp \
    -lm -lpthread

# cat instead of cp avoids ExtFS/Docker dealloc quirks on macOS
cat src/module.json > "dist/$ID/module.json"
cat src/ui.js       > "dist/$ID/ui.js"
cat src/help.json   > "dist/$ID/help.json"
cat build/dsp.so    > "dist/$ID/dsp.so"
chmod +x "dist/$ID/dsp.so"

(cd dist && tar -czf "$ID-module.tar.gz" "$ID/")

echo ""
echo "=== Build complete ==="
echo "Module:  dist/$ID/"
echo "Tarball: dist/$ID-module.tar.gz"
echo "Install: ./scripts/install.sh   (Move must be reachable as move.local)"
