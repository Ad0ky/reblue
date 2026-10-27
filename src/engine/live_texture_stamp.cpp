/**
 * @file    engine/live_texture_stamp.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "engine/live_texture_stamp.h"

#include <algorithm>

#include "gpu/gpu.h"

namespace bd::engine {

void LiveTextureStamp::Sync(
    const char *name, u32 generation,
    const std::function<std::vector<u8>()> &compose) {
  const u64 version = gpu::NativeTextureNamesVersion();
  if (seen_ && version == seen_version_ && generation == seen_generation_)
    return;
  const std::vector<gpu::NativeTextureRef> refs =
      gpu::NativeTexturesByName(name);

  for (auto it = stamped_.begin(); it != stamped_.end();) {
    const bool live = std::any_of(
        refs.begin(), refs.end(),
        [&](const gpu::NativeTextureRef &r) { return r.seq == it->first; });
    it = live ? std::next(it) : stamped_.erase(it);
  }

  std::vector<u8> blob;
  for (const gpu::NativeTextureRef &ref : refs) {
    const auto it = stamped_.find(ref.seq);
    if (it != stamped_.end() && it->second == generation)
      continue;
    if (blob.empty()) {
      blob = compose();
      if (blob.empty())
        return;
    }
    // Recorded even when the replace is refused: a geometry mismatch means the
    // instance was not loaded from this provider's art, and asking again next
    // tick cannot change that.
    gpu::NativeTextureReplace(ref.va, blob.data(), blob.size());
    stamped_[ref.seq] = generation;
  }
  seen_version_ = version;
  seen_generation_ = generation;
  seen_ = true;
}

} // namespace bd::engine
