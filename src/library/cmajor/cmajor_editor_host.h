/*
 * Copyright 2017-2023 Elk Audio AB
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

#ifndef SUSHI_CMAJOR_EDITOR_HOST_H
#define SUSHI_CMAJOR_EDITOR_HOST_H

#include <functional>
#include <memory>
#include <optional>

#include "sushi/constants.h"

#include "library/id_generator.h"
#include "cmajor_wrapper.h"

namespace sushi::internal::cmajor_plugin {

struct CmajorEditorRect
{
    int width;
    int height;
};

using CmajorEditorResizeCallback = std::function<bool(int processor_id, int width, int height)>;

class CmajorEditorHost
{
public:
    SUSHI_DECLARE_NON_COPYABLE(CmajorEditorHost);

    CmajorEditorHost(CmajorWrapper& wrapper,
                     ObjectId processor_id,
                     CmajorEditorResizeCallback resize_callback);

    ~CmajorEditorHost();

    std::pair<bool, CmajorEditorRect> open(void* parent_handle);
    void close();
    bool is_open() const;
    bool notify_size(int width, int height);

private:
    void _set_session(std::optional<CmajorWrapper::EditorSession> session);

    struct Impl;

    CmajorWrapper& _wrapper;
    ObjectId _processor_id;
    CmajorEditorResizeCallback _resize_callback;
    std::unique_ptr<Impl> _impl;
    void* _parent_handle {nullptr};
    bool _is_open {false};
};

} // namespace sushi::internal::cmajor_plugin

#endif // SUSHI_CMAJOR_EDITOR_HOST_H
