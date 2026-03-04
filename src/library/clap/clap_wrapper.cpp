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
 * @brief Wrapper for CLAP plugins.
 */

#include <cstring>
#include <algorithm>

#include "twine/twine.h"

#include "elklog/static_logger.h"

#include "clap_wrapper.h"
#include "library/event.h"

namespace sushi::internal::clap_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("clap");

constexpr int CLAP_NAME_BUFFER_SIZE = 256;

// ClapEventList implementation

ClapEventList::ClapEventList()
{
    _buffer.resize(CLAP_EVENT_BUFFER_SIZE);
    _offsets.reserve(256);

    _input_events.ctx = this;
    _input_events.size = _input_size;
    _input_events.get = _input_get;

    _output_events.ctx = this;
    _output_events.try_push = _output_try_push;
}

void ClapEventList::clear()
{
    _count = 0;
    _write_pos = 0;
    _offsets.clear();
}

void ClapEventList::push(const clap_event_header_t* event)
{
    auto needed = _write_pos + event->size;
    if (needed > _buffer.size())
    {
        _buffer.resize(needed * 2);
    }
    _offsets.push_back(_write_pos);
    memcpy(_buffer.data() + _write_pos, event, event->size);
    _write_pos += event->size;
    _count++;
}

const clap_event_header_t* ClapEventList::get(uint32_t index) const
{
    if (index >= _count)
    {
        return nullptr;
    }
    return reinterpret_cast<const clap_event_header_t*>(_buffer.data() + _offsets[index]);
}

uint32_t ClapEventList::_input_size(const clap_input_events_t* list)
{
    return static_cast<ClapEventList*>(list->ctx)->size();
}

const clap_event_header_t* ClapEventList::_input_get(const clap_input_events_t* list, uint32_t index)
{
    return static_cast<ClapEventList*>(list->ctx)->get(index);
}

bool ClapEventList::_output_try_push(const clap_output_events_t* list, const clap_event_header_t* event)
{
    static_cast<ClapEventList*>(list->ctx)->push(event);
    return true;
}

// ClapWrapper implementation

ClapWrapper::ClapWrapper(HostControl host_control,
                         const std::string& plugin_path,
                         int plugin_index,
                         ClapHostContext* host_context) :
        Processor(host_control),
        _plugin_load_path(plugin_path),
        _plugin_index(plugin_index),
        _host_context(host_context)
{
    _max_input_channels = CLAP_WRAPPER_MAX_N_CHANNELS;
    _max_output_channels = CLAP_WRAPPER_MAX_N_CHANNELS;
    _enabled = false;
}

ClapWrapper::~ClapWrapper()
{
    ELKLOG_LOG_DEBUG("Unloading CLAP plugin {}", this->name());
    _cleanup();
}

void ClapWrapper::_cleanup()
{
    if (_instance.is_processing())
    {
        _instance.stop_processing();
    }
    if (_instance.is_active())
    {
        _instance.deactivate();
    }
}

