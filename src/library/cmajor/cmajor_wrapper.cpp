/*
 * Copyright 2026 Oliver Larkin
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

#include "cmajor_wrapper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_set>

#include "elklog/static_logger.h"

#include "library/event.h"
#include "library/midi_encoder.h"

#include "cmajor/API/cmaj_BuildSettings.h"
#include "cmajor/API/cmaj_DiagnosticMessages.h"
#include "cmajor/API/cmaj_Engine.h"
#include "cmajor/API/cmaj_Program.h"
#include "cmajor/helpers/cmaj_AudioMIDIPerformer.h"
#include "choc/audio/choc_MIDI.h"
#include "choc/audio/choc_SampleBuffers.h"
#include "choc/text/choc_Files.h"
#include "choc/text/choc_JSON.h"

namespace sushi::internal::cmajor_plugin {

namespace {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("cmajor");

constexpr cmaj::EndpointHandle INVALID_ENDPOINT_HANDLE = std::numeric_limits<cmaj::EndpointHandle>::max();
constexpr uint32_t EVENT_FIFO_SIZE = 4096;

bool is_valid_handle(cmaj::EndpointHandle handle)
{
    return handle != INVALID_ENDPOINT_HANDLE;
}

bool is_supported_parameter_endpoint(const cmaj::EndpointDetails& endpoint)
{
    if (!endpoint.isInput || !endpoint.isParameter() || endpoint.dataTypes.size() != 1)
    {
        return false;
    }

    const auto& type = endpoint.dataTypes.front();
    return type.isBool() || type.isInt() || type.isFloat();
}

bool is_audio_endpoint(const cmaj::EndpointDetails& endpoint)
{
    return endpoint.getNumAudioChannels() > 0;
}

std::string endpoint_signature(const cmaj::EndpointDetails& endpoint)
{
    return endpoint.endpointID.toString() + "|" +
           (endpoint.isEvent() ? "event" : "value") + "|" +
           endpoint.dataTypes.front().getDescription();
}

std::string make_unique_name(const std::string& endpoint_id, std::unordered_set<std::string>& used_names)
{
    auto base_name = cmaj::makeSafeIdentifierName(endpoint_id);
    if (base_name.empty())
    {
        base_name = "parameter";
    }

    auto candidate = base_name;
    int suffix = 2;
    while (used_names.count(candidate) > 0)
    {
        candidate = base_name + "_" + std::to_string(suffix++);
    }

    used_names.insert(candidate);
    return candidate;
}

float normalized_default_value(const cmaj::PatchParameterProperties& properties, const choc::value::Type& value_type)
{
    if (value_type.isBool())
    {
        return properties.defaultValue >= 0.5f ? 1.0f : 0.0f;
    }

    if (value_type.isInt())
    {
        auto rounded = std::round(properties.defaultValue);
        return properties.convertTo0to1(rounded);
    }

    return properties.convertTo0to1(properties.snapAndConstrainValue(properties.defaultValue));
}

float domain_from_normalized(const cmaj::PatchParameterProperties& properties,
                             const choc::value::Type& value_type,
                             float normalized_value)
{
    normalized_value = std::clamp(normalized_value, 0.0f, 1.0f);

    if (value_type.isBool())
    {
        return normalized_value >= 0.5f ? 1.0f : 0.0f;
    }

    auto domain_value = properties.convertFrom0to1(normalized_value);

    if (value_type.isInt())
    {
        return std::round(properties.snapAndConstrainValue(domain_value));
    }

    return properties.snapAndConstrainValue(domain_value);
}

std::string formatted_parameter_value(const cmaj::PatchParameterProperties& properties,
                                      const choc::value::Type& value_type,
                                      float domain_value)
{
    if (value_type.isBool())
    {
        return domain_value >= 0.5f ? "True" : "False";
    }

    return properties.getValueAsString(domain_value);
}

std::unique_ptr<ParameterDescriptor> make_descriptor(ObjectId id,
                                                     const std::string& name,
                                                     const cmaj::PatchParameterProperties& properties,
                                                     const choc::value::Type& value_type)
{
    std::unique_ptr<ParameterDescriptor> descriptor;
    auto direction = properties.automatable ? Direction::AUTOMATABLE : Direction::OUTPUT;

    if (value_type.isBool())
    {
        descriptor = std::make_unique<BoolParameterDescriptor>(name,
                                                               properties.name,
                                                               properties.unit,
                                                               false,
                                                               true,
                                                               direction,
                                                               nullptr);
    }
    else if (value_type.isInt())
    {
        descriptor = std::make_unique<IntParameterDescriptor>(name,
                                                              properties.name,
                                                              properties.unit,
                                                              static_cast<int>(std::round(properties.minValue)),
                                                              static_cast<int>(std::round(properties.maxValue)),
                                                              direction,
                                                              new IntParameterPreProcessor(static_cast<int>(std::round(properties.minValue)),
                                                                                           static_cast<int>(std::round(properties.maxValue))));
    }
    else
    {
        descriptor = std::make_unique<FloatParameterDescriptor>(name,
                                                                properties.name,
                                                                properties.unit,
                                                                properties.minValue,
                                                                properties.maxValue,
                                                                direction,
                                                                new FloatParameterPreProcessor(properties.minValue, properties.maxValue));
    }

    descriptor->set_id(id);
    return descriptor;
}

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

std::string build_log_string(const cmaj::Engine& engine, const cmaj::DiagnosticMessageList& messages)
{
    auto build_log = engine.getLastBuildLog();
    auto message_log = messages.toString();

    if (!build_log.empty() && !message_log.empty())
    {
        return build_log + "\n" + message_log;
    }

    return build_log.empty() ? message_log : build_log;
}

} // namespace

struct CmajorWrapper::Runtime
{
    struct EndpointBinding
    {
        cmaj::EndpointHandle handle = INVALID_ENDPOINT_HANDLE;
        bool is_event = false;
    };

    struct PendingParameterBinding
    {
        PendingParameterBinding(std::string endpoint_id_,
                                std::string signature_,
                                cmaj::PatchParameterProperties properties_,
                                choc::value::Type value_type_,
                                cmaj::EndpointHandle handle_,
                                bool is_event_)
            : endpoint_id(std::move(endpoint_id_)),
              signature(std::move(signature_)),
              properties(std::move(properties_)),
              value_type(std::move(value_type_)),
              handle(handle_),
              is_event(is_event_)
        {
        }

        std::string endpoint_id;
        std::string signature;
        cmaj::PatchParameterProperties properties;
        choc::value::Type value_type;
        cmaj::EndpointHandle handle = INVALID_ENDPOINT_HANDLE;
        bool is_event = false;
    };

    struct InputParameterBinding
    {
        cmaj::EndpointHandle handle = INVALID_ENDPOINT_HANDLE;
        choc::value::Type value_type;
        uint32_t ramp_frames = 0;
        bool is_event = false;
    };

    cmaj::Engine engine;
    std::unique_ptr<cmaj::AudioMIDIPerformer> performer;
    cmaj::EndpointDetailsList input_endpoints;
    cmaj::EndpointDetailsList output_endpoints;
    std::vector<PendingParameterBinding> pending_parameter_bindings;
    std::unordered_map<ObjectId, InputParameterBinding> input_parameter_bindings;
    EndpointBinding tempo_input;
    EndpointBinding time_signature_input;
    EndpointBinding transport_state_input;
    EndpointBinding position_input;
    cmaj::TimelineEventGenerator timeline_events;
    int max_input_channels = 0;
    int max_output_channels = 0;
};

CmajorWrapper::CmajorWrapper(HostControl host_control, PluginInfo plugin_info)
    : Processor(host_control), _plugin_info(std::move(plugin_info))
{
    _max_input_channels = MAX_TRACK_CHANNELS;
    _max_output_channels = MAX_TRACK_CHANNELS;
    _source_path = _plugin_info.path;
    _source_code = _plugin_info.source_code;
    _register_properties();
}

CmajorWrapper::~CmajorWrapper() = default;

ProcessorReturnCode CmajorWrapper::init(float sample_rate)
{
    _sample_rate = sample_rate;

    if (!_source_path.empty() || !_source_code.empty())
    {
        return _compile_current_source(true);
    }

    std::scoped_lock<std::mutex> lock(_state_lock);
    _set_compile_feedback_locked(CompileStatus::EMPTY, "", "", false);
    return ProcessorReturnCode::OK;
}

void CmajorWrapper::configure(float sample_rate)
{
    _sample_rate = sample_rate;
    _compile_current_source(false);
}

void CmajorWrapper::process_event(const RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::FLOAT_PARAMETER_CHANGE:
        case RtEventType::INT_PARAMETER_CHANGE:
        case RtEventType::BOOL_PARAMETER_CHANGE:
        {
            auto typed_event = event.parameter_change_event();

            ParameterState* parameter = nullptr;
            {
                std::scoped_lock<std::mutex> lock(_state_lock);
                auto it = _parameters_by_id.find(typed_event->param_id());
                if (it != _parameters_by_id.end() && it->second->descriptor->automatable())
                {
                    parameter = it->second.get();
                }
            }

            if (parameter == nullptr)
            {
                break;
            }

            auto normalized_value = std::clamp(typed_event->value(), 0.0f, 1.0f);
            auto domain_value = domain_from_normalized(parameter->properties, parameter->value_type, normalized_value);

            parameter->normalized_value.store(normalized_value);
            parameter->domain_value.store(domain_value);

            if (auto runtime = _load_runtime())
            {
                _apply_parameter_value_to_runtime(*runtime, typed_event->param_id(), normalized_value);
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
                ELKLOG_LOG_WARNING("Cmajor MIDI queue overflow on processor {}", this->name());
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

void CmajorWrapper::process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer)
{
    auto runtime = _load_runtime();

    if (this->bypassed())
    {
        this->bypass_process(in_buffer, out_buffer);
        _pending_midi_count = 0;
        return;
    }

    if (!runtime || !runtime->performer)
    {
        out_buffer.clear();
        _pending_midi_count = 0;
        return;
    }

    const auto* transport = _host_control.transport();
    if (transport)
    {
        if (is_valid_handle(runtime->time_signature_input.handle))
        {
            auto signature = transport->time_signature();
            auto value = runtime->timeline_events.getTimeSigEvent(signature.numerator, signature.denominator);
            if (runtime->time_signature_input.is_event)
            {
                runtime->performer->postEvent(runtime->time_signature_input.handle, value, 0);
            }
            else
            {
                runtime->performer->postValue(runtime->time_signature_input.handle, value, 0, 0);
            }
        }

        if (is_valid_handle(runtime->tempo_input.handle))
        {
            auto value = runtime->timeline_events.getBPMEvent(transport->current_tempo());
            if (runtime->tempo_input.is_event)
            {
                runtime->performer->postEvent(runtime->tempo_input.handle, value, 0);
            }
            else
            {
                runtime->performer->postValue(runtime->tempo_input.handle, value, 0, 0);
            }
        }

        if (is_valid_handle(runtime->transport_state_input.handle))
        {
            auto mode = transport->playing_mode();
            auto value = runtime->timeline_events.getTransportStateEvent(mode == PlayingMode::RECORDING,
                                                                         transport->playing(),
                                                                         false);
            if (runtime->transport_state_input.is_event)
            {
                runtime->performer->postEvent(runtime->transport_state_input.handle, value, 0);
            }
            else
            {
                runtime->performer->postValue(runtime->transport_state_input.handle, value, 0, 0);
            }
        }

        if (is_valid_handle(runtime->position_input.handle))
        {
            auto value = runtime->timeline_events.getPositionEvent(transport->current_samples(),
                                                                   transport->current_beats(),
                                                                   transport->current_bar_start_beats());
            if (runtime->position_input.is_event)
            {
                runtime->performer->postEvent(runtime->position_input.handle, value, 0);
            }
            else
            {
                runtime->performer->postValue(runtime->position_input.handle, value, 0, 0);
            }
        }
    }

    std::array<const float*, MAX_TRACK_CHANNELS> input_channels {};
    std::array<float*, MAX_TRACK_CHANNELS> output_channels {};

    for (int channel = 0; channel < _current_input_channels; ++channel)
    {
        input_channels[static_cast<size_t>(channel)] = in_buffer.channel(channel);
    }

    for (int channel = 0; channel < _current_output_channels; ++channel)
    {
        output_channels[static_cast<size_t>(channel)] = out_buffer.channel(channel);
    }

    std::array<choc::midi::ShortMessage, MAX_PENDING_MIDI_EVENTS> midi_message_storage {};
    std::array<choc::audio::AudioMIDIBlockDispatcher::MIDIMessage, MAX_PENDING_MIDI_EVENTS> midi_messages {};
    std::array<int, MAX_PENDING_MIDI_EVENTS> midi_times {};

    for (size_t i = 0; i < _pending_midi_count; ++i)
    {
        const auto& midi_data = _pending_midi_messages[i];
        midi_message_storage[i] = choc::midi::ShortMessage(midi_data[0], midi_data[1], midi_data[2]);
        midi_messages[i].time = {};
        midi_messages[i].sourceDeviceID = nullptr;
        midi_messages[i].message = midi_message_storage[i];
        midi_times[i] = _pending_midi_offsets[i];
    }

    auto input_view = choc::buffer::createChannelArrayView(input_channels.data(),
                                                           static_cast<uint32_t>(_current_input_channels),
                                                           static_cast<uint32_t>(AUDIO_CHUNK_SIZE));
    auto output_view = choc::buffer::createChannelArrayView(output_channels.data(),
                                                            static_cast<uint32_t>(_current_output_channels),
                                                            static_cast<uint32_t>(AUDIO_CHUNK_SIZE));

    runtime->performer->processWithTimeStampedMIDI(input_view,
                                                   output_view,
                                                   midi_messages.data(),
                                                   midi_times.data(),
                                                   static_cast<uint32_t>(_pending_midi_count),
                                                   [this](uint32_t frame, const choc::midi::ShortMessage& message)
                                                   {
                                                       MidiDataByte midi_data {
                                                           message.data()[0],
                                                           message.data()[1],
                                                           message.data()[2],
                                                           0
                                                       };
                                                       this->output_midi_event_as_internal(midi_data, static_cast<int>(frame));
                                                   },
                                                   true);

    _pending_midi_count = 0;
}

void CmajorWrapper::set_channels(int inputs, int outputs)
{
    _requested_input_channels = inputs;
    _requested_output_channels = outputs;

    _current_input_channels = std::min(inputs, _max_input_channels);
    _current_output_channels = std::min(outputs, _max_output_channels);

    if (!_source_path.empty() || !_source_code.empty() || _load_runtime())
    {
        _compile_current_source(false);
    }
}

int CmajorWrapper::parameter_count() const
{
    std::scoped_lock<std::mutex> lock(_state_lock);
    return static_cast<int>(_active_parameter_ids.size() + _properties.size());
}

const ParameterDescriptor* CmajorWrapper::parameter_from_name(const std::string& name) const
{
    std::scoped_lock<std::mutex> lock(_state_lock);

    auto parameter_it = _parameter_ids_by_name.find(name);
    if (parameter_it != _parameter_ids_by_name.end())
    {
        auto state_it = _parameters_by_id.find(parameter_it->second);
        return state_it != _parameters_by_id.end() ? state_it->second->descriptor.get() : nullptr;
    }

    for (const auto& property : _properties)
    {
        if (property.second.descriptor->name() == name)
        {
            return property.second.descriptor.get();
        }
    }

    return nullptr;
}

const ParameterDescriptor* CmajorWrapper::parameter_from_id(ObjectId id) const
{
    std::scoped_lock<std::mutex> lock(_state_lock);

    auto parameter_it = _parameters_by_id.find(id);
    if (parameter_it != _parameters_by_id.end())
    {
        return parameter_it->second->descriptor.get();
    }

    auto property_it = _properties.find(id);
    return property_it != _properties.end() ? property_it->second.descriptor.get() : nullptr;
}

std::vector<ParameterDescriptor*> CmajorWrapper::all_parameters() const
{
    std::vector<ParameterDescriptor*> parameters;

    std::scoped_lock<std::mutex> lock(_state_lock);
    parameters.reserve(_active_parameter_ids.size() + _properties.size());

    for (auto id : _active_parameter_ids)
    {
        auto it = _parameters_by_id.find(id);
        if (it != _parameters_by_id.end())
        {
            parameters.push_back(it->second->descriptor.get());
        }
    }

    for (const auto& property : _properties)
    {
        parameters.push_back(property.second.descriptor.get());
    }

    return parameters;
}

std::pair<ProcessorReturnCode, float> CmajorWrapper::parameter_value(ObjectId parameter_id) const
{
    std::scoped_lock<std::mutex> lock(_state_lock);
    auto it = _parameters_by_id.find(parameter_id);
    if (it == _parameters_by_id.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }

    return {ProcessorReturnCode::OK, it->second->normalized_value.load()};
}

std::pair<ProcessorReturnCode, float> CmajorWrapper::parameter_value_in_domain(ObjectId parameter_id) const
{
    std::scoped_lock<std::mutex> lock(_state_lock);
    auto it = _parameters_by_id.find(parameter_id);
    if (it == _parameters_by_id.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }

    return {ProcessorReturnCode::OK, it->second->domain_value.load()};
}

std::pair<ProcessorReturnCode, std::string> CmajorWrapper::parameter_value_formatted(ObjectId parameter_id) const
{
    std::scoped_lock<std::mutex> lock(_state_lock);
    auto it = _parameters_by_id.find(parameter_id);
    if (it == _parameters_by_id.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
    }

    return {ProcessorReturnCode::OK,
            formatted_parameter_value(it->second->properties,
                                      it->second->value_type,
                                      it->second->domain_value.load())};
}

std::pair<ProcessorReturnCode, std::string> CmajorWrapper::property_value(ObjectId property_id) const
{
    std::scoped_lock<std::mutex> lock(_state_lock);
    auto it = _properties.find(property_id);
    if (it == _properties.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
    }

    return {ProcessorReturnCode::OK, it->second.value};
}

ProcessorReturnCode CmajorWrapper::set_property_value(ObjectId property_id, const std::string& value)
{
    if (property_id == COMPILE_STATUS_PROPERTY_ID ||
        property_id == BUILD_LOG_PROPERTY_ID ||
        property_id == PROGRAM_DETAILS_PROPERTY_ID)
    {
        return ProcessorReturnCode::UNSUPPORTED_OPERATION;
    }

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        auto property_it = _properties.find(property_id);
        if (property_it == _properties.end())
        {
            return ProcessorReturnCode::PARAMETER_NOT_FOUND;
        }

        if (property_id == SOURCE_PATH_PROPERTY_ID)
        {
            _source_path = value;
            _source_code.clear();
        }
        else if (property_id == SOURCE_CODE_PROPERTY_ID)
        {
            _source_code = value;
        }

        _set_property_value_locked(property_id, value, true);
        if (property_id == SOURCE_PATH_PROPERTY_ID)
        {
            _set_property_value_locked(SOURCE_CODE_PROPERTY_ID, "", true);
        }
    }

    return _compile_current_source(false);
}

ProcessorReturnCode CmajorWrapper::set_state(ProcessorState* state, bool /*realtime_running*/)
{
    if (state == nullptr)
    {
        return ProcessorReturnCode::ERROR;
    }

    bool should_recompile = false;

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        for (const auto& property : state->properties())
        {
            switch (property.first)
            {
                case SOURCE_PATH_PROPERTY_ID:
                    _source_path = property.second;
                    _set_property_value_locked(SOURCE_PATH_PROPERTY_ID, property.second, true);
                    should_recompile = true;
                    break;

                case SOURCE_CODE_PROPERTY_ID:
                    _source_code = property.second;
                    _set_property_value_locked(SOURCE_CODE_PROPERTY_ID, property.second, true);
                    should_recompile = true;
                    break;

                default:
                    break;
            }
        }
    }

    if (should_recompile)
    {
        _compile_current_source(false);
    }

    if (state->bypassed().has_value())
    {
        this->set_bypassed(*state->bypassed());
    }

    std::vector<std::pair<ObjectId, float>> parameter_updates;
    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        for (const auto& parameter : state->parameters())
        {
            auto it = _parameters_by_id.find(parameter.first);
            if (it == _parameters_by_id.end())
            {
                continue;
            }

            auto normalized_value = std::clamp(parameter.second, 0.0f, 1.0f);
            auto domain_value = domain_from_normalized(it->second->properties, it->second->value_type, normalized_value);
            it->second->normalized_value.store(normalized_value);
            it->second->domain_value.store(domain_value);
            parameter_updates.push_back({parameter.first, normalized_value});
        }
    }

    if (auto runtime = _load_runtime())
    {
        for (const auto& update : parameter_updates)
        {
            _apply_parameter_value_to_runtime(*runtime, update.first, update.second);
        }
    }

    if (!state->binary_data().empty())
    {
        _restore_named_state_from_binary(state->binary_data());
    }

    return ProcessorReturnCode::OK;
}

