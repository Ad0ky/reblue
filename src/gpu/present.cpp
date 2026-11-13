/**
 * @file    gpu/present.cpp
 * @brief   End of frame: the deferred clear, the swapchain
 * acquire/blit/present, and the pre-Runtime overlay-only present the installer
 * uses.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/frame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <plume_render_interface.h>
#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

#include "gpu/gpu_profiling.h"

#include "core/logging.h"
#include "engine/engine.h"
#include "gpu/backend.h"
#include "engine/frame_clock.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "platform/switch_tuning.h"
#include "gpu/output.h"
#include "gpu/pass_timer.h"
#include "gpu/render_queue.h"
#include "gpu/settings.h"

#if defined(__SWITCH__)
// The SDK's Switch profiler counts presented frames through this (its
// "juego N fps" column and the console overlays). re:Blue has its own
// renderer, so it reports them itself.
extern "C" void RexSwitchPerfCount(unsigned id);
// The SDK's positioned reads (switch_libc_supplement.c), whole process.
extern "C" unsigned long long rex_sd_reads, rex_sd_read_bytes, rex_sd_read_ticks;
#endif

namespace bd::gpu {

namespace {

#if defined(__SWITCH__)
// Frame timing on the console, logged every 10 s: where each frame's time goes
// between two presents. "GPU wait" is the wait for the fence of the frame slot
// being reused (the GPU still busy with an older frame), "pacing" the frame
// limiter, "acquire+present" the swapchain calls, and the rest is CPU work on
// the game and render threads before the present.
class FrameStats {
public:
  using Clock = std::chrono::steady_clock;

  void Present(double acquire_ms, double present_ms, double fence_ms, double pace_ms) {
    const Clock::time_point now = Clock::now();
    if (started_) {
      const double frame_ms = Ms(last_, now);
      NoteFrame(frame_ms, fence_ms, pace_ms);
      ++frames_;
      total_ms_ += frame_ms;
      worst_ms_ = std::max(worst_ms_, frame_ms);
      if (frame_ms > 34.0) ++over_33_;
      if (frame_ms > 50.0) ++over_50_;
      if (frame_ms > 100.0) ++over_100_;
      acquire_ms_ += acquire_ms;
      present_ms_ += present_ms;
      fence_ms_ += fence_ms;
      pace_ms_ += pace_ms;
    } else {
      started_ = true;
      window_ = now;
    }
    last_ = now;
    const double window_ms = Ms(window_, now);
    if (window_ms >= 10000.0 && frames_ > 0) {
      const double n = double(frames_);
      const double cpu =
          std::max(0.0, (total_ms_ - acquire_ms_ - present_ms_ - fence_ms_ - pace_ms_) / n);
      BD_INFO("[frames] {:.1f} s: {:.1f} fps, frame {:.1f} ms (worst {:.0f}) | per frame: GPU wait "
              "{:.1f} ms, acquire+present {:.1f} ms, pacing {:.1f} ms, CPU {:.1f} ms | frames over "
              "33 ms {}, 50 ms {}, 100 ms {}",
              window_ms / 1000.0, n * 1000.0 / window_ms, total_ms_ / n, worst_ms_, fence_ms_ / n,
              (acquire_ms_ + present_ms_) / n, pace_ms_ / n, cpu, over_33_, over_50_, over_100_);
#if defined(PLUME_DEKO3D)
      if (UsingDeko3D())
        ReportDeko3D(n);
#endif
      frames_ = over_33_ = over_50_ = over_100_ = 0;
      hitches_logged_ = 0;
      total_ms_ = worst_ms_ = acquire_ms_ = present_ms_ = fence_ms_ = pace_ms_ = 0;
      window_ = now;
    }
  }

  // [hitch]: a frame over 100 ms, with what happened during it, from
  // cumulative counters taken at every present: the GPU wait, the waits for
  // the render thread, SD card reads (any thread), textures, pipelines and
  // shaders created, uploads. Those that go up together with long frames are
  // where the long frames come from. At most 20 lines per 10 s window.
  struct Snapshot {
    u64 sdReads = 0, sdBytes = 0, sdTicks = 0;
    u64 textures = 0, textureUs = 0, pipelines = 0, pipelineUs = 0;
    u64 syncUs = 0, compiles = 0, uploads = 0, draws = 0;
  };
  static Snapshot Take() {
    Snapshot n;
    n.sdReads = __atomic_load_n(&rex_sd_reads, __ATOMIC_RELAXED);
    n.sdBytes = __atomic_load_n(&rex_sd_read_bytes, __ATOMIC_RELAXED);
    n.sdTicks = __atomic_load_n(&rex_sd_read_ticks, __ATOMIC_RELAXED);
    n.textures = g_creations.textures.count.load(std::memory_order_relaxed);
    n.textureUs = g_creations.textures.us.load(std::memory_order_relaxed);
    n.pipelines = g_creations.pipelines.count.load(std::memory_order_relaxed);
    n.pipelineUs = g_creations.pipelines.us.load(std::memory_order_relaxed);
    n.syncUs = rq::SyncMicrosTotal();
#if defined(PLUME_DEKO3D)
    if (UsingDeko3D()) {
      const auto &st = plume::GetDeko3DStats();
      n.compiles = st.shaderCompiles.load();
      n.uploads = st.copyUpload.load();
      n.draws = st.draws.load();
    }
#endif
    return n;
  }
  void NoteFrame(double frame_ms, double fence_ms, double pace_ms) {
    const Snapshot n = Take();
    const Snapshot &o = snap_;
    if (frame_ms > 100.0 && hitches_logged_ < 20) {
      ++hitches_logged_;
      BD_INFO("[hitch] {:.0f} ms frame: GPU wait {:.1f} ms, pacing {:.1f} ms, waits for the render thread "
              "{:.1f} ms | SD reads {} ({:.2f} MB, {:.1f} ms) | created: {} textures ({:.1f} ms), {} pipelines "
              "({:.1f} ms) | {} uam compiles, {} uploads, {} draws",
              frame_ms, fence_ms, pace_ms, double(n.syncUs - o.syncUs) / 1e3, n.sdReads - o.sdReads,
              double(n.sdBytes - o.sdBytes) / 1048576.0, double(n.sdTicks - o.sdTicks) / 19200.0,
              n.textures - o.textures, double(n.textureUs - o.textureUs) / 1e3, n.pipelines - o.pipelines,
              double(n.pipelineUs - o.pipelineUs) / 1e3, n.compiles - o.compiles, n.uploads - o.uploads,
              n.draws - o.draws);
    }
    snap_ = n;
  }
  Snapshot snap_ = Take();
  u32 hitches_logged_ = 0;

#if defined(PLUME_DEKO3D)
  // The deko3d back end's counters over the window, per frame: what the GPU is asked to wait for and copy.
  void ReportDeko3D(double frames) {
    const auto &st = plume::GetDeko3DStats();
    const u64 now[] = {st.draws.load(), st.pipelineBinds.load(), st.barriersPrimitives.load(),
                       st.barriersFragments.load(), st.barriersFull.load(), st.barriersImageInvalidate.load(),
                       st.copyBuffer.load(), st.copyUpload.load(), st.copyImage.load(), st.copyReadback.load(),
                       st.shaderCompiles.load(), st.shaderCacheHits.load(), st.descriptorWrites.load(),
                       st.descriptorSame.load(), st.descriptorInvalidates.load()};
    double d[std::size(now)];
    for (size_t i = 0; i < std::size(now); ++i) {
      d[i] = double(now[i] - dk_last_[i]);
      dk_last_[i] = now[i];
    }
    BD_INFO("[deko3d] per frame: {:.0f} draws, {:.0f} pipeline binds | barriers: {:.1f} primitives, {:.1f} "
            "fragments, {:.1f} full, {:.1f} texture cache drops | copies: {:.1f} buffer, {:.1f} uploads, {:.1f} "
            "image, {:.1f} readbacks | in the window: {:.0f} uam compiles, {:.0f} DKSH cache hits",
            d[0] / frames, d[1] / frames, d[2] / frames, d[3] / frames, d[4] / frames, d[5] / frames,
            d[6] / frames, d[7] / frames, d[8] / frames, d[9] / frames, d[10], d[11]);
    BD_INFO("[deko3d] descriptors per frame: {:.1f} written, {:.1f} rewritten with the same bytes (skipped) | "
            "{:.1f} descriptor and shader cache invalidations",
            d[12] / frames, d[13] / frames, d[14] / frames);
#if REBLUE_UBO_CONSTANTS
    if (DeltaConstantsEnabled()) {
      u64 stages = 0, updates = 0, bytes = 0, regs = 0, misses = 0;
      DeltaConstantStats(stages, updates, bytes, regs, misses);
      BD_INFO("[deko3d] in-place constants per frame: {:.0f} stage checks, {:.0f} with changes, {:.0f} registers "
              "read, {:.1f} KiB sent | changes no setter announced (whole run): {}",
              double(stages) / frames, double(updates) / frames, double(regs) / frames,
              double(bytes) / frames / 1024.0, misses);
    }
#endif
    if (rq::Threaded()) {
      u64 issued = 0, syncs = 0, sync_us = 0, full = 0;
      rq::Stats(issued, syncs, sync_us, full);
      BD_INFO("[render-queue] per frame: {:.0f} records, {:.1f} waits for the render thread ({:.2f} ms) | queue "
              "full in the window: {} | waits by caller: {}",
              double(issued) / frames, double(syncs) / frames, double(sync_us) / frames / 1000.0, full,
              rq::SyncSites(frames));
    }
  }
  u64 dk_last_[15] = {};
#endif

  static double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  }

private:
  bool started_ = false;
  Clock::time_point last_{};
  Clock::time_point window_{};
  u64 frames_ = 0, over_33_ = 0, over_50_ = 0, over_100_ = 0;
  double total_ms_ = 0, worst_ms_ = 0, acquire_ms_ = 0, present_ms_ = 0, fence_ms_ = 0, pace_ms_ = 0;
};

// Only the presenting thread touches it, under the video state's mutex or right after.
FrameStats &Stats() {
  static FrameStats stats;
  return stats;
}
#endif

void ApplyVsync(VideoState &s) {
  if (s.swap_chain) {
    s.swap_chain->setVsyncEnabled(Settings::Get().Vsync());
  }
}

constexpr i32 kIdleFPS = 30;

void PaceFrame(bool idle = false) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point next{};
  i32 fps = bd::engine::Settings::Get().FPSLimit();
  if (bd::engine::SofdecPlayer::Playing())
    fps = 30;
  // Too slow to interpolate: the simulation's own rate, as with a 30 cap.
  if (bd::engine::InterpolationThrottled() && (fps <= 0 || fps > 30))
    fps = 30;
  if (idle && (fps <= 0 || fps > kIdleFPS))
    fps = kIdleFPS;
  if (fps <= 0) {
    next = {};
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(1000.0 / fps));
  const auto now = Clock::now();
  if (next.time_since_epoch().count() != 0 && now < next) {
    std::this_thread::sleep_until(next);
    bd::engine::AddPacingSleep(
        std::chrono::duration<double>(Clock::now() - now).count());
    next += period;
  } else {
    next = now + period;
  }
}

void AbandonFrame(VideoState &s, std::unique_lock<std::mutex> &lock) {
  s.frame_present_committed = true;
  if (!s.command_list_open)
    return;
  SubmitOpenListLocked(s);
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

void RebuildSwapChain(VideoState &s) {
  SubmitOpenListLocked(s);
  for (u32 i = 0; i < kNumFrames; ++i) {
    if (s.command_list_submitted[i]) {
      s.queue->waitForCommandFence(s.fences[i].get());
      s.command_list_submitted[i] = false;
    }
  }
  s.framebuffers.clear();
  if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
      !BuildPresentSemaphores(s)) {
    if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
      BD_ERROR("Swap chain resize failed");
    return;
  }
  if (static_cast<AspectMode>(Settings::Get().AspectRatio()) ==
      AspectMode::Auto)
    Output::Recompute();
}

GuestTexture *SelectPresentSource(VideoState &s, GuestTexture *frontBuffer) {
  GuestTexture *last_rt = s.last_drawn_rt;
  GuestTexture *rt = (s.back_buffer_surface && last_rt == s.back_buffer_surface)
                         ? s.back_buffer_surface
                     : (frontBuffer && frontBuffer->texture) ? frontBuffer
                     : last_rt                               ? last_rt
                     : s.last_resolved_dst ? s.last_resolved_dst
                                           : s.back_buffer_surface;
  if (rt && rt->sourceSurface && rt->sourceSurface != rt &&
      rt->sourceSurface->texture &&
      rt->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
      rt->sourceSurface->descriptorIndex != kInvalidDescriptorIndex) {
    rt = rt->sourceSurface;
  }
  return rt;
}

void RecordPresentPass(VideoState &s, GuestTexture *rt,
                       plume::RenderTexture *back,
                       plume::RenderFramebuffer *back_fb) {
  BD_GPU_ZONE("RecordPresentPass");
  if (rt->layout != plume::RenderTextureLayout::SHADER_READ) {
    const bool needs_discard =
        (rt->layout == plume::RenderTextureLayout::UNKNOWN);
    if (needs_discard) {
      s.command_list->barriers(
          plume::RenderBarrierStage::GRAPHICS,
          plume::RenderTextureBarrier(rt->texture,
                                      plume::RenderTextureLayout::COLOR_WRITE));
      s.command_list->discardTexture(rt->texture);
    }
    s.command_list->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(rt->texture,
                                    plume::RenderTextureLayout::SHADER_READ));
    rt->layout = plume::RenderTextureLayout::SHADER_READ;
  }

  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();
  const u32 gamma_src_desc = rt->descriptorIndex;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);

  const double present_aspect =
      (bd::engine::SofdecPlayer::Playing() && !Output::StretchToFill())
          ? kDesignCanvasAspect
          : Output::RenderAspect();
  u32 fit_w = swap_w, fit_h = swap_h;
  i32 off_x = 0, off_y = 0;
  Output::ComputeFit(swap_w, swap_h, present_aspect, fit_w, fit_h, off_x,
                          off_y);
  if (fit_w != swap_w || fit_h != swap_h) {
    s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  }
  s.command_list->setViewports(plume::RenderViewport(
      static_cast<float>(off_x), static_cast<float>(off_y),
      static_cast<float>(fit_w), static_cast<float>(fit_h)));
  s.command_list->setScissors(
      plume::RenderRect(off_x, off_y, off_x + static_cast<i32>(fit_w),
                        off_y + static_cast<i32>(fit_h)));
  constexpr float kPresentGamma = 1.0f;
  constexpr float kPresentDisplayCorrection = 1.0f;
  // FSR 1 (fsr_easu_ps.hlsl) when the image is smaller than its viewport and
  // bd_switch_fsr wants it; its second "index" is the viewport's size.
  const bool fsr = s.fsr_easu_pipeline &&
                   bd::platform::SwitchFsrWanted(rt->width, rt->height, fit_w,
                                                 fit_h) &&
                   fit_w < 65536 && fit_h < 65536;
  s.command_list->setPipeline(fsr ? s.fsr_easu_pipeline.get()
                                  : s.gamma_correction_pipeline.get());
  struct PresentPushConstants {
    u32 descriptor_index;
    u32 descriptor_index_2;
    float gamma;
    float display_correction;
  } pc{gamma_src_desc, fsr ? (fit_w | (fit_h << 16)) : 0u,
       s.guest_gamma * kPresentGamma, kPresentDisplayCorrection};
  s.command_list->setGraphicsPushConstants(kCopyPushConstantRangeIndex, &pc,
                                           kCopyPushConstantByteOffset,
                                           sizeof(pc));
  s.command_list->drawInstanced(3, 1, 0, 0);

  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));

  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }

  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));
}

} // namespace

void Video::Clear(u32 flags, u32 color_argb, float depth, u32 stencil) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  s.frame_present_committed = false;
  BeginCommandList(s);
  if (!s.command_list_open)
    return;
  GuestTexture *rt = nullptr;
  GuestTexture *ds = nullptr;
  ResolveEffectiveTargets(s, rt, ds);
  const bool clear_color = (flags & kClearTarget) != 0 && rt != nullptr;
  const bool clear_depth = (flags & kClearZBuffer) != 0 && ds != nullptr;
  const bool clear_stencil = (flags & kClearStencil) != 0 && ds != nullptr;
  if (!clear_color && !clear_depth && !clear_stencil)
    return;
  if (!BindDrawFramebufferLocked(clear_color))
    return;
  if (clear_color)
    s.command_list->clearColor(0, ArgbToRenderColor(color_argb));
  if (clear_depth || clear_stencil) {
    s.command_list->clearDepthStencil(clear_depth, clear_stencil, depth,
                                      stencil);
  }
}

void Video::RequestResize() {
  state().resize_requested.store(true, std::memory_order_release);
}

void Video::Present(GuestTexture *frontBuffer) {
  // Every draw issued before it recorded (render thread plan, stage 2).
  rq::SyncIfThreaded();
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (s.frame_present_committed) {
    return;
  }

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    RebuildSwapChain(s);
  }

  if (s.framebuffers.empty()) {
    AbandonFrame(s, lock);
    if (lock.owns_lock())
      lock.unlock();
    PaceFrame(true);
    return;
  }

  GuestTexture *rt = SelectPresentSource(s, frontBuffer);

  const bool have_rt_blit =
      rt && rt->texture && rt->descriptorIndex != kInvalidDescriptorIndex &&
      rt->sampleCount == plume::RenderSampleCount::COUNT_1;
  if (!have_rt_blit) {
    static std::atomic<u32> s_log{0};
    const u32 n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 5) {
      BD_ERROR("Present #{} skipped: no drawable RT", n);
    }
    AbandonFrame(s, lock);
    return;
  }

  // Dynamic resolution (bd_switch_dynamic_resolution): the first frames after
  // the render size changes come from freshly rebuilt targets (the screen
  // blinked at each change), so the last image stays on screen for two.
  // Both checks come before the swapchain acquire: an image acquired and not
  // presented is lost, and deko3d aborts on the next acquire once none is left
  // (the console crashed at the first change, 2026-11-13).
  {
    static u32 s_last_w = 0, s_last_h = 0;
    static int s_hold = 0;
    if (s_last_w != 0 && (rt->width != s_last_w || rt->height != s_last_h))
      s_hold = 2;
    s_last_w = rt->width;
    s_last_h = rt->height;
    if (s_hold > 0) {
      --s_hold;
      AbandonFrame(s, lock);
      return;
    }
  }

  u32 texture_index = 0;
#if defined(__SWITCH__)
  const auto t_acquire = FrameStats::Clock::now();
#endif
  {
    BD_CPU_ZONE("AcquireTexture");
    if (!s.swap_chain->acquireTexture(
            s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)].get(),
            &texture_index)) {
      RebuildSwapChain(s);
      if (s.framebuffers.empty() ||
          !s.swap_chain->acquireTexture(
              s.acquire_semaphores[s.frame.load(std::memory_order_relaxed)]
                  .get(),
              &texture_index)) {
        CheckDeviceRemoved("swapchain acquire");
        AbandonFrame(s, lock);
        return;
      }
    }
  }

#if defined(__SWITCH__)
  const double acquire_ms = FrameStats::Ms(t_acquire, FrameStats::Clock::now());
#endif
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();

  BeginCommandList(s);
  pass_timer::MarkPresent(s.command_list, s.frame.load(std::memory_order_relaxed));
  RecordPresentPass(s, rt, back, back_fb);

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  pass_timer::EndFrame(s.command_lists[cur].get(), cur);
  s.command_lists[cur]->end();
  s.command_list_open = false;

  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  {
    BD_CPU_ZONE("Submit");
    s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                                 s.fences[cur].get());
  }
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
#if defined(__SWITCH__)
  const auto t_present = FrameStats::Clock::now();
#endif
  {
    BD_CPU_ZONE("PresentSwap");
    if (!s.swap_chain->present(texture_index, signals, 1)) {
      if (!CheckDeviceRemoved("swapchain present"))
        s.resize_requested.store(true, std::memory_order_release);
    }
  }
#if defined(__SWITCH__)
  const auto t_fence = FrameStats::Clock::now();
#endif
  {
    BD_CPU_ZONE("WaitFence");
    AdvanceAndWaitReused(s);
  }
#if defined(__SWITCH__)
  const auto t_fence_end = FrameStats::Clock::now();
#endif
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  s.frame_present_committed = true;
  BD_FRAME_MARK();
  Video::SyncBackBufferSizeLocked();
  lock.unlock();
  {
    BD_CPU_ZONE("DrainSlot");
    DrainSlot(s, reclaimed);
  }
#if defined(__SWITCH__)
  const auto t_pace = FrameStats::Clock::now();
#endif
  {
    BD_CPU_ZONE("PaceFrame");
    PaceFrame();
  }
#if defined(__SWITCH__)
  const auto t_end = FrameStats::Clock::now();
  RexSwitchPerfCount(0);
  Stats().Present(acquire_ms, FrameStats::Ms(t_present, t_fence), FrameStats::Ms(t_fence, t_fence_end),
                  FrameStats::Ms(t_pace, t_end));
#endif
}

void Video::SkipPresent() {
  // Every draw issued before it recorded (render thread plan, stage 2).
  rq::SyncIfThreaded();
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.ready || s.shutting_down.load(std::memory_order_acquire) ||
      s.frame_present_committed) {
    return;
  }
  AbandonFrame(s, lock);
}

void Video::PresentOverlayFrame() {
  // Every draw issued before it recorded (render thread plan, stage 2).
  rq::SyncIfThreaded();
  auto &s = state();
  if (s.shutting_down.load(std::memory_order_acquire))
    return;
  std::unique_lock lock(s.mutex);
  if (!s.swap_chain || s.ready)
    return;

  const bool resize_requested =
      s.resize_requested.exchange(false, std::memory_order_acq_rel);
  if (s.swap_chain->needsResize() || resize_requested) {
    for (u32 i = 0; i < kNumFrames; ++i) {
      if (s.command_list_submitted[i]) {
        s.queue->waitForCommandFence(s.fences[i].get());
        s.command_list_submitted[i] = false;
      }
    }
    s.framebuffers.clear();
    if (!s.swap_chain->resize() || !BuildFramebuffers(s) ||
        !BuildPresentSemaphores(s)) {
      if (s.swap_chain->getWidth() && s.swap_chain->getHeight())
        BD_ERROR("Swap chain resize failed");
    }
  }
  if (s.framebuffers.empty())
    return;

  const u32 cur = s.frame.load(std::memory_order_relaxed);
  u32 texture_index = 0;
  if (!s.swap_chain->acquireTexture(s.acquire_semaphores[cur].get(),
                                    &texture_index)) {
    s.resize_requested.store(true, std::memory_order_release);
    return;
  }
  plume::RenderTexture *back = s.swap_chain->getTexture(texture_index);
  plume::RenderFramebuffer *back_fb = s.framebuffers[texture_index].get();
  const u32 swap_w = s.swap_chain->getWidth();
  const u32 swap_h = s.swap_chain->getHeight();

  BeginCommandList(s);
  if (!s.command_list_open)
    return;

  s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(
                               back, plume::RenderTextureLayout::COLOR_WRITE));
  s.command_list->setFramebuffer(back_fb);
  s.command_list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  s.command_list->setViewports(plume::RenderViewport(
      0.0f, 0.0f, static_cast<float>(swap_w), static_cast<float>(swap_h)));
  s.command_list->setScissors(plume::RenderRect(0, 0, static_cast<i32>(swap_w),
                                                static_cast<i32>(swap_h)));
  if (g_overlay_draw_hook) {
    g_overlay_draw_hook(s.command_list, back_fb, swap_w, swap_h);
  }
  s.command_list->setFramebuffer(nullptr);
  s.command_list->barriers(
      plume::RenderBarrierStage::GRAPHICS,
      plume::RenderTextureBarrier(back, plume::RenderTextureLayout::PRESENT));

  s.command_lists[cur]->end();
  s.command_list_open = false;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  plume::RenderCommandSemaphore *waits[] = {s.acquire_semaphores[cur].get()};
  plume::RenderCommandSemaphore *signals[] = {
      s.render_semaphores[texture_index].get()};
  s.queue->executeCommandLists(lists, 1, waits, 1, signals, 1,
                               s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  ApplyVsync(s);
  if (!s.swap_chain->present(texture_index, signals, 1)) {
    CheckDeviceRemoved("swapchain present (overlay)");
  }
  AdvanceAndWaitReused(s);
  const u32 reclaimed = s.frame.load(std::memory_order_relaxed);
  lock.unlock();
  DrainSlot(s, reclaimed);
}

} // namespace bd::gpu
