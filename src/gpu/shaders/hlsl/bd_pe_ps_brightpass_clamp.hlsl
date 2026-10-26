// bd_pe_ps_brightpass (0xFFDBD782126EB6E8) from the XenosRecomp shader cache,
// with the bright-mask export clamp reblue swaps in at shader link time.
// Shared plumbing lives in bd_pe_ps_common.hlsli.

#include "bd_pe_ps_common.hlsli"

#if defined(__spirv__) && !defined(REBLUE_UBO_CONSTANTS) // device addresses; else the cbuffers (D3D12, uniform buffers)

#define g_vParam vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 432, 0x10)

#else

REBLUE_CBUFFER_BINDING(1) cbuffer PixelShaderConstants : register(b1, space4)
{
	float4 g_vParam : packoffset(c27);
};

#endif

#ifndef __spirv__
[shader("pixel")]
#endif
void main(BD_PE_PS_PARAMS)
{
	float4 c252 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c253 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c254 = asfloat(uint4(0x3E800000, 0x40800000, 0x3E59999A, 0x0));
	float4 c255 = asfloat(uint4(0x3D93A92A, 0x3F372474, 0x3F800000, 0x0));

	BD_PE_PS_REGISTERS;

	r0.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz;
	r1.xyz = r0.xyz + -g_vParam.xxx;
	r0.xzw = min(r0.xyz, c254.xxx);
	ps = max(g_vParam.y, g_vParam.y);
	r1.yzw = max(r1.xyz, c255.www);
	ps = c254.z * ps;
	r0.y = ps;
	r2.z = r0.y * r1.y;
	ps = c254.y * r0.x;
	r1.x = ps;
	r2.xy = r1.zw * g_vParam.yy;
	ps = c254.y * r0.z;
	r1.y = ps;
	r0.y = dot(r2.yxz, c255.xyz);
	ps = c254.y * r0.w;
	r1.z = ps;
	oC0.xyz = r1.xyz * r0.yyy;
	oC0.w = 1.0;
	// X360 saturates this bright-mask on the EDRAM export before the bloom
	// blur, and unclamped FP16 lets BriMulti-scaled masks (up to ~59 on wc01)
	// blur into giant white blobs.
	oC0.xyz = min(oC0.xyz, 1.0);
	[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)	{		clip(oC0.w - g_AlphaThreshold);
	}	return;
}
