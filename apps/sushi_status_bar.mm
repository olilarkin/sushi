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
 * @brief macOS status bar icon for Sushi GUI.
 * @Copyright 2026 Oliver Larkin
 */

#include <atomic>
#include <set>
#include <string>

#import <Cocoa/Cocoa.h>

#include "sushi/control_interface.h"
#include "sushi/control_notifications.h"
#include "sushi_status_bar.h"

using namespace sushi::control;

// ---------------------------------------------------------------------------
// C++ notification bridge: receives on event dispatcher thread, forwards to main
// ---------------------------------------------------------------------------

class StatusBarNotificationBridge final : public ControlListener
{
public:
    ~StatusBarNotificationBridge() = default;

    StatusBarNotificationBridge(SushiStatusBar* owner) : _owner(owner) {}

    void shutdown() { _shutdown.store(true, std::memory_order_release); }

    void notification(const ControlNotification* notification) override
    {
        if (_shutdown.load(std::memory_order_acquire))
        {
            return;
        }

        switch (notification->type())
        {
            case NotificationType::TRANSPORT_UPDATE:
            {
                auto* tn = static_cast<const TransportNotification*>(notification);
                auto action = tn->action();
                auto value = tn->value();
                SushiStatusBar* owner = _owner;

                switch (action)
                {
                    case TransportAction::PLAYING_MODE_CHANGED:
                        if (auto* pm = std::get_if<PlayingMode>(&value))
                        {
                            int mode = static_cast<int>(*pm);
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [owner handlePlayingModeChanged:mode];
                            });
                        }
                        break;
                    case TransportAction::SYNC_MODE_CHANGED:
                        if (auto* sm = std::get_if<SyncMode>(&value))
                        {
                            int mode = static_cast<int>(*sm);
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [owner handleSyncModeChanged:mode];
                            });
                        }
                        break;
                    case TransportAction::TEMPO_CHANGED:
                        if (auto* t = std::get_if<float>(&value))
                        {
                            float tempo = *t;
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [owner handleTempoChanged:tempo];
                            });
                        }
                        break;
                    case TransportAction::TIME_SIGNATURE_CHANGED:
                        if (auto* ts = std::get_if<TimeSignature>(&value))
                        {
                            int num = ts->numerator;
                            int den = ts->denominator;
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [owner handleTimeSigChanged:num denominator:den];
                            });
                        }
                        break;
                }
                break;
            }
            case NotificationType::CPU_TIMING_UPDATE:
            {
                auto* cn = static_cast<const CpuTimingNotification*>(notification);
                auto timings = cn->cpu_timings();
                float avg = timings.avg;
                float mn = timings.min;
                float mx = timings.max;
                SushiStatusBar* owner = _owner;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [owner handleCpuTimingsAvg:avg min:mn max:mx];
                });
                break;
            }
            case NotificationType::TRACK_UPDATE:
            case NotificationType::PROCESSOR_UPDATE:
            {
                SushiStatusBar* owner = _owner;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [owner invalidateTrackCache];
                });
                break;
            }
            default:
                break;
        }
    }

private:
    SushiStatusBar* _owner;
    std::atomic<bool> _shutdown{false};
};

// ---------------------------------------------------------------------------
// Menu item tag encoding for processor toggle actions
// ---------------------------------------------------------------------------

static constexpr NSInteger kProcessorTagBase = 10000;

static NSString* playing_mode_string(PlayingMode mode)
{
    switch (mode)
    {
        case PlayingMode::PLAYING:   return @"\u25A0 Stop";
        case PlayingMode::STOPPED:   return @"\u25B6 Play";
        case PlayingMode::RECORDING: return @"\u25B6 Play";
    }
    return @"\u25B6 Play";
}

static NSString* sync_mode_string(SyncMode mode)
{
    switch (mode)
    {
        case SyncMode::INTERNAL: return @"Internal";
        case SyncMode::MIDI:     return @"MIDI";
        case SyncMode::GATE:     return @"Gate";
        case SyncMode::LINK:     return @"Link";
    }
    return @"Internal";
}

