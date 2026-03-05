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
 * @brief No-op stub implementation of PluginWindow for Linux/Windows.
 * @Copyright 2017-2023 Elk Audio AB, Stockholm
 */

#include "vst3x_plugin_window.h"

namespace sushi::internal::vst3 {

struct PluginWindow::Impl {};

PluginWindow::PluginWindow() : _impl(std::make_unique<Impl>()) {}

PluginWindow::~PluginWindow() = default;

void* PluginWindow::create(const std::string& /*title*/, int /*width*/, int /*height*/, bool /*resizable*/)
{
    return nullptr;
}

void PluginWindow::resize(int /*width*/, int /*height*/) {}

void PluginWindow::show() {}

void PluginWindow::close() {}

bool PluginWindow::is_open() const
{
    return false;
}

void PluginWindow::set_resize_callback(WindowResizeCallback /*callback*/) {}

void PluginWindow::set_position(int /*x*/, int /*y*/) {}

void PluginWindow::get_frame(int& x, int& y, int& width, int& height) const
{
    x = y = width = height = 0;
}

bool PluginWindow::capture_screenshot(const std::string& /*output_path*/, int /*max_width*/, int /*max_height*/) const
{
    return false;
}

} // end namespace sushi::internal::vst3
