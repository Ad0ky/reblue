/**
 * @file    gpu/lookup_cache.h
 * @brief   Per-thread cache in front of a mutex-guarded u32-keyed map.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <atomic>

#include <rex/types.h>

namespace bd::gpu {

// The draw thread looks the same few resources up several times per draw,
// and on the Switch each lookup took a mutex and a hash probe (~6% of the
// draw thread in the field). Every change to the map bumps the generation
// (under the map's own lock), so a cached answer is used only while nothing
// was changed since it was looked up. Negative answers are cached too.
class LookupGeneration {
public:
  // Call with the map's lock held, after changing it.
  void Bump() {
    u32 next = gen_.load(std::memory_order_relaxed) + 1;
    if (next == 0)
      next = 1;
    gen_.store(next, std::memory_order_release);
  }
  u32 Load() const { return gen_.load(std::memory_order_acquire); }

private:
  std::atomic<u32> gen_{1};
};

template <typename V, u32 kSlots = 256> class LookupCache {
  static_assert((kSlots & (kSlots - 1)) == 0);

public:
  bool Find(u32 key, u32 gen, V *out) const {
    const Slot &s = slots_[Index(key)];
    if (s.gen != gen || s.key != key)
      return false;
    *out = s.value;
    return true;
  }
  void Put(u32 key, u32 gen, V value) {
    Slot &s = slots_[Index(key)];
    s.key = key;
    s.gen = gen;
    s.value = value;
  }

private:
  struct Slot {
    u32 key = 0;
    u32 gen = 0; // 0: empty (generations start at 1)
    V value{};
  };
  static u32 Index(u32 key) { return (key ^ (key >> 12)) >> 2 & (kSlots - 1); }
  Slot slots_[kSlots];
};

} // namespace bd::gpu
