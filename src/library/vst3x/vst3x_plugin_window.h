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
 * @brief Platform window abstraction for hosting VST3 plugin editors.
 * @Copyright 2017-2023 Elk Audio AB, Stockholm
 */

#ifndef SUSHI_VST3X_PLUGIN_WINDOW_H
#define SUSHI_VST3X_PLUGIN_WINDOW_H

#include <functional>
#include <memory>
#include <string>

namespace sushi::internal::vst3 {

using WindowResizeCallback = std::function<void(int width, int height)>;

class PluginWindow
{
public:
    PluginWindow();
    ~PluginWindow();

    PluginWindow(const PluginWindow&) = delete;
    PluginWindow& operator=(const PluginWindow&) = delete;

    void* create(const std::string& title, int width, int height, bool resizable = false);
    void resize(int width, int height);
    void show();
    void close();
    bool is_open() const;
    void set_resize_callback(WindowResizeCallback callback);
    void set_position(int x, int y);
    void get_frame(int& x, int& y, int& width, int& height) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // end namespace sushi::internal::vst3

#endif // SUSHI_VST3X_PLUGIN_WINDOW_H
