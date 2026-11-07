/**
 * @file    gpu/render_queue.cpp
 * @brief   The render queue (gpu/render_queue.h): a byte ring of records, any
 *          number of producers (under one lock), one consumer, the render
 *          thread.
 *
 * Record layout: a 16-byte header (total size, invoke function), the closure,
 * then the payload. A record never wraps: one that does not fit before the
 * end of the ring leaves a skip record there and starts again at 0.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/render_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>

#include "core/logging.h"
#include "gpu/constant_buffers.h"
#include "platform/switch_tuning.h"

REXCVAR_DEFINE_BOOL(bd_render_thread, true, "Switch",
                    "Record the draws on a render thread of their own (render "
                    "thread plan, docs/SWITCH.md). Off: the game's draw thread "
                    "records them as it issues them. Needs the deko3d in-place "
                    "constants (bd_deko3d_delta_constants); read at start-up.");
REXCVAR_DEFINE_INT32(bd_render_thread_priority, 0x2E, "Switch",
                     "Horizon priority of the render thread (0x2C highest an "
                     "application thread may take, 0x3B the game's threads).")
    .range(0x2C, 0x3B);
REXCVAR_DEFINE_INT32(bd_render_queue_mb, 8, "Switch",
                     "Size of the render queue in MB (the draws the game may "
                     "be ahead of the render thread).")
    .range(1, 64);

namespace bd::gpu::rq {

namespace detail {
bool g_threaded = false;
std::atomic<bool> g_abandoned{false};
} // namespace detail

namespace {

constexpr u32 kAlign = 16;
constexpr u32 kHeaderBytes = 16;

struct Header {
  u32 size;      // whole record, header included, a multiple of kAlign
  u32 payload;   // payload offset from the closure; 0 = skip record
  u32 payloadSize;
  u32 pad;
};
static_assert(sizeof(Header) == kHeaderBytes);

// The header is the first 16 bytes, the invoke pointer the next 16, so the
// closure keeps a 16-byte alignment.
constexpr u32 kRecordPrefix = kHeaderBytes * 2;

struct Ring {
  std::unique_ptr<u8[]> storage;
  u8 *data = nullptr;
  u64 capacity = 0;
  // Bytes written and read since the start (they only grow). The write
  // cursor moves under the producer lock; published is what the consumer may
  // read up to.
  u64 write = 0;
  std::atomic<u64> published{0};
  std::atomic<u64> consumed{0};
  // Records run (Sync compares it with the records issued).
  std::atomic<u64> issuedRecords{0};
  std::atomic<u64> doneRecords{0};

  std::mutex producer;
  std::mutex wake_mutex;
  std::condition_variable wake;    // the render thread: work arrived
  std::condition_variable drained; // Sync / a full ring: work done
  std::atomic<bool> sleeping{false};
  std::atomic<bool> stop{false};
  // Callers blocked in Sync or on a full ring: the render thread signals
  // 'drained' only when there are any (a signal is a system call).
  std::atomic<u32> waiters{0};
  // Records committed since the render thread was last woken.
  u32 sinceWake = 0;
  std::thread thread;
};

Ring g_ring;
thread_local bool t_render_thread = false;

// The record being written between Reserve and Commit.
u64 g_pending_end = 0;

std::atomic<u64> g_sync_us_total{0};
std::atomic<u64> g_stat_issued{0}, g_stat_syncs{0}, g_stat_sync_us{0},
    g_stat_full{0};

inline u32 AlignUp(u32 v) { return (v + kAlign - 1) & ~(kAlign - 1); }

void Notify() {
  if (g_ring.sleeping.load(std::memory_order_seq_cst)) {
    g_ring.sinceWake = 0;
    std::lock_guard lock(g_ring.wake_mutex);
    g_ring.wake.notify_one();
  }
}

void SignalDrained() {
  if (g_ring.waiters.load(std::memory_order_seq_cst) != 0) {
    std::lock_guard lock(g_ring.wake_mutex);
    g_ring.drained.notify_all();
  }
}

// Where the game waited (Sync callers), for the report.
struct SyncSite {
  std::atomic<const char *> name{nullptr};
  std::atomic<u64> count{0};
};
SyncSite g_sites[32];

void CountSite(const char *site) {
  for (auto &s : g_sites) {
    const char *n = s.name.load(std::memory_order_relaxed);
    if (n == nullptr) {
      const char *expected = nullptr;
      if (s.name.compare_exchange_strong(expected, site) || expected == site) {
        s.count.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      continue;
    }
    if (n == site) {
      s.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

void Loop() {
  t_render_thread = true;
#if defined(__SWITCH__)
  bd::platform::SpreadCurrentThread(-1);
  bd::platform::SetCurrentThreadPriority(
      REXCVAR_GET(bd_render_thread_priority));
#endif
  auto &r = g_ring;
  u64 read = r.consumed.load(std::memory_order_relaxed);
  for (;;) {
    u64 end = r.published.load(std::memory_order_acquire);
    if (read == end) {
      // Spin briefly before sleeping: a chunk often follows the last within
      // microseconds, and a sleep and a wake are two system calls. Not long:
      // the spin holds a core the game's threads could run on (the console
      // run spent 40% of the render thread spinning with 4096).
      for (int i = 0; i < 512 && read == end; ++i) {
#if defined(__aarch64__)
        asm volatile("yield");
#endif
        end = r.published.load(std::memory_order_acquire);
      }
    }
    if (read == end) {
      std::unique_lock lock(r.wake_mutex);
      r.sleeping.store(true, std::memory_order_seq_cst);
      end = r.published.load(std::memory_order_seq_cst);
      while (read == end && !r.stop.load(std::memory_order_relaxed)) {
        r.wake.wait(lock);
        end = r.published.load(std::memory_order_acquire);
      }
      r.sleeping.store(false, std::memory_order_relaxed);
      if (read == end)
        break; // stopping, nothing left
      continue;
    }
    while (read != end) {
      u8 *rec = r.data + (read % r.capacity);
      Header h;
      std::memcpy(&h, rec, kHeaderBytes);
      if (h.payload != 0) {
        detail::InvokeFn invoke;
        std::memcpy(&invoke, rec + kHeaderBytes, sizeof(invoke));
        u8 *closure = rec + kRecordPrefix;
        invoke(closure, closure + h.payload, h.payloadSize);
        r.doneRecords.fetch_add(1, std::memory_order_release);
      }
      read += h.size;
      r.consumed.store(read, std::memory_order_seq_cst);
    }
    SignalDrained();
  }
  {
    std::lock_guard lock(r.wake_mutex);
    r.drained.notify_all();
  }
}

void *Reserve(u32 closure_size, u32 payload_size, detail::InvokeFn invoke,
              u32 &payload_offset) {
  auto &r = g_ring;
  r.producer.lock();
  payload_offset = AlignUp(closure_size ? closure_size : 1);
  const u32 size = kRecordPrefix + payload_offset + AlignUp(payload_size);
  if (size > r.capacity / 4) {
    // Far beyond any draw; the queue was sized wrong.
    std::fprintf(stderr, "[render-queue] record of %u bytes, the ring is %llu\n",
                 size, static_cast<unsigned long long>(r.capacity));
    std::abort();
  }
  u64 at = r.write;
  const u64 offset = at % r.capacity;
  const bool wrap = offset + size > r.capacity;
  const u64 need = (wrap ? r.capacity - offset : 0) + size;
  // Wait for room.
  if (at + need - r.consumed.load(std::memory_order_acquire) > r.capacity) {
    g_stat_full.fetch_add(1, std::memory_order_relaxed);
    Notify();
    r.waiters.fetch_add(1, std::memory_order_seq_cst);
    {
      std::unique_lock lock(r.wake_mutex);
      while (at + need - r.consumed.load(std::memory_order_seq_cst) >
             r.capacity) {
        r.drained.wait_for(lock, std::chrono::milliseconds(1));
      }
    }
    r.waiters.fetch_sub(1, std::memory_order_relaxed);
  }
  if (wrap) {
    Header skip{};
    skip.size = u32(r.capacity - offset);
    skip.payload = 0;
    std::memcpy(r.data + offset, &skip, kHeaderBytes);
    at += skip.size;
  }
  u8 *rec = r.data + (at % r.capacity);
  Header h{};
  h.size = size;
  h.payload = payload_offset;
  h.payloadSize = payload_size;
  std::memcpy(rec, &h, kHeaderBytes);
  std::memcpy(rec + kHeaderBytes, &invoke, sizeof(invoke));
  g_pending_end = at + size;
  return rec + kRecordPrefix;
}

void Commit(bool kick) {
  auto &r = g_ring;
  r.write = g_pending_end;
  r.issuedRecords.fetch_add(1, std::memory_order_relaxed);
  r.published.store(r.write, std::memory_order_seq_cst);
  // A sleeping render thread is woken for a draw (kick) or once enough state
  // records piled up, not for each record: a wake is two system calls, and
  // the state set before a draw is only needed by the draw.
  const bool wake = kick || ++r.sinceWake >= 64;
  if (wake)
    Notify();
  r.producer.unlock();
}


// A thread's records not sent to the ring yet. Each one: a 32-byte prefix
// (size, payload offset, payload size, invoke), the closure, the payload.
struct Batch {
  std::unique_ptr<u8[]> storage;
  u8 *data = nullptr;
  u32 capacity = 0;
  u32 used = 0;
  u32 records = 0;
  u32 draws = 0; // since the batch was last sent
};
thread_local Batch t_batch;
constexpr u32 kBatchSend = 32 * 1024; // sent on when it grows past this
constexpr u32 kDrawsPerSend = 16;      // or after this many draws

struct SubHeader {
  u32 size;
  u32 payload;
  u32 payloadSize;
  u32 pad;
};

void GrowBatch(Batch &b, u32 need) {
  u32 cap = b.capacity ? b.capacity : 64 * 1024;
  while (cap < need)
    cap *= 2;
  std::unique_ptr<u8[]> storage(new u8[cap + kAlign]);
  auto addr = reinterpret_cast<uintptr_t>(storage.get());
  u8 *data =
      reinterpret_cast<u8 *>((addr + kAlign - 1) & ~uintptr_t(kAlign - 1));
  if (b.used)
    std::memcpy(data, b.data, b.used);
  b.storage = std::move(storage);
  b.data = data;
  b.capacity = cap;
}

// The ring record a batch travels in: runs its records in order.
void RunBatch(void * /*closure*/, const u8 *payload, u32 size) {
  u32 at = 0;
  while (at < size) {
    SubHeader h;
    std::memcpy(&h, payload + at, sizeof(h));
    detail::InvokeFn invoke;
    std::memcpy(&invoke, payload + at + kHeaderBytes, sizeof(invoke));
    u8 *closure = const_cast<u8 *>(payload) + at + kRecordPrefix;
    invoke(closure, closure + h.payload, h.payloadSize);
    at += h.size;
  }
}
} // namespace