ProcessorState CmajorWrapper::save_state() const
{
    ProcessorState state;
    state.set_bypass(this->bypassed());

    std::scoped_lock<std::mutex> lock(_state_lock);
    state.add_property_change(SOURCE_PATH_PROPERTY_ID, _source_path);
    state.add_property_change(SOURCE_CODE_PROPERTY_ID, _source_code);

    for (auto id : _active_parameter_ids)
    {
        auto it = _parameters_by_id.find(id);
        if (it != _parameters_by_id.end())
        {
            state.add_parameter_change(id, it->second->normalized_value.load());
        }
    }

    state.set_binary_data(_create_named_state_binary());
    return state;
}

PluginInfo CmajorWrapper::info() const
{
    auto info = _plugin_info;
    info.type = PluginType::CMAJOR;
    info.path = _source_path;
    info.source_code = _source_code;
    return info;
}

void CmajorWrapper::_register_properties()
{
    auto add_property = [this](ObjectId id, const std::string& name, const std::string& label)
    {
        PropertySlot property;
        property.descriptor = std::make_unique<StringPropertyDescriptor>(name, label, "");
        property.descriptor->set_id(id);
        _properties.emplace(id, std::move(property));
    };

    add_property(SOURCE_PATH_PROPERTY_ID, "source_path", "Source Path");
    add_property(SOURCE_CODE_PROPERTY_ID, "source_code", "Source Code");
    add_property(COMPILE_STATUS_PROPERTY_ID, "compile_status", "Compile Status");
    add_property(BUILD_LOG_PROPERTY_ID, "build_log", "Build Log");
    add_property(PROGRAM_DETAILS_PROPERTY_ID, "program_details", "Program Details");
}

