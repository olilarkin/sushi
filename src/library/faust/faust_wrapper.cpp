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

#include "faust_wrapper.h"

#include "faust/dsp/interpreter-dsp.h"
#ifdef SUSHI_FAUST_WITH_LLVM
#include "faust/dsp/llvm-dsp.h"
#endif
#include "faust/gui/JSONUI.h"

#include "elklog/static_logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace sushi::internal::faust_wrapper {

namespace {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("faust");

// Parse Faust menu metadata: "menu{'label0':0;'label1':1}" or "menu{label0:0;label1:1}"
std::map<int, std::string> parse_menu_style(const std::string& style)
{
    std::map<int, std::string> labels;
    auto brace_start = style.find('{');
    auto brace_end = style.rfind('}');
    if (brace_start == std::string::npos || brace_end == std::string::npos)
        return labels;

    std::string content = style.substr(brace_start + 1, brace_end - brace_start - 1);

    size_t pos = 0;
    while (pos < content.size())
    {
        // Skip whitespace
        while (pos < content.size() && content[pos] == ' ') pos++;

        // Skip optional quote
        bool quoted = (pos < content.size() && content[pos] == '\'');
        if (quoted) pos++;

        // Read label
        size_t label_start = pos;
        size_t label_end;
        if (quoted)
        {
            label_end = content.find('\'', pos);
            if (label_end == std::string::npos) break;
            pos = label_end + 1;
        }
        else
        {
            label_end = content.find(':', pos);
            if (label_end == std::string::npos) break;
            pos = label_end;
        }

        std::string label = content.substr(label_start, label_end - label_start);

        // Skip colon
        if (pos < content.size() && content[pos] == ':') pos++;

        // Read value
        size_t val_start = pos;
        while (pos < content.size() && content[pos] != ';') pos++;
        int value = std::atoi(content.substr(val_start, pos - val_start).c_str());

        if (!label.empty())
            labels[value] = label;

        // Skip semicolon
        if (pos < content.size() && content[pos] == ';') pos++;
    }

    return labels;
}

bool is_bool_param(const FaustParameterInfo& param)
{
    return param.is_button || (param.min == 0 && param.max == 1 && param.step == 1);
}

bool is_int_param(const FaustParameterInfo& param)
{
    return param.step >= 1.0f
        && param.min == std::floor(param.min)
        && param.max == std::floor(param.max);
}

} // namespace

FaustWrapper::FaustWrapper(HostControl host_control, PluginInfo plugin_info)
    : Processor(host_control)
    , _plugin_info(std::move(plugin_info))
{
#ifdef FAUST_LIBRARIES_DIR
    _faust_libraries_dir = FAUST_LIBRARIES_DIR;
#endif

    register_parameter(new StringPropertyDescriptor(
        "source_code", "Source Code", ""),
        SOURCE_CODE_PROPERTY_ID);
    register_parameter(new StringPropertyDescriptor(
        "source_path", "Source Path", ""),
        SOURCE_PATH_PROPERTY_ID);
    register_parameter(new StringPropertyDescriptor(
        "compile_status", "Compile Status", ""),
        COMPILE_STATUS_PROPERTY_ID);
    register_parameter(new StringPropertyDescriptor(
        "build_log", "Build Log", ""),
        BUILD_LOG_PROPERTY_ID);
}

FaustWrapper::~FaustWrapper()
{
    auto* rt = _runtime.load(std::memory_order_acquire);
    if (rt)
    {
        _delete_runtime(rt);
    }
}

