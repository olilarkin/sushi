/*
 * Copyright 2017-2023 Elk Audio AB
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

#ifndef SUSHI_JSFX_WRAPPER_H
#define SUSHI_JSFX_WRAPPER_H

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "libjsfx/libjsfx.h"

#include "library/processor.h"

namespace sushi::internal::jsfx_wrapper {

class JsfxWrapper : public Processor
{
public:
    static constexpr int JSFX_MAX_CHANNELS = 64;

    JsfxWrapper(HostControl host_control, PluginInfo plugin_info);
    ~JsfxWrapper() override;

    ProcessorReturnCode init(float sample_rate) override;
    void configure(float sample_rate) override;
    void process_event(const RtEvent& event) override;
    void process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer) override;
    void set_channels(int inputs, int outputs) override;

    ProcessorReturnCode set_state(ProcessorState* state, bool realtime_running) override;
    ProcessorState save_state() const override;
    PluginInfo info() const override;
    bool has_editor() const override { return false; }

private:
    void _register_parameters();
    libjsfx_effect_t* _create_effect(int sample_rate);

    PluginInfo _plugin_info;
    libjsfx_context_t* _context {nullptr};
    libjsfx_effect_t* _effect {nullptr};
    float _sample_rate {44100.0f};
    int _num_channels {0};

    std::vector<double> _interleaved_buffer;
    std::vector<libjsfx_slider_info_t> _slider_infos;

    static constexpr size_t MAX_PENDING_MIDI_EVENTS = 256;
    std::array<MidiDataByte, MAX_PENDING_MIDI_EVENTS> _pending_midi_messages {};
    std::array<int, MAX_PENDING_MIDI_EVENTS> _pending_midi_offsets {};
    size_t _pending_midi_count {0};
};

} // namespace sushi::internal::jsfx_wrapper

#endif // SUSHI_JSFX_WRAPPER_H
