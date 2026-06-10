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

#include "supercollider_wrapper.h"

#include "elklog/static_logger.h"
#include "sushi/constants.h"
#include "library/event.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace sushi::internal::supercollider {

namespace {
ELKLOG_GET_LOGGER_WITH_MODULE_NAME("supercollider");

// SuperCollider's master output is bus 0 (stereo). We host a fixed stereo
// in/out World; tracks with more channels see only the first two driven.
constexpr int SC_CHANNELS = 2;
} // anonymous namespace

SuperColliderWrapper::SuperColliderWrapper(HostControl host_control, PluginInfo plugin_info)
    : Processor(host_control)
    , _plugin_info(std::move(plugin_info))
{
    set_name("supercollider");
    set_label("SuperCollider");

    _max_input_channels = SC_CHANNELS;
    _max_output_channels = SC_CHANNELS;

    register_parameter(new StringPropertyDescriptor("synthdef", "SynthDef Path", ""),
                       SYNTHDEF_PATH_PROPERTY_ID);
    register_parameter(new StringPropertyDescriptor("osc", "OSC Command", ""),
                       OSC_PROPERTY_ID);
    register_parameter(new StringPropertyDescriptor("status", "Status", ""),
                       STATUS_PROPERTY_ID);
}

SuperColliderWrapper::~SuperColliderWrapper() = default;

ProcessorReturnCode SuperColliderWrapper::init(float sample_rate)
{
    _sample_rate = sample_rate;

    if (!_bridge.init(static_cast<double>(sample_rate), AUDIO_CHUNK_SIZE, SC_CHANNELS, SC_CHANNELS))
    {
        ELKLOG_LOG_ERROR("Failed to initialise SuperCollider engine. Only one SuperCollider "
                         "processor can exist per Sushi instance.");
        _set_status("error: engine unavailable");
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    ELKLOG_LOG_INFO("SuperCollider engine initialised at {} Hz, {} sample blocks",
                    sample_rate, AUDIO_CHUNK_SIZE);
    _set_status("ok");

    // Load a SynthDef if one was supplied.
    if (!_plugin_info.path.empty())
    {
        _synthdef_path = _plugin_info.path;
        if (!_load_synthdef_file(_synthdef_path))
        {
            ELKLOG_LOG_WARNING("SuperCollider: failed to load SynthDef '{}'", _synthdef_path);
        }
    }

    // Run any initial OSC commands provided as source_code (one per line).
    if (!_plugin_info.source_code.empty())
    {
        _run_osc_script(_plugin_info.source_code);
    }

    return ProcessorReturnCode::OK;
}

void SuperColliderWrapper::configure(float sample_rate)
{
    // The scsynth World is built once at init() with a fixed sample rate and
    // block size. Sushi's sample rate is effectively fixed for a session, so a
    // changed rate here is logged rather than triggering a costly World rebuild.
    if (sample_rate != _sample_rate)
    {
        ELKLOG_LOG_WARNING("SuperCollider: sample rate change to {} Hz ignored (World built at {} Hz)",
                           sample_rate, _sample_rate);
    }
}

void SuperColliderWrapper::process_event(const RtEvent& event)
{
    if (event.type() == RtEventType::SET_BYPASS)
    {
        set_bypassed(static_cast<bool>(event.processor_command_event()->value()));
    }
}

void SuperColliderWrapper::process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer)
{
    if (bypassed())
    {
        bypass_process(in_buffer, out_buffer);
        return;
    }

    if (!_bridge.is_initialised())
    {
        out_buffer.clear();
        return;
    }

    std::array<const float*, SC_CHANNELS> inputs {};
    std::array<float*, SC_CHANNELS> outputs {};

    int in_ch = std::min(in_buffer.channel_count(), SC_CHANNELS);
    int out_ch = std::min(out_buffer.channel_count(), SC_CHANNELS);

    for (int ch = 0; ch < in_ch; ++ch)
    {
        inputs[ch] = in_buffer.channel(ch);
    }
    for (int ch = 0; ch < out_ch; ++ch)
    {
        outputs[ch] = out_buffer.channel(ch);
    }

    _bridge.process(inputs.data(), in_ch, outputs.data(), out_ch);

    // Zero any track output channels the stereo World did not fill.
    for (int ch = out_ch; ch < out_buffer.channel_count(); ++ch)
    {
        std::memset(out_buffer.channel(ch), 0, AUDIO_CHUNK_SIZE * sizeof(float));
    }
}

