/**
 * @file    gpu/constant_buffers.h
 * @brief   Shader constant upload path.
 *
 *   VS f4 constants at device+0x700, PS f4 at device+0x1700, SharedConstants
 *   bound through the main RenderPipelineLayout's three root descriptors
 *   (b0/b1/b2, space4).
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstddef>
#include <rex/types.h>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

namespace bd::gpu {

struct GuestShader;

// Mirror of the SharedConstants cbuffer (b2, space4). Layout matches the
// packoffset(c<N>) emit in the recompiled shader prelude, so reordering or
// resizing means regenerating every shader in the DXIL cache.
struct SharedConstants {
  u32 texture2DIndices[16]{};   // c0..c3,  bytes 0..63
  u32 texture3DIndices[16]{};   // c4..c7,  bytes 64..127
  u32 textureCubeIndices[16]{}; // c8..c11, bytes 128..191
  u32 samplerIndices[16]{};     // c12..c15, bytes 192..255
  u32 booleansArr[8]{};         // c16, c17 (g_BooleansArr[2]) bytes 256..287
  u32 swappedTexcoords{};       // c18.x, byte 288
  float halfPixelOffsetX{};     // c18.y, byte 292
  float halfPixelOffsetY{};     // c18.z, byte 296
  float alphaThreshold{};       // c18.w, byte 300
  u32 swappedNormals{};         // c19.x, byte 304
  u32 swappedBinormals{};       // c19.y, byte 308
  u32 swappedTangents{};        // c19.z, byte 312
  u32 swappedBlendWeights{};    // c19.w, byte 316
  u32 swappedPositions{};       // c20.x, byte 320
  // Bit N set when TEXCOORD<N> is bound R16G16(B16A16)_UINT, and the shader's
  // sintTexcoord() sign-extends to recover the X360 raw-int-as-float vfetch.
  u32 sintTexcoords{}; // c20.y, byte 324
  // Sun shadow PCF tap scale (g_ShadowPcfScale). The kernel's tap offsets are
  // shader literals in 1/1024-of-the-map UV units, so their world footprint
  // grows with the bd_shadow_distance coverage box, so the recompiler
  // multiplies each by this to hold the penumbra constant in world space.
  // max(1/distance, 1024/dimension) never scales a tap below one map texel.
  float shadowPcfScale{1.0f}; // c20.z, byte 328
  u32 _pad_c20_w{};           // c20.w, byte 332
  // Half-pixel NDC nudge read only by the substituted 2D blit VS
  float blitHalfPixelOffsetX{}; // c21.x, byte 336
  float blitHalfPixelOffsetY{}; // c21.y, byte 340
  u32 _pad_c21_z{};             // c21.z, byte 344
  u32 _pad_c21_w{};             // c21.w, byte 348
};
static_assert(sizeof(SharedConstants) == 352);

// memory stays valid until ResetFrame().
struct ConstantAllocation {
  u8 *memory = nullptr;
  plume::RenderBufferReference ref{};
  u64 gpuAddress = 0;
  u32 size = 0;
};

bool TryInit();

// Rewind a frame slot's upload chunks. Caller must have awaited that slot's
// GPU fence
// (DrainSlot), never while in flight.
void ResetFrame(u32 slot);

// Byte-swap VS/PS float constants from guest device+0x700 / device+0x1700 into
// the upload heap.
// 'bytes': how much of the 4 KiB block to copy (FloatConstantBytes); the
// rest of the allocation's 4 KiB window is left as it was.
ConstantAllocation UploadVertexShaderConstants(u32 device_guest,
                                               u32 bytes = 4096);
ConstantAllocation UploadPixelShaderConstants(u32 device_guest,
                                              u32 bytes = 4096);

// The bytes of the float constant block 'shader' reads: on the Switch from
// the shader cache (most pixel shaders read under 1 KiB of the 4), elsewhere
// and for shaders without an entry the whole block.
u32 FloatConstantBytes(const GuestShader *shader);

// Rebuilt from live guest state every draw: sampler fetch constants and bool
// constants come from unhooked recompiled code, so there is no dirty signal.
// size == 0 means byte-identical to what is already bound on this command list
// and the caller can skip the upload + root rebind.
ConstantAllocation UploadSharedConstants(u32 device_guest);

// Root bindings do not survive begin(), so this drops the 'shared CB still
// bound' reuse gate. Call on every command list reset.
void InvalidateSharedBinding();

// Every file that tests REBLUE_UBO_CONSTANTS includes this header: it is 0
// rather than undefined elsewhere, so a missing include fails loudly under
// -Wundef instead of silently taking the push-constant path.
#if defined(__SWITCH__) && !defined(REBLUE_D3D12)
// The Switch: guest shaders read their three constant blocks from dynamic
// uniform buffers in descriptor set 5 (vertex b0, pixel b1, shared b2), which
// NVK keeps in the Maxwell constant banks, instead of through the device
// addresses in push constants the other Vulkan builds use (one memory load per
// constant read). Matches REBLUE_UBO_CONSTANTS in XenosRecomp's shader_common.h.
#define REBLUE_UBO_CONSTANTS 1
inline constexpr u32 kConstantDescriptorSetIndex = 5;
enum ConstantBank : u32 { kVertexBank = 0, kPixelBank = 1, kSharedBank = 2 };

// bd_switch_ubo_constants, read once: the uniform-buffer form is in use. Off,
// the guest shaders are the device-address variant and push constants carry
// the three addresses as on the PC.
bool UboConstantsEnabled();

// Adds set 5 to the main pipeline layout (BuildPipelineLayout, after set 4).
void AddConstantDescriptorSet(plume::RenderPipelineLayoutBuilder &layout);
// A block just uploaded for the next draw.
void SetDrawConstants(ConstantBank bank, const ConstantAllocation &alloc);
// Binds set 5 with the current blocks if any changed since the last bind.
void BindDrawConstants(plume::RenderCommandList *list);

// deko3d with bd_deko3d_delta_constants: the vertex and pixel banks are one
// uniform buffer per stage that the command list updates in place
// (dkCmdBufPushConstants, pipelined: earlier draws keep the old values), with
// only the registers that differ from what the GPU will already hold, instead
// of a byte-swapped 4 KiB block per stage and draw. Set 5 is not used then:
// BindDeltaConstants binds the three slots directly.
bool DeltaConstantsEnabled();
// The D3D float setters: registers [start, start + count) of the bank were
// written. The in-place update looks only at these (plus a few the engine
// writes inline, and a sampled check of the rest).
void MarkConstantRegisters(ConstantBank bank, u32 start, u32 count);
// The first 'bytes' of the bank from the guest device; false if not done.
bool PushDeltaConstants(plume::RenderCommandList *list, ConstantBank bank,
                        u32 device_guest, u32 bytes);
// The shared block (texture and sampler indices, bool constants...) kept in
// place too: built from the live state, its changed registers pushed, bound at
// slot 2. False if not done (the caller uploads it as before).
bool PushDeltaShared(plume::RenderCommandList *list, u32 device_guest);
// Binds the two in-place banks and, if it has memory, the shared block.
void BindDeltaConstants(plume::RenderCommandList *list,
                        const ConstantAllocation &shared);
// Draws, updates and bytes since the last call (the profiler's report).
// Also the registers looked at, and the changed ones no setter announced.
void DeltaConstantStats(u64 &draws, u64 &updates, u64 &bytes, u64 &regs,
                        u64 &misses);
#else
#define REBLUE_UBO_CONSTANTS 0
#endif

// alignment defaults to 4 (the upload buffer is WRITE_COMBINE), and CBV callers
// pass 256.
ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment = 4);

// Verbatim, no swap, for texture pixel data.
ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment = 4);

// For pixel data already untiled to host staging.
ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment = 4);

} // namespace bd::gpu
