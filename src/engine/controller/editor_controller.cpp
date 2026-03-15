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
 * @brief Implementation of EditorController for managing plugin editor views.
 * @Copyright 2026 Oliver Larkin
 */

#include "elklog/static_logger.h"

#include "editor_controller.h"

#ifdef SUSHI_BUILD_WITH_VST3
#include "library/vst3x/vst3x_wrapper.h"
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
#include "library/clap/clap_wrapper.h"
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
#include "library/auv2/auv2_wrapper.h"
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
#include "library/cmajor/cmajor_wrapper.h"
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
#include "library/jsfx/jsfx_wrapper.h"
#endif

#if defined(__APPLE__) && (defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || defined(SUSHI_BUILD_WITH_CMAJOR) || defined(SUSHI_BUILD_WITH_JSFX))
#include <dispatch/dispatch.h>
#include <pthread.h>

// Run a callable on the main thread. If already on the main thread, call directly
// to avoid deadlocking dispatch_sync(main_queue).
template<typename F>
auto run_on_main_thread(F&& func) -> decltype(func())
{
    if (pthread_main_np())
    {
        return func();
    }
    else
    {
        using R = decltype(func());
        __block R result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = func();
        });
        return result;
    }
}
#endif

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("controller");

namespace sushi::internal::engine::controller_impl {

EditorController::EditorController(const BaseProcessorContainer* processors)
    : _processors(processors)
{
}

EditorController::~EditorController()
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    auto cleanup = [this]() {
        std::lock_guard<std::mutex> lock(_mutex);
#ifdef SUSHI_BUILD_WITH_VST3
        _vst3_editors.clear();
#endif
#ifdef SUSHI_BUILD_WITH_CLAP
        _clap_editors.clear();
#endif
#ifdef SUSHI_BUILD_WITH_AUV2
        _auv2_editors.clear();
#endif
#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
        _cmajor_editors.clear();
#endif
#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
        _jsfx_editors.clear();
#endif
        _windows.clear();
    };

#ifdef __APPLE__
    if (pthread_main_np())
    {
        cleanup();
    }
    else
    {
        dispatch_sync(dispatch_get_main_queue(), ^{
            cleanup();
        });
    }
#else
    cleanup();
#endif
#endif
}

std::pair<control::ControlStatus, bool> EditorController::has_editor(int processor_id) const
{
    auto processor = _processors->processor(static_cast<ObjectId>(processor_id));
    if (!processor)
    {
        return {control::ControlStatus::NOT_FOUND, false};
    }
    return {control::ControlStatus::OK, processor->has_editor()};
}

std::pair<control::ControlStatus, control::EditorRect> EditorController::open_editor(int processor_id,
                                                                                     void* parent_handle)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    ELKLOG_LOG_DEBUG("open_editor called for processor {}", processor_id);

    auto processor = _processors->mutable_processor(static_cast<ObjectId>(processor_id));
    if (!processor)
    {
        return {control::ControlStatus::NOT_FOUND, {0, 0}};
    }

    if (!processor->has_editor())
    {
        return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
    }

    auto plugin_type = processor->info().type;

