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
#include "gpu/render_queue.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"

#if defined(PLUME_DEKO3D)
#include <plume_deko3d.h>
#endif

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
  // DeltaConstantsEnabled: one uniform buffer holding the vertex bank then the
  // pixel bank, updated in place by the command list; the bytes the GPU will
  // hold after the updates recorded so far, and how many of them are known.
  std::unique_ptr<plume::RenderBuffer> deltaBuffer;
  u64 deltaGpu = 0;
  bool deltaFailed = false;
  alignas(16) u8 deltaShadow[2][kConstantBlockBytes];
  // The guest's registers as the capture last sent them (big-endian): what
  // a register the GPU copy lacks is rebuilt from (ApplyDeltaConstants).
  // Inline: the guest's own big-endian bytes of what deltaShadow holds
  // swapped, compared in this form (two plain loads, no byte swap).
  alignas(16) u8 deltaRaw[2][kConstantBlockBytes];
  alignas(16) u8 deltaMirror[2][kConstantBlockBytes];
  u64 deltaPending[2][4] = {}; // mirror registers changed, not pushed yet
  u32 deltaValid[2] = {0, 0};
  bool deltaPinned = false;  // c50 of the pixel bank holds the blur's 0.5
  // The shared block in place as well (third bank, after the two 4 KiB ones).
  alignas(16) SharedConstants deltaShared{};
  bool deltaSharedValid = false;
#endif
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

// Bumped by InvalidateSharedBinding (a new command list): the GPU copy is
// rebuilt from scratch, so the capture (CaptureDeltaConstants) refreshes
// every register once too, which also picks up a register a writer changed
// without a mark.
std::atomic<u32> g_delta_generation{0};

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
void CopyByteSwap32Host(u8 *dst, const u32 *src, u32 size) {
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

template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  CopyByteSwap32Host<kFlushNaN>(dst, src, size);
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
  // The in-place banks: the first draw of the list pushes what its shaders
  // read in full, so a list that never reaches the GPU cannot leave the copy
  // the CPU compares against wrong.
  for (u32 &valid : s.deltaValid)
    valid = 0;
  s.deltaSharedValid = false;
  g_delta_generation.fetch_add(1, std::memory_order_release);
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
    const bool on = REXCVAR_GET(bd_switch_ubo_constants) || UsingDeko3D();
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

#if REBLUE_UBO_CONSTANTS
}  // namespace bd::gpu

REXCVAR_DEFINE_BOOL(bd_deko3d_delta_constants, true, "Switch",
                    "deko3d: the vertex and pixel shader constants stay in one "
                    "uniform buffer per stage that each draw updates in place, "
                    "only the registers that changed, instead of a new 4 KiB "
                    "block per stage and draw. Read at start.");

namespace bd::gpu {

bool DeltaConstantsEnabled() {
#if defined(PLUME_DEKO3D)
  static const bool enabled = [] {
    const bool on = UsingDeko3D() && UboConstantsEnabled() &&
                    REXCVAR_GET(bd_deko3d_delta_constants);
    std::fprintf(stderr, "[ubo] vertex/pixel constants: %s\n",
                 on ? "updated in place (bd_deko3d_delta_constants)"
                    : "a new block per draw");
    return on;
  }();
  return enabled;
#else
  return false;
#endif
}

#if defined(PLUME_DEKO3D)
namespace {

constexpr u32 kSharedBlockBytes = 512;  // sizeof(SharedConstants), rounded
static_assert(sizeof(SharedConstants) <= kSharedBlockBytes);

bool EnsureDeltaBuffer(UploadState &s) {
  if (s.deltaGpu)
    return true;
  if (s.deltaFailed)
    return false;
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  s.deltaBuffer = bd::gpu::CreateHostBuffer(
      device,
      plume::RenderBufferDesc::UploadBuffer(
          2 * kConstantBlockBytes + kSharedBlockBytes,
          plume::RenderBufferFlag::CONSTANT |
                                       plume::RenderBufferFlag::DEVICE_ADDRESSABLE),
      "cb-delta-banks");
  if (!s.deltaBuffer) {
    s.deltaFailed = true;
    std::fprintf(stderr, "[ubo] in-place constant buffer: createBuffer failed\n");
    return false;
  }
  s.deltaGpu = s.deltaBuffer->getDeviceAddress();
  return s.deltaGpu != 0;
}

// One register from the guest's big-endian copy: byte swapped, NaN -> +0 (as
// CopyByteSwap32FlushNaN).
inline void LoadGuestRegister(u8 *out, const u8 *guest) {
#if defined(__aarch64__) || defined(_M_ARM64)
  uint32x4_t v = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(guest)));
  v = vbicq_u32(v, vcgtq_u32(vandq_u32(v, vdupq_n_u32(0x7FFFFFFFu)),
                             vdupq_n_u32(0x7F800000u)));
  vst1q_u8(out, vreinterpretq_u8_u32(v));
#else
  for (int i = 0; i < 4; ++i) {
    u32 w;
    std::memcpy(&w, guest + 4 * i, 4);
    w = __builtin_bswap32(w);
    if ((w & 0x7FFFFFFFu) > 0x7F800000u)
      w = 0;
    std::memcpy(out + 4 * i, &w, 4);
  }
#endif
}