static NSString* plugin_type_string(PluginType type)
{
    switch (type)
    {
        case PluginType::INTERNAL: return @"internal";
        case PluginType::VST2X:    return @"vst2";
        case PluginType::VST3X:    return @"vst3";
        case PluginType::LV2:      return @"lv2";
        case PluginType::CLAP:     return @"clap";
        case PluginType::AUV2:     return @"auv2";
        case PluginType::CMAJOR:   return @"cmajor";
    }
    return @"";
}

// ---------------------------------------------------------------------------
// SushiStatusBar private interface
// ---------------------------------------------------------------------------

@interface SushiStatusBar () <NSMenuDelegate>
{
    SushiControl* _controller;
    std::unique_ptr<StatusBarNotificationBridge> _bridge;

    NSStatusItem* _statusItem;
    NSMenu* _menu;

    // Tempo edit panel
    NSPanel* _tempoPanel;
    NSTextField* _tempoField;

    // Cached state updated via notifications
    PlayingMode _playingMode;
    SyncMode _syncMode;
    float _tempo;
    TimeSignature _timeSig;
    CpuTimings _cpuTimings;
    bool _trackCacheDirty;

    // Set of processor IDs whose editors the user has explicitly opened
    std::set<int> _openEditorIds;

    // Static info queried once
    NSString* _versionString;
    NSString* _audioInfoString;
    int _bufferSize;
    int _inputChannels;
    int _outputChannels;
    float _sampleRate;
}

@end

// ---------------------------------------------------------------------------
// SushiStatusBar implementation
// ---------------------------------------------------------------------------

@implementation SushiStatusBar

- (instancetype)initWithController:(SushiControl*)controller
{
    self = [super init];
    if (!self)
    {
        return nil;
    }

    _controller = controller;

    // Query static info
    auto* sys = _controller->system_controller();
    auto build_info = sys->get_sushi_build_info();
    _versionString = [NSString stringWithFormat:@"Sushi %s",
                      build_info.version.c_str()];
    _inputChannels = sys->get_input_audio_channel_count();
    _outputChannels = sys->get_output_audio_channel_count();
    _bufferSize = build_info.audio_buffer_size;

    auto* transport = _controller->transport_controller();
    _sampleRate = transport->get_samplerate();
    _playingMode = transport->get_playing_mode();
    _syncMode = transport->get_sync_mode();
    _tempo = transport->get_tempo();
    _timeSig = transport->get_time_signature();
    _cpuTimings = {0, 0, 0};
    _trackCacheDirty = true;

    _audioInfoString = [NSString stringWithFormat:@"%@ (%.0f Hz, buf %d) | I/O: %din / %dout",
                        _versionString, _sampleRate, _bufferSize,
                        _inputChannels, _outputChannels];

    // Create status item
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.image = [NSImage imageWithSystemSymbolName:@"waveform"
                                         accessibilityDescription:@"Sushi"];
    _statusItem.button.imagePosition = NSImageLeft;
    [self updateButtonTitle];

    // Create menu (shown on right-click / ctrl-click)
    _menu = [[NSMenu alloc] init];
    _menu.delegate = self;
    _menu.autoenablesItems = NO;

    // Left-click: open all editors. Right/ctrl-click: show menu.
    _statusItem.button.target = self;
    _statusItem.button.action = @selector(statusBarClicked:);
    [_statusItem.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];

    // Subscribe to notifications
    _bridge = std::make_unique<StatusBarNotificationBridge>(self);
    _controller->subscribe_to_notifications(NotificationType::TRANSPORT_UPDATE, _bridge.get());
    _controller->subscribe_to_notifications(NotificationType::CPU_TIMING_UPDATE, _bridge.get());
    _controller->subscribe_to_notifications(NotificationType::TRACK_UPDATE, _bridge.get());
    _controller->subscribe_to_notifications(NotificationType::PROCESSOR_UPDATE, _bridge.get());

    // Enable timing stats so we get CPU notifications
    _controller->timing_controller()->set_timing_statistics_enabled(true);

    return self;
}

- (void)teardown
{
    if (_bridge)
    {
        _bridge->shutdown();
    }

    if (_statusItem)
    {
        [[NSStatusBar systemStatusBar] removeStatusItem:_statusItem];
        _statusItem = nil;
    }

    _bridge.reset();
    _controller = nullptr;
}

// ---------------------------------------------------------------------------
// Notification handlers (called on main thread)
// ---------------------------------------------------------------------------

