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
 * @brief ObjC++ implementation of the audio graph bridge.
 * @Copyright 2026 Oliver Larkin
 */

#include <atomic>

#import <Foundation/Foundation.h>

#include "sushi/control_interface.h"
#include "sushi/control_notifications.h"
#include "node_graph_bridge.h"

using namespace sushi::control;

// ---------------------------------------------------------------------------
// Data object implementations
// ---------------------------------------------------------------------------

@implementation SushiProcessorNode
{
    int _processorId;
    NSString* _name;
    NSString* _pluginType;
    BOOL _hasEditor;
}

- (instancetype)initWithId:(int)processorId
                      name:(NSString*)name
                pluginType:(NSString*)pluginType
                 hasEditor:(BOOL)hasEditor
{
    self = [super init];
    if (self)
    {
        _processorId = processorId;
        _name = [name copy];
        _pluginType = [pluginType copy];
        _hasEditor = hasEditor;
    }
    return self;
}

- (int)processorId { return _processorId; }
- (NSString*)name { return _name; }
- (NSString*)pluginType { return _pluginType; }
- (BOOL)hasEditor { return _hasEditor; }

@end

@implementation SushiAudioConnectionInfo
{
    int _trackId;
    int _trackChannel;
    int _engineChannel;
}

- (instancetype)initWithTrackId:(int)trackId
                   trackChannel:(int)trackChannel
                  engineChannel:(int)engineChannel
{
    self = [super init];
    if (self)
    {
        _trackId = trackId;
        _trackChannel = trackChannel;
        _engineChannel = engineChannel;
    }
    return self;
}

- (int)trackId { return _trackId; }
- (int)trackChannel { return _trackChannel; }
- (int)engineChannel { return _engineChannel; }

@end

@implementation SushiMidiKbdConnectionInfo
{
    int _trackId;
    int _channel;
    int _port;
    BOOL _rawMidi;
}

- (instancetype)initWithTrackId:(int)trackId
                        channel:(int)channel
                           port:(int)port
                        rawMidi:(BOOL)rawMidi
{
    self = [super init];
    if (self)
    {
        _trackId = trackId;
        _channel = channel;
        _port = port;
        _rawMidi = rawMidi;
    }
    return self;
}

- (int)trackId { return _trackId; }
- (int)channel { return _channel; }
- (int)port { return _port; }
- (BOOL)rawMidi { return _rawMidi; }

@end

@implementation SushiTrackNode
{
    int _trackId;
    NSString* _name;
    int _channels;
    NSArray<SushiProcessorNode*>* _processors;
    float _gain;
    float _pan;
    BOOL _hasPan;
}

- (instancetype)initWithId:(int)trackId
                      name:(NSString*)name
                  channels:(int)channels
                processors:(NSArray<SushiProcessorNode*>*)processors
                      gain:(float)gain
                       pan:(float)pan
                    hasPan:(BOOL)hasPan
{
    self = [super init];
    if (self)
    {
        _trackId = trackId;
        _name = [name copy];
        _channels = channels;
        _processors = [processors copy];
        _gain = gain;
        _pan = pan;
        _hasPan = hasPan;
    }
    return self;
}

- (int)trackId { return _trackId; }
- (NSString*)name { return _name; }
- (int)channels { return _channels; }
- (NSArray<SushiProcessorNode*>*)processors { return _processors; }
- (float)gain { return _gain; }
- (float)pan { return _pan; }
- (BOOL)hasPan { return _hasPan; }

@end

// ---------------------------------------------------------------------------
// C++ notification listener
// ---------------------------------------------------------------------------

static NSString* plugin_type_string(PluginType type)
{
    switch (type)
    {
        case PluginType::INTERNAL: return @"INT";
        case PluginType::VST2X:    return @"VST2";
        case PluginType::VST3X:    return @"VST3";
        case PluginType::LV2:      return @"LV2";
        case PluginType::CLAP:     return @"CLAP";
        case PluginType::AUV2:     return @"AUV2";
        case PluginType::CMAJOR:   return @"CMAJ";
    }
    return @"";
}

// Forward-declare so GraphBridgeListener can call it
@interface SushiGraphBridge (Notification)
- (void)notifyListeners;
@end

class GraphBridgeListener final : public ControlListener
{
public:
    GraphBridgeListener(SushiGraphBridge* bridge) : _bridge(bridge) {}

