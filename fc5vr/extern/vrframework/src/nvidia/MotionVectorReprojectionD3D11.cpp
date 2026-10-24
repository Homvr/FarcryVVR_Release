#include "MotionVectorReprojectionD3D11.h"
#include <d3dcompiler.h> // For compiling shaders if needed, though we use bytecode here
#include <spdlog/spdlog.h> // Assuming you use spdlog

// You need to provide these dependencies from your project
//#include <ModSettings.h>
#include <mods/VR.hpp>

// This header should contain the compiled compute shader bytecode
// for a D3D11 target (e.g., cs_5_0)
namespace shaders::compute {
//#include "shader-headers/motion_vector_correction_cs.h"
}

bool MotionVectorReprojectionD3D11::setup(ID3D11Device* device) {
    if (!createComputeShader(device) || !createConstantBuffer(device)) {
        spdlog::error("[VR] Failed to setup MotionVectorReprojection for D3D11");
        return false;
    }

    m_initialized = true;
    spdlog::info("[VR] MotionVectorReprojection for D3D11 initialized successfully.");
    return true;
}

void MotionVectorReprojectionD3D11::Reset() {
    m_compute_shader.Reset();
    m_constants_buffer.Reset();
    m_srv_cache.clear();
    m_uav_cache.clear();
    m_initialized = false;
}

bool MotionVectorReprojectionD3D11::createComputeShader(ID3D11Device* device) {
//    HRESULT hr = device->CreateComputeShader(
//        shaders::compute::g_CSMain, // Pointer to compiled shader bytecode
//        sizeof(shaders::compute::g_CSMain), // Size of the bytecode
//        nullptr,
//        &m_compute_shader
//    );

//    if (FAILED(hr)) {
//        spdlog::error("[VR] Failed to create compute shader. HRESULT: {:#x}", hr);
//        return false;
//    }
    return true;
}

bool MotionVectorReprojectionD3D11::createConstantBuffer(ID3D11Device* device) {
    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.ByteWidth = sizeof(MotionVectorCorrectionConstants);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb_desc.MiscFlags = 0;
    cb_desc.StructureByteStride = 0;

    HRESULT hr = device->CreateBuffer(&cb_desc, nullptr, &m_constants_buffer);
    if (FAILED(hr)) {
        spdlog::error("[VR] Failed to create constant buffer. HRESULT: {:#x}", hr);
        return false;
    }
    return true;
}

ID3D11ShaderResourceView* MotionVectorReprojectionD3D11::getDepthSRV(ID3D11Device* device, ID3D11Resource* depthResource) {
    if (m_srv_cache.count(depthResource)) {
        return m_srv_cache[depthResource].Get();
    }

    ComPtr<ID3D11Texture2D> depth_texture;
    if (FAILED(depthResource->QueryInterface(IID_PPV_ARGS(&depth_texture)))) {
        spdlog::error("[VR] Failed to query ID3D11Texture2D from depth resource.");
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC tex_desc;
    depth_texture->GetDesc(&tex_desc);

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = getCorrectDXGIFormat(tex_desc.Format);
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MostDetailedMip = 0;
    srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(depthResource, &srv_desc, &srv))) {
        spdlog::error("[VR] Failed to create SRV for depth texture. Format: {}", srv_desc.Format);
        return nullptr;
    }

    m_srv_cache[depthResource] = srv;
    return srv.Get();
}

