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

#ifndef SUSHI_FAUST_WRAPPER_H
#define SUSHI_FAUST_WRAPPER_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "library/processor.h"
#include "faust_ui.h"

class interpreter_dsp_factory;
class interpreter_dsp;

namespace sushi::internal::faust_wrapper {

class FaustWrapper : public Processor
{
public:
    static constexpr ObjectId SOURCE_CODE_PROPERTY_ID = 1'000'000'000;
    static constexpr ObjectId SOURCE_PATH_PROPERTY_ID = 1'000'000'001;
    static constexpr ObjectId COMPILE_STATUS_PROPERTY_ID = 1'000'000'002;
    static constexpr ObjectId BUILD_LOG_PROPERTY_ID = 1'000'000'003;

    FaustWrapper(HostControl host_control, PluginInfo plugin_info);
    ~FaustWrapper() override;

    ProcessorReturnCode init(float sample_rate) override;
    void configure(float sample_rate) override;
    void process_event(const RtEvent& event) override;
    void process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer) override;

    using EditorRecompileCallback = std::function<void()>;

    PluginInfo info() const override;
#if defined(SUSHI_BUILD_WITH_FAUST) && defined(__APPLE__)
    bool has_editor() const override { return true; }
#else
    bool has_editor() const override { return false; }
#endif

    std::string ui_json() const;
    const std::vector<FaustParameterInfo>& current_parameters() const;
    void set_editor_recompile_callback(EditorRecompileCallback callback);

    ProcessorReturnCode set_property_value(ObjectId property_id, const std::string& value) override;
    std::pair<ProcessorReturnCode, std::string> property_value(ObjectId property_id) const override;

private:
    struct Runtime
    {
        interpreter_dsp_factory* factory {nullptr};
        interpreter_dsp* dsp {nullptr};
        std::vector<FaustParameterInfo> parameters;
        std::vector<FAUSTFLOAT*> zones;
    };

    bool _compile(const std::string& source, bool is_file);
    void _register_parameters(const std::vector<FaustParameterInfo>& params);
    Runtime* _load_runtime();
    void _store_runtime(Runtime* runtime);
    void _delete_runtime(Runtime* runtime);

    PluginInfo _plugin_info;
    float _sample_rate {44100.0f};
    std::atomic<Runtime*> _runtime {nullptr};
    mutable std::mutex _compile_lock;

    std::string _source_code;
    std::string _source_path;
    std::string _compile_status {"uncompiled"};
    std::string _build_log;
    std::string _faust_libraries_dir;
    EditorRecompileCallback _editor_recompile_callback;
};

} // namespace sushi::internal::faust_wrapper

#endif // SUSHI_FAUST_WRAPPER_H
