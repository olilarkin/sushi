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

#ifndef SUSHI_CMAJOR_WRAPPER_H
#define SUSHI_CMAJOR_WRAPPER_H

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cmajor/API/cmaj_Performer.h"
#include "cmajor/helpers/cmaj_PatchHelpers.h"
#include "choc/containers/choc_Value.h"

#include "library/processor.h"

namespace sushi::internal::cmajor_plugin {

class CmajorWrapper : public Processor
{
public:
    CmajorWrapper(HostControl host_control, PluginInfo plugin_info);
    ~CmajorWrapper() override;

    ProcessorReturnCode init(float sample_rate) override;
    void configure(float sample_rate) override;
    void process_event(const RtEvent& event) override;
    void process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer) override;
    void set_channels(int inputs, int outputs) override;

    int parameter_count() const override;
    const ParameterDescriptor* parameter_from_name(const std::string& name) const override;
    const ParameterDescriptor* parameter_from_id(ObjectId id) const override;
    std::vector<ParameterDescriptor*> all_parameters() const override;

    std::pair<ProcessorReturnCode, float> parameter_value(ObjectId parameter_id) const override;
    std::pair<ProcessorReturnCode, float> parameter_value_in_domain(ObjectId parameter_id) const override;
    std::pair<ProcessorReturnCode, std::string> parameter_value_formatted(ObjectId parameter_id) const override;

    std::pair<ProcessorReturnCode, std::string> property_value(ObjectId property_id) const override;
    ProcessorReturnCode set_property_value(ObjectId property_id, const std::string& value) override;

    ProcessorReturnCode set_state(ProcessorState* state, bool realtime_running) override;
    ProcessorState save_state() const override;
    PluginInfo info() const override;

private:
    enum class CompileStatus
    {
        EMPTY,
        READY,
        ERROR
    };

    struct PropertySlot
    {
        std::unique_ptr<StringPropertyDescriptor> descriptor;
        std::string value;
    };

    struct ParameterState
    {
        ParameterState(cmaj::PatchParameterProperties properties_,
                       choc::value::Type value_type_)
            : properties(std::move(properties_)),
              value_type(std::move(value_type_))
        {
        }

        std::unique_ptr<ParameterDescriptor> descriptor;
        std::string endpoint_id;
        std::string signature;
        cmaj::PatchParameterProperties properties;
        choc::value::Type value_type;
        bool is_event = false;
        bool input = true;
        std::atomic<float> normalized_value {0.0f};
        std::atomic<float> domain_value {0.0f};
    };

    struct Runtime;

    static constexpr ObjectId SOURCE_PATH_PROPERTY_ID = 1'000'000'000u;
    static constexpr ObjectId SOURCE_CODE_PROPERTY_ID = 1'000'000'001u;
    static constexpr ObjectId COMPILE_STATUS_PROPERTY_ID = 1'000'000'002u;
    static constexpr ObjectId BUILD_LOG_PROPERTY_ID = 1'000'000'003u;
    static constexpr ObjectId PROGRAM_DETAILS_PROPERTY_ID = 1'000'000'004u;

    void _register_properties();
    void _set_property_value_locked(ObjectId property_id, const std::string& value, bool notify);
    void _set_compile_feedback_locked(CompileStatus status, std::string build_log, std::string program_details, bool notify);

    ProcessorReturnCode _compile_current_source(bool fail_if_empty);
    ProcessorReturnCode _compile_source(const std::string& source_path,
                                        const std::string& source_code,
                                        bool fail_if_empty);

    std::pair<ProcessorReturnCode, std::string> _load_source_from_path(const std::string& source_path);

    void _install_runtime(std::shared_ptr<Runtime> runtime);
    void _apply_parameter_value_to_runtime(const Runtime& runtime, ObjectId parameter_id, float normalized_value) const;
    void _apply_all_parameter_values_to_runtime(const Runtime& runtime) const;
    void _restore_named_state_from_binary(const std::vector<std::byte>& binary_data);
    std::shared_ptr<Runtime> _load_runtime() const;
    void _store_runtime(std::shared_ptr<Runtime> runtime);

    std::string _compile_status_to_string(CompileStatus status) const;
    std::string _create_program_details_json(const Runtime& runtime) const;
    std::vector<std::byte> _create_named_state_binary() const;

    mutable std::mutex _state_lock;
    std::mutex _compile_lock;
    std::shared_ptr<Runtime> _runtime;

    std::unordered_map<ObjectId, PropertySlot> _properties;
    std::unordered_map<ObjectId, std::unique_ptr<ParameterState>> _parameters_by_id;
    std::unordered_map<std::string, ObjectId> _parameter_ids_by_name;
    std::vector<ObjectId> _active_parameter_ids;
    ObjectId _next_parameter_id {0};

    std::string _source_path;
    std::string _source_code;
    PluginInfo _plugin_info;

    int _requested_input_channels {0};
    int _requested_output_channels {0};
    float _sample_rate {44100.0f};

    static constexpr size_t MAX_PENDING_MIDI_EVENTS = 256;
    std::array<MidiDataByte, MAX_PENDING_MIDI_EVENTS> _pending_midi_messages {};
    std::array<int, MAX_PENDING_MIDI_EVENTS> _pending_midi_offsets {};
    size_t _pending_midi_count {0};
};

} // namespace sushi::internal::cmajor_plugin

#endif // SUSHI_CMAJOR_WRAPPER_H
