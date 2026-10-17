// FXAA, after Timothy Lottes' FXAA 3.11 quality path: find the local edge and
// its orientation from luma, walk along it to both ends, and resample the
// pixel across the edge in proportion to where it sits along the span. A
// sub-pixel term softens single-pixel features the edge walk can't see.
#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

// Minimum local contrast, relative and absolute, for a pixel to be processed.
static const float kEdgeThreshold = 0.125;
static const float kEdgeThresholdMin = 0.0312;
// Strength of the sub-pixel blend (0 off, 1 full).
static const float kSubpixel = 0.75;
// Edge walk: step sizes in pixels, growing as the walk gets longer.
static const uint kSearchSteps = 12u;
static const float kStepScale[kSearchSteps] = {
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    SamplerState s = g_SamplerDescriptorHeap[0];
    float2 dims;
    tex.GetDimensions(dims.x, dims.y);
    float2 texel = 1.0 / dims;

    float4 centre = tex.SampleLevel(s, texCoord, 0.0);
    float lumaM = Luma(centre.rgb);
    float lumaN = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(0, -1)).rgb);
    float lumaS = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(0, 1)).rgb);
    float lumaW = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(-1, 0)).rgb);
    float lumaE = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(1, 0)).rgb);

    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float range = lumaMax - lumaMin;
    if (range < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold))
        return centre;

    float lumaNW = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(-1, -1)).rgb);
    float lumaNE = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(1, -1)).rgb);
    float lumaSW = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(-1, 1)).rgb);
    float lumaSE = Luma(tex.SampleLevel(s, texCoord, 0.0, int2(1, 1)).rgb);

    // Edge orientation: compare the second differences across each axis.
    float edgeHorizontal = abs(lumaNW + lumaSW - 2.0 * lumaW) +
                           2.0 * abs(lumaN + lumaS - 2.0 * lumaM) +
                           abs(lumaNE + lumaSE - 2.0 * lumaE);
    float edgeVertical = abs(lumaNW + lumaNE - 2.0 * lumaN) +
                         2.0 * abs(lumaW + lumaE - 2.0 * lumaM) +
                         abs(lumaSW + lumaSE - 2.0 * lumaS);
    bool horizontal = edgeHorizontal >= edgeVertical;

    // Which side of the pixel the edge lies on: the steeper neighbour.
    float luma1 = horizontal ? lumaN : lumaW;
    float luma2 = horizontal ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool side1 = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));
    float stepLength = horizontal ? texel.y : texel.x;
    float lumaLocalAverage;
    if (side1)
    {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocalAverage = 0.5 * (luma2 + lumaM);
    }

    // Start on the edge itself, half a pixel towards that side.
    float2 edgeUv = texCoord;
    if (horizontal)
        edgeUv.y += 0.5 * stepLength;
    else
        edgeUv.x += 0.5 * stepLength;

    // Walk both ways along the edge until the luma leaves the edge's average.
    float2 offset = horizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 uv1 = edgeUv - offset * kStepScale[0];
    float2 uv2 = edgeUv + offset * kStepScale[0];
    float lumaEnd1 = Luma(tex.SampleLevel(s, uv1, 0.0).rgb) - lumaLocalAverage;
    float lumaEnd2 = Luma(tex.SampleLevel(s, uv2, 0.0).rgb) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    [loop] for (uint i = 1u; i < kSearchSteps && !(reached1 && reached2); ++i)
    {
        if (!reached1)
        {
            uv1 -= offset * kStepScale[i];
            lumaEnd1 = Luma(tex.SampleLevel(s, uv1, 0.0).rgb) - lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2)
        {
            uv2 += offset * kStepScale[i];
            lumaEnd2 = Luma(tex.SampleLevel(s, uv2, 0.0).rgb) - lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
    }

    // Blend across the edge by how close the nearer end is.
    float distance1 = horizontal ? texCoord.x - uv1.x : texCoord.y - uv1.y;
    float distance2 = horizontal ? uv2.x - texCoord.x : uv2.y - texCoord.y;
    bool nearer1 = distance1 < distance2;
    float distanceNearest = min(distance1, distance2);
    float pixelOffset = 0.5 - distanceNearest / (distance1 + distance2);
    // Only when the nearer end's luma moves away from this pixel's, so the
    // blend goes towards the side the edge really lies on.
    bool centreSmaller = lumaM < lumaLocalAverage;
    bool correct = ((nearer1 ? lumaEnd1 : lumaEnd2) < 0.0) != centreSmaller;
    float finalOffset = correct ? pixelOffset : 0.0;

    // Sub-pixel aliasing: contrast against the 3x3 neighbourhood average.
    float lumaAverage = (2.0 * (lumaN + lumaS + lumaW + lumaE) +
                         lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
    float subpixel1 = saturate(abs(lumaAverage - lumaM) / range);
    float subpixel2 = (-2.0 * subpixel1 + 3.0) * subpixel1 * subpixel1;
    finalOffset = max(finalOffset, subpixel2 * subpixel2 * kSubpixel);

    float2 finalUv = texCoord;
    if (horizontal)
        finalUv.y += finalOffset * stepLength;
    else
        finalUv.x += finalOffset * stepLength;
    float4 result = tex.SampleLevel(s, finalUv, 0.0);
    return float4(result.rgb * g_PushConstants.Param0, centre.a);
}
