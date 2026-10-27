/**
 * @file    engine/frame_clock.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "engine/frame_clock.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <rex/cvar.h>

#include "core/logging.h"
#include "engine/cutscene_pause.h"
#include "engine/settings.h"
#include "engine/sofdec_player.h"

REXCVAR_DEFINE_INT32(bd_interp_min_fps, 36, "Game",
                     "With the frame rate unlimited, the 30Hz simulation is "
                     "interpolated. While a frame's work (without the frame "
                     "limiter's wait) takes longer than 1/this s (a 2 s "
                     "average) the game runs as with a 30 fps limit instead: "
                     "no interpolation work, which then buys nothing. Back "
                     "above this + 12 (interpolating costs ~4 ms a frame on "
                     "the Switch, so a closer threshold flips back and "
                     "forth). 0 = always interpolate.");

namespace bd::engine {
namespace {

constexpr double kTick = 1.0 / 30.0;
// InterpolationActive's inputs, refreshed once per frame by Advance: it is
// asked from every interpolation hook, many times per frame.
std::atomic<bool> g_interp{false};
std::atomic<bool> g_interpKnown{false};
bool g_slowFrames = false;
std::atomic<bool> g_throttled{false};
double g_rawFrameAvg = kTick;
std::atomic<i64> g_pacingSleepNs{0};
constexpr double kMaxReportedDelta = 1.0 / 15.0;
constexpr double kMaxBacklog = kTick * 4.0;

using Clock = std::chrono::steady_clock;

double g_lastTime = 0.0;
double g_frameTime = 0.0;
double g_lastDelta = kTick;
double g_accum = 0.0;
float g_alpha = 0.0f;
bool g_tickDue = true;
u64 g_tickCount = 0;
double g_tps = 0.0;
constexpr double kTpsWindow = 0.5;
double g_tpsTicks = 0.0;
double g_tpsSeconds = 0.0;
constexpr int kDeltaWindow = 8;
constexpr double kDeltaHitchRatio = 3.0;
double g_deltaRing[kDeltaWindow] = {};
int g_deltaCount = 0;
int g_deltaNext = 0;

double DeltaMean() {
  double sum = 0.0;
  for (int i = 0; i < g_deltaCount; ++i)
    sum += g_deltaRing[i];
  return sum / g_deltaCount;
}

double SmoothDelta(double dt) {
  if (g_deltaCount == kDeltaWindow && dt > DeltaMean() * kDeltaHitchRatio) {
    g_deltaCount = 0;
    g_deltaNext = 0;
    return dt;
  }
  g_deltaRing[g_deltaNext] = dt;
  g_deltaNext = (g_deltaNext + 1) % kDeltaWindow;
  if (g_deltaCount < kDeltaWindow)
    ++g_deltaCount;
  return DeltaMean();
}

double NowSeconds() {
  static const Clock::time_point kEpoch = Clock::now();
  return std::chrono::duration<double>(Clock::now() - kEpoch).count();
}

} // namespace

namespace {

bool InterpolationWanted() {
  const i32 fps = Settings::Get().FPSLimit();
  return (fps == 0 || fps > 30) && !SofdecPlayer::Playing() && !g_slowFrames;
}

// A 2 s average of the real frame time decides g_slowFrames, with a gap
// between the two thresholds so it does not flip at the edge.
void UpdateSlowFrames(double raw) {
  const i32 minFps = REXCVAR_GET(bd_interp_min_fps);
  if (minFps <= 0) {
    g_slowFrames = false;
    return;
  }
  const double a = std::clamp(raw / 2.0, 0.01, 0.5);
  g_rawFrameAvg += (std::min(raw, 0.5) - g_rawFrameAvg) * a;
  const bool was = g_slowFrames;
  if (!g_slowFrames && g_rawFrameAvg > 1.0 / double(minFps))
    g_slowFrames = true;
  else if (g_slowFrames && g_rawFrameAvg < 1.0 / double(minFps + 12))
    g_slowFrames = false;
  if (g_slowFrames != was)
    BD_INFO("[frames] interpolation {} (frame work {:.1f} ms, bd_interp_min_fps {})",
            g_slowFrames ? "off, 30 fps cap" : "back on", g_rawFrameAvg * 1000.0, minFps);
}

} // namespace

bool InterpolationThrottled() {
  return g_throttled.load(std::memory_order_relaxed);
}

void AddPacingSleep(double seconds) {
  if (seconds > 0.0)
    g_pacingSleepNs.fetch_add(i64(seconds * 1e9), std::memory_order_relaxed);
}

bool InterpolationActive() {
  if (g_interpKnown.load(std::memory_order_acquire))
    return g_interp.load(std::memory_order_relaxed);
  return InterpolationWanted();
}

void Advance() {
  const double now = NowSeconds();
  if (CutscenePause::Get().Frozen()) {
    g_lastTime = now;
    g_lastDelta = 0.0;
    g_tickDue = false;
    return;
  }
  const double raw =
      (g_lastTime > 0.0) ? std::max(now - g_lastTime, 0.0) : kTick;
  g_lastTime = now;
  {
    const double slept =
        double(g_pacingSleepNs.exchange(0, std::memory_order_relaxed)) * 1e-9;
    UpdateSlowFrames(std::max(raw - slept, 0.0));
    g_throttled.store(g_slowFrames, std::memory_order_relaxed);
  }
  g_interp.store(InterpolationWanted(), std::memory_order_relaxed);
  g_interpKnown.store(true, std::memory_order_release);
  const double dt = SmoothDelta(std::min(raw, kMaxReportedDelta));
  g_lastDelta = dt;
  g_frameTime += dt;

  if (!InterpolationActive()) {
    g_tickDue = true;
    g_alpha = 0.0f;
    g_accum = 0.0;
  } else {
    g_accum = std::min(g_accum + dt, kMaxBacklog);
    g_tickDue = g_accum >= kTick;
    if (g_tickDue)
      g_accum -= kTick;
    g_alpha = static_cast<float>(std::clamp(g_accum / kTick, 0.0, 0.9999));
  }

  if (g_tickDue) {
    ++g_tickCount;
    g_tpsTicks += 1.0;
  }
  g_tpsSeconds += dt;
  if (g_tpsSeconds >= kTpsWindow) {
    g_tps = g_tpsTicks / g_tpsSeconds;
    g_tpsTicks = 0.0;
    g_tpsSeconds = 0.0;
  }
}

namespace {

struct RenderClock {
  double time = 0.0;
  double delta = kTick;
  float alpha = 0.0f;
  bool tickDue = true;
  u64 tick = 0;
};
RenderClock g_render;
thread_local bool t_renderThread = false;

} // namespace

void PublishRenderClock() {
  if (t_renderThread)
    return;
  g_render.time = g_frameTime;
  g_render.delta = g_lastDelta;
  g_render.alpha = g_alpha;
  g_render.tickDue = g_tickDue;
  g_render.tick = g_tickCount;
}

void BindRenderThread() { t_renderThread = true; }
bool IsRenderThread() { return t_renderThread; }

bool TickDue() { return t_renderThread ? g_render.tickDue : g_tickDue; }
float Alpha() {
  if (!InterpolationActive())
    return 0.0f;
  return t_renderThread ? g_render.alpha : g_alpha;
}
u64 TickCount() { return t_renderThread ? g_render.tick : g_tickCount; }
double TicksPerSecond() { return g_tps; }
double FrameTime() { return t_renderThread ? g_render.time : g_frameTime; }
double FrameDelta() { return t_renderThread ? g_render.delta : g_lastDelta; }

} // namespace bd::engine
