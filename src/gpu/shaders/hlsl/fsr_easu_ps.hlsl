// The present pass with AMD FidelityFX Super Resolution 1's EASU upscaler (2026-11-12, Switch dynamic resolution):
// the rendered image (any size up to the output's) to the output viewport, then the same display ramp as
// gamma_correction_ps.hlsl. Used instead of that shader's bilinear sample when the image is smaller than the output
// (bd_switch_fsr).
//
// EASU is ported from ffx_fsr1.h (FsrEasuCon, FsrEasuTapF, FsrEasuSetF, FsrEasuF, the 32-bit version) and the
// approximations it takes from ffx_a.h (APrxLoRcpF1, APrxLoRsqF1), FidelityFX-FSR 1.0.2:
//
//   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
//   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
//   the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
//   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
//   IN THE SOFTWARE.
//
// Push constants (copy_common.hlsli): ResourceDescriptorIndex the image, ResourceDescriptorIndex2 the output
// viewport's size (width | height << 16), Param0 / Param1 as in gamma_correction_ps.

#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

// ffx_a.h: APrxLoRcpF1, APrxLoRsqF1.
float FsrRcpLo(float a) { return asfloat(0x7ef07ebbu - asuint(a)); }
float FsrRsqLo(float a) { return asfloat(0x5f347d74u - (asuint(a) >> 1u)); }

void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    float2 v;
    v.x = (off.x * ( dir.x)) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 + (-1.0);
    float wA = lob * d2 + (-1.0);
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB + (-(25.0 / 16.0 - 1.0));
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

// The bilinear weight's corner is a parameter here (0 s, 1 t, 2 u, 3 v) instead of four booleans.
void FsrEasuSet(inout float2 dir, inout float len, float2 pp, int corner,
                float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (corner == 0) w = (1.0 - pp.x) * (1.0 - pp.y);
    if (corner == 1) w = pp.x * (1.0 - pp.y);
    if (corner == 2) w = (1.0 - pp.x) * pp.y;
    if (corner == 3) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = FsrRcpLo(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = FsrRcpLo(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float3 FsrEasu(Texture2D<float4> tex, SamplerState smp, uint2 ip, float2 inSize, float2 outSize)
{
    // FsrEasuCon (the input viewport is the whole image).
    const float2 scale = inSize / outSize;
    const float2 bias = 0.5 * inSize / outSize - 0.5;
    const float2 rcpIn = 1.0 / inSize;

    float2 pp = float2(ip) * scale + bias;
    float2 fp = floor(pp);
    pp -= fp;
    float2 p0 = fp * rcpIn + float2(1.0, -1.0) * rcpIn;
    float2 p1 = p0 + float2(-1.0, 2.0) * rcpIn;
    float2 p2 = p0 + float2(1.0, 2.0) * rcpIn;
    float2 p3 = p0 + float2(0.0, 4.0) * rcpIn;
    float4 bczzR = tex.GatherRed(smp, p0);
    float4 bczzG = tex.GatherGreen(smp, p0);
    float4 bczzB = tex.GatherBlue(smp, p0);
    float4 ijfeR = tex.GatherRed(smp, p1);
    float4 ijfeG = tex.GatherGreen(smp, p1);
    float4 ijfeB = tex.GatherBlue(smp, p1);
    float4 klhgR = tex.GatherRed(smp, p2);
    float4 klhgG = tex.GatherGreen(smp, p2);
    float4 klhgB = tex.GatherBlue(smp, p2);
    float4 zzonR = tex.GatherRed(smp, p3);
    float4 zzonG = tex.GatherGreen(smp, p3);
    float4 zzonB = tex.GatherBlue(smp, p3);
    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;
    float2 dir = 0.0;
    float len = 0.0;
    FsrEasuSet(dir, len, pp, 0, bL, eL, fL, gL, jL);
    FsrEasuSet(dir, len, pp, 1, cL, fL, gL, hL, kL);
    FsrEasuSet(dir, len, pp, 2, fL, iL, jL, kL, nL);
    FsrEasuSet(dir, len, pp, 3, gL, jL, kL, lL, oL);
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = FsrRsqLo(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) * FsrRcpLo(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 + (-0.5) * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = FsrRcpLo(lob);
    float3 min4 = min(min(min(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w)),
                          float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));
    float3 max4 = max(max(max(float3(ijfeR.z, ijfeG.z, ijfeB.z), float3(klhgR.w, klhgG.w, klhgB.w)),
                          float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));
    float3 aC = 0.0;
    float aW = 0.0;
    FsrEasuTap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x)); // b
    FsrEasuTap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y)); // c
    FsrEasuTap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x)); // i
    FsrEasuTap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y)); // j
    FsrEasuTap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z)); // f
    FsrEasuTap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w)); // e
    FsrEasuTap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x)); // k
    FsrEasuTap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y)); // l
    FsrEasuTap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z)); // h
    FsrEasuTap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w)); // g
    FsrEasuTap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z)); // o
    FsrEasuTap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w)); // n
    return min(max4, max(min4, aC * (1.0 / aW)));
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint2 inDims;
    tex.GetDimensions(inDims.x, inDims.y);
    const uint packed = g_PushConstants.ResourceDescriptorIndex2;
    const float2 outSize = float2(packed & 0xFFFFu, packed >> 16);
    // The output pixel from the interpolated 0..1 coordinate of the viewport (its centers are (i + 0.5) / size).
    const uint2 ip = uint2(min(texCoord * outSize, outSize - 1.0));
    float3 rgb = FsrEasu(tex, g_SamplerDescriptorHeap[0], ip, float2(inDims), outSize);

    // gamma_correction_ps.hlsl's display ramp.
    float3 ramp = pow(max(rgb, 0.0), g_PushConstants.Param0);
    float3 lin = lerp(ramp / 12.92, pow((ramp + 0.055) / 1.055, 2.4), step(0.04045, ramp));
    float3 corrected = lerp(lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099, step(0.018, lin));
    return float4(lerp(ramp, corrected, g_PushConstants.Param1), 1.0);
}
