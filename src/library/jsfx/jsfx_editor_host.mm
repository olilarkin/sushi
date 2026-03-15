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
 * @brief JSFX plugin GFX editor host — framebuffer-based NSView + timer rendering.
 */

#import <Cocoa/Cocoa.h>
#include <vector>

#include "elklog/static_logger.h"

#include "jsfx_editor_host.h"

namespace sushi::internal::jsfx_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("jsfx_editor");

} // namespace sushi::internal::jsfx_wrapper

// ---------------------------------------------------------------------------
// JsfxFramebufferView — NSView subclass that renders libjsfx GFX output
// ---------------------------------------------------------------------------

@interface JsfxFramebufferView : NSView
{
    libjsfx_effect_t* _effect;
    libjsfx_framebuffer_t _framebuffer;
    libjsfx_gfx_input_t _gfxInput;
    std::vector<uint32_t> _pixelBuffer;
    std::vector<uint32_t> _rgbaBuffer;
    NSTimer* _timer;
    int _fbWidth;
    int _fbHeight;
}

- (instancetype)initWithEffect:(libjsfx_effect_t*)effect width:(int)w height:(int)h;
- (void)startTimer;
- (void)stopTimer;

@end

@implementation JsfxFramebufferView

- (instancetype)initWithEffect:(libjsfx_effect_t*)effect width:(int)w height:(int)h
{
    self = [super initWithFrame:NSMakeRect(0, 0, w, h)];
    if (self)
    {
        _effect = effect;
        _fbWidth = w;
        _fbHeight = h;

        size_t num_pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
        _pixelBuffer.resize(num_pixels, 0);
        _rgbaBuffer.resize(num_pixels, 0);

        _framebuffer.pixels = _pixelBuffer.data();
        _framebuffer.width = w;
        _framebuffer.height = h;
        _framebuffer.stride = w;

        memset(&_gfxInput, 0, sizeof(_gfxInput));

        _timer = nil;
    }
    return self;
}

- (void)dealloc
{
    [self stopTimer];
}

- (void)startTimer
{
    if (_timer)
    {
        return;
    }
    // 30 fps
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                              target:self
                                            selector:@selector(renderFrame)
                                            userInfo:nil
                                             repeats:YES];
}

- (void)stopTimer
{
    if (_timer)
    {
        [_timer invalidate];
        _timer = nil;
    }
}

- (void)renderFrame
{
    if (!_effect)
    {
        return;
    }

    libjsfx_render_gfx(_effect, &_framebuffer, &_gfxInput);
    _gfxInput.mouse_wheel = 0;

    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect
{
    (void)dirtyRect;

    int numPixels = _fbWidth * _fbHeight;

    // Convert BGRA (libjsfx) to RGBA (CoreGraphics kCGImageAlphaLast)
    for (int i = 0; i < numPixels; i++)
    {
        uint32_t bgra = _pixelBuffer[static_cast<size_t>(i)];
        uint8_t b = bgra & 0xFF;
        uint8_t g = (bgra >> 8) & 0xFF;
        uint8_t r = (bgra >> 16) & 0xFF;
        uint8_t a = (bgra >> 24) & 0xFF;
        if (a == 0 && (r || g || b))
        {
            a = 255;
        }
        _rgbaBuffer[static_cast<size_t>(i)] = r | (g << 8) | (b << 16) | (static_cast<uint32_t>(a) << 24);
    }

    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr,
        _rgbaBuffer.data(),
        static_cast<size_t>(numPixels) * 4,
        nullptr);

    if (provider)
    {
        CGImageRef image = CGImageCreate(
            static_cast<size_t>(_fbWidth),
            static_cast<size_t>(_fbHeight),
            8,
            32,
            static_cast<size_t>(_fbWidth) * 4,
            colorSpace,
            kCGImageAlphaLast | kCGBitmapByteOrderDefault,
            provider,
            nullptr,
            false,
            kCGRenderingIntentDefault);

        if (image)
        {
            NSImage* nsImage = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(_fbWidth, _fbHeight)];
            [nsImage drawInRect:[self bounds]];
            CGImageRelease(image);
        }
        CGDataProviderRelease(provider);
    }
    CGColorSpaceRelease(colorSpace);
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    (void)event;
    return YES;
}

