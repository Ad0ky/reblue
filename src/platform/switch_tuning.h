/**
 * @file    platform/switch_tuning.h
 * @brief   Nintendo Switch performance settings: the handheld GPU clock
 *          profile and the scene resolution in handheld mode. No-ops on the
 *          other platforms.
 * @license BSD 3-Clause, see LICENSE
 */
#pragma once

#include <rex/types.h>

namespace bd::platform {

#if defined(__SWITCH__)
// Asks the system for the handheld GPU profile (bd_switch_gpu_mhz) and points
// plume at the pipeline cache file. Once, after the config is loaded and before
// the GPU device is created.
void ApplySwitchClocks();

// The size the game renders at when the console is in handheld mode and
// bd_switch_handheld_height is set (540 by default): false docked, or when it is
// 0. The swapchain stays at the display size and the frame is scaled to it.
bool SwitchHandheldRenderSize(u32 &w, u32 &h);

// Dynamic resolution (bd_switch_dynamic_resolution, handheld only): fed each
// frame's measured GPU time; true when the handheld render size it wants
// changed, for the caller to apply (gpu::Output::Recompute, then the game's
// render targets are rebuilt at the next frame submit).
bool SwitchDynamicResolutionFrame(double gpu_ms);
// Whether the present pass upscales with FSR 1 (bd_switch_fsr) for an image of
// src_w x src_h shown at dst_w x dst_h.
bool SwitchFsrWanted(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h);

// For a host worker thread (the movie decoder): lets the calling thread run
// on every core of the process, starting on 'core'. A thread made with
// std::thread on Horizon is otherwise confined to the core it was created on.
void SpreadCurrentThread(int core);
// Horizon priority of the calling thread (0x2C..0x3B for an application).
void SetCurrentThreadPriority(int priority);
#else
inline void ApplySwitchClocks() {}
inline bool SwitchHandheldRenderSize(u32 &, u32 &) { return false; }
inline bool SwitchDynamicResolutionFrame(double) { return false; }
inline bool SwitchFsrWanted(u32, u32, u32, u32) { return false; }
inline void SpreadCurrentThread(int) {}
inline void SetCurrentThreadPriority(int) {}
#endif

} // namespace bd::platform