- (void)handlePlayingModeChanged:(int)mode
{
    _playingMode = static_cast<PlayingMode>(mode);
}

- (void)handleSyncModeChanged:(int)mode
{
    _syncMode = static_cast<SyncMode>(mode);
}

- (void)handleTempoChanged:(float)tempo
{
    _tempo = tempo;
}

- (void)handleTimeSigChanged:(int)numerator denominator:(int)denominator
{
    _timeSig = {numerator, denominator};
}

- (void)handleCpuTimingsAvg:(float)avg min:(float)min max:(float)max
{
    _cpuTimings = {avg, min, max};
    [self updateButtonTitle];
}

- (void)invalidateTrackCache
{
    _trackCacheDirty = true;
}

- (void)updateButtonTitle
{
    if (_cpuTimings.avg > 0.01f)
    {
        _statusItem.button.title = [NSString stringWithFormat:@" %.0f%%", _cpuTimings.avg];
    }
    else
    {
        _statusItem.button.title = @"";
    }
}

// ---------------------------------------------------------------------------
// NSMenuDelegate — lazy population
// ---------------------------------------------------------------------------

- (void)menuNeedsUpdate:(NSMenu*)menu
{
    [menu removeAllItems];

    // -- Header info --
    auto* header = [[NSMenuItem alloc] initWithTitle:_audioInfoString action:nil keyEquivalent:@""];
    header.enabled = NO;
    [menu addItem:header];
    [menu addItem:[NSMenuItem separatorItem]];

    // -- Transport section --
    auto* playStop = [[NSMenuItem alloc] initWithTitle:playing_mode_string(_playingMode)
                                                action:@selector(togglePlayStop:)
                                         keyEquivalent:@""];
    playStop.target = self;
    [menu addItem:playStop];

    auto* tempoItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Tempo: %.1f BPM", _tempo]
                                                 action:@selector(editTempo:)
                                          keyEquivalent:@""];
    tempoItem.target = self;
    [menu addItem:tempoItem];

    auto* timeSigItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Time Sig: %d/%d",
                                                           _timeSig.numerator, _timeSig.denominator]
                                                   action:nil
                                            keyEquivalent:@""];
    timeSigItem.enabled = NO;
    [menu addItem:timeSigItem];

    // Sync mode submenu
    auto* syncItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Sync: %@",
                                                        sync_mode_string(_syncMode)]
                                                action:nil
                                         keyEquivalent:@""];
    auto* syncSubmenu = [[NSMenu alloc] init];
    SyncMode modes[] = {SyncMode::INTERNAL, SyncMode::MIDI, SyncMode::GATE, SyncMode::LINK};
    for (auto mode : modes)
    {
        auto* item = [[NSMenuItem alloc] initWithTitle:sync_mode_string(mode)
                                                action:@selector(setSyncMode:)
                                         keyEquivalent:@""];
        item.target = self;
        item.tag = static_cast<NSInteger>(mode);
        if (mode == _syncMode)
        {
            item.state = NSControlStateValueOn;
        }
        [syncSubmenu addItem:item];
    }
    syncItem.submenu = syncSubmenu;
    [menu addItem:syncItem];

    // CPU line
    auto* cpuItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"CPU: avg %.1f%%  max %.1f%%",
                                                       _cpuTimings.avg, _cpuTimings.max]
                                               action:nil
                                        keyEquivalent:@""];
    cpuItem.enabled = NO;
    [menu addItem:cpuItem];

    [menu addItem:[NSMenuItem separatorItem]];

    // -- Tracks section --
    [self buildTrackMenuItems:menu];

    [menu addItem:[NSMenuItem separatorItem]];

    // -- Open/Close All Editors --
    auto* openAll = [[NSMenuItem alloc] initWithTitle:@"Open All Editors"
                                               action:@selector(openAllEditors:)
                                        keyEquivalent:@""];
    openAll.target = self;
    [menu addItem:openAll];

    auto* closeAll = [[NSMenuItem alloc] initWithTitle:@"Close All Editors"
                                                action:@selector(closeAllEditors:)
                                         keyEquivalent:@""];
    closeAll.target = self;
    [menu addItem:closeAll];

    [menu addItem:[NSMenuItem separatorItem]];

    // -- Quit --
    auto* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit Sushi"
                                                action:@selector(terminate:)
                                         keyEquivalent:@"q"];
    [menu addItem:quitItem];
}