bool OnRenderThread() { return t_render_thread; }

namespace detail {

void *Append(u32 closure_size, u32 payload_size, InvokeFn invoke,
             u32 &payload_offset) {
  auto &b = t_batch;
  payload_offset = AlignUp(closure_size ? closure_size : 1);
  const u32 size = kRecordPrefix + payload_offset + AlignUp(payload_size);
  if (b.used + size > b.capacity)
    GrowBatch(b, b.used + size);
  u8 *rec = b.data + b.used;
  SubHeader h{size, payload_offset, payload_size, 0};
  std::memcpy(rec, &h, sizeof(h));
  std::memcpy(rec + kHeaderBytes, &invoke, sizeof(invoke));
  b.used += size;
  ++b.records;
  return rec + kRecordPrefix;
}

void AfterAppend() {
  if (t_batch.used >= kBatchSend)
    Flush(false);
}

void AfterDraw() {
  auto &b = t_batch;
  if (++b.draws >= kDrawsPerSend || b.used >= kBatchSend)
    Flush(true);
}

void Flush(bool kick) {
  auto &b = t_batch;
  if (!b.used) {
    if (kick) {
      std::lock_guard lock(g_ring.producer);
      Notify();
    }
    return;
  }
  u32 payload_offset = 0;
  u8 *at = static_cast<u8 *>(Reserve(0, b.used, &RunBatch, payload_offset));
  std::memcpy(at + payload_offset, b.data, b.used);
  Commit(kick);
  g_stat_issued.fetch_add(b.records, std::memory_order_relaxed);
  b.used = 0;
  b.records = 0;
  b.draws = 0;
}

} // namespace detail


