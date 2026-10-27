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

// The sun shadow pass is a pair of render command objects in the scene's
// command list (bdSceneSubmitRenderList walks it: type 2 nodes call the
// object's vtable[1]). sub_82187168 opens the pass: it pushes a 1024x1024
// depth surface, clears it, builds the camera view (bdBuildViewMatrix on the
// camera at this+12, then sub_82283068), recomputes the sun's projector map
// info at 0x82DD6100 (sub_821752E8, which the receivers sample with) and makes
// the light view the render view. sub_82187330 closes it: resolves the depth
// into the projector map's texture and pops the surface. Every node between
// the two draws shadow casters, ~3000 of the ~4000 draws of a busy field frame
// on the console.
//
// On a frame that reuses the map, the opening is replaced by what it leaves
// behind for later passes (the camera view build and the render states it
// sets and does not restore), every node up to the closing is left out of the
// walk, in whichever lists they are, and the closing only restores its render
// state. The texture keeps last frame's depth (the resolve alias is
// materialized before its surface is issued again, gpu/graveyard.cpp) and the
// projector map info keeps the light matrix it was drawn with, since
// sub_821752E8 does not run.
//
// A camera cut or a new scene draws the map at once: the inputs the light
// frustum is built from (camera +288 and +300) moving more than
// kCutDistance, or a different pass object.

REX_EXTERN(__imp__bdSceneSubmitRenderList);
REX_EXTERN(__imp__sub_82187168);
REX_EXTERN(__imp__sub_82187330);
REX_EXTERN(bdSetRenderState);
REX_EXTERN(bdBuildViewMatrix);
REX_EXTERN(sub_82283068);

namespace {

constexpr u32 kShadowPassOpen = 0x82187168;
constexpr u32 kShadowPassClose = 0x82187330;
constexpr u32 kNodeTypeObject = 2;
constexpr u32 kNodeNext = 8;
constexpr u32 kNodeObject = 12;
constexpr u32 kPassCamera = 12;
constexpr u32 kCameraView = 160;
constexpr u32 kCameraProj = 224;
constexpr u32 kCameraLightInputs = 288; // two vec3, +288 and +300
constexpr float kCutDistance = 48.0f;
constexpr int kMaxNodes = 4096;

struct Stats {
  u64 drawn = 0, reused = 0, forced = 0, opened = 0, unpaired = 0;
  std::chrono::steady_clock::time_point last{};
};

struct State {
  u32 object = 0;     // pass object the map was last drawn by
  int sinceDrawn = 0; // frames reusing it since
  float inputs[6] = {};
  Stats stats;
} g_shadow;

u32 NodeFunction(u32 node) {
  if (bd::mem::load<u32>(node) != kNodeTypeObject)
    return 0;
  const u32 object = bd::mem::load<u32>(node + kNodeObject);
  const u32 vtable = object ? bd::mem::load<u32>(object) : 0;
  return vtable ? bd::mem::load<u32>(vtable + 4) : 0;
}

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
    BD_INFO("[shadow] sun map drawn {} reused {} (cuts {}), pass opened by the "
            "game {}, closing missed {}",
            st.drawn, st.reused, st.forced, st.opened, st.unpaired);
  st = Stats{};
  st.last = now;
}

