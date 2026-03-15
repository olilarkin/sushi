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
 * @brief JSFX plugin GFX editor host for macOS.
 */

#ifndef SUSHI_JSFX_EDITOR_HOST_H
#define SUSHI_JSFX_EDITOR_HOST_H

#include <functional>

#include "libjsfx/libjsfx.h"

#include "sushi/constants.h"

#include "library/id_generator.h"

namespace sushi::internal::jsfx_wrapper {

struct JsfxEditorRect
{
    int width;
    int height;
};

using JsfxEditorResizeCallback = std::function<bool(int processor_id, int width, int height)>;

class JsfxEditorHost
{
public:
    SUSHI_DECLARE_NON_COPYABLE(JsfxEditorHost);

    JsfxEditorHost(libjsfx_effect_t* effect,
                   ObjectId processor_id,
                   JsfxEditorResizeCallback resize_callback);

    ~JsfxEditorHost();

    std::pair<bool, JsfxEditorRect> open(void* parent_handle);
    void close();
    bool is_open() const;

private:
    libjsfx_effect_t* _effect;
    [[maybe_unused]] ObjectId _processor_id;
    JsfxEditorResizeCallback _resize_callback;
    bool _is_open{false};

    // Opaque pointer to Cocoa view (NSView*)
    void* _cocoa_view{nullptr};
};

} // namespace sushi::internal::jsfx_wrapper

#endif // SUSHI_JSFX_EDITOR_HOST_H