// ---------------------------------------------------------------------------
// Track/processor submenu
// ---------------------------------------------------------------------------

- (void)buildTrackMenuItems:(NSMenu*)menu
{
    auto* graph = _controller->audio_graph_controller();
    auto* editor = _controller->editor_controller();
    auto tracks = graph->get_all_tracks();

    if (tracks.empty())
    {
        auto* noTracks = [[NSMenuItem alloc] initWithTitle:@"No tracks" action:nil keyEquivalent:@""];
        noTracks.enabled = NO;
        [menu addItem:noTracks];
        return;
    }

    auto* tracksHeader = [[NSMenuItem alloc] initWithTitle:@"Tracks" action:nil keyEquivalent:@""];
    tracksHeader.enabled = NO;
    [menu addItem:tracksHeader];

    for (const auto& track : tracks)
    {
        const auto& track_display_name = track.label.empty() ? track.name : track.label;
        auto* trackItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"  %s (%dch)",
                                                             track_display_name.c_str(), track.channels]
                                                     action:nil
                                              keyEquivalent:@""];
        trackItem.enabled = NO;
        [menu addItem:trackItem];

        auto [status, processors] = graph->get_track_processors(track.id);
        if (status != ControlStatus::OK)
        {
            continue;
        }

        for (const auto& proc : processors)
        {
            auto [has_ed_status, has_ed] = editor->has_editor(proc.id);
            auto [is_open_status, is_open] = editor->is_editor_open(proc.id);
            const auto& proc_display_name = proc.label.empty() ? proc.name : proc.label;

            NSString* title = [NSString stringWithFormat:@"    %s", proc_display_name.c_str()];

            auto* procItem = [[NSMenuItem alloc] initWithTitle:title
                                                        action:nil
                                                 keyEquivalent:@""];

            if (has_ed_status == ControlStatus::OK && has_ed)
            {
                procItem.action = @selector(toggleEditor:);
                procItem.target = self;
                procItem.tag = kProcessorTagBase + proc.id;
                if (is_open_status == ControlStatus::OK && is_open)
                {
                    procItem.state = NSControlStateValueOn;
                }
            }
            else
            {
                procItem.enabled = NO;
            }

            [menu addItem:procItem];
        }
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

- (void)statusBarClicked:(id)sender
{
    NSEvent* event = [NSApp currentEvent];
    if (event.type == NSEventTypeRightMouseUp ||
        (event.modifierFlags & NSEventModifierFlagControl))
    {
        [_statusItem popUpStatusItemMenu:_menu];
    }
    else
    {
        [self toggleAllEditors:sender];
    }
}

- (void)toggleAllEditors:(id)sender
{
    (void)sender;

    SushiControl* controller = _controller;
    std::set<int> ids_to_restore = _openEditorIds;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto* graph = controller->audio_graph_controller();
        auto* editor = controller->editor_controller();

        // First pass: check if any editor is currently open
        bool any_open = false;
        for (const auto& track : graph->get_all_tracks())
        {
            auto [status, processors] = graph->get_track_processors(track.id);
            if (status != ControlStatus::OK)
            {
                continue;
            }
            for (const auto& proc : processors)
            {
                auto [open_status, is_open] = editor->is_editor_open(proc.id);
                if (open_status == ControlStatus::OK && is_open)
                {
                    any_open = true;
                    break;
                }
            }
            if (any_open)
            {
                break;
            }
        }

        if (any_open)
        {
            // Close all currently open editors
            for (const auto& track : graph->get_all_tracks())
            {
                auto [status, processors] = graph->get_track_processors(track.id);
                if (status != ControlStatus::OK)
                {
                    continue;
                }
                for (const auto& proc : processors)
                {
                    auto [open_status, is_open] = editor->is_editor_open(proc.id);
                    if (open_status == ControlStatus::OK && is_open)
                    {
                        editor->close_editor(proc.id);
                    }
                }
            }
        }
        else
        {
            // Reopen only editors that were previously user-opened
            for (int proc_id : ids_to_restore)
            {
                editor->open_editor(proc_id);
            }
        }
    });
}