inline bool SameRegister(const u8 *a, const u8 *b) {
  u64 x[2], y[2];
  std::memcpy(x, a, 16);
  std::memcpy(y, b, 16);
  return ((x[0] ^ y[0]) | (x[1] ^ y[1])) == 0;
}

// One register copied as one 16-byte load and store. memcpy(.., 16) becomes
// two 8-byte stores, and a 16-byte load of the copy soon after cannot be
// forwarded from them on the Cortex-A57 (it waits for both to reach the
// cache): the mirror and the capture are read back that way.
inline void CopyRegister(u8 *dst, const u8 *src) {
#if defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(dst, vld1q_u8(src));
#else
  std::memcpy(dst, src, 16);
#endif
}

#if defined(__aarch64__) || defined(_M_ARM64)
// The lowered register kept in a vector register: LoadGuestRegister stored it
// to the stack as 16 bytes and SameRegister read it back as two 8-byte loads,
// which the Cortex-A57 cannot forward from the store (the stall was the
// hottest instruction of PushDeltaConstants on the console).
inline uint8x16_t LoadGuestRegisterVec(const u8 *guest) {
  uint32x4_t v = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(guest)));
  v = vbicq_u32(v, vcgtq_u32(vandq_u32(v, vdupq_n_u32(0x7FFFFFFFu)),
                             vdupq_n_u32(0x7F800000u)));
  return vreinterpretq_u8_u32(v);
}
inline bool SameRegisterVec(uint8x16_t a, const u8 *b) {
  const uint64x2_t x = vreinterpretq_u64_u8(veorq_u8(a, vld1q_u8(b)));
  return (vgetq_lane_u64(x, 0) | vgetq_lane_u64(x, 1)) == 0;
}
#endif

// Registers the engine writes straight into the device copy, not through the
// D3D setters (state.cpp, tweaks.cpp): checked on every draw, since some of
// those writers run between draws inside one hooked function.
// c53 and c57 were found by the sampled check on the console (foliage sway:
// bushes and trees flickered without them).
constexpr u8 kAlwaysVertex[] = {20, 21, 50, 51, 53, 57};
constexpr u8 kAlwaysPixel[] = {3, 21, 50, 53};

struct DeltaDirty {
  std::atomic<u64> bits[4]{};
};
// Marks from the thread that draws (the setters run on it): plain words, no
// atomic read-modify-write per draw (an exclusive-store loop each on the
// Cortex-A57, ~1.6% of the Draw Thread). Marks from any other thread go to
// g_delta_dirty, which a draw only takes apart when something is in it.
u64 g_delta_local[2][4];
thread_local bool t_delta_draw = false;
DeltaDirty g_delta_dirty[2];
// Registers the sampled check caught changing without a setter: checked on
// every draw from then on, so a writer missing from the lists above costs at
// most a few frames of a stale value.
DeltaDirty g_delta_learned[2];

std::atomic<u64> g_delta_pushes{0}, g_delta_bytes{0}, g_delta_draws{0},
    g_delta_regs{0}, g_delta_misses{0};

} // namespace
#endif

void MarkConstantRegisters(ConstantBank bank, u32 start, u32 count) {
#if defined(PLUME_DEKO3D)
  if (bank > kPixelBank || start >= kConstantRegisterCount || !count)
    return;
  const u32 end = std::min(start + count, kConstantRegisterCount);
  auto &d = g_delta_dirty[bank];
  for (u32 w = start / 64; w <= (end - 1) / 64; ++w) {
    const u32 lo = std::max(start, w * 64) - w * 64;
    const u32 hi = std::min(end, w * 64 + 64) - w * 64;
    const u64 mask = (hi - lo == 64) ? ~0ull : (((1ull << (hi - lo)) - 1) << lo);
    if (t_delta_draw)
      g_delta_local[bank][w] |= mask;
    else
      d.bits[w].fetch_or(mask, std::memory_order_relaxed);
  }
#else
  (void)bank;
  (void)start;
  (void)count;
#endif
}

