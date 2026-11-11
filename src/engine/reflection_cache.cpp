/**
 * @file    engine/reflection_cache.cpp
 * @brief   Plane reflection (water) redrawn every Nth frame, small objects left
 *          out of it.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "cpu/hot_calls.h"
#include "engine/reflection_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"

REXCVAR_DEFINE_INT32(bd_reflection_update_interval, 2, "Graphics",
                     "Water and floor reflections are drawn every this many "
                     "frames and reused in between (with the mirror matrix "
                     "they were drawn with). 1 draws them every frame, as the "
                     "game does.");

REXCVAR_DEFINE_DOUBLE(bd_reflection_min_pixels, 3.0, "Graphics",
                      "Objects that would look smaller than this many pixels "
                      "across in the reflection texture are not drawn into it "
                      "(the reflection is 320x180 on the Switch: grass and "
                      "small props vanish in it anyway). 0 draws all.");

// bdRenderViewSubmit draws a plane reflection (the water's, 320x180 RGBA8 on the
// Switch) for each active slot of the reflect info array at 0x82DD7170: it
// builds the slot's mirror view and projection (bdBuildMirrorViewProjection,
// from the camera's eye at +288 and target at +300), then a stack pass object
// (vtable 0x8206C910) whose vtable[3], sub_821875F8, opens the pass: it pushes
// a colour and a depth surface of the slot's size, clears them and binds the
// mirror view from the slot (+16 view, +80 projection). The scene's reflection
// list is walked by bdSceneSubmitRenderList (in busy water areas 1400-1500 draws
// a frame, a third of the frame's draws), and vtable[4], sub_821877C8, closes
// it: resolves the colour into the slot's texture (+12), pops the surfaces and
// releases them.
//
// On a frame that reuses the reflection, the mirror matrices are not rebuilt (so
// the water samples the texture with the matrix it was drawn with), the opening
// runs as the game wrote it, the list walk is skipped and the closing runs
// without its resolve. The texture keeps the last drawn reflection (the resolve
// alias is materialized before its surface is issued again, gpu/graveyard.cpp).
// A camera cut (the eye more than kCutDistance from where it was when the slot
// was drawn) draws it at once.

REX_EXTERN(__imp__sub_821875F8);
REX_EXTERN(__imp__sub_821877C8);
REBLUE_HOT_DECLARE(bdSceneSubmitRenderList)
REX_EXTERN(bdDestroySurface);
REX_EXTERN(bdRenderTargetRelease);

namespace {

constexpr u32 kPassInfo = 40;     // the slot's reflect info
constexpr u32 kPassColor = 28;    // the pushed colour surface
constexpr u32 kPassDepth = 36;    // the pushed depth surface
constexpr u32 kInfoHeight = 172;  // PlaneReflectInfo::height
constexpr u32 kPassFlag = 0x82DBEA60;
constexpr float kCutDistance = 128.0f;

struct Slot {
  int sinceDrawn = 0;
  float eye[3] = {};
  bool drawn = false;
};

struct Stats {
  u64 drawn = 0, reused = 0, cuts = 0, objects = 0, skipped = 0;
  std::chrono::steady_clock::time_point last{};
};

std::unordered_map<u32, Slot> g_slots;
Stats g_stats;

// From the mirror build to the opening of that slot's pass.
bool g_reuseNext = false;
// From the opening to the closing.
bool g_reusing = false;
u32 g_passHeight = 0;
float g_eye[3] = {};

void LogStats() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_stats.last < std::chrono::seconds(10))
    return;
  if (g_stats.last.time_since_epoch().count() != 0)
    BD_INFO("[reflection] drawn {} reused {} (cuts {}) | objects left out as "
            "small (under {} pixels) {} of {}",
            g_stats.drawn, g_stats.reused, g_stats.cuts,
            REXCVAR_GET(bd_reflection_min_pixels), g_stats.skipped,
            g_stats.objects);
  g_stats = Stats{};
  g_stats.last = now;
}

} // namespace

bool bd::engine::ReflectionReuseThisFrame(u32 info, u32 eyeVa) {
  LogStats();
  float eye[3];
  for (int i = 0; i < 3; ++i)
    eye[i] = bd::mem::load<float>(eyeVa + 4 * i);
  std::copy(eye, eye + 3, g_eye);
  const int interval = REXCVAR_GET(bd_reflection_update_interval);
  Slot &slot = g_slots[info];
  bool draw = interval <= 1 || !slot.drawn || ++slot.sinceDrawn >= interval;
  if (!draw) {
    for (int i = 0; i < 3; ++i)
      if (!(std::fabs(eye[i] - slot.eye[i]) <= kCutDistance)) {
        draw = true;
        ++g_stats.cuts;
        break;
      }
  }
  if (draw) {
    slot.drawn = true;
    slot.sinceDrawn = 0;
    std::copy(eye, eye + 3, slot.eye);
    ++g_stats.drawn;
  } else {
    ++g_stats.reused;
  }
  g_reuseNext = !draw;
  return !draw;
}

u32 bd::engine::ReflectionPassHeight() { return g_passHeight; }

bool bd::engine::ReflectionPassEye(float eye[3]) {
  if (!g_passHeight)
    return false;
  std::copy(g_eye, g_eye + 3, eye);
  return true;
}

void bd::engine::CountReflectionObject(bool skipped) {
  ++g_stats.objects;
  g_stats.skipped += skipped;
}

// The opening: as written, so the pass leaves the same state behind.
REX_HOOK_RAW(sub_821875F8) {
  g_reusing = g_reuseNext;
  g_reuseNext = false;
  const u32 info = bd::mem::load<u32>(ctx.r3.u32 + kPassInfo);
  g_passHeight = info ? bd::mem::load<u32>(info + kInfoHeight) : 0;
  if (!g_passHeight)
    g_passHeight = 180;
  __imp__sub_821875F8(ctx, base);
}

// The reflection list of a reused pass is not walked.
REX_HOOK_RAW(bdSceneSubmitRenderList) {
  if (g_reusing) {
    BD_CPU_ZONE("ReflectionReuse");
    return;
  }
  REBLUE_HOT_CALL(bdSceneSubmitRenderList)(ctx, base);
}

// The closing: without the resolve on a reused frame (the surface was cleared
// and never drawn), the rest as the game wrote it.
REX_HOOK_RAW(sub_821877C8) {
  g_passHeight = 0;
  if (!g_reusing) {
    __imp__sub_821877C8(ctx, base);
    return;
  }
  g_reusing = false;
  const u32 pass = ctx.r3.u32;
  bdDestroySurface(ctx, base);
  for (const u32 off : {kPassColor, kPassDepth}) {
    if (const u32 surface = bd::mem::load<u32>(pass + off)) {
      ctx.r3.u64 = surface;
      bdRenderTargetRelease(ctx, base);
      bd::mem::store<u32>(pass + off, 0);
    }
  }
  bd::mem::store<u8>(kPassFlag, 0);
}
