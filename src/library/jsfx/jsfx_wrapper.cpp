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

#include "jsfx_wrapper.h"

#include <cstring>

#include "elklog/static_logger.h"

#include "library/event.h"
#include "library/midi_encoder.h"

namespace sushi::internal::jsfx_wrapper {

namespace {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("jsfx");

MidiDataByte make_midi_bytes(const RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::NOTE_ON:
        {
            auto typed = event.keyboard_event();
            return midi::encode_note_on(typed->channel(), typed->note(), typed->velocity());
        }
        case RtEventType::NOTE_OFF:
        {
            auto typed = event.keyboard_event();
            return midi::encode_note_off(typed->channel(), typed->note(), typed->velocity());
        }
        case RtEventType::NOTE_AFTERTOUCH:
        {
            auto typed = event.keyboard_event();
            return midi::encode_poly_key_pressure(typed->channel(), typed->note(), typed->velocity());
        }
        case RtEventType::AFTERTOUCH:
        {
            auto typed = event.keyboard_common_event();
            return midi::encode_channel_pressure(typed->channel(), typed->value());
        }
        case RtEventType::MODULATION:
        {
            auto typed = event.keyboard_common_event();
            return midi::encode_control_change(typed->channel(), midi::MOD_WHEEL_CONTROLLER_NO, typed->value());
        }
        case RtEventType::PITCH_BEND:
        {
            auto typed = event.keyboard_common_event();
            return midi::encode_pitch_bend(typed->channel(), typed->value());
        }
        case RtEventType::WRAPPED_MIDI_EVENT:
            return event.wrapped_midi_event()->midi_data();
        default:
            return {};
    }
}

} // namespace

JsfxWrapper::JsfxWrapper(HostControl host_control, PluginInfo plugin_info)
    : Processor(host_control),
      _plugin_info(std::move(plugin_info))
{
    _max_input_channels = JSFX_MAX_CHANNELS;
    _max_output_channels = JSFX_MAX_CHANNELS;
}

JsfxWrapper::~JsfxWrapper()
{
    if (_effect)
    {
        libjsfx_destroy_effect(_effect);
    }
    if (_context)
    {
        libjsfx_shutdown(_context);
    }
}