namespace {
// The capture half's own state (render thread plan, stage 2): it runs on the
// thread that issues the draw, the apply half on the one that records it,
// so they share nothing but the generation below.
struct DeltaCaptureState {
  // The guest's big-endian bytes of each register as last captured: a
  // register is compared in this form (two plain loads, no byte swap).
  alignas(16) u8 raw[2][kConstantBlockBytes];
  // Registers below this were captured once since the last full refresh.
  u32 known[2] = {0, 0};
  u32 seenGeneration = 0;
  u32 verifyTick[2] = {0, 0};
};
DeltaCaptureState g_capture;
} // namespace

bool CaptureDeltaConstants(ConstantBank bank, u32 device_guest, u32 bytes,
                           const GuestShader *shader, DeltaCapture &out) {
#if defined(PLUME_DEKO3D)
  BD_CPU_ZONE("CaptureDeltaConstants");
  out.regs = 0;
  out.count = 0;
  if (!device_guest || bank > kPixelBank)
    return false;
  const u32 regs = (std::min(bytes, kConstantBlockBytes) + 15u) / 16u;
  const u8 *guest = bd::mem::at<const u8>(
      device_guest + u32(bank == kVertexBank
                             ? offsetof(D3DDevice, vsFloatConstants)
                             : offsetof(D3DDevice, psFloatConstants)));
  if (!guest)
    return false;
  auto &c = g_capture;
  const u32 generation = g_delta_generation.load(std::memory_order_acquire);
  if (generation != c.seenGeneration) {
    c.seenGeneration = generation;
    c.known[0] = c.known[1] = 0;
  }

  // The registers to look at: the ones the setters touched since they were
  // last sent, the always-checked ones, and all not captured since the last
  // refresh (after a new command list, or a shader reading further).
  u64 want[4];
  auto &dirty = g_delta_dirty[bank];
  t_delta_draw = true;
  for (u32 w = 0; w < 4; ++w) {
    const u32 lo = w * 64;
    if (lo >= regs) {
      want[w] = 0;
      continue;
    }
    const u64 inRange =
        regs - lo >= 64 ? ~0ull : ((1ull << (regs - lo)) - 1);
    // Bits past the shader's range stay set for a later shader that reads them.
    want[w] = g_delta_local[bank][w] & inRange;
    g_delta_local[bank][w] &= ~inRange;
    if (dirty.bits[w].load(std::memory_order_relaxed) & inRange)
      want[w] |= dirty.bits[w].fetch_and(~inRange, std::memory_order_relaxed) &
                 inRange;
  }
  const u32 known = c.known[bank];
  for (u32 r = known; r < regs; ++r)
    want[r / 64] |= 1ull << (r % 64);
  const u8 *always = bank == kVertexBank ? kAlwaysVertex : kAlwaysPixel;
  const u32 nAlways = bank == kVertexBank ? std::size(kAlwaysVertex)
                                          : std::size(kAlwaysPixel);
  for (u32 i = 0; i < nAlways; ++i)
    if (always[i] < regs)
      want[always[i] / 64] |= 1ull << (always[i] % 64);
  for (u32 w = 0; w < 4 && w * 64 < regs; ++w) {
    const u32 lo = w * 64;
    const u64 inRange = regs - lo >= 64 ? ~0ull : ((1ull << (regs - lo)) - 1);
    want[w] |= g_delta_learned[bank].bits[w].load(std::memory_order_relaxed) &
               inRange;
  }

  // Sampled check of the whole range: a register that changed without being
  // marked is a writer the lists above miss; it is learned (g_delta_learned).
  // One call in 32 for the first 4096 checks, one in 256 after (the console
  // runs found their writers within seconds; the full read was ~2% of the
  // draw thread).
  const u32 tick = ++c.verifyTick[bank];
  const bool verify = (tick & (tick < 131072u ? 31u : 255u)) == 0;

  u8 *raw = c.raw[bank];
  u32 count = 0, looked = 0;
  for (u32 r = 0; r < regs; ++r) {
    const bool marked = (want[r / 64] >> (r % 64)) & 1u;
    if (!marked && !verify) {
      // Skip to the next marked register.
      u32 w = r / 64;
      u64 rest = want[w] & (~0ull << (r % 64));
      while (!rest && ++w < 4)
        rest = want[w];
      if (!rest)
        break;
      r = w * 64 + u32(__builtin_ctzll(rest)) - 1;
      continue;
    }
    ++looked;
    const u8 *gr = guest + r * 16;
    if (r < known && SameRegister(gr, raw + r * 16))
      continue;
    if (!marked) {
      // Changed with no mark: from now on looked at on every draw.
      g_delta_misses.fetch_add(1, std::memory_order_relaxed);
      const u64 bit = 1ull << (r % 64);
      const u64 before = g_delta_learned[bank].bits[r / 64].fetch_or(
          bit, std::memory_order_relaxed);
      if (!(before & bit)) {
        const u64 h =
            (shader && shader->shaderCacheEntry) ? shader->shaderCacheEntry->hash
                                                 : 0;
        std::fprintf(stderr,
                     "[ubo] in-place constants: %s c%u changed without a "
                     "setter (shader %016llX): checked on every draw from now\n",
                     bank == kVertexBank ? "VS" : "PS", r,
                     static_cast<unsigned long long>(h));
      }
    }
    CopyRegister(raw + r * 16, gr);
    out.reg[count] = u16(r);
    CopyRegister(out.values[count], gr);
    ++count;
  }
  c.known[bank] = std::max(known, regs);
  out.regs = u16(regs);
  out.count = u16(count);
  g_delta_regs.fetch_add(looked, std::memory_order_relaxed);
  return true;
#else
  (void)bank;
  (void)device_guest;
  (void)bytes;
  (void)shader;
  (void)out;
  return false;
#endif
}