    void shutdown() { _shutdown.store(true, std::memory_order_release); }

    void notification(const ControlNotification* notification) override
    {
        if (_shutdown.load(std::memory_order_acquire))
        {
            return;
        }

        switch (notification->type())
        {
            case NotificationType::TRACK_UPDATE:
            case NotificationType::PROCESSOR_UPDATE:
            {
                SushiGraphBridge* bridge = _bridge;
                dispatch_async(dispatch_get_main_queue(), ^{
                    [bridge notifyListeners];
                });
                break;
            }
            default:
                break;
        }
    }

private:
    SushiGraphBridge* _bridge;
    std::atomic<bool> _shutdown{false};
};

// ---------------------------------------------------------------------------
// SushiGraphBridge
// ---------------------------------------------------------------------------

@interface SushiGraphBridge ()
{
    SushiControl* _controller;
    std::unique_ptr<GraphBridgeListener> _listener;
    NSHashTable<id<SushiGraphChangeListener>>* _changeListeners;
}

- (void)notifyListeners;

@end

@implementation SushiGraphBridge

- (instancetype)initWithController:(SushiControl*)controller
{
    self = [super init];
    if (self)
    {
        _controller = controller;
        _changeListeners = [NSHashTable weakObjectsHashTable];

        _listener = std::make_unique<GraphBridgeListener>(self);
        _controller->subscribe_to_notifications(NotificationType::TRACK_UPDATE, _listener.get());
        _controller->subscribe_to_notifications(NotificationType::PROCESSOR_UPDATE, _listener.get());
    }
    return self;
}

- (NSArray<SushiTrackNode*>*)allTracks
{
    if (!_controller)
    {
        return @[];
    }

    auto* graph = _controller->audio_graph_controller();
    auto* params = _controller->parameter_controller();
    auto tracks = graph->get_all_tracks();

    NSMutableArray<SushiTrackNode*>* result = [NSMutableArray arrayWithCapacity:tracks.size()];
    for (const auto& track : tracks)
    {
        const auto& display_name = track.label.empty() ? track.name : track.label;

        auto [status, processors] = graph->get_track_processors(track.id);
        NSMutableArray<SushiProcessorNode*>* procNodes = [NSMutableArray array];
        if (status == ControlStatus::OK)
        {
            for (const auto& proc : processors)
            {
                const auto& proc_display_name = proc.label.empty() ? proc.name : proc.label;
                auto [has_ed_status, has_ed] = _controller->editor_controller()->has_editor(proc.id);
                BOOL hasEditor = (has_ed_status == ControlStatus::OK && has_ed);
                auto* node = [[SushiProcessorNode alloc]
                    initWithId:proc.id
                          name:[NSString stringWithUTF8String:proc_display_name.c_str()]
                    pluginType:plugin_type_string(proc.type)
                     hasEditor:hasEditor];
                [procNodes addObject:node];
            }
        }

        // Fetch gain and pan values
        float gainDb = 0.0f;
        float panVal = 0.0f;
        BOOL hasPan = NO;
        auto [tp_status, track_params] = params->get_track_parameters(track.id);
        if (tp_status == ControlStatus::OK)
        {
            for (const auto& p : track_params)
            {
                if (p.name == "gain")
                {
                    auto [gs, gv] = params->get_parameter_value_in_domain(track.id, p.id);
                    if (gs == ControlStatus::OK) gainDb = gv;
                }
                else if (p.name == "pan")
                {
                    auto [ps, pv] = params->get_parameter_value_in_domain(track.id, p.id);
                    if (ps == ControlStatus::OK) panVal = pv;
                    hasPan = YES;
                }
            }
        }

        auto* trackNode = [[SushiTrackNode alloc]
            initWithId:track.id
                  name:[NSString stringWithUTF8String:display_name.c_str()]
              channels:track.channels
            processors:procNodes
                  gain:gainDb
                   pan:panVal
                hasPan:hasPan];
        [result addObject:trackNode];
    }

    return result;
}

