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
 * @brief Implementation of EditorController for managing VST3 plugin editor views.
 * @Copyright 2026 Oliver Larkin
 */

#include "elklog/static_logger.h"

#include "editor_controller.h"

#ifdef SUSHI_BUILD_WITH_VST3
#include "library/vst3x/vst3x_wrapper.h"
#endif

#if defined(__APPLE__) && defined(SUSHI_BUILD_WITH_VST3)
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
#ifdef SUSHI_BUILD_WITH_VST3
    auto cleanup = [this]() {
        std::lock_guard<std::mutex> lock(_mutex);
        _editors.clear();
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
#ifdef SUSHI_BUILD_WITH_VST3
    ELKLOG_LOG_DEBUG("open_editor called for processor {}", processor_id);

    auto processor = _processors->mutable_processor(static_cast<ObjectId>(processor_id));
    if (!processor)
    {
        return {control::ControlStatus::NOT_FOUND, {0, 0}};
    }

    if (!processor->has_editor() || processor->info().type != PluginType::VST3X)
    {
        return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
    }

    auto* vst3_wrapper = static_cast<vst3::Vst3xWrapper*>(processor.get());

    auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);
        auto it = _editors.find(id);
        if (it != _editors.end() && it->second->is_open())
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

        _editors[id] = std::move(editor_host);
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
#else
    (void)processor_id;
    (void)parent_handle;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
#endif
}

std::pair<control::ControlStatus, control::EditorRect> EditorController::open_editor(int processor_id)
{
#ifdef SUSHI_BUILD_WITH_VST3
    ELKLOG_LOG_DEBUG("open_editor (managed window) called for processor {}", processor_id);

    auto processor = _processors->mutable_processor(static_cast<ObjectId>(processor_id));
    if (!processor)
    {
        return {control::ControlStatus::NOT_FOUND, {0, 0}};
    }

    if (!processor->has_editor() || processor->info().type != PluginType::VST3X)
    {
        return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
    }

    auto* vst3_wrapper = static_cast<vst3::Vst3xWrapper*>(processor.get());
    auto proc_name = processor->name();

    auto do_open = [&]() -> std::pair<control::ControlStatus, control::EditorRect> {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);
        auto it = _editors.find(id);
        if (it != _editors.end() && it->second->is_open())
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
        ELKLOG_LOG_DEBUG("Native window created, view={}", native_view);

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

        ELKLOG_LOG_DEBUG("Opening editor host for processor {}", processor_id);
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

        _editors[id] = std::move(editor_host);
        _windows[id] = std::move(window);

        _windows[id]->set_resize_callback([this, id](int w, int h) {
            std::lock_guard<std::mutex> lock(_mutex);
            auto ed = _editors.find(id);
            if (ed != _editors.end())
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
#else
    (void)processor_id;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, {0, 0}};
#endif
}

control::ControlStatus EditorController::close_editor(int processor_id)
{
#ifdef SUSHI_BUILD_WITH_VST3
    ELKLOG_LOG_DEBUG("close_editor called for processor {}", processor_id);

    auto do_close = [&]() -> control::ControlStatus {
        std::lock_guard<std::mutex> lock(_mutex);

        auto id = static_cast<ObjectId>(processor_id);
        auto it = _editors.find(id);
        if (it == _editors.end())
        {
            return control::ControlStatus::NOT_FOUND;
        }

        it->second->close();
        _editors.erase(it);

        auto win_it = _windows.find(id);
        if (win_it != _windows.end())
        {
            win_it->second->close();
            _windows.erase(win_it);
        }

        return control::ControlStatus::OK;
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
#ifdef SUSHI_BUILD_WITH_VST3
    std::lock_guard<std::mutex> lock(_mutex);

    auto id = static_cast<ObjectId>(processor_id);
    auto it = _editors.find(id);
    if (it != _editors.end())
    {
        return {control::ControlStatus::OK, it->second->is_open()};
    }
    return {control::ControlStatus::OK, false};
#else
    (void)processor_id;
    return {control::ControlStatus::UNSUPPORTED_OPERATION, false};
#endif
}

void EditorController::set_resize_callback(control::EditorResizeCallback callback)
{
#ifdef SUSHI_BUILD_WITH_VST3
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
    auto it = _editors.find(id);
    if (it == _editors.end() || !it->second->is_open())
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
