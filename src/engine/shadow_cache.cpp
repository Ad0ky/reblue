/**
 * @file    engine/shadow_cache.cpp
 * @brief   Sun shadow map redrawn every Nth frame, reused in between.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <algorithm>
#include <chrono>
#include <cmath>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"

REXCVAR_DEFINE_INT32(bd_shadow_update_interval, 2, "Graphics",
                     "The sun shadow map is drawn every this many frames and "
                     "reused in between (its light matrix with it, so static "
                     "shadows stay put; moving ones trail by up to N-1 "
                     "frames). 1 draws it every frame, as the game does.");

// The sun shadow pass is a stack object bdRenderViewSubmit builds (vtable
// 0x8206C8A0) and runs in three steps. bdShaderSystemBeginFrame calls its
// vtable[3], sub_82187168, which opens the pass: it pushes a 1024x1024 depth
// surface, clears it, builds the camera view (bdBuildViewMatrix on the camera
// at this+12, then sub_82283068), recomputes the sun's projector map info at
// 0x82DD6100 (sub_821752E8, which the receivers sample with) and makes the
// light view the render view. bdSceneNodeSubmitRenderList then walks the
// shadow caster list (~3000 of the ~4000 draws of a busy field frame on the
// console) and calls vtable[4], sub_82187330, which closes the pass: it
// resolves the depth into the projector map's texture and pops the surface.
//
// On a frame that reuses the map, the opening is replaced by what it leaves
// behind for later passes (sampler setup, the camera view build and the render
// states it sets and does not restore), the caster list is not walked, and the
// closing only restores its render state. The texture keeps last frame's depth
// (the resolve alias is materialized before its surface is issued again,
// gpu/graveyard.cpp) and the projector map info keeps the light matrix it was
// drawn with, since sub_821752E8 does not run.
//
// A camera cut or a new camera draws the map at once: the eye and target the
// light frustum is built from (camera +288 and +300) moving more than
// kCutDistance, or a different camera.

REX_EXTERN(__imp__sub_82187168);
REX_EXTERN(__imp__sub_82187330);
REX_EXTERN(__imp__bdSceneNodeSubmitRenderList);
REX_EXTERN(bdSetRenderState);
REX_EXTERN(bdBuildViewMatrix);
REX_EXTERN(sub_82283068);
REX_EXTERN(sub_82184A88);

namespace {

constexpr u32 kShadowPassOpen = 0x82187168;
constexpr u32 kVtableOpen = 12;
constexpr u32 kPassCamera = 12;
constexpr u32 kCameraView = 160;
constexpr u32 kCameraProj = 224;
constexpr u32 kCameraLightInputs = 288; // eye at +288, target at +300
constexpr float kCutDistance = 48.0f;

struct Stats {
  u64 drawn = 0, reused = 0, forced = 0, unpaired = 0;
  std::chrono::steady_clock::time_point last{};
};

struct State {
  u32 camera = 0;     // camera the map was last drawn for
  int sinceDrawn = 0; // frames reusing it since
  float inputs[6] = {};
  Stats stats;
} g_shadow;

// Set from a skipped opening to its closing.
bool g_reusing = false;

void ReadInputs(u32 camera, float out[6]) {
  for (int i = 0; i < 6; ++i)
    out[i] = camera ? bd::mem::load<float>(camera + kCameraLightInputs + 4 * i)
                    : 0.0f;
}

bool MovedFar(const float a[6], const float b[6]) {
  for (int i = 0; i < 6; ++i)
    if (!(std::fabs(a[i] - b[i]) <= kCutDistance)) // NaN counts as far
      return true;
  return false;
}

void LogStats() {
  auto &st = g_shadow.stats;
  const auto now = std::chrono::steady_clock::now();
  if (now - st.last < std::chrono::seconds(10))
    return;
  if (st.last.time_since_epoch().count() != 0)
    BD_INFO("[shadow] sun map drawn {} reused {} (cuts {}), closing missed {}",
            st.drawn, st.reused, st.forced, st.unpaired);
  st = Stats{};
  st.last = now;
}

// Should this frame's map be drawn? Updates the bookkeeping either way.
bool DrawThisFrame(u32 camera, int interval) {
  auto &g = g_shadow;
  float inputs[6];
  ReadInputs(camera, inputs);
  bool draw = interval <= 1 || ++g.sinceDrawn >= interval;
  if (!draw && (camera != g.camera || MovedFar(inputs, g.inputs))) {
    draw = true;
    ++g.stats.forced;
  }
  if (draw) {
    g.camera = camera;
    g.sinceDrawn = 0;
    std::copy(inputs, inputs + 6, g.inputs);
    ++g.stats.drawn;
  } else {
    ++g.stats.reused;
  }
  return draw;
}

void SetRenderState(PPCContext &ctx, u8 *base, u32 state, u32 value) {
  ctx.r3.u64 = state;
  ctx.r4.u64 = value;
  bdSetRenderState(ctx, base);
}

// What the skipped opening would have left for the passes after it.
void StandInForOpen(PPCContext &ctx, u8 *base, u32 camera) {
  SetRenderState(ctx, base, 60, 0);
  SetRenderState(ctx, base, 72, 6);
  SetRenderState(ctx, base, 76, 7);
  SetRenderState(ctx, base, 40, 1);
  sub_82184A88(ctx, base);
  if (camera) {
    ctx.r3.u64 = 0;
    ctx.r4.u64 = camera + kCameraView;
    ctx.r5.u64 = camera + kCameraProj;
    bdBuildViewMatrix(ctx, base);
    sub_82283068(ctx, base);
  }
}

// And the closing: the opening sets 212 to 0, the closing back to 7.
void StandInForClose(PPCContext &ctx, u8 *base) {
  g_reusing = false;
  SetRenderState(ctx, base, 212, 7);
}

bool IsSunShadowPass(u32 object) {
  const u32 vtable = object ? bd::mem::load<u32>(object) : 0;
  return vtable && bd::mem::load<u32>(vtable + kVtableOpen) == kShadowPassOpen;
}

} // namespace

REX_HOOK_RAW(sub_82187168) {
  if (g_reusing) {
    g_reusing = false; // the last one was never closed
    ++g_shadow.stats.unpaired;
  }
  LogStats();
  const int interval = REXCVAR_GET(bd_shadow_update_interval);
  const u32 camera = bd::mem::load<u32>(ctx.r3.u32 + kPassCamera);
  if (DrawThisFrame(camera, interval)) {
    __imp__sub_82187168(ctx, base);
    return;
  }
  g_reusing = true;
  StandInForOpen(ctx, base, camera);
}

// The caster list and the closing of a skipped pass: nothing to draw, and the
// closing would resolve a depth surface that was never drawn.
REX_HOOK_RAW(bdSceneNodeSubmitRenderList) {
  if (g_reusing && IsSunShadowPass(ctx.r3.u32)) {
    BD_CPU_ZONE("ShadowMapReuse");
    StandInForClose(ctx, base);
    return;
  }
  __imp__bdSceneNodeSubmitRenderList(ctx, base);
}

REX_HOOK_RAW(sub_82187330) {
  if (g_reusing) {
    StandInForClose(ctx, base);
    return;
  }
  __imp__sub_82187330(ctx, base);
}
