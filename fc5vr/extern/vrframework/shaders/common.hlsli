struct CommonVertex
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD0;
};

void vsScreenQuad(uint vertexId : SV_VERTEXID, out float4 position : SV_POSITION, out float2 texcoord : TEXCOORD0)
{
    float4 positions[6] = {
        { -1.0f, -1.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 1.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 1.0f, 1.0f },
        {  1.0f,  1.0f, 1.0f, 1.0f }
    };

    float2 texcoords[6] = {
        { 0.0f,  1.0f },
        { 0.0f,  0.0f },
        { 1.0f,  1.0f },
        { 1.0f,  1.0f },
        { 0.0f,  0.0f },
        { 1.0f,  0.0f }
    };

    position = positions[vertexId % 6];
    texcoord = texcoords[vertexId % 6];
}
