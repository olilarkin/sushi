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
 * @brief macOS NSWindow implementation of PluginWindow.
 * @Copyright 2026 Oliver Larkin
 */

#include "vst3x_plugin_window.h"

#import <Cocoa/Cocoa.h>

// Forward-declare the C++ callback type for the delegate
using ResizeFn = std::function<void(int, int)>;

@interface SushiWindowDelegate : NSObject <NSWindowDelegate>
{
    ResizeFn _callback;
}

- (instancetype)initWithCallback:(ResizeFn)callback;

@end

@implementation SushiWindowDelegate

- (instancetype)initWithCallback:(ResizeFn)callback
{
    self = [super init];
    if (self)
    {
        _callback = std::move(callback);
    }
    return self;
}

- (void)windowDidResize:(NSNotification*)notification
{
    if (!_callback)
    {
        return;
    }

    NSWindow* win = notification.object;
    NSRect content = [[win contentView] frame];
    _callback(static_cast<int>(content.size.width), static_cast<int>(content.size.height));
}

@end

namespace sushi::internal::vst3 {

namespace {

void ensure_nsapp()
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (NSApp == nil)
        {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
    });
}

} // anonymous namespace

struct PluginWindow::Impl
{
    NSWindow* window{nil};
    SushiWindowDelegate* delegate{nil};
    bool open{false};
    WindowResizeCallback resize_callback;
};

PluginWindow::PluginWindow() : _impl(std::make_unique<Impl>())
{
}

PluginWindow::~PluginWindow()
{
    close();
}

void* PluginWindow::create(const std::string& title, int width, int height, bool resizable)
{
    ensure_nsapp();

    NSRect frame = NSMakeRect(0, 0, width, height);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
    if (resizable)
    {
        style |= NSWindowStyleMaskResizable;
    }
    NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win setTitle:[NSString stringWithUTF8String:title.c_str()]];
    [win center];
    [win setReleasedWhenClosed:NO];

    auto delegate = [[SushiWindowDelegate alloc] initWithCallback:
        [this](int w, int h) {
            if (_impl->resize_callback)
            {
                _impl->resize_callback(w, h);
            }
        }];
    [win setDelegate:delegate];

    _impl->window = win;
    _impl->delegate = delegate;
    _impl->open = true;
    return (__bridge void*)[win contentView];
}

void PluginWindow::resize(int width, int height)
{
    if (!_impl->window)
    {
        return;
    }

    NSWindow* win = _impl->window;
    NSRect frame = [win frame];
    NSRect content_rect = [win contentRectForFrameRect:frame];
    CGFloat title_bar_height = frame.size.height - content_rect.size.height;
    CGFloat new_height = height + title_bar_height;
    frame.origin.y += frame.size.height - new_height;
    frame.size.width = width;
    frame.size.height = new_height;
    [win setFrame:frame display:YES animate:NO];
}

void PluginWindow::show()
{
    if (!_impl->window)
    {
        return;
    }

    NSWindow* win = _impl->window;
    [NSApp activateIgnoringOtherApps:YES];
    [win makeKeyAndOrderFront:nil];
}

void PluginWindow::close()
{
    if (!_impl->window)
    {
        return;
    }

    NSWindow* win = _impl->window;
    [win setDelegate:nil];
    _impl->delegate = nil;
    _impl->window = nil;
    _impl->open = false;
    [win orderOut:nil];
}

bool PluginWindow::is_open() const
{
    return _impl->open;
}

void PluginWindow::set_resize_callback(WindowResizeCallback callback)
{
    _impl->resize_callback = std::move(callback);
}

void PluginWindow::set_position(int x, int y)
{
    if (!_impl->window)
    {
        return;
    }

    NSWindow* win = _impl->window;
    NSRect screen_frame = [[NSScreen mainScreen] frame];
    NSRect win_frame = [win frame];
    // Convert from top-left screen coords to Cocoa bottom-left origin
    CGFloat cocoa_y = screen_frame.size.height - y - win_frame.size.height;
    [win setFrameOrigin:NSMakePoint(x, cocoa_y)];
}

void PluginWindow::get_frame(int& x, int& y, int& width, int& height) const
{
    if (!_impl->window)
    {
        x = y = width = height = 0;
        return;
    }

    NSWindow* win = _impl->window;
    NSRect screen_frame = [[NSScreen mainScreen] frame];
    NSRect content = [win contentRectForFrameRect:[win frame]];
    x = static_cast<int>(content.origin.x);
    // Convert from Cocoa bottom-left to top-left screen coords
    y = static_cast<int>(screen_frame.size.height - content.origin.y - content.size.height);
    width = static_cast<int>(content.size.width);
    height = static_cast<int>(content.size.height);
}

} // end namespace sushi::internal::vst3
