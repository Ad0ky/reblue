/**
 * @file    gpu/pass_timer.h
 * @brief   GPU time per render pass from timestamp queries (Switch).
 *
 * One timestamp at the start of each frame's command list, one at every
 * render-target change (BindDrawFramebufferLocked), one before and one after
 * the present pass. Once the frame's fence has signalled, the gaps between them
 * are the GPU time of each pass, keyed by its targets (size and format). Every
 * 10 s the log gets a [gpu-passes] block: GPU time per frame and the passes that
 * cost the most. Off the Switch every call is a no-op.
 */
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderCommandList;
struct RenderDevice;
} // namespace plume

namespace bd::gpu {

struct GuestTexture;

namespace pass_timer {

#if defined(__SWITCH__)
void BeginFrame(plume::RenderDevice *device, plume::RenderCommandList *list,
                u32 slot);
void MarkTargets(plume::RenderCommandList *list, u32 slot,
                 const GuestTexture *rt, const GuestTexture *ds);
void MarkPresent(plume::RenderCommandList *list, u32 slot);
void EndFrame(plume::RenderCommandList *list, u32 slot);
// After the slot's fence has signalled.
void Collect(u32 slot);
#else
inline void BeginFrame(plume::RenderDevice *, plume::RenderCommandList *, u32) {}
inline void MarkTargets(plume::RenderCommandList *, u32, const GuestTexture *,
                        const GuestTexture *) {}
inline void MarkPresent(plume::RenderCommandList *, u32) {}
inline void EndFrame(plume::RenderCommandList *, u32) {}
inline void Collect(u32) {}
#endif

} // namespace pass_timer
} // namespace bd::gpu