ProcessorReturnCode JsfxWrapper::init(float sample_rate)
{
    _sample_rate = sample_rate;

    _context = libjsfx_init(nullptr);
    if (!_context)
    {
        ELKLOG_LOG_ERROR("Failed to initialize libjsfx context");
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    _effect = _create_effect(static_cast<int>(sample_rate));
    if (!_effect)
    {
        ELKLOG_LOG_ERROR("Failed to load JSFX effect: {}", libjsfx_get_error(_context));
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    libjsfx_metadata_t metadata;
    if (libjsfx_get_metadata(_effect, &metadata) == LIBJSFX_OK)
    {
        set_name(metadata.name);
        _max_input_channels = metadata.num_inputs;
        _max_output_channels = metadata.num_outputs;
        _supports_midi_input = (metadata.is_instrument != 0);
    }

    _register_parameters();

    return ProcessorReturnCode::OK;
}

void JsfxWrapper::configure(float sample_rate)
{
    _sample_rate = sample_rate;

    if (_effect && _context)
    {
        libjsfx_destroy_effect(_effect);
        _effect = _create_effect(static_cast<int>(sample_rate));
        if (!_effect)
        {
            ELKLOG_LOG_ERROR("Failed to recreate JSFX effect on sample rate change: {}", libjsfx_get_error(_context));
        }
    }
}

void JsfxWrapper::process_event(const RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::FLOAT_PARAMETER_CHANGE:
        case RtEventType::INT_PARAMETER_CHANGE:
        case RtEventType::BOOL_PARAMETER_CHANGE:
        {
            auto typed_event = event.parameter_change_event();
            auto param_id = typed_event->param_id();
            if (param_id < _slider_infos.size())
            {
                auto& slider = _slider_infos[param_id];
                float normalized = std::clamp(typed_event->value(), 0.0f, 1.0f);
                double domain_value = slider.min_value + normalized * (slider.max_value - slider.min_value);
                libjsfx_set_slider(_effect, static_cast<int>(param_id), domain_value);
                libjsfx_trigger_slider(_effect);
            }
            break;
        }

        case RtEventType::NOTE_ON:
        case RtEventType::NOTE_OFF:
        case RtEventType::NOTE_AFTERTOUCH:
        case RtEventType::PITCH_BEND:
        case RtEventType::AFTERTOUCH:
        case RtEventType::MODULATION:
        case RtEventType::WRAPPED_MIDI_EVENT:
        {
            if (_pending_midi_count < MAX_PENDING_MIDI_EVENTS)
            {
                _pending_midi_messages[_pending_midi_count] = make_midi_bytes(event);
                _pending_midi_offsets[_pending_midi_count] = event.sample_offset();
                ++_pending_midi_count;
            }
            else
            {
                ELKLOG_LOG_WARNING("JSFX MIDI queue overflow on processor {}", this->name());
            }
            break;
        }

        case RtEventType::SET_BYPASS:
            this->set_bypassed(static_cast<bool>(event.processor_command_event()->value()));
            break;

        default:
            break;
    }
}

void JsfxWrapper::process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer)
{
    if (this->bypassed())
    {
        this->bypass_process(in_buffer, out_buffer);
        _pending_midi_count = 0;
        return;
    }

    if (!_effect)
    {
        out_buffer.clear();
        _pending_midi_count = 0;
        return;
    }

    // Send pending MIDI events to the effect
    for (size_t i = 0; i < _pending_midi_count; ++i)
    {
        libjsfx_midi_event_t midi_event;
        midi_event.frame_offset = _pending_midi_offsets[i];
        midi_event.status = _pending_midi_messages[i][0];
        midi_event.data1 = _pending_midi_messages[i][1];
        midi_event.data2 = _pending_midi_messages[i][2];
        libjsfx_midi_send(_effect, &midi_event);
    }
    _pending_midi_count = 0;

    int in_channels = in_buffer.channel_count();
    int out_channels = out_buffer.channel_count();
    _num_channels = std::max(in_channels, out_channels);

    if (_num_channels == 0)
    {
        out_buffer.clear();
        return;
    }

    // Resize interleaved buffer if needed
    size_t required_size = static_cast<size_t>(AUDIO_CHUNK_SIZE) * static_cast<size_t>(_num_channels);
    if (_interleaved_buffer.size() < required_size)
    {
        _interleaved_buffer.resize(required_size, 0.0);
    }

    // Deinterleave input: non-interleaved float -> interleaved double
    for (int frame = 0; frame < AUDIO_CHUNK_SIZE; ++frame)
    {
        for (int ch = 0; ch < _num_channels; ++ch)
        {
            if (ch < in_channels)
            {
                _interleaved_buffer[static_cast<size_t>(frame) * static_cast<size_t>(_num_channels) + static_cast<size_t>(ch)] =
                    static_cast<double>(in_buffer.channel(ch)[frame]);
            }
            else
            {
                _interleaved_buffer[static_cast<size_t>(frame) * static_cast<size_t>(_num_channels) + static_cast<size_t>(ch)] = 0.0;
            }
        }
    }

    // Process with transport info
    const auto* transport = _host_control.transport();
    if (transport)
    {
        auto time_sig = transport->time_signature();
        double playstate = (transport->playing_mode() == PlayingMode::PLAYING) ? 1.0 : 0.0;
        double playpos_sec = static_cast<double>(transport->current_samples()) / static_cast<double>(_sample_rate);
        double playpos_beats = transport->current_beats();

        libjsfx_process_ex(_effect,
                           _interleaved_buffer.data(),
                           AUDIO_CHUNK_SIZE,
                           _num_channels,
                           static_cast<double>(transport->current_tempo()),
                           time_sig.numerator,
                           time_sig.denominator,
                           playstate,
                           playpos_sec,
                           playpos_beats);
    }
    else
    {
        libjsfx_process(_effect,
                        _interleaved_buffer.data(),
                        AUDIO_CHUNK_SIZE,
                        _num_channels);
    }

    // Interleave output: interleaved double -> non-interleaved float
    for (int frame = 0; frame < AUDIO_CHUNK_SIZE; ++frame)
    {
        for (int ch = 0; ch < out_channels; ++ch)
        {
            if (ch < _num_channels)
            {
                out_buffer.channel(ch)[frame] = static_cast<float>(
                    _interleaved_buffer[static_cast<size_t>(frame) * static_cast<size_t>(_num_channels) + static_cast<size_t>(ch)]);
            }
            else
            {
                out_buffer.channel(ch)[frame] = 0.0f;
            }
        }
    }

    // Receive output MIDI events and forward them
    libjsfx_midi_event_t out_midi;
    while (libjsfx_midi_recv(_effect, &out_midi) == 1)
    {
        MidiDataByte midi_data = {out_midi.status, out_midi.data1, out_midi.data2, 0};
        output_event(RtEvent::make_wrapped_midi_event(this->id(), out_midi.frame_offset, midi_data));
    }
}

