#include "common.hlsli"

cbuffer Constants : register(b0)
{
    float  scale;         // Scale factor for visualization
    uint   channel_mask;  // Bitmask: bit0=R, bit1=G, bit2=B, bit3=A
    uint   show_abs;      // Show absolute values
    uint   padding;
    float2 mvecScale;
    float2 padding2;
} //TODO none of resrouces actually tells that it has to me 16 byes aligned for root Signature


Texture2D<float2> mvec : register(t0);
// Texture2D<float> depthBuffer : register(t1);
// Texture2D<float2> mvecProcessed : register(t2);

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

float4 ps_main(CommonVertex inp) : SV_TARGET {
    float2 modifiedUV = inp.texcoord;

    float2 result = mvec.Sample(PointSampler, modifiedUV);
    float4 outputColor = float4(0, 0, 0, 1);
    result = result * mvecScale;
    result = result * scale;

    if(show_abs) {
        result = abs(result);
        outputColor.r = result.x;
        outputColor.g = result.y;
        return outputColor;
    }

    if (channel_mask == 0) { // horizontal motion
        if(result.x > 0) {
            outputColor.r = result.x;
        } else {
            outputColor.g = -result.x;
        }
        return outputColor;
    } else {
       if(result.y > 0) {
            outputColor.b = result.y;
        } else {
            outputColor.g = -result.y;
        }
        return outputColor;
    }
    return outputColor;
}
