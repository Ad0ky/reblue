/**
 * @file    gpu/lookup_cache.h
 * @brief   Lock-free cache in front of a mutex-guarded u32-keyed map.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>
#include <cstring>
#include <type_traits>

#include <rex/types.h>

namespace bd::gpu {

// The draw thread looks the same resources up several times per draw, and on
// the Switch each lookup took the map's mutex and a hash probe (~8% of the draw
// thread in a busy area). This direct-mapped cache answers hits without the
// lock.
//
// Every slot mirrors the map for the key it holds: whoever changes the map
// (with the map's lock held) calls Set or Erase for that key, and a miss is
// filled with the lock held too, so a slot is never older than the map. Each
// slot is a seqlock: a reader that overlaps a write sees the sequence change
// and falls back to the map. Absent keys are cached as well.
template <typename V, u32 kSlots = 4096> class LookupCache {
  static_assert((kSlots & (kSlots - 1)) == 0);
  static_assert(std::is_trivially_copyable_v<V> && sizeof(V) <= sizeof(u64));

public:
  // Lock-free. True when the slot holds 'key': *present says whether the map
  // has it, and *out its value.
  bool Find(u32 key, bool *present, V *out) const {
    const Slot &s = slots_[Index(key)];
    const u32 seq = s.seq.load(std::memory_order_acquire);
    if (seq & 1u)
      return false;
    const u32 k = s.key.load(std::memory_order_relaxed);
    const u64 bits = s.value.load(std::memory_order_relaxed);
    const u32 p = s.present.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != seq || k != key || !key)
      return false;
    *present = p != 0;
    std::memcpy(out, &bits, sizeof(V));
    return true;
  }

  // With the map's lock held: the map now has key -> value (or, present =
  // false, does not have key).
  void Set(u32 key, bool present, V value) {
    Slot &s = slots_[Index(key)];
    u64 bits = 0;
    std::memcpy(&bits, &value, sizeof(V));
    const u32 seq = s.seq.load(std::memory_order_relaxed);
    s.seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.key.store(key, std::memory_order_relaxed);
    s.value.store(bits, std::memory_order_relaxed);
    s.present.store(present ? 1u : 0u, std::memory_order_relaxed);
    s.seq.store(seq + 2, std::memory_order_release);
  }

  // With the map's lock held, after removing key from the map.
  void Erase(u32 key) {
    const Slot &s = slots_[Index(key)];
    if (s.key.load(std::memory_order_relaxed) == key)
      Set(key, false, V{});
  }

private:
  struct Slot {
    std::atomic<u32> seq{0};
    std::atomic<u32> key{0}; // 0: empty (no map uses key 0)
    std::atomic<u64> value{0};
    std::atomic<u32> present{0};
  };
  static u32 Index(u32 key) { return (key * 2654435761u) >> (32 - Log2()); }
  static constexpr u32 Log2() {
    u32 n = 0;
    while ((1u << n) < kSlots)
      ++n;
    return n;
  }
  Slot slots_[kSlots];
};

} // namespace bd::gpu
