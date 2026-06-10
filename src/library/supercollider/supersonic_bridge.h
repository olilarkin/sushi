/*
 * Copyright 2026 Oliver Larkin
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

/**
 * @brief Thin, dependency-isolating bridge to the (JUCE-free) Supersonic scsynth core.
 *
 * This is the ONLY translation unit in Sushi that includes Supersonic's internal
 * headers (which drag in the emscripten NativeShim, scsynth, oscpack, boost, etc.).
 * Everything it exposes here is plain C++ with no Supersonic types, so the rest of
 * Sushi can talk to the engine without inheriting its include/macro surface.
 *
 * IMPORTANT: Supersonic's scsynth core keeps process-global state (the static
 * ring buffer and the single `g_world`). Only ONE SupersonicBridge can own the
 * engine at a time within a process; init() returns false if another instance
 * already holds it. The owning instance tears the World down in its destructor.
 */

#ifndef SUSHI_SUPERSONIC_BRIDGE_H
#define SUSHI_SUPERSONIC_BRIDGE_H

#include <cstdint>
#include <string>

namespace sushi::internal::supercollider {

class SupersonicBridge
{
public:
    SupersonicBridge() = default;
    ~SupersonicBridge();

    SupersonicBridge(const SupersonicBridge&) = delete;
    SupersonicBridge& operator = (const SupersonicBridge&) = delete;

    /**
     * @brief Build the scsynth World and initialise the shared-memory ring buffers.
     * @param block_size  scsynth control block size; must equal Sushi's audio chunk size.
     * @return false if another bridge already owns the process-global engine, or on failure.
     */
    bool init(double sample_rate, int block_size, int num_inputs, int num_outputs);

    bool is_initialised() const { return _initialised; }

    /// scsynth block size the World was built with (0 before init).
    int block_size() const;

    /**
     * @brief Render one block. Each channel pointer must hold block_size() samples.
     *        Safe to call from the audio thread (the World runs in NRT/inline mode).
     */
    void process(const float* const* inputs, int num_inputs,
                 float* const* outputs, int num_outputs);

    /// Enqueue an already-encoded OSC packet into the engine's command ring. RT-safe-ish (brief spinlock).
    bool send_osc_packet(const void* data, uint32_t size);

    /// Parse a single textual OSC command (e.g. "/s_new default -1 0 0 freq 440") and enqueue it.
    bool send_osc_text(const std::string& line);

    /// Load a compiled SynthDef (.scsyndef) blob via /d_recv.
    bool load_synthdef_blob(const void* data, uint32_t size);

private:
    bool   _initialised {false};
    bool   _owns_engine {false};
    int    _block_size {0};
    int    _num_inputs {0};
    int    _num_outputs {0};
    double _time {0.0};
    double _block_duration {0.0};
};

} // namespace sushi::internal::supercollider

#endif // SUSHI_SUPERSONIC_BRIDGE_H
