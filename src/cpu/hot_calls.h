/**
 * @file    cpu/hot_calls.h
 * @brief   How a hook calls the original of a function the Switch build also
 *          has a localized copy of (src/platform/switch/hot/localized.txt).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

#include <rex/ppc.h>

// A hook whose function is listed "hooked" in localized.txt calls
// REBLUE_HOT_CALL(name)(ctx, base) where it called __imp__name: on the Switch
// that is the localized copy (or the original with bd_cpu_natives off),
// elsewhere the original as before.
#if defined(__SWITCH__)
// Every function listed "hooked" in localized.txt, at namespace scope here
// (hooks are often inside an anonymous namespace). localize.py writes their
// definitions: the copy or, if it left the function alone, the original.
#define REBLUE_HOT_HOOKED(X)                                                   \
  X(bdSceneNodeDrawSingle)                                                     \
  X(bdBuildViewMatrix)                                                         \
  X(bdDoubleBufferAcquire)                                                     \
  X(bdCameraRender)                                                            \
  X(bdSetRenderState)                                                          \
  X(D3DDevice_SetPixelShaderConstantFN)                                        \
  X(D3DDevice_SetVertexShaderConstantFN)
namespace bd::hot {
#define REBLUE_HOT_DECLARE_ONE(name)                                           \
  void Call_##name(PPCContext &__restrict ctx, uint8_t *base);
REBLUE_HOT_HOOKED(REBLUE_HOT_DECLARE_ONE)
#undef REBLUE_HOT_DECLARE_ONE
} // namespace bd::hot
#define REBLUE_HOT_DECLARE(name)
#define REBLUE_HOT_CALL(name) ::bd::hot::Call_##name
#else
#define REBLUE_HOT_DECLARE(name) REX_EXTERN(__imp__##name);
#define REBLUE_HOT_CALL(name) __imp__##name
#endif
