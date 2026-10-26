// Host DXIL substitute for the engine's 2D blit VS: it's built at runtime via
// hcgVertexShaderCreateByHlsl (0x82286630) from HLSL text the recompiler can't see.
// Y-flipped (top-left origin).

#include "thirdparty/XenosRecomp/XenosRecomp/shader_common.h"

#if defined(__spirv__) && !defined(REBLUE_UBO_CONSTANTS) // device addresses; else the cbuffers (D3D12, uniform buffers)
#define g_BlitHalfPixelOffset \
    vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 336)
#else
REBLUE_CBUFFER_BINDING(2) cbuffer SharedConstants : register(b2, space4)
{
    float2 g_BlitHalfPixelOffset : packoffset(c21.x);
};
#endif

// Locations must match REBLUE_VERTEX_INPUT_LOCATIONS: this VS is bound against
// a guest vertex declaration, whose input layout is numbered from that table.
struct VS_IN
{
    [[vk::location(0)]]  float2 Position : POSITION;
    [[vk::location(7)]]  float2 UV       : TEXCOORD0;
    [[vk::location(10)]] float4 Color    : COLOR;
};

struct VS_OUT
{
    float4 Position : SV_Position;
    float4 Color    : COLOR0;
    float2 UV       : TEXCOORD0;
};

VS_OUT main(VS_IN In)
{
    VS_OUT Out;
    Out.Position.x =  (In.Position.x * 2.0f - 1.0f);
    Out.Position.y = -(In.Position.y * 2.0f - 1.0f);
    Out.Position.z = 0.0f;
    Out.Position.w = 1.0f;
    Out.Position.xy += g_BlitHalfPixelOffset;
    Out.Color = In.Color;
    Out.UV    = In.UV;
    return Out;
}
