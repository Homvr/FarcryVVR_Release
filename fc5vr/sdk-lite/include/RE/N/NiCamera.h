#pragma once
#include "NiAVObject.h"
#include "NiPoint2.h"
#include "NiRect.h"
#include "RE/N/NiPoint3.h"
namespace RE
{

	class NiFrustum
	{
	public:
		float left;    // 00
		float right;   // 04
		float top;     // 08
		float bottom;  // 0C
		float _near;   // 10
		float _far;    // 14
	};
	static_assert(sizeof(NiFrustum) == 0x18);

	class NiCamera : public NiAVObject
	{
	public:
//		SF_RTTI_VTABLE(NiCamera);

		virtual ~NiCamera() = default;

		virtual void* createClone(NiCloneProcess* cloningProcess) override;
		//leftBoundary and rightBoundary are outputted based on the W (radius) component of worldPt.
		//If the W component is small enough, they are essentially equal, and point to the exact
		//XYZ location provided.
		//The returned NiPoint2 does not contain any world-to-screen information and can be ignored.
		/*NiPoint2 WorldToScreenInternal(const NiPoint4* worldPt, NiPoint3* leftBoundary, NiPoint3* rightBoundary)
		{
			using func_t = decltype(&NiCamera::WorldToScreenInternal);
			REL::Relocation<func_t> func{ REL::ID(210415) };
			return func(this, worldPt, leftBoundary, rightBoundary);
		}

		void SetNiFrustumInternal(NiFrustum* frustum)
		{
			using func_t = decltype(&NiCamera::SetNiFrustumInternal);
			REL::Relocation<func_t> func{ REL::ID(210419) };
			return func(this, frustum);
		}

		void SetViewport(NiRect<float>* viewport)
		{
			using func_t = decltype(&NiCamera::SetViewport);
			REL::Relocation<func_t> func{ ID::NiCamera::SetViewport };
			return func(this, viewport);
		}

		void CalcFrustum(float fov, float aspectRatio, float nearDist, float farDist, char lodAdjust)
		{
			using func_t = decltype(&NiCamera::CalcFrustum);
			REL::Relocation<func_t> func{ ID::NiCamera::CalcFrustum };
			return func(this, fov, aspectRatio, nearDist, farDist, lodAdjust);
		}

		void SetScissors(NiRect<float>* scissors)
		{
			using func_t = decltype(&NiCamera::SetScissors);
			REL::Relocation<func_t> func{ ID::NiCamera::SetScissors };
			return func(this, scissors);
		} 

		void RegisterAsRenderCamera()
		{
			using func_t = decltype(&NiCamera::RegisterAsRenderCamera);
			REL::Relocation<func_t> func{ ID::NiCamera::registerAsRendererCamera };
			return func(this);
		}

		void SetNearFar(float nearDist, float farDist)
		{
			using func_t = decltype(&NiCamera::SetNearFar);
			REL::Relocation<func_t> func{ ID::NiCamera::SetNearFar };
			return func(this, nearDist, farDist);
		}

		// 0 - perspective
		void SetClipspaceType(uint8_t type)
		{
			using func_t = decltype(&NiCamera::SetClipspaceType);
			REL::Relocation<func_t> func{ ID::NiCamera::SetClipspaceType };
			return func(this, type);
		}

		void Translate(NiPoint3& translate, NiPoint3& unk) {
			using func_t = decltype(&NiCamera::Translate);
			REL::Relocation<func_t> func{ ID::NiCamera::Translate };
			return func(this, translate, unk);

		}

		NiCamera* CloneNonVirtual()
		{
			using func_t = decltype(&NiCamera::CloneNonVirtual);
			REL::Relocation<func_t> func{ ID::NiCamera::CreateClone };
			return func(this);
		}

		static NiCamera* BuildRendererCamera(char* name) {
			auto camera = Construct();
			//camera->SetName(name);
			camera->RegisterAsRenderCamera();
			return camera;
		}

		static NiCamera* Construct()
		{
			using func_t = decltype(&NiCamera::Construct);
			REL::Relocation<func_t> func{ ID::NiCamera::Construct };
			return func();
		}*/

