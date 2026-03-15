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

#ifndef SUSHI_FAUST_EDITOR_HOST_H
#define SUSHI_FAUST_EDITOR_HOST_H

#include <functional>
#include <memory>

#include "sushi/constants.h"

#include "library/id_generator.h"

namespace sushi::internal::faust_wrapper {

class FaustWrapper;

struct FaustEditorRect
{
    int width;
    int height;
};

using FaustEditorResizeCallback = std::function<bool(int processor_id, int width, int height)>;

class FaustEditorHost
{
public:
    SUSHI_DECLARE_NON_COPYABLE(FaustEditorHost);

    FaustEditorHost(FaustWrapper& wrapper,
                    ObjectId processor_id,
                    FaustEditorResizeCallback resize_callback);

    ~FaustEditorHost();

    std::pair<bool, FaustEditorRect> open(void* parent_handle);
    void close();
    bool is_open() const;

private:
    struct Impl;

    FaustWrapper& _wrapper;
    ObjectId _processor_id;
    FaustEditorResizeCallback _resize_callback;
    std::unique_ptr<Impl> _impl;
    void* _parent_handle{nullptr};
    bool _is_open{false};
};

} // namespace sushi::internal::faust_wrapper

#endif // SUSHI_FAUST_EDITOR_HOST_H
