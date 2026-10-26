// bd_pe_ps_ms_bright (0xD386EA2FABF16CE9) from the XenosRecomp shader cache,
// with the bright-mask export clamp reblue swaps in at shader link time.
// Shared plumbing lives in bd_pe_ps_common.hlsli.

#include "bd_pe_ps_common.hlsli"

#if defined(__spirv__) && !defined(REBLUE_UBO_CONSTANTS) // device addresses; else the cbuffers (D3D12, uniform buffers)

#define g_vCount vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + 416, 0x10)
#define g_vSampleOffsets(INDEX) select((INDEX) < 224, vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (0 + min(INDEX, 223)) * 16, 0x10), 0.0)
#define g_vSampleWeights(INDEX) select((INDEX) < 211, vk::RawBufferLoad<float4>(g_PushConstants.PixelShaderConstants + (13 + min(INDEX, 210)) * 16, 0x10), 0.0)

#else

REBLUE_CBUFFER_BINDING(1) cbuffer PixelShaderConstants : register(b1, space4)
{
	// In offset order: SPIR-V wants a block's members by increasing offset.
	float4 g_vSampleOffsets[13] : packoffset(c0);
#define g_vSampleOffsets(INDEX) select((INDEX) < 224, g_vSampleOffsets[min(INDEX, 223)], 0.0)
	float4 g_vSampleWeights[13] : packoffset(c13);
#define g_vSampleWeights(INDEX) select((INDEX) < 211, g_vSampleWeights[min(INDEX, 210)], 0.0)
	float4 g_vCount : packoffset(c26);
};

#endif

