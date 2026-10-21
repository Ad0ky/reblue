/**
 * @file    gpu/pass_timer.cpp
 * @brief   GPU time per render pass from timestamp queries (Switch). See
 *          pass_timer.h.
 */
#include "gpu/pass_timer.h"

#if defined(__SWITCH__)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "core/logging.h"
#include "gpu/device.h"
#include "gpu/resources.h"

namespace bd::gpu::pass_timer {

namespace {

// Timestamps per frame. A frame with more render-target changes keeps the
// first ones and folds the rest into the last pass it timed.
constexpr u32 kMaxStamps = 128;

struct Slot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  u32 used = 0;       // timestamps written this frame
  bool open = false;  // between BeginFrame and EndFrame
  bool ready = false; // EndFrame reached, results pending
  // Label of the segment that starts at timestamp i (i < used - 1).
  std::vector<std::string> labels;
};

struct Totals {
  double ns = 0;
  u64 passes = 0;
};

Slot g_slots[kNumFrames];
bool g_failed = false;

// Accumulated over the report window.
std::unordered_map<std::string, Totals> g_totals;
double g_frame_ns = 0;
double g_worst_frame_ns = 0;
u64 g_frames = 0;
u64 g_overflow = 0;
std::chrono::steady_clock::time_point g_window_start{};

const char *FormatName(plume::RenderFormat f) {
  switch (f) {
  case plume::RenderFormat::R8G8B8A8_UNORM:
    return "RGBA8";
  case plume::RenderFormat::B8G8R8A8_UNORM:
    return "BGRA8";
  case plume::RenderFormat::R16G16B16A16_FLOAT:
    return "RGBA16F";
  case plume::RenderFormat::R32_FLOAT:
    return "R32F";
  case plume::RenderFormat::R16G16_FLOAT:
    return "RG16F";
  case plume::RenderFormat::D32_FLOAT_S8_UINT:
    return "D32S8";
  case plume::RenderFormat::D32_FLOAT:
    return "D32";
  case plume::RenderFormat::D24_UNORM_S8_UINT:
    return "D24S8";
  case plume::RenderFormat::D16_UNORM:
    return "D16";
  default:
    return nullptr;
  }
}

void Describe(std::string &out, const GuestTexture *t) {
  char buf[64];
  const char *name = FormatName(t->format);
  if (name)
    std::snprintf(buf, sizeof(buf), "%ux%u %s", t->width, t->height, name);
  else
    std::snprintf(buf, sizeof(buf), "%ux%u fmt%u", t->width, t->height,
                  static_cast<u32>(t->format));
  out += buf;
  if (t->sampleCount != plume::RenderSampleCount::COUNT_1)
    out += " MSAA";
}

std::string Label(const GuestTexture *rt, const GuestTexture *ds) {
  std::string s;
  if (rt)
    Describe(s, rt);
  else
    s = "depth only";
  if (ds) {
    s += rt ? " + depth " : " ";
    if (!rt)
      Describe(s, ds);
    else {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%ux%u", ds->width, ds->height);
      s += buf;
    }
  }
  return s;
}

void Stamp(plume::RenderCommandList *list, Slot &slot, std::string label) {
  if (slot.used >= kMaxStamps - 1) { // keep the last index for EndFrame
    ++g_overflow;
    return;
  }
  list->writeTimestamp(slot.pool.get(), slot.used);
  slot.labels.resize(slot.used + 1);
  slot.labels[slot.used] = std::move(label);
  ++slot.used;
}

void Report(double window_s) {
  if (!g_frames)
    return;
  std::vector<std::pair<std::string, Totals>> rows(g_totals.begin(),
                                                  g_totals.end());
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    return a.second.ns > b.second.ns;
  });
  const double frames = double(g_frames);
  const double frame_ms = g_frame_ns / frames / 1e6;
  BD_INFO("[gpu-passes] {:.1f} s, {} frames: GPU {:.1f} ms per frame (worst "
          "{:.1f}), {} distinct targets{}",
          window_s, g_frames, frame_ms, g_worst_frame_ns / 1e6, rows.size(),
          g_overflow ? " (some frames had more passes than timed)" : "");
  const size_t shown = std::min<size_t>(rows.size(), 14);
  for (size_t i = 0; i < shown; ++i) {
    const auto &[label, t] = rows[i];
    BD_INFO("[gpu-passes]   {:5.1f} ms/frame {:4.1f}%  x{:.1f}/frame  {}",
            t.ns / frames / 1e6, g_frame_ns > 0 ? 100.0 * t.ns / g_frame_ns : 0.0,
            double(t.passes) / frames, label);
  }
  g_totals.clear();
  g_frame_ns = 0;
  g_worst_frame_ns = 0;
  g_frames = 0;
  g_overflow = 0;
}

} // namespace

void BeginFrame(plume::RenderDevice *device, plume::RenderCommandList *list,
                u32 slot_index) {
  if (g_failed || !device || !list || slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.pool) {
    slot.pool = device->createQueryPool(kMaxStamps);
    if (!slot.pool) {
      g_failed = true;
      return;
    }
  }
  // Reopened before its results were read (an abandoned frame): drop them.
  slot.ready = false;
  slot.used = 0;
  slot.open = true;
  list->resetQueryPool(slot.pool.get(), 0, kMaxStamps);
  Stamp(list, slot, "start of frame (before the first target)");
}

void MarkTargets(plume::RenderCommandList *list, u32 slot_index,
                 const GuestTexture *rt, const GuestTexture *ds) {
  if (slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.open)
    return;
  Stamp(list, slot, Label(rt, ds));
}

void MarkPresent(plume::RenderCommandList *list, u32 slot_index) {
  if (slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.open)
    return;
  Stamp(list, slot, "present (scale to the swapchain)");
}

void EndFrame(plume::RenderCommandList *list, u32 slot_index) {
  if (slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.open)
    return;
  // Every query of the pool must be written for the results to be available,
  // so the unused ones repeat the last timestamp.
  for (u32 i = slot.used; i < kMaxStamps; ++i)
    list->writeTimestamp(slot.pool.get(), i);
  slot.open = false;
  slot.ready = true;
}

void Collect(u32 slot_index) {
  if (slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.ready)
    return;
  slot.ready = false;
  slot.pool->queryResults();
  const u64 *ts = slot.pool->getResults();
  if (!ts || slot.used == 0)
    return;
  // Index `used` holds the first of the end-of-frame stamps.
  const u32 last = slot.used;
  const u64 start = ts[0];
  const u64 end = ts[last];
  if (end <= start)
    return;
  for (u32 i = 0; i < last; ++i) {
    const u64 a = ts[i];
    const u64 b = ts[i + 1];
    if (b < a)
      continue;
    Totals &t = g_totals[slot.labels[i]];
    t.ns += double(b - a);
    ++t.passes;
  }
  const double frame_ns = double(end - start);
  g_frame_ns += frame_ns;
  g_worst_frame_ns = std::max(g_worst_frame_ns, frame_ns);
  ++g_frames;

  const auto now = std::chrono::steady_clock::now();
  if (g_window_start.time_since_epoch().count() == 0)
    g_window_start = now;
  const double window_s =
      std::chrono::duration<double>(now - g_window_start).count();
  if (window_s >= 10.0) {
    Report(window_s);
    g_window_start = now;
  }
}

} // namespace bd::gpu::pass_timer

#endif // __SWITCH__