void ConstantsWritten(ConstantBank bank, u32 device_guest, u32 start,
                      u32 count) {
#if defined(PLUME_DEKO3D)
  // Inline, or a thread that does not draw (its batch would wait for a draw
  // of its own): the marks, as before.
  if (!rq::Threaded() || !t_delta_draw || bank > kPixelBank ||
      start >= kConstantRegisterCount || !count) {
    MarkConstantRegisters(bank, start, count);
    return;
  }
  count = std::min(count, kConstantRegisterCount - start);
  const u8 *guest = bd::mem::at<const u8>(
      device_guest + u32(bank == kVertexBank
                             ? offsetof(D3DDevice, vsFloatConstants)
                             : offsetof(D3DDevice, psFloatConstants)) +
      start * 16);
  if (!guest) {
    MarkConstantRegisters(bank, start, count);
    return;
  }
  // Only the runs of registers that differ from what was last sent (the game
  // sets many to the values they already hold): each run is one record. The
  // capture's copy (what it compares with, and what was sent) takes them too,
  // so its sampled check does not take them for writes without a setter.
  u8 *raw = g_capture.raw[bank] + start * 16;
  u32 i = 0;
  while (i < count) {
    if (SameRegister(guest + i * 16, raw + i * 16)) {
      ++i;
      continue;
    }
    u32 end = i + 1;
    while (end < count && !SameRegister(guest + end * 16, raw + end * 16))
      ++end;
    const u32 run_bytes = (end - i) * 16;
    std::memcpy(raw + i * 16, guest + i * 16, run_bytes);
    const u32 first = start + i;
    rq::RunBytes(guest + i * 16, run_bytes,
                 [bank, first](const u8 *data, u32 size) {
                   auto &s = upload_state();
                   u8 *mirror = s.deltaMirror[bank];
                   const u32 n = size / 16;
                   for (u32 k = 0; k < n; ++k) {
                     const u32 r = first + k;
                     CopyRegister(mirror + r * 16, data + k * 16);
                     s.deltaPending[bank][r / 64] |= 1ull << (r % 64);
                   }
                 });
    i = end;
  }
#else
  MarkConstantRegisters(bank, start, count);
  (void)device_guest;
#endif
}

void AbsorbDeltaConstants(ConstantBank bank, const DeltaCaptureView &cap) {
#if defined(PLUME_DEKO3D)
  if (bank > kPixelBank)
    return;
  auto &s = upload_state();
  // The guest's registers as captured so far (raw): the captured ones are
  // new, the rest still hold what the guest had when they were captured.
  // Pending until a draw pushes them: a capture whose draw is dropped (no
  // shader, no target) must still reach the GPU copy with the next one.
  u8 *mirror = s.deltaMirror[bank];
  for (u32 i = 0; i < cap.count; ++i) {
    const u32 r = cap.reg[i];
    CopyRegister(mirror + r * 16, cap.values + i * 16);
    s.deltaPending[bank][r / 64] |= 1ull << (r % 64);
  }
#else
  (void)bank;
  (void)cap;
#endif
}

