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

/**
 * @brief Wrapper for Audio Unit V2 plugins.
 */

#ifndef SUSHI_AUV2_WRAPPER_H
#define SUSHI_AUV2_WRAPPER_H

#include <map>

#include <AudioToolbox/AudioToolbox.h>

#include "library/processor.h"

namespace sushi::internal::auv2_wrapper {

constexpr int AUV2_WRAPPER_MAX_N_CHANNELS = MAX_TRACK_CHANNELS;

class AUv2WrapperAccessor;

inline UInt32 fourcc_to_uint32(const char* s)
{
    return (static_cast<UInt32>(s[0]) << 24) |
           (static_cast<UInt32>(s[1]) << 16) |
           (static_cast<UInt32>(s[2]) << 8)  |
           (static_cast<UInt32>(s[3]));
}

inline std::string uint32_to_fourcc(UInt32 val)
{
    char buf[5];
    buf[0] = static_cast<char>((val >> 24) & 0xFF);
    buf[1] = static_cast<char>((val >> 16) & 0xFF);
    buf[2] = static_cast<char>((val >> 8) & 0xFF);
    buf[3] = static_cast<char>(val & 0xFF);
    buf[4] = '\0';
    return std::string(buf);
}

class AUv2Wrapper : public Processor
{
public:
    SUSHI_DECLARE_NON_COPYABLE(AUv2Wrapper);

    AUv2Wrapper(HostControl host_control, const std::string& uid);

    ~AUv2Wrapper() override;

    ProcessorReturnCode init(float sample_rate) override;

    void configure(float sample_rate) override;

    void process_event(const RtEvent& event) override;

    void process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer) override;

    void set_enabled(bool enabled) override;

    void set_bypassed(bool bypassed) override;

    bool bypassed() const override;

    std::pair<ProcessorReturnCode, float> parameter_value(ObjectId parameter_id) const override;

    std::pair<ProcessorReturnCode, float> parameter_value_in_domain(ObjectId parameter_id) const override;

    std::pair<ProcessorReturnCode, std::string> parameter_value_formatted(ObjectId parameter_id) const override;

    ProcessorReturnCode set_state(ProcessorState* state, bool realtime_running) override;

    ProcessorState save_state() const override;

    bool has_editor() const override;

    PluginInfo info() const override;

    AudioUnit audio_unit() const { return _audio_unit; }

private:
    friend AUv2WrapperAccessor;

    bool _register_parameters();

    bool _setup_stream_format(float sample_rate);

    bool _set_render_callback();

    static OSStatus _render_callback(void* inRefCon,
                                     AudioUnitRenderActionFlags* ioActionFlags,
                                     const AudioTimeStamp* inTimeStamp,
                                     UInt32 inBusNumber,
                                     UInt32 inNumberFrames,
                                     AudioBufferList* ioData);

    void _cleanup();

    std::string _uid;

    float _sample_rate{0};

    BypassManager _bypass_manager{_bypassed};

    AudioComponentInstance _audio_unit{nullptr};
    AudioComponentDescription _component_desc{};

    bool _is_instrument{false};

    // Bidirectional parameter maps
    std::map<AudioUnitParameterID, ObjectId> _au_to_sushi_param;
    std::map<ObjectId, AudioUnitParameterID> _sushi_to_au_param;

    // Cached parameter values (in AU domain)
    std::map<AudioUnitParameterID, Float32> _param_values;

    // Audio buffer lists for render
    AudioBufferList* _input_buffer_list{nullptr};
    AudioBufferList* _output_buffer_list{nullptr};

    // Render input data pointers (set per process call)
    const ChunkSampleBuffer* _render_input{nullptr};

    // Sample time counter
    Float64 _sample_time{0};
};

} // end namespace sushi::internal::auv2_wrapper

#endif // SUSHI_AUV2_WRAPPER_H
