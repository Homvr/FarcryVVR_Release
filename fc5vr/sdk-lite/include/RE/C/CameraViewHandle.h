#pragma once

#include "RE/C/CreationRenderer.h"
#include "RE/C/CreationRendererPrivate.h"
#include "RE/S/StorageTable.h"

namespace RE::StorageTable
{
	struct alignas(0) HeightfieldSettings
	{
		uint8_t data[0x10];
	};
	static_assert(sizeof(HeightfieldSettings) == 16);

		struct alignas(0) AverageLuminanceFutureReadback
	{
		uint8_t data[0x10];
	};
	static_assert(sizeof(AverageLuminanceFutureReadback) == 16);

	struct alignas(4) CameraViewData
	{
		uint32_t sceneId;
		uint32_t cameraViewID;
		uint8_t  unk8{ 1 };      // enabled? 0x8
		float    fov{ 85.0 };    // 0xC default is 54.01 maybe it is not fov
		float    cameraNear{ 0.1 };    // 0x10
		float    cameraFar{ 4000.0 };  // 0x14
		uint16_t unk18{ 1 };     // 0x18
	};                           // 28 size correlates with
	static_assert(sizeof(CameraViewData) == 0x1C);

	struct ClearColor
	{
		void* data;  // unconfirmed
	};

	struct alignas(0) HighlightSettings
	{
		void* data;  // unconfirmed
	};

	namespace CameraView
	{
		template <typename ST>
		struct ObserverCameraViewHostOnlyTables : public DirectColumnDataStorageInterface
		{
			HostOnlyMemory<ST, CreationRendererPrivate::SkyCubemapSystem::SkyCubemapData>*     pSkyCubeMapData;                // 0x0 CreationRendererPrivate::SkyCubemapSystem::SkyCubemapData
			HostOnlyMemory<ST, CreationRendererPrivate::ShadowMapCameraCutData>*               pShadowMapCameraCutData;        // 0x8 item size 0x4 unkonfirmed CreationRendererPrivate::ShadowMapCameraCutData
			HostOnlyMemory<ST, CreationRendererPrivate::CascadeShadowMapCache>*                pCascadeShadowMapCache;         // 0x10 struct CreationRendererPrivate::CascadeShadowMapCache
			HostOnlyMemory<ST, CreationRendererPrivate::TiledCullingData>*                     pTiledCullingData;              // 0x18 item size 6*8 class CreationRendererPrivate::TiledCullingData
			HostOnlyMemory<ST, CreationRenderer::CGen::HDRData>*                               pHdrData;                       // 0x20 struct CreationRenderer::CGen::HDRData
			HostOnlyMemory<ST, CreationRendererPrivate::HDRSystem::HDRConfig>*                 pHdrConfig;                     // 0x28 item size 550*8 struct CreationRendererPrivate::HDRSystem::HDRConfig
			HostOnlyMemory<ST, CreationRendererPrivate::DepthSystem::HistoryData>*             pHostOnlyMemory6;               // 0x30 item size 88 struct CreationRendererPrivate::DepthSystem::HistoryData
			HostOnlyMemory<ST, CreationRendererPrivate::GPUDebugGeometry::HistoryData>*        pGPUGeometryDebugHistoryData;   // 0x38 struct CreationRendererPrivate::GPUDebugGeometry::HistoryData
			HostOnlyMemory<ST, CreationRendererPrivate::DepthSystem::VisualizationData>*       pDepthSystemVisualizationData;  // 0x40 struct CreationRendererPrivate::DepthSystem::VisualizationData
			HostOnlyMemory<ST, CreationRendererPrivate::SkyOcclusionSystem::SkyOcclusionData>* pSkyOclussionData;              // 0x48 struct CreationRendererPrivate::SkyOcclusionSystem::SkyOcclusionData
			HostOnlyMemory<ST, CreationRendererPrivate::HeightfieldSystem::HeightfieldData>*   pHeightFieldData;               // 0x50 struct CreationRendererPrivate::HeightfieldSystem::HeightfieldData
			HostOnlyMemory<ST, CreationRendererPrivate::ProbeSystem::ProbeSystemData>*         pProbeSystemData;               // 0x58 struct CreationRendererPrivate::ProbeSystem::ProbeSystemData
		};
		struct alignas(8) Storage
		{
			AsyncTableColumnBase<Storage, StorageTable::HeightfieldSettings>*       pColumnHeightfieldSettings;              // struct StorageTable::HeightfieldSettings
			AsyncTableColumnBase<Storage, CreationRenderer::DirectionalShadowData>* pColumnDirectionalShadowData;            // struct CreationRenderer::DirectionalShadowData
			AsyncTableColumnBase<Storage, AverageLuminanceFutureReadback>*          pColumnDAverageLuminanceFutureReadback;  // struct StorageTable::AverageLuminanceFutureReadback
			AsyncTableColumnBase<Storage, HighlightSettings>*                       pColumnHighlightSettings;                // struct StorageTable::HighlightSettings
			AsyncTableColumnBase<Storage, CreationRenderer::FeatureSetup>*          pColumnFeatureSetup;                     // 0x20 struct CreationRenderer::FeatureSetup itemSize is 8*5 bytes
			AsyncTableColumnBase<Storage, uint32_t>*                                pColumnImageSpaceData;                   // class StorageTable::StrongKey<struct StorageTable::ImageSpaceData::Storage> 0x84 or 0x210 size item
			AsyncTableColumnBase<Storage, ClearColor>*                              pColumnClearColor;                       // struct StorageTable::ClearColor
			AsyncTableColumnBase<Storage, CameraViewData>*                          pColumnCameraViewData;                   // struct StorageTable::CameraViewData
			WriterDirectStorageData<Storage, void*>*                                pWriterColumnData;                       // pointer to 0
			ObserverCameraViewHostOnlyTables<Storage>*                              pObserverStorageData;                    // size 0x68 12 pointers to HostOnlyMemory ObserverDirectStorageData<struct StorageTable::CameraView::Storage
			void*                                                                   buffer1;                                 // byte buffer
			void*                                                                   buffer2;                                 // byte buffer
			void*                                                                   pUnk_60;                                 // 0x60 pointer to pointers
		};
	};