bool ApplyDeltaConstants(plume::RenderCommandList *list, ConstantBank bank,
                         const DeltaCaptureView &cap) {
#if defined(PLUME_DEKO3D)
  BD_CPU_ZONE("ApplyDeltaConstants");
  AbsorbDeltaConstants(bank, cap);
  auto &s = upload_state();
  if (!list || bank > kPixelBank || !EnsureDeltaBuffer(s))
    return false;
  const u32 regs = cap.regs;
  const u32 known = s.deltaValid[bank] / 16;
  u8 *mirror = s.deltaMirror[bank];

  // The captured registers not pushed yet, within the shader's range (the
  // rest stay pending for a shader that reads further).
  u64 todo[4];
  for (u32 w = 0; w < 4; ++w) {
    const u32 lo = w * 64;
    const u64 inRange =
        lo >= regs ? 0ull : (regs - lo >= 64 ? ~0ull : ((1ull << (regs - lo)) - 1));
    todo[w] = s.deltaPending[bank][w] & inRange;
    s.deltaPending[bank][w] &= ~inRange;
  }
  // All that the GPU copy does not hold yet (a new command list, a shader
  // reading further), from the mirror.
  for (u32 r = known; r < regs; ++r)
    todo[r / 64] |= 1ull << (r % 64);

  // The blur pixel shader reads c50 as the screen UV scale and gets 0.5
  // (PinScreenUVScaleReg); the GPU copy then differs from the guest's c50, so
  // it is looked at again by the next draw.
  const auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const bool pinUv = bank == kPixelBank && regs > 50 && ps &&
                     ps->shaderCacheEntry &&
                     ps->shaderCacheEntry->hash == kBDBlurPSHash;
  // c50 is looked at whenever the pin is on now or was on the last time.
  const bool pinCheck =
      bank == kPixelBank && regs > 50 && (pinUv || s.deltaPinned);
  if (pinCheck)
    todo[0] |= 1ull << 50;

  // Lowered to the GPU's form, compared with what the GPU copy holds, and
  // pushed in runs (gaps of up to two unchanged registers are pushed along,
  // one push instead of two).
  u8 *shadow = s.deltaShadow[bank];
  const u64 base = s.deltaGpu + u64(bank) * kConstantBlockBytes;
  u32 pushed = 0;
  u32 runStart = 0, runEnd = 0; // [start, end) registers, end 0 = no run
  auto flush = [&] {
    if (!runEnd)
      return;
    const u32 off = runStart * 16, len = (runEnd - runStart) * 16;
    plume::Deko3DPushUniform(list, base, kConstantBlockBytes, off,
                             shadow + off, len);
    pushed += len;
    runEnd = 0;
  };
  for (u32 w = 0; w < 4; ++w) {
    u64 bits = todo[w];
    while (bits) {
      const u32 r = w * 64 + u32(__builtin_ctzll(bits));
      bits &= bits - 1;
      if (r >= regs)
        break; // only c50 can sit past regs here, and only when regs > 50
      if (runEnd && r > runEnd + 2)
        flush();
#if defined(__aarch64__) || defined(_M_ARM64)
      uint8x16_t v = LoadGuestRegisterVec(mirror + r * 16);
      if (pinUv && r == 50) {
        const uint64x2_t halves = vsetq_lane_u64(
            0x3F0000003F000000ull, vreinterpretq_u64_u8(v), 0); // x = y = 0.5f
        v = vreinterpretq_u8_u64(halves);
      }
      if (r < known && SameRegisterVec(v, shadow + r * 16))
        continue;
      vst1q_u8(shadow + r * 16, v);
#else
      alignas(16) u8 v[16];
      LoadGuestRegister(v, mirror + r * 16);
      if (pinUv && r == 50) {
        const float half = 0.5f;
        std::memcpy(v, &half, 4);
        std::memcpy(v + 4, &half, 4);
      }
      if (r < known && SameRegister(v, shadow + r * 16))
        continue;
      std::memcpy(shadow + r * 16, v, 16);
#endif
      if (runEnd && r <= runEnd + 2) {
        runEnd = r + 1;
      } else {
        flush();
        runStart = r;
        runEnd = r + 1;
      }
    }
  }
  flush();
  if (bank == kPixelBank && regs > 50)
    s.deltaPinned = pinUv;
  s.deltaValid[bank] = std::max(s.deltaValid[bank], regs * 16);
  g_delta_draws.fetch_add(1, std::memory_order_relaxed);
  if (pushed) {
    g_delta_pushes.fetch_add(1, std::memory_order_relaxed);
    g_delta_bytes.fetch_add(pushed, std::memory_order_relaxed);
  }
  return true;
#else
  (void)list;
  (void)bank;
  (void)cap;
  return false;
#endif
}

