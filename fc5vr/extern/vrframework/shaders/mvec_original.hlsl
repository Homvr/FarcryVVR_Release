// Constant Buffer definition matching the DXBC dump
cbuffer ConstBuf__passData : register(b0)
{
    float4x4 projectionToWorld;        // Offset:    0
    float4x4 previousWorldToProjection;    // Offset:   64
    float3   clearColor;                 // Offset:  128
    bool     fogBackground;              // Offset:  140 (Not used in this shader)
    bool     drawEnvMap;                 // Offset:  144
    float3   eyePosWS;                   // Offset:  148
    bool     useReflectionPlane;         // Offset:  160
    float3   envmapTint;                 // Offset:  164
    float    envmapIntensity;            // Offset:  176
    float2x2 envmapRotation;             // Offset:  192
    float2   viewportScale;              // Offset:  216
    float2   viewportOffset;             // Offset:  224
    float4   reflectionPlaneWS;          // Offset:  240

    // AtmosphericConstants are defined in the cbuffer but not used by this specific shader.
    // ... (offsets 256 to 460)

    int      mipOffset;                  // Offset:  464
};

// Resource bindings
TextureCube environmentMap : register(t1);
SamplerState envmapSampler : register(s1);

// Input from the Vertex Shader
struct PS_INPUT
{
    float4 position   : SV_POSITION;
    // The assembly uses v1.zw, which corresponds to the second texture coordinate set.
    // This is likely the screen UVs in [0,1] range passed from a full-screen triangle.
    float2 texcoord0  : TEXCOORD0; // v1.xy (unused)
    float2 screenUV   : TEXCOORD1; // v1.zw (used)
};

// Output to multiple render targets (MRTs)
struct PS_OUTPUT
{
    float4 Color        : SV_Target0;
    float2 MotionVector : SV_Target1;
};

// Decompiled Pixel Shader Main Function
PS_OUTPUT main(PS_INPUT input)
{
    PS_OUTPUT output;

    //=========================================================================
    // Part 1: Motion Vector Calculation
    //=========================================================================

    // Reconstruct current screen position in Normalized Device Coordinates (NDC) [-1, 1]
    // The assembly does: (input.screenUV + viewportOffset) * viewportScale
    float2 currentNdc = (input.screenUV + viewportOffset) * viewportScale;

    // Create a 4D clip-space vector for the current pixel.
    // Z is set to a very small value to represent the near plane.
    float4 currentClipPos = float4(currentNdc, 0.000001f, 1.0f);

    // Unproject the current pixel's clip-space position back to world space.
    float4 worldPosH = mul(currentClipPos, projectionToWorld);
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // Project the world position using the *previous* frame's world-to-projection matrix.
    float4 prevClipPos = mul(float4(worldPos, 1.0f), previousWorldToProjection);

    // Calculate motion vector, with a check to prevent division by zero.
    float2 motionVector = float2(0, 0);
    if (abs(prevClipPos.w) > 0.0f)
    {
        // Perform perspective divide to get the previous frame's NDC.
        float2 prevNdc = prevClipPos.xy / prevClipPos.w;

        // The motion vector is the difference between the current and previous NDC positions.
        motionVector = currentNdc - prevNdc;
    }

    // Write the motion vector to the second render target.
    output.MotionVector = motionVector;


    //=========================================================================
    // Part 2: Background/Sky Color Calculation
    //=========================================================================

    float3 finalColor;

    if (drawEnvMap)
    {
        // Calculate the view direction from the camera to the world position.
        float3 viewDir = worldPos - eyePosWS;

        float3 sampleDir = normalize(viewDir);

        // If using a reflection plane, calculate the reflected direction.
        if (useReflectionPlane)
        {
            // The assembly implements the reflect() function manually.
            // reflect(I, N) = I - 2 * dot(I, N) * N
            // Here, I = sampleDir, N = reflectionPlaneWS.xyz
            float3 reflectedDir = reflect(sampleDir, reflectionPlaneWS.xyz);
            sampleDir = reflectedDir;
        }

        // Apply 2D rotation to the environment map lookup direction on the XZ plane.
        sampleDir.xz = mul(sampleDir.xz, envmapRotation);

        // Calculate the mip level for sampling the cubemap.
        // This allows for programmatic blurring of the environment map.
        uint numMips, width, height, depth;
        environmentMap.GetDimensions(0, width, height, depth, numMips);
        float mipLevel = max(0.0, (float)(numMips - mipOffset));

        // Sample the environment map.
        float3 envColor = environmentMap.SampleLevel(envmapSampler, sampleDir, mipLevel).rgb;

        // Apply tint and intensity.
        finalColor = envColor * envmapTint * envmapIntensity;
    }
    else
    {
        // If not drawing the environment map, just use the solid clear color.
        finalColor = clearColor;
    }

    output.Color.rgb = finalColor;

    // The shader sets W to a large negative number. This is a common technique
    // in deferred rendering to tag sky/background pixels so they can be ignored
    // by subsequent screen-space effects (like SSAO) that rely on depth.
    output.Color.a = -1000000.0f;

    return output;
}