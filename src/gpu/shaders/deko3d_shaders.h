/**
 * @file    gpu/shaders/deko3d_shaders.h
 * @brief   The deko3d back end's shaders: GLSL for uam, made by the host stage (docs/SWITCH-deko3d.md).
 *
 * tools/switch/deko3d/make_dk_shaders.py writes out/switch-host/deko3d_shaders.cpp: the game's shaders (by shader
 * cache hash) and re:Blue's own (by name) as plume Deko3DShaderBlob, zlib-compressed in one array. Only builds with
 * REBLUE_SWITCH_DEKO3D have it (PLUME_DEKO3D).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace bd::gpu {

struct Deko3DGameShader {
  uint64_t hash; // the shader cache entry's
  uint32_t offset, size;
};
struct Deko3DHostShader {
  const char *name; // src/gpu/shaders/hlsl/<name>.hlsl
  uint32_t offset, size;
};

extern const size_t g_deko3dPackSize;
extern const size_t g_deko3dPackCompressedSize;
extern const unsigned char g_deko3dPackCompressed[];
extern const Deko3DGameShader g_deko3dGameShaders[];
extern const size_t g_deko3dGameShaderCount;
extern const Deko3DHostShader g_deko3dHostShaders[];
extern const size_t g_deko3dHostShaderCount;

// A shader's blob (RenderShaderFormat::DEKO3D), or false. The pack is unpacked on the first call.
bool Deko3DGameShaderBlob(uint64_t hash, const void *&data, size_t &size);
bool Deko3DHostShaderBlob(const char *name, const void *&data, size_t &size);

} // namespace bd::gpu
