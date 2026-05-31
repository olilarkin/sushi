# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Sushi is a headless plugin host for Elk Audio OS, written in C++20. It supports multiple audio frontends (JACK, PortAudio, Core Audio, RASPA/Xenomai) and plugin formats (VST2, VST3, LV2, CLAP, AUv2, Cmajor, JSFX, Faust, SuperCollider). It can run as a standalone application or be embedded as a library. On macOS, `sushi-gui` provides a Cocoa status bar app with plugin editor window hosting.

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

Key CMake options: `SUSHI_WITH_JACK`, `SUSHI_WITH_PORTAUDIO`, `SUSHI_WITH_APPLE_COREAUDIO`, `SUSHI_WITH_VST3`, `SUSHI_WITH_LV2`, `SUSHI_WITH_CLAP`, `SUSHI_WITH_AUV2`, `SUSHI_WITH_CMAJOR`, `SUSHI_WITH_JSFX`, `SUSHI_WITH_FAUST`, `SUSHI_FAUST_WITH_LLVM`, `SUSHI_WITH_SUPERCOLLIDER`, `SUSHI_WITH_RPC_INTERFACE`, `SUSHI_WITH_LINK`, `SUSHI_BUILD_STANDALONE_APP`, `SUSHI_BUILD_WITH_SANITIZERS`, `SUSHI_AUDIO_BUFFER_SIZE` (8-512, default 64; fixed at compile time).

## Architecture

### Factory Modes

Three factory classes (all inheriting `FactoryInterface`) define how Sushi is instantiated:
- `StandaloneFactory` — full-featured standalone app (Sushi owns audio I/O)
- `ReactiveFactory` — embedded use where host drives audio callbacks via `RtController` (stereo-only, async MIDI)
- `OfflineFactory` — batch/offline rendering without real-time constraints

Each factory's `new_instance(SushiOptions&)` returns a `std::pair<unique_ptr<Sushi>, Status>`. `ReactiveFactory` additionally provides `rt_controller()` which returns the RT-safe callback interface (`RtController`).

### Core Processing Pipeline

- `AudioEngine` is the real-time audio processor, called by an audio frontend
- `AudioGraph` manages `Track` objects, each containing a chain of `Processor` instances
- `Processor` is the abstract base for all plugins (built-in, VST2/3, LV2, CLAP, AUv2, Cmajor, JSFX, Faust)
- Events (parameter changes, MIDI, transport) flow through a real-time FIFO (`EventDispatcher`)

### Threading Model

- **RT thread:** `AudioEngine::process_chunk` runs entirely on the audio callback thread. All code called from here must be RT-safe (no locks, no allocations).
- **Event thread:** `EventDispatcher` runs its own `std::thread` (`_event_loop`) for non-RT event processing.
- **Worker thread:** A lower-priority `Worker` thread handles time-consuming tasks (plugin instantiation, async processor work).
- **RT↔non-RT boundary:** Two `RtSafeRtEventFifo` queues (lock-free, wait-free SPSC circular FIFOs of 1024 `RtEvent`s) shuttle events between RT and non-RT sides. Non-RT side uses mutex-based `SynchronizedQueue`.
- **Multicore audio:** When `SushiOptions::rt_cpu_cores > 1`, `AudioGraph` creates a `twine::WorkerPool` and distributes tracks across cores round-robin.
- **RT-safe locking:** `SpinLock` (`src/library/spinlock.h`) is used where mutexes are forbidden.

### Event System

Two distinct event types cross the RT boundary:
- **`RtEvent`** (`src/library/rt_event.h`): Fixed 32-byte value type, no heap allocation, 40+ types (NOTE_ON/OFF, FLOAT_PARAMETER_CHANGE, SET_BYPASS, INSERT_PROCESSOR, ASYNC_WORK, etc.). Travels via lock-free FIFO.
- **`Event`** (`src/library/event.h`): Heap-allocated polymorphic type with timestamps and completion callbacks. Converted to/from `RtEvent` at the RT boundary.

Flow: non-RT caller → `EventDispatcher::post_event()` → mutex queue → event loop converts to `RtEvent` → lock-free FIFO → RT callback dispatches to processors → processors emit `RtEvent`s back → lock-free FIFO → event loop converts to `Event` → notifies listeners.

### Control Layer

`SushiControl` (`include/sushi/control_interface.h`) is the thread-safe control facade, composed of 13 sub-controller interfaces:

`SystemController`, `TransportController`, `TimingController`, `KeyboardController`, `AudioGraphController`, `ProgramController`, `ParameterController`, `MidiController`, `AudioRoutingController`, `CvGateController`, `OscController`, `SessionController`, `EditorController`