void Start() {
  if (g_ring.thread.joinable())
    return;
  bool want = REXCVAR_GET(bd_render_thread);
#if REBLUE_UBO_CONSTANTS
  const bool delta = DeltaConstantsEnabled();
#else
  const bool delta = false;
#endif
  if (want && !delta) {
    BD_WARN("[render-queue] bd_render_thread needs the deko3d in-place "
            "constants: drawing inline");
    want = false;
  }
  if (!want) {
    BD_INFO("[render-queue] draws recorded inline, on the thread that issues "
            "them (bd_render_thread off)");
    return;
  }
  const u64 bytes = u64(REXCVAR_GET(bd_render_queue_mb)) << 20;
  g_ring.storage.reset(new (std::nothrow) u8[bytes + kAlign]);
  if (!g_ring.storage) {
    BD_WARN("[render-queue] no memory for a {} MB queue: drawing inline",
            bytes >> 20);
    return;
  }
  auto addr = reinterpret_cast<uintptr_t>(g_ring.storage.get());
  g_ring.data = reinterpret_cast<u8 *>((addr + kAlign - 1) & ~uintptr_t(kAlign - 1));
  g_ring.capacity = bytes;
  detail::g_threaded = true;
  g_ring.thread = std::thread(Loop);
  BD_INFO("[render-queue] render thread on: {} MB queue, priority {:#x}",
          bytes >> 20, REXCVAR_GET(bd_render_thread_priority));
}

