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
 * @brief macOS status bar icon for Sushi GUI.
 * @copyright 2017-2023 Elk Audio AB, Stockholm
 */

#ifndef SUSHI_STATUS_BAR_H
#define SUSHI_STATUS_BAR_H

#import <Cocoa/Cocoa.h>

namespace sushi::control {
class SushiControl;
}

@interface SushiStatusBar : NSObject

- (instancetype)initWithController:(sushi::control::SushiControl*)controller;
- (void)teardown;

// Called from notification bridge on main thread
- (void)handlePlayingModeChanged:(int)mode;
- (void)handleSyncModeChanged:(int)mode;
- (void)handleTempoChanged:(float)tempo;
- (void)handleTimeSigChanged:(int)numerator denominator:(int)denominator;
- (void)handleCpuTimingsAvg:(float)avg min:(float)min max:(float)max;
- (void)invalidateTrackCache;

@end

#endif // SUSHI_STATUS_BAR_H
