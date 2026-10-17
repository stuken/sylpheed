#include "copy_common.hlsli"

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> source = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    float2 dims;
    source.GetDimensions(dims.x, dims.y);
    // Source texels per output pixel. sylpheed renders at a resolution scale (and
    // twice that under supersampling), so this pass usually shrinks. One
    // bilinear tap is an exact box filter at 2:1 but skips texels beyond it,
    // so larger footprints average a grid of bilinear taps spanning the pixel.
    float2 footprint = abs(float2(ddx(texCoord.x), ddy(texCoord.y))) * dims;
    float4 sampled;
    if (footprint.x > 2.0 || footprint.y > 2.0)
    {
        // Each bilinear tap covers two texels per axis.
        int2 taps = clamp(int2(ceil(footprint * 0.5)), 1, 4);
        float2 step = footprint / (taps * dims);
        float2 origin = texCoord - 0.5 * footprint / dims + 0.5 * step;
        sampled = 0.0;
        for (int y = 0; y < taps.y; ++y)
            for (int x = 0; x < taps.x; ++x)
                sampled += source.SampleLevel(g_SamplerDescriptorHeap[0],
                                              origin + float2(x, y) * step, 0.0);
        sampled /= taps.x * taps.y;
    }
    else
    {
        sampled = source.SampleLevel(g_SamplerDescriptorHeap[0], texCoord, 0.0);
    }
    // max() guards pow() against NaN on negative HDR values (source RT may be R16F).
    float3 ramp = pow(max(sampled.rgb, 0.0), g_PushConstants.Param0);
    // X360 scanout: D3D reshapes the ramp for the display before uploading it
    // to the display controller (SYLPHEED: sub_8225EDD8): for display-gamma type 2
    // (HDTV) the hardware LUT is Rec709_encode(sRGB_decode(ramp)), matching
    // the guest's own table to within one 10-bit step. Param1 blends from the
    // raw frame (0) to that corrected output (1, sylpheed_display_correction).
    float3 lin = lerp(ramp / 12.92, pow((ramp + 0.055) / 1.055, 2.4),
                      step(0.04045, ramp));
    float3 corrected = lerp(lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099,
                            step(0.018, lin));
    sampled.rgb = lerp(ramp, corrected, g_PushConstants.Param1);
    // The swapchain is 8 bits per channel; under sylpheed_output_16bit the source
    // carries more. A 4x4 ordered dither under half a step turns the rounding
    // into fine noise instead of contour bands, and leaves a value that is
    // already a whole 8-bit step (all of an 8-bit source) unchanged.
    static const float kBayer4[16] = {
        0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
        3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0 };
    uint2 cell = uint2(position.xy) & 3u;
    float dither = (kBayer4[cell.y * 4u + cell.x] + 0.5) / 16.0 - 0.5;
    sampled.rgb += dither / 255.0;
    return float4(sampled.rgb, 1.0);
}
