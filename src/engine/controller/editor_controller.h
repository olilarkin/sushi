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

#ifndef SUSHI_EDITOR_CONTROLLER_H
#define SUSHI_EDITOR_CONTROLLER_H

#include <mutex>
#include <unordered_map>
#include <memory>

#include "sushi/control_interface.h"

#include "engine/base_processor_container.h"

#ifdef SUSHI_BUILD_WITH_VST3
#include "library/vst3x/vst3x_editor_host.h"
#include "library/vst3x/vst3x_plugin_window.h"
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
#include "library/clap/clap_editor_host.h"
#include "library/vst3x/vst3x_plugin_window.h"
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
#include "library/auv2/auv2_editor_host.h"
#include "library/vst3x/vst3x_plugin_window.h"
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
#include "library/cmajor/cmajor_editor_host.h"
#include "library/vst3x/vst3x_plugin_window.h"
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
#include "library/jsfx/jsfx_editor_host.h"
#include "library/vst3x/vst3x_plugin_window.h"
#endif

#if defined(SUSHI_BUILD_WITH_FAUST) && defined(__APPLE__)
#include "library/faust/faust_editor_host.h"
#endif

namespace sushi::internal::engine::controller_impl {

class EditorController : public control::EditorController
{
public:
    explicit EditorController(const BaseProcessorContainer* processors);

    ~EditorController() override;

    std::pair<control::ControlStatus, bool> has_editor(int processor_id) const override;

    std::pair<control::ControlStatus, control::EditorRect> open_editor(int processor_id, void* parent_handle) override;

    std::pair<control::ControlStatus, control::EditorRect> open_editor(int processor_id) override;

    control::ControlStatus close_editor(int processor_id) override;

    std::pair<control::ControlStatus, bool> is_editor_open(int processor_id) const override;

    void set_resize_callback(control::EditorResizeCallback callback) override;

    control::ControlStatus set_content_scale_factor(int processor_id, float scale_factor) override;

    std::pair<control::ControlStatus, control::EditorRect> get_editor_info(int processor_id) const override;

    control::ControlStatus set_editor_position(int processor_id, int x, int y) override;

    control::ControlStatus capture_editor_screenshot(int processor_id, const std::string& output_path, int max_width, int max_height) override;

private:
    const BaseProcessorContainer* _processors;

#if defined(SUSHI_BUILD_WITH_VST3) || defined(SUSHI_BUILD_WITH_CLAP) || defined(SUSHI_BUILD_WITH_AUV2) || (defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)) || (defined(SUSHI_BUILD_WITH_FAUST) && defined(__APPLE__))
    std::unordered_map<ObjectId, std::unique_ptr<vst3::PluginWindow>> _windows;
    std::unordered_map<ObjectId, control::EditorRect> _saved_frames;
    control::EditorResizeCallback _resize_callback;
    mutable std::mutex _mutex;
#endif

#ifdef SUSHI_BUILD_WITH_VST3
    std::unordered_map<ObjectId, std::unique_ptr<vst3::Vst3xEditorHost>> _vst3_editors;
#endif

#ifdef SUSHI_BUILD_WITH_CLAP
    std::unordered_map<ObjectId, std::unique_ptr<clap_wrapper::ClapEditorHost>> _clap_editors;
#endif

#ifdef SUSHI_BUILD_WITH_AUV2
    std::unordered_map<ObjectId, std::unique_ptr<auv2_wrapper::AUv2EditorHost>> _auv2_editors;
#endif

#if defined(SUSHI_BUILD_WITH_CMAJOR) && defined(__APPLE__)
    std::unordered_map<ObjectId, std::unique_ptr<cmajor_plugin::CmajorEditorHost>> _cmajor_editors;
#endif

#if defined(SUSHI_BUILD_WITH_JSFX) && defined(__APPLE__)
    std::unordered_map<ObjectId, std::unique_ptr<jsfx_wrapper::JsfxEditorHost>> _jsfx_editors;
#endif

#if defined(SUSHI_BUILD_WITH_FAUST) && defined(__APPLE__)
    std::unordered_map<ObjectId, std::unique_ptr<faust_wrapper::FaustEditorHost>> _faust_editors;
#endif
};

} // end namespace sushi::internal::engine::controller_impl

#endif // SUSHI_EDITOR_CONTROLLER_H