The concrete `engine::Controller` owns all 13 `controller_impl::*` implementations by value. `controller_common.h` provides `to_internal()`/`to_external()` conversions between public (`sushi::control::`) and internal (`sushi::internal::`) namespaces.

Remote control is available via gRPC (`rpc_interface/`) and OSC (`control_frontends/`).

### Plugin Format Wrappers

All plugin wrappers follow a uniform pattern:
- Inherit from `Processor`, override `init()`, `configure()`, `process_event()`, `process_audio()`
- Each format has a `*ProcessorFactory` inheriting `BaseProcessorFactory` with `new_instance()` returning `std::pair<ProcessorReturnCode, shared_ptr<Processor>>`
- `PluginRegistry` maps `PluginType` → factory, lazily created
- Each format lives in its own namespace (e.g. `sushi::internal::vst3`, `sushi::internal::clap_wrapper`)
- Editor hosting: VST3, CLAP, AUv2, Cmajor, JSFX, Faust each have a `*EditorHost` class; Apple platforms use `.mm` (Objective-C++); Faust editor uses Swift (`FaustSwiftUI`)

### sushi-gui (macOS)

Cocoa status bar app (`apps/main_cocoa.mm`, `apps/sushi_status_bar.mm`) that hosts plugin editor windows. Shows CPU usage, transport controls, tempo, sync mode, and track/processor hierarchy. Built as `sushi-gui` CMake target when on macOS with at least one editor-capable plugin format enabled.

### JSON Configuration

`JsonConfigurator` uses RapidJSON with embedded JSON Schema validation. Seven config sections loaded sequentially at startup: `HOST_CONFIG`, `TRACKS`/`PRE_TRACK`/`POST_TRACK`, `MIDI`, `OSC`, `CV_GATE`, `EVENTS`, `STATE`. Schemas are in `src/engine/json_schemas/`.

### Key Directories

- `src/engine/` — audio engine, graph, transport, event dispatcher, controllers
- `src/library/` — plugin format wrappers (vst2x/, vst3x/, lv2/, clap/, auv2/, cmajor/, jsfx/, faust/, supercollider/) and base processor
- `apps/` — standalone and sushi-gui Cocoa application entry points
- `src/audio_frontends/` — platform audio I/O (JACK, PortAudio, CoreAudio, RASPA)
- `src/plugins/` — 20+ built-in plugins including brickworks DSP suite
- `include/sushi/` — public API headers
- `rpc_interface/` — gRPC service definition and implementation
- `test/unittests/` — GTest files; test data in `test/data/`
- `misc/config_files/` — example JSON configuration files

## Code Style

LLVM-based style configured in `.clang-format`: 4-space indentation, braces on new line after classes/functions/enums/control statements, no column limit, pointers left-aligned (`int* p`), C++20 standard, includes are not sorted. Format with `clang-format`.

## Testing Patterns

- Google Test + Google Mock. All tests use `TEST_F` with fixtures.
- Many tests `#include` the `.cpp` file under test directly (not just headers) for access to internal symbols.
- Production code uses `friend class *Accessor` pattern; test files define `*Accessor` classes to expose private members.
- Hand-written mockups in `test_utils/` (e.g. `EventDispatcherMockup`, `EngineMockup`, `HostControlMockup`) alongside GMock-based mocks.
- Tests mirror source layout: `test/unittests/engine/`, `test/unittests/library/`, `test/unittests/plugins/`, etc.

## Important Constraints

- Audio buffer size is compile-time fixed (`SUSHI_AUDIO_BUFFER_SIZE`)
- RTTI is disabled (`-fno-rtti`); fast math is enabled (`-ffast-math`)
- ALSA MIDI and RtMidi are mutually exclusive
- VST2 SDK is not included; must be provided externally if needed
- LV2 requires system libraries (lilv, lv2); not available on macOS or Windows
- CLAP, JSFX, and Faust are off by default on all platforms and must be explicitly enabled
- Faust has two backends: interpreter (always) and LLVM JIT (opt-in via `SUSHI_FAUST_WITH_LLVM`)
- `SUSHI_TEST_DATA_DIR` env var must be set for manual test runs; for VST3 tests, set working directory to `build/test/`
- Cmajor supports inline source in JSON configs (`source_code` field) or patch files (`path` field)
- SuperCollider support (`SUSHI_WITH_SUPERCOLLIDER`, off by default) embeds the JUCE-free Supersonic scsynth core (`third-party/supersonic` submodule) as a `supersonic_core` static lib; the wrapper lives in `src/library/supercollider/` and talks to it only through `supersonic_bridge.h`. Single-instance per process (scsynth has process-global state), stereo in/out, NRT mode, OSC-driven, no GUI editor, built with `NO_LIBSNDFILE`
- AUv2 and sushi-gui Cocoa code uses Objective-C++ (`.mm` files) with ARC (`-fobjc-arc`)