ProcessorReturnCode FaustWrapper::init(float sample_rate)
{
    _sample_rate = sample_rate;

    if (_plugin_info.backend == "interpreter")
    {
        _backend = FaustBackend::INTERPRETER;
    }
    else
    {
        // Default to LLVM when available (better performance)
#ifdef SUSHI_FAUST_WITH_LLVM
        _backend = FaustBackend::LLVM;
#else
        if (_plugin_info.backend == "llvm")
        {
            ELKLOG_LOG_ERROR("Faust LLVM backend requested but not compiled in");
            return ProcessorReturnCode::PLUGIN_INIT_ERROR;
        }
        _backend = FaustBackend::INTERPRETER;
#endif
    }
    ELKLOG_LOG_WARNING("Faust using {} backend", _backend == FaustBackend::LLVM ? "LLVM" : "interpreter");
    _llvm_opt_level = _plugin_info.llvm_opt_level;

    if (!_plugin_info.source_code.empty())
    {
        _source_code = _plugin_info.source_code;
        if (!_compile(_source_code, false))
        {
            return ProcessorReturnCode::PLUGIN_INIT_ERROR;
        }
    }
    else if (!_plugin_info.path.empty())
    {
        _source_path = _plugin_info.path;
        if (!_compile(_source_path, true))
        {
            return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
        }
    }
    else
    {
        ELKLOG_LOG_ERROR("No Faust source code or path provided");
        return ProcessorReturnCode::PLUGIN_LOAD_ERROR;
    }

    return ProcessorReturnCode::OK;
}

void FaustWrapper::configure(float sample_rate)
{
    _sample_rate = sample_rate;

    std::lock_guard<std::mutex> lock(_compile_lock);
    auto* rt = _load_runtime();
    if (rt && rt->dsp_instance)
    {
        rt->dsp_instance->init(static_cast<int>(sample_rate));
    }
}

