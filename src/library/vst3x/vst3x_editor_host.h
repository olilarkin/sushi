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
 * @brief IPlugFrame implementation and IPlugView lifecycle manager for VST3 plugin editors.
 * @Copyright 2026 Oliver Larkin
 */

#ifndef SUSHI_VST3X_EDITOR_HOST_H
#define SUSHI_VST3X_EDITOR_HOST_H

#include <atomic>
#include <functional>

#include "sushi/constants.h"

#include "elk-warning-suppressor/warning_suppressor.hpp"

ELK_PUSH_WARNING
ELK_DISABLE_EXTRA
ELK_DISABLE_DEPRECATED_DECLARATIONS
ELK_DISABLE_SHORTEN_64_TO_32

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

ELK_POP_WARNING

#include "library/id_generator.h"

namespace sushi::internal::vst3 {

struct EditorRect
{
    int width;
    int height;
};

using EditorResizeCallback = std::function<bool(int processor_id, int width, int height)>;

class Vst3xEditorHost : public Steinberg::IPlugFrame
{
public:
    SUSHI_DECLARE_NON_COPYABLE(Vst3xEditorHost);

    Vst3xEditorHost(Steinberg::Vst::IEditController* controller, ObjectId processor_id,
                    EditorResizeCallback resize_callback);

    ~Vst3xEditorHost();

    std::pair<bool, EditorRect> open(void* parent_handle);
    void close();
    bool is_open() const;
    bool set_content_scale_factor(float scale_factor);
    bool notify_size(int width, int height);

    // IPlugFrame
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view,
                                             Steinberg::ViewRect* new_size) override;

    // FUnknown
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;

private:
    Steinberg::Vst::IEditController* _controller;
    Steinberg::IPlugView* _view{nullptr};
    ObjectId _processor_id;
    EditorResizeCallback _resize_callback;
    bool _in_resize{false};
    std::atomic<Steinberg::int32> _ref_count{1};
};

} // end namespace sushi::internal::vst3

#endif // SUSHI_VST3X_EDITOR_HOST_H
