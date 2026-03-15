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
 * @brief AUv2 plugin GUI lifecycle manager.
 */

#ifndef SUSHI_AUV2_EDITOR_HOST_H
#define SUSHI_AUV2_EDITOR_HOST_H

#include <functional>

#include <AudioToolbox/AudioToolbox.h>

#include "sushi/constants.h"

#include "library/id_generator.h"

namespace sushi::internal::auv2_wrapper {

struct AUv2EditorRect
{
    int width;
    int height;
};

using AUv2EditorResizeCallback = std::function<bool(int processor_id, int width, int height)>;

class AUv2EditorHost
{
public:
    SUSHI_DECLARE_NON_COPYABLE(AUv2EditorHost);

    AUv2EditorHost(AudioUnit audio_unit,
                   ObjectId processor_id,
                   AUv2EditorResizeCallback resize_callback);

    ~AUv2EditorHost();

    std::pair<bool, AUv2EditorRect> open(void* parent_handle);
    void close();
    bool is_open() const;

private:
    AudioUnit _audio_unit;
    [[maybe_unused]] ObjectId _processor_id;
    AUv2EditorResizeCallback _resize_callback;
    bool _is_open{false};

    // Opaque pointer to Cocoa view (NSView*)
    void* _cocoa_view{nullptr};
};

} // end namespace sushi::internal::auv2_wrapper

#endif // SUSHI_AUV2_EDITOR_HOST_H
