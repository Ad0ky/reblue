/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace bd::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
#if REBLUE_UBO_CONSTANTS
  // Set 5 over this chunk: the three constant blocks of a draw live in one
  // chunk and are picked out by dynamic offsets.
  std::unique_ptr<plume::RenderDescriptorSet> constantSet;
#endif
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, and the fetch constants almost never change between draws, so
// a 24-byte compare replaces them on the hot path. Sampler heap slots are never
// reclaimed, so a cached index stays valid until device teardown.
struct SamplerSlotCache {
  u8 fc[sizeof(DeviceFetchConstant)]{};
  u32 sampler = 0;
  i32 aniso = -1;
  bool clamp3d = false;
  bool valid = false;
};

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[16];
  float shadowPcfScale = 1.0f;
  bool sharedBound = false;
  bool ready = false;
#if REBLUE_UBO_CONSTANTS
  plume::RenderDescriptorSetBuilder constantSetBuilder;
  bool haveConstantSetBuilder = false;
  ConstantAllocation drawBlocks[3];
  bool drawBlocksChanged = true;
#endif
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

void RecomputeShadowPcfScale(UploadState &s) {
  const f64 dist = std::clamp(ShadowCoverageScale(), 1.0, 4.0);
  const f64 dim = std::max(512, Settings::Get().ShadowDimension());
  s.shadowPcfScale = static_cast<float>(std::max(1.0 / dist, 1024.0 / dim));
}

