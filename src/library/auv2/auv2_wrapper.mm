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
 * @brief Wrapper for Audio Unit V2 plugins.
 */

#include <cstring>

#import <CoreFoundation/CoreFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include "twine/twine.h"

#include "elklog/static_logger.h"

#include "auv2_wrapper.h"
#include "library/event.h"

namespace sushi::internal::auv2_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("auv2");

AUv2Wrapper::AUv2Wrapper(HostControl host_control, const std::string& uid) :
        Processor(host_control),
        _uid(uid)
{
    _max_input_channels = AUV2_WRAPPER_MAX_N_CHANNELS;
    _max_output_channels = AUV2_WRAPPER_MAX_N_CHANNELS;
    _enabled = false;
}

AUv2Wrapper::~AUv2Wrapper()
{
    ELKLOG_LOG_DEBUG("Unloading AUv2 plugin {}", this->name());
    _cleanup();
}

void AUv2Wrapper::_cleanup()
{
    if (_audio_unit)
    {
        AudioUnitUninitialize(_audio_unit);
        AudioComponentInstanceDispose(_audio_unit);
        _audio_unit = nullptr;
    }

    if (_input_buffer_list)
    {
        free(_input_buffer_list);
        _input_buffer_list = nullptr;
    }

    if (_output_buffer_list)
    {
        free(_output_buffer_list);
        _output_buffer_list = nullptr;
    }
}

