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
#include "gpu/shaders/shader_cache.h"

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

// ---- shaders ----

// One frame in this many is timed per shader.
constexpr u32 kShaderFrameEvery = 8;
// Timestamps in such a frame. A frame with more shader changes keeps the
// first ones and folds the rest into the last segment.
constexpr u32 kMaxShaderStamps = 2048;

// What the GPU time after a stamp belongs to.
struct Segment {
  u64 ps = 0; // pixel shader hash, 0 for none
  u64 vs = 0;
  u32 target = 0; // index into g_targetNames
  u32 draws = 0;  // 0: clears, copies and resolves before the target's first draw
  bool present = false;
};

struct ShaderSlot {
  std::unique_ptr<plume::RenderQueryPool> pool;
  u32 used = 0;
  bool open = false;
  bool ready = false;
  std::vector<Segment> segments; // segment i starts at stamp i
};

struct ShaderTotals {
  double ns = 0;
  u64 draws = 0;
  std::unordered_map<u64, u64> vsDraws;
  std::unordered_map<u32, double> targetNs;
};

ShaderSlot g_shaderSlots[kNumFrames];
bool g_shaderFailed = false;
u64 g_frameCounter = 0;
u32 g_currentTarget = 0; // of the frame being recorded

std::vector<std::string> g_targetNames;
std::unordered_map<std::string, u32> g_targetIndex;

std::unordered_map<u64, ShaderTotals> g_shaderTotals;
double g_shaderFrameNs = 0;  // all stamped GPU time of the timed frames
double g_betweenNs = 0;      // time not inside draws (clears, resolves)
u64 g_shaderFrames = 0;
u64 g_shaderDraws = 0;
u64 g_shaderOverflow = 0;

u32 TargetIndex(const std::string &name) {
  auto it = g_targetIndex.find(name);
  if (it != g_targetIndex.end())
    return it->second;
  const u32 index = u32(g_targetNames.size());
  g_targetNames.push_back(name);
  g_targetIndex.emplace(name, index);
  return index;
}

u64 ShaderHash(const GuestShader *shader) {
  if (!shader)
    return 0;
  if (shader->shaderCacheEntry)
    return shader->shaderCacheEntry->hash;
  // Not in the cache: by guest address, marked with the top bit.
  return (u64(1) << 63) | shader->selfVa;
}

void ShaderStamp(plume::RenderCommandList *list, ShaderSlot &slot,
                 const Segment &segment) {
  if (slot.used >= kMaxShaderStamps - 1) { // the last index is EndFrame's
    ++g_shaderOverflow;
    if (!slot.segments.empty())
      slot.segments.back().draws += segment.draws;
    return;
  }
  list->writeTimestamp(slot.pool.get(), slot.used);
  slot.segments.resize(slot.used + 1);
  slot.segments[slot.used] = segment;
  ++slot.used;
}

void ReportShaders(double window_s) {
  if (!g_shaderFrames)
    return;
  std::vector<std::pair<u64, ShaderTotals *>> rows;
  rows.reserve(g_shaderTotals.size());
  for (auto &[hash, totals] : g_shaderTotals)
    rows.emplace_back(hash, &totals);
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
    return a.second->ns > b.second->ns;
  });
  const double frames = double(g_shaderFrames);
  BD_INFO("[gpu-shaders] {:.1f} s, {} frames timed: {:.1f} ms of GPU per frame "
          "in {:.0f} draws, {} pixel shaders; {:.1f} ms outside draws (clears, "
          "copies, resolves){}",
          window_s, g_shaderFrames, g_shaderFrameNs / frames / 1e6,
          double(g_shaderDraws) / frames, rows.size(),
          g_betweenNs / frames / 1e6,
          g_shaderOverflow ? " (frames with more shader changes than timed)"
                           : "");
  const size_t shown = std::min<size_t>(rows.size(), 20);
  for (size_t i = 0; i < shown; ++i) {
    const u64 hash = rows[i].first;
    const ShaderTotals &t = *rows[i].second;
    u64 vs = 0, vsMost = 0;
    for (const auto &[h, n] : t.vsDraws)
      if (n > vsMost) {
        vsMost = n;
        vs = h;
      }
    u32 target = 0;
    double targetMost = -1;
    for (const auto &[index, ns] : t.targetNs)
      if (ns > targetMost) {
        targetMost = ns;
        target = index;
      }
    BD_INFO("[gpu-shaders]   {:5.2f} ms/frame {:4.1f}%  {:6.1f} draws/frame  "
            "ps {:016x}  vs {:016x}{}  in {}",
            t.ns / frames / 1e6,
            g_shaderFrameNs > 0 ? 100.0 * t.ns / g_shaderFrameNs : 0.0,
            double(t.draws) / frames, hash, vs,
            t.vsDraws.size() > 1
                ? " (+" + std::to_string(t.vsDraws.size() - 1) + " more)"
                : std::string(),
            target < g_targetNames.size() ? g_targetNames[target] : "?");
  }
  g_shaderTotals.clear();
  g_shaderFrameNs = 0;
  g_betweenNs = 0;
  g_shaderFrames = 0;
  g_shaderDraws = 0;
  g_shaderOverflow = 0;
}

