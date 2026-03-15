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

#include "elklog/static_logger.h"

namespace sushi::internal::faust_wrapper {

namespace {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("faust");

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
    if (rt && rt->dsp)
    {
        rt->dsp->init(static_cast<int>(sample_rate));
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
    if (!rt || !rt->dsp)
    {
        out_buffer.clear();
        return;
    }

    int in_channels = std::min(in_buffer.channel_count(), rt->dsp->getNumInputs());
    int out_channels = std::min(out_buffer.channel_count(), rt->dsp->getNumOutputs());

    // Set up input channel pointers
    std::array<FAUSTFLOAT*, 64> inputs {};
    for (int ch = 0; ch < in_channels; ++ch)
    {
        inputs[ch] = const_cast<FAUSTFLOAT*>(in_buffer.channel(ch));
    }
    // Provide silent buffers for any extra Faust inputs
    FAUSTFLOAT silence[AUDIO_CHUNK_SIZE] = {};
    for (int ch = in_channels; ch < rt->dsp->getNumInputs(); ++ch)
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
    for (int ch = out_channels; ch < rt->dsp->getNumOutputs(); ++ch)
    {
        outputs[ch] = scratch;
    }

    rt->dsp->compute(AUDIO_CHUNK_SIZE, inputs.data(), outputs.data());

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
    std::lock_guard<std::mutex> lock(_compile_lock);

    std::string error_msg;
    interpreter_dsp_factory* factory = nullptr;

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

    if (is_file)
    {
        factory = createInterpreterDSPFactoryFromFile(source, argc, argv_ptr, error_msg);
    }
    else
    {
        factory = createInterpreterDSPFactoryFromString("faust_dsp", source, argc, argv_ptr, error_msg);
    }

    if (!factory)
    {
        _compile_status = "error";
        _build_log = error_msg;
        ELKLOG_LOG_ERROR("Faust compilation failed: {}", error_msg);
        return false;
    }

    auto* dsp = factory->createDSPInstance();
    if (!dsp)
    {
        _compile_status = "error";
        _build_log = "Failed to create DSP instance";
        ELKLOG_LOG_ERROR("Failed to create Faust DSP instance");
        deleteInterpreterDSPFactory(factory);
        return false;
    }

    dsp->init(static_cast<int>(_sample_rate));

    // Discover parameters
    FaustUICollector ui_collector;
    dsp->buildUserInterface(&ui_collector);

    // Set up new runtime
    auto* new_runtime = new Runtime();
    new_runtime->factory = factory;
    new_runtime->dsp = dsp;
    new_runtime->parameters = ui_collector.parameters();

    new_runtime->zones.reserve(new_runtime->parameters.size());
    for (const auto& param : new_runtime->parameters)
    {
        new_runtime->zones.push_back(param.zone);
    }

    _max_input_channels = dsp->getNumInputs();
    _max_output_channels = dsp->getNumOutputs();

    _register_parameters(new_runtime->parameters);

    // Swap in the new runtime
    auto* old_runtime = _runtime.exchange(new_runtime, std::memory_order_acq_rel);
    if (old_runtime)
    {
        _delete_runtime(old_runtime);
    }

    _compile_status = "ok";
    _build_log.clear();
    ELKLOG_LOG_INFO("Faust DSP compiled successfully ({} inputs, {} outputs, {} parameters)",
                    dsp->getNumInputs(), dsp->getNumOutputs(), new_runtime->parameters.size());

    return true;
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

        auto* descriptor = new FloatParameterDescriptor(
            param.label,
            param.label,
            "",
            param.min,
            param.max,
            Direction::AUTOMATABLE,
            new FloatParameterPreProcessor(param.min, param.max));

        register_parameter(descriptor, static_cast<ObjectId>(i));
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
    if (runtime->dsp)
    {
        delete runtime->dsp;
    }
    if (runtime->factory)
    {
        deleteInterpreterDSPFactory(runtime->factory);
    }
    delete runtime;
}

} // namespace sushi::internal::faust_wrapper
