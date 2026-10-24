#include "common.hlsli"

CommonVertex vs_main(uint id: SV_VERTEXID)
{
	CommonVertex output;
	vsScreenQuad(id, output.position, output.texcoord);

	return output;

}