ProcessorReturnCode AUv2Wrapper::init(float sample_rate)
{
    _sample_rate = sample_rate;

    // Parse uid "type:subtype:manufacturer"
    auto first_colon = _uid.find(':');
    auto second_colon = _uid.find(':', first_colon + 1);
    if (first_colon == std::string::npos || second_colon == std::string::npos ||
        first_colon != 4 || second_colon != 9 || _uid.size() != 14)
    {
        ELKLOG_LOG_ERROR("Invalid AUv2 uid format '{}', expected 'type:subt:mfgr'", _uid);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    auto type_str = _uid.substr(0, 4);
    auto subtype_str = _uid.substr(5, 4);
    auto mfr_str = _uid.substr(10, 4);

    _component_desc.componentType = fourcc_to_uint32(type_str.c_str());
    _component_desc.componentSubType = fourcc_to_uint32(subtype_str.c_str());
    _component_desc.componentManufacturer = fourcc_to_uint32(mfr_str.c_str());
    _component_desc.componentFlags = 0;
    _component_desc.componentFlagsMask = 0;

    _is_instrument = (_component_desc.componentType == kAudioUnitType_MusicDevice ||
                      _component_desc.componentType == kAudioUnitType_MusicEffect);

    AudioComponent component = AudioComponentFindNext(nullptr, &_component_desc);
    if (!component)
    {
        ELKLOG_LOG_ERROR("AUv2 component not found for uid '{}'", _uid);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    OSStatus status = AudioComponentInstanceNew(component, &_audio_unit);
    if (status != noErr || !_audio_unit)
    {
        ELKLOG_LOG_ERROR("Failed to create AUv2 instance for uid '{}', status: {}", _uid, status);
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    // Get the component name
    CFStringRef au_name = nullptr;
    status = AudioComponentCopyName(component, &au_name);
    if (status == noErr && au_name)
    {
        char name_buf[256];
        if (CFStringGetCString(au_name, name_buf, sizeof(name_buf), kCFStringEncodingUTF8))
        {
            set_name(name_buf);
            set_label(name_buf);
        }
        CFRelease(au_name);
    }

    if (!_setup_stream_format(sample_rate))
    {
        ELKLOG_LOG_ERROR("Failed to setup stream format for AUv2 plugin '{}'", _uid);
        _cleanup();
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    // Set maximum frames per slice
    UInt32 max_frames = AUDIO_CHUNK_SIZE;
    status = AudioUnitSetProperty(_audio_unit,
                                  kAudioUnitProperty_MaximumFramesPerSlice,
                                  kAudioUnitScope_Global,
                                  0,
                                  &max_frames,
                                  sizeof(max_frames));
    if (status != noErr)
    {
        ELKLOG_LOG_WARNING("Failed to set MaximumFramesPerSlice, status: {}", status);
    }

    if (!_set_render_callback())
    {
        ELKLOG_LOG_ERROR("Failed to set render callback for AUv2 plugin '{}'", _uid);
        _cleanup();
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    status = AudioUnitInitialize(_audio_unit);
    if (status != noErr)
    {
        ELKLOG_LOG_ERROR("Failed to initialize AUv2 plugin '{}', status: {}", _uid, status);
        _cleanup();
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    // Query actual channel counts after initialization
    AudioStreamBasicDescription output_format{};
    UInt32 format_size = sizeof(output_format);
    status = AudioUnitGetProperty(_audio_unit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output,
                                  0,
                                  &output_format,
                                  &format_size);
    if (status == noErr)
    {
        _max_output_channels = std::min(static_cast<int>(output_format.mChannelsPerFrame),
                                        AUV2_WRAPPER_MAX_N_CHANNELS);
    }
    else
    {
        _max_output_channels = 2;
    }

    AudioStreamBasicDescription input_format{};
    format_size = sizeof(input_format);
    status = AudioUnitGetProperty(_audio_unit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  0,
                                  &input_format,
                                  &format_size);
    if (status == noErr)
    {
        _max_input_channels = std::min(static_cast<int>(input_format.mChannelsPerFrame),
                                       AUV2_WRAPPER_MAX_N_CHANNELS);
    }
    else
    {
        _max_input_channels = 2;
    }

    ELKLOG_LOG_INFO("AUv2 plugin '{}' channels: {} in, {} out", _uid, _max_input_channels, _max_output_channels);

    // Allocate AudioBufferLists
    auto alloc_buffer_list = [](int channels) -> AudioBufferList* {
        size_t size = offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * static_cast<size_t>(channels);
        auto* list = static_cast<AudioBufferList*>(calloc(1, size));
        list->mNumberBuffers = static_cast<UInt32>(channels);
        for (int i = 0; i < channels; ++i)
        {
            list->mBuffers[i].mNumberChannels = 1;
            list->mBuffers[i].mDataByteSize = 0;
            list->mBuffers[i].mData = nullptr;
        }
        return list;
    };

    _input_buffer_list = alloc_buffer_list(_max_input_channels);
    _output_buffer_list = alloc_buffer_list(_max_output_channels);

    if (!_register_parameters())
    {
        ELKLOG_LOG_WARNING("Parameter registration failed for AUv2 plugin '{}'", _uid);
    }

    return ProcessorReturnCode::OK;
}

bool AUv2Wrapper::_setup_stream_format(float sample_rate)
{
    AudioStreamBasicDescription format{};
    format.mSampleRate = sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat |
                          kAudioFormatFlagIsNonInterleaved |
                          kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = 2;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4;
    format.mBytesPerPacket = 4;

    // Set output format
    OSStatus status = AudioUnitSetProperty(_audio_unit,
                                           kAudioUnitProperty_StreamFormat,
                                           kAudioUnitScope_Output,
                                           0,
                                           &format,
                                           sizeof(format));
    if (status != noErr)
    {
        ELKLOG_LOG_WARNING("Failed to set output stream format, status: {}", status);
    }

    // Set input format
    status = AudioUnitSetProperty(_audio_unit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input,
                                  0,
                                  &format,
                                  sizeof(format));
    if (status != noErr)
    {
        ELKLOG_LOG_WARNING("Failed to set input stream format, status: {}", status);
    }

    return true;
}

bool AUv2Wrapper::_set_render_callback()
{
    AURenderCallbackStruct callback_struct{};
    callback_struct.inputProc = _render_callback;
    callback_struct.inputProcRefCon = this;

    OSStatus status = AudioUnitSetProperty(_audio_unit,
                                           kAudioUnitProperty_SetRenderCallback,
                                           kAudioUnitScope_Input,
                                           0,
                                           &callback_struct,
                                           sizeof(callback_struct));
    if (status != noErr)
    {
        ELKLOG_LOG_ERROR("Failed to set render callback, status: {}", status);
        return false;
    }
    return true;
}

OSStatus AUv2Wrapper::_render_callback(void* inRefCon,
                                        AudioUnitRenderActionFlags* /*ioActionFlags*/,
                                        const AudioTimeStamp* /*inTimeStamp*/,
                                        UInt32 /*inBusNumber*/,
                                        UInt32 inNumberFrames,
                                        AudioBufferList* ioData)
{
    auto* self = static_cast<AUv2Wrapper*>(inRefCon);

    if (!self->_render_input || !ioData)
    {
        return noErr;
    }

    int channels = std::min(static_cast<int>(ioData->mNumberBuffers),
                            self->_current_input_channels);
    UInt32 byte_size = inNumberFrames * sizeof(float);

    for (int ch = 0; ch < channels; ++ch)
    {
        ioData->mBuffers[ch].mDataByteSize = byte_size;
        memcpy(ioData->mBuffers[ch].mData,
               self->_render_input->channel(ch),
               byte_size);
    }

    // Zero any extra channels
    for (UInt32 ch = static_cast<UInt32>(channels); ch < ioData->mNumberBuffers; ++ch)
    {
        ioData->mBuffers[ch].mDataByteSize = byte_size;
        memset(ioData->mBuffers[ch].mData, 0, byte_size);
    }

    return noErr;
}

void AUv2Wrapper::configure(float sample_rate)
{
    _sample_rate = sample_rate;
    bool was_enabled = enabled();
    if (was_enabled)
    {
        set_enabled(false);
    }

    AudioUnitUninitialize(_audio_unit);
    _setup_stream_format(sample_rate);

    UInt32 max_frames = AUDIO_CHUNK_SIZE;
    AudioUnitSetProperty(_audio_unit,
                         kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global,
                         0,
                         &max_frames,
                         sizeof(max_frames));

    OSStatus status = AudioUnitInitialize(_audio_unit);
    if (status != noErr)
    {
        ELKLOG_LOG_ERROR("Error reconfiguring AUv2 plugin to sample rate {}", sample_rate);
    }

    if (was_enabled)
    {
        set_enabled(true);
    }
}

void AUv2Wrapper::process_event(const RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::FLOAT_PARAMETER_CHANGE:
        {
            auto typed_event = event.parameter_change_event();
            auto sushi_id = typed_event->param_id();
            auto it = _sushi_to_au_param.find(sushi_id);
            if (it != _sushi_to_au_param.end())
            {
                auto au_param_id = it->second;
                auto param_desc = parameter_from_id(sushi_id);
                Float32 plain_value = typed_event->value();
                if (param_desc)
                {
                    plain_value = param_desc->min_domain_value() +
                                  typed_event->value() * (param_desc->max_domain_value() - param_desc->min_domain_value());
                }

                AudioUnitSetParameter(_audio_unit,
                                      au_param_id,
                                      kAudioUnitScope_Global,
                                      0,
                                      plain_value,
                                      0);

                _param_values[au_param_id] = plain_value;
            }
            break;
        }
        case RtEventType::NOTE_ON:
        {
            if (_is_instrument)
            {
                auto kbd = event.keyboard_event();
                MusicDeviceMIDIEvent(_audio_unit,
                                     0x90 | static_cast<UInt32>(kbd->channel()),
                                     static_cast<UInt32>(kbd->note()),
                                     static_cast<UInt32>(kbd->velocity() * 127.0f),
                                     static_cast<UInt32>(kbd->sample_offset()));
            }
            break;
        }
        case RtEventType::NOTE_OFF:
        {
            if (_is_instrument)
            {
                auto kbd = event.keyboard_event();
                MusicDeviceMIDIEvent(_audio_unit,
                                     0x80 | static_cast<UInt32>(kbd->channel()),
                                     static_cast<UInt32>(kbd->note()),
                                     static_cast<UInt32>(kbd->velocity() * 127.0f),
                                     static_cast<UInt32>(kbd->sample_offset()));
            }
            break;
        }
        case RtEventType::WRAPPED_MIDI_EVENT:
        {
            if (_is_instrument)
            {
                auto midi_ev = event.wrapped_midi_event();
                auto data = midi_ev->midi_data();
                MusicDeviceMIDIEvent(_audio_unit,
                                     data[0],
                                     data[1],
                                     data[2],
                                     static_cast<UInt32>(midi_ev->sample_offset()));
            }
            break;
        }
        case RtEventType::PITCH_BEND:
        {
            if (_is_instrument)
            {
                auto kbd = event.keyboard_common_event();
                // Sushi pitch bend is -1 to +1, MIDI pitch bend is 0 to 16383 with center at 8192
                int bend_value = static_cast<int>((kbd->value() + 1.0f) * 8191.5f);
                bend_value = std::clamp(bend_value, 0, 16383);
                MusicDeviceMIDIEvent(_audio_unit,
                                     0xE0 | static_cast<UInt32>(kbd->channel()),
                                     static_cast<UInt32>(bend_value & 0x7F),
                                     static_cast<UInt32>((bend_value >> 7) & 0x7F),
                                     static_cast<UInt32>(kbd->sample_offset()));
            }
            break;
        }
        case RtEventType::MODULATION:
        {
            if (_is_instrument)
            {
                auto kbd = event.keyboard_common_event();
                UInt32 cc_value = static_cast<UInt32>(std::clamp(kbd->value(), 0.0f, 1.0f) * 127.0f);
                MusicDeviceMIDIEvent(_audio_unit,
                                     0xB0 | static_cast<UInt32>(kbd->channel()),
                                     1, // CC#1 = modulation
                                     cc_value,
                                     static_cast<UInt32>(kbd->sample_offset()));
            }
            break;
        }
        case RtEventType::SET_BYPASS:
        {
            bool bypass = static_cast<bool>(event.processor_command_event()->value());
            _bypass_manager.set_bypass(bypass, _sample_rate);
            break;
        }
        default:
            break;
    }
}

void AUv2Wrapper::process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer)
{
    if (!_bypass_manager.should_process())
    {
        bypass_process(in_buffer, out_buffer);
        return;
    }

    // Set up render input for the callback
    _render_input = &in_buffer;

    // Set up output buffer list
    UInt32 byte_size = AUDIO_CHUNK_SIZE * sizeof(float);
    for (int ch = 0; ch < _current_output_channels; ++ch)
    {
        _output_buffer_list->mBuffers[ch].mDataByteSize = byte_size;
        _output_buffer_list->mBuffers[ch].mData = out_buffer.channel(ch);
    }

    // Prepare timestamp
    AudioTimeStamp timestamp{};
    timestamp.mSampleTime = _sample_time;
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;

    AudioUnitRenderActionFlags flags = 0;

    OSStatus status = AudioUnitRender(_audio_unit,
                                      &flags,
                                      &timestamp,
                                      0,
                                      AUDIO_CHUNK_SIZE,
                                      _output_buffer_list);

    if (status != noErr)
    {
        out_buffer.clear();
    }

    _sample_time += AUDIO_CHUNK_SIZE;
    _render_input = nullptr;

    if (_bypass_manager.should_ramp())
    {
        _bypass_manager.crossfade_output(in_buffer, out_buffer, _current_input_channels, _current_output_channels);
    }
}

void AUv2Wrapper::set_enabled(bool enabled)
{
    if (enabled == _enabled)
    {
        return;
    }

    if (enabled && _audio_unit)
    {
        // AudioUnit is already initialized, just toggle processing state
        AudioUnitReset(_audio_unit, kAudioUnitScope_Global, 0);
    }
    Processor::set_enabled(enabled);
}

void AUv2Wrapper::set_bypassed(bool bypassed)
{
    assert(twine::is_current_thread_realtime() == false);
    _host_control.post_event(std::make_unique<SetProcessorBypassEvent>(
        this->id(), bypassed, IMMEDIATE_PROCESS));
}

bool AUv2Wrapper::bypassed() const
{
    return _bypass_manager.bypassed();
}

std::pair<ProcessorReturnCode, float> AUv2Wrapper::parameter_value(ObjectId parameter_id) const
{
    auto it = _sushi_to_au_param.find(parameter_id);
    if (it == _sushi_to_au_param.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }

    Float32 value = 0;
    OSStatus status = AudioUnitGetParameter(_audio_unit,
                                            it->second,
                                            kAudioUnitScope_Global,
                                            0,
                                            &value);
    if (status == noErr)
    {
        auto param_desc = parameter_from_id(parameter_id);
        if (param_desc)
        {
            float range = param_desc->max_domain_value() - param_desc->min_domain_value();
            if (range > 0.0f)
            {
                float normalized = (value - param_desc->min_domain_value()) / range;
                return {ProcessorReturnCode::OK, normalized};
            }
        }
        return {ProcessorReturnCode::OK, value};
    }

    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
}

std::pair<ProcessorReturnCode, float> AUv2Wrapper::parameter_value_in_domain(ObjectId parameter_id) const
{
    auto it = _sushi_to_au_param.find(parameter_id);
    if (it == _sushi_to_au_param.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }

    Float32 value = 0;
    OSStatus status = AudioUnitGetParameter(_audio_unit,
                                            it->second,
                                            kAudioUnitScope_Global,
                                            0,
                                            &value);
    if (status == noErr)
    {
        return {ProcessorReturnCode::OK, value};
    }

    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
}

std::pair<ProcessorReturnCode, std::string> AUv2Wrapper::parameter_value_formatted(ObjectId parameter_id) const
{
    auto it = _sushi_to_au_param.find(parameter_id);
    if (it == _sushi_to_au_param.end())
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
    }

    Float32 value = 0;
    OSStatus status = AudioUnitGetParameter(_audio_unit,
                                            it->second,
                                            kAudioUnitScope_Global,
                                            0,
                                            &value);
    if (status == noErr)
    {
        // Try to get the value string from the AU
        AudioUnitParameterInfo param_info{};
        UInt32 info_size = sizeof(param_info);
        OSStatus info_status = AudioUnitGetProperty(_audio_unit,
                                                     kAudioUnitProperty_ParameterInfo,
                                                     kAudioUnitScope_Global,
                                                     it->second,
                                                     &param_info,
                                                     &info_size);
        if (info_status == noErr && (param_info.flags & kAudioUnitParameterFlag_HasCFNameString))
        {
            // For display, just format the value with the unit name
            std::string unit_str;
            if (param_info.unitName)
            {
                char unit_buf[64];
                if (CFStringGetCString(param_info.unitName, unit_buf, sizeof(unit_buf), kCFStringEncodingUTF8))
                {
                    unit_str = std::string(" ") + unit_buf;
                }
                CFRelease(param_info.unitName);
            }
            return {ProcessorReturnCode::OK, std::to_string(value) + unit_str};
        }

        return {ProcessorReturnCode::OK, std::to_string(value)};
    }
    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
}

ProcessorReturnCode AUv2Wrapper::set_state(ProcessorState* state, bool /*realtime_running*/)
{
    if (state->has_binary_data())
    {
        auto& binary_data = state->binary_data();
        CFDataRef cf_data = CFDataCreate(kCFAllocatorDefault,
                                         reinterpret_cast<const UInt8*>(binary_data.data()),
                                         static_cast<CFIndex>(binary_data.size()));
        if (!cf_data)
        {
            return ProcessorReturnCode::ERROR;
        }

        CFErrorRef error = nullptr;
        CFPropertyListRef plist = CFPropertyListCreateWithData(kCFAllocatorDefault,
                                                                cf_data,
                                                                kCFPropertyListImmutable,
                                                                nullptr,
                                                                &error);
        CFRelease(cf_data);

        if (!plist || error)
        {
            if (error) CFRelease(error);
            if (plist) CFRelease(plist);
            return ProcessorReturnCode::ERROR;
        }

        OSStatus status = AudioUnitSetProperty(_audio_unit,
                                               kAudioUnitProperty_ClassInfo,
                                               kAudioUnitScope_Global,
                                               0,
                                               &plist,
                                               sizeof(plist));
        CFRelease(plist);

        if (status != noErr)
        {
            return ProcessorReturnCode::ERROR;
        }
        return ProcessorReturnCode::OK;
    }

    // Apply parameter values
    for (const auto& param : state->parameters())
    {
        auto sushi_id = param.first;
        float value = param.second;
        auto it = _sushi_to_au_param.find(sushi_id);
        if (it != _sushi_to_au_param.end())
        {
            auto param_desc = parameter_from_id(sushi_id);
            if (param_desc)
            {
                Float32 plain = param_desc->min_domain_value() +
                                value * (param_desc->max_domain_value() - param_desc->min_domain_value());
                AudioUnitSetParameter(_audio_unit, it->second, kAudioUnitScope_Global, 0, plain, 0);
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

ProcessorState AUv2Wrapper::save_state() const
{
    ProcessorState state;

    CFPropertyListRef plist = nullptr;
    UInt32 plist_size = sizeof(plist);
    OSStatus status = AudioUnitGetProperty(_audio_unit,
                                           kAudioUnitProperty_ClassInfo,
                                           kAudioUnitScope_Global,
                                           0,
                                           &plist,
                                           &plist_size);
    if (status == noErr && plist)
    {
        CFDataRef cf_data = CFPropertyListCreateData(kCFAllocatorDefault,
                                                      plist,
                                                      kCFPropertyListBinaryFormat_v1_0,
                                                      0,
                                                      nullptr);
        CFRelease(plist);

        if (cf_data)
        {
            auto length = CFDataGetLength(cf_data);
            auto* bytes = CFDataGetBytePtr(cf_data);
            std::vector<std::byte> binary_data(length);
            memcpy(binary_data.data(), bytes, static_cast<size_t>(length));
            state.set_binary_data(std::move(binary_data));
            CFRelease(cf_data);
        }
    }
    else
    {
        ELKLOG_LOG_WARNING("Failed to save AUv2 plugin state");
    }

    return state;
}

bool AUv2Wrapper::has_editor() const
{
    if (!_audio_unit)
    {
        return false;
    }

    UInt32 data_size = 0;
    Boolean writable = false;
    OSStatus status = AudioUnitGetPropertyInfo(_audio_unit,
                                               kAudioUnitProperty_CocoaUI,
                                               kAudioUnitScope_Global,
                                               0,
                                               &data_size,
                                               &writable);
    return (status == noErr && data_size > 0);
}

PluginInfo AUv2Wrapper::info() const
{
    PluginInfo plugin_info;
    plugin_info.type = PluginType::AUV2;
    plugin_info.uid = _uid;
    return plugin_info;
}

bool AUv2Wrapper::_register_parameters()
{
    UInt32 param_list_size = 0;
    OSStatus status = AudioUnitGetPropertyInfo(_audio_unit,
                                               kAudioUnitProperty_ParameterList,
                                               kAudioUnitScope_Global,
                                               0,
                                               &param_list_size,
                                               nullptr);

    if (status != noErr || param_list_size == 0)
    {
        ELKLOG_LOG_INFO("AUv2 plugin has no parameters");
        return true;
    }

    UInt32 param_count = param_list_size / sizeof(AudioUnitParameterID);
    std::vector<AudioUnitParameterID> param_ids(param_count);

    status = AudioUnitGetProperty(_audio_unit,
                                  kAudioUnitProperty_ParameterList,
                                  kAudioUnitScope_Global,
                                  0,
                                  param_ids.data(),
                                  &param_list_size);

    if (status != noErr)
    {
        ELKLOG_LOG_ERROR("Failed to get AUv2 parameter list");
        return false;
    }

    ELKLOG_LOG_INFO("AUv2 plugin has {} parameters", param_count);

    for (UInt32 i = 0; i < param_count; ++i)
    {
        AudioUnitParameterInfo param_info{};
        UInt32 info_size = sizeof(param_info);
        status = AudioUnitGetProperty(_audio_unit,
                                      kAudioUnitProperty_ParameterInfo,
                                      kAudioUnitScope_Global,
                                      param_ids[i],
                                      &param_info,
                                      &info_size);

        if (status != noErr)
        {
            ELKLOG_LOG_WARNING("Failed to get info for parameter {}", param_ids[i]);
            continue;
        }

        std::string param_name;
        if (param_info.flags & kAudioUnitParameterFlag_HasCFNameString && param_info.cfNameString)
        {
            char name_buf[256];
            if (CFStringGetCString(param_info.cfNameString, name_buf, sizeof(name_buf), kCFStringEncodingUTF8))
            {
                param_name = name_buf;
            }
            if (param_info.flags & kAudioUnitParameterFlag_CFNameRelease)
            {
                CFRelease(param_info.cfNameString);
            }
        }
        else
        {
            param_name = std::string(param_info.name);
        }

        if (param_name.empty())
        {
            param_name = "Param " + std::to_string(param_ids[i]);
        }

        auto unique_name = _make_unique_parameter_name(param_name);
        bool automatable = (param_info.flags & kAudioUnitParameterFlag_IsWritable) != 0;
        auto direction = automatable ? Direction::AUTOMATABLE : Direction::OUTPUT;

        // Get default value
        Float32 default_value = param_info.defaultValue;
        _param_values[param_ids[i]] = default_value;

        std::string unit;
        if (param_info.unitName)
        {
            char unit_buf[64];
            if (CFStringGetCString(param_info.unitName, unit_buf, sizeof(unit_buf), kCFStringEncodingUTF8))
            {
                unit = unit_buf;
            }
            CFRelease(param_info.unitName);
        }

        ParameterDescriptor* descriptor = nullptr;

        if (param_info.unit == kAudioUnitParameterUnit_Boolean ||
            param_info.unit == kAudioUnitParameterUnit_Indexed)
        {
            int min = static_cast<int>(param_info.minValue);
            int max = static_cast<int>(param_info.maxValue);
            descriptor = new IntParameterDescriptor(unique_name,
                                                     param_name,
                                                     unit,
                                                     min,
                                                     max,
                                                     direction,
                                                     nullptr);
        }
        else
        {
            descriptor = new FloatParameterDescriptor(unique_name,
                                                       param_name,
                                                       unit,
                                                       param_info.minValue,
                                                       param_info.maxValue,
                                                       direction,
                                                       nullptr);
        }

        auto sushi_id = static_cast<ObjectId>(this->all_parameters().size());
        if (register_parameter(descriptor, sushi_id))
        {
            _au_to_sushi_param[param_ids[i]] = sushi_id;
            _sushi_to_au_param[sushi_id] = param_ids[i];
            ELKLOG_LOG_INFO("Registered parameter {} (au_id: {}, sushi_id: {})", param_name, param_ids[i], sushi_id);
        }
        else
        {
            ELKLOG_LOG_WARNING("Failed to register parameter {}", param_name);
            delete descriptor;
        }
    }

    return true;
}

} // end namespace sushi::internal::auv2_wrapper
