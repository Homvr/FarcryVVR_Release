#pragma once
#include <DescriptorHeap.h>
#include <Framework.hpp>
#include <d3d12.h>
// #include <mods/vr/d3d12/ComPtr.hpp>
// #include "sl.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace d3d12
{
    constexpr int MAX_SMALL_RING_POOL_SIZE = 5;
    enum SRV_UAV : unsigned int
    {
        SRV_HEAP_START = 0,
        UAV_HEAP_START = SRV_HEAP_START + MAX_SMALL_RING_POOL_SIZE,
        SRV_UAV_COUNT  = UAV_HEAP_START + MAX_SMALL_RING_POOL_SIZE
    };
    struct RingResource
    {
        uintptr_t resource_ptr{};
        int heapIndex{-1};
        unsigned int Width{};
        unsigned int Height{};

        void Reset()
        {
            resource_ptr = 0;
            heapIndex = -1;
            Width = 0;
            Height = 0;
        }
    };
    static DXGI_FORMAT getCorrectDXGIFormat(DXGI_FORMAT Format);
    static D3D12_SHADER_RESOURCE_VIEW_DESC getSRVdesc(const D3D12_RESOURCE_DESC& desc);
    struct SmallRingPool
    {
        RingResource srv_pool[MAX_SMALL_RING_POOL_SIZE]{};
        int current_srv_pool_index{-1};
        RingResource uav_pool[MAX_SMALL_RING_POOL_SIZE]{};
        int current_uav_pool_index{-1};
        std::unique_ptr<DescriptorHeap> m_compute_heap{};
        ID3D12Device* m_pDevice{nullptr};

        RingResource& GetSRVSlot(ID3D12Resource* pDepth);
        RingResource& GetUAVSlot(ID3D12Resource* pMVec);
        D3D12_GPU_DESCRIPTOR_HANDLE GetGpuSrvHandle(ID3D12Resource* pResource);
        D3D12_GPU_DESCRIPTOR_HANDLE GetGpuUavHandle(ID3D12Resource* pResource);
        void Init(ID3D12Device* pDevice);

        void Reset()
        {
            for (auto& res : srv_pool) {
                res.Reset();
            }
            current_srv_pool_index = -1;
            for (auto& res : uav_pool) {
                res.Reset();
            }
            current_uav_pool_index = -1;
            m_compute_heap.reset();
            m_pDevice = nullptr;

        }


    };
}

class MotionVectorReprojection
{
public:
    MotionVectorReprojection() = default;
    ~MotionVectorReprojection() { Reset(); };

    // Lifecycle methods (called by UpscalerAfrNvidiaModule)
    void on_d3d12_initialize(ID3D12Device* device, const D3D12_RESOURCE_DESC& backBuffer_desc);
    void on_device_reset();

    [[nodiscard]] inline bool isInitialized() const
    {
        if (!g_framework->is_ready() || !m_initialized) {
            return false;
        }
        return true;
    }

    void Reset()
    {
        m_initialized = false;
        m_compute_pso.Reset();
        m_computeRootSignature.Reset();
        m_small_ring_pool.Reset();
    }

    // Run the compute shader to process motion vectors
    //    void ProcessMotionVectors(ID3D12Resource* mvec, D3D12_RESOURCE_STATES state, uint32_t frame);
    void ProcessMotionVectors(ID3D12Resource* mvec, D3D12_RESOURCE_STATES mvec_state, ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state, uint32_t frame,
                              ID3D12GraphicsCommandList* cmd_list);

    glm::vec2 m_mvecScale {0.5f, -0.5f};
private:

    bool CreateComputeRootSignature(ID3D12Device* device);
    bool CreatePipelineStates(ID3D12Device* device, DXGI_FORMAT backBufferFormat);

    ComPtr<ID3D12RootSignature>              m_computeRootSignature;
    ComPtr<ID3D12PipelineState>              m_compute_pso;
    d3d12::SmallRingPool                     m_small_ring_pool;
    bool m_initialized{ false };
};
