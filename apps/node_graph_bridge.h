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
 * @brief Pure ObjC bridge for exposing the audio graph to Swift/SwiftUI.
 * @copyright 2017-2023 Elk Audio AB, Stockholm
 *
 * This header is intentionally pure Objective-C (no C++ includes) so that
 * Swift can import it via a bridging header.
 */

#ifndef SUSHI_NODE_GRAPH_BRIDGE_H
#define SUSHI_NODE_GRAPH_BRIDGE_H

#import <Foundation/Foundation.h>

// ---------------------------------------------------------------------------
// Data objects
// ---------------------------------------------------------------------------

@interface SushiProcessorNode : NSObject

@property (nonatomic, readonly) int processorId;
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly, copy) NSString* pluginType;
@property (nonatomic, readonly) BOOL hasEditor;

@end

@interface SushiAudioConnectionInfo : NSObject

@property (nonatomic, readonly) int trackId;
@property (nonatomic, readonly) int trackChannel;
@property (nonatomic, readonly) int engineChannel;

@end

@interface SushiMidiKbdConnectionInfo : NSObject

@property (nonatomic, readonly) int trackId;
@property (nonatomic, readonly) int channel;  // 0-15 = CH1-CH16, 16 = OMNI
@property (nonatomic, readonly) int port;
@property (nonatomic, readonly) BOOL rawMidi;

@end

@interface SushiTrackNode : NSObject

@property (nonatomic, readonly) int trackId;
@property (nonatomic, readonly, copy) NSString* name;
@property (nonatomic, readonly) int channels;
@property (nonatomic, readonly, copy) NSArray<SushiProcessorNode*>* processors;

@end

// ---------------------------------------------------------------------------
// Protocols
// ---------------------------------------------------------------------------

@protocol SushiGraphDataSource <NSObject>
- (NSArray<SushiTrackNode*>*)allTracks;
@end

@protocol SushiGraphChangeListener <NSObject>
- (void)graphDidChange;
@end

// ---------------------------------------------------------------------------
// Bridge class
// ---------------------------------------------------------------------------

@interface SushiGraphBridge : NSObject <SushiGraphDataSource>

- (NSArray<SushiTrackNode*>*)allTracks;
- (NSArray<SushiAudioConnectionInfo*>*)allAudioInputConnections;
- (NSArray<SushiAudioConnectionInfo*>*)allAudioOutputConnections;
- (NSArray<SushiMidiKbdConnectionInfo*>*)allMidiKbdInputConnections;
- (BOOL)hasEditorForProcessor:(int)processorId;
- (BOOL)isEditorOpenForProcessor:(int)processorId;
- (void)toggleEditorForProcessor:(int)processorId;
- (void)addListener:(id<SushiGraphChangeListener>)listener;
- (void)removeListener:(id<SushiGraphChangeListener>)listener;
- (void)shutdown;

@end

// C++-only initializer — not visible to Swift
#ifdef __cplusplus
namespace sushi::control { class SushiControl; }
@interface SushiGraphBridge (CppInit)
- (instancetype)initWithController:(sushi::control::SushiControl*)controller;
@end
#endif

#endif // SUSHI_NODE_GRAPH_BRIDGE_H
