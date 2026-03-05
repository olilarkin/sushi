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

#if defined(__APPLE__) && (defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2))
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("controller");

namespace sushi::internal::engine::controller_impl {

EditorController::EditorController(const BaseProcessorContainer* processors)
    : _processors(processors)
{
}

EditorController::~EditorController()
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2)
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
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2)
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
            return {control::ControlStatus::OK, {rect.width, rect.height}};
        };

#ifdef __APPLE__
        __block std::pair<control::ControlStatus, control::EditorRect> result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = do_open();
        });
        return result;
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
            return {control::ControlStatus::OK, {rect.width, rect.height}};
        };

#ifdef __APPLE__
        __block std::pair<control::ControlStatus, control::EditorRect> result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = do_open();
        });
        return result;
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
            return {control::ControlStatus::OK, {rect.width, rect.height}};
        };

#ifdef __APPLE__
        __block std::pair<control::ControlStatus, control::EditorRect> result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = do_open();
        });
        return result;
#else
        return do_open();
#endif
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
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2)
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

            return {control::ControlStatus::OK, {rect.width, rect.height}};
        };

#ifdef __APPLE__
        __block std::pair<control::ControlStatus, control::EditorRect> result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = do_open();
        });
        return result;
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
            window->show();

            _clap_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            return {control::ControlStatus::OK, {rect.width, rect.height}};
        };

#ifdef __APPLE__
        __block std::pair<control::ControlStatus, control::EditorRect> result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = do_open();
        });
        return result;
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
            window->show();

            _auv2_editors[id] = std::move(editor_host);
            _windows[id] = std::move(window);

            return {control::ControlStatus::OK, {rect.width, rect.height}};
        };

#ifdef __APPLE__
        __block std::pair<control::ControlStatus, control::EditorRect> result;
        dispatch_sync(dispatch_get_main_queue(), ^{
            result = do_open();
        });
        return result;
#else
        return do_open();
#endif
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
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2)
    ELKLOG_LOG_DEBUG("close_editor called for processor {}", processor_id);

    auto do_close = [&]() -> control::ControlStatus {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);

#ifdef SUSHI_BUILD_WITH_VST3
        auto vst3_it = _vst3_editors.find(id);
        if (vst3_it != _vst3_editors.end())
        {
            vst3_it->second->close();
            _vst3_editors.erase(vst3_it);

            auto win_it = _windows.find(id);
            if (win_it != _windows.end())
            {
                win_it->second->close();
                _windows.erase(win_it);
            }
            return control::ControlStatus::OK;
        }
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
        auto clap_it = _clap_editors.find(id);
        if (clap_it != _clap_editors.end())
        {
            clap_it->second->close();
            _clap_editors.erase(clap_it);

            auto win_it = _windows.find(id);
            if (win_it != _windows.end())
            {
                win_it->second->close();
                _windows.erase(win_it);
            }
            return control::ControlStatus::OK;
        }
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
        auto auv2_it = _auv2_editors.find(id);
        if (auv2_it != _auv2_editors.end())
        {
            auv2_it->second->close();
            _auv2_editors.erase(auv2_it);

            auto win_it = _windows.find(id);
            if (win_it != _windows.end())
            {
                win_it->second->close();
                _windows.erase(win_it);
            }
            return control::ControlStatus::OK;
        }
#endif

        return control::ControlStatus::NOT_FOUND;
    };

#ifdef __APPLE__
    __block control::ControlStatus result;
    dispatch_sync(dispatch_get_main_queue(), ^{
        result = do_close();
    });
    return result;
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
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2)
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

    return {control::ControlStatus::OK, false};
#else
    (void)processor_id;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, false};
#endif
}

void EditorController::set_resize_callback(control::EditorResizeCallback callback)
{
#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2)
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

} // end namespace sushi::internal::engine::controller_impl
