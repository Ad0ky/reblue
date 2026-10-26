// Host substitute for the engine's 2D blit pixel shader: samples Tex0 through
// the bindless heap (via SharedConstants), like every recompiled shader.

#include "thirdparty/XenosRecomp/XenosRecomp/shader_common.h"

// Declared field-by-field, not via DEFINE_SHARED_CONSTANTS(): this shader is
// built without REBLUE_RECOMP, so that macro would place them on the other
// layout's offsets.
#if defined(__spirv__) && !defined(REBLUE_UBO_CONSTANTS) // device addresses; else the cbuffers (D3D12, uniform buffers)
#define Tex0_ResourceDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 0)
#define Tex0_SamplerDescriptorIndex  vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 192)
#else
REBLUE_CBUFFER_BINDING(2) cbuffer SharedConstants : register(b2, space4)
{
    uint Tex0_ResourceDescriptorIndex : packoffset(c0.x);   // texture2DIndices[0]
    uint Tex0_SamplerDescriptorIndex  : packoffset(c12.x);  // samplerIndices[0]
};
#endif

struct PS_IN
{
    float4 Position : SV_Position;
    float4 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
};

float4 main(PS_IN In) : SV_Target
{
    Texture2D<float4> tex  = g_Texture2DDescriptorHeap[Tex0_ResourceDescriptorIndex];
    // bdBeginTextBatch asks for LINEAR, bdRenderDebugTextBegin for NEAREST, so a
    // hardcoded slot gets one of the two wrong.
    SamplerState      samp = g_SamplerDescriptorHeap[Tex0_SamplerDescriptorIndex];
    return tex.Sample(samp, In.UV) * In.Color;
}
