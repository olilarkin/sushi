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
 * @brief CLAP host context and plugin instance management
 */

#ifndef SUSHI_CLAP_HOST_CONTEXT_H
#define SUSHI_CLAP_HOST_CONTEXT_H

#include <string>
#include <atomic>

#include "sushi/constants.h"

#include <clap/clap.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/log.h>
#include <clap/ext/gui.h>

namespace sushi::internal::clap_wrapper {

class ClapWrapper;

class ClapHostContext
{
public:
    SUSHI_DECLARE_NON_COPYABLE(ClapHostContext);

    ClapHostContext();

    const clap_host_t* host() { return &_host; }

    void set_wrapper(ClapWrapper* wrapper) { _wrapper = wrapper; }

private:
    static const void* _get_extension(const clap_host_t* host, const char* extension_id);
    static void _request_restart(const clap_host_t* host);
    static void _request_process(const clap_host_t* host);
    static void _request_callback(const clap_host_t* host);

    static void _log(const clap_host_t* host, clap_log_severity severity, const char* msg);

    static void _params_rescan(const clap_host_t* host, clap_param_rescan_flags flags);
    static void _params_clear(const clap_host_t* host, clap_id param_id, clap_param_clear_flags flags);
    static void _params_request_flush(const clap_host_t* host);

    static void _state_mark_dirty(const clap_host_t* host);

    static void _gui_resize_hints_changed(const clap_host_t* host);
    static bool _gui_request_resize(const clap_host_t* host, uint32_t width, uint32_t height);
    static bool _gui_request_show(const clap_host_t* host);
    static bool _gui_request_hide(const clap_host_t* host);
    static void _gui_closed(const clap_host_t* host, bool was_destroyed);

    clap_host_t _host{};
    clap_host_log_t _host_log{};
    clap_host_params_t _host_params{};
    clap_host_state_t _host_state{};
    clap_host_gui_t _host_gui{};

    ClapWrapper* _wrapper{nullptr};
};

class ClapPluginInstance
{
public:
    SUSHI_DECLARE_NON_COPYABLE(ClapPluginInstance);

    ClapPluginInstance() = default;
    ~ClapPluginInstance();

    bool load_plugin(const std::string& plugin_path, int plugin_index, const clap_host_t* host);

    bool activate(double sample_rate, uint32_t min_frames, uint32_t max_frames);
    void deactivate();

    bool start_processing();
    void stop_processing();

    clap_process_status process(const clap_process_t* process_data);

    const clap_plugin_t* plugin() const { return _plugin; }
    const clap_plugin_descriptor_t* descriptor() const { return _plugin ? _plugin->desc : nullptr; }

    const clap_plugin_params_t* params() const { return _params; }
    const clap_plugin_state_t* state() const { return _state; }
    const clap_plugin_audio_ports_t* audio_ports() const { return _audio_ports; }
    const clap_plugin_note_ports_t* note_ports() const { return _note_ports; }
    const clap_plugin_gui_t* gui() const { return _gui; }

    bool is_active() const { return _active; }
    bool is_processing() const { return _processing; }

private:
    std::string _resolve_library_path(const std::string& plugin_path);
    void _query_extensions();

    void* _library_handle{nullptr};
    const clap_plugin_entry_t* _entry{nullptr};
    const clap_plugin_factory_t* _factory{nullptr};
    const clap_plugin_t* _plugin{nullptr};

    const clap_plugin_params_t* _params{nullptr};
    const clap_plugin_state_t* _state{nullptr};
    const clap_plugin_audio_ports_t* _audio_ports{nullptr};
    const clap_plugin_note_ports_t* _note_ports{nullptr};
    const clap_plugin_gui_t* _gui{nullptr};

    bool _active{false};
    bool _processing{false};
    bool _entry_initialized{false};
};

} // end namespace sushi::internal::clap_wrapper

#endif // SUSHI_CLAP_HOST_CONTEXT_H
