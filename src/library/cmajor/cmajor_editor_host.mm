/*
 * Copyright 2026 Oliver Larkin
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY AND FITNESS FOR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI. If not, see http://www.gnu.org/licenses/
 */

#import <Cocoa/Cocoa.h>

#include <filesystem>

#include "elklog/static_logger.h"

#include "cmajor_editor_host.h"

#include "cmajor/helpers/cmaj_PatchManifest.h"
#include "cmajor/helpers/cmaj_PatchWebView.h"
#include "choc/gui/choc_WebView.h"
#include "choc/network/choc_MIMETypes.h"

namespace sushi::internal::cmajor_plugin {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("cmajor_editor");

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

bool set_view_size(void* view, uint32_t width, uint32_t height)
{
    if (view == nullptr)
    {
        return false;
    }

    NSView* cocoa_view = (__bridge NSView*) view;
    [cocoa_view setFrame:NSMakeRect(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height))];
    return true;
}

class SushiPatchWebView : public cmaj::PatchView
{
public:
    SushiPatchWebView(cmaj::Patch& patch, const cmaj::PatchManifest::View& view)
        : PatchView(patch, view)
    {
        choc::ui::WebView::Options options;
        options.enableDebugMode = false;
        options.transparentBackground = true;
        options.acceptsFirstMouseClick = true;
        options.fetchResource = [this](const auto& path) { return _on_request(path); };
        options.webviewIsReady = [this, &patch](choc::ui::WebView& ready_view)
        {
            auto bound_ok = ready_view.bind("cmaj_sendMessageToServer",
                                            [this, &patch](const choc::value::ValueView& args) -> choc::value::Value
                                            {
                                                try
                                                {
                                                    if (args.isArray() && args.size() != 0)
                                                    {
                                                        auto message = args[0];
                                                        if (message.isObject() && message["type"].toString() == "load_patch")
                                                        {
                                                            ELKLOG_LOG_WARNING("Ignoring load_patch request from embedded Cmajor view");
                                                            return {};
                                                        }

                                                        patch.handleClientMessage(*this, message);
                                                    }
                                                }
                                                catch (const std::exception& e)
                                                {
                                                    ELKLOG_LOG_ERROR("Error processing message from embedded Cmajor view: {}", e.what());
                                                }

                                                return {};
                                            });

            (void) bound_ok;
        };

        _webview = std::make_unique<choc::ui::WebView>(options);
    }

    void sendMessage(const choc::value::ValueView& msg) override
    {
        _webview->evaluateJavascript("window.cmaj_deliverMessageFromServer?.(" + choc::json::toString(msg, true) + ");");
    }

    choc::ui::WebView& webview() { return *_webview; }

    void reload()
    {
        _webview->evaluateJavascript("document.location.reload()");
    }

private:
    std::optional<choc::ui::WebView::Options::Resource> _on_request(const std::string& path)
    {
        auto relative_path = std::filesystem::path(path).relative_path();

        if (relative_path.empty())
        {
            choc::value::Value manifest_object;
            cmaj::PatchManifest::View view_to_use;

            if (auto manifest = patch.getManifest())
            {
                manifest_object = manifest->manifest;

                if (auto default_view = manifest->findDefaultView())
                {
                    view_to_use = *default_view;
                }
            }

            return choc::ui::WebView::Options::Resource(choc::text::replace(cmaj::cmajor_patch_gui_html,
                                                                             "$MANIFEST$", choc::json::toString(manifest_object, true),
                                                                             "$VIEW_TO_USE$", choc::json::toString(view_to_use.view, true),
                                                                             "$EXTRA_SETUP_CODE$", std::string()),
                                                        "text/html");
        }

        if (auto content = cmaj::readJavascriptResource(path, patch.getManifest()))
        {
            if (!content->empty())
            {
                return choc::ui::WebView::Options::Resource(*content,
                                                            choc::network::getMIMETypeFromFilename(relative_path.extension().string(),
                                                                                                   "application/octet-stream"));
            }
        }

        return {};
    }

