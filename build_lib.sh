#!/bin/bash
#
# Build the sushi static library (libsushi_library.a).
# Uses Unix Makefiles by default (no Swift/Ninja dependency).
#
# Usage:
#   ./build_lib.sh                  # Release build
#   ./build_lib.sh Debug            # Debug build
#   ./build_lib.sh Release clean    # Clean + rebuild

set -euo pipefail

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build-lib"
TOOLCHAIN="third-party/vcpkg/scripts/buildsystems/vcpkg.cmake"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$SCRIPT_DIR"

# Bootstrap vcpkg if needed
if [ ! -f third-party/vcpkg/vcpkg ]; then
    git submodule update --init --recursive
    ./third-party/vcpkg/bootstrap-vcpkg.sh
fi

# Clean if requested
if [ "${2:-}" = "clean" ]; then
    echo "Cleaning $BUILD_DIR..."
    rm -rf "$BUILD_DIR"
fi

# Configure
if [ ! -f "$BUILD_DIR/Makefile" ]; then
    echo "Configuring (${BUILD_TYPE})..."
    cmake -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DSUSHI_WITH_VST3=ON \
        -DSUSHI_WITH_CLAP=ON \
        -DSUSHI_WITH_AUV2=ON \
        -DSUSHI_WITH_CMAJOR=ON
fi

# Build
echo "Building sushi_library..."
cmake --build "$BUILD_DIR" --target sushi_library -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

echo ""
echo "Done: $BUILD_DIR/libsushi_library.a"
