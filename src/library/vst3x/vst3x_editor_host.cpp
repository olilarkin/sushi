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
 * @brief IPlugFrame implementation and IPlugView lifecycle manager for VST3 plugin editors.
 * @Copyright 2017-2023 Elk Audio AB, Stockholm
 */

#include "elk-warning-suppressor/warning_suppressor.hpp"

ELK_PUSH_WARNING
ELK_DISABLE_EXTRA
ELK_DISABLE_DEPRECATED_DECLARATIONS
ELK_DISABLE_SHORTEN_64_TO_32

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"

ELK_POP_WARNING

#include "elklog/static_logger.h"

#include "vst3x_editor_host.h"

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("vst3_editor");

namespace sushi::internal::vst3 {

namespace {

Steinberg::FIDString platform_type()
{
#if __APPLE__
    return Steinberg::kPlatformTypeNSView;
#elif _WIN32
    return Steinberg::kPlatformTypeHWND;
#else
    return Steinberg::kPlatformTypeX11EmbedWindowID;
#endif
}

} // anonymous namespace

Vst3xEditorHost::Vst3xEditorHost(Steinberg::Vst::IEditController* controller,
                                 ObjectId processor_id,
                                 EditorResizeCallback resize_callback)
    : _controller(controller),
      _processor_id(processor_id),
      _resize_callback(std::move(resize_callback))
{
}

Vst3xEditorHost::~Vst3xEditorHost()
{
    close();
}

std::pair<bool, EditorRect> Vst3xEditorHost::open(void* parent_handle)
{
    if (_view)
    {
        ELKLOG_LOG_WARNING("Editor view already open for processor {}", _processor_id);
        return {false, {0, 0}};
    }

    _view = _controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!_view)
    {
        ELKLOG_LOG_WARNING("Plugin {} does not provide an editor view", _processor_id);
        return {false, {0, 0}};
    }

    if (_view->isPlatformTypeSupported(platform_type()) != Steinberg::kResultTrue)
    {
        ELKLOG_LOG_WARNING("Plugin {} does not support platform type {}", _processor_id, platform_type());
        _view->release();
        _view = nullptr;
        return {false, {0, 0}};
    }

    _view->setFrame(this);

    Steinberg::ViewRect rect{};
    if (_view->getSize(&rect) != Steinberg::kResultOk)
    {
        ELKLOG_LOG_WARNING("Failed to get initial editor size for processor {}", _processor_id);
    }

    if (_view->attached(parent_handle, platform_type()) != Steinberg::kResultOk)
    {
        ELKLOG_LOG_ERROR("Failed to attach editor view for processor {}", _processor_id);
        _view->setFrame(nullptr);
        _view->release();
        _view = nullptr;
        return {false, {0, 0}};
    }

    EditorRect result{rect.getWidth(), rect.getHeight()};
    ELKLOG_LOG_INFO("Opened editor for processor {} ({}x{})", _processor_id, result.width, result.height);
    return {true, result};
}

void Vst3xEditorHost::close()
{
    if (!_view)
    {
        return;
    }

    _view->setFrame(nullptr);
    _view->removed();
    _view->release();
    _view = nullptr;
    ELKLOG_LOG_INFO("Closed editor for processor {}", _processor_id);
}

bool Vst3xEditorHost::is_open() const
{
    return _view != nullptr;
}

bool Vst3xEditorHost::set_content_scale_factor(float scale_factor)
{
    if (!_view)
    {
        return false;
    }

    Steinberg::IPlugViewContentScaleSupport* scale_support = nullptr;
    if (_view->queryInterface(Steinberg::IPlugViewContentScaleSupport::iid,
                              reinterpret_cast<void**>(&scale_support)) == Steinberg::kResultOk
        && scale_support)
    {
        auto result = scale_support->setContentScaleFactor(scale_factor);
        scale_support->release();
        return result == Steinberg::kResultOk;
    }

    return false;
}

Steinberg::tresult PLUGIN_API Vst3xEditorHost::queryInterface(const Steinberg::TUID iid, void** obj)
{
    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugFrame::iid))
    {
        addRef();
        *obj = static_cast<Steinberg::IPlugFrame*>(this);
        return Steinberg::kResultOk;
    }
    if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid))
    {
        addRef();
        *obj = static_cast<Steinberg::IPlugFrame*>(this);
        return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
}

Steinberg::uint32 PLUGIN_API Vst3xEditorHost::addRef()
{
    return static_cast<Steinberg::uint32>(_ref_count.fetch_add(1, std::memory_order_relaxed) + 1);
}

Steinberg::uint32 PLUGIN_API Vst3xEditorHost::release()
{
    auto new_count = _ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    return static_cast<Steinberg::uint32>(std::max(Steinberg::int32{0}, new_count));
}

Steinberg::tresult PLUGIN_API Vst3xEditorHost::resizeView(Steinberg::IPlugView* view,
                                                           Steinberg::ViewRect* new_size)
{
    if (view != _view || !new_size)
    {
        return Steinberg::kInvalidArgument;
    }

    if (_in_resize)
    {
        return Steinberg::kResultOk;
    }

    _in_resize = true;

    bool accepted = true;
    if (_resize_callback)
    {
        accepted = _resize_callback(static_cast<int>(_processor_id),
                                    new_size->getWidth(),
                                    new_size->getHeight());
    }

    if (accepted)
    {
        view->onSize(new_size);
    }

    _in_resize = false;

    return accepted ? Steinberg::kResultOk : Steinberg::kResultFalse;
}

} // end namespace sushi::internal::vst3
