#include "ShaderDebugOverlay.h"
#include <directxtk12-src/Src/d3dx12.h>
#include <directxtk12-src/Src/PlatformHelpers.h>
#include <Framework.hpp>
#include <ModSettings.h>
#include <d3d12.h>
#include <experimental/DebugUtils.h>
#include <imgui.h>
#include <mods/VR.hpp>
#include <nvidia/UpscalerAfrNvidiaModule.h>
#include <amd/UpscalerFsr31Module.h>

#ifdef COMPILE_SHADERS
namespace shaders::debug {
    #include "debug_layer_ps.h"
    #include "debug_layer_vs.h"
}
#endif
namespace
{
    struct ShaderConstants {
        float scale;           // Scale factor for visualization
        uint32_t channel_mask; // Bitmask: bit0=R, bit1=G, bit2=B, bit3=A
        uint32_t show_abs;     // Show absolute values
        uint32_t pad;          // Padding to align to 16 bytes
        glm::vec2 mvecScale{0.5f, -0.5f};
        glm::vec2 pad2;
    };
    static_assert(sizeof(ShaderConstants) % 16 == 0, "YourStruct size must be a multiple of 16 bytes");
    constexpr int CONSTANTS_COUNT = sizeof(ShaderConstants) / 4;
}



bool ShaderDebugOverlay::CreateRootSignature(ID3D12Device* device)
{
    HRESULT hr;
    CD3DX12_DESCRIPTOR_RANGE1 descriptorRange[1];
    CD3DX12_ROOT_PARAMETER1 rootParams[2];

    descriptorRange[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);
    rootParams[0].InitAsDescriptorTable(1, &descriptorRange[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParams[1].InitAsConstants(CONSTANTS_COUNT, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_STATIC_SAMPLER_DESC staticSamplers[2];

    // D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
    staticSamplers[0].Init(0, D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // D3D12_FILTER_MIN_MAG_MIP_POINT
    staticSamplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                                                    | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(_countof(rootParams), rootParams,
                               _countof(staticSamplers), staticSamplers, rootSignatureFlags);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, signature.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr)) {
        if (error)
            spdlog::error("[ShaderDebug] Error: D3DX12SerializeVersionedRootSignature {}", 
                         static_cast<char*>(error->GetBufferPointer()));
        return false;
    }
    
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), 
                                     IID_PPV_ARGS(m_rootSignature.GetAddressOf()));
    if (FAILED(hr)) {
        spdlog::error("[ShaderDebug] Error: CreateRootSignature failed");
        return false;
    }
    
    spdlog::info("[ShaderDebug] Root signature created.");
    return true;
}

bool ShaderDebugOverlay::CreatePipelineStates(ID3D12Device* device, DXGI_FORMAT backBufferFormat)
{
    // Debug layer PSO
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC debug_layer_pso = {};
        debug_layer_pso.InputLayout = { };
        debug_layer_pso.pRootSignature = m_rootSignature.Get();
#ifdef COMPILE_SHADERS
        debug_layer_pso.VS = CD3DX12_SHADER_BYTECODE(&shaders::debug::g_vs_main, sizeof(shaders::debug::g_vs_main));
        debug_layer_pso.PS = CD3DX12_SHADER_BYTECODE(&shaders::debug::g_ps_main, sizeof(shaders::debug::g_ps_main));
#endif
        auto blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

        debug_layer_pso.BlendState = blendDesc;
        debug_layer_pso.DepthStencilState             = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        debug_layer_pso.DepthStencilState.DepthEnable = false;
        debug_layer_pso.SampleMask                    = UINT_MAX;
        debug_layer_pso.RasterizerState               = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT); // a default rasterizer state.
        debug_layer_pso.PrimitiveTopologyType         = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        debug_layer_pso.NumRenderTargets              = 1;
        debug_layer_pso.DSVFormat                     = DXGI_FORMAT_UNKNOWN;
        debug_layer_pso.RTVFormats[0]                 = backBufferFormat;
        debug_layer_pso.SampleDesc.Count              = 1;
        debug_layer_pso.SampleDesc.Quality            = 0;

        ThrowIfFailed(device->CreateGraphicsPipelineState(&debug_layer_pso, IID_PPV_ARGS(m_debug_layer_pso.GetAddressOf())));
        spdlog::info("[VR] Debug layer PSO created.");
    }
    
    return true;
}

D3D12_SHADER_RESOURCE_VIEW_DESC getSRVdesc1(const D3D12_RESOURCE_DESC& desc) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ShaderDebugOverlay::getCorrectDXGIFormat(desc.Format);
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;
    srvDesc.Texture2D.MostDetailedMip = 0;
    return srvDesc;
}