void CmajorWrapper::_set_property_value_locked(ObjectId property_id, const std::string& value, bool notify)
{
    auto it = _properties.find(property_id);
    if (it == _properties.end())
    {
        return;
    }

    it->second.value = value;

    if (notify)
    {
        _host_control.post_event(std::make_unique<PropertyChangeNotificationEvent>(this->id(),
                                                                                   property_id,
                                                                                   value,
                                                                                   IMMEDIATE_PROCESS));
    }
}

void CmajorWrapper::_set_compile_feedback_locked(CompileStatus status,
                                                 std::string build_log,
                                                 std::string program_details,
                                                 bool notify)
{
    _set_property_value_locked(COMPILE_STATUS_PROPERTY_ID, _compile_status_to_string(status), notify);
    _set_property_value_locked(BUILD_LOG_PROPERTY_ID, std::move(build_log), notify);
    _set_property_value_locked(PROGRAM_DETAILS_PROPERTY_ID, std::move(program_details), notify);
}

ProcessorReturnCode CmajorWrapper::_compile_current_source(bool fail_if_empty)
{
    std::string source_path;
    std::string source_code;

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        source_path = _source_path;
        source_code = _source_code;
    }

    return _compile_source(source_path, source_code, fail_if_empty);
}

ProcessorReturnCode CmajorWrapper::_compile_source(const std::string& source_path,
                                                   const std::string& source_code,
                                                   bool fail_if_empty)
{
    std::scoped_lock<std::mutex> compile_guard(_compile_lock);

    if (source_path.empty() && source_code.empty())
    {
        if (fail_if_empty)
        {
            return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
        }

        auto had_layout = false;
        {
            std::scoped_lock<std::mutex> lock(_state_lock);
            had_layout = !_active_parameter_ids.empty() || static_cast<bool>(_load_runtime());
            _parameters_by_id.clear();
            _parameter_ids_by_name.clear();
            _active_parameter_ids.clear();
            _store_runtime(nullptr);
            _max_input_channels = MAX_TRACK_CHANNELS;
            _max_output_channels = MAX_TRACK_CHANNELS;
            _current_input_channels = std::min(_requested_input_channels, _max_input_channels);
            _current_output_channels = std::min(_requested_output_channels, _max_output_channels);
            _set_compile_feedback_locked(CompileStatus::EMPTY, "", "", true);
        }

        if (had_layout)
        {
            _host_control.post_event(std::make_unique<AudioGraphNotificationEvent>(AudioGraphNotificationEvent::Action::PROCESSOR_LAYOUT_CHANGED,
                                                                                   this->id(),
                                                                                   0,
                                                                                   IMMEDIATE_PROCESS));
        }

        return ProcessorReturnCode::OK;
    }

    auto loaded_source = source_code;
    std::string compile_filename = source_path.empty() ? "inline.cmajor" : source_path;

    if (loaded_source.empty())
    {
        auto [status, source_or_error] = _load_source_from_path(source_path);
        if (status != ProcessorReturnCode::OK)
        {
            std::scoped_lock<std::mutex> lock(_state_lock);
            _set_compile_feedback_locked(CompileStatus::ERROR, source_or_error, "", true);
            return status;
        }

        loaded_source = source_or_error;
    }

    cmaj::DiagnosticMessageList messages;
    cmaj::Program program;

    if (!program.parse(messages, compile_filename, loaded_source))
    {
        ELKLOG_LOG_ERROR("Failed to parse Cmajor source for {}: {}", this->name(), messages.toString());
        std::scoped_lock<std::mutex> lock(_state_lock);
        _set_compile_feedback_locked(CompileStatus::ERROR, messages.toString(), "", true);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    auto engine_options = choc::value::createObject({});
    auto engine = cmaj::Engine::create("cpp", &engine_options);
    if (!engine)
    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        _set_compile_feedback_locked(CompileStatus::ERROR, "Failed to create the Cmajor C++ engine", "", true);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    cmaj::BuildSettings build_settings;
    build_settings.setFrequency(_sample_rate)
                  .setMaxFrequency(_sample_rate)
                  .setMaxBlockSize(static_cast<uint32_t>(AUDIO_CHUNK_SIZE))
                  .setEventBufferSize(256)
                  .setSessionID(static_cast<int32_t>(this->id()))
                  .setOptimisationLevel(2);
    engine.setBuildSettings(build_settings);

    if (!engine.load(messages, program, {}, {}))
    {
        ELKLOG_LOG_ERROR("Failed to load Cmajor program for {}: {}", this->name(), build_log_string(engine, messages));
        std::scoped_lock<std::mutex> lock(_state_lock);
        _set_compile_feedback_locked(CompileStatus::ERROR, build_log_string(engine, messages), "", true);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    auto runtime = std::make_shared<Runtime>();
    runtime->engine = engine;
    runtime->input_endpoints = engine.getInputEndpoints();
    runtime->output_endpoints = engine.getOutputEndpoints();

    for (const auto& endpoint : runtime->input_endpoints)
    {
        if (is_audio_endpoint(endpoint))
        {
            runtime->max_input_channels += static_cast<int>(endpoint.getNumAudioChannels());
        }
    }

    for (const auto& endpoint : runtime->output_endpoints)
    {
        if (is_audio_endpoint(endpoint))
        {
            runtime->max_output_channels += static_cast<int>(endpoint.getNumAudioChannels());
        }
    }

    auto connected_input_channels = std::min(_requested_input_channels, runtime->max_input_channels);
    auto connected_output_channels = std::min(_requested_output_channels, runtime->max_output_channels);

    cmaj::AudioMIDIPerformer::Builder builder(engine, EVENT_FIFO_SIZE);

    int next_input_channel = 0;
    for (const auto& endpoint : runtime->input_endpoints)
    {
        if (is_supported_parameter_endpoint(endpoint))
        {
            runtime->pending_parameter_bindings.emplace_back(endpoint.endpointID.toString(),
                                                             endpoint_signature(endpoint),
                                                             cmaj::PatchParameterProperties(endpoint),
                                                             endpoint.dataTypes.front(),
                                                             engine.getEndpointHandle(endpoint.endpointID),
                                                             endpoint.isEvent());
            continue;
        }

        if (endpoint.isTimelineTimeSignature())
        {
            runtime->time_signature_input = {engine.getEndpointHandle(endpoint.endpointID), endpoint.isEvent()};
            continue;
        }

        if (endpoint.isTimelineTempo())
        {
            runtime->tempo_input = {engine.getEndpointHandle(endpoint.endpointID), endpoint.isEvent()};
            continue;
        }

        if (endpoint.isTimelineTransportState())
        {
            runtime->transport_state_input = {engine.getEndpointHandle(endpoint.endpointID), endpoint.isEvent()};
            continue;
        }

        if (endpoint.isTimelinePosition())
        {
            runtime->position_input = {engine.getEndpointHandle(endpoint.endpointID), endpoint.isEvent()};
            continue;
        }

        if (endpoint.isMIDI())
        {
            builder.connectMIDIInputTo(endpoint);
            continue;
        }

        if (is_audio_endpoint(endpoint))
        {
            std::vector<uint32_t> input_channels;
            std::vector<uint32_t> endpoint_channels;
            auto channel_count = static_cast<int>(endpoint.getNumAudioChannels());

            for (int channel = 0; channel < channel_count && next_input_channel < connected_input_channels; ++channel, ++next_input_channel)
            {
                input_channels.push_back(static_cast<uint32_t>(next_input_channel));
                endpoint_channels.push_back(static_cast<uint32_t>(channel));
            }

            if (!input_channels.empty())
            {
                builder.connectAudioInputTo(input_channels, endpoint, endpoint_channels, nullptr);
            }
        }
    }

    int next_output_channel = 0;
    for (const auto& endpoint : runtime->output_endpoints)
    {
        if (endpoint.isMIDI())
        {
            builder.connectMIDIOutputTo(endpoint);
            continue;
        }

        if (is_audio_endpoint(endpoint))
        {
            std::vector<uint32_t> output_channels;
            std::vector<uint32_t> endpoint_channels;
            auto channel_count = static_cast<int>(endpoint.getNumAudioChannels());

            for (int channel = 0; channel < channel_count && next_output_channel < connected_output_channels; ++channel, ++next_output_channel)
            {
                output_channels.push_back(static_cast<uint32_t>(next_output_channel));
                endpoint_channels.push_back(static_cast<uint32_t>(channel));
            }

            if (!output_channels.empty())
            {
                builder.connectAudioOutputTo(endpoint, endpoint_channels, output_channels, nullptr);
            }
        }
    }

    if (!engine.link(messages))
    {
        ELKLOG_LOG_ERROR("Failed to link Cmajor program for {}: {}", this->name(), build_log_string(engine, messages));
        std::scoped_lock<std::mutex> lock(_state_lock);
        _set_compile_feedback_locked(CompileStatus::ERROR, build_log_string(engine, messages), "", true);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    runtime->performer = builder.createPerformer();
    if (!runtime->performer || !runtime->performer->prepareToStart())
    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        _set_compile_feedback_locked(CompileStatus::ERROR, "Failed to prepare the Cmajor performer", "", true);
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        _source_path = source_path;
        _source_code = loaded_source;
        _set_property_value_locked(SOURCE_PATH_PROPERTY_ID, _source_path, true);
        _set_property_value_locked(SOURCE_CODE_PROPERTY_ID, _source_code, true);
    }

    _install_runtime(runtime);

    auto program_details_json = _create_program_details_json(*runtime);

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        _set_compile_feedback_locked(CompileStatus::READY,
                                     build_log_string(runtime->engine, messages),
                                     std::move(program_details_json),
                                     true);
    }

    return ProcessorReturnCode::OK;
}

std::pair<ProcessorReturnCode, std::string> CmajorWrapper::_load_source_from_path(const std::string& source_path)
{
    if (source_path.empty())
    {
        return {ProcessorReturnCode::PARAMETER_ERROR, "No Cmajor source path supplied"};
    }

    auto resolved_path = std::filesystem::path(source_path);
    if (resolved_path.is_relative())
    {
        resolved_path = _host_control.to_absolute_path(source_path);
    }

    try
    {
        return {ProcessorReturnCode::OK, choc::file::loadFileAsString(resolved_path.string())};
    }
    catch (const std::exception& e)
    {
        return {ProcessorReturnCode::PLUGIN_LOAD_ERROR, e.what()};
    }
}

void CmajorWrapper::_install_runtime(std::shared_ptr<Runtime> runtime)
{
    auto notify_layout_change = false;

    {
        struct PreviousParameter
        {
            ObjectId id;
            std::string name;
            std::string signature;
            float normalized_value;
        };

        std::unordered_map<std::string, PreviousParameter> previous_parameters;

        std::scoped_lock<std::mutex> lock(_state_lock);
        for (auto id : _active_parameter_ids)
        {
            auto it = _parameters_by_id.find(id);
            if (it != _parameters_by_id.end())
            {
                previous_parameters[it->second->endpoint_id] = {
                    id,
                    it->second->descriptor->name(),
                    it->second->signature,
                    it->second->normalized_value.load()
                };
            }
        }

        notify_layout_change = !_active_parameter_ids.empty() || static_cast<bool>(_load_runtime());

        _parameters_by_id.clear();
        _parameter_ids_by_name.clear();
        _active_parameter_ids.clear();

        if (runtime)
        {
            std::unordered_set<std::string> used_names;
            for (const auto& property : _properties)
            {
                used_names.insert(property.second.descriptor->name());
            }

            for (const auto& pending_binding : runtime->pending_parameter_bindings)
            {
                ObjectId parameter_id = _next_parameter_id++;
                auto normalized_value = normalized_default_value(pending_binding.properties, pending_binding.value_type);
                auto descriptor_name = std::string();

                auto previous_it = previous_parameters.find(pending_binding.endpoint_id);
                if (previous_it != previous_parameters.end() && previous_it->second.signature == pending_binding.signature)
                {
                    parameter_id = previous_it->second.id;
                    normalized_value = previous_it->second.normalized_value;
                    descriptor_name = previous_it->second.name;
                    used_names.insert(descriptor_name);
                }
                else
                {
                    descriptor_name = make_unique_name(pending_binding.endpoint_id, used_names);
                }

                auto parameter_state = std::make_unique<ParameterState>(pending_binding.properties,
                                                                       pending_binding.value_type);
                parameter_state->endpoint_id = pending_binding.endpoint_id;
                parameter_state->signature = pending_binding.signature;
                parameter_state->is_event = pending_binding.is_event;
                parameter_state->descriptor = make_descriptor(parameter_id,
                                                              descriptor_name,
                                                              parameter_state->properties,
                                                              parameter_state->value_type);

                auto domain_value = domain_from_normalized(parameter_state->properties,
                                                           parameter_state->value_type,
                                                           normalized_value);

                parameter_state->normalized_value.store(normalized_value);
                parameter_state->domain_value.store(domain_value);

                runtime->input_parameter_bindings[parameter_id] = {
                    pending_binding.handle,
                    pending_binding.value_type,
                    pending_binding.properties.rampFrames,
                    pending_binding.is_event
                };

                _parameter_ids_by_name.emplace(descriptor_name, parameter_id);
                _active_parameter_ids.push_back(parameter_id);
                _parameters_by_id.emplace(parameter_id, std::move(parameter_state));
                _next_parameter_id = std::max(_next_parameter_id, parameter_id + 1);
            }

            _max_input_channels = std::max(runtime->max_input_channels, 0);
            _max_output_channels = std::max(runtime->max_output_channels, 0);
        }
        else
        {
            _max_input_channels = MAX_TRACK_CHANNELS;
            _max_output_channels = MAX_TRACK_CHANNELS;
        }

        _current_input_channels = std::min(_requested_input_channels, _max_input_channels);
        _current_output_channels = std::min(_requested_output_channels, _max_output_channels);
        _store_runtime(runtime);
    }

    if (runtime)
    {
        _apply_all_parameter_values_to_runtime(*runtime);
    }

    if (notify_layout_change)
    {
        _host_control.post_event(std::make_unique<AudioGraphNotificationEvent>(AudioGraphNotificationEvent::Action::PROCESSOR_LAYOUT_CHANGED,
                                                                               this->id(),
                                                                               0,
                                                                               IMMEDIATE_PROCESS));
    }
}

void CmajorWrapper::_apply_parameter_value_to_runtime(const Runtime& runtime,
                                                      ObjectId parameter_id,
                                                      float normalized_value) const
{
    auto binding_it = runtime.input_parameter_bindings.find(parameter_id);
    if (binding_it == runtime.input_parameter_bindings.end() || !runtime.performer)
    {
        return;
    }

    const ParameterState* parameter = nullptr;
    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        auto parameter_it = _parameters_by_id.find(parameter_id);
        if (parameter_it != _parameters_by_id.end())
        {
            parameter = parameter_it->second.get();
        }
    }

    if (parameter == nullptr)
    {
        return;
    }

    auto domain_value = domain_from_normalized(parameter->properties,
                                               parameter->value_type,
                                               normalized_value);

    if (binding_it->second.value_type.isBool())
    {
        auto value = choc::value::createBool(domain_value >= 0.5f);
        if (binding_it->second.is_event)
        {
            runtime.performer->postEvent(binding_it->second.handle, value, 0);
        }
        else
        {
            runtime.performer->postValue(binding_it->second.handle, value, binding_it->second.ramp_frames, 0);
        }
    }
    else if (binding_it->second.value_type.isInt32())
    {
        auto value = choc::value::createInt32(static_cast<int32_t>(std::round(domain_value)));
        if (binding_it->second.is_event)
        {
            runtime.performer->postEvent(binding_it->second.handle, value, 0);
        }
        else
        {
            runtime.performer->postValue(binding_it->second.handle, value, binding_it->second.ramp_frames, 0);
        }
    }
    else if (binding_it->second.value_type.isInt64())
    {
        auto value = choc::value::createInt64(static_cast<int64_t>(std::llround(domain_value)));
        if (binding_it->second.is_event)
        {
            runtime.performer->postEvent(binding_it->second.handle, value, 0);
        }
        else
        {
            runtime.performer->postValue(binding_it->second.handle, value, binding_it->second.ramp_frames, 0);
        }
    }
    else if (binding_it->second.value_type.isFloat64())
    {
        auto value = choc::value::createFloat64(domain_value);
        if (binding_it->second.is_event)
        {
            runtime.performer->postEvent(binding_it->second.handle, value, 0);
        }
        else
        {
            runtime.performer->postValue(binding_it->second.handle, value, binding_it->second.ramp_frames, 0);
        }
    }
    else
    {
        auto value = choc::value::createFloat32(domain_value);
        if (binding_it->second.is_event)
        {
            runtime.performer->postEvent(binding_it->second.handle, value, 0);
        }
        else
        {
            runtime.performer->postValue(binding_it->second.handle, value, binding_it->second.ramp_frames, 0);
        }
    }
}

