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
 * @brief CLAP plugin GUI lifecycle manager.
 */

#include "elklog/static_logger.h"

#include "clap_editor_host.h"

namespace sushi::internal::clap_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("clap");

namespace {

const char* platform_gui_api()
{
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

} // anonymous namespace

ClapEditorHost::ClapEditorHost(const clap_plugin_t* plugin,
                               const clap_plugin_gui_t* gui,
                               ObjectId processor_id,
                               ClapEditorResizeCallback resize_callback)
    : _plugin(plugin)
    , _gui(gui)
    , _processor_id(processor_id)
    , _resize_callback(std::move(resize_callback))
{
}

ClapEditorHost::~ClapEditorHost()
{
    if (_is_open)
    {
        close();
    }
}

std::pair<bool, ClapEditorRect> ClapEditorHost::open(void* parent_handle)
{
    if (_is_open)
    {
        return {false, {0, 0}};
    }

    auto* api = platform_gui_api();

    if (!_gui->is_api_supported(_plugin, api, false))
    {
        ELKLOG_LOG_ERROR("CLAP plugin does not support {} GUI API (embedded)", api);
        return {false, {0, 0}};
    }

    if (!_gui->create(_plugin, api, false))
    {
        ELKLOG_LOG_ERROR("Failed to create CLAP plugin GUI");
        return {false, {0, 0}};
    }

    uint32_t width = 0;
    uint32_t height = 0;
    if (!_gui->get_size(_plugin, &width, &height))
    {
        ELKLOG_LOG_WARNING("Failed to get CLAP plugin GUI size, using defaults");
        width = 640;
        height = 480;
    }

    clap_window_t window{};
    window.api = api;
    window.cocoa = parent_handle;

    if (!_gui->set_parent(_plugin, &window))
    {
        ELKLOG_LOG_ERROR("Failed to set CLAP plugin GUI parent window");
        _gui->destroy(_plugin);
        return {false, {0, 0}};
    }

    if (!_gui->show(_plugin))
    {
        ELKLOG_LOG_WARNING("CLAP plugin gui->show() returned false, continuing anyway");
    }

    _is_open = true;
    ELKLOG_LOG_INFO("CLAP plugin GUI opened: {}x{}", width, height);
    return {true, {static_cast<int>(width), static_cast<int>(height)}};
}

void ClapEditorHost::close()
{
    if (!_is_open)
    {
        return;
    }
    _gui->hide(_plugin);
    _gui->destroy(_plugin);
    _is_open = false;
    ELKLOG_LOG_INFO("CLAP plugin GUI closed");
}

bool ClapEditorHost::is_open() const
{
    return _is_open;
}

} // end namespace sushi::internal::clap_wrapper