void FaustWrapper::process_event(const RtEvent& event)
{
    switch (event.type())
    {
        case RtEventType::FLOAT_PARAMETER_CHANGE:
        case RtEventType::INT_PARAMETER_CHANGE:
        case RtEventType::BOOL_PARAMETER_CHANGE:
        {
            auto typed_event = event.parameter_change_event();
            auto param_id = typed_event->param_id();
            auto* rt = _load_runtime();
            if (rt && param_id < rt->zones.size())
            {
                auto& param_info = rt->parameters[param_id];
                float normalized = std::clamp(typed_event->value(), 0.0f, 1.0f);
                FAUSTFLOAT domain_value = param_info.min + normalized * (param_info.max - param_info.min);
                *rt->zones[param_id] = domain_value;
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

void FaustWrapper::process_audio(const ChunkSampleBuffer& in_buffer, ChunkSampleBuffer& out_buffer)
{
    if (this->bypassed())
    {
        this->bypass_process(in_buffer, out_buffer);
        return;
    }

    auto* rt = _load_runtime();
    if (!rt || !rt->dsp_instance)
    {
        out_buffer.clear();
        return;
    }

    int in_channels = std::min(in_buffer.channel_count(), rt->dsp_instance->getNumInputs());
    int out_channels = std::min(out_buffer.channel_count(), rt->dsp_instance->getNumOutputs());

    // Set up input channel pointers
    std::array<FAUSTFLOAT*, 64> inputs {};
    for (int ch = 0; ch < in_channels; ++ch)
    {
        inputs[ch] = const_cast<FAUSTFLOAT*>(in_buffer.channel(ch));
    }
    // Provide silent buffers for any extra Faust inputs
    FAUSTFLOAT silence[AUDIO_CHUNK_SIZE] = {};
    for (int ch = in_channels; ch < rt->dsp_instance->getNumInputs(); ++ch)
    {
        inputs[ch] = silence;
    }

    // Set up output channel pointers
    std::array<FAUSTFLOAT*, 64> outputs {};
    for (int ch = 0; ch < out_channels; ++ch)
    {
        outputs[ch] = out_buffer.channel(ch);
    }
    // Provide scratch buffers for any extra Faust outputs
    FAUSTFLOAT scratch[AUDIO_CHUNK_SIZE] = {};
    for (int ch = out_channels; ch < rt->dsp_instance->getNumOutputs(); ++ch)
    {
        outputs[ch] = scratch;
    }

    rt->dsp_instance->compute(AUDIO_CHUNK_SIZE, inputs.data(), outputs.data());

    // Clear any output channels beyond what Faust provides
    for (int ch = out_channels; ch < out_buffer.channel_count(); ++ch)
    {
        std::memset(out_buffer.channel(ch), 0, AUDIO_CHUNK_SIZE * sizeof(float));
    }
}

PluginInfo FaustWrapper::info() const
{
    return _plugin_info;
}

ProcessorReturnCode FaustWrapper::set_property_value(ObjectId property_id, const std::string& value)
{
    if (property_id == SOURCE_CODE_PROPERTY_ID)
    {
        _source_code = value;
        return _compile(value, false) ? ProcessorReturnCode::OK : ProcessorReturnCode::ERROR;
    }
    else if (property_id == SOURCE_PATH_PROPERTY_ID)
    {
        _source_path = value;
        return _compile(value, true) ? ProcessorReturnCode::OK : ProcessorReturnCode::ERROR;
    }
    return ProcessorReturnCode::PARAMETER_NOT_FOUND;
}

std::pair<ProcessorReturnCode, std::string> FaustWrapper::property_value(ObjectId property_id) const
{
    switch (property_id)
    {
        case SOURCE_CODE_PROPERTY_ID:    return {ProcessorReturnCode::OK, _source_code};
        case SOURCE_PATH_PROPERTY_ID:    return {ProcessorReturnCode::OK, _source_path};
        case COMPILE_STATUS_PROPERTY_ID: return {ProcessorReturnCode::OK, _compile_status};
        case BUILD_LOG_PROPERTY_ID:      return {ProcessorReturnCode::OK, _build_log};
        default:                         return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
    }
}

bool FaustWrapper::_compile(const std::string& source, bool is_file)
{
    std::unique_lock<std::mutex> lock(_compile_lock);

    std::string error_msg;
    dsp_factory* factory = nullptr;

    std::vector<const char*> argv;
    std::string lib_path_arg;
    if (!_faust_libraries_dir.empty())
    {
        argv.push_back("-I");
        lib_path_arg = _faust_libraries_dir;
        argv.push_back(lib_path_arg.c_str());
    }

    int argc = static_cast<int>(argv.size());
    const char** argv_ptr = argv.empty() ? nullptr : argv.data();

#ifdef SUSHI_FAUST_WITH_LLVM
    if (_backend == FaustBackend::LLVM)
    {
        if (is_file)
        {
            factory = createDSPFactoryFromFile(source, argc, argv_ptr, "", error_msg, _llvm_opt_level);
        }
        else
        {
            factory = createDSPFactoryFromString("faust_dsp", source, argc, argv_ptr, "", error_msg, _llvm_opt_level);
        }
    }
    else
#endif
    {
        if (is_file)
        {
            factory = createInterpreterDSPFactoryFromFile(source, argc, argv_ptr, error_msg);
        }
        else
        {
            factory = createInterpreterDSPFactoryFromString("faust_dsp", source, argc, argv_ptr, error_msg);
        }
    }

    if (!factory)
    {
        _compile_status = "error";
        _build_log = error_msg;
        ELKLOG_LOG_ERROR("Faust compilation failed: {}", error_msg);
        return false;
    }

    auto* dsp_inst = factory->createDSPInstance();
    if (!dsp_inst)
    {
        _compile_status = "error";
        _build_log = "Failed to create DSP instance";
        ELKLOG_LOG_ERROR("Failed to create Faust DSP instance");
#ifdef SUSHI_FAUST_WITH_LLVM
        if (_backend == FaustBackend::LLVM)
        {
            deleteDSPFactory(static_cast<llvm_dsp_factory*>(factory));
        }
        else
#endif
        {
            deleteInterpreterDSPFactory(static_cast<interpreter_dsp_factory*>(factory));
        }
        return false;
    }

    dsp_inst->init(static_cast<int>(_sample_rate));

    // Discover parameters
    FaustUICollector ui_collector;
    dsp_inst->buildUserInterface(&ui_collector);

    // Set up new runtime
    auto* new_runtime = new Runtime();
    new_runtime->factory = factory;
    new_runtime->dsp_instance = dsp_inst;
    new_runtime->backend = _backend;
    new_runtime->parameters = ui_collector.parameters();

    new_runtime->zones.reserve(new_runtime->parameters.size());
    for (const auto& param : new_runtime->parameters)
    {
        new_runtime->zones.push_back(param.zone);
    }

    _max_input_channels = dsp_inst->getNumInputs();
    _max_output_channels = dsp_inst->getNumOutputs();

    _register_parameters(new_runtime->parameters);

    // Parse menu labels from metadata
    new_runtime->menu_labels.resize(new_runtime->parameters.size());
    for (size_t i = 0; i < new_runtime->parameters.size(); ++i)
    {
        auto style_it = new_runtime->parameters[i].metadata.find("style");
        if (style_it != new_runtime->parameters[i].metadata.end()
            && style_it->second.substr(0, 5) == "menu{")
        {
            new_runtime->menu_labels[i] = parse_menu_style(style_it->second);
        }
    }

    // Swap in the new runtime
    auto* old_runtime = _runtime.exchange(new_runtime, std::memory_order_acq_rel);

    _compile_status = "ok";
    _build_log.clear();
    ELKLOG_LOG_WARNING("Faust DSP compiled successfully via {} ({} inputs, {} outputs, {} parameters)",
                    _backend == FaustBackend::LLVM ? "LLVM" : "interpreter",
                    dsp_inst->getNumInputs(), dsp_inst->getNumOutputs(), new_runtime->parameters.size());

    // Copy callback and release lock before invoking — the callback may call ui_json()
    // which also takes _compile_lock
    auto recompile_cb = _editor_recompile_callback;
    lock.unlock();

    // Invoke recompile callback before deleting old runtime so zone pointers remain valid
    // until the editor has rebound to the new ones
    if (recompile_cb)
    {
        recompile_cb();
    }

    if (old_runtime)
    {
        _delete_runtime(old_runtime);
    }

    return true;
}

std::string FaustWrapper::ui_json() const
{
    std::lock_guard<std::mutex> lock(_compile_lock);
    auto* rt = _runtime.load(std::memory_order_acquire);
    if (!rt || !rt->dsp_instance)
    {
        return {};
    }

    JSONUI jsonui;
    rt->dsp_instance->buildUserInterface(&jsonui);
    return jsonui.JSON();
}

const std::vector<FaustParameterInfo>& FaustWrapper::current_parameters() const
{
    static const std::vector<FaustParameterInfo> empty;
    auto* rt = _runtime.load(std::memory_order_acquire);
    if (!rt)
    {
        return empty;
    }
    return rt->parameters;
}

void FaustWrapper::set_editor_recompile_callback(EditorRecompileCallback callback)
{
    _editor_recompile_callback = std::move(callback);
}

std::pair<ProcessorReturnCode, float> FaustWrapper::parameter_value(ObjectId parameter_id) const
{
    auto* rt = const_cast<FaustWrapper*>(this)->_load_runtime();
    if (!rt || parameter_id >= rt->zones.size())
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};

    auto& param = rt->parameters[parameter_id];
    float range = param.max - param.min;
    if (range <= 0.0f) return {ProcessorReturnCode::OK, 0.0f};

    float domain_value = *rt->zones[parameter_id];
    float normalized = (domain_value - param.min) / range;
    return {ProcessorReturnCode::OK, std::clamp(normalized, 0.0f, 1.0f)};
}

std::pair<ProcessorReturnCode, float> FaustWrapper::parameter_value_in_domain(ObjectId parameter_id) const
{
    auto* rt = const_cast<FaustWrapper*>(this)->_load_runtime();
    if (!rt || parameter_id >= rt->zones.size())
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};

    return {ProcessorReturnCode::OK, static_cast<float>(*rt->zones[parameter_id])};
}

std::pair<ProcessorReturnCode, std::string> FaustWrapper::parameter_value_formatted(ObjectId parameter_id) const
{
    auto* rt = const_cast<FaustWrapper*>(this)->_load_runtime();
    if (!rt || parameter_id >= rt->zones.size())
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};

    auto [status, domain_val] = parameter_value_in_domain(parameter_id);
    if (status != ProcessorReturnCode::OK) return {status, ""};

    const auto& param = rt->parameters[parameter_id];

    if (is_bool_param(param))
    {
        return {ProcessorReturnCode::OK, domain_val >= 0.5f ? "On" : "Off"};
    }

    if (is_int_param(param))
    {
        int int_val = static_cast<int>(std::round(domain_val));
        // Check for menu labels
        if (parameter_id < rt->menu_labels.size() && !rt->menu_labels[parameter_id].empty())
        {
            auto it = rt->menu_labels[parameter_id].find(int_val);
            if (it != rt->menu_labels[parameter_id].end())
                return {ProcessorReturnCode::OK, it->second};
        }
        return {ProcessorReturnCode::OK, std::to_string(int_val)};
    }

    // Float formatting
    char buf[64];
    if (domain_val == std::floor(domain_val) && std::abs(domain_val) < 1e6f)
        snprintf(buf, sizeof(buf), "%.0f", domain_val);
    else
        snprintf(buf, sizeof(buf), "%.2f", domain_val);
    return {ProcessorReturnCode::OK, buf};
}

std::pair<ProcessorReturnCode, std::string> FaustWrapper::parameter_value_formatted(ObjectId parameter_id, float normalized_value) const
{
    auto* rt = const_cast<FaustWrapper*>(this)->_load_runtime();
    if (!rt || parameter_id >= rt->zones.size())
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};

    const auto& param = rt->parameters[parameter_id];
    float domain_val = param.min + normalized_value * (param.max - param.min);

    if (is_bool_param(param))
    {
        return {ProcessorReturnCode::OK, domain_val >= 0.5f ? "On" : "Off"};
    }

    if (is_int_param(param))
    {
        int int_val = static_cast<int>(std::round(domain_val));
        if (parameter_id < rt->menu_labels.size() && !rt->menu_labels[parameter_id].empty())
        {
            auto it = rt->menu_labels[parameter_id].find(int_val);
            if (it != rt->menu_labels[parameter_id].end())
                return {ProcessorReturnCode::OK, it->second};
        }
        return {ProcessorReturnCode::OK, std::to_string(int_val)};
    }

    char buf[64];
    if (domain_val == std::floor(domain_val) && std::abs(domain_val) < 1e6f)
        snprintf(buf, sizeof(buf), "%.0f", domain_val);
    else
        snprintf(buf, sizeof(buf), "%.2f", domain_val);
    return {ProcessorReturnCode::OK, buf};
}