// Inline (bd_render_thread off): both halves in one pass, as before the
// render queue. The split costs ~4% more of the Draw Thread per draw (a
// second pass over the changed registers and the mirror), which only pays
// when the halves run on two threads.
bool PushDeltaConstants(plume::RenderCommandList *list, ConstantBank bank,
                        u32 device_guest, u32 bytes,
                        const GuestShader * /*shader*/) {
#if defined(PLUME_DEKO3D)
  BD_CPU_ZONE("PushDeltaConstants");
  auto &s = upload_state();
  if (!list || !device_guest || bank > kPixelBank || !EnsureDeltaBuffer(s))
    return false;
  const u32 regs = (std::min(bytes, kConstantBlockBytes) + 15u) / 16u;
  const u8 *guest = bd::mem::at<const u8>(
      device_guest + u32(bank == kVertexBank
                             ? offsetof(D3DDevice, vsFloatConstants)
                             : offsetof(D3DDevice, psFloatConstants)));
  if (!guest)
    return false;

  // The registers to look at: the ones the setters touched since they were
  // last sent, the always-checked ones, and all that the GPU copy does not
  // hold yet (after a new command list, or a shader reading further).
  u64 want[4];
  auto &dirty = g_delta_dirty[bank];
  t_delta_draw = true;
  for (u32 w = 0; w < 4; ++w) {
    const u32 lo = w * 64;
    if (lo >= regs) {
      want[w] = 0;
      continue;
    }
    const u64 inRange =
        regs - lo >= 64 ? ~0ull : ((1ull << (regs - lo)) - 1);
    // Bits past the shader's range stay set for a later shader that reads them.
    want[w] = g_delta_local[bank][w] & inRange;
    g_delta_local[bank][w] &= ~inRange;
    if (dirty.bits[w].load(std::memory_order_relaxed) & inRange)
      want[w] |= dirty.bits[w].fetch_and(~inRange, std::memory_order_relaxed) &
                 inRange;
  }
  const u32 known = s.deltaValid[bank] / 16;
  for (u32 r = known; r < regs; ++r)
    want[r / 64] |= 1ull << (r % 64);
  const u8 *always = bank == kVertexBank ? kAlwaysVertex : kAlwaysPixel;
  const u32 nAlways = bank == kVertexBank ? std::size(kAlwaysVertex)
                                          : std::size(kAlwaysPixel);
  for (u32 i = 0; i < nAlways; ++i)
    if (always[i] < regs)
      want[always[i] / 64] |= 1ull << (always[i] % 64);
  for (u32 w = 0; w < 4 && w * 64 < regs; ++w) {
    const u32 lo = w * 64;
    const u64 inRange = regs - lo >= 64 ? ~0ull : ((1ull << (regs - lo)) - 1);
    want[w] |= g_delta_learned[bank].bits[w].load(std::memory_order_relaxed) &
               inRange;
  }

  // Sampled check of the whole range: a register that changed without being
  // marked is a writer the lists above miss; it is learned (g_delta_learned).
  // One call in 32 for the first 4096 checks, one in 256 after (the console
  // runs found their writers within seconds; the full read was ~2% of the
  // draw thread).
  static u32 verifyTick[2] = {0, 0};
  const u32 tick = ++verifyTick[bank];
  const bool verify = (tick & (tick < 131072u ? 31u : 255u)) == 0;

  // The blur pixel shader reads c50 as the screen UV scale and gets 0.5
  // (PinScreenUVScaleReg); the GPU copy then differs from the guest's c50, so
  // it is looked at again by the next draw.
  const auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const bool pinUv = bank == kPixelBank && regs > 50 && ps &&
                     ps->shaderCacheEntry &&
                     ps->shaderCacheEntry->hash == kBDBlurPSHash;
  // c50 is looked at whenever the pin is on now or was on the last time.
  const bool pinCheck =
      bank == kPixelBank && regs > 50 && (pinUv || s.deltaPinned);
  if (pinCheck)
    want[0] |= 1ull << 50;

  // Stage 1 of the render thread plan (docs/SWITCH.md): the registers are
  // read from the guest in one pass (capture: the ones that may have changed,
  // as the guest has them now) and lowered and pushed in a second (apply),
  // which reads only the capture. Recorded on another thread later, the apply
  // half must not look at the guest's registers, which the game keeps writing.
  struct Captured {
    u16 reg;
    bool marked;
  };
  Captured entries[kConstantRegisterCount];
  alignas(16) u8 values[kConstantRegisterCount][16];
  u32 count = 0;
  u8 *rawShadow = s.deltaRaw[bank];
  u32 looked = 0;
  for (u32 r = 0; r < regs; ++r) {
    const bool marked = (want[r / 64] >> (r % 64)) & 1u;
    if (!marked && !verify) {
      // Skip to the next marked register.
      u32 w = r / 64;
      u64 rest = want[w] & (~0ull << (r % 64));
      while (!rest && ++w < 4)
        rest = want[w];
      if (!rest)
        break;
      r = w * 64 + u32(__builtin_ctzll(rest)) - 1;
      continue;
    }
    ++looked;
    const u8 *gr = guest + r * 16;
    if (r < known && !(pinCheck && r == 50) && SameRegister(gr, rawShadow + r * 16))
      continue;
    std::memcpy(rawShadow + r * 16, gr, 16);
    entries[count] = {u16(r), marked};
    std::memcpy(values[count], gr, 16);
    ++count;
  }

  // Apply: lowered to the GPU's form, compared with what the GPU copy holds,
  // and pushed in runs (gaps of up to two unchanged registers are pushed
  // along, one push instead of two).
  u8 *shadow = s.deltaShadow[bank];
  const u64 base = s.deltaGpu + u64(bank) * kConstantBlockBytes;
  u32 pushed = 0;
  u32 runStart = 0, runEnd = 0; // [start, end) registers, end 0 = no run
  auto flush = [&] {
    if (!runEnd)
      return;
    const u32 off = runStart * 16, len = (runEnd - runStart) * 16;
    plume::Deko3DPushUniform(list, base, kConstantBlockBytes, off,
                             shadow + off, len);
    pushed += len;
    runEnd = 0;
  };
  for (u32 i = 0; i < count; ++i) {
    const u32 r = entries[i].reg;
    const bool marked = entries[i].marked;
    if (runEnd && r > runEnd + 2)
      flush();
#if defined(__aarch64__) || defined(_M_ARM64)
    uint8x16_t v = LoadGuestRegisterVec(values[i]);
    if (pinUv && r == 50) {
      const uint64x2_t halves = vsetq_lane_u64(
          0x3F0000003F000000ull, vreinterpretq_u64_u8(v), 0); // x = y = 0.5f
      v = vreinterpretq_u8_u64(halves);
    }
    if (r < known && SameRegisterVec(v, shadow + r * 16))
      continue;
#else
    alignas(16) u8 v[16];
    LoadGuestRegister(v, values[i]);
    if (pinUv && r == 50) {
      const float half = 0.5f;
      std::memcpy(v, &half, 4);
      std::memcpy(v + 4, &half, 4);
    }
    if (r < known && SameRegister(v, shadow + r * 16))
      continue;
#endif
    if (!marked) {
      g_delta_misses.fetch_add(1, std::memory_order_relaxed);
      const u64 bit = 1ull << (r % 64);
      const u64 before = g_delta_learned[bank].bits[r / 64].fetch_or(
          bit, std::memory_order_relaxed);
      if (!(before & bit)) {
        const auto *sh = bank == kVertexBank
                             ? bd::gpu::state().pipelineState.vertexShader
                             : ps;
        const u64 h = (sh && sh->shaderCacheEntry) ? sh->shaderCacheEntry->hash : 0;
        std::fprintf(stderr,
                     "[ubo] in-place constants: %s c%u changed without a "
                     "setter (shader %016llX): checked on every draw from now\n",
                     bank == kVertexBank ? "VS" : "PS", r,
                     static_cast<unsigned long long>(h));
      }
    }
#if defined(__aarch64__) || defined(_M_ARM64)
    vst1q_u8(shadow + r * 16, v);
#else
    std::memcpy(shadow + r * 16, v, 16);
#endif
    if (runEnd && r <= runEnd + 2) {
      runEnd = r + 1;
    } else {
      flush();
      runStart = r;
      runEnd = r + 1;
    }
  }
  flush();
  if (bank == kPixelBank && regs > 50)
    s.deltaPinned = pinUv;
  s.deltaValid[bank] = std::max(s.deltaValid[bank], regs * 16);
  g_delta_draws.fetch_add(1, std::memory_order_relaxed);
  g_delta_regs.fetch_add(looked, std::memory_order_relaxed);
  if (pushed) {
    g_delta_pushes.fetch_add(1, std::memory_order_relaxed);
    g_delta_bytes.fetch_add(pushed, std::memory_order_relaxed);
  }
  return true;
#else
  (void)list;
  (void)bank;
  (void)device_guest;
  (void)bytes;
  return false;
#endif
}

