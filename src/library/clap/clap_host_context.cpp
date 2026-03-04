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
 * @brief CLAP host context and plugin instance management
 */

#include <dlfcn.h>
#include <filesystem>

#include "elklog/static_logger.h"

#include "clap_host_context.h"

namespace sushi::internal::clap_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("clap");

constexpr char HOST_NAME[] = "Sushi";
constexpr char HOST_VENDOR[] = "Elk Audio";
constexpr char HOST_VERSION[] = "1.0.0";

ClapHostContext::ClapHostContext()
{
    _host.clap_version = CLAP_VERSION;
    _host.host_data = this;
    _host.name = HOST_NAME;
    _host.vendor = HOST_VENDOR;
    _host.url = "";
    _host.version = HOST_VERSION;
    _host.get_extension = _get_extension;
    _host.request_restart = _request_restart;
    _host.request_process = _request_process;
    _host.request_callback = _request_callback;

    _host_log.log = _log;

    _host_params.rescan = _params_rescan;
    _host_params.clear = _params_clear;
    _host_params.request_flush = _params_request_flush;

    _host_state.mark_dirty = _state_mark_dirty;

    _host_gui.resize_hints_changed = _gui_resize_hints_changed;
    _host_gui.request_resize = _gui_request_resize;
    _host_gui.request_show = _gui_request_show;
    _host_gui.request_hide = _gui_request_hide;
    _host_gui.closed = _gui_closed;
}

const void* ClapHostContext::_get_extension(const clap_host_t* host, const char* extension_id)
{
    auto* self = static_cast<ClapHostContext*>(host->host_data);
    if (strcmp(extension_id, CLAP_EXT_LOG) == 0)
    {
        return &self->_host_log;
    }
    if (strcmp(extension_id, CLAP_EXT_PARAMS) == 0)
    {
        return &self->_host_params;
    }
    if (strcmp(extension_id, CLAP_EXT_STATE) == 0)
    {
        return &self->_host_state;
    }
    if (strcmp(extension_id, CLAP_EXT_GUI) == 0)
    {
        return &self->_host_gui;
    }
    ELKLOG_LOG_DEBUG("Plugin requested unsupported host extension: {}", extension_id);
    return nullptr;
}

void ClapHostContext::_request_restart(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin requested restart");
}

void ClapHostContext::_request_process(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin requested process");
}

void ClapHostContext::_request_callback(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin requested main thread callback");
}

void ClapHostContext::_log(const clap_host_t* /*host*/, clap_log_severity severity, const char* msg)
{
    switch (severity)
    {
        case CLAP_LOG_DEBUG:
            ELKLOG_LOG_DEBUG("CLAP plugin: {}", msg);
            break;
        case CLAP_LOG_INFO:
            ELKLOG_LOG_INFO("CLAP plugin: {}", msg);
            break;
        case CLAP_LOG_WARNING:
            ELKLOG_LOG_WARNING("CLAP plugin: {}", msg);
            break;
        case CLAP_LOG_ERROR:
        case CLAP_LOG_FATAL:
        case CLAP_LOG_HOST_MISBEHAVING:
        case CLAP_LOG_PLUGIN_MISBEHAVING:
            ELKLOG_LOG_ERROR("CLAP plugin: {}", msg);
            break;
        default:
            ELKLOG_LOG_INFO("CLAP plugin: {}", msg);
            break;
    }
}

void ClapHostContext::_params_rescan(const clap_host_t* /*host*/, clap_param_rescan_flags /*flags*/)
{
    ELKLOG_LOG_DEBUG("Plugin requested parameter rescan");
}

void ClapHostContext::_params_clear(const clap_host_t* /*host*/, clap_id /*param_id*/, clap_param_clear_flags /*flags*/)
{
    ELKLOG_LOG_DEBUG("Plugin requested parameter clear");
}

void ClapHostContext::_params_request_flush(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin requested parameter flush");
}

void ClapHostContext::_state_mark_dirty(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin marked state as dirty");
}

void ClapHostContext::_gui_resize_hints_changed(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin GUI resize hints changed");
}

bool ClapHostContext::_gui_request_resize(const clap_host_t* /*host*/, uint32_t width, uint32_t height)
{
    ELKLOG_LOG_DEBUG("Plugin GUI requested resize to {}x{}", width, height);
    return true;
}

bool ClapHostContext::_gui_request_show(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin GUI requested show");
    return true;
}

bool ClapHostContext::_gui_request_hide(const clap_host_t* /*host*/)
{
    ELKLOG_LOG_DEBUG("Plugin GUI requested hide");
    return true;
}

void ClapHostContext::_gui_closed(const clap_host_t* /*host*/, bool was_destroyed)
{
    ELKLOG_LOG_DEBUG("Plugin GUI closed (was_destroyed={})", was_destroyed);
}

// ClapPluginInstance

ClapPluginInstance::~ClapPluginInstance()
{
    if (_processing)
    {
        stop_processing();
    }
    if (_active)
    {
        deactivate();
    }
    if (_plugin)
    {
        _plugin->destroy(_plugin);
        _plugin = nullptr;
    }
    if (_entry_initialized && _entry)
    {
        _entry->deinit();
        _entry = nullptr;
    }
    if (_library_handle)
    {
        dlclose(_library_handle);
        _library_handle = nullptr;
    }
}

