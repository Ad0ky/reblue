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
// On a frame that reuses the map, the opening runs as the game wrote it
// except for sub_821752E8, so the projector map info keeps the light matrix
// the map was drawn with; the caster list is not walked, and the closing runs
// without its resolve, which would copy the freshly cleared surface over the
// map. Everything else the pass sets for later passes (render states, the
// light view the receivers are drawn against, the shader eye, the sampler
// setup) is set exactly as on a drawn frame. A first version replaced the
// opening by a stand-in and the receivers then used another pass's light
// matrix on reused frames: the shadows jumped every other frame.
//
// A camera cut draws the map at once: the eye or target the light frustum is
// built from (camera +288 and +300) more than kCutDistance from where they
// were when the map was drawn. Which camera object it is does not matter: in
// some field scenes two cameras with the same eye open the pass every frame
// (a reused map is still the pair of map and matrix last drawn), and keying on
// the camera made every one of those frames a cut.

REX_EXTERN(__imp__sub_82187168);
REX_EXTERN(__imp__sub_82187330);
REX_EXTERN(__imp__sub_821752E8);
REX_EXTERN(__imp__bdSceneNodeSubmitRenderList);
REX_EXTERN(bdSetRenderState);
REX_EXTERN(bdDestroySurface);
REX_EXTERN(bdRenderTargetRelease);

namespace {

constexpr u32 kShadowPassOpen = 0x82187168;
constexpr u32 kVtableOpen = 12;
constexpr u32 kPassCamera = 12;
constexpr u32 kCameraLightInputs = 288; // eye at +288, target at +300
constexpr float kCutDistance = 128.0f; // the sun frustum is ~512 units across at coverage 1

struct Stats {
  u64 drawn = 0, reused = 0, forced = 0, unpaired = 0;
  std::chrono::steady_clock::time_point last{};
};

struct State {
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
  if (!draw && MovedFar(inputs, g.inputs)) {
    draw = true;
    ++g.stats.forced;
  }
  if (draw) {
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

constexpr u32 kPassSurface = 36;
constexpr u32 kShadowPassFlag = 0x82DBEA60;

// sub_82187330 without its D3DDevice_Resolve: render state 212 back to 7, pop
// the render target, release the pass's depth surface, clear the pass flag.
void CloseWithoutResolve(PPCContext &ctx, u8 *base, u32 pass) {
  g_reusing = false;
  SetRenderState(ctx, base, 212, 7);
  bdDestroySurface(ctx, base);
  if (const u32 surface = bd::mem::load<u32>(pass + kPassSurface)) {
    ctx.r3.u64 = surface;
    bdRenderTargetRelease(ctx, base);
    bd::mem::store<u32>(pass + kPassSurface, 0);
  }
  bd::mem::store<u8>(kShadowPassFlag, 0);
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
  g_reusing = !DrawThisFrame(camera, interval);
  __imp__sub_82187168(ctx, base);
}

// The sun's light matrix: kept from the frame the map was drawn on.
REX_HOOK_RAW(sub_821752E8) {
  if (g_reusing)
    return;
  __imp__sub_821752E8(ctx, base);
}

// The caster list and the closing of a reused pass.
REX_HOOK_RAW(bdSceneNodeSubmitRenderList) {
  if (g_reusing && IsSunShadowPass(ctx.r3.u32)) {
    BD_CPU_ZONE("ShadowMapReuse");
    CloseWithoutResolve(ctx, base, ctx.r3.u32);
    return;
  }
  __imp__bdSceneNodeSubmitRenderList(ctx, base);
}

REX_HOOK_RAW(sub_82187330) {
  if (g_reusing) {
    CloseWithoutResolve(ctx, base, ctx.r3.u32);
    return;
  }
  __imp__sub_82187330(ctx, base);
}