void JsfxWrapper::set_channels(int inputs, int outputs)
{
    Processor::set_channels(inputs, outputs);
}

ProcessorReturnCode JsfxWrapper::set_state(ProcessorState* state, bool /*realtime_running*/)
{
    if (!_effect || !state->has_binary_data())
    {
        return ProcessorReturnCode::UNSUPPORTED_OPERATION;
    }

    const auto& data = state->binary_data();
    auto result = libjsfx_load_state(_effect,
                                     reinterpret_cast<const char*>(data.data()),
                                     static_cast<int>(data.size()));
    if (result != LIBJSFX_OK)
    {
        ELKLOG_LOG_ERROR("Failed to load JSFX state for processor {}", this->name());
        return ProcessorReturnCode::ERROR;
    }

    return ProcessorReturnCode::OK;
}

ProcessorState JsfxWrapper::save_state() const
{
    ProcessorState state;

    if (!_effect)
    {
        return state;
    }

    // First call to get required size
    int required_size = libjsfx_save_state(_effect, nullptr, 0);
    if (required_size <= 0)
    {
        return state;
    }

    std::vector<char> buffer(static_cast<size_t>(required_size));
    int written = libjsfx_save_state(_effect, buffer.data(), required_size);
    if (written > 0)
    {
        std::vector<std::byte> binary_data(static_cast<size_t>(written));
        std::memcpy(binary_data.data(), buffer.data(), static_cast<size_t>(written));
        state.set_binary_data(std::move(binary_data));
    }

    return state;
}

bool JsfxWrapper::has_editor() const
{
    return _effect && libjsfx_has_gfx(_effect) == 1;
}

PluginInfo JsfxWrapper::info() const
{
    return _plugin_info;
}

libjsfx_effect_t* JsfxWrapper::_create_effect(int sample_rate)
{
    if (!_plugin_info.source_code.empty())
    {
        return libjsfx_create_effect_from_string(_context, _plugin_info.source_code.c_str(), sample_rate);
    }

    if (!_plugin_info.path.empty())
    {
        return libjsfx_create_effect(_context, _plugin_info.path.c_str(), sample_rate);
    }

    return nullptr;
}

void JsfxWrapper::_register_parameters()
{
    int num_sliders = libjsfx_get_num_sliders(_effect);
    if (num_sliders <= 0)
    {
        return;
    }

    _slider_infos.resize(static_cast<size_t>(num_sliders));

    for (int i = 0; i < num_sliders; ++i)
    {
        libjsfx_slider_info_t info;
        if (libjsfx_get_slider_info(_effect, i, &info) != LIBJSFX_OK)
        {
            continue;
        }

        _slider_infos[static_cast<size_t>(i)] = info;

        if (info.is_hidden || info.is_file_slider)
        {
            continue;
        }

        auto* descriptor = new FloatParameterDescriptor(
            info.name,
            info.name,
            "",
            static_cast<float>(info.min_value),
            static_cast<float>(info.max_value),
            Direction::AUTOMATABLE,
            new FloatParameterPreProcessor(static_cast<float>(info.min_value),
                                           static_cast<float>(info.max_value)));

        register_parameter(descriptor, static_cast<ObjectId>(i));
    }
}

} // namespace sushi::internal::jsfx_wrapper
