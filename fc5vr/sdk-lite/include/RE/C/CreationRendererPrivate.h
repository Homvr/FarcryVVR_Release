#pragma once

#include "CameraHandle.h"
#include "RE/B/BSFixedString.h"
#include "RE/B/BSTArray.h"
#include "REL/Relocation.h"
//#include "RE/C/CreationRendererContext.h"
//#include "RE/utils.h"
#include "CreationEngine/memory/offsets.h"

struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace RE::CreationRendererPrivate
{
	class RenderGraph;
	class RenderGraphDataInner;

	struct RenderTarget
	{
		char*    name;
		uint32_t renderTargetId;  // 0x8
		uint32_t unk_C;
		uint32_t unk_10;
		uint32_t flags;  // 0x14 &0x207A0 mask
		uint64_t unk_18;
		void*    unk_20;
	};
	static_assert(sizeof(RenderTarget) == 0x28);

#pragma pack(push, 1)
	struct RenderPassD3D12Resource
	{
		uint8_t         pad[0x38];
		ID3D12Resource* pResource;
		uint8_t         trailing[1024];
	};
#pragma pack(pop)

#pragma pack(push, 1)
	struct RenderPassD3D12ResourceWrapper
	{
		uint8_t                          pad[4];
		uint32_t                         width;
		uint32_t                         height;
		uint8_t                          pad1[76];
		RenderPassD3D12Resource*         pResourceContext;
		uint32_t                         sizeOrFrameCountOrIndex;
		uint32_t                         unk_48;
		RenderPassD3D12ResourceWrapper** resourcePool;
		char                             _pad1[8];
		D3D12_CPU_DESCRIPTOR_HANDLE*     m_RTVCpuDescriptors;
	};
#pragma pack(pop)

#pragma pack(push, 1)
	struct RenderGraphResource
	{
		uint8_t                         pad[184];
		uint32_t                        resourceSize;
		uint32_t                        unkBC;
		uint32_t            			renderGraphIndex;
		uint32_t            			unkC4;
		RenderPassD3D12ResourceWrapper* pResourceWrapper;
		uint8_t                         padD0[8];
	};
#pragma pack(pop)
	static_assert(offsetof(RenderGraphResource, renderGraphIndex) == 0xC0);
	static_assert(offsetof(RenderGraphResource, pResourceWrapper) == 0xC8);
	static_assert(sizeof(RenderGraphResource) == 0xD8);

	struct RenderPassItem
	{
		uint32_t resourceSize;
		uint32_t unk_4;
		uint32_t renderGraphIndex;
		uint32_t unk_C;
		uint32_t stateOrFlags; //confirmed transition state
		uint32_t unk14;
		uint64_t unk_18;
		[[nodiscard]] inline uint32_t getDXGIState() const {
			return getDXGIState(stateOrFlags);
		}
//		inline static uint32_t getDXGIState(uint32_t state) {
//			using func_t = int (*)(int);
//			static REL::Relocation<func_t> getDXGIState_original{ GameStore::MemoryOffsets::CreationRenderer::GetDXGIState() };
//			return getDXGIState_original(state);
//		}

        inline static uint32_t getDXGIState(uint32_t state) {
            int result; // eax
            int v3; // ecx
            int v4; // edx
            int v5; // r9d
            int v6; // ecx
            int v7; // edx

            if ( (state & 0x4581C) == 284700 )
                return 2755;
            if ( (state & 2) != 0 )
                return 0;
            v3 = (state >> 3) & 0x800;
            v4 = v3 | 0x400;
            if ( (state & 0x2000) == 0 )
                v4 = v3;
            v5 = v4 | 8;
            if ( (state & 0x40) == 0 )
                v5 = v4;
            if ( (state & 0x10000) != 0 )
                return 0;
            v6 = v5 | 4;
            if ( (state & 0x20) == 0 )
                v6 = v5;
            v7 = v6 | 0x40;
            if ( (state & 0x800) == 0 )
                v7 = v6;
            if ( (state & 0x20000) != 0 )
                return 0;
            result = v7 | 0x80;
            if ( (state & 0x40000) == 0 )
                return v7;
            return result;
        }
	};
	static_assert(sizeof(RenderPassItem) == 0x20);
	static_assert(offsetof(RenderPassItem, renderGraphIndex) == 0x08);

	struct RenderGraphContext
	{
		uint32_t viewPortId;  // 0x20
	};

	// scrapheap allocator?
	struct RenderGraphDataHeapAllocator
	{
		uintptr_t unk0;
		void *struct3;
	};

	struct RenderPassD3D12ResourceWrapper2
	{
		RE::CreationRendererPrivate::RenderPassD3D12ResourceWrapper *resource;
		__int32 unk8;
		__int32 unkC;
	};


	struct RenderGraphDataResourceArray
	{
		RenderPassD3D12ResourceWrapper2 nodeArray[2];
		uint32_t frameNumber;
		uint32_t unk24;
		char flag28;
		char flag29;
		char flag2A;
	};


	struct RenderGraphDataResourcesList
	{
		RenderGraphDataResourceArray *resources[12];
		char cameraCut;
	};

	struct RenderGraphDataBase0x30 {
		uint8_t unk0[0xF8];
		float jitterOffset[2];
		uint8_t unk100[0x80];
	};

#pragma pack(push, 1)
	struct RenderGraphDataBase
	{
		RE::StorageTable::CameraViewport cameraViewport;
		RE::StorageTable::CameraScissor cameraScissors;
		unsigned int rootRenderGraphID;
		unsigned int sceneId; // or CameraHandleId
		RenderGraphDataHeapAllocator *allocator;
		RenderGraphDataBase0x30 *unk30_0x180size;
		uint8_t unk38[8];
		RenderGraphDataResourcesList *resources40;
	};
#pragma pack(pop)

#pragma pack(push, 1)
	struct RenderGraphData
	{
		// v7 = *(_DWORD *)(*(&renderGRaphData->field_108 + *(unsigned int *)renderGRaphData->gap_138) + 36); === ID for column feature setup
		uint64_t              field_0;
		uint8_t               gap_8[24];
		uint32_t              rootRenderGraphID;
		uint32_t              sceneId;
		uint8_t               gap_28[200]; // 0x28 Possible huge constants structure with views projections and past projections
		uint64_t              field_F0;
		uint32_t              field_F8;
		uint32_t              field_FC;
		uint64_t              field_100;
		RenderGraphDataBase*  renderGraphResources[1]; // [viewportId]
		uint8_t               gap_110[24];
		uint64_t              field_128;
		uint64_t              field_130;  // number of items in some unk structure
		RenderGraphDataInner* pRenderGraphDataInner;
		uint32_t              viewportId;  // 0x140 increments during render pass and decrements when pass is done, most likely to support async frame rendering
		uint32_t              unk144;  // 0x144
		uint64_t              field_148;
		[[nodiscard]] inline uintptr_t getCommandList() const
		{
			using func_t = uintptr_t(*)(const RenderGraphData*);
			static REL::Relocation<func_t> getCommandListFunc{ GameStore::MemoryOffsets::CreationRenderer::GetCommandList() };
			return getCommandListFunc(this);
		}

		inline RenderGraphDataBase* getRenderGraphDataBase()
		{
			return renderGraphResources[viewportId];
		}

		inline bool resetHistory()
		{
			auto resources = renderGraphResources[viewportId];
			if(resources == nullptr) {
				return false;
			}
			auto resources40 = resources->resources40;
			if(resources40 == nullptr) {
				return false;
			}
			return resources40->cameraCut;
		}

		[[nodiscard]] inline ID3D12Resource* getResourceByIndex(uint32_t index, int frameCount)
		{
			auto resources = renderGraphResources[viewportId];
			if(resources == nullptr) {
				return nullptr;
			}
			auto resources40 = resources->resources40;
			if(resources40 == nullptr) {
				return nullptr;
			}
			auto resourceArray = resources40->resources[index];
			if(resourceArray == nullptr) {
				return nullptr;
			}
			auto& wrapper2 = resourceArray->nodeArray[frameCount & 1];
			auto resourceWrapper = wrapper2.resource;
			auto state = RenderPassItem::getDXGIState(wrapper2.unk8);
			if(resourceWrapper == nullptr || state != 8) {
				return nullptr;
			}
			auto context = resourceWrapper->pResourceContext;
			if(context == nullptr) {
				return nullptr;
			}
			return context->pResource;
		}
	};
#pragma pack(pop)

	struct alignas(0) RenderPassData
	{
		BSTSmallArray<RenderPassItem, 8>*       renderPassItems;  // 0x0
		BSTSmallArray<RenderGraphResource, 64>* renderGraphResources;
		RenderGraph*                            pRenderGraph;
		uint16_t                                unk_18;
		[[nodiscard]] inline RenderPassItem* getRenderPassItemByIndex(uint32_t index) const
		{
			if(renderPassItems == nullptr || index >= renderPassItems->_size) {
				return nullptr;
			}
			auto renderPassItemsHeap = renderPassItems->_data.heap;
			if(renderPassItems->_local) {
				renderPassItemsHeap = renderPassItems->_data.local;
			}
			return &renderPassItemsHeap[index];
		}
/*
		[[nodiscard]] inline RenderPassD3D12ResourceWrapper* getResourceWrapperByIndexInternal(uint32_t index) const
		{
			using func_t = RenderPassD3D12ResourceWrapper*(*)(const RenderPassData*, int32_t index);

			static REL::Relocation<func_t> getResourceWrapperByIndexInternal_fn{ REL::ID(206433) };
			return getResourceWrapperByIndexInternal_fn(this, index);
		}*/

		[[nodiscard]] inline ID3D12Resource* getNativeResourceByIndex(uint32_t index) const
		{
			auto resource = getResourceByIndex(index);
			if(resource == nullptr || resource->pResourceWrapper == nullptr || resource->pResourceWrapper->pResourceContext == nullptr) {
				return nullptr;
			}
			return resource->pResourceWrapper->pResourceContext->pResource;
		}

		[[nodiscard]] inline RenderGraphResource* getResourceByIndex(uint32_t index) const
		{
			if(renderPassItems == nullptr || renderGraphResources == nullptr) {
				return nullptr;
			}
			RE::CreationRendererPrivate::RenderGraphResource* resource = nullptr;
			auto renderPassItem = getRenderPassItemByIndex(index);
			if(renderPassItem == nullptr) {
				return nullptr;
			}
			auto heap = renderGraphResources->_data.heap;
			auto last = &heap[renderGraphResources->_inline_capacity];
			if(renderGraphResources->_inline_capacity) {
				for(auto item = heap; item != last; item++) {
					if(item->renderGraphIndex == renderPassItem->renderGraphIndex && item->resourceSize == renderPassItem->resourceSize) {
						resource = item;
						break;
					}
				}
			}
			return resource;
		}
	};

	class alignas(0) RenderPass
	{
	public:
		virtual ~RenderPass();

		virtual void unk_1();

		virtual void unk_2();

		virtual void unk_3();

		virtual void unk_4();

		virtual void unk_5();                                   //nimpl
		virtual void unk_6();                                   //nimpl
		virtual void unk_7(RenderGraphData*, RenderPassData*);  //nimpl

		// members
		char*  name;   // name 0x8->0x98 is a copy from another pass 0x0->0x90
		uint64_t      flags;  // 0x10 flags 0x12 byte possible enabled/disabled &1 == 1 means enabled
		uint32_t      renderTargetLen;
		uint32_t      unk_1C;
		uint32_t      renderHandleId;
		uint32_t      cameraViewHandleID;
		uint32_t      renderTargetsSize;  // renderTargetsSize
		uint32_t      gap2A;
		RenderTarget* pRenderTargets;  // 0x30
		uint64_t      unk_38;
		uint64_t      unk_40;
		uint64_t      unk_48;
		uint64_t      unk_50;
		uint64_t      unk_58;
		uint64_t      unk_60;
		uint64_t      unk_68;
		uint64_t      unk_70;
		uint64_t      unk_78;
		uint64_t      unk_80;
		uint64_t      unk_88;
		uint64_t      unk_90;  // first byte set to zero probably type RenderPass/RenderGraph
		uint64_t      unk_98;
		uint64_t      unk_A0;
		uint64_t      unk_A8;
		uint64_t      unk_B0;
		uint64_t      unk_B8;
		uint64_t      unk_C0;
		uint64_t      unk_C8;
		uint64_t      shadingRateIndex;  // 0xD0
		BSFixedString unk_D8;
		RenderPass*   pSelfOrParent;
		uint64_t      unk_E8;
	};
	static_assert(sizeof(RenderPass) == 0xF0);

	class TemporalAA_idTech7RenderPass : public RenderPass
	{
		uint32_t colorInId;  // likely someIds colouIn/colorOut
		uint32_t colorOutId;
	};
	static_assert(sizeof(TemporalAA_idTech7RenderPass) == 0xF8);

	class MotionVectorCompositeRenderPass : public RenderPass
	{
		uint32_t unkF0;  // D3DContextTechniqueData index
		uint32_t unkF4;
	};
	static_assert(sizeof(MotionVectorCompositeRenderPass) == 0xF8);

	class DLSSUpscaleRenderPass : public RenderPass
	{
	};

#pragma pack(push, 1)
	struct RenderPassContainer
	{
		uint64_t                         field_0;
		uint64_t                         field_8;
		uint64_t                         field_10;
		uint64_t                         field_18;
		RenderPass*                      pRenderPass;
		BSTSmallArray<RenderPassItem, 8> renderPassData;
		uint64_t                         field_138;
		uint64_t                         field_140;
		uint8_t                          gap_148[2];
		char                             field_14A;
		uint8_t                          gap_14B[5];
	};
#pragma pack(pop)
	static_assert(sizeof(BSTSmallArray<RenderPassItem, 8>) == 256 + 16);
	static_assert(sizeof(RenderPassContainer) == 0x150);
	static_assert(offsetof(RenderPassContainer, pRenderPass) == 0x20);
	static_assert(offsetof(RenderPassContainer, field_138) == 0x138);

#pragma pack(push, 1)
	struct RenderGraphUnk
	{
		unsigned __int64 possibleSize;
		int              renderGraphIndex;
		__int16          possibleIDorIndex;
		char             field_E;
		bool             field_F;
	};
#pragma pack(pop)

	class RenderGraph : public RenderPass
	{
	public:
		~RenderGraph() override;

		virtual void updateOrAddSubGraphs();  // update views
		virtual void unk_9();

		virtual void unk_10();

		BSTArray<void*>                         arrayF0;  // 0xF0 size of int32(a2->0x20)
		uint8_t                                 unk_100[0x118];
		BSTSmallArray<RenderGraphResource, 64>* resources;
		BSTArray<RenderGraphUnk>*               renderGraphUnkArray;
		BSTSmallArray<RenderPassContainer, 8>   renderGraphs;            // 228h element size  0x150u 8 elementsx0x150 size
		uint8_t                                 unk_CB8[0xD20 - 0xCB8];  // 0xCC0
																		 // a1->unk_CB8[96] RE::CreationRendererPrivate::RenderGraphData
																		 // 0x0CE0 renderGraphsSize
		uint32_t renderGraphID;                                          // 0xD20
		uint32_t unk_D24;                                                // some byteflag at 0xD26
	};

	static_assert(offsetof(RenderGraph, arrayF0) == 0xF0);
	static_assert(offsetof(RenderGraph, renderGraphs) == 0x228);
	static_assert(offsetof(RenderGraph, renderGraphID) == 0xD20);
	static_assert(sizeof(RenderGraph) == 0xD28);

	static_assert(sizeof(BSTSmallArray<RenderPassContainer, 8>) == 2688 + 0x10);
	static_assert(sizeof(BSTSmallArray<uint32_t, 4>) == 0x20);
	using RenderGraphBSTAllocator = BSTArrayAllocatorFunctor<BSTSmallArrayHeapAllocator<RenderPassContainer, 8>>;

	class FrameRenderGraph : public RenderGraph
	{
	public:
		uint32_t id;
		uint32_t unk_D2C;
	};

	class RenderViewSubGraph : public RenderGraph
	{
	public:
		~RenderViewSubGraph() override;

		uint32_t sceneId;  // 0xD28
		uint32_t unk_D2C;    // pad
	};

	static_assert(sizeof(RenderViewSubGraph) == 0xD30);

	class MainRenderGraph : public RenderGraph
	{
	public:
		~MainRenderGraph() override;

		BSTSmallArray<uint32_t, 4>      multiViewDataArray;  // 0xD28 4 elem 4 size represent viewIndexes from StorageTable::REnderGraph::MiltiView
		BSTSmallArray<BSFixedString, 4> subGraphNames;
	};

	static_assert(offsetof(MainRenderGraph, multiViewDataArray) == 0xD28);
	static_assert(offsetof(MainRenderGraph, subGraphNames) == 0xD48);
	static_assert(sizeof(MainRenderGraph) == 0xD78);

	class RenderSceneSubGraph : public RenderGraph
	{
		uint8_t   flag;  // 0xD28
		uint8_t   unk_D29;
		uintptr_t unk_D30[0x40];  // 0xD30 64 pointers
		uint64_t  unk_F30;
		uint64_t  unk_F38;
		uint32_t  unk_F40;
		uint32_t  unk_F44;
		uint32_t  unk_F48;
		uint32_t  unk_F4C;
		uint32_t  unk_F50;
		uint32_t  unk_F54;
		uint32_t  unk_F58;
		uint32_t  unk_F5C;
		uint32_t  unk_F60;
		uint32_t  unk_F64;
		uint32_t  unk_F68;
		uint32_t  unk_F6C;
	};
	static_assert(sizeof(RenderSceneSubGraph) == 0xF70);

	// REnder Graph struct REL::ID(885886)
	// RenderSceneSubGraph  (qword_1459AA8D8 + 232)

	struct alignas(0) VisualizationSetup
	{
		uint8_t data[0x3C];
	};
	static_assert(sizeof(VisualizationSetup) == 0x3C);

	struct TiledCullingData
	{
		uint8_t data[6 * 8];
	};
	static_assert(sizeof(TiledCullingData) == 6 * 8);

	struct alignas(4) CascadeShadowMapCache
	{
		uint8_t data[4];
	};
	static_assert(sizeof(CascadeShadowMapCache) == 4);

	struct ShadowMapCameraCutData
	{
		uint8_t data[1024 << 4];  // unconfirmed
	};

	namespace GPUDebugGeometry
	{
		struct HistoryData
		{
			uint8_t data[1024 << 4];  // unconfirmed
		};
	};

	namespace DepthSystem
	{
		struct VisualizationData
		{
			uint8_t data[1024 << 4];  // unconfirmed
		};

		struct HistoryData
		{
			uint8_t data[88];  // unconfirmed
		};
		static_assert(sizeof(HistoryData) == 0x58);
	};

	namespace SkyOcclusionSystem
	{
		struct SkyOcclusionData
		{
			uint8_t data[1024 << 4];  // unconfirmed
		};
	};

	namespace HeightfieldSystem
	{
		struct HeightfieldData
		{
			uint8_t data[550 * 8];
		};
	};
	static_assert(sizeof(HeightfieldSystem::HeightfieldData) == 550 * 8);

	namespace ProbeSystem
	{
		struct ProbeSystemData
		{
			void* data;  // unconfirmed
		};
	};

	namespace HDRSystem
	{
		struct HDRConfig
		{
			void* data;  // unconfirmed
		};
	}
	namespace SkyCubemapSystem
	{
		struct SkyCubemapData
		{
			void* data;  // unconfirmed
		};
	};

	namespace ApiAbstractionLayer
	{
		namespace FSR3
		{
			struct ContextData
			{
				uint8_t data[1024 << 4];  // unconfirmed
			};
		}

		namespace FSR2
		{
			struct ContextData
			{
				uint8_t data[1024 << 4];  // unconfirmed
			};
		}
	};

	namespace Intel_XeSS
	{
		struct XeSSContext
		{
			uint8_t data[1024 << 4];  // unconfirmed
		};
	}

	namespace NV_Streamline
	{
		namespace Utils
		{
			struct ViewportData
			{
				uint8_t data[1024 << 4];  // unconfirmed
										  // uint32_t offfset 0x10 viewportHandleId
			};
		}
	}

}