ID3D11UnorderedAccessView* MotionVectorReprojectionD3D11::getMotionVectorUAV(ID3D11Device* device, ID3D11Resource* mvResource) {
    if (m_uav_cache.count(mvResource)) {
        return m_uav_cache[mvResource].Get();
    }

    ComPtr<ID3D11Texture2D> mv_texture;
    if (FAILED(mvResource->QueryInterface(IID_PPV_ARGS(&mv_texture)))) {
        spdlog::error("[VR] Failed to query ID3D11Texture2D from motion vector resource.");
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC tex_desc;
    mv_texture->GetDesc(&tex_desc);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = tex_desc.Format; // UAV format must match resource format
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11UnorderedAccessView> uav;
    if (FAILED(device->CreateUnorderedAccessView(mvResource, &uav_desc, &uav))) {
        spdlog::error("[VR] Failed to create UAV for motion vector texture. Format: {}", uav_desc.Format);
        return nullptr;
    }

    m_uav_cache[mvResource] = uav;
    return uav.Get();
}

void MotionVectorReprojectionD3D11::Process(ID3D11DeviceContext* context, ID3D11Resource* depth, ID3D11Resource* motionVectors, uint32_t frame) {
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);

    if (!isInitialized()) {
        if (!setup(device.Get())) {
            return;
        }
    }

    // Assuming you have a way to get these settings
    // if (!ModSettings::g_internalSettings.nvidiaMotionVectorFix) {
    //     return;
    // }

    // 1. Get Views for the resources
    auto* depth_srv = getDepthSRV(device.Get(), depth);
    auto* mv_uav = getMotionVectorUAV(device.Get(), motionVectors);

    if (!depth_srv || !mv_uav) {
        return;
    }

    // 2. Update Constant Buffer
    D3D11_MAPPED_SUBRESOURCE mapped_resource;
    if (SUCCEEDED(context->Map(m_constants_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_resource))) {
        auto* constants = static_cast<MotionVectorCorrectionConstants*>(mapped_resource.pData);

        ComPtr<ID3D11Texture2D> mv_texture;
        motionVectors->QueryInterface(IID_PPV_ARGS(&mv_texture));
        D3D11_TEXTURE2D_DESC desc;
        mv_texture->GetDesc(&desc);

        constants->texSize.x = (float)desc.Width;
        constants->texSize.y = (float)desc.Height;
        constants->texSize.z = 1.0f / (float)desc.Width;
        constants->texSize.w = 1.0f / (float)desc.Height;

        // This entire block is your specific logic and is copied directly
        // You must ensure VR::get() and getSlConstants() are available and work in your D3D11 context
//        static auto vr = VR::get();
//        glm::mat4 correction = glm::mat4(1.0f);
//        if (vr->is_hmd_active()) {
//            auto eye = vr->m_views.get_eye_view(frame);
//            auto past_eye = vr->m_views.get_eye_view(frame - 1);
//            if (frame % 2 == 0) {
//                auto hmd_transform = vr->m_views.get_hmd_view(frame);
//                auto hmd_transform_past = vr->m_views.get_hmd_view(frame - 2);
//                correction = eye * hmd_transform_past * glm::inverse(hmd_transform) * glm::inverse(past_eye);
//            } else {
//                correction = eye * glm::inverse(past_eye);
//            }
//        } // else if (ModSettings::g_internalSettings.cameraShake) { ... }
//
//        const auto& sl_constants_n_1 = getSlConstants(frame - 1);
//        const auto& sl_constants_n = getSlConstants(frame);
//        auto projection = *(glm::mat4*)&sl_constants_n.cameraViewToClip;
//        constants->correction = projection * correction * glm::inverse(projection);
//        constants->cameraMotionCorrection = *(glm::mat4*)&sl_constants_n_1.prevClipToClip;

        context->Unmap(m_constants_buffer.Get(), 0);
    }

    // 3. Set Compute Shader and Resources
    context->CSSetShader(m_compute_shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 1, &depth_srv); // Depth at t0
    context->CSSetUnorderedAccessViews(0, 1, &mv_uav, nullptr); // Motion Vectors at u0
    context->CSSetConstantBuffers(0, 1, m_constants_buffer.GetAddressOf()); // Constants at b0

    // 4. Dispatch
    ComPtr<ID3D11Texture2D> mv_texture;
    motionVectors->QueryInterface(IID_PPV_ARGS(&mv_texture));
    D3D11_TEXTURE2D_DESC desc;
    mv_texture->GetDesc(&desc);
    UINT width = desc.Width;
    UINT height = desc.Height;
    context->Dispatch((width + 15) / 16, (height + 15) / 16, 1);

    // 5. Unbind resources to avoid issues with later rendering passes
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    ID3D11ShaderResourceView* nullSRV[] = { nullptr };
    context->CSSetShaderResources(0, 1, nullSRV);
    ID3D11Buffer* nullCB[] = { nullptr };
    context->CSSetConstantBuffers(0, 1, nullCB);
    context->CSSetShader(nullptr, nullptr, 0);
}

// This is the same helper function from your D3D12 code, it's compatible.
DXGI_FORMAT MotionVectorReprojectionD3D11::getCorrectDXGIFormat(DXGI_FORMAT Format) {
    switch (Format) {
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return Format;
    }
}