    std::unique_ptr<choc::ui::WebView> _webview;
};

bool same_session(const std::optional<CmajorWrapper::EditorSession>& lhs,
                  const std::optional<CmajorWrapper::EditorSession>& rhs)
{
    if (lhs.has_value() != rhs.has_value())
    {
        return false;
    }

    if (!lhs.has_value())
    {
        return true;
    }

    return lhs->patch.get() == rhs->patch.get() &&
           lhs->view.getSource() == rhs->view.getSource() &&
           lhs->view.getWidth() == rhs->view.getWidth() &&
           lhs->view.getHeight() == rhs->view.getHeight() &&
           lhs->view.isResizable() == rhs->view.isResizable();
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

struct CmajorEditorHost::Impl
{
    std::unique_ptr<SushiPatchWebView> view;
    std::optional<CmajorWrapper::EditorSession> session;
};

CmajorEditorHost::CmajorEditorHost(CmajorWrapper& wrapper,
                                   ObjectId processor_id,
                                   CmajorEditorResizeCallback resize_callback)
    : _wrapper(wrapper),
      _processor_id(processor_id),
      _resize_callback(std::move(resize_callback)),
      _impl(std::make_unique<Impl>())
{
}

CmajorEditorHost::~CmajorEditorHost()
{
    close();
}

std::pair<bool, CmajorEditorRect> CmajorEditorHost::open(void* parent_handle)
{
    if (_is_open)
    {
        return {false, {0, 0}};
    }

    auto session = _wrapper.current_editor_session();
    if (!session.has_value())
    {
        return {false, {0, 0}};
    }

    _parent_handle = parent_handle;
    _is_open = true;

    _wrapper.set_editor_session_callback([this](std::optional<CmajorWrapper::EditorSession> next_session)
    {
        auto apply = ^{
            if (this->_is_open)
            {
                this->_set_session(std::move(next_session));
            }
        };

        if (pthread_main_np())
        {
            apply();
        }
        else
        {
            dispatch_sync(dispatch_get_main_queue(), apply);
        }
    });

    _set_session(std::move(session));

    if (!_impl->view)
    {
        close();
        return {false, {0, 0}};
    }

    return {true,
            {static_cast<int>(_impl->view->width),
             static_cast<int>(_impl->view->height)}};
}

void CmajorEditorHost::close()
{
    if (!_is_open)
    {
        return;
    }

    _wrapper.set_editor_session_callback({});
    _set_session(std::nullopt);
    _parent_handle = nullptr;
    _is_open = false;
}

bool CmajorEditorHost::is_open() const
{
    return _is_open;
}

bool CmajorEditorHost::notify_size(int width, int height)
{
    if (!_impl->view)
    {
        return false;
    }

    return set_view_size(_impl->view->webview().getViewHandle(),
                         static_cast<uint32_t>(width),
                         static_cast<uint32_t>(height));
}

void CmajorEditorHost::_set_session(std::optional<CmajorWrapper::EditorSession> session)
{
    if (same_session(_impl->session, session))
    {
        return;
    }

    if (_impl->view)
    {
        remove_native_view(_impl->view->webview().getViewHandle());
        _impl->view.reset();
    }

    _impl->session = std::move(session);

    if (!_impl->session.has_value() || _parent_handle == nullptr)
    {
        return;
    }

    auto new_view = std::make_unique<SushiPatchWebView>(*_impl->session->patch, _impl->session->view);
    auto* native_view = new_view->webview().getViewHandle();

    if (!add_child_view(_parent_handle, native_view))
    {
        ELKLOG_LOG_ERROR("Failed to attach Cmajor editor view");
        _impl->session.reset();
        return;
    }

    set_view_size(native_view, new_view->width, new_view->height);
    _impl->view = std::move(new_view);

    if (_resize_callback)
    {
        _resize_callback(static_cast<int>(_processor_id),
                         static_cast<int>(_impl->view->width),
                         static_cast<int>(_impl->view->height));
    }
}

} // namespace sushi::internal::cmajor_plugin
