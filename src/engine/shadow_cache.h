/**
 * @file    engine/shadow_cache.h
 * @brief   Sun shadow map reuse (engine/shadow_cache.cpp).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

namespace bd::engine {

// World position of the eye of the camera the sun shadow pass now being drawn
// belongs to (set when the pass opens). False before the first pass.
bool ShadowPassEye(float eye[3]);

} // namespace bd::engine
