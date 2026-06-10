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

#ifndef SUSHI_SUPERCOLLIDER_WRAPPER_H
#define SUSHI_SUPERCOLLIDER_WRAPPER_H

#include <memory>
#include <string>

#include "library/processor.h"
#include "supersonic_bridge.h"

namespace sushi::internal::supercollider {

/**
 * @brief Hosts a SuperCollider (scsynth) synthesis engine via the embedded,
 *        JUCE-free Supersonic core.
 *
 * Control is over OSC, mirroring scsynth: a compiled SynthDef (.scsyndef) can be
 * loaded at startup via the "synthdef" path, an initial set of OSC commands can be
 * run from the plugin's source_code (one command per line), and arbitrary OSC
 * commands can be sent at runtime by writing to the "osc" property.
 *
 * NOTE: scsynth keeps process-global state, so only one SuperCollider processor
 * may exist per Sushi instance. A second one will fail to initialise.
 */
class SuperColliderWrapper : public Processor
{
public:
    static constexpr ObjectId SYNTHDEF_PATH_PROPERTY_ID = 1'000'000'000;
    static constexpr ObjectId OSC_PROPERTY_ID           = 1'000'000'001;
    static constexpr ObjectId STATUS_PROPERTY_ID        = 1'000'000'002;

    SuperColliderWrapper(HostControl host_control, PluginInfo plugin_info);
    ~SuperColliderWrapper() override;

    ProcessorReturnCode init(float sample_rate) override;
    void configure(float sample_rate) override;
    void process_event(const RtEvent& event) override;
    void process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer) override;

    PluginInfo info() const override;
    bool has_editor() const override { return false; }

    ProcessorReturnCode set_property_value(ObjectId property_id, const std::string& value) override;
    std::pair<ProcessorReturnCode, std::string> property_value(ObjectId property_id) const override;

private:
    bool _load_synthdef_file(const std::string& path);
    void _run_osc_script(const std::string& script);
    void _set_status(const std::string& status);

    PluginInfo _plugin_info;
    SupersonicBridge _bridge;
    float _sample_rate {44100.0f};

    std::string _synthdef_path;
    std::string _last_osc;
    std::string _status {"uninitialised"};
};

} // namespace sushi::internal::supercollider

#endif // SUSHI_SUPERCOLLIDER_WRAPPER_H