void CmajorWrapper::_apply_all_parameter_values_to_runtime(const Runtime& runtime) const
{
    std::vector<std::pair<ObjectId, float>> parameter_values;

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        parameter_values.reserve(_active_parameter_ids.size());
        for (auto id : _active_parameter_ids)
        {
            auto it = _parameters_by_id.find(id);
            if (it != _parameters_by_id.end())
            {
                parameter_values.push_back({id, it->second->normalized_value.load()});
            }
        }
    }

    for (const auto& parameter : parameter_values)
    {
        _apply_parameter_value_to_runtime(runtime, parameter.first, parameter.second);
    }
}

void CmajorWrapper::_restore_named_state_from_binary(const std::vector<std::byte>& binary_data)
{
    if (binary_data.empty())
    {
        return;
    }

    std::string json(reinterpret_cast<const char*>(binary_data.data()), binary_data.size());
    std::vector<std::pair<ObjectId, float>> parameter_updates;

    try
    {
        auto root = choc::json::parse(json);
        auto params = root["parameters"];

        if (!params.isArray())
        {
            return;
        }

        std::scoped_lock<std::mutex> lock(_state_lock);
        for (uint32_t i = 0; i < params.size(); ++i)
        {
            auto entry = params[i];
            auto endpoint_id = entry["endpoint"].toString();
            auto normalized_value = std::clamp(entry["value"].getWithDefault<float>(0.0f), 0.0f, 1.0f);

            for (auto parameter_id : _active_parameter_ids)
            {
                auto it = _parameters_by_id.find(parameter_id);
                if (it != _parameters_by_id.end() && it->second->endpoint_id == endpoint_id)
                {
                    auto domain_value = domain_from_normalized(it->second->properties, it->second->value_type, normalized_value);
                    it->second->normalized_value.store(normalized_value);
                    it->second->domain_value.store(domain_value);
                    parameter_updates.push_back({parameter_id, normalized_value});
                    break;
                }
            }
        }
    }
    catch (const std::exception&)
    {
        return;
    }

    if (auto runtime = _load_runtime())
    {
        for (const auto& update : parameter_updates)
        {
            _apply_parameter_value_to_runtime(*runtime, update.first, update.second);
        }
    }
}

