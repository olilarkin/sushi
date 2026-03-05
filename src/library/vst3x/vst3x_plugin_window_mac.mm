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
 * @brief macOS NSWindow implementation of PluginWindow.
 * @Copyright 2017-2023 Elk Audio AB, Stockholm
 */

#include "vst3x_plugin_window.h"

#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <dlfcn.h>

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

namespace {

// Function pointer type matching CGWindowListCreateImage signature
using CGWindowListCreateImageFunc = CGImageRef (*)(CGRect, uint32_t, uint32_t, uint32_t);

CGImageRef capture_window_via_cgwindowlist(CGWindowID window_id)
{
    static CGWindowListCreateImageFunc fn = nullptr;
    static bool loaded = false;

    if (!loaded)
    {
        loaded = true;
        void* handle = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
        if (handle)
        {
            fn = reinterpret_cast<CGWindowListCreateImageFunc>(dlsym(handle, "CGWindowListCreateImage"));
        }
    }

    if (!fn)
    {
        return nullptr;
    }

    return fn(CGRectNull,
              kCGWindowListOptionIncludingWindow,
              window_id,
              kCGWindowImageBoundsIgnoreFraming);
}

bool capture_window_via_screencapturekit(CGWindowID window_id, NSString* path_str, NSBitmapImageFileType file_type)
{
    __block bool success = false;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
        if (error || !content)
        {
            dispatch_semaphore_signal(semaphore);
            return;
        }

        SCWindow* target_window = nil;
        for (SCWindow* sc_window in content.windows)
        {
            if (sc_window.windowID == window_id)
            {
                target_window = sc_window;
                break;
            }
        }

        if (!target_window)
        {
            dispatch_semaphore_signal(semaphore);
            return;
        }

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target_window];
        SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
        config.captureResolution = SCCaptureResolutionAutomatic;
        config.showsCursor = NO;
        config.shouldBeOpaque = YES;

        [SCScreenshotManager captureImageWithFilter:filter
                                      configuration:config
                                  completionHandler:^(CGImageRef image, NSError* capture_error) {
            if (capture_error || !image)
            {
                dispatch_semaphore_signal(semaphore);
                return;
            }

            NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:image];
            NSData* data = [rep representationUsingType:file_type properties:@{}];
            if (data)
            {
                success = [data writeToFile:path_str atomically:YES];
            }

            dispatch_semaphore_signal(semaphore);
        }];
    }];

    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return success;
}

NSData* resize_and_encode(CGImageRef source, int max_width, int max_height, NSBitmapImageFileType file_type)
{
    size_t src_w = CGImageGetWidth(source);
    size_t src_h = CGImageGetHeight(source);
    size_t dst_w = src_w;
    size_t dst_h = src_h;

    if (max_width > 0 && max_height > 0 && (src_w > static_cast<size_t>(max_width) || src_h > static_cast<size_t>(max_height)))
    {
        double scale_w = static_cast<double>(max_width) / src_w;
        double scale_h = static_cast<double>(max_height) / src_h;
        double scale = std::min(scale_w, scale_h);
        dst_w = static_cast<size_t>(src_w * scale);
        dst_h = static_cast<size_t>(src_h * scale);
    }
    else if (max_width > 0 && src_w > static_cast<size_t>(max_width))
    {
        double scale = static_cast<double>(max_width) / src_w;
        dst_w = max_width;
        dst_h = static_cast<size_t>(src_h * scale);
    }
    else if (max_height > 0 && src_h > static_cast<size_t>(max_height))
    {
        double scale = static_cast<double>(max_height) / src_h;
        dst_h = max_height;
        dst_w = static_cast<size_t>(src_w * scale);
    }

    if (dst_w != src_w || dst_h != src_h)
    {
        CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(nullptr, dst_w, dst_h, 8, dst_w * 4,
                                                  color_space, kCGImageAlphaPremultipliedLast);
        CGColorSpaceRelease(color_space);
        if (!ctx)
        {
            return nil;
        }

        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextDrawImage(ctx, CGRectMake(0, 0, dst_w, dst_h), source);
        CGImageRef resized = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        if (!resized)
        {
            return nil;
        }

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:resized];
        CGImageRelease(resized);
        return [rep representationUsingType:file_type properties:@{}];
    }

    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:source];
    return [rep representationUsingType:file_type properties:@{}];
}

} // anonymous namespace

bool PluginWindow::capture_screenshot(const std::string& output_path, int max_width, int max_height) const
{
    if (!_impl->window)
    {
        return false;
    }

    NSWindow* win = _impl->window;
    CGWindowID window_id = static_cast<CGWindowID>([win windowNumber]);

    NSString* path_str = [NSString stringWithUTF8String:output_path.c_str()];
    NSString* extension = [[path_str pathExtension] lowercaseString];
    NSBitmapImageFileType file_type = NSBitmapImageFileTypePNG;
    if ([extension isEqualToString:@"jpg"] || [extension isEqualToString:@"jpeg"])
    {
        file_type = NSBitmapImageFileTypeJPEG;
    }

    // Try CGWindowListCreateImage first (loaded via dlsym to bypass availability check)
    CGImageRef image = capture_window_via_cgwindowlist(window_id);
    if (image)
    {
        // Crop out the title bar to get content area only
        NSRect frame = [win frame];
        NSRect content_rect = [win contentRectForFrameRect:frame];
        CGFloat scale = [win backingScaleFactor];
        CGFloat title_bar_height = (frame.size.height - content_rect.size.height) * scale;
        size_t img_w = CGImageGetWidth(image);
        size_t img_h = CGImageGetHeight(image);

        CGImageRef content_image = image;
        if (title_bar_height > 0 && static_cast<size_t>(title_bar_height) < img_h)
        {
            CGRect crop = CGRectMake(0, title_bar_height, img_w, img_h - static_cast<size_t>(title_bar_height));
            content_image = CGImageCreateWithImageInRect(image, crop);
            CGImageRelease(image);
        }

        if (content_image)
        {
            NSData* data = resize_and_encode(content_image, max_width, max_height, file_type);
            CGImageRelease(content_image);

            if (data && [data writeToFile:path_str atomically:YES])
            {
                return true;
            }
        }
    }

    // Fall back to ScreenCaptureKit (includes window chrome)
    return capture_window_via_screencapturekit(window_id, path_str, file_type);
}

} // end namespace sushi::internal::vst3