std::string ClapPluginInstance::_resolve_library_path(const std::string& plugin_path)
{
#ifdef __APPLE__
    // On macOS, .clap files are bundles: path/Contents/MacOS/<stem>
    std::filesystem::path bundle_path(plugin_path);
    auto stem = bundle_path.stem().string();
    auto lib_path = bundle_path / "Contents" / "MacOS" / stem;
    if (std::filesystem::exists(lib_path))
    {
        return lib_path.string();
    }
    // Fall back to direct path
#endif
    return plugin_path;
}

bool ClapPluginInstance::load_plugin(const std::string& plugin_path, int plugin_index, const clap_host_t* host)
{
    auto lib_path = _resolve_library_path(plugin_path);

    _library_handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!_library_handle)
    {
        ELKLOG_LOG_ERROR("Failed to open CLAP plugin library: {} ({})", lib_path, dlerror());
        return false;
    }

    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(dlsym(_library_handle, "clap_entry"));
    if (!entry)
    {
        ELKLOG_LOG_ERROR("Failed to find clap_entry symbol in {}", lib_path);
        return false;
    }
    _entry = entry;

    if (!_entry->init(plugin_path.c_str()))
    {
        ELKLOG_LOG_ERROR("Failed to initialize CLAP plugin entry in {}", plugin_path);
        _entry = nullptr;
        return false;
    }
    _entry_initialized = true;

    _factory = static_cast<const clap_plugin_factory_t*>(_entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!_factory)
    {
        ELKLOG_LOG_ERROR("Failed to get CLAP plugin factory from {}", plugin_path);
        return false;
    }

    auto plugin_count = _factory->get_plugin_count(_factory);
    if (plugin_count == 0)
    {
        ELKLOG_LOG_ERROR("No plugins found in {}", plugin_path);
        return false;
    }

    if (plugin_index < 0 || static_cast<uint32_t>(plugin_index) >= plugin_count)
    {
        ELKLOG_LOG_ERROR("Plugin index {} out of range (0-{})", plugin_index, plugin_count - 1);
        return false;
    }

    auto* desc = _factory->get_plugin_descriptor(_factory, static_cast<uint32_t>(plugin_index));
    if (!desc)
    {
        ELKLOG_LOG_ERROR("Failed to get plugin descriptor at index {}", plugin_index);
        return false;
    }

    ELKLOG_LOG_INFO("Loading CLAP plugin: {} ({})", desc->name, desc->id);

    _plugin = _factory->create_plugin(_factory, host, desc->id);
    if (!_plugin)
    {
        ELKLOG_LOG_ERROR("Failed to create CLAP plugin instance for {}", desc->id);
        return false;
    }

    if (!_plugin->init(_plugin))
    {
        ELKLOG_LOG_ERROR("Failed to initialize CLAP plugin {}", desc->name);
        _plugin->destroy(_plugin);
        _plugin = nullptr;
        return false;
    }

    _query_extensions();
    return true;
}

void ClapPluginInstance::_query_extensions()
{
    _params = static_cast<const clap_plugin_params_t*>(
        _plugin->get_extension(_plugin, CLAP_EXT_PARAMS));
    if (_params)
    {
        ELKLOG_LOG_INFO("Plugin supports params extension");
    }

    _state = static_cast<const clap_plugin_state_t*>(
        _plugin->get_extension(_plugin, CLAP_EXT_STATE));
    if (_state)
    {
        ELKLOG_LOG_INFO("Plugin supports state extension");
    }

    _audio_ports = static_cast<const clap_plugin_audio_ports_t*>(
        _plugin->get_extension(_plugin, CLAP_EXT_AUDIO_PORTS));
    if (_audio_ports)
    {
        ELKLOG_LOG_INFO("Plugin supports audio ports extension");
    }

    _note_ports = static_cast<const clap_plugin_note_ports_t*>(
        _plugin->get_extension(_plugin, CLAP_EXT_NOTE_PORTS));
    if (_note_ports)
    {
        ELKLOG_LOG_INFO("Plugin supports note ports extension");
    }

    _gui = static_cast<const clap_plugin_gui_t*>(
        _plugin->get_extension(_plugin, CLAP_EXT_GUI));
    if (_gui)
    {
        ELKLOG_LOG_INFO("Plugin supports GUI extension");
    }
}

bool ClapPluginInstance::activate(double sample_rate, uint32_t min_frames, uint32_t max_frames)
{
    if (_active)
    {
        return true;
    }
    if (!_plugin->activate(_plugin, sample_rate, min_frames, max_frames))
    {
        ELKLOG_LOG_ERROR("Failed to activate CLAP plugin");
        return false;
    }
    _active = true;
    return true;
}

void ClapPluginInstance::deactivate()
{
    if (!_active)
    {
        return;
    }
    if (_processing)
    {
        stop_processing();
    }
    _plugin->deactivate(_plugin);
    _active = false;
}

bool ClapPluginInstance::start_processing()
{
    if (_processing)
    {
        return true;
    }
    if (!_plugin->start_processing(_plugin))
    {
        ELKLOG_LOG_ERROR("Failed to start CLAP plugin processing");
        return false;
    }
    _processing = true;
    return true;
}

void ClapPluginInstance::stop_processing()
{
    if (!_processing)
    {
        return;
    }
    _plugin->stop_processing(_plugin);
    _processing = false;
}

clap_process_status ClapPluginInstance::process(const clap_process_t* process_data)
{
    return _plugin->process(_plugin, process_data);
}

} // end namespace sushi::internal::clap_wrapper
