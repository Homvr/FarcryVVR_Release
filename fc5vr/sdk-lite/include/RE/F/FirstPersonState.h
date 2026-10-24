#pragma once

#include <RE/N/NiQuaternion.h>

namespace RE
{

    class NiCamera;
    class NiNode;
    class PlayerCamera;

	class FirstPersonState
	{
	public:
		virtual ~FirstPersonState();  // 00
        char pad8[0x40];
        RE::PlayerCamera* pPlayerCamera; // 48
        void* unk50;
		//members
		std::uint64_t  pitchFlightCameraJoy;               //0x0058
		std::uint64_t rollFlightCameraJoy;               //0x0060
		float     N00000A24;               //0x0068
		std::uint32_t  _pad2;                   //0x006C
		NiCamera* pNiCamera;               //0x0070
		float     position[3];             //0x0078
		float     N000007A7;               //0x0084
		std::uint64_t  N000006ED;               //0x0088
		std::uint64_t  N000006EE;               //0x0090
		std::uint32_t  N000006EF;               //0x0098
		std::uint32_t  _pad3;                   //0x009C
		NiCamera* cameraRoot;              //0x00A0 worldRoot
		NiNode*   pNiNode;                 //0x00A8
		NiNode*   pNiNode2;                //0x00B0
		NiPoint3* cameraOffset;               //0x00B8 ->x yaw, -> y pitch
		float     cameraPitch2Multiplier;  //0x00C0 //multiplier for camera_pitch
		float     N00000812;               //0x00C4
		float     camera_pitch;            //0x00C8 -120 to 120
		float     camera_pitch2;           //0x00CC
		float     N00000819;               //0x00D0
		float     N000006F6;               //0x00D4
		float     N00000820;               //0x00D8
		float     N000006F7;               //0x00DC
		std::uint16_t  N00000823;               //0x00E0
		std::uint8_t   N00000825;               //0x00E2
		char      pad_00E3[1];             //0x00E3
		std::uint64_t  N000006F8;               //0x00E4
		std::uint8_t   N00000836;               //0x00EC
		char      pad_00ED[3];             //0x00ED
	};
	static_assert(offsetof(FirstPersonState, pitchFlightCameraJoy) == 0x58);
	static_assert(offsetof(FirstPersonState, camera_pitch) == 0xC8);
	static_assert(offsetof(FirstPersonState, pNiCamera) == 0x70);
	//check_size<offsetof(FirstPersonState, camera_pitch), 0xC8> trace_size11;

}
