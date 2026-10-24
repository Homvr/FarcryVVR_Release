#pragma once

#include <d3d11.h>
#include <wrl/client.h> // For ComPtr
#include <DirectXMath.h>
#include <glm/glm.hpp>
#include <memory>
#include <map>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class MotionVectorReprojectionD3D11
{
public:
    static std::shared_ptr<MotionVectorReprojectionD3D11>& Get()
    {
        static auto instance = std::make_shared<MotionVectorReprojectionD3D11>();
        return instance;
    }

    bool isInitialized() const { return m_initialized; }

    // Main processing function to be called from your hook
    void Process(ID3D11DeviceContext* context, ID3D11Resource* depth, ID3D11Resource* motionVectors, uint32_t frame);

    void Reset();

    // Utility function to get a typeless format for creating views
    static DXGI_FORMAT getCorrectDXGIFormat(DXGI_FORMAT Format);

private:

    struct ProjectionConstants {
        glm::mat4 projection;
        glm::mat4 invProjection;
        glm::mat4 viewProjection;
        glm::mat4 invViewProjection;
    };
    // Setup function, called internally on first Process call
    bool setup(ID3D11Device* device);

    // D3D11 resource creation and management
    bool createComputeShader(ID3D11Device* device);
    bool createConstantBuffer(ID3D11Device* device);

    // View caching to avoid recreating views every frame
    ID3D11ShaderResourceView* getDepthSRV(ID3D11Device* device, ID3D11Resource* depthResource);
    ID3D11UnorderedAccessView* getMotionVectorUAV(ID3D11Device* device, ID3D11Resource* mvResource);

    // Constants struct for the shader
    struct alignas(16) MotionVectorCorrectionConstants {
        glm::mat4 correction{};
        glm::mat4 cameraMotionCorrection{};
        XMFLOAT4 texSize{};
    };

    bool m_initialized{ false };

    ComPtr<ID3D11ComputeShader> m_compute_shader;
    ComPtr<ID3D11Buffer> m_constants_buffer;

    // Cache for resource views
    std::map<ID3D11Resource*, ComPtr<ID3D11ShaderResourceView>> m_srv_cache;
    std::map<ID3D11Resource*, ComPtr<ID3D11UnorderedAccessView>> m_uav_cache;

    // History for SL constants (assuming you have a way to get these)
    // You will need to adapt how you get/store these constants for D3D11
    static constexpr uint32_t MAX_CONSTANTS_HISTORY = 4;
    ProjectionConstants m_constants_history[MAX_CONSTANTS_HISTORY]{};
    inline void submitSlConstants(const ProjectionConstants& constants, uint32_t frame) {
        m_constants_history[frame % MAX_CONSTANTS_HISTORY] = constants;
    }
    [[nodiscard]] inline const auto& getSlConstants(uint32_t frame) const {
        return m_constants_history[frame % MAX_CONSTANTS_HISTORY];
    }
};