void FaustWrapper::_register_parameters(const std::vector<FaustParameterInfo>& params)
{
    for (size_t i = 0; i < params.size(); ++i)
    {
        const auto& param = params[i];
        if (param.is_bargraph)
        {
            continue;
        }

        // Extract unit from metadata
        std::string unit;
        auto unit_it = param.metadata.find("unit");
        if (unit_it != param.metadata.end())
        {
            unit = unit_it->second;
        }

        if (is_bool_param(param))
        {
            auto* descriptor = new BoolParameterDescriptor(
                param.label,
                param.label,
                unit,
                false,
                true,
                Direction::AUTOMATABLE,
                new BoolParameterPreProcessor(false, true));

            register_parameter(descriptor, static_cast<ObjectId>(i));
        }
        else if (is_int_param(param))
        {
            auto* descriptor = new IntParameterDescriptor(
                param.label,
                param.label,
                unit,
                static_cast<int>(param.min),
                static_cast<int>(param.max),
                Direction::AUTOMATABLE,
                new IntParameterPreProcessor(static_cast<int>(param.min), static_cast<int>(param.max)));

            register_parameter(descriptor, static_cast<ObjectId>(i));
        }
        else
        {
            auto* descriptor = new FloatParameterDescriptor(
                param.label,
                param.label,
                unit,
                param.min,
                param.max,
                Direction::AUTOMATABLE,
                new FloatParameterPreProcessor(param.min, param.max));

            register_parameter(descriptor, static_cast<ObjectId>(i));
        }
    }
}

FaustWrapper::Runtime* FaustWrapper::_load_runtime()
{
    return _runtime.load(std::memory_order_acquire);
}

void FaustWrapper::_store_runtime(Runtime* runtime)
{
    _runtime.store(runtime, std::memory_order_release);
}

void FaustWrapper::_delete_runtime(Runtime* runtime)
{
    if (runtime->dsp_instance)
    {
        delete runtime->dsp_instance;
    }
    if (runtime->factory)
    {
#ifdef SUSHI_FAUST_WITH_LLVM
        if (runtime->backend == FaustBackend::LLVM)
        {
            deleteDSPFactory(static_cast<llvm_dsp_factory*>(runtime->factory));
        }
        else
#endif
        {
            deleteInterpreterDSPFactory(static_cast<interpreter_dsp_factory*>(runtime->factory));
        }
    }
    delete runtime;
}

} // namespace sushi::internal::faust_wrapper