void BindDeltaConstants(plume::RenderCommandList *list,
                        const ConstantAllocation &shared) {
#if defined(PLUME_DEKO3D)
  auto &s = upload_state();
  if (!list || !EnsureDeltaBuffer(s))
    return;
  // Set 5 is not bound in this mode: its slots are bound directly (binding 0
  // vertex, 1 pixel, 2 shared, as the deko3d shaders declare them). Unchanged
  // bindings cost a compare (Deko3DBindUniform).
  plume::Deko3DBindUniform(list, kVertexBank, s.deltaGpu, kConstantBlockBytes);
  plume::Deko3DBindUniform(list, kPixelBank, s.deltaGpu + kConstantBlockBytes,
                           kConstantBlockBytes);
  if (shared.memory)
    plume::Deko3DBindUniform(list, kSharedBank, shared.gpuAddress,
                             u32(sizeof(SharedConstants)));
#else
  (void)list;
  (void)shared;
#endif
}

namespace {
void BuildSharedConstants(UploadState &s, u32 device_guest,
                          const GuestDrawState &guest);
} // namespace

bool PushDeltaShared(plume::RenderCommandList *list, u32 device_guest,
                     const GuestDrawState &guest) {
#if defined(PLUME_DEKO3D)
  BD_CPU_ZONE("PushDeltaShared");
  auto &s = upload_state();
  if (!s.ready || !list || !EnsureDeltaBuffer(s))
    return false;
  BuildSharedConstants(s, device_guest, guest);
  const u64 base = s.deltaGpu + 2ull * kConstantBlockBytes;
  const auto *now = reinterpret_cast<const u8 *>(&s.shared);
  auto *held = reinterpret_cast<u8 *>(&s.deltaShared);
  constexpr u32 kRegs = u32(sizeof(SharedConstants)) / 16;
  static_assert(sizeof(SharedConstants) % 16 == 0);
  u32 pushed = 0, runStart = 0, runEnd = 0;
  auto flush = [&] {
    if (!runEnd)
      return;
    const u32 off = runStart * 16, len = (runEnd - runStart) * 16;
    plume::Deko3DPushUniform(list, base, kSharedBlockBytes, off, held + off, len);
    pushed += len;
    runEnd = 0;
  };
  for (u32 r = 0; r < kRegs; ++r) {
    if (s.deltaSharedValid && SameRegister(now + r * 16, held + r * 16))
      continue;
    std::memcpy(held + r * 16, now + r * 16, 16);
    if (runEnd && r <= runEnd + 2) {
      runEnd = r + 1;
    } else {
      flush();
      runStart = r;
      runEnd = r + 1;
    }
  }
  flush();
  s.deltaSharedValid = true;
  if (pushed)
    g_delta_bytes.fetch_add(pushed, std::memory_order_relaxed);
  // Bound at slot 2; bound textures (plume's bindTextures) read the indices
  // from this CPU copy, which holds what the GPU copy will at the draw.
  plume::Deko3DBindUniform(list, kSharedBank, base, kSharedBlockBytes);
  plume::Deko3DSetSharedWords(list, &s.deltaShared, u32(sizeof(SharedConstants)));
  return true;
#else
  (void)list;
  (void)device_guest;
  (void)guest;
  return false;
#endif
}