		//The returned X and Y are in the range -1 to 1, with 1 being top right and -1 being bottom left.
		//The returned Z is the distance from the camera to the worldPt, with negative values meaning the
		//camera is facing away from the worldPt.
		//Note: The Z component is very small due to Starfield's unit scale.
		//i.e. distance from player to camera when the camera is close is ~0.0004
		NiPoint3 WorldToScreen(const NiPoint3& a_worldPt)
		{
			NiPoint3 result{ 0.0f, 0.0f, -1.0f };
			float    worldDiffRotated = (((a_worldPt.y - world.translate.y) * world.rotate[0][1]) +
                                         ((a_worldPt.x - world.translate.x) * world.rotate[0][0]) +
                                         ((a_worldPt.z - world.translate.z) * world.rotate[0][2])) -
			                         viewFrustum._near;

			result.z = worldDiffRotated * (1.0f / (viewFrustum._far - viewFrustum._near));

			float trace = (a_worldPt.x * worldToCam[3][0]) + (a_worldPt.y * worldToCam[3][1]) + ((a_worldPt.z * worldToCam[3][2]) + worldToCam[3][3]);
			if (trace <= 0.00001f) {
				return result;
			}

			float traceInv = 1.0f / trace;
			result.x = (((a_worldPt.y * worldToCam[0][1]) + (a_worldPt.x * worldToCam[0][0])) + ((a_worldPt.z * worldToCam[0][2]) + worldToCam[0][3])) * traceInv;
			result.y = (((a_worldPt.y * worldToCam[1][1]) + (a_worldPt.x * worldToCam[1][0])) + ((a_worldPt.z * worldToCam[1][2]) + worldToCam[1][3])) * traceInv;
			return result;
		}

		//Same as WorldToScreen, but normalizes X and Y to the 0 to 1 range,
		//and flips the Y component so that 0 is top left and 1 is bottom right.
		NiPoint3 WorldToScreenNormalized(const NiPoint3& a_worldPt)
		{
			auto result = WorldToScreen(a_worldPt);
			result.x = (result.x + 1.0f) * 0.5f;
			result.y = -result.y;
			result.y = (result.y + 1.0f) * 0.5f;
			return result;
		}

		virtual void* UpdateWorldData(NiUpdateData* data) override;
		virtual void* UpdateTransformAndBounds(NiUpdateData* data) override;  // calls UpdateWorldData if flags & 4 == 0 also calls unk78 with 0 also flips flags if they are in specific state
		virtual void* UpdateTransforms(NiUpdateData* data) override;          // calls UpdateWorldData if flags & 4 != 0
		virtual void* updateWorldBoundFromWorldTranslate();

		uint32_t      cameraHandleID{ 0xFFFFFF };  //130
		uint8_t       pad_0133[12];                //134
		float         offsetMatrix[4][4];          // 140 diagonal 1.0f
		float         worldToCam[4][4];            // 180 last column changes when moving
		NiFrustum     viewFrustum;                 // 0x1C0
		uint8_t       clipSpaceType;               // 0 - perspective
		float         minNearPlaneDist;            // 1DC
		float         maxFarNearRatio;             // 1E0
		NiRect<float> viewPort;                    // 1E4
		NiRect<float> scissors;                    // 1F4
		float         m_fLODAdjust;                // 204
		uint8_t       flags2;                      // 210
		uint8_t       pad_0211[20];
	};
	static_assert(offsetof(NiCamera, cameraHandleID) == 0x130);
	static_assert(offsetof(NiCamera, offsetMatrix) == 0x140);
	static_assert(offsetof(NiCamera, viewFrustum) == 0x1C0);
	static_assert(offsetof(NiCamera, viewPort) == 0x1E4);
	static_assert(sizeof(NiCamera) == 0x220);
	static_assert(offsetof(NiCamera, worldToCam) == 384);
}
