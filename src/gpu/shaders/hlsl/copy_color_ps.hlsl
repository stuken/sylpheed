#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

static const uint kMaxBoxTaps = 8u;

// Cubic B-spline reconstruction from four bilinear taps (GPU Gems 2, ch. 20).
// Each axis folds its four texel weights into two taps placed between texel
// centres, so the result is C2-smooth: no kinks at texel edges, no ringing.
float4 SampleBSpline(Texture2D<float4> tex, float2 texCoord)
{
    float2 dims;
    tex.GetDimensions(dims.x, dims.y);
    float2 p = texCoord * dims - 0.5;
    float2 i = floor(p);
    float2 f = p - i;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;
    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    // Texel k's centre is at k + 0.5.
    float2 t0 = (i - 0.5 + w1 / g0) / dims;
    float2 t1 = (i + 1.5 + w3 / g1) / dims;
    SamplerState s = g_SamplerDescriptorHeap[0];
    return g0.y * (g0.x * tex.SampleLevel(s, float2(t0.x, t0.y), 0.0) +
                   g1.x * tex.SampleLevel(s, float2(t1.x, t0.y), 0.0)) +
           g1.y * (g0.x * tex.SampleLevel(s, float2(t0.x, t1.y), 0.0) +
                   g1.x * tex.SampleLevel(s, float2(t1.x, t1.y), 0.0));
}

// Param1 picks the filter: a negative value is a smooth (B-spline) upscale,
// an integer ratio of 2 or more a box downsample, anything else one bilinear
// tap.
float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    if (g_PushConstants.Param1 < 0.0)
    {
        float4 s = SampleBSpline(tex, texCoord);
        return float4(s.rgb * g_PushConstants.Param0, s.a);
    }
    uint ratio = (uint)(g_PushConstants.Param1 + 0.5);
    float3 rgb;
    float a;
    if (ratio >= 2u)
    {
        uint taps = min(ratio, kMaxBoxTaps);
        int2 base = int2(position.xy) * (int)ratio;
        float4 acc = 0.0;
        [loop] for (uint y = 0u; y < taps; ++y)
        {
            int oy = (int)((y * ratio + ratio / 2u) / taps);
            [loop] for (uint x = 0u; x < taps; ++x)
            {
                int ox = (int)((x * ratio + ratio / 2u) / taps);
                acc += tex.Load(int3(base.x + ox, base.y + oy, 0));
            }
        }
        rgb = acc.rgb / (float)(taps * taps);
        a = acc.a / (float)(taps * taps);
    }
    else
    {
        float4 s = tex.Sample(g_SamplerDescriptorHeap[0], texCoord);
        rgb = s.rgb;
        a = s.a;
    }
    return float4(rgb * g_PushConstants.Param0, a);
}
