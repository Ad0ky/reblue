/**
 * @file    gpu/render_queue.h
 * @brief   The render thread plan, stage 2 (docs/SWITCH.md): the renderer's
 *          work as a queue of records, run where it is issued (inline, the
 *          default) or by a render thread (bd_render_thread).
 *
 * The guest's D3D calls arrive on the game's threads. Inline, a record runs
 * at once on the calling thread, exactly as the call did before the queue.
 * Threaded, it is copied into a ring and the render thread runs the records
 * in the order they were issued, while the game goes on.
 *
 * What a record may do: anything the renderer did for that call, on host
 * objects and VideoState. What it must not do: read or write guest memory, or
 * produce a value the guest gets back. Those stay on the calling thread,
 * before the record is issued (the guest state a draw reads is captured,
 * the bytes a draw uploads are copied into the record).
 *
 * Calls not issued as records (the rest of the D3D hooks) call Sync() first,
 * so they find the renderer as it would have been inline: every record
 * issued before them has run.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include <rex/types.h>

namespace bd::gpu::rq {

namespace detail {
// Set once by Start(), before the game's threads issue anything.
extern bool g_threaded;
// Shutdown began: records are dropped, Sync returns at once.
extern std::atomic<bool> g_abandoned;

using InvokeFn = void (*)(void *closure, const u8 *payload, u32 size);

// Room in the calling thread's batch for a record of 'closure_size' +
// 'payload_size' bytes: returns where the closure goes (the payload follows
// it at payload_offset).
void *Append(u32 closure_size, u32 payload_size, InvokeFn invoke,
             u32 &payload_offset);
// Sends the batch on when it has grown large.
void AfterAppend();
// After a draw: sends the batch on every few draws (and wakes the render
// thread), not after each one: one lock, copy and wake per chunk.
void AfterDraw();
// Sends the calling thread's batch to the ring as one record. kick: wake
// the render thread now (a draw, a Sync).
void Flush(bool kick);

template <class F> void Invoke(void *closure, const u8 *payload, u32 size) {
  auto *f = static_cast<F *>(closure);
  if constexpr (std::is_invocable_v<F &, const u8 *, u32>)
    (*f)(payload, size);
  else
    (*f)();
  f->~F();
}
} // namespace detail

// bd_render_thread, as read at Start().
inline bool Threaded() { return detail::g_threaded; }
// True on the render thread itself.
bool OnRenderThread();

// Reads bd_render_thread and starts the thread when it is on. Once, after
// the GPU device is created and before the game draws.
void Start();
// Runs what is queued and stops the thread.
void Stop();
// Shutdown: from now on nothing is queued or waited for (the render thread
// may be parked in a record that waits on the thread shutting down).
void Abandon();

// Waits until every record issued so far has run. Inline, and on the render
// thread itself, it returns at once.
// 'site' names the caller in the report.
void Sync(const char *site = __builtin_FUNCTION());
inline void SyncIfThreaded(const char *site = __builtin_FUNCTION()) {
  if (detail::g_threaded)
    Sync(site);
}

// Runs f() now (inline) or queues it (threaded). Queued, f is copied into
// the calling thread's batch, which reaches the ring as one record with the
// next draw (RunDraw), at Sync, or when it fills up: the state set before a
// draw is only needed by that draw, and one record costs one lock instead of
// one per call. What f captures must stay valid until it runs (host objects
// do: they are destroyed a frame later, after the fence), and f must be
// trivially copyable (it is copied as bytes).
template <class F> void Run(F &&f) {
  using Fn = std::decay_t<F>;
  if (!detail::g_threaded || OnRenderThread()) {
    f();
    return;
  }
  static_assert(std::is_trivially_copyable_v<Fn>,
                "a queued record is copied as bytes");
  static_assert(alignof(Fn) <= 16);
  if (detail::g_abandoned.load(std::memory_order_relaxed))
    return;
  u32 payload_offset = 0;
  void *at = detail::Append(u32(sizeof(Fn)), 0, &detail::Invoke<Fn>,
                            payload_offset);
  new (at) Fn(std::forward<F>(f));
  detail::AfterAppend();
}

// Like Run, with 'size' bytes copied from 'bytes' now: f(const u8 *data,
// u32 size) gets the copy (inline, the original).
template <class F> void RunBytes(const void *bytes, u32 size, F &&f) {
  using Fn = std::decay_t<F>;
  if (!detail::g_threaded || OnRenderThread()) {
    f(static_cast<const u8 *>(bytes), size);
    return;
  }
  static_assert(std::is_trivially_copyable_v<Fn>,
                "a queued record is copied as bytes");
  if (detail::g_abandoned.load(std::memory_order_relaxed))
    return;
  u32 payload_offset = 0;
  void *at = detail::Append(u32(sizeof(Fn)), size, &detail::Invoke<Fn>,
                            payload_offset);
  new (at) Fn(std::forward<F>(f));
  if (size)
    std::memcpy(static_cast<u8 *>(at) + payload_offset, bytes, size);
  detail::AfterAppend();
}

// A draw, queued only (the caller records inline itself when not threaded):
// 'write' fills the record's 'size' payload bytes in place, f(const u8 *data,
// u32 size) gets them on the render thread. Every few draws the batch goes to
// the ring and the render thread is woken.
template <class W, class F> void RunDraw(u32 size, W &&write, F &&f) {
  using Fn = std::decay_t<F>;
  static_assert(std::is_trivially_copyable_v<Fn>,
                "a queued record is copied as bytes");
  if (detail::g_abandoned.load(std::memory_order_relaxed))
    return;
  u32 payload_offset = 0;
  void *at = detail::Append(u32(sizeof(Fn)), size, &detail::Invoke<Fn>,
                            payload_offset);
  new (at) Fn(std::forward<F>(f));
  write(static_cast<u8 *>(at) + payload_offset);
  detail::AfterDraw();
}

// Records issued and run since the last call, and the times a caller waited
// in Sync() (the profiler's report).
void Stats(u64 &issued, u64 &syncs, u64 &sync_us, u64 &full_waits);
// Microseconds callers have waited in Sync() since start (not reset).
u64 SyncMicrosTotal();
// The callers that waited most, per frame ("name n, ..."), since the last
// call.
std::string SyncSites(double frames);

} // namespace bd::gpu::rq

namespace bd::gpu {

struct GuestShader;

// What the issuing side needs of the state it set itself, kept by the Set*
// hooks on the thread that calls them: threaded, VideoState belongs to the
// render thread.
struct FrontState {
  GuestShader *vs = nullptr;
  GuestShader *ps = nullptr;
  // The size of the texture last bound to sampler 0 (a null bind keeps it, as
  // Video::SetTexture keeps the texture).
  u32 texture0Width = 0;
  u32 texture0Height = 0;
};
FrontState &Front();

} // namespace bd::gpu