bool CreateChunk(UploadChunk &chunk) {
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  // One constant block of slack past the allocator's end: a uniform-buffer
  // binding reads a whole 4 KiB window from its offset (BindDrawConstants)
  // even when less was uploaded there.
  chunk.buffer = bd::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(
          kUploadChunkSize + kConstantBlockBytes, plume::RenderBufferFlag::CONSTANT |
                                plume::RenderBufferFlag::VERTEX |
                                plume::RenderBufferFlag::INDEX |
                                plume::RenderBufferFlag::DEVICE_ADDRESSABLE),
      "cb-upload-chunk");
  if (!chunk.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    BD_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = chunk.buffer->getDeviceAddress();
  return true;
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      BD_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so BD's
// degenerate constants (e.g. the bloom/glare 0/0 weight normalization when
// intensity is zero) are harmless on hardware. Our recompiled D3D12 shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant propagates and blackens the
// post-fx composite. No BD shader reinterprets a float constant register as
// int, so the flush cannot corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  u32 i = 0;
#if defined(__x86_64__) || defined(_M_X64)
  const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
  const __m128i inf_bits = _mm_set1_epi32(0x7F800000);
  for (; i + 4 <= count; i += 4) {
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
    v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    v = _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    v = _mm_shufflehi_epi16(v, _MM_SHUFFLE(2, 3, 0, 1));
    if constexpr (kFlushNaN) {
      const __m128i is_nan =
          _mm_cmpgt_epi32(_mm_and_si128(v, abs_mask), inf_bits);
      v = _mm_andnot_si128(is_nan, v);
    }
    _mm_storeu_si128(reinterpret_cast<__m128i *>(out + i), v);
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  // The Switch: the scalar loop was ~13% of the draw thread, and the stores
  // go to the upload chunk's write-combined memory, where 16-byte stores
  // matter as much as the swap.
  const uint32x4_t abs_mask = vdupq_n_u32(0x7FFFFFFFu);
  const uint32x4_t inf_bits = vdupq_n_u32(0x7F800000u);
  for (; i + 16 <= count; i += 16) {
    uint32x4_t v[4];
    for (int k = 0; k < 4; ++k) {
      v[k] = vreinterpretq_u32_u8(
          vrev32q_u8(vld1q_u8(reinterpret_cast<const u8 *>(src + i + 4 * k))));
      if constexpr (kFlushNaN)
        v[k] = vbicq_u32(v[k], vcgtq_u32(vandq_u32(v[k], abs_mask), inf_bits));
    }
    for (int k = 0; k < 4; ++k)
      vst1q_u32(out + i + 4 * k, v[k]);
  }
  for (; i + 4 <= count; i += 4) {
    uint32x4_t v = vreinterpretq_u32_u8(
        vrev32q_u8(vld1q_u8(reinterpret_cast<const u8 *>(src + i))));
    if constexpr (kFlushNaN)
      v = vbicq_u32(v, vcgtq_u32(vandq_u32(v, abs_mask), inf_bits));
    vst1q_u32(out + i, v);
  }
#endif
  for (; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() {
  auto &s = upload_state();
  s.sharedBound = false;
#if REBLUE_UBO_CONSTANTS
  // A new command list: nothing is bound, and the blocks recorded so far may
  // belong to a frame slot that is being rewound.
  for (auto &block : s.drawBlocks)
    block = {};
  s.drawBlocksChanged = true;
#endif
}

#if REBLUE_UBO_CONSTANTS
}  // namespace bd::gpu

REXCVAR_DEFINE_BOOL(bd_switch_ubo_constants, true, "Switch",
                    "Guest shader constants in uniform buffers (descriptor set "
                    "5, the GPU's constant banks) instead of device addresses: "
                    "~40% less GPU time in the field's main shader. Read at "
                    "start; delete cache/vulkan_pipelines.bin when changing it.");

namespace bd::gpu {

bool UboConstantsEnabled() {
  static const bool enabled = [] {
    const bool on = REXCVAR_GET(bd_switch_ubo_constants);
    BD_INFO("Switch: guest shader constants through {}",
            on ? "uniform buffers (set 5)" : "device addresses");
    std::fprintf(stderr, "[ubo] guest shader constants through %s\n",
                 on ? "uniform buffers (set 5)" : "device addresses");
    return on;
  }();
  return enabled;
}

namespace {

constexpr u32 kBankBytes[3] = {kConstantBlockBytes, kConstantBlockBytes,
                               u32(sizeof(SharedConstants))};

// The upload chunk of the current frame slot that holds 'alloc', or -1.
int ChunkOf(UploadState &s, const ConstantAllocation &alloc) {
  FrameUpload &up = s.frames[s.cursor];
  for (size_t i = 0; i < up.chunks.size(); ++i)
    if (up.chunks[i].buffer.get() == alloc.ref.ref)
      return int(i);
  return -1;
}

} // namespace

void AddConstantDescriptorSet(plume::RenderPipelineLayoutBuilder &layout) {
  auto &s = upload_state();
  s.constantSetBuilder.begin();
  for (u32 bank = 0; bank < 3; ++bank)
    s.constantSetBuilder.addDynamicConstantBuffer(bank);
  s.constantSetBuilder.end();
  s.haveConstantSetBuilder = true;
  layout.addDescriptorSet(s.constantSetBuilder);
}

void SetDrawConstants(ConstantBank bank, const ConstantAllocation &alloc) {
  auto &s = upload_state();
  if (!alloc.memory || bank > kSharedBank)
    return;
  s.drawBlocks[bank] = alloc;
  s.drawBlocksChanged = true;
}

// Why a bind did not happen, to stderr (written at once, unlike the log): a
// draw without set 5 reads constants from nowhere and can hang the GPU.
void BindFailed(const char *why) {
  static std::atomic<u32> count{0};
  const u32 n = count.fetch_add(1, std::memory_order_relaxed);
  if (n < 20 || (n & (n - 1)) == 0)
    std::fprintf(stderr, "[ubo] set 5 not bound (#%u): %s\n", n, why);
}

void BindDrawConstants(plume::RenderCommandList *list) {
  auto &s = upload_state();
  if (!s.drawBlocksChanged)
    return;
  if (!list || !s.haveConstantSetBuilder || !s.ready) {
    BindFailed(!list ? "no command list"
                     : !s.haveConstantSetBuilder ? "no set layout" : "not ready");
    return;
  }

  // A bank nothing was uploaded for yet reads zeros rather than leaving set 5
  // unbound for a pipeline that uses it.
  static const u8 kZeros[kConstantBlockBytes] = {};
  for (u32 bank = 0; bank < 3; ++bank) {
    if (s.drawBlocks[bank].memory)
      continue;
    ConstantAllocation zeros = Allocate(s, kBankBytes[bank], kCBVAlignment);
    if (!zeros.memory) {
      BindFailed("allocation failed");
      return;
    }
    std::memcpy(zeros.memory, kZeros, kBankBytes[bank]);
    s.drawBlocks[bank] = zeros;
  }

  // All three in the newest of their chunks: the others are copied there
  // (their bytes stay valid until the slot is rewound). Only at a chunk
  // boundary.
  for (int attempt = 0; attempt < 3; ++attempt) {
    int target = -1;
    for (const auto &block : s.drawBlocks)
      target = std::max(target, ChunkOf(s, block));
    if (target < 0) {
      BindFailed("blocks not in this frame's chunks");
      return;
    }
    bool moved = false;
    for (u32 bank = 0; bank < 3; ++bank) {
      if (ChunkOf(s, s.drawBlocks[bank]) == target)
        continue;
      ConstantAllocation copy = Allocate(s, kBankBytes[bank], kCBVAlignment);
      if (!copy.memory) {
        BindFailed("allocation failed");
        return;
      }
      std::memcpy(copy.memory, s.drawBlocks[bank].memory, kBankBytes[bank]);
      s.drawBlocks[bank] = copy;
      moved = true;
    }
    if (!moved)
      break;
  }
  const int chunkIndex = ChunkOf(s, s.drawBlocks[0]);
  if (chunkIndex < 0 || ChunkOf(s, s.drawBlocks[1]) != chunkIndex ||
      ChunkOf(s, s.drawBlocks[2]) != chunkIndex) {
    BindFailed("blocks in different chunks");
    return;
  }

  UploadChunk &chunk = s.frames[s.cursor].chunks[size_t(chunkIndex)];
  if (!chunk.constantSet) {
    auto *device = bd::gpu::Video::HostDevice();
    if (!device) {
      BindFailed("no device");
      return;
    }
    chunk.constantSet = s.constantSetBuilder.create(device);
    if (!chunk.constantSet) {
      BindFailed("createDescriptorSet failed");
      return;
    }
    std::fprintf(stderr, "[ubo] set 5 for slot %u chunk %d created\n",
                 s.cursor, chunkIndex);
    for (u32 bank = 0; bank < 3; ++bank)
      chunk.constantSet->setBuffer(bank, chunk.buffer.get(), kBankBytes[bank]);
  }
  const u32 offsets[3] = {u32(s.drawBlocks[0].ref.offset),
                          u32(s.drawBlocks[1].ref.offset),
                          u32(s.drawBlocks[2].ref.offset)};
  static std::atomic<u32> binds{0};
  if (binds.fetch_add(1, std::memory_order_relaxed) < 8)
    std::fprintf(stderr, "[ubo] bind set 5: slot %u chunk %d offsets %u %u %u\n",
                 s.cursor, chunkIndex, offsets[0], offsets[1], offsets[2]);
  list->setGraphicsDescriptorSetDynamic(chunk.constantSet.get(),
                                        kConstantDescriptorSetIndex, offsets,
                                        3);
  s.drawBlocksChanged = false;
}
#endif

// c50.xy is BD's NDC->UV half-scale (0.5 on hw). bd_blur_ps reconstructs its
// sample UV as uv = c50.xy*(ndc+1), so it MUST be 0.5. The guest derives it
// from sceneDim/1280x720*0.5, letting output res and supersampling leak in, and
// the pass oversamples until the source collapses into the top-left. bd_blur_ps
// is the ONLY pixel shader reading c50 as this screen->UV scale (verified
// across all 17 c50-using PS). The rest own c50 as material data, so a blanket
// pin would corrupt them (bd_lightshaft_ps's g_vLightShaftDiffuse -> gray
// god rays). bdCameraRefractionUvScaleHook pins device reg50 at the
// bdCameraRender writers, and this catches blur draws those writers miss.
constexpr u32 kScreenUVScaleRegByteOffset = 50 * 16;
constexpr u64 kBDBlurPSHash = 0xD94E164866C3B9BCull;
void PinScreenUVScaleReg(u8 *block) {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 h = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  if (h == kBDBlurPSHash) {
    auto *reg = reinterpret_cast<float *>(block + kScreenUVScaleRegByteOffset);
    reg[0] = 0.5f;
    reg[1] = 0.5f;
  }
}

u32 FloatConstantBytes(const GuestShader *shader) {
#if defined(__SWITCH__)
  if (shader && shader->shaderCacheEntry) {
    const size_t index = size_t(shader->shaderCacheEntry - g_shaderCacheEntries);
    if (index < g_shaderCacheEntryCount) {
      const u32 bytes = g_shaderCacheFloatConstantBytes[index];
      // PinScreenUVScaleReg writes a register of its own.
      if (shader->shaderCacheEntry->hash == kBDBlurPSHash)
        return std::max(bytes, kScreenUVScaleRegByteOffset + 16);
      return std::min(std::max(bytes, 16u), kConstantBlockBytes);
    }
  }
#else
  (void)shader;
#endif
  return kConstantBlockBytes;
}

// The allocation is always the whole 4 KiB block (the D3D12 root CBV and the
// uniform-buffer window read that much); only the first 'bytes' are written.
ConstantAllocation UploadVertexShaderConstants(u32 device_guest, u32 bytes) {
  BD_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, vsFloatConstants),
                         std::min(bytes, kConstantBlockBytes));
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest, u32 bytes) {
  BD_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, psFloatConstants),
                         std::min(bytes, kConstantBlockBytes));
  PinScreenUVScaleReg(alloc.memory);
  return alloc;
}