#ifndef __spirv__
[shader("pixel")]
#endif
void main(BD_PE_PS_PARAMS)
{
	float4 c248 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c249 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c250 = asfloat(uint4(0x0, 0x0, 0x0, 0x0));
	float4 c251 = asfloat(uint4(0x40E00000, 0x40400000, 0x41200000, 0x41000000));
	float4 c252 = asfloat(uint4(0x40C00000, 0x3F800000, 0x41400000, 0x3E800000));
	float4 c253 = asfloat(uint4(0x41300000, 0x41100000, 0x0, 0x40800000));
	float4 c254 = asfloat(uint4(0x3D93A92A, 0x3E59999A, 0x3F372474, 0x40A00000));
	float4 c255 = asfloat(uint4(0x40000000, 0x0, 0x0, 0x0));

	BD_PE_PS_REGISTERS;

	r1.xyz = -abs(r0.xxx) > c253.zzz;
	p0 = g_vCount.x > 0.0;
	ps = p0 ? 0.0 : 1.0;
	if (p0)
	{
		r0.zw = r0.xy + g_vSampleOffsets(0).xy;
		r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
		r0.z = g_vCount.x > c252.y;
		r3.xyz = r2.xyz + -c252.www;
		r1.xyz = min(r2.xyz, c252.www);
		r1.xyz = r1.xyz * c253.www;
		r3.xyz = max(r3.xyz, c253.zzz);
		r0.w = dot(r3.zxy, c254.xyz);
		r1.xyz = r1.xyz * r0.www;
		r1.xyz = r1.xzy * g_vSampleWeights(0).xzy;
		p0 = r0.z != 0.0;
		ps = p0 ? 0.0 : 1.0;
		r1.xyz = r2.xyz * g_vSampleWeights(0).xyz + r1.xzy;
		if (p0)
		{
			r0.zw = r0.xy + g_vSampleOffsets(1).xy;
			r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
			r0.z = g_vCount.x > c255.x;
			r1.xyz = r2.zxy * g_vSampleWeights(1).zxy + r1.zxy;
			r3.xyz = r2.xyz + -c252.www;
			r2.xyz = min(r2.xyz, c252.www);
			r2.xyz = r2.xyz * c253.www;
			r3.xyz = max(r3.xyz, c253.zzz);
			r0.w = dot(r3.zxy, c254.xyz);
			r2.xyz = r2.xyz * r0.www;
			p0 = r0.z != 0.0;
			ps = p0 ? 0.0 : 1.0;
			r1.xyz = r2.xyz * g_vSampleWeights(1).xyz + r1.yzx;
			if (p0)
			{
				r0.zw = r0.xy + g_vSampleOffsets(2).xy;
				r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
				r0.z = g_vCount.x > c251.y;
				r1.xyz = r2.zxy * g_vSampleWeights(2).zxy + r1.zxy;
				r3.xyz = r2.xyz + -c252.www;
				r2.xyz = min(r2.xyz, c252.www);
				r2.xyz = r2.xyz * c253.www;
				r3.xyz = max(r3.xyz, c253.zzz);
				r0.w = dot(r3.zxy, c254.xyz);
				r2.xyz = r2.xyz * r0.www;
				p0 = r0.z != 0.0;
				ps = p0 ? 0.0 : 1.0;
				r1.xyz = r2.xyz * g_vSampleWeights(2).xyz + r1.yzx;
				if (p0)
				{
					r0.zw = r0.xy + g_vSampleOffsets(3).xy;
					r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
					r0.z = g_vCount.x > c253.w;
					r1.xyz = r2.zxy * g_vSampleWeights(3).zxy + r1.zxy;
					r3.xyz = r2.xyz + -c252.www;
					r2.xyz = min(r2.xyz, c252.www);
					r2.xyz = r2.xyz * c253.www;
					r3.xyz = max(r3.xyz, c253.zzz);
					r0.w = dot(r3.zxy, c254.xyz);
					r2.xyz = r2.xyz * r0.www;
					p0 = r0.z != 0.0;
					ps = p0 ? 0.0 : 1.0;
					r1.xyz = r2.xyz * g_vSampleWeights(3).xyz + r1.yzx;
					if (p0)
					{
						r0.zw = r0.xy + g_vSampleOffsets(4).xy;
						r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
						r0.z = g_vCount.x > c254.w;
						r1.xyz = r2.zxy * g_vSampleWeights(4).zxy + r1.zxy;
						r3.xyz = r2.xyz + -c252.www;
						r2.xyz = min(r2.xyz, c252.www);
						r2.xyz = r2.xyz * c253.www;
						r3.xyz = max(r3.xyz, c253.zzz);
						r0.w = dot(r3.zxy, c254.xyz);
						r2.xyz = r2.xyz * r0.www;
						p0 = r0.z != 0.0;
						ps = p0 ? 0.0 : 1.0;
						r1.xyz = r2.xyz * g_vSampleWeights(4).xyz + r1.yzx;
						if (p0)
						{
							r0.zw = r0.xy + g_vSampleOffsets(5).xy;
							r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
							r0.z = g_vCount.x > c252.x;
							r1.xyz = r2.zxy * g_vSampleWeights(5).zxy + r1.zxy;
							r3.xyz = r2.xyz + -c252.www;
							r2.xyz = min(r2.xyz, c252.www);
							r2.xyz = r2.xyz * c253.www;
							r3.xyz = max(r3.xyz, c253.zzz);
							r0.w = dot(r3.zxy, c254.xyz);
							r2.xyz = r2.xyz * r0.www;
							p0 = r0.z != 0.0;
							ps = p0 ? 0.0 : 1.0;
							r1.xyz = r2.xyz * g_vSampleWeights(5).xyz + r1.yzx;
							if (p0)
							{
								r0.zw = r0.xy + g_vSampleOffsets(6).xy;
								r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
								r0.z = g_vCount.x > c251.x;
								r1.xyz = r2.zxy * g_vSampleWeights(6).zxy + r1.zxy;
								r3.xyz = r2.xyz + -c252.www;
								r2.xyz = min(r2.xyz, c252.www);
								r2.xyz = r2.xyz * c253.www;
								r3.xyz = max(r3.xyz, c253.zzz);
								r0.w = dot(r3.zxy, c254.xyz);
								r2.xyz = r2.xyz * r0.www;
								p0 = r0.z != 0.0;
								ps = p0 ? 0.0 : 1.0;
								r1.xyz = r2.xyz * g_vSampleWeights(6).xyz + r1.yzx;
								if (p0)
								{
									r0.zw = r0.xy + g_vSampleOffsets(7).xy;
									r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
									r0.z = g_vCount.x > c251.w;
									r1.xyz = r2.zxy * g_vSampleWeights(7).zxy + r1.zxy;
									r3.xyz = r2.xyz + -c252.www;
									r2.xyz = min(r2.xyz, c252.www);
									r2.xyz = r2.xyz * c253.www;
									r3.xyz = max(r3.xyz, c253.zzz);
									r0.w = dot(r3.zxy, c254.xyz);
									r2.xyz = r2.xyz * r0.www;
									p0 = r0.z != 0.0;
									ps = p0 ? 0.0 : 1.0;
									r1.xyz = r2.xyz * g_vSampleWeights(7).xyz + r1.yzx;
									if (p0)
									{
										r0.zw = r0.xy + g_vSampleOffsets(8).xy;
										r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
										r0.z = g_vCount.x > c253.y;
										r1.xyz = r2.zxy * g_vSampleWeights(8).zxy + r1.zxy;
										r3.xyz = r2.xyz + -c252.www;
										r2.xyz = min(r2.xyz, c252.www);
										r2.xyz = r2.xyz * c253.www;
										r3.xyz = max(r3.xyz, c253.zzz);
										r0.w = dot(r3.zxy, c254.xyz);
										r2.xyz = r2.xyz * r0.www;
										p0 = r0.z != 0.0;
										ps = p0 ? 0.0 : 1.0;
										r1.xyz = r2.xyz * g_vSampleWeights(8).xyz + r1.yzx;
										if (p0)
										{
											r0.zw = r0.xy + g_vSampleOffsets(9).xy;
											r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
											r0.z = g_vCount.x > c251.z;
											r1.xyz = r2.zxy * g_vSampleWeights(9).zxy + r1.zxy;
											r3.xyz = r2.xyz + -c252.www;
											r2.xyz = min(r2.xyz, c252.www);
											r2.xyz = r2.xyz * c253.www;
											r3.xyz = max(r3.xyz, c253.zzz);
											r0.w = dot(r3.zxy, c254.xyz);
											r2.xyz = r2.xyz * r0.www;
											p0 = r0.z != 0.0;
											ps = p0 ? 0.0 : 1.0;
											r1.xyz = r2.xyz * g_vSampleWeights(9).xyz + r1.yzx;
											if (p0)
											{
												r0.zw = r0.xy + g_vSampleOffsets(10).xy;
												r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
												r0.z = g_vCount.x > c253.x;
												r1.xyz = r2.zxy * g_vSampleWeights(10).zxy + r1.zxy;
												r3.xyz = r2.xyz + -c252.www;
												r2.xyz = min(r2.xyz, c252.www);
												r2.xyz = r2.xyz * c253.www;
												r3.xyz = max(r3.xyz, c253.zzz);
												r0.w = dot(r3.zxy, c254.xyz);
												r2.xyz = r2.xyz * r0.www;
												p0 = r0.z != 0.0;
												ps = p0 ? 0.0 : 1.0;
												r1.xyz = r2.xyz * g_vSampleWeights(10).xyz + r1.yzx;
												if (p0)
												{
													r0.zw = r0.xy + g_vSampleOffsets(11).xy;
													r2.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.zw, float2(0, 0)).xyz;
													r0.z = g_vCount.x > c252.z;
													r1.xyz = r2.zxy * g_vSampleWeights(11).zxy + r1.zxy;
													r3.xyz = r2.xyz + -c252.www;
													r2.xyz = min(r2.xyz, c252.www);
													r2.xyz = r2.xyz * c253.www;
													r3.xyz = max(r3.xyz, c253.zzz);
													r0.w = dot(r3.zxy, c254.xyz);
													r2.xyz = r2.xyz * r0.www;
													p0 = r0.z != 0.0;
													ps = p0 ? 0.0 : 1.0;
													r1.xyz = r2.xyz * g_vSampleWeights(11).xyz + r1.yzx;
													if (p0)
													{
														r0.xy = r0.xy + g_vSampleOffsets(12).xy;
														r3.xyz = tfetch2D(ScreenTexture_Texture2DDescriptorIndex, ScreenTexture_SamplerDescriptorIndex, r0.xy, float2(0, 0)).xyz;
														r0.xyz = r3.zxy * g_vSampleWeights(12).zxy + r1.zxy;
														r2.xyz = r3.xyz + -c252.www;
														r1.xyz = min(r3.xyz, c252.www);
														r1.xyz = r1.xyz * c253.www;
														r2.xyz = max(r2.xyz, c253.zzz);
														r0.w = dot(r2.zxy, c254.xyz);
														r1.xyz = r1.xyz * r0.www;
														r1.xyz = r1.xyz * g_vSampleWeights(12).xyz + r0.yzx;
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	oC0.xyz = max(r1.xyz, r1.xyz);
	oC0.w = 1.0;
	// X360 saturates this bright-mask on the EDRAM export before the bloom
	// blur, and unclamped FP16 lets BriMulti-scaled masks (up to ~59 on wc01)
	// blur into giant white blobs.
	oC0.xyz = min(oC0.xyz, 1.0);
	[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)	{		clip(oC0.w - g_AlphaThreshold);
	}	return;
}