ProcessorReturnCode ClapWrapper::init(float sample_rate)
{
    _sample_rate = sample_rate;
    auto abs_path = _host_control.to_absolute_path(_plugin_load_path);

    if (!_instance.load_plugin(abs_path, _plugin_index, _host_context->host()))
    {
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    auto* desc = _instance.descriptor();
    if (desc)
    {
        set_name(desc->name ? desc->name : "");
        set_label(desc->name ? desc->name : "");
    }

    if (!_setup_audio_ports())
    {
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    if (!_instance.activate(sample_rate, 1, AUDIO_CHUNK_SIZE))
    {
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    if (!_register_parameters())
    {
        return ProcessorReturnCode::PARAMETER_ERROR;
    }

    return ProcessorReturnCode::OK;
}

void ClapWrapper::configure(float sample_rate)
{
    _sample_rate = sample_rate;
    bool was_enabled = enabled();
    if (was_enabled)
    {
        set_enabled(false);
    }

    _instance.deactivate();
    if (!_instance.activate(sample_rate, 1, AUDIO_CHUNK_SIZE))
    {
        ELKLOG_LOG_ERROR("Error reconfiguring CLAP plugin to sample rate {}", sample_rate);
    }

    if (was_enabled)
    {
        set_enabled(true);
    }
}

void ClapWrapper::process_event(const RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::FLOAT_PARAMETER_CHANGE:
        {
            auto typed_event = event.parameter_change_event();
            auto sushi_id = typed_event->param_id();
            auto it = _sushi_to_clap_param.find(sushi_id);
            if (it != _sushi_to_clap_param.end())
            {
                auto clap_id = it->second;
                auto val_it = _param_values.find(clap_id);
                // The value coming from Sushi is normalized (0-1), we need to convert to
                // the plugin's plain domain
                auto param_desc = parameter_from_id(sushi_id);
                double plain_value = typed_event->value();
                if (param_desc)
                {
                    plain_value = param_desc->min_domain_value() +
                                  typed_event->value() * (param_desc->max_domain_value() - param_desc->min_domain_value());
                }

                clap_event_param_value_t pv{};
                pv.header.size = sizeof(pv);
                pv.header.time = static_cast<uint32_t>(typed_event->sample_offset());
                pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                pv.header.type = CLAP_EVENT_PARAM_VALUE;
                pv.header.flags = 0;
                pv.param_id = clap_id;
                pv.cookie = nullptr;
                pv.note_id = -1;
                pv.port_index = -1;
                pv.channel = -1;
                pv.key = -1;
                pv.value = plain_value;
                _in_events.push(&pv.header);

                if (val_it != _param_values.end())
                {
                    val_it->second = plain_value;
                }
            }
            break;
        }
        case RtEventType::NOTE_ON:
        {
            auto kbd = event.keyboard_event();
            clap_event_note_t note{};
            note.header.size = sizeof(note);
            note.header.time = static_cast<uint32_t>(kbd->sample_offset());
            note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            note.header.type = CLAP_EVENT_NOTE_ON;
            note.header.flags = 0;
            note.note_id = -1;
            note.port_index = 0;
            note.channel = static_cast<int16_t>(kbd->channel());
            note.key = static_cast<int16_t>(kbd->note());
            note.velocity = kbd->velocity();
            _in_events.push(&note.header);
            break;
        }
        case RtEventType::NOTE_OFF:
        {
            auto kbd = event.keyboard_event();
            clap_event_note_t note{};
            note.header.size = sizeof(note);
            note.header.time = static_cast<uint32_t>(kbd->sample_offset());
            note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            note.header.type = CLAP_EVENT_NOTE_OFF;
            note.header.flags = 0;
            note.note_id = -1;
            note.port_index = 0;
            note.channel = static_cast<int16_t>(kbd->channel());
            note.key = static_cast<int16_t>(kbd->note());
            note.velocity = kbd->velocity();
            _in_events.push(&note.header);
            break;
        }
        case RtEventType::NOTE_AFTERTOUCH:
        {
            auto kbd = event.keyboard_event();
            clap_event_note_expression_t expr{};
            expr.header.size = sizeof(expr);
            expr.header.time = static_cast<uint32_t>(kbd->sample_offset());
            expr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            expr.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            expr.header.flags = 0;
            expr.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
            expr.note_id = -1;
            expr.port_index = 0;
            expr.channel = static_cast<int16_t>(kbd->channel());
            expr.key = static_cast<int16_t>(kbd->note());
            expr.value = kbd->velocity();
            _in_events.push(&expr.header);
            break;
        }
        case RtEventType::PITCH_BEND:
        {
            auto kbd = event.keyboard_common_event();
            clap_event_note_expression_t expr{};
            expr.header.size = sizeof(expr);
            expr.header.time = static_cast<uint32_t>(kbd->sample_offset());
            expr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            expr.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            expr.header.flags = 0;
            expr.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
            expr.note_id = -1;
            expr.port_index = -1;
            expr.channel = static_cast<int16_t>(kbd->channel());
            expr.key = -1;
            // Sushi pitch bend is -1 to +1, CLAP tuning is in semitones (-120 to 120)
            // Standard pitch bend range is 2 semitones
            expr.value = kbd->value() * 2.0;
            _in_events.push(&expr.header);
            break;
        }
        case RtEventType::MODULATION:
        {
            auto kbd = event.keyboard_common_event();
            clap_event_note_expression_t expr{};
            expr.header.size = sizeof(expr);
            expr.header.time = static_cast<uint32_t>(kbd->sample_offset());
            expr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            expr.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            expr.header.flags = 0;
            expr.expression_id = CLAP_NOTE_EXPRESSION_BRIGHTNESS;
            expr.note_id = -1;
            expr.port_index = -1;
            expr.channel = static_cast<int16_t>(kbd->channel());
            expr.key = -1;
            expr.value = kbd->value();
            _in_events.push(&expr.header);
            break;
        }
        case RtEventType::WRAPPED_MIDI_EVENT:
        {
            auto midi_ev = event.wrapped_midi_event();
            clap_event_midi_t midi{};
            midi.header.size = sizeof(midi);
            midi.header.time = static_cast<uint32_t>(midi_ev->sample_offset());
            midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            midi.header.type = CLAP_EVENT_MIDI;
            midi.header.flags = 0;
            midi.port_index = 0;
            auto data = midi_ev->midi_data();
            midi.data[0] = data[0];
            midi.data[1] = data[1];
            midi.data[2] = data[2];
            _in_events.push(&midi.header);
            break;
        }
        case RtEventType::SET_BYPASS:
        {
            bool bypass = static_cast<bool>(event.processor_command_event()->value());
            _bypass_manager.set_bypass(bypass, _sample_rate);

            if (_has_bypass_parameter)
            {
                clap_event_param_value_t pv{};
                pv.header.size = sizeof(pv);
                pv.header.time = 0;
                pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                pv.header.type = CLAP_EVENT_PARAM_VALUE;
                pv.header.flags = 0;
                pv.param_id = _bypass_param_id;
                pv.cookie = nullptr;
                pv.note_id = -1;
                pv.port_index = -1;
                pv.channel = -1;
                pv.key = -1;
                pv.value = bypass ? 1.0 : 0.0;
                _in_events.push(&pv.header);
            }
            break;
        }
        default:
            break;
    }
}

void ClapWrapper::process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer)
{
    if (!_has_bypass_parameter && !_bypass_manager.should_process())
    {
        bypass_process(in_buffer, out_buffer);
        _in_events.clear();
        _out_events.clear();
        return;
    }

    // Setup audio buffers
    for (int ch = 0; ch < _current_input_channels; ++ch)
    {
        _input_channel_ptrs[ch] = const_cast<float*>(in_buffer.channel(ch));
    }
    for (int ch = 0; ch < _current_output_channels; ++ch)
    {
        _output_channel_ptrs[ch] = out_buffer.channel(ch);
    }

    _clap_input_buffer.data32 = _input_channel_ptrs;
    _clap_input_buffer.data64 = nullptr;
    _clap_input_buffer.channel_count = static_cast<uint32_t>(_current_input_channels);
    _clap_input_buffer.latency = 0;
    _clap_input_buffer.constant_mask = 0;

    _clap_output_buffer.data32 = _output_channel_ptrs;
    _clap_output_buffer.data64 = nullptr;
    _clap_output_buffer.channel_count = static_cast<uint32_t>(_current_output_channels);
    _clap_output_buffer.latency = 0;
    _clap_output_buffer.constant_mask = 0;

    // Setup transport
    clap_event_transport_t transport{};
    _fill_transport(transport);

    // Setup process struct
    clap_process_t process{};
    process.steady_time = -1;
    process.frames_count = AUDIO_CHUNK_SIZE;
    process.transport = &transport;
    process.audio_inputs = &_clap_input_buffer;
    process.audio_outputs = &_clap_output_buffer;
    process.audio_inputs_count = (_current_input_channels > 0) ? 1 : 0;
    process.audio_outputs_count = (_current_output_channels > 0) ? 1 : 0;
    process.in_events = _in_events.input_events();
    process.out_events = _out_events.output_events();

    _instance.process(&process);

    if (!_has_bypass_parameter && _bypass_manager.should_ramp())
    {
        _bypass_manager.crossfade_output(in_buffer, out_buffer, _current_input_channels, _current_output_channels);
    }

    _forward_output_events();

    _in_events.clear();
    _out_events.clear();
}

void ClapWrapper::set_enabled(bool enabled)
{
    if (enabled == _enabled)
    {
        return;
    }

    if (enabled)
    {
        if (!_instance.is_active())
        {
            _instance.activate(_sample_rate, 1, AUDIO_CHUNK_SIZE);
        }
        _instance.start_processing();
    }
    else
    {
        _instance.stop_processing();
    }
    Processor::set_enabled(enabled);
}

void ClapWrapper::set_bypassed(bool bypassed)
{
    assert(twine::is_current_thread_realtime() == false);
    if (_has_bypass_parameter)
    {
        // Find the sushi param id for the bypass parameter
        auto it = _clap_to_sushi_param.find(_bypass_param_id);
        if (it != _clap_to_sushi_param.end())
        {
            _host_control.post_event(std::make_unique<ParameterChangeEvent>(
                ParameterChangeEvent::Subtype::FLOAT_PARAMETER_CHANGE,
                this->id(),
                it->second,
                bypassed ? 1.0f : 0.0f,
                IMMEDIATE_PROCESS));
        }
        _bypass_manager.set_bypass(bypassed, _sample_rate);
    }
    else
    {
        _host_control.post_event(std::make_unique<SetProcessorBypassEvent>(
            this->id(), bypassed, IMMEDIATE_PROCESS));
    }
}

bool ClapWrapper::bypassed() const
{
    if (_has_bypass_parameter)
    {
        auto it = _param_values.find(_bypass_param_id);
        if (it != _param_values.end())
        {
            return it->second > 0.5;
        }
    }
    return _bypass_manager.bypassed();
}

std::pair<ProcessorReturnCode, float> ClapWrapper::parameter_value(ObjectId parameter_id) const
{
    auto it = _sushi_to_clap_param.find(parameter_id);
    if (it == _sushi_to_clap_param.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }
    auto clap_id = it->second;

    // Try to get the value directly from the plugin if params extension is available
    if (_instance.params())
    {
        double value = 0.0;
        if (_instance.params()->get_value(_instance.plugin(), clap_id, &value))
        {
            // Normalize to 0-1
            auto param_desc = parameter_from_id(parameter_id);
            if (param_desc)
            {
                float range = param_desc->max_domain_value() - param_desc->min_domain_value();
                if (range > 0.0f)
                {
                    float normalized = static_cast<float>((value - param_desc->min_domain_value()) / range);
                    return {ProcessorReturnCode::OK, normalized};
                }
            }
            return {ProcessorReturnCode::OK, static_cast<float>(value)};
        }
    }

    // Fallback to cached value
    auto val_it = _param_values.find(clap_id);
    if (val_it != _param_values.end())
    {
        auto param_desc = parameter_from_id(parameter_id);
        if (param_desc)
        {
            float range = param_desc->max_domain_value() - param_desc->min_domain_value();
            if (range > 0.0f)
            {
                float normalized = static_cast<float>((val_it->second - param_desc->min_domain_value()) / range);
                return {ProcessorReturnCode::OK, normalized};
            }
        }
        return {ProcessorReturnCode::OK, static_cast<float>(val_it->second)};
    }
    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
}

std::pair<ProcessorReturnCode, float> ClapWrapper::parameter_value_in_domain(ObjectId parameter_id) const
{
    auto it = _sushi_to_clap_param.find(parameter_id);
    if (it == _sushi_to_clap_param.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }

    if (_instance.params())
    {
        double value = 0.0;
        if (_instance.params()->get_value(_instance.plugin(), it->second, &value))
        {
            return {ProcessorReturnCode::OK, static_cast<float>(value)};
        }
    }

    auto val_it = _param_values.find(it->second);
    if (val_it != _param_values.end())
    {
        return {ProcessorReturnCode::OK, static_cast<float>(val_it->second)};
    }
    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
}

std::pair<ProcessorReturnCode, std::string> ClapWrapper::parameter_value_formatted(ObjectId parameter_id) const
{
    auto it = _sushi_to_clap_param.find(parameter_id);
    if (it == _sushi_to_clap_param.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
    }

    if (_instance.params())
    {
        double value = 0.0;
        if (_instance.params()->get_value(_instance.plugin(), it->second, &value))
        {
            char buffer[CLAP_NAME_BUFFER_SIZE] = {};
            if (_instance.params()->value_to_text(_instance.plugin(), it->second, value, buffer, sizeof(buffer)))
            {
                return {ProcessorReturnCode::OK, std::string(buffer)};
            }
            return {ProcessorReturnCode::OK, std::to_string(value)};
        }
    }
    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
}

ProcessorReturnCode ClapWrapper::set_state(ProcessorState* state, bool /*realtime_running*/)
{
    if (state->has_binary_data() && _instance.state())
    {
        struct ReadCtx
        {
            const std::byte* data;
            size_t size;
            size_t pos;
        };

        auto& binary_data = state->binary_data();
        ReadCtx ctx{binary_data.data(), binary_data.size(), 0};

        clap_istream_t istream{};
        istream.ctx = &ctx;
        istream.read = [](const clap_istream_t* stream, void* buffer, uint64_t size) -> int64_t {
            auto* c = static_cast<ReadCtx*>(stream->ctx);
            auto remaining = c->size - c->pos;
            auto to_read = std::min(static_cast<size_t>(size), remaining);
            if (to_read == 0) return 0;
            memcpy(buffer, c->data + c->pos, to_read);
            c->pos += to_read;
            return static_cast<int64_t>(to_read);
        };

        if (_instance.state()->load(_instance.plugin(), &istream))
        {
            return ProcessorReturnCode::OK;
        }
        return ProcessorReturnCode::ERROR;
    }

    // Apply parameter values
    for (const auto& param : state->parameters())
    {
        auto sushi_id = param.first;
        float value = param.second;
        auto it = _sushi_to_clap_param.find(sushi_id);
        if (it != _sushi_to_clap_param.end())
        {
            auto param_desc = parameter_from_id(sushi_id);
            if (param_desc)
            {
                double plain = param_desc->min_domain_value() +
                               value * (param_desc->max_domain_value() - param_desc->min_domain_value());
                _param_values[it->second] = plain;
            }
        }
    }

    if (state->bypassed().has_value())
    {
        set_bypassed(*state->bypassed());
    }

    return ProcessorReturnCode::OK;
}

ProcessorState ClapWrapper::save_state() const
{
    ProcessorState state;

    if (_instance.state())
    {
        struct WriteCtx
        {
            std::vector<std::byte> data;
        };

        WriteCtx ctx;
        ctx.data.reserve(4096);

        clap_ostream_t ostream{};
        ostream.ctx = &ctx;
        ostream.write = [](const clap_ostream_t* stream, const void* buffer, uint64_t size) -> int64_t {
            auto* c = static_cast<WriteCtx*>(stream->ctx);
            auto* bytes = static_cast<const std::byte*>(buffer);
            c->data.insert(c->data.end(), bytes, bytes + size);
            return static_cast<int64_t>(size);
        };

        if (_instance.state()->save(_instance.plugin(), &ostream))
        {
            state.set_binary_data(std::move(ctx.data));
        }
        else
        {
            ELKLOG_LOG_WARNING("Failed to save CLAP plugin state");
        }
    }

    return state;
}

bool ClapWrapper::has_editor() const
{
    return _instance.gui() != nullptr;
}

PluginInfo ClapWrapper::info() const
{
    PluginInfo plugin_info;
    plugin_info.type = PluginType::CLAP;
    plugin_info.path = _plugin_load_path;
    if (_instance.descriptor() && _instance.descriptor()->id)
    {
        plugin_info.uid = _instance.descriptor()->id;
    }
    return plugin_info;
}

bool ClapWrapper::_register_parameters()
{
    if (!_instance.params())
    {
        ELKLOG_LOG_INFO("Plugin does not support params extension, no parameters to register");
        return true;
    }

    auto param_count = _instance.params()->count(_instance.plugin());
    ELKLOG_LOG_INFO("Plugin has {} parameters", param_count);

    for (uint32_t i = 0; i < param_count; ++i)
    {
        clap_param_info_t param_info{};
        if (!_instance.params()->get_info(_instance.plugin(), i, &param_info))
        {
            ELKLOG_LOG_WARNING("Failed to get info for parameter {}", i);
            continue;
        }

        if (param_info.flags & CLAP_PARAM_IS_HIDDEN)
        {
            continue;
        }

        auto param_name = std::string(param_info.name);
        auto unique_name = _make_unique_parameter_name(param_name);
        bool automatable = (param_info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
        auto direction = automatable ? Direction::AUTOMATABLE : Direction::OUTPUT;

        // Cache the default value
        double default_value = param_info.default_value;
        _param_values[param_info.id] = default_value;

        // Check for bypass parameter
        if (param_info.flags & CLAP_PARAM_IS_BYPASS)
        {
            _has_bypass_parameter = true;
            _bypass_param_id = param_info.id;
            ELKLOG_LOG_INFO("Plugin supports soft bypass via parameter {}", param_name);
        }

        ParameterDescriptor* descriptor = nullptr;
        if (param_info.flags & CLAP_PARAM_IS_STEPPED)
        {
            int min = static_cast<int>(param_info.min_value);
            int max = static_cast<int>(param_info.max_value);
            descriptor = new IntParameterDescriptor(unique_name,
                                                    param_name,
                                                    "",
                                                    min,
                                                    max,
                                                    direction,
                                                    nullptr);
        }
        else
        {
            descriptor = new FloatParameterDescriptor(unique_name,
                                                      param_name,
                                                      "",
                                                      static_cast<float>(param_info.min_value),
                                                      static_cast<float>(param_info.max_value),
                                                      direction,
                                                      nullptr);
        }

        auto sushi_id = static_cast<ObjectId>(this->all_parameters().size());
        if (register_parameter(descriptor, sushi_id))
        {
            _clap_to_sushi_param[param_info.id] = sushi_id;
            _sushi_to_clap_param[sushi_id] = param_info.id;
            ELKLOG_LOG_INFO("Registered parameter {} (clap_id: {}, sushi_id: {})", param_name, param_info.id, sushi_id);
        }
        else
        {
            ELKLOG_LOG_WARNING("Failed to register parameter {}", param_name);
            delete descriptor;
        }
    }

    return true;
}

bool ClapWrapper::_setup_audio_ports()
{
    _max_input_channels = 0;
    _max_output_channels = 0;

    if (_instance.audio_ports())
    {
        auto input_count = _instance.audio_ports()->count(_instance.plugin(), true);
        auto output_count = _instance.audio_ports()->count(_instance.plugin(), false);
        ELKLOG_LOG_INFO("Plugin has {} audio input ports and {} audio output ports", input_count, output_count);

        // Use the first (main) input port
        if (input_count > 0)
        {
            clap_audio_port_info_t info{};
            if (_instance.audio_ports()->get(_instance.plugin(), 0, true, &info))
            {
                _max_input_channels = std::min(static_cast<int>(info.channel_count), CLAP_WRAPPER_MAX_N_CHANNELS);
                ELKLOG_LOG_INFO("Main input port: {} channels ({})", info.channel_count, info.name);
            }
        }

        // Use the first (main) output port
        if (output_count > 0)
        {
            clap_audio_port_info_t info{};
            if (_instance.audio_ports()->get(_instance.plugin(), 0, false, &info))
            {
                _max_output_channels = std::min(static_cast<int>(info.channel_count), CLAP_WRAPPER_MAX_N_CHANNELS);
                ELKLOG_LOG_INFO("Main output port: {} channels ({})", info.channel_count, info.name);
            }
        }
    }
    else
    {
        // Default to stereo if no audio ports extension
        _max_input_channels = 2;
        _max_output_channels = 2;
        ELKLOG_LOG_INFO("Plugin does not support audio ports extension, defaulting to stereo");
    }

    if (_max_output_channels == 0)
    {
        ELKLOG_LOG_ERROR("Plugin has no audio output channels");
        return false;
    }

    return true;
}

void ClapWrapper::_fill_transport(clap_event_transport_t& transport)
{
    transport.header.size = sizeof(transport);
    transport.header.time = 0;
    transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    transport.header.type = CLAP_EVENT_TRANSPORT;
    transport.header.flags = 0;

    auto host_transport = _host_control.transport();
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;

    if (host_transport->playing())
    {
        transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
    }

    transport.tempo = host_transport->current_tempo();
    transport.tempo_inc = 0;

    auto time_sig = host_transport->time_signature();
    transport.tsig_num = static_cast<uint16_t>(time_sig.numerator);
    transport.tsig_denom = static_cast<uint16_t>(time_sig.denominator);

    transport.song_pos_beats = static_cast<int64_t>(host_transport->current_bar_beats() * CLAP_BEATTIME_FACTOR);
    transport.song_pos_seconds = 0;

    transport.bar_start = static_cast<int64_t>(host_transport->current_bar_start_beats() * CLAP_BEATTIME_FACTOR);
    transport.bar_number = 0;

    transport.loop_start_beats = 0;
    transport.loop_end_beats = 0;
    transport.loop_start_seconds = 0;
    transport.loop_end_seconds = 0;
}

void ClapWrapper::_forward_output_events()
{
    for (uint32_t i = 0; i < _out_events.size(); ++i)
    {
        auto* header = _out_events.get(i);
        if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
        {
            continue;
        }

        switch (header->type)
        {
            case CLAP_EVENT_NOTE_ON:
            {
                auto* note = reinterpret_cast<const clap_event_note_t*>(header);
                auto rt_event = RtEvent::make_note_on_event(this->id(),
                                                            header->time,
                                                            note->channel,
                                                            note->key,
                                                            static_cast<float>(note->velocity));
                output_event(rt_event);
                break;
            }
            case CLAP_EVENT_NOTE_OFF:
            {
                auto* note = reinterpret_cast<const clap_event_note_t*>(header);
                auto rt_event = RtEvent::make_note_off_event(this->id(),
                                                             header->time,
                                                             note->channel,
                                                             note->key,
                                                             static_cast<float>(note->velocity));
                output_event(rt_event);
                break;
            }
            case CLAP_EVENT_PARAM_VALUE:
            {
                auto* pv = reinterpret_cast<const clap_event_param_value_t*>(header);
                auto it = _clap_to_sushi_param.find(pv->param_id);
                if (it != _clap_to_sushi_param.end())
                {
                    _param_values[pv->param_id] = pv->value;
                    maybe_output_cv_value(it->second, static_cast<float>(pv->value));
                }
                break;
            }
            case CLAP_EVENT_MIDI:
            {
                auto* midi = reinterpret_cast<const clap_event_midi_t*>(header);
                MidiDataByte data{{midi->data[0], midi->data[1], midi->data[2], 0}};
                output_midi_event_as_internal(data, static_cast<int>(header->time));
                break;
            }
            default:
                break;
        }
    }
}

} // end namespace sushi::internal::clap_wrapper
