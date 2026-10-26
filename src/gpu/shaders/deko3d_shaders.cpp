/**
 * @file    gpu/shaders/deko3d_shaders.cpp
 * @brief   Lookup of the deko3d back end's shaders (deko3d_shaders.h).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/shaders/deko3d_shaders.h"

#if defined(PLUME_DEKO3D)

#include <cstring>
#include <memory>
#include <mutex>

#include <miniz.h>

#include "core/logging.h"

namespace bd::gpu {

namespace {

std::once_flag g_once;
std::unique_ptr<unsigned char[]> g_pack;

const unsigned char *Pack() {
  std::call_once(g_once, [] {
    g_pack = std::make_unique<unsigned char[]>(g_deko3dPackSize);
    mz_ulong n = mz_ulong(g_deko3dPackSize);
    const int rc = mz_uncompress(g_pack.get(), &n, g_deko3dPackCompressed, mz_ulong(g_deko3dPackCompressedSize));
    if (rc != MZ_OK || n != g_deko3dPackSize) {
      BD_ERROR("deko3d: shader pack unpacking failed ({}, {} of {} bytes)", rc, size_t(n), g_deko3dPackSize);
      g_pack.reset();
    }
  });
  return g_pack.get();
}

} // namespace

bool Deko3DGameShaderBlob(uint64_t hash, const void *&data, size_t &size) {
  const unsigned char *pack = Pack();
  if (!pack)
    return false;
  // Sorted by hash (make_dk_shaders.py).
  size_t lo = 0, hi = g_deko3dGameShaderCount;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (g_deko3dGameShaders[mid].hash < hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == g_deko3dGameShaderCount || g_deko3dGameShaders[lo].hash != hash)
    return false;
  data = pack + g_deko3dGameShaders[lo].offset;
  size = g_deko3dGameShaders[lo].size;
  return true;
}

bool Deko3DHostShaderBlob(const char *name, const void *&data, size_t &size) {
  const unsigned char *pack = Pack();
  if (!pack)
    return false;
  for (size_t i = 0; i < g_deko3dHostShaderCount; ++i) {
    if (std::strcmp(g_deko3dHostShaders[i].name, name) == 0) {
      data = pack + g_deko3dHostShaders[i].offset;
      size = g_deko3dHostShaders[i].size;
      return true;
    }
  }
  return false;
}

bool Deko3DBuiltinDksh(unsigned uam_flags, std::vector<uint8_t> &records) {
  if (g_deko3dDkshCount == 0 || uam_flags != g_deko3dDkshFlags)
    return false;
  records.resize(g_deko3dDkshSize);
  mz_ulong n = mz_ulong(g_deko3dDkshSize);
  const int rc = mz_uncompress(records.data(), &n, g_deko3dDkshCompressed, mz_ulong(g_deko3dDkshCompressedSize));
  if (rc != MZ_OK || n != g_deko3dDkshSize) {
    BD_ERROR("deko3d: the build's DKSH cache did not unpack ({}, {} of {} bytes)", rc, size_t(n), g_deko3dDkshSize);
    records.clear();
    return false;
  }
  return true;
}

} // namespace bd::gpu

#endif // PLUME_DEKO3D