void Stop() {
  if (!g_ring.thread.joinable())
    return;
  Sync();
  g_ring.stop.store(true, std::memory_order_seq_cst);
  {
    std::lock_guard lock(g_ring.wake_mutex);
    g_ring.wake.notify_all();
  }
  g_ring.thread.join();
  // Anything issued from here on runs inline.
  detail::g_threaded = false;
}

void Abandon() { detail::g_abandoned.store(true, std::memory_order_release); }

void Sync(const char *site) {
  if (!detail::g_threaded || t_render_thread ||
      detail::g_abandoned.load(std::memory_order_acquire))
    return;
  // This thread's own records first.
  detail::Flush(false);
  auto &r = g_ring;
  // Everything published so far; a record still being written by another
  // producer is not this caller's to wait for.
  const u64 target = r.published.load(std::memory_order_acquire);
  if (r.consumed.load(std::memory_order_acquire) >= target)
    return;
  const auto t0 = std::chrono::steady_clock::now();
  CountSite(site);
  {
    std::lock_guard lock(r.producer);
    Notify();
  }
  // Spin first: the render thread is usually a few records behind.
  for (int i = 0; i < 2048; ++i) {
    if (r.consumed.load(std::memory_order_acquire) >= target)
      break;
#if defined(__aarch64__)
    asm volatile("yield");
#endif
  }
  if (r.consumed.load(std::memory_order_acquire) < target) {
    r.waiters.fetch_add(1, std::memory_order_seq_cst);
    {
      std::unique_lock lock(r.wake_mutex);
      while (r.consumed.load(std::memory_order_seq_cst) < target &&
             !detail::g_abandoned.load(std::memory_order_acquire))
        r.drained.wait_for(lock, std::chrono::milliseconds(1));
    }
    r.waiters.fetch_sub(1, std::memory_order_relaxed);
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  g_stat_syncs.fetch_add(1, std::memory_order_relaxed);
  g_stat_sync_us.fetch_add(u64(us), std::memory_order_relaxed);
  g_sync_us_total.fetch_add(u64(us), std::memory_order_relaxed);
}

u64 SyncMicrosTotal() { return g_sync_us_total.load(std::memory_order_relaxed); }

void Stats(u64 &issued, u64 &syncs, u64 &sync_us, u64 &full_waits) {
  issued = g_stat_issued.exchange(0, std::memory_order_relaxed);
  syncs = g_stat_syncs.exchange(0, std::memory_order_relaxed);
  sync_us = g_stat_sync_us.exchange(0, std::memory_order_relaxed);
  full_waits = g_stat_full.exchange(0, std::memory_order_relaxed);
}

std::string SyncSites(double frames) {
  struct Entry {
    const char *name;
    u64 count;
  };
  Entry list[std::size(g_sites)];
  size_t n = 0;
  for (auto &s : g_sites) {
    const char *name = s.name.load(std::memory_order_relaxed);
    if (!name)
      break;
    const u64 c = s.count.exchange(0, std::memory_order_relaxed);
    if (c)
      list[n++] = {name, c};
  }
  std::sort(list, list + n,
            [](const Entry &a, const Entry &b) { return a.count > b.count; });
  std::string out;
  for (size_t i = 0; i < n && i < 6; ++i) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s%s %.1f", out.empty() ? "" : ", ",
                  list[i].name, double(list[i].count) / frames);
    out += buf;
  }
  return out.empty() ? "none" : out;
}

} // namespace bd::gpu::rq

namespace bd::gpu {
FrontState &Front() {
  static FrontState front;
  return front;
}
} // namespace bd::gpu
