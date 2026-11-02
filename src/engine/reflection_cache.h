/**
 * @file    engine/reflection_cache.h
 * @brief   Plane reflection (water) redrawn every Nth frame, small objects left
 *          out of it (engine/reflection_cache.cpp).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <rex/types.h>

namespace bd::engine {

// Called by the bdBuildMirrorViewProjection hook before it does anything, with
// the plane reflection info (r3) and the eye (r4, camera +288). True: this
// frame reuses the reflection, so its mirror matrices must stay as they were.
bool ReflectionReuseThisFrame(u32 info, u32 eyeVa);

// A plane reflection pass is being drawn now (between its opening and its
// closing), with this many pixels of target height. 0 outside one.
u32 ReflectionPassHeight();

// The eye of the camera the reflection pass belongs to.
bool ReflectionPassEye(float eye[3]);

// Counts an object the reflection pass left out as too small (for the log).
void CountReflectionObject(bool skipped);

} // namespace bd::engine