#ifdef SUSHI_BUILD_WITH_VST3
    if (plugin_type == PluginType::VST3X)
    {
        auto* vst3_wrapper = static_cast<vst3::Vst3xWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _vst3_editors.find(id);
            if (it != _vst3_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<vst3::Vst3xEditorHost>(
                vst3_wrapper->edit_controller(), id, _resize_callback);

            auto [success, rect] = editor_host->open(parent_handle);
            if (!success)
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            _vst3_editors[id] = std::move(editor_host);
            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

#ifdef __APPLE__
        return run_on_main_thread(do_open);
#else
        return do_open();
#endif
    }
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
    if (plugin_type == PluginType::CLAP)
    {
        auto* clap_wrap = static_cast<clap_wrapper::ClapWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _clap_editors.find(id);
            if (it != _clap_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<clap_wrapper::ClapEditorHost>(
                clap_wrap->instance().plugin(),
                clap_wrap->instance().gui(),
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(parent_handle);
            if (!success)
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            _clap_editors[id] = std::move(editor_host);
            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

#ifdef __APPLE__
        return run_on_main_thread(do_open);
#else
        return do_open();
#endif
    }
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
    if (plugin_type == PluginType::AUV2)
    {
        auto* auv2_wrap = static_cast<auv2_wrapper::AUv2Wrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _auv2_editors.find(id);
            if (it != _auv2_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<auv2_wrapper::AUv2EditorHost>(
                auv2_wrap->audio_unit(),
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(parent_handle);
            if (!success)
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            _auv2_editors[id] = std::move(editor_host);
            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

#ifdef __APPLE__
        return run_on_main_thread(do_open);
#else
        return do_open();
#endif
    }
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
    if (plugin_type == PluginType::CMAJOR)
    {
        auto* cmajor_wrapper = static_cast<cmajor_plugin::CmajorWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _cmajor_editors.find(id);
            if (it != _cmajor_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<cmajor_plugin::CmajorEditorHost>(
                *cmajor_wrapper,
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(parent_handle);
            if (!success)
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            _cmajor_editors[id] = std::move(editor_host);
            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

        return run_on_main_thread(do_open);
    }
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
    if (plugin_type == PluginType::JSFX)
    {
        auto* jsfx_wrap = static_cast<jsfx_wrapper::JsfxWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _jsfx_editors.find(id);
            if (it != _jsfx_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<jsfx_wrapper::JsfxEditorHost>(
                jsfx_wrap->effect(),
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(parent_handle);
            if (!success)
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            _jsfx_editors[id] = std::move(editor_host);
            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

        return run_on_main_thread(do_open);
    }
#endif

    (void)plugin_type;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
#else
    (void)processor_id;
    (void)parent_handle;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
#endif
}

std::pair<control::ControlStatus, control::EditorRect> EditorController::open_editor(int processor_id)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    ELKLOG_LOG_DEBUG("open_editor (managed window) called for processor {}", processor_id);

    auto processor = _processors->mutable_processor(static_cast<ObjectId>(processor_id));
    if (!processor)
    {
        return {control::ControlStatus::NOT_FOUND, {0, 0}};
    }

    if (!processor->has_editor())
    {
        return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
    }

    auto plugin_type = processor->info().type;
    auto proc_name = processor->name();

#ifdef SUSHI_BUILD_WITH_VST3
    if (plugin_type == PluginType::VST3X)
    {
        auto* vst3_wrapper = static_cast<vst3::Vst3xWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _vst3_editors.find(id);
            if (it != _vst3_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            ELKLOG_LOG_DEBUG("Creating PluginWindow for processor {}", processor_id);
            auto window = std::make_unique<vst3::PluginWindow>();
            auto* native_view = window->create(proc_name, 640, 480);
            if (!native_view)
            {
                ELKLOG_LOG_ERROR("Failed to create native window for processor {}", processor_id);
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto* window_ptr = window.get();
            auto editor_host = std::make_unique<vst3::Vst3xEditorHost>(
                vst3_wrapper->edit_controller(), id,
                [this, window_ptr](int proc_id, int width, int height) -> bool {
                    window_ptr->resize(width, height);
                    if (_resize_callback)
                    {
                        return _resize_callback(proc_id, width, height);
                    }
                    return true;
                });

            auto [success, rect] = editor_host->open(native_view);
            if (!success)
            {
                ELKLOG_LOG_ERROR("Editor host open failed for processor {}", processor_id);
                window->close();
                return {control::ControlStatus::ERROR, {0, 0}};
            }
            ELKLOG_LOG_DEBUG("Editor opened: {}x{}", rect.width, rect.height);

            window->resize(rect.width, rect.height);

            auto saved_it = _saved_frames.find(id);
            if (saved_it != _saved_frames.end())
            {
                window->set_position(saved_it->second.x, saved_it->second.y);
            }

            window->show();

            _vst3_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            _windows[id]->set_resize_callback([this, id](int w, int h) {
                std::lock_guard<std::mutex> lock(_mutex);
                auto ed = _vst3_editors.find(id);
                if (ed != _vst3_editors.end())
                {
                    ed->second->notify_size(w, h);
                }
            });

            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

#ifdef __APPLE__
        return run_on_main_thread(do_open);
#else
        return do_open();
#endif
    }
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
    if (plugin_type == PluginType::CLAP)
    {
        auto* clap_wrap = static_cast<clap_wrapper::ClapWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _clap_editors.find(id);
            if (it != _clap_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            ELKLOG_LOG_DEBUG("Creating PluginWindow for CLAP processor {}", processor_id);
            auto window = std::make_unique<vst3::PluginWindow>();
            auto* native_view = window->create(proc_name, 640, 480);
            if (!native_view)
            {
                ELKLOG_LOG_ERROR("Failed to create native window for processor {}", processor_id);
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<clap_wrapper::ClapEditorHost>(
                clap_wrap->instance().plugin(),
                clap_wrap->instance().gui(),
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(native_view);
            if (!success)
            {
                ELKLOG_LOG_ERROR("CLAP editor open failed for processor {}", processor_id);
                window->close();
                return {control::ControlStatus::ERROR, {0, 0}};
            }
            ELKLOG_LOG_DEBUG("CLAP editor opened: {}x{}", rect.width, rect.height);

            window->resize(rect.width, rect.height);

            auto saved_it = _saved_frames.find(id);
            if (saved_it != _saved_frames.end())
            {
                window->set_position(saved_it->second.x, saved_it->second.y);
            }

            window->show();

            _clap_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

#ifdef __APPLE__
        return run_on_main_thread(do_open);
#else
        return do_open();
#endif
    }
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
    if (plugin_type == PluginType::AUV2)
    {
        auto* auv2_wrap = static_cast<auv2_wrapper::AUv2Wrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _auv2_editors.find(id);
            if (it != _auv2_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            ELKLOG_LOG_DEBUG("Creating PluginWindow for AUv2 processor {}", processor_id);
            auto window = std::make_unique<vst3::PluginWindow>();
            auto* native_view = window->create(proc_name, 640, 480);
            if (!native_view)
            {
                ELKLOG_LOG_ERROR("Failed to create native window for processor {}", processor_id);
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<auv2_wrapper::AUv2EditorHost>(
                auv2_wrap->audio_unit(),
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(native_view);
            if (!success)
            {
                ELKLOG_LOG_ERROR("AUv2 editor open failed for processor {}", processor_id);
                window->close();
                return {control::ControlStatus::ERROR, {0, 0}};
            }
            ELKLOG_LOG_DEBUG("AUv2 editor opened: {}x{}", rect.width, rect.height);

            window->resize(rect.width, rect.height);

            auto saved_it = _saved_frames.find(id);
            if (saved_it != _saved_frames.end())
            {
                window->set_position(saved_it->second.x, saved_it->second.y);
            }

            window->show();

            _auv2_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

#ifdef __APPLE__
        return run_on_main_thread(do_open);
#else
        return do_open();
#endif
    }
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
    if (plugin_type == PluginType::CMAJOR)
    {
        auto* cmajor_wrapper = static_cast<cmajor_plugin::CmajorWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _cmajor_editors.find(id);
            if (it != _cmajor_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto session = cmajor_wrapper->current_editor_session();
            if (!session.has_value())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            ELKLOG_LOG_DEBUG("Creating PluginWindow for Cmajor processor {}", processor_id);
            auto window = std::make_unique<vst3::PluginWindow>();
            auto* native_view = window->create(proc_name,
                                               static_cast<int>(session->view.getWidth()),
                                               static_cast<int>(session->view.getHeight()),
                                               session->view.isResizable());
            if (!native_view)
            {
                ELKLOG_LOG_ERROR("Failed to create native window for processor {}", processor_id);
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto* window_ptr = window.get();
            auto editor_host = std::make_unique<cmajor_plugin::CmajorEditorHost>(
                *cmajor_wrapper,
                id,
                [this, window_ptr](int proc_id, int width, int height) -> bool {
                    window_ptr->resize(width, height);
                    if (_resize_callback)
                    {
                        return _resize_callback(proc_id, width, height);
                    }
                    return true;
                });

            auto [success, rect] = editor_host->open(native_view);
            if (!success)
            {
                ELKLOG_LOG_ERROR("Cmajor editor open failed for processor {}", processor_id);
                window->close();
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            window->resize(rect.width, rect.height);

            auto saved_it = _saved_frames.find(id);
            if (saved_it != _saved_frames.end())
            {
                window->set_position(saved_it->second.x, saved_it->second.y);
            }

            window->show();

            _cmajor_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            _windows[id]->set_resize_callback([this, id](int w, int h) {
                std::lock_guard<std::mutex> lock(_mutex);
                auto ed = _cmajor_editors.find(id);
                if (ed != _cmajor_editors.end())
                {
                    ed->second->notify_size(w, h);
                }
            });

            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

        return run_on_main_thread(do_open);
    }
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
    if (plugin_type == PluginType::JSFX)
    {
        auto* jsfx_wrap = static_cast<jsfx_wrapper::JsfxWrapper*>(processor.get());

        auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
            std::lock_guard<std::mutex> lock(_mutex);

            auto id = static_cast<ObjectId>(processor_id);
            auto it = _jsfx_editors.find(id);
            if (it != _jsfx_editors.end() && it->second->is_open())
            {
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            ELKLOG_LOG_DEBUG("Creating PluginWindow for JSFX processor {}", processor_id);
            auto window = std::make_unique<vst3::PluginWindow>();
            auto* native_view = window->create(proc_name, 640, 480);
            if (!native_view)
            {
                ELKLOG_LOG_ERROR("Failed to create native window for processor {}", processor_id);
                return {control::ControlStatus::ERROR, {0, 0}};
            }

            auto editor_host = std::make_unique<jsfx_wrapper::JsfxEditorHost>(
                jsfx_wrap->effect(),
                id,
                _resize_callback);

            auto [success, rect] = editor_host->open(native_view);
            if (!success)
            {
                ELKLOG_LOG_ERROR("JSFX editor open failed for processor {}", processor_id);
                window->close();
                return {control::ControlStatus::ERROR, {0, 0}};
            }
            ELKLOG_LOG_DEBUG("JSFX editor opened: {}x{}", rect.width, rect.height);

            window->resize(rect.width, rect.height);

            auto saved_it = _saved_frames.find(id);
            if (saved_it != _saved_frames.end())
            {
                window->set_position(saved_it->second.x, saved_it->second.y);
            }

            window->show();

            _jsfx_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            return {control::ControlStatus::OK, {0, 0, rect.width, rect.height}};
        };

        return run_on_main_thread(do_open);
    }
#endif

    (void)plugin_type;
    (void)proc_name;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
#else
    (void)processor_id;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
#endif
}

control::ControlStatus EditorController::close_editor(int processor_id)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    ELKLOG_LOG_DEBUG("close_editor called for processor {}", processor_id);

    auto do_close = [&]() -> control::ControlStatus {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);

        // Save window frame before closing for position restoration
        auto _save_window_frame = [&](ObjectId win_id) {
            auto win_it = _windows.find(win_id);
            if (win_it != _windows.end())
            {
                control::EditorRect frame;
                win_it->second->get_frame(frame.x, frame.y, frame.width, frame.height);
                _saved_frames[win_id] = frame;
                win_it->second->close();
                _windows.erase(win_it);
            }
        };

#ifdef SUSHI_BUILD_WITH_VST3
        auto vst3_it = _vst3_editors.find(id);
        if (vst3_it != _vst3_editors.end())
        {
            vst3_it->second->close();
            _vst3_editors.erase(vst3_it);
            _save_window_frame(id);
            return control::ControlStatus::OK;
        }
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
        auto clap_it = _clap_editors.find(id);
        if (clap_it != _clap_editors.end())
        {
            clap_it->second->close();
            _clap_editors.erase(clap_it);
            _save_window_frame(id);
            return control::ControlStatus::OK;
        }
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
        auto auv2_it = _auv2_editors.find(id);
        if (auv2_it != _auv2_editors.end())
        {
            auv2_it->second->close();
            _auv2_editors.erase(auv2_it);
            _save_window_frame(id);
            return control::ControlStatus::OK;
        }
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
        auto cmajor_it = _cmajor_editors.find(id);
        if (cmajor_it != _cmajor_editors.end())
        {
            cmajor_it->second->close();
            _cmajor_editors.erase(cmajor_it);
            _save_window_frame(id);
            return control::ControlStatus::OK;
        }
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
        auto jsfx_it = _jsfx_editors.find(id);
        if (jsfx_it != _jsfx_editors.end())
        {
            jsfx_it->second->close();
            _jsfx_editors.erase(jsfx_it);
            _save_window_frame(id);
            return control::ControlStatus::OK;
        }
#endif

        return control::ControlStatus::NOT_FOUND;
    };

#ifdef __APPLE__
    return run_on_main_thread(do_close);
#else
    return do_close();
#endif
#else
    (void)processor_id;
    return control::ControlStatus::UNSUPPORTED_OPERATION;
#endif
}

std::pair<control::ControlStatus, bool> EditorController::is_editor_open(int processor_id) const
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    std::lock_guard<std::mutex> lock(_mutex);

    auto id = static_cast<ObjectId>(processor_id);

#ifdef SUSHI_BUILD_WITH_VST3
    auto vst3_it = _vst3_editors.find(id);
    if (vst3_it != _vst3_editors.end())
    {
        return {control::ControlStatus::OK, vst3_it->second->is_open()};
    }
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
    auto clap_it = _clap_editors.find(id);
    if (clap_it != _clap_editors.end())
    {
        return {control::ControlStatus::OK, clap_it->second->is_open()};
    }
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
    auto auv2_it = _auv2_editors.find(id);
    if (auv2_it != _auv2_editors.end())
    {
        return {control::ControlStatus::OK, auv2_it->second->is_open()};
    }
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
    auto cmajor_it = _cmajor_editors.find(id);
    if (cmajor_it != _cmajor_editors.end())
    {
        return {control::ControlStatus::OK, cmajor_it->second->is_open()};
    }
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
    auto jsfx_it = _jsfx_editors.find(id);
    if (jsfx_it != _jsfx_editors.end())
    {
        return {control::ControlStatus::OK, jsfx_it->second->is_open()};
    }
#endif

    return {control::ControlStatus::OK, false};
#else
    (void)processor_id;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, false};
#endif
}

void EditorController::set_resize_callback(control::EditorResizeCallback callback)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    std::lock_guard<std::mutex> lock(_mutex);
    _resize_callback = std::move(callback);
#else
    (void)callback;
#endif
}

control::ControlStatus EditorController::set_content_scale_factor(int processor_id, float scale_factor)
{
#ifdef SUSHI_BUILD_WITH_VST3
    ELKLOG_LOG_DEBUG("set_content_scale_factor called for processor {} with factor {}", processor_id, scale_factor);

    std::lock_guard<std::mutex> lock(_mutex);

    auto id = static_cast<ObjectId>(processor_id);
    auto it = _vst3_editors.find(id);
    if (it == _vst3_editors.end() || !it->second->is_open())
    {
        return control::ControlStatus::NOT_FOUND;
    }

    return it->second->set_content_scale_factor(scale_factor) ? control::ControlStatus::OK
                                                              : control::ControlStatus::UNSUPPORTED_OPERATION;
#else
    (void)processor_id;
    (void)scale_factor;
    return control::ControlStatus::UNSUPPORTED_OPERATION;
#endif
}

std::pair<control::ControlStatus, control::EditorRect> EditorController::get_editor_info(int processor_id) const
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    std::lock_guard<std::mutex> lock(_mutex);

    auto id = static_cast<ObjectId>(processor_id);

    // If window is open, return live frame
    auto win_it = _windows.find(id);
    if (win_it != _windows.end())
    {
        control::EditorRect frame;
        win_it->second->get_frame(frame.x, frame.y, frame.width, frame.height);
        return {control::ControlStatus::OK, frame};
    }

    // Otherwise return saved frame if available
    auto saved_it = _saved_frames.find(id);
    if (saved_it != _saved_frames.end())
    {
        return {control::ControlStatus::OK, saved_it->second};
    }

    return {control::ControlStatus::NOT_FOUND, {}};
#else
    (void)processor_id;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {}};
#endif
}

control::ControlStatus EditorController::set_editor_position(int processor_id, int x, int y)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    auto do_set = [&]() -> control::ControlStatus {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);

        // Always update saved frames
        auto saved_it = _saved_frames.find(id);
        if (saved_it != _saved_frames.end())
        {
            saved_it->second.x = x;
            saved_it->second.y = y;
        }
        else
        {
            _saved_frames[id] = {x, y, 0, 0};
        }

        // If window is open, move it immediately
        auto win_it = _windows.find(id);
        if (win_it != _windows.end())
        {
            win_it->second->set_position(x, y);
        }

        return control::ControlStatus::OK;
    };

#ifdef __APPLE__
    return run_on_main_thread(do_set);
#else
    return do_set();
#endif
#else
    (void)processor_id;
    (void)x;
    (void)y;
    return control::ControlStatus::UNSUPPORTED_OPERATION;
#endif
}

control::ControlStatus EditorController::capture_editor_screenshot(int processor_id, const std::string& output_path, int max_width, int max_height)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__))
    auto do_capture = [&]() -> control::ControlStatus {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);
        auto win_it = _windows.find(id);
        if (win_it == _windows.end())
        {
            return control::ControlStatus::NOT_FOUND;
        }

        bool success = win_it->second->capture_screenshot(output_path, max_width, max_height);
        return success ? control::ControlStatus::OK : control::ControlStatus::ERROR;
    };

#ifdef __APPLE__
    return run_on_main_thread(do_capture);
#else
    return do_capture();
#endif
#else
    (void)processor_id;
    (void)output_path;
    (void)max_width;
    (void)max_height;
    return control::ControlStatus::UNSUPPORTED_OPERATION;
#endif
}

} // end namespace sushi::internal::engine::controller_impl