void CollectShaders(u32 slot_index) {
  ShaderSlot &slot = g_shaderSlots[slot_index];
  if (!slot.ready)
    return;
  slot.ready = false;
  slot.pool->queryResults();
  const u64 *ts = slot.pool->getResults();
  if (!ts || slot.used == 0)
    return;
  const u32 last = slot.used;
  if (ts[last] <= ts[0])
    return;
  for (u32 i = 0; i < last; ++i) {
    if (ts[i + 1] < ts[i])
      continue;
    const double ns = double(ts[i + 1] - ts[i]);
    const Segment &seg = slot.segments[i];
    if (seg.present || seg.draws == 0) {
      g_betweenNs += ns;
      continue;
    }
    ShaderTotals &t = g_shaderTotals[seg.ps];
    t.ns += ns;
    t.draws += seg.draws;
    t.vsDraws[seg.vs] += seg.draws;
    t.targetNs[seg.target] += ns;
    g_shaderDraws += seg.draws;
  }
  g_shaderFrameNs += double(ts[last] - ts[0]);
  ++g_shaderFrames;
}

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

  // One frame in kShaderFrameEvery: per-shader timestamps too.
  ShaderSlot &shaders = g_shaderSlots[slot_index];
  shaders.ready = false;
  shaders.open = false;
  if (g_shaderFailed || (g_frameCounter++ % kShaderFrameEvery) != 0)
    return;
  if (!shaders.pool) {
    shaders.pool = device->createQueryPool(kMaxShaderStamps);
    if (!shaders.pool) {
      g_shaderFailed = true;
      return;
    }
  }
  shaders.used = 0;
  shaders.segments.clear();
  shaders.open = true;
  g_currentTarget = TargetIndex("start of frame");
  list->resetQueryPool(shaders.pool.get(), 0, kMaxShaderStamps);
  Segment start;
  start.target = g_currentTarget;
  ShaderStamp(list, shaders, start);
}

void MarkTargets(plume::RenderCommandList *list, u32 slot_index,
                 const GuestTexture *rt, const GuestTexture *ds) {
  if (slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.open)
    return;
  std::string label = Label(rt, ds);
  ShaderSlot &shaders = g_shaderSlots[slot_index];
  if (shaders.open) {
    g_currentTarget = TargetIndex(label);
    Segment seg;
    seg.target = g_currentTarget;
    ShaderStamp(list, shaders, seg);
  }
  Stamp(list, slot, std::move(label));
}

void MarkDraw(plume::RenderCommandList *list, u32 slot_index,
              const GuestShader *vs, const GuestShader *ps) {
  if (slot_index >= kNumFrames)
    return;
  ShaderSlot &slot = g_shaderSlots[slot_index];
  if (!slot.open || !list)
    return;
  const u64 psHash = ShaderHash(ps);
  const u64 vsHash = ShaderHash(vs);
  if (!slot.segments.empty()) {
    Segment &cur = slot.segments.back();
    if (cur.draws > 0 && !cur.present && cur.ps == psHash && cur.vs == vsHash &&
        cur.target == g_currentTarget) {
      ++cur.draws;
      return;
    }
  }
  Segment seg;
  seg.ps = psHash;
  seg.vs = vsHash;
  seg.target = g_currentTarget;
  seg.draws = 1;
  ShaderStamp(list, slot, seg);
}

void MarkPresent(plume::RenderCommandList *list, u32 slot_index) {
  if (slot_index >= kNumFrames)
    return;
  Slot &slot = g_slots[slot_index];
  if (!slot.open)
    return;
  ShaderSlot &shaders = g_shaderSlots[slot_index];
  if (shaders.open) {
    Segment seg;
    seg.present = true;
    ShaderStamp(list, shaders, seg);
  }
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

  ShaderSlot &shaders = g_shaderSlots[slot_index];
  if (shaders.open) {
    for (u32 i = shaders.used; i < kMaxShaderStamps; ++i)
      list->writeTimestamp(shaders.pool.get(), i);
    shaders.open = false;
    shaders.ready = true;
  }
}

void Collect(u32 slot_index) {
  if (slot_index >= kNumFrames)
    return;
  CollectShaders(slot_index);
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
    ReportShaders(window_s);
    g_window_start = now;
  }
}

} // namespace bd::gpu::pass_timer

#endif // __SWITCH__