std::string CmajorWrapper::_compile_status_to_string(CompileStatus status) const
{
    switch (status)
    {
        case CompileStatus::READY: return "ready";
        case CompileStatus::ERROR: return "error";
        case CompileStatus::EMPTY: return "empty";
        default:                   return "empty";
    }
}

std::string CmajorWrapper::_create_program_details_json(const Runtime& runtime) const
{
    auto value = runtime.engine.getProgramDetails();
    if (!value.isObject())
    {
        value = choc::value::createObject({});
    }

    value.setMember("inputEndpoints", runtime.input_endpoints.toJSON(true));
    value.setMember("outputEndpoints", runtime.output_endpoints.toJSON(true));

    auto parameter_array = choc::value::createEmptyArray();
    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        for (auto parameter_id : _active_parameter_ids)
        {
            auto it = _parameters_by_id.find(parameter_id);
            if (it != _parameters_by_id.end())
            {
                parameter_array.addArrayElement(choc::json::create("id",
                                                                   static_cast<int32_t>(parameter_id),
                                                                   "endpointID",
                                                                   it->second->endpoint_id,
                                                                   "name",
                                                                   it->second->descriptor->name(),
                                                                   "label",
                                                                   it->second->descriptor->label()));
            }
        }
    }

    value.setMember("parameters", parameter_array);
    return choc::json::toString(value, true);
}

std::vector<std::byte> CmajorWrapper::_create_named_state_binary() const
{
    auto parameters = choc::value::createEmptyArray();

    {
        std::scoped_lock<std::mutex> lock(_state_lock);
        for (auto parameter_id : _active_parameter_ids)
        {
            auto it = _parameters_by_id.find(parameter_id);
            if (it != _parameters_by_id.end())
            {
                parameters.addArrayElement(choc::json::create("endpoint",
                                                              it->second->endpoint_id,
                                                              "value",
                                                              it->second->normalized_value.load()));
            }
        }
    }

    auto state = choc::json::create("parameters", parameters);
    auto json = choc::json::toString(state, false);

    std::vector<std::byte> binary_data(json.size());
    if (!json.empty())
    {
        std::memcpy(binary_data.data(), json.data(), json.size());
    }
    return binary_data;
}

std::shared_ptr<CmajorWrapper::Runtime> CmajorWrapper::_load_runtime() const
{
    return std::atomic_load(&_runtime);
}

void CmajorWrapper::_store_runtime(std::shared_ptr<Runtime> runtime)
{
    std::atomic_store(&_runtime, std::move(runtime));
}

} // namespace sushi::internal::cmajor_plugin