ConstantAllocation UploadSharedConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // bd_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: our SetTexture hook replaces BD's recompiled
  // body, so the engine's per-slot bound-texture shadow (device+0x2FF0+slot*4)
  // and GPU texture fetch constants (device+0x400+slot*0x18) are never written.
  // Video::SetTexture mirrors Xenos semantics (a null bind is ignored, since on
  // hardware it does not rebuild the fetch constant), so vs.textures holds the
  // last real texture per slot, exactly what the GPU would still be sampling.
  auto &vs = bd::gpu::state();
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  for (u32 i = 0; i < 16; ++i) {
    s.shared.samplerIndices[i] = 0;
    s.shared.texture2DIndices[i] = bd::gpu::kNullTexture2DDescriptorIndex;
    s.shared.texture3DIndices[i] = bd::gpu::kNullTexture3DDescriptorIndex;
    s.shared.textureCubeIndices[i] = bd::gpu::kNullTextureCubeDescriptorIndex;

    bd::gpu::GuestTexture *tex = vs.textures[i];
    if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
        tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        tex->sourceSurface != vs.render_target &&
        tex->sourceSurface != vs.depth_stencil &&
        tex->sourceSurface->descriptorIndex !=
            bd::gpu::kInvalidDescriptorIndex) {
      tex = tex->sourceSurface;
    }
    if (tex && tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex) {
      switch (tex->viewDimension) {
      case plume::RenderTextureViewDimension::TEXTURE_3D:
        s.shared.texture3DIndices[i] = tex->descriptorIndex;
        // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
        // volume as its slice-0 2D view too so tfetch2D samples the base layer.
        if (tex->companion2D && tex->companion2D->descriptorIndex !=
                                    bd::gpu::kInvalidDescriptorIndex) {
          s.shared.texture2DIndices[i] = tex->companion2D->descriptorIndex;
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_CUBE:
        s.shared.textureCubeIndices[i] = tex->descriptorIndex;
        break;
      case plume::RenderTextureViewDimension::TEXTURE_2D:
      case plume::RenderTextureViewDimension::UNKNOWN:
      default:
        s.shared.texture2DIndices[i] = tex->descriptorIndex;
        // BD static reflection cubes bind as a 2D atlas yet the water/glass
        // shader cube-fetches the slot, so publish the sliced TextureCube
        // companion so tfetchCube resolves a real cube, not the null cube.
        if (tex->companionCube && tex->companionCube->descriptorIndex !=
                                      bd::gpu::kInvalidDescriptorIndex) {
          s.shared.textureCubeIndices[i] = tex->companionCube->descriptorIndex;
        }
        break;
      }

      // X360 stores sampler state in fetchConstants[N].dword[*]. The
      // SetSamplerState_* setters are unhooked and run their recompiled bodies,
      // so the address mode bits there are valid.
      if (device_p) {
        const auto &fc_be = device_p->fetchConstants[i];
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto &sc = s.samplerSlots[i];
        if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
            std::memcmp(sc.fc, &fc_be, sizeof(sc.fc)) == 0) {
          s.shared.samplerIndices[i] = sc.sampler;
        } else {
          const u32 fc[6] = {
              u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
              u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
          };
          auto desc = DecodeFromFetch(fc);

          // Shell fur volumes encode shell depth in W, and X360-default WRAP
          // wraps a z=0 fetch's second tap to the tip slice and halves density,
          // so force CLAMP to keep both taps on the dense base slice.
          if (clamp3d) {
            desc.addressW = plume::RenderTextureAddressMode::CLAMP;
          }
          const u32 resolved = ResolveSlotLocked(desc);
          std::memcpy(sc.fc, &fc_be, sizeof(sc.fc));
          sc.sampler = resolved;
          sc.aniso = aniso_now;
          sc.clamp3d = clamp3d;
          sc.valid = true;
          s.shared.samplerIndices[i] = resolved;
        }
      }
    }
  }

  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (device_guest) {
    const auto *device = bd::mem::at<const D3DDevice>(device_guest);
    for (u32 i = 0; i < 4; ++i) {
      s.shared.booleansArr[i] = device ? u32(device->vsBoolConstants[i]) : 0u;
      s.shared.booleansArr[4 + i] =
          device ? u32(device->psBoolConstants[i]) : 0u;
    }
  }

  s.shared.alphaThreshold = bd::gpu::Video::AlphaThreshold();
  s.shared.halfPixelOffsetX = 0.0f;
  s.shared.halfPixelOffsetY = 0.0f;
  s.shared.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  s.shared.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  s.shared.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  s.shared.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  s.shared.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  s.shared.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  s.shared.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  s.shared.shadowPcfScale = s.shadowPcfScale;

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  s.shared.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

  // Byte-identical to the block already bound on this command list: the live
  // CBV is still correct, skip the upload and let the caller skip the rebind.
  // SharedConstants padding is zero-initialized and never written, so memcmp
  // is deterministic.
  if (s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0) {
    return {};
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  return alloc;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  BD_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    BD_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

} // namespace bd::gpu