// Should this frame's map be drawn? Updates the bookkeeping either way.
bool DrawThisFrame(u32 object, int interval) {
  auto &g = g_shadow;
  const u32 camera = bd::mem::load<u32>(object + kPassCamera);
  float inputs[6];
  ReadInputs(camera, inputs);
  bool draw = interval <= 1 || ++g.sinceDrawn >= interval;
  if (!draw && (object != g.object || MovedFar(inputs, g.inputs))) {
    draw = true;
    ++g.stats.forced;
  }
  if (draw) {
    g.object = object;
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
void StandInForOpen(PPCContext &ctx, u8 *base, u32 object) {
  SetRenderState(ctx, base, 60, 0);
  SetRenderState(ctx, base, 72, 6);
  SetRenderState(ctx, base, 76, 7);
  SetRenderState(ctx, base, 40, 1);
  if (const u32 camera = bd::mem::load<u32>(object + kPassCamera)) {
    ctx.r3.u64 = 0;
    ctx.r4.u64 = camera + kCameraView;
    ctx.r5.u64 = camera + kCameraProj;
    bdBuildViewMatrix(ctx, base);
    sub_82283068(ctx, base);
  }
}

// And the closing: the opening sets 212 to 0, the closing back to 7.
void StandInForClose(PPCContext &ctx, u8 *base) {
  SetRenderState(ctx, base, 212, 7);
}

// Set from a skipped opening to the closing: every node in between is left
// out, whichever lists it is in.
bool g_reusing = false;

void Submit(PPCContext &ctx, u8 *base, u32 scene, u32 mask, u32 head) {
  ctx.r3.u64 = scene;
  ctx.r4.u64 = mask;
  ctx.r5.u64 = head;
  __imp__bdSceneSubmitRenderList(ctx, base);
}

// The nodes from head up to (not including) stop.
void SubmitUntil(PPCContext &ctx, u8 *base, u32 scene, u32 mask, u32 head,
                 u32 last, u32 stop) {
  if (!last)
    return; // stop is the head
  bd::mem::store<u32>(last + kNodeNext, 0);
  Submit(ctx, base, scene, mask, head);
  bd::mem::store<u32>(last + kNodeNext, stop);
}

// First node from head running fn, and the node before it.
u32 FindNode(u32 head, u32 fn, u32 &before) {
  before = 0;
  int n = 0;
  for (u32 node = head; node && n < kMaxNodes;
       before = node, node = bd::mem::load<u32>(node + kNodeNext), ++n)
    if (NodeFunction(node) == fn)
      return node;
  before = 0;
  return 0;
}

} // namespace

REX_HOOK_RAW(bdSceneSubmitRenderList) {
  const int interval = REXCVAR_GET(bd_shadow_update_interval);
  const u32 scene = ctx.r3.u32, mask = ctx.r4.u32, head = ctx.r5.u32;
  if ((interval <= 1 && !g_reusing) || !head) {
    __imp__bdSceneSubmitRenderList(ctx, base);
    return;
  }
  u32 beforeOpen = 0;
  const u32 open = FindNode(head, kShadowPassOpen, beforeOpen);
  if (g_reusing && open) {
    // The closing never came: draw from here as the game would.
    g_reusing = false;
    ++g_shadow.stats.unpaired;
  }
  if (g_reusing) {
    // Inside a skipped pass: only the closing and what follows it run.
    u32 beforeClose = 0;
    if (const u32 close = FindNode(head, kShadowPassClose, beforeClose))
      Submit(ctx, base, scene, mask, close);
    return;
  }
  if (!open) {
    __imp__bdSceneSubmitRenderList(ctx, base);
    return;
  }
  LogStats();
  const u32 object = bd::mem::load<u32>(open + kNodeObject);
  if (DrawThisFrame(object, interval)) {
    __imp__bdSceneSubmitRenderList(ctx, base);
    return;
  }
  BD_CPU_ZONE("ShadowMapReuse");
  SubmitUntil(ctx, base, scene, mask, head, beforeOpen, open);
  StandInForOpen(ctx, base, object);
  g_reusing = true;
  u32 beforeClose = 0;
  if (const u32 close = FindNode(open, kShadowPassClose, beforeClose))
    Submit(ctx, base, scene, mask, close);
}

// Counts the game's own opens: with the reuse on, a busy log line where
// "reused" stays 0 and this keeps counting means the pass was not found in
// the lists bdSceneSubmitRenderList walks.
REX_HOOK_RAW(sub_82187168) {
  ++g_shadow.stats.opened;
  __imp__sub_82187168(ctx, base);
}

// The closing of a skipped pass resolves nothing: the depth surface it would
// resolve was never drawn.
REX_HOOK_RAW(sub_82187330) {
  if (g_reusing) {
    g_reusing = false;
    StandInForClose(ctx, base);
    return;
  }
  __imp__sub_82187330(ctx, base);
}
