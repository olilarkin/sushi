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

#import <Cocoa/Cocoa.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#include "elklog/static_logger.h"

#include "auv2_editor_host.h"

namespace sushi::internal::auv2_wrapper {

ELKLOG_GET_LOGGER_WITH_MODULE_NAME("auv2_editor");

AUv2EditorHost::AUv2EditorHost(AudioUnit audio_unit,
                                ObjectId processor_id,
                                AUv2EditorResizeCallback resize_callback) :
    _audio_unit(audio_unit),
    _processor_id(processor_id),
    _resize_callback(std::move(resize_callback))
{
}

AUv2EditorHost::~AUv2EditorHost()
{
    close();
}

std::pair<bool, AUv2EditorRect> AUv2EditorHost::open(void* parent_handle)
{
    if (_is_open)
    {
        return {false, {0, 0}};
    }

    // Get the Cocoa UI view info
    UInt32 data_size = 0;
    Boolean writable = false;
    OSStatus status = AudioUnitGetPropertyInfo(_audio_unit,
                                               kAudioUnitProperty_CocoaUI,
                                               kAudioUnitScope_Global,
                                               0,
                                               &data_size,
                                               &writable);
    if (status != noErr || data_size == 0)
    {
        ELKLOG_LOG_ERROR("AUv2 plugin has no Cocoa UI");
        return {false, {0, 0}};
    }

    auto num_classes = (data_size - sizeof(CFURLRef)) / sizeof(CFStringRef);
    if (num_classes == 0)
    {
        ELKLOG_LOG_ERROR("AUv2 Cocoa UI has no view classes");
        return {false, {0, 0}};
    }

    std::vector<uint8_t> buffer(data_size);
    auto* cocoa_info = reinterpret_cast<AudioUnitCocoaViewInfo*>(buffer.data());

    status = AudioUnitGetProperty(_audio_unit,
                                  kAudioUnitProperty_CocoaUI,
                                  kAudioUnitScope_Global,
                                  0,
                                  cocoa_info,
                                  &data_size);
    if (status != noErr)
    {
        ELKLOG_LOG_ERROR("Failed to get AUv2 Cocoa UI info, status: {}", status);
        return {false, {0, 0}};
    }

    CFURLRef bundle_url = cocoa_info->mCocoaAUViewBundleLocation;
    CFStringRef class_name = cocoa_info->mCocoaAUViewClass[0];

    // Load the bundle
    NSBundle* bundle = [NSBundle bundleWithURL:(__bridge NSURL*)bundle_url];
    if (!bundle)
    {
        ELKLOG_LOG_ERROR("Failed to load AUv2 UI bundle");
        CFRelease(bundle_url);
        CFRelease(class_name);
        return {false, {0, 0}};
    }

    NSString* class_name_ns = (__bridge NSString*)class_name;
    Class view_class = [bundle classNamed:class_name_ns];

    CFRelease(bundle_url);
    CFRelease(class_name);

    if (!view_class)
    {
        ELKLOG_LOG_ERROR("Failed to find AUv2 UI view class");
        return {false, {0, 0}};
    }

    // Create the view
    id<AUCocoaUIBase> factory = [[view_class alloc] init];
    if (!factory)
    {
        ELKLOG_LOG_ERROR("Failed to create AUv2 UI factory");
        return {false, {0, 0}};
    }

    AudioUnit au = _audio_unit;
    NSView* view = [factory uiViewForAudioUnit:au withSize:NSZeroSize];
    if (!view)
    {
        ELKLOG_LOG_ERROR("Failed to create AUv2 UI view");
        return {false, {0, 0}};
    }

    NSView* parent_view = (__bridge NSView*)parent_handle;
    NSRect view_frame = [view frame];
    int width = static_cast<int>(view_frame.size.width);
    int height = static_cast<int>(view_frame.size.height);

    // Resize the host window to match the AU view BEFORE adding it.
    // AU views lay out their content relative to the parent bounds at
    // insertion time, so the parent must already be the correct size.
    NSWindow* win = [parent_view window];
    if (win)
    {
        NSRect win_frame = [win frame];
        NSRect content_rect = [win contentRectForFrameRect:win_frame];
        CGFloat title_bar_height = win_frame.size.height - content_rect.size.height;
        CGFloat new_height = height + title_bar_height;
        win_frame.origin.y += win_frame.size.height - new_height;
        win_frame.size.width = width;
        win_frame.size.height = new_height;
        [win setFrame:win_frame display:YES animate:NO];
    }

    [view setFrame:[parent_view bounds]];
    [parent_view addSubview:view];

    _cocoa_view = (__bridge_retained void*)view;
    _is_open = true;

    AUv2EditorRect rect{width, height};

    ELKLOG_LOG_DEBUG("AUv2 editor opened: {}x{}", rect.width, rect.height);
    return {true, rect};
}

void AUv2EditorHost::close()
{
    if (!_is_open || !_cocoa_view)
    {
        return;
    }

    NSView* view = (__bridge_transfer NSView*)_cocoa_view;
    [view removeFromSuperview];

    _cocoa_view = nullptr;
    _is_open = false;

    ELKLOG_LOG_DEBUG("AUv2 editor closed");
}

bool AUv2EditorHost::is_open() const
{
    return _is_open;
}

} // end namespace sushi::internal::auv2_wrapper
