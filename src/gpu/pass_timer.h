/**
 * @file    gpu/pass_timer.h
 * @brief   GPU time per render pass from timestamp queries (Switch).
 *
 * One timestamp at the start of each frame's command list, one at every
 * render-target change (BindDrawFramebufferLocked), one before and one after
 * the present pass. Once the frame's fence has signalled, the gaps between them
 * are the GPU time of each pass, keyed by its targets (size and format). Every
 * 10 s the log gets a [gpu-passes] block: GPU time per frame and the passes that
 * cost the most.
 *
 * Shaders: one frame in kShaderFrameEvery also gets a timestamp at every draw
 * whose shaders differ from the draw before (MarkDraw), so the GPU time between
 * two such stamps belongs to the draws of one vertex/pixel shader pair. Every
 * 10 s the log gets a [gpu-shaders] block: the pixel shaders that cost the most
 * GPU time per frame, by shader cache hash (the name of the shader in the HLSL
 * dump), with their draws per frame, main vertex shader and render target.
 * Timestamps wait for the work before them, so the numbers rank shaders rather
 * than add up to the frame exactly. Off the Switch every call is a no-op.
 */
#pragma once

#include <rex/types.h>

namespace plume {
struct RenderCommandList;
struct RenderDevice;
} // namespace plume

namespace bd::gpu {

struct GuestShader;
struct GuestTexture;

namespace pass_timer {

#if defined(__SWITCH__)
void BeginFrame(plume::RenderDevice *device, plume::RenderCommandList *list,
                u32 slot);
void MarkTargets(plume::RenderCommandList *list, u32 slot,
                 const GuestTexture *rt, const GuestTexture *ds);
void MarkPresent(plume::RenderCommandList *list, u32 slot);
// Right before a draw is recorded, with the shaders it draws with.
void MarkDraw(plume::RenderCommandList *list, u32 slot, const GuestShader *vs,
              const GuestShader *ps);
void EndFrame(plume::RenderCommandList *list, u32 slot);
// After the slot's fence has signalled.
void Collect(u32 slot);
#else
inline void BeginFrame(plume::RenderDevice *, plume::RenderCommandList *, u32) {}
inline void MarkTargets(plume::RenderCommandList *, u32, const GuestTexture *,
                        const GuestTexture *) {}
inline void MarkPresent(plume::RenderCommandList *, u32) {}
inline void MarkDraw(plume::RenderCommandList *, u32, const GuestShader *,
                     const GuestShader *) {}
inline void EndFrame(plume::RenderCommandList *, u32) {}
inline void Collect(u32) {}
#endif

} // namespace pass_timer
} // namespace bd::gpu