void DeltaConstantStats(u64 &draws, u64 &updates, u64 &bytes, u64 &regs,
                        u64 &misses) {
#if defined(PLUME_DEKO3D)
  draws = g_delta_draws.exchange(0, std::memory_order_relaxed);
  updates = g_delta_pushes.exchange(0, std::memory_order_relaxed);
  bytes = g_delta_bytes.exchange(0, std::memory_order_relaxed);
  regs = g_delta_regs.exchange(0, std::memory_order_relaxed);
  misses = g_delta_misses.load(std::memory_order_relaxed);
#else
  draws = updates = bytes = regs = misses = 0;
#endif
}
#endif

namespace {

// s.shared from the live guest and renderer state.
void BuildSharedConstants(UploadState &s, u32 device_guest,
                          const GuestDrawState &guest) {
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
  // Fetch and bool constants: the copy taken when the draw was issued.
  const bool have_device = guest.device;
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
      if (have_device) {
        const auto &fc_be = guest.fetchRaw[i];
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto &sc = s.samplerSlots[i];
        if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
            std::memcmp(sc.fc, &fc_be, sizeof(sc.fc)) == 0) {
          s.shared.samplerIndices[i] = sc.sampler;
        } else {
          // fetchRaw holds the words as stored (big-endian).
          DeviceFetchConstant stored;
          std::memcpy(&stored, fc_be, sizeof(stored));
          const u32 fc[6] = {
              u32(stored.dword[0]), u32(stored.dword[1]), u32(stored.dword[2]),
              u32(stored.dword[3]), u32(stored.dword[4]), u32(stored.dword[5]),
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
    for (u32 i = 0; i < 4; ++i) {
      s.shared.booleansArr[i] = have_device ? guest.vsBool[i] : 0u;
      s.shared.booleansArr[4 + i] = have_device ? guest.psBool[i] : 0u;
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

}

} // namespace

ConstantAllocation UploadSharedConstants(u32 device_guest,
                                         const GuestDrawState &guest) {
  BD_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  BuildSharedConstants(s, device_guest, guest);
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

ConstantAllocation UploadBytesByteSwap32(const u8 *bytes, u32 size,
                                         u32 alignment) {
  BD_CPU_ZONE("UploadBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !bytes || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  if (reinterpret_cast<uintptr_t>(bytes) % alignof(u32) == 0) {
    CopyByteSwap32Host<false>(alloc.memory, reinterpret_cast<const u32 *>(bytes),
                              size);
  } else {
    std::vector<u32> aligned((size + 3) / 4);
    std::memcpy(aligned.data(), bytes, size);
    CopyByteSwap32Host<false>(alloc.memory, aligned.data(), size);
  }
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
