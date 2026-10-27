/**
 * @file    engine/frame_clock.h
 * @brief   Fixed 30Hz logic tick clock for the fps unlock.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::engine {

void Advance();

bool InterpolationActive();

// The frame rate cannot keep up with the 30Hz simulation (bd_interp_min_fps):
// interpolation is off and the frame limiter holds 30 fps, as during a movie.
bool InterpolationThrottled();
// Time the frame limiter slept, so the throttle measures the frame's work.
void AddPacingSleep(double seconds);

bool TickDue();

float Alpha();

u64 TickCount();

double TicksPerSecond();

double FrameTime();

double FrameDelta();

void PublishRenderClock();
void BindRenderThread();
bool IsRenderThread();

} // namespace bd::engine