PluginInfo SuperColliderWrapper::info() const
{
    return _plugin_info;
}

ProcessorReturnCode SuperColliderWrapper::set_property_value(ObjectId property_id, const std::string& value)
{
    switch (property_id)
    {
        case SYNTHDEF_PATH_PROPERTY_ID:
        {
            _synthdef_path = value;
            _host_control.post_event(std::make_unique<PropertyChangeNotificationEvent>(
                id(), SYNTHDEF_PATH_PROPERTY_ID, value, IMMEDIATE_PROCESS));
            return _load_synthdef_file(value) ? ProcessorReturnCode::OK : ProcessorReturnCode::ERROR;
        }
        case OSC_PROPERTY_ID:
        {
            _last_osc = value;
            _host_control.post_event(std::make_unique<PropertyChangeNotificationEvent>(
                id(), OSC_PROPERTY_ID, value, IMMEDIATE_PROCESS));
            return _bridge.send_osc_text(value) ? ProcessorReturnCode::OK : ProcessorReturnCode::ERROR;
        }
        default:
            return ProcessorReturnCode::PARAMETER_NOT_FOUND;
    }
}

std::pair<ProcessorReturnCode, std::string> SuperColliderWrapper::property_value(ObjectId property_id) const
{
    switch (property_id)
    {
        case SYNTHDEF_PATH_PROPERTY_ID: return {ProcessorReturnCode::OK, _synthdef_path};
        case OSC_PROPERTY_ID:           return {ProcessorReturnCode::OK, _last_osc};
        case STATUS_PROPERTY_ID:        return {ProcessorReturnCode::OK, _status};
        default:                        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
    }
}

bool SuperColliderWrapper::_load_synthdef_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        ELKLOG_LOG_ERROR("SuperCollider: cannot open SynthDef file '{}'", path);
        _set_status("error: synthdef not found");
        return false;
    }

    auto size = static_cast<std::streamsize>(file.tellg());
    if (size <= 0)
    {
        _set_status("error: empty synthdef");
        return false;
    }
    file.seekg(0, std::ios::beg);

    std::vector<char> data(static_cast<size_t>(size));
    if (!file.read(data.data(), size))
    {
        _set_status("error: synthdef read failed");
        return false;
    }

    bool ok = _bridge.load_synthdef_blob(data.data(), static_cast<uint32_t>(size));
    ELKLOG_LOG_INFO("SuperCollider: loaded SynthDef '{}' ({} bytes): {}", path, size, ok ? "ok" : "failed");
    _set_status(ok ? "ok" : "error: d_recv failed");
    return ok;
}

void SuperColliderWrapper::_run_osc_script(const std::string& script)
{
    std::istringstream stream(script);
    std::string line;
    while (std::getline(stream, line))
    {
        // Trim leading whitespace; skip blank lines and '#' comments.
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos)
        {
            continue;
        }
        line = line.substr(start);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        if (!_bridge.send_osc_text(line))
        {
            ELKLOG_LOG_WARNING("SuperCollider: failed to send OSC command '{}'", line);
        }
    }
}

void SuperColliderWrapper::_set_status(const std::string& status)
{
    _status = status;
    _host_control.post_event(std::make_unique<PropertyChangeNotificationEvent>(
        id(), STATUS_PROPERTY_ID, _status, IMMEDIATE_PROCESS));
}

} // namespace sushi::internal::supercollider
