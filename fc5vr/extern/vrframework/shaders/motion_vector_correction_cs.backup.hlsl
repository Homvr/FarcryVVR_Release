cbuffer Constants : register(b0)
{
    float4x4 correctionMatrixCurrent;
    float4x4 correctionMatrixPast;
    float4 textureSize;
}

// Input textures
Texture2D<float2> mvPast : register(t0);
Texture2D<float2> mvCurrent : register(t1);
Texture2D<float> depthPast : register(t2);
Texture2D<float> depthCurrent : register(t3);

// Output texture
RWTexture2D<float2> correctedMv : register(u0);

// correct motion vector is MV = P_Current - P_Previous
[numthreads(16, 16, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    uint2 pixelId = DTid.xy;
    float2 pixelCenter = float2(pixelId)+0.5;

    if (pixelId.x >= textureSize.x || pixelId.y >= textureSize.y)
    {
        return;
    }

    float2 motionVector = mvCurrent[pixelId].xy;

//     {
//         float2 uvCurrent = pixelCenter * textureSize.zw;
//         float2 ssCurrent = float2(uvCurrent.x * 2.0 - 1.0, 1.0 - uvCurrent.y * 2.0);
//         float2 ssPast = ssCurrent - motionVector;
//         ssPast = min(ssPast, float2(1.0, 1.0));
//         ssPast = max(ssPast, float2(-1.0, -1.0));
//
//         float2 uvPast = float2(0.5, -0.5) * ssPast + 0.5;
//         uint2 maxSize = uint2(textureSize.x - 1, textureSize.y - 1);
//         uint2 offsetCoord = min(uint2(uvPast * textureSize.xy), maxSize);
//         float pastDepth = depthCurrent[pixelId].x;
//         float4 screenSpacePosPast = float4(ssPast, pastDepth, 1.0);
//         float4 screenSpacePosPastReprojected = mul(correctionMatrixPast, screenSpacePosPast);
//         float2 ssPastReprojected = screenSpacePosPastReprojected.xy / screenSpacePosPastReprojected.w;
//         correctedMv[pixelId] =  ssCurrent - ssPastReprojected;
//     }

     {
        float2 uvCurrent = pixelCenter * textureSize.zw;
        float2 ssCurrent = float2(uvCurrent.x * 2.0 - 1.0, 1.0 - uvCurrent.y * 2.0);

        float curDepth = depthCurrent[pixelId];
        float4 screenSpaceCurrent = float4(ssCurrent, curDepth, 1.0);
        float4 screenSpaceEyeOffsetReprojected = mul(correctionMatrixCurrent, screenSpaceCurrent);
        float2 ssEyeOffsetReprojected = screenSpaceEyeOffsetReprojected.xy / screenSpaceEyeOffsetReprojected.w;
        float2 eyeMotionVector = ssCurrent - ssEyeOffsetReprojected;
        correctedMv[pixelId] =  motionVector - eyeMotionVector;
    }

//     float2 texCoord = pixelCenter * textureSize.zw;
//     uint2 maxSize = uint2(textureSize.x - 1, textureSize.y - 1);
//     uint2 offsetCoord = min(uint2(( texCoord - (motionVector * float2(-0.5, 0.5))) * textureSize.xy), maxSize);
//     float2 delta = mvPast[offsetCoord];
//     float2 finalMotionVector = motionVector + delta;
//     correctedMv[pixelId] = finalMotionVector;
}