- (void)togglePlayStop:(id)sender
{
    (void)sender;
    auto* transport = _controller->transport_controller();
    if (_playingMode == PlayingMode::PLAYING)
    {
        transport->set_playing_mode(PlayingMode::STOPPED);
    }
    else
    {
        transport->set_playing_mode(PlayingMode::PLAYING);
    }
}

- (void)editTempo:(id)sender
{
    (void)sender;

    if (_tempoPanel)
    {
        [_tempoPanel makeKeyAndOrderFront:nil];
        return;
    }

    _tempoPanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 240, 80)
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    [_tempoPanel setTitle:@"Set Tempo"];
    [_tempoPanel center];
    [_tempoPanel setReleasedWhenClosed:NO];

    _tempoField = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 40, 200, 24)];
    _tempoField.stringValue = [NSString stringWithFormat:@"%.1f", _tempo];
    [[_tempoPanel contentView] addSubview:_tempoField];

    auto* button = [[NSButton alloc] initWithFrame:NSMakeRect(80, 8, 80, 28)];
    button.title = @"Set";
    button.bezelStyle = NSBezelStyleRounded;
    button.target = self;
    button.action = @selector(tempoButtonClicked:);
    [[_tempoPanel contentView] addSubview:button];

    [_tempoPanel setLevel:NSFloatingWindowLevel];
    [_tempoPanel makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)tempoButtonClicked:(id)sender
{
    (void)sender;
    float newTempo = [_tempoField floatValue];
    if (newTempo >= 20.0f && newTempo <= 999.0f)
    {
        _controller->transport_controller()->set_tempo(newTempo);
    }
    [_tempoPanel close];
    _tempoPanel = nil;
    _tempoField = nil;
}

- (void)setSyncMode:(NSMenuItem*)sender
{
    auto mode = static_cast<SyncMode>(sender.tag);
    _controller->transport_controller()->set_sync_mode(mode);
}

- (void)toggleEditor:(NSMenuItem*)sender
{
    int processor_id = static_cast<int>(sender.tag - kProcessorTagBase);
    auto* editor = _controller->editor_controller();

    auto [status, is_open] = editor->is_editor_open(processor_id);
    if (status != ControlStatus::OK)
    {
        return;
    }

    // EditorController uses dispatch_sync(main_queue) internally on macOS,
    // so we must call from a background thread to avoid deadlocking.
    if (is_open)
    {
        _openEditorIds.erase(processor_id);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            editor->close_editor(processor_id);
        });
    }
    else
    {
        _openEditorIds.insert(processor_id);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            editor->open_editor(processor_id);
        });
    }
}

- (void)openAllEditors:(id)sender
{
    (void)sender;

    // Collect all editor-capable processor IDs on main thread, then open on background
    auto* graph = _controller->audio_graph_controller();
    auto* editor = _controller->editor_controller();
    for (const auto& track : graph->get_all_tracks())
    {
        auto [status, processors] = graph->get_track_processors(track.id);
        if (status != ControlStatus::OK)
        {
            continue;
        }
        for (const auto& proc : processors)
        {
            auto [has_status, has_ed] = editor->has_editor(proc.id);
            if (has_status == ControlStatus::OK && has_ed)
            {
                _openEditorIds.insert(proc.id);
            }
        }
    }

    std::set<int> ids_to_open = _openEditorIds;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (int proc_id : ids_to_open)
        {
            auto [open_status, is_open] = editor->is_editor_open(proc_id);
            if (open_status == ControlStatus::OK && !is_open)
            {
                editor->open_editor(proc_id);
            }
        }
    });
}

- (void)closeAllEditors:(id)sender
{
    (void)sender;

    _openEditorIds.clear();

    SushiControl* controller = _controller;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        auto* graph = controller->audio_graph_controller();
        auto* editor = controller->editor_controller();
        for (const auto& track : graph->get_all_tracks())
        {
            auto [status, processors] = graph->get_track_processors(track.id);
            if (status != ControlStatus::OK)
            {
                continue;
            }
            for (const auto& proc : processors)
            {
                auto [open_status, is_open] = editor->is_editor_open(proc.id);
                if (open_status == ControlStatus::OK && is_open)
                {
                    editor->close_editor(proc.id);
                }
            }
        }
    });
}

@end
