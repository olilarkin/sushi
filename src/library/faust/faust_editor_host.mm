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

#import <Cocoa/Cocoa.h>

#include "elklog/static_logger.h"

#include "faust_editor_host.h"
#include "faust_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

// Swift bridging functions — implemented in FaustEditorSwift.swift
void* SushiFaustViewModel_create(void);
void SushiFaustViewModel_setZone(void* vm, const char* address, float* zone);
void SushiFaustViewModel_clearZones(void* vm);
void SushiFaustViewModel_release(void* vm);

void* SushiFaustEditorView_create(const char* json, void* viewModel);
void SushiFaustEditorView_getSize(void* view, int* width, int* height);
void SushiFaustEditorView_release(void* view);

#ifdef __cplusplus
}
#endif

namespace sushi::internal::faust_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("faust_editor");

namespace {

bool add_child_view(void* parent, void* child)
{
    if (parent == nullptr || child == nullptr)
    {
        return false;
    }

    NSView* parent_view = (__bridge NSView*) parent;
    NSView* child_view = (__bridge NSView*) child;
    [parent_view addSubview:child_view];
    return true;
}

bool set_view_size(void* view, int width, int height)
{
    if (view == nullptr)
    {
        return false;
    }

    NSView* cocoa_view = (__bridge NSView*) view;
    [cocoa_view setFrame:NSMakeRect(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height))];
    return true;
}

void remove_native_view(void* native_view)
{
    if (native_view == nullptr)
    {
        return;
    }

    NSView* view = (__bridge NSView*) native_view;
    [view removeFromSuperview];
}

} // namespace

struct FaustEditorHost::Impl
{
    void* view_model{nullptr};  // Retained SushiFaustViewModel*
    void* editor_view{nullptr}; // Retained SushiFaustEditorView (NSView*)
};

FaustEditorHost::FaustEditorHost(FaustWrapper& wrapper,
                                 ObjectId processor_id,
                                 FaustEditorResizeCallback resize_callback)
    : _wrapper(wrapper),
      _processor_id(processor_id),
      _resize_callback(std::move(resize_callback)),
      _impl(std::make_unique<Impl>())
{
}

FaustEditorHost::~FaustEditorHost()
{
    close();
}

std::pair<bool, FaustEditorRect> FaustEditorHost::open(void* parent_handle)
{
    if (_is_open)
    {
        return {false, {0, 0}};
    }

    auto json = _wrapper.ui_json();
    if (json.empty())
    {
        ELKLOG_LOG_ERROR("Failed to get Faust UI JSON");
        return {false, {0, 0}};
    }

    auto params = _wrapper.current_parameters();

    // Create view model and populate zone pointers
    _impl->view_model = SushiFaustViewModel_create();
    if (!_impl->view_model)
    {
        ELKLOG_LOG_ERROR("Failed to create Faust view model");
        return {false, {0, 0}};
    }

    for (const auto& param : params)
    {
        // JSONUI addresses start with '/' — prepend if needed to match
        std::string address = param.full_path;
        if (!address.empty() && address[0] != '/')
        {
            address = "/" + address;
        }
        SushiFaustViewModel_setZone(_impl->view_model, address.c_str(), param.zone);
    }

    // Create SwiftUI editor view
    _impl->editor_view = SushiFaustEditorView_create(json.c_str(), _impl->view_model);
    if (!_impl->editor_view)
    {
        ELKLOG_LOG_ERROR("Failed to create Faust editor view");
        SushiFaustViewModel_release(_impl->view_model);
        _impl->view_model = nullptr;
        return {false, {0, 0}};
    }

    int width = 0;
    int height = 0;
    SushiFaustEditorView_getSize(_impl->editor_view, &width, &height);

    if (!add_child_view(parent_handle, _impl->editor_view))
    {
        ELKLOG_LOG_ERROR("Failed to attach Faust editor view");
        SushiFaustEditorView_release(_impl->editor_view);
        SushiFaustViewModel_release(_impl->view_model);
        _impl->editor_view = nullptr;
        _impl->view_model = nullptr;
        return {false, {0, 0}};
    }

    set_view_size(_impl->editor_view, width, height);

    _parent_handle = parent_handle;
    _is_open = true;

    // Register recompile callback
    _wrapper.set_editor_recompile_callback([this]()
    {
        auto rebuild = ^{
            if (!this->_is_open)
            {
                return;
            }

            // Remove old view
            remove_native_view(this->_impl->editor_view);
            SushiFaustEditorView_release(this->_impl->editor_view);
            this->_impl->editor_view = nullptr;

            // Refresh zone pointers
            SushiFaustViewModel_clearZones(this->_impl->view_model);
            auto new_params = this->_wrapper.current_parameters();
            for (const auto& param : new_params)
            {
                std::string address = param.full_path;
                if (!address.empty() && address[0] != '/')
                {
                    address = "/" + address;
                }
                SushiFaustViewModel_setZone(this->_impl->view_model, address.c_str(), param.zone);
            }

            // Create new view with new JSON
            auto new_json = this->_wrapper.ui_json();
            this->_impl->editor_view = SushiFaustEditorView_create(new_json.c_str(), this->_impl->view_model);
            if (!this->_impl->editor_view)
            {
                ELKLOG_LOG_ERROR("Failed to recreate Faust editor view after recompile");
                return;
            }

            int new_width = 0;
            int new_height = 0;
            SushiFaustEditorView_getSize(this->_impl->editor_view, &new_width, &new_height);

            add_child_view(this->_parent_handle, this->_impl->editor_view);
            set_view_size(this->_impl->editor_view, new_width, new_height);

            if (this->_resize_callback)
            {
                this->_resize_callback(static_cast<int>(this->_processor_id), new_width, new_height);
            }
        };

        if (pthread_main_np())
        {
            rebuild();
        }
        else
        {
            dispatch_sync(dispatch_get_main_queue(), rebuild);
        }
    });

    ELKLOG_LOG_INFO("Faust editor opened: {}x{}", width, height);
    return {true, {width, height}};
}

void FaustEditorHost::close()
{
    if (!_is_open)
    {
        return;
    }

    _wrapper.set_editor_recompile_callback({});

    if (_impl->editor_view)
    {
        remove_native_view(_impl->editor_view);
        SushiFaustEditorView_release(_impl->editor_view);
        _impl->editor_view = nullptr;
    }

    if (_impl->view_model)
    {
        SushiFaustViewModel_release(_impl->view_model);
        _impl->view_model = nullptr;
    }

    _parent_handle = nullptr;
    _is_open = false;

    ELKLOG_LOG_DEBUG("Faust editor closed");
}

bool FaustEditorHost::is_open() const
{
    return _is_open;
}

} // namespace sushi::internal::faust_wrapper
