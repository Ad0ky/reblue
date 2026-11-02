/**
 * @file    platform/switch/pgo_stream.h
 * @brief   Branch counts of the instrumented Switch build to the SD card
 *          (platform/switch/pgo_stream.cpp). No-ops in every other build.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

namespace bd::platform {

// Picks this session's file and starts writing every minute.
void PgoStart();
// Writes the counts now.
void PgoWriteNow();
// Stops the writer and writes the counts a last time.
void PgoStop();

} // namespace bd::platform
