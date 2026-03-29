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

#include <map>
#include <string>

#include "elklog/static_logger.h"

#include "faust_editor_host.h"
#include "faust_wrapper.h"
#include "faust/gui/GUI.h"

#ifdef __cplusplus
extern "C" {
#endif

// Swift bridging functions — implemented in FaustEditorSwift.swift
void* SushiFaustViewModel_create(void);
void SushiFaustViewModel_setZone(void* vm, const char* address, float* zone);
void SushiFaustViewModel_clearZones(void* vm);
void SushiFaustViewModel_setParameterChangeCallback(void* vm,
    void (*callback)(void* context, const char* address, float domain_value),
    void* context);
void SushiFaustViewModel_updateValue(void* vm, const char* address, float value);
void SushiFaustViewModel_release(void* vm);

void* SushiFaustEditorView_create(const char* json, void* viewModel);
void SushiFaustEditorView_getSize(void* view, int* width, int* height);
void SushiFaustEditorView_release(void* view);

#ifdef __cplusplus
}
#endif

// Static member definitions for Faust's header-only GUI class
std::list<GUI*> GUI::fGuiList;
ztimedmap GUI::gTimedZoneMap;

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

struct FaustParamMapping
{
    ObjectId param_id;
    float min;
    float max;
};

// Faust GUI callback context — passed as user data to uiCallbackItem
struct ZoneCallbackContext
{
    void* view_model;
    std::string address;
};

struct FaustEditorHost::Impl
{
    void* view_model{nullptr};
    void* editor_view{nullptr};
    std::map<std::string, FaustParamMapping> address_to_param;

    // Faust GUI instance — owns uiCallbackItems that fire on zone changes
    std::unique_ptr<GUI> gui;
    std::vector<std::unique_ptr<ZoneCallbackContext>> callback_contexts;
    NSTimer* sync_timer{nil};

    void startSyncTimer()
    {
        sync_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                                    repeats:YES
                                                      block:^(NSTimer*) {
            if (gui)
            {
                gui->updateAllZones();
            }
        }];
    }

    void stopSyncTimer()
    {
        [sync_timer invalidate];
        sync_timer = nil;
    }

    void setupGui(const std::vector<FaustParameterInfo>& params, void* vm)
    {
        gui = std::make_unique<GUI>();
        callback_contexts.clear();

        for (size_t i = 0; i < params.size(); ++i)
        {
            const auto& param = params[i];

            std::string address = param.full_path;
            if (!address.empty() && address[0] != '/')
            {
                address = "/" + address;
            }

            auto ctx = std::make_unique<ZoneCallbackContext>();
            ctx->view_model = vm;
            ctx->address = address;

            gui->addCallback(param.zone,
                [](FAUSTFLOAT val, void* data)
                {
                    auto* cb = static_cast<ZoneCallbackContext*>(data);
                    SushiFaustViewModel_updateValue(cb->view_model, cb->address.c_str(), val);
                },
                ctx.get());

            callback_contexts.push_back(std::move(ctx));
        }
    }

    void teardownGui()
    {
        stopSyncTimer();
        gui.reset();
        callback_contexts.clear();
    }
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

    _impl->address_to_param.clear();
    for (size_t i = 0; i < params.size(); ++i)
    {
        const auto& param = params[i];
        std::string address = param.full_path;
        if (!address.empty() && address[0] != '/')
        {
            address = "/" + address;
        }
        SushiFaustViewModel_setZone(_impl->view_model, address.c_str(), param.zone);

        if (!param.is_bargraph)
        {
            _impl->address_to_param[address] = {static_cast<ObjectId>(i), param.min, param.max};
        }
    }

    // Set up callback so editor-originated changes notify the host
    SushiFaustViewModel_setParameterChangeCallback(
        _impl->view_model,
        [](void* context, const char* address, float domain_value)
        {
            auto* self = static_cast<FaustEditorHost*>(context);
            auto it = self->_impl->address_to_param.find(address);
            if (it == self->_impl->address_to_param.end())
            {
                return;
            }
            auto& mapping = it->second;
            float range = mapping.max - mapping.min;
            float normalized = (range > 0.0f) ? (domain_value - mapping.min) / range : 0.0f;
            self->_wrapper.notify_parameter_change_from_editor(mapping.param_id, normalized);
        },
        this);

    // Register Faust GUI callbacks so zone changes push into the SwiftUI ViewModel
    _impl->setupGui(params, _impl->view_model);

    // Create SwiftUI editor view
    _impl->editor_view = SushiFaustEditorView_create(json.c_str(), _impl->view_model);
    if (!_impl->editor_view)
    {
        ELKLOG_LOG_ERROR("Failed to create Faust editor view");
        _impl->teardownGui();
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
        _impl->teardownGui();
        SushiFaustViewModel_release(_impl->view_model);
        _impl->editor_view = nullptr;
        _impl->view_model = nullptr;
        return {false, {0, 0}};
    }

    set_view_size(_impl->editor_view, width, height);

    _parent_handle = parent_handle;
    _is_open = true;

    // Start the sync timer — calls GUI::updateAllZones() at 30Hz on the main thread
    _impl->startSyncTimer();

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

            // Rebuild GUI callbacks and zone pointers
            this->_impl->teardownGui();
            SushiFaustViewModel_clearZones(this->_impl->view_model);
            this->_impl->address_to_param.clear();

            auto new_params = this->_wrapper.current_parameters();
            for (size_t i = 0; i < new_params.size(); ++i)
            {
                const auto& param = new_params[i];
                std::string address = param.full_path;
                if (!address.empty() && address[0] != '/')
                {
                    address = "/" + address;
                }
                SushiFaustViewModel_setZone(this->_impl->view_model, address.c_str(), param.zone);

                if (!param.is_bargraph)
                {
                    this->_impl->address_to_param[address] = {static_cast<ObjectId>(i), param.min, param.max};
                }
            }

            this->_impl->setupGui(new_params, this->_impl->view_model);

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

            this->_impl->startSyncTimer();

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

    _impl->teardownGui();

    if (_impl->editor_view)
    {
        remove_native_view(_impl->editor_view);
        SushiFaustEditorView_release(_impl->editor_view);
        _impl->editor_view = nullptr;
    }

    if (_impl->view_model)
    {
        SushiFaustViewModel_setParameterChangeCallback(_impl->view_model, nullptr, nullptr);
        SushiFaustViewModel_release(_impl->view_model);
        _impl->view_model = nullptr;
    }

    _impl->address_to_param.clear();
    _parent_handle = nullptr;
    _is_open = false;

    ELKLOG_LOG_DEBUG("Faust editor closed");
}

bool FaustEditorHost::is_open() const
{
    return _is_open;
}

} // namespace sushi::internal::faust_wrapper