- (void)_updateMouseFromEvent:(NSEvent*)event buttons:(int)buttons
{
    NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
    _gfxInput.mouse_x = static_cast<int>(loc.x);
    _gfxInput.mouse_y = static_cast<int>(NSHeight([self bounds]) - loc.y);

    int modifiers = 0;
    NSEventModifierFlags flags = [event modifierFlags];
    if (flags & NSEventModifierFlagControl)
    {
        modifiers |= 8;
    }
    if (flags & NSEventModifierFlagShift)
    {
        modifiers |= 16;
    }
    if (flags & NSEventModifierFlagOption)
    {
        modifiers |= 32;
    }

    _gfxInput.mouse_buttons = buttons | modifiers;
}

- (void)mouseDown:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:1];
}

- (void)mouseUp:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:0];
}

- (void)mouseDragged:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:1];
}

- (void)mouseMoved:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:0];
}

- (void)rightMouseDown:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:2];
}

- (void)rightMouseUp:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:0];
}

- (void)rightMouseDragged:(NSEvent*)event
{
    [self _updateMouseFromEvent:event buttons:2];
}

- (void)scrollWheel:(NSEvent*)event
{
    _gfxInput.mouse_wheel += static_cast<int>([event deltaY]);
}

@end

// ---------------------------------------------------------------------------
// JsfxEditorHost implementation
// ---------------------------------------------------------------------------

namespace sushi::internal::jsfx_wrapper {

JsfxEditorHost::JsfxEditorHost(libjsfx_effect_t* effect,
                               ObjectId processor_id,
                               JsfxEditorResizeCallback resize_callback)
    : _effect(effect),
      _processor_id(processor_id),
      _resize_callback(std::move(resize_callback))
{
}

JsfxEditorHost::~JsfxEditorHost()
{
    close();
}

std::pair<bool, JsfxEditorRect> JsfxEditorHost::open(void* parent_handle)
{
    if (_is_open)
    {
        return {false, {0, 0}};
    }

    int gfx_w = 0;
    int gfx_h = 0;
    libjsfx_get_gfx_size(_effect, &gfx_w, &gfx_h);
    if (gfx_w <= 0)
    {
        gfx_w = 640;
    }
    if (gfx_h <= 0)
    {
        gfx_h = 480;
    }

    JsfxFramebufferView* view = [[JsfxFramebufferView alloc] initWithEffect:_effect
                                                                      width:gfx_w
                                                                     height:gfx_h];

    NSView* parent_view = (__bridge NSView*)parent_handle;

    // Resize the host window to match the GFX view before adding it
    NSWindow* win = [parent_view window];
    if (win)
    {
        NSRect win_frame = [win frame];
        NSRect content_rect = [win contentRectForFrameRect:win_frame];
        CGFloat title_bar_height = win_frame.size.height - content_rect.size.height;
        CGFloat new_height = gfx_h + title_bar_height;
        win_frame.origin.y += win_frame.size.height - new_height;
        win_frame.size.width = gfx_w;
        win_frame.size.height = new_height;
        [win setFrame:win_frame display:YES animate:NO];
    }

    [view setFrame:[parent_view bounds]];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [parent_view addSubview:view];
    [view startTimer];

    _cocoa_view = (__bridge_retained void*)view;
    _is_open = true;

    JsfxEditorRect rect{gfx_w, gfx_h};

    ELKLOG_LOG_DEBUG("JSFX editor opened: {}x{}", rect.width, rect.height);
    return {true, rect};
}

void JsfxEditorHost::close()
{
    if (!_is_open || !_cocoa_view)
    {
        return;
    }

    JsfxFramebufferView* view = (__bridge_transfer JsfxFramebufferView*)_cocoa_view;
    [view stopTimer];
    [view removeFromSuperview];

    _cocoa_view = nullptr;
    _is_open = false;

    ELKLOG_LOG_DEBUG("JSFX editor closed");
}

bool JsfxEditorHost::is_open() const
{
    return _is_open;
}

} // namespace sushi::internal::jsfx_wrapper