void ShaderDebugOverlay::Draw(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, const D3D12_CPU_DESCRIPTOR_HANDLE* rtv_handle) {
    if (!m_initialized) {
        return;
    }

    if (!isValid()) {
        return;
    }

    auto device = g_framework->get_d3d12_hook()->get_device();
    if (!device) {
        return;
    }

//    mvReprojection->commandContext.wait(10);

    const auto render_target_desc = resource->GetDesc();

    {
        D3D12_RESOURCE_BARRIER barriers[3]
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(m_motion_vector_buffer[0].Get(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_motion_vector_buffer[1].Get(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        commandList->ResourceBarrier(3, barriers);
    }

    static ShaderConstants constants{};
    constants.scale = m_scale;
    constants.channel_mask = m_debug_axis;
    constants.show_abs = m_show_abs ? 1 : 0;
    constants.mvecScale = mvecScale;

    const UINT half_width = m_debug_width / 2;

    commandList->SetPipelineState(m_debug_layer_pso.Get());
    commandList->OMSetRenderTargets(1, rtv_handle, FALSE, nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->SetGraphicsRoot32BitConstants(1, CONSTANTS_COUNT, &constants, 0);
    ID3D12DescriptorHeap* descriptorHeaps[] = { m_srv_heap->Heap() };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    // Left half — left eye
    D3D12_VIEWPORT leftVP{ 0.f, 0.f, (float)half_width, (float)m_debug_height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
    D3D12_RECT     leftSR{ 0, 0, (LONG)half_width, (LONG)m_debug_height };
    commandList->RSSetViewports(1, &leftVP);
    commandList->RSSetScissorRects(1, &leftSR);
    commandList->SetGraphicsRootDescriptorTable(0, m_srv_heap->GetGpuHandle(SRV_HEAP::MVEC_LEFT));
    commandList->DrawInstanced(6, 1, 0, 0);

    // Right half — right eye
    D3D12_VIEWPORT rightVP{ (float)half_width, 0.f, (float)half_width, (float)m_debug_height, D3D12_MIN_DEPTH, D3D12_MAX_DEPTH };
    D3D12_RECT     rightSR{ (LONG)half_width, 0, (LONG)m_debug_width, (LONG)m_debug_height };
    commandList->RSSetViewports(1, &rightVP);
    commandList->RSSetScissorRects(1, &rightSR);
    commandList->SetGraphicsRootDescriptorTable(0, m_srv_heap->GetGpuHandle(SRV_HEAP::MVEC_RIGHT));
    commandList->DrawInstanced(6, 1, 0, 0);

    {
        D3D12_RESOURCE_BARRIER barriers[3]
        {
            CD3DX12_RESOURCE_BARRIER::Transition(resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_motion_vector_buffer[0].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_GENERIC_READ),
            CD3DX12_RESOURCE_BARRIER::Transition(m_motion_vector_buffer[1].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_GENERIC_READ),
        };
        commandList->ResourceBarrier(3, barriers);
    }
}


bool ShaderDebugOverlay::ValidateResource(ID3D12Resource* source, ComPtr<ID3D12Resource> buffers[2])
{
    if (source == nullptr) {
        return false;
    }
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    if (buffers[0] != nullptr) {
        D3D12_RESOURCE_DESC desc2 = buffers[0]->GetDesc();
        if (desc.Width != desc2.Width || desc.Height != desc2.Height || desc.Format != desc2.Format) {
            spdlog::info("Resource size mismatch {} {} {} {} {} {} {} {}", fmt::ptr(source), desc.Width, desc.Height, desc.Format, fmt::ptr(buffers[0].Get()), desc2.Width,
                         desc2.Height, desc2.Format);
            buffers[0].Reset();
            buffers[1].Reset();
        }
    }
    auto device = g_framework->get_d3d12_hook()->get_device();
    if (device == nullptr) {
        return false;
    }
    if (buffers[0] == nullptr || buffers[1] == nullptr) {
        D3D12_HEAP_PROPERTIES heap_props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        spdlog::info("[VR] Creating resource copy for AFR motion vector backbuffer. format={}", desc.Format);
        for (int i = 0; i < 2; i++) {
            HRESULT hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                         IID_PPV_ARGS(buffers[i].GetAddressOf()));
            if (FAILED(hr))
            {
                spdlog::error("[VR] Failed to create resource {} HRESULT: 0x{:X}", desc.Format, static_cast<unsigned int>(hr));
                return false;
            }
        }
        auto& instance = *Get();
        if (instance.m_srv_heap) {
            auto srv_desc = getSRVdesc1(desc);
            device->CreateShaderResourceView(buffers[0].Get(), &srv_desc, instance.m_srv_heap->GetCpuHandle(SRV_HEAP::MVEC_LEFT));
            device->CreateShaderResourceView(buffers[1].Get(), &srv_desc, instance.m_srv_heap->GetCpuHandle(SRV_HEAP::MVEC_RIGHT));
        }
    }
    return true;
}

void ShaderDebugOverlay::CopyResource(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* pSrcResource, ID3D12Resource* pDstResource, D3D12_RESOURCE_STATES srcState,
                                            D3D12_RESOURCE_STATES dstState)
{
    D3D12_RESOURCE_BARRIER barriers[2] = { CD3DX12_RESOURCE_BARRIER::Transition(pSrcResource, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                           CD3DX12_RESOURCE_BARRIER::Transition(pDstResource, dstState, D3D12_RESOURCE_STATE_COPY_DEST) };
    cmdList->ResourceBarrier(2, barriers);
    cmdList->CopyResource(pDstResource, pSrcResource);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter  = srcState;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter  = dstState;
    cmdList->ResourceBarrier(2, barriers);
}



bool ShaderDebugOverlay::setup(ID3D12Device* device, D3D12_RESOURCE_DESC& backBufferDesc)
{
    try {
        m_srv_heap = std::make_unique<DirectX::DescriptorHeap>(device,
                                                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, SRV_HEAP::COUNT);
        m_rtv_heap = std::make_unique<DirectX::DescriptorHeap>(device,
                                                                   D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 1);

    } catch(...) {
        spdlog::error("Failed to create SRV/RTV descriptor heap for MotionVectorFix");
        return false;
    }

    if (!CreateRootSignature(device) ||
        !CreatePipelineStates(device, backBufferDesc.Format))
    {
        spdlog::error("Failed to setup MotionVectorFix");
        return false;
    }

    if(!m_commandContext.setup(L"ShaderDebugOverlay")) {
        spdlog::error("Failed to setup command context for ShaderDebugOverlay");
        return false;
    }
    
    // Create debug texture: full width (both eyes side by side), half height
    m_debug_width = (UINT)backBufferDesc.Width;
    m_debug_height = (UINT)backBufferDesc.Height / 2;
    
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = m_debug_width;
    texDesc.Height = m_debug_height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(m_image1.GetAddressOf())))) {
        spdlog::error("Failed to create debug texture");
        return false;
    }

    auto srv_desc = getSRVdesc1(texDesc);

    device->CreateRenderTargetView(m_image1.Get(), nullptr, m_rtv_heap->GetCpuHandle(0));
    device->CreateShaderResourceView(m_image1.Get(), &srv_desc, m_srv_heap->GetCpuHandle(SRV_HEAP::MVEC_PROCESSED));
    
    // Also create SRV in Framework heap for ImGui
    device->CreateShaderResourceView(m_image1.Get(), &srv_desc, g_framework->m_d3d12.get_cpu_srv(device, Framework::D3D12::SRV::DEBUG_OVERLAY));

    m_initialized = true;
    spdlog::info("[ShaderDebug] Initialized with debug texture {}x{}", m_debug_width, m_debug_height);
    return true;
}

void ShaderDebugOverlay::on_pre_imgui_frame()
{
    if (m_initialized && DebugUtils::config.debugShaders) {
        const D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle = m_rtv_heap->GetCpuHandle(0);
        Draw(m_commandContext.cmd_list.Get(), m_image1.Get(), &cpuHandle);
        m_commandContext.has_commands = true;
        m_commandContext.execute();
    }
}

void ShaderDebugOverlay::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    ImGui::Checkbox("Enable Shader Debug Overlay", &DebugUtils::config.debugShaders);
    
    if (!m_initialized || !m_image1) {
        ImGui::Text("Debug texture not ready");
        return;
    }
    
    if (!DebugUtils::config.debugShaders) {
        ImGui::Text("Enable debugShaders in settings to see output");
        return;
    }

    // Shader configuration controls
    ImGui::SliderFloat("Scale", &m_scale, 1.0f, 100.0f, "%.1f");
    ImGui::Checkbox("Show Absolute Values", &m_show_abs);

    ImGui::RadioButton("Debug Axis (X - positive R, negative G)", (int*)&m_debug_axis, 0);
    ImGui::RadioButton("Debug Axis (Y - positive B, negative G)", (int*)&m_debug_axis, 1);
    ImGui::Text("Size: %u x %u", m_debug_width, m_debug_height);

    // Get main window position to place debug window next to it
    ImVec2 mainWindowPos = ImGui::GetWindowPos();
    ImVec2 mainWindowSize = ImGui::GetWindowSize();
    
    // Calculate debug window position (to the right of main window)
    ImVec2 debugWindowPos = ImVec2(mainWindowPos.x + mainWindowSize.x + 10.0f, mainWindowPos.y);
    
    const float defaultWidth = static_cast<float>(m_debug_width);
    const float defaultHeight = static_cast<float>(m_debug_height);
    
    ImGui::SetNextWindowPos(debugWindowPos, ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(defaultWidth + 20.0f, defaultHeight + 40.0f), ImGuiCond_Once);
    
    if (ImGui::Begin("Debug Texture View", &DebugUtils::config.debugShaders)) {
        // Get available content region and scale image to fit
        ImVec2 availableSize = ImGui::GetContentRegionAvail();
        float aspectRatio = static_cast<float>(m_debug_width) / static_cast<float>(m_debug_height);
        
        float displayWidth = availableSize.x;
        float displayHeight = displayWidth / aspectRatio;
        
        // If height exceeds available, scale by height instead
        if (displayHeight > availableSize.y) {
            displayHeight = availableSize.y;
            displayWidth = displayHeight * aspectRatio;
        }
        
        auto device = g_framework->get_d3d12_hook()->get_device();
        auto gpu_handle = g_framework->m_d3d12.get_gpu_srv(device, Framework::D3D12::SRV::DEBUG_OVERLAY);
        ImGui::Image((ImTextureID)gpu_handle.ptr, ImVec2(displayWidth, displayHeight));
    }
    ImGui::End();
}