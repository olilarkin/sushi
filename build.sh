#!/bin/bash
#
# Build sushi-gui with SwiftUI support (Audio Graph viewer).
# Requires the Ninja generator because CMake's Unix Makefiles generator
# does not support Swift.
#
# Usage:
#   ./build_gui.sh                  # Release build
#   ./build_gui.sh Debug            # Debug build
#   ./build_gui.sh Release clean    # Clean + rebuild

set -euo pipefail

BUILD_TYPE="${1:-Release}"
BUILD_DIR="build-gui"
TOOLCHAIN="third-party/vcpkg/scripts/buildsystems/vcpkg.cmake"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

cd "$SCRIPT_DIR"

# Check for ninja
if ! command -v ninja &>/dev/null; then
    echo "Error: ninja is required (brew install ninja)" >&2
    exit 1
fi

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
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "Configuring with Ninja (${BUILD_TYPE})..."
    cmake -G Ninja -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DSUSHI_WITH_VST3=ON \
        -DSUSHI_WITH_CLAP=ON \
        -DSUSHI_WITH_AUV2=ON \
        -DSUSHI_WITH_CMAJOR=ON \
        -DSUSHI_WITH_JSFX=ON
fi

# Build
echo "Building sushi-gui..."
ninja -C "$BUILD_DIR" sushi-gui

echo ""
echo "Done: $BUILD_DIR/apps/sushi-gui.app"
