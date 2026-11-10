/**
 * @file    engine/audio_threads.cpp
 * @brief   More worker threads for the game's XAudio2 engine.
 *
 * The game's audio graph (voices, their effects, the mix) is processed by the
 * Xbox 360 XAudio2 library linked into it. That library starts one worker
 * thread per hardware thread in the processor mask it is created with
 * (sub_8268A7A0: the first one drives each 5 ms block, sub_8268A410; the
 * others, sub_8268A4D8, take voices from the same per-stage queues,
 * sub_82689D88 / sub_82689A00), and the game asks for one.
 *
 * On the Switch that one thread ran out of time (2026-11-10): the areas after
 * loading run up to ten reverbs (sub_8267F4D8: 1880 calls a second, 440 us
 * each, 83% of a core on their own), the thread sat at 94% of a core and the
 * audio output found no data ~1300 times a run, while a fourth of the CPU was
 * free. sub_82688DD8 turns the mask into the thread table, for the engine's
 * memory sizes and for the thread creation alike: adding bits to the mask
 * there gives the library's own multi-threaded mode, each worker with its own
 * hardware thread number (its scratch buffers are indexed by it).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include <atomic>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/types.h>

REX_EXTERN(__imp__sub_82688DD8);

#if defined(__SWITCH__)
#define BD_AUDIO_ENGINE_THREADS_DEFAULT 2
#else
#define BD_AUDIO_ENGINE_THREADS_DEFAULT 0
#endif

REXCVAR_DEFINE_INT32(bd_audio_engine_threads, BD_AUDIO_ENGINE_THREADS_DEFAULT,
                     "Audio",
                     "Worker threads of the game's XAudio2 engine (voices, "
                     "reverbs, mixing): 0 = as the game asks (one), 2 or 3 = "
                     "that many, sharing the voices of each audio block. "
                     "Read when the game starts its audio.");

// XAudio2's processor mask -> thread table (r3 = mask, r4 = table).
REX_HOOK_RAW(sub_82688DD8) {
  const int want = REXCVAR_GET(bd_audio_engine_threads);
  const u32 asked = ctx.r3.u32 & 0x3F;
  u32 mask = asked ? asked : 0x10; // the library's default: hardware thread 4
  if (want > 1) {
    // Thread 4 stays the driving one when it is in the mask (the library
    // picks it, else the highest); the others are added from the top.
    static const u32 order[] = {0x20, 0x08, 0x04, 0x02, 0x01, 0x10};
    for (u32 bit : order) {
      if (__builtin_popcount(mask) >= (want > 6 ? 6 : want))
        break;
      mask |= bit;
    }
  }
  static std::atomic<bool> told{false};
  if (!told.exchange(true))
    std::fprintf(stderr,
                 "[audio] XAudio2 engine threads: the game asks for mask "
                 "0x%02X, running with 0x%02X (%d threads, "
                 "bd_audio_engine_threads)\n",
                 asked, mask, __builtin_popcount(mask));
  ctx.r3.u64 = mask;
  __imp__sub_82688DD8(ctx, base);
}
