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

#ifndef SUSHI_CLAP_WRAPPER_H
#define SUSHI_CLAP_WRAPPER_H

#include <vector>
#include <map>
#include <cstring>

#include <clap/clap.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>

#include "clap_host_context.h"
#include "library/processor.h"

namespace sushi::internal::clap_wrapper {

constexpr int CLAP_WRAPPER_MAX_N_CHANNELS = MAX_TRACK_CHANNELS;
constexpr int CLAP_EVENT_BUFFER_SIZE = 4096;

class ClapWrapperAccessor;

/**
 * @brief A simple flat buffer for CLAP events that provides the
 *        clap_input_events_t and clap_output_events_t callback interfaces.
 */
class ClapEventList
{
public:
    ClapEventList();

    void clear();

    void push(const clap_event_header_t* event);

    const clap_input_events_t* input_events() { return &_input_events; }
    const clap_output_events_t* output_events() { return &_output_events; }

    uint32_t size() const { return _count; }
    const clap_event_header_t* get(uint32_t index) const;

private:
    static uint32_t _input_size(const clap_input_events_t* list);
    static const clap_event_header_t* _input_get(const clap_input_events_t* list, uint32_t index);
    static bool _output_try_push(const clap_output_events_t* list, const clap_event_header_t* event);

    std::vector<uint8_t> _buffer;
    std::vector<uint32_t> _offsets;
    uint32_t _count{0};
    uint32_t _write_pos{0};

    clap_input_events_t _input_events{};
    clap_output_events_t _output_events{};
};

/**
 * @brief Wrapper class for loading CLAP plugins and making them accessible
 *        as Processor to the Engine.
 */
class ClapWrapper : public Processor
{
public:
    SUSHI_DECLARE_NON_COPYABLE(ClapWrapper);

    ClapWrapper(HostControl host_control,
                const std::string& plugin_path,
                int plugin_index,
                ClapHostContext* host_context);

    ~ClapWrapper() override;

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

    ClapPluginInstance& instance() { return _instance; }

private:
    friend ClapWrapperAccessor;

    void _cleanup();

    bool _register_parameters();

    bool _setup_audio_ports();

    void _fill_transport(clap_event_transport_t& transport);

    void _forward_output_events();

    float _sample_rate{0};

    BypassManager _bypass_manager{_bypassed};

    std::string _plugin_load_path;
    int _plugin_index{0};

    ClapHostContext* _host_context;
    ClapPluginInstance _instance;

    ClapEventList _in_events;
    ClapEventList _out_events;

    // Audio buffer pointers for CLAP process
    float* _input_channel_ptrs[CLAP_WRAPPER_MAX_N_CHANNELS]{};
    float* _output_channel_ptrs[CLAP_WRAPPER_MAX_N_CHANNELS]{};
    clap_audio_buffer_t _clap_input_buffer{};
    clap_audio_buffer_t _clap_output_buffer{};

    // Bypass parameter tracking
    bool _has_bypass_parameter{false};
    clap_id _bypass_param_id{0};

    // Map from CLAP param_id to Sushi ObjectId for parameter lookup
    std::map<clap_id, ObjectId> _clap_to_sushi_param;
    std::map<ObjectId, clap_id> _sushi_to_clap_param;

    // Cached parameter values (plain domain values)
    std::map<clap_id, double> _param_values;
};

} // end namespace sushi::internal::clap_wrapper

#endif // SUSHI_CLAP_WRAPPER_H
