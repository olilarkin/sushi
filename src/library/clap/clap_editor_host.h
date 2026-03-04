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

#ifndef SUSHI_CLAP_EDITOR_HOST_H
#define SUSHI_CLAP_EDITOR_HOST_H

#include <functional>

#include "sushi/constants.h"

#include <clap/clap.h>
#include <clap/ext/gui.h>

#include "library/id_generator.h"

namespace sushi::internal::clap_wrapper {

struct ClapEditorRect
{
    int width;
    int height;
};

using ClapEditorResizeCallback = std::function<bool(int processor_id, int width, int height)>;

class ClapEditorHost
{
public:
    SUSHI_DECLARE_NON_COPYABLE(ClapEditorHost);

    ClapEditorHost(const clap_plugin_t* plugin,
                   const clap_plugin_gui_t* gui,
                   ObjectId processor_id,
                   ClapEditorResizeCallback resize_callback);

    ~ClapEditorHost();

    std::pair<bool, ClapEditorRect> open(void* parent_handle);
    void close();
    bool is_open() const;

private:
    const clap_plugin_t* _plugin;
    const clap_plugin_gui_t* _gui;
    ObjectId _processor_id;
    ClapEditorResizeCallback _resize_callback;
    bool _is_open{false};
};

} // end namespace sushi::internal::clap_wrapper

#endif // SUSHI_CLAP_EDITOR_HOST_H