- (NSArray<SushiAudioConnectionInfo*>*)allAudioInputConnections
{
    if (!_controller) return @[];

    auto conns = _controller->audio_routing_controller()->get_all_input_connections();
    NSMutableArray* result = [NSMutableArray arrayWithCapacity:conns.size()];
    for (const auto& c : conns)
    {
        [result addObject:[[SushiAudioConnectionInfo alloc]
            initWithTrackId:c.track_id
               trackChannel:c.track_channel
              engineChannel:c.engine_channel]];
    }
    return result;
}

- (NSArray<SushiAudioConnectionInfo*>*)allAudioOutputConnections
{
    if (!_controller) return @[];

    auto conns = _controller->audio_routing_controller()->get_all_output_connections();
    NSMutableArray* result = [NSMutableArray arrayWithCapacity:conns.size()];
    for (const auto& c : conns)
    {
        [result addObject:[[SushiAudioConnectionInfo alloc]
            initWithTrackId:c.track_id
               trackChannel:c.track_channel
              engineChannel:c.engine_channel]];
    }
    return result;
}

- (NSArray<SushiMidiKbdConnectionInfo*>*)allMidiKbdInputConnections
{
    if (!_controller) return @[];

    auto conns = _controller->midi_controller()->get_all_kbd_input_connections();
    NSMutableArray* result = [NSMutableArray arrayWithCapacity:conns.size()];
    for (const auto& c : conns)
    {
        [result addObject:[[SushiMidiKbdConnectionInfo alloc]
            initWithTrackId:c.track_id
                    channel:static_cast<int>(c.channel)
                       port:c.port
                    rawMidi:c.raw_midi ? YES : NO]];
    }
    return result;
}

- (BOOL)hasEditorForProcessor:(int)processorId
{
    if (!_controller) return NO;
    auto [status, has_ed] = _controller->editor_controller()->has_editor(processorId);
    return status == ControlStatus::OK && has_ed;
}

- (BOOL)isEditorOpenForProcessor:(int)processorId
{
    if (!_controller) return NO;
    auto [status, is_open] = _controller->editor_controller()->is_editor_open(processorId);
    return status == ControlStatus::OK && is_open;
}

- (void)toggleEditorForProcessor:(int)processorId
{
    if (!_controller) return;
    auto* editor = _controller->editor_controller();
    auto [status, is_open] = editor->is_editor_open(processorId);
    if (status != ControlStatus::OK) return;

    // EditorController uses dispatch_sync(main_queue) internally on macOS,
    // so we must call from a background thread to avoid deadlocking.
    if (is_open)
    {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            editor->close_editor(processorId);
        });
    }
    else
    {
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            editor->open_editor(processorId);
        });
    }
}

- (void)setGain:(float)gainDb forTrack:(int)trackId
{
    if (!_controller) return;
    auto* params = _controller->parameter_controller();
    auto [status, track_params] = params->get_track_parameters(trackId);
    if (status != ControlStatus::OK) return;

    for (const auto& p : track_params)
    {
        if (p.name == "gain")
        {
            // Normalize dB to 0..1 range: -120..24
            float normalized = (gainDb - p.min_domain_value) / (p.max_domain_value - p.min_domain_value);
            normalized = std::max(0.0f, std::min(1.0f, normalized));
            params->set_parameter_value(trackId, p.id, normalized);
            break;
        }
    }
}

- (void)setPan:(float)pan forTrack:(int)trackId
{
    if (!_controller) return;
    auto* params = _controller->parameter_controller();
    auto [status, track_params] = params->get_track_parameters(trackId);
    if (status != ControlStatus::OK) return;

    for (const auto& p : track_params)
    {
        if (p.name == "pan")
        {
            // Normalize -1..1 to 0..1
            float normalized = (pan - p.min_domain_value) / (p.max_domain_value - p.min_domain_value);
            normalized = std::max(0.0f, std::min(1.0f, normalized));
            params->set_parameter_value(trackId, p.id, normalized);
            break;
        }
    }
}

- (void)addListener:(id<SushiGraphChangeListener>)listener
{
    [_changeListeners addObject:listener];
}

- (void)removeListener:(id<SushiGraphChangeListener>)listener
{
    [_changeListeners removeObject:listener];
}

- (void)notifyListeners
{
    for (id<SushiGraphChangeListener> listener in _changeListeners)
    {
        [listener graphDidChange];
    }
}

- (void)shutdown
{
    if (_listener)
    {
        _listener->shutdown();
    }
    _listener.reset();
    _controller = nullptr;
}

@end