	class CameraViewHandle : public StorageData<CameraView::Storage>
	{
	public:
		virtual ~CameraViewHandle() = 0;

		/*void Register(uint32_t* idOut) {
			using func_t = decltype(&CameraViewHandle::Register);
			REL::Relocation<func_t> func{ REL::ID(201697) };
			return func(this, idOut);
		}

		static CameraViewHandle* Singleton() {
			static REL::Relocation<CameraViewHandle**> singleton{ REL::ID(772635) };
			return *singleton;
		}*/

		/**
		 * @param id assigned after register usually it is id for scene.
		 */
		/*static void writeCameraViewData(uint32_t id, CameraViewData* data)
		{
			using func_t = decltype(&CameraViewHandle::writeCameraViewData);
			REL::Relocation<func_t> func{ REL::ID(201916) };
			return func(id, data);
		}

		static void writeFeatureSetupData(uint32_t id, CreationRenderer::FeatureSetup* data)
		{
			using func_t = decltype(&CameraViewHandle::writeFeatureSetupData);
			REL::Relocation<func_t> func{ REL::ID(201919) }; // 10.36 version is missing this function
			return func(id, data);
		}

		static void writeImageSpaceData(uint32_t id, uint32_t* data)
		{
			using func_t = decltype(&CameraViewHandle::writeImageSpaceData);
			REL::Relocation<func_t> func{ REL::ID(201921) };
			return func(id, data);
		}

		static  CreationRenderer::FeatureSetup* getFeatureSetup(uint32_t id)
		{
			using func_t = decltype(&CameraViewHandle::getFeatureSetup);
			REL::Relocation<func_t> func{ REL::ID(201271) };
			return func(id);
		}*/
	};
	static_assert(sizeof(CameraViewHandle) == 0x150);
}
