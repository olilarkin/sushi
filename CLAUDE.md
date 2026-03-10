# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Sushi is a headless plugin host for Elk Audio OS, written in C++20. It supports multiple audio frontends (JACK, PortAudio, Core Audio, RASPA/Xenomai) and plugin formats (VST2, VST3, LV2, CLAP, AUv2, Cmajor). It can run as a standalone application or be embedded as a library. On macOS, `sushi-gui` provides a Cocoa status bar app with plugin editor window hosting.

## Build Commands

Sushi uses CMake with vcpkg for dependency management. Out-of-source builds are required.

```bash
# First-time setup (submodules + vcpkg bootstrap)
git submodule update --init --recursive
./third-party/vcpkg/bootstrap-vcpkg.sh

# Configure and build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=../third-party/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DSUSHI_WITH_UNIT_TESTS=ON \
      ..
make -j$(nproc)

# Run all tests
make run_tests

# Run tests manually (with filters)
export SUSHI_TEST_DATA_DIR=$(pwd)/../test/data
./test/unit_tests
./test/unit_tests --gtest_filter=TestName.SubTest
```

Key CMake options: `SUSHI_WITH_JACK`, `SUSHI_WITH_PORTAUDIO`, `SUSHI_WITH_APPLE_COREAUDIO`, `SUSHI_WITH_VST3`, `SUSHI_WITH_LV2`, `SUSHI_WITH_CLAP`, `SUSHI_WITH_AUV2`, `SUSHI_WITH_CMAJOR`, `SUSHI_WITH_RPC_INTERFACE`, `SUSHI_WITH_LINK`, `SUSHI_AUDIO_BUFFER_SIZE` (8-512, default 64; fixed at compile time).

## Architecture

**Three factory modes** define how Sushi is instantiated:
- `StandaloneFactory` — full-featured standalone app (Sushi owns audio I/O)
- `ReactiveFactory` — embedded use where host drives audio callbacks via `RtController`
- `OfflineFactory` — batch/offline rendering without real-time constraints

**Core processing pipeline:**
- `AudioEngine` is the real-time audio processor, called by an audio frontend
- `AudioGraph` manages `Track` objects, each containing a chain of `Processor` instances
- `Processor` is the abstract base for all plugins (built-in, VST2/3, LV2, CLAP, AUv2, Cmajor)
- Events (parameter changes, MIDI, transport) flow through a real-time FIFO (`EventDispatcher`)

**Control layer:**
- `SushiControl` is the thread-safe control interface, composed of ~31 specialized controllers (AudioGraphController, MidiController, ParameterController, etc.)
- `RtController` provides a real-time-safe subset for use in audio callbacks
- `EditorController` manages plugin editor windows (open/close/resize/screenshot) across all plugin formats
- Remote control via gRPC (`rpc_interface/`) and OSC (`control_frontends/`)

**sushi-gui (macOS):**
- Cocoa status bar app (`apps/main_cocoa.mm`, `apps/sushi_status_bar.mm`) that hosts plugin editor windows
- Shows CPU usage, transport controls, tempo, sync mode, and track/processor hierarchy
- Built as `sushi-gui` CMake target when on macOS with at least one editor-capable plugin format enabled

**Key directories:**
- `src/engine/` — audio engine, graph, transport, event dispatcher, controllers
- `src/library/` — plugin format wrappers (vst2x/, vst3x/, lv2/, clap/, auv2/, cmajor/) and base processor
- `apps/` — standalone and sushi-gui Cocoa application entry points
- `src/audio_frontends/` — platform audio I/O (JACK, PortAudio, CoreAudio, RASPA)
- `src/plugins/` — 20+ built-in plugins including brickworks DSP suite
- `include/sushi/` — public API headers
- `rpc_interface/` — gRPC service definition and implementation
- `test/unittests/` — ~65 GTest files; test data in `test/data/`
- `misc/config_files/` — example JSON configuration files

## Code Style

LLVM-based style configured in `.clang-format`: 4-space indentation, braces on new line after classes/functions/enums/control statements, no column limit, pointers left-aligned (`int* p`), C++20 standard. Format with `clang-format`.

## Important Constraints

- Audio buffer size is compile-time fixed (`SUSHI_AUDIO_BUFFER_SIZE`)
- Reactive mode is stereo-only with async MIDI
- ALSA MIDI and RtMidi are mutually exclusive
- VST2 SDK is not included; must be provided externally if needed
- LV2 requires system libraries (lilv, lv2)
- `SUSHI_TEST_DATA_DIR` env var must be set for manual test runs; for VST3 tests, set working directory to `build/test/`
- RTTI is disabled (`-fno-rtti`); fast math is enabled (`-ffast-math`)
- Cmajor supports inline source in JSON configs (`source_code` field) or patch files (`path` field)
- AUv2 and sushi-gui Cocoa code uses Objective-C++ (`.mm` files) with ARC (`-fobjc-arc`)
