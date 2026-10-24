#pragma once

#include <DescriptorHeap.h>
#include <d3d12.h>
#include <Mod.hpp>
#include <mods/vr/d3d12/ComPtr.hpp>
#include <mods/vr/d3d12/TextureContext.hpp>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Forward declaration
class UpscalerAfrNvidiaModule;

class ShaderDebugOverlay : public Mod
{
public:
    inline static std::shared_ptr<ShaderDebugOverlay> &Get() {
        static auto instance = std::make_shared<ShaderDebugOverlay>();
        return instance;
    }

    bool setup(ID3D12Device *device, D3D12_RESOURCE_DESC& backBuffer_desc);

    // Mod interface
    std::string_view get_name() const override { return "ShaderDebug"; }
    void on_device_reset() override { Reset(); }
    void on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc) override { setup(pDevice4, desc); }
    void on_draw_ui() override;
    void Draw(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv);
    void on_pre_imgui_frame() override;

    inline bool isValid()
    {
        for (const auto& buffer : m_motion_vector_buffer) {
            if (!buffer)
                return false;
        }
        //        for (const auto& buffer : m_depth_buffer) {
        //            if (!buffer) return false;
        //        }
        return true;
    }

    inline void Reset() {
        m_debug_layer_pso.Reset();
        m_rootSignature.Reset();
        m_srv_heap.reset();
        m_rtv_heap.reset();
        m_commandContext.reset();
        m_image1.Reset();
        m_motion_vector_buffer[0].Reset();
        m_motion_vector_buffer[1].Reset();
    }

    ShaderDebugOverlay() = default;

    ~ShaderDebugOverlay() override {
        Reset();
    };

    static inline DXGI_FORMAT getCorrectDXGIFormat(DXGI_FORMAT Format)
    {
        switch (Format)
        {
        case DXGI_FORMAT_D16_UNORM: // casting from non typeless is supported from RS2+
            return DXGI_FORMAT_R16_UNORM;
        case DXGI_FORMAT_D32_FLOAT: // casting from non typeless is supported from RS2+
        case DXGI_FORMAT_R32_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32_TYPELESS:
            return DXGI_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R16G16_TYPELESS:
            return DXGI_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: // casting from non typeless is supported from RS2+
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: // casting from non typeless is supported from RS2+
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            return Format;
        };
    }


    static bool ValidateResource(ID3D12Resource* source, ComPtr<ID3D12Resource> buffers[2]);


    static void CopyResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* pSrcResource, ID3D12Resource* pDstResource, D3D12_RESOURCE_STATES srcState,
                             D3D12_RESOURCE_STATES dstState);

    static void SetMvecScale(const float x, const float y) {
        const auto instance = Get();
        instance->mvecScale.x = x;
        instance->mvecScale.y = y;
    }

private:
    enum SRV_HEAP : unsigned int {
        MVEC_LEFT  = 0,
        MVEC_RIGHT = 1,
        MVEC_PROCESSED = 2,
        COUNT = 3
    };

    bool CreateRootSignature(ID3D12Device *device);
    bool CreatePipelineStates(ID3D12Device *device, DXGI_FORMAT backBufferFormat);
//    void RenderToDebugTexture(ID3D12GraphicsCommandList* commandList);

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_debug_layer_pso;
    std::unique_ptr<DirectX::DescriptorHeap> m_srv_heap{};
    std::unique_ptr<DirectX::DescriptorHeap> m_rtv_heap{};
    d3d12::CommandContext m_commandContext{};
    ComPtr<ID3D12Resource> m_image1{ nullptr};

public:
    ComPtr<ID3D12Resource> m_motion_vector_buffer[2]{};
private:
    
    UINT m_debug_width{0};
    UINT m_debug_height{0};
    bool m_initialized{false};

    glm::vec2 mvecScale {0.5f, -0.5f};
    
    // Shader configuration
    float m_scale{40.0f};           // Scale factor for visualization
    uint8_t m_debug_axis;  // 0 - X, 1 - Y
    bool m_show_abs{true};          // Show absolute values

};
