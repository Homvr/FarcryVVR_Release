#include "VariableRateShadingImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <thread>

#include <directxtk12-src/Src/d3dx12.h>
#include <Framework.hpp>
#include <mods/vr/d3d12/CommandContext.hpp>
#include <spdlog/spdlog.h>

bool VariableRateShadingImage::Setup(ID3D12Device* device) {
    if (device == nullptr) {
        spdlog::warn("[VR] VariableRateShadingImage setup called with null device");
        return false;
    }

    if (m_initialized) {
        Reset();
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options, sizeof(options)))) {
        spdlog::warn("[VR] Failed to query D3D12_OPTIONS6 for VRS support");
        return false;
    }

    if (options.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2) {
        spdlog::info("[VR] VRS Tier 2 not supported, disabling shading rate image");
        return false;
    }

    if (options.ShadingRateImageTileSize == 0) {
        spdlog::warn("[VR] Reported VRS tile size is zero, aborting shading rate image setup");
        return false;
    }

    m_tileSize = options.ShadingRateImageTileSize;
    m_supportsAdditionalRates = options.AdditionalShadingRatesSupported != FALSE;

    m_initialized = true;
    if (!m_commandContext.setup(L"VariableRateShadingImage")) {
        spdlog::error("[VR] Failed to setup command context for VariableRateShadingImage");
        m_commandContext.reset();
        return false;
    }
    spdlog::info("[VR] VariableRateShadingImage initialized (tile {}px, additional rates {})", m_tileSize, m_supportsAdditionalRates);
    return true;
}

void VariableRateShadingImage::Update(/*const UpdateConfig& config*/) {
    m_radii[0] = m_fine_radius->value();
    m_radii[1] = m_coarse_radius->value();
    if (m_initialized) {
        for (auto& slot : m_slots) {
            slot.resource.dirty = true;
        }
    }
}

ID3D12Resource* VariableRateShadingImage::GetResource(UINT rtWidth, UINT rtHeight) {
    if (!m_initialized || m_tileSize == 0) {
        return nullptr;
    }

    const UINT tilesX = static_cast<UINT>(std::ceil(static_cast<float>(rtWidth) / static_cast<float>(m_tileSize)));
    const UINT tilesY = static_cast<UINT>(std::ceil(static_cast<float>(rtHeight) / static_cast<float>(m_tileSize)));
    if(tilesX == 0 || tilesY == 0) {
        return nullptr;
    }
    int slotIndex = -1;
    auto result = findSlot(tilesX, tilesY, slotIndex);
    if (result == 0) {
        auto& resourceWrapper = m_slots[slotIndex].resource;
        resourceWrapper.frameLastUsed = frameCount;
        if(slotIndex == 0) {
            return nullptr;
        }
        return resourceWrapper.texture.Get();
    } else if (result == 1) {
        generateImagePattern(tilesX, tilesY, slotIndex);
    }
    return nullptr;
}


void VariableRateShadingImage::Reset() {
    spdlog::info("[VR] Resetting VariableRateShadingImage");
    m_initialized = false;
    m_commandContext.reset();

    for (auto& slot : m_slots) {
        slot.Reset();
    }
    m_tileSize = 0;
    m_supportsAdditionalRates = false;
}

void VariableRateShadingImage::on_frame()
{
    frameCount++;
}
//
//
//bool isVRSCandidate(UINT width, UINT height) {
//    auto rt_size = g_framework->get_d3d12_rt_size();
//    auto render_width = rt_size.x;
//    auto render_height = rt_size.y;
//    auto width_ratio = (float)width / render_width;
//    auto height_ratio = (float)height / render_height;
//    auto aspect_ratio_diff = fabsf(((float)render_width/(float)render_height) - ((float)width/(float)height));
//    if(fminf(width_ratio, height_ratio) >= 0.25 && aspect_ratio_diff <= 0.2f) {
//        return true;
//    }
//    return false;
//}


void VariableRateShadingImage::on_d3d12_set_render_targets(ID3D12GraphicsCommandList5* cmd_list, UINT num_rtvs, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle,
                                                           D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
    if (!m_initialized || !m_enabled->value()) {
        return;
    }
    static const D3D12_SHADING_RATE_COMBINER combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT] = {
        D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX};
    ID3D12Resource* pVRSResource = nullptr;
    /*if(num_rtvs < 5 || m_g_buffer_vrs->value()) */{
        for(UINT i = 0; i < num_rtvs; i++) {
            RTVDesc desc{};
            if (!m_rtv.get((rtvs + i)->ptr, desc)) {
                continue;
            }
            pVRSResource = GetResource(desc.w, desc.h);
            if (pVRSResource) {
                break;
            }
        }
        cmd_list->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
        cmd_list->RSSetShadingRateImage(pVRSResource);
    }
}

void VariableRateShadingImage::on_d3d12_create_render_target_view(ID3D12Device* device, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                                  D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if(pResource) {
        const D3D12_RESOURCE_DESC& desc = pResource->GetDesc();
        if(desc.Width > 1024 && desc.Height > 1024) {
            m_rtv.put(DestDescriptor.ptr, { static_cast<int>(desc.Width), static_cast<int>(desc.Height)});
        }
    }

}

void VariableRateShadingImage::on_d3d12_set_scissor_rects(ID3D12GraphicsCommandList5* cmd_list, UINT num_rects, const D3D12_RECT* rects)
{
    return;
    if (!m_initialized || !m_enabled->value()) {
        return;
    }

    static const D3D12_SHADING_RATE_COMBINER combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT] = {D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX};
    ID3D12Resource* pVRSResource = nullptr;
    for(int rect = 0; rect < (int)num_rects; rect++) {
        const D3D12_RECT& scissorRect = rects[rect];
        const UINT scissorWidth = static_cast<UINT>(std::max(0l, scissorRect.right - scissorRect.left));
        const UINT scissorHeight = static_cast<UINT>(std::max(0l, scissorRect.bottom - scissorRect.top));
        pVRSResource = GetResource(scissorWidth, scissorHeight);
        if(pVRSResource) {
            break;
        }
    }
    cmd_list->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
    cmd_list->RSSetShadingRateImage(pVRSResource);
}

void VariableRateShadingImage::generateImagePattern(UINT tilesX, UINT tilesY, int slotIndex)
{
    if (!m_initialized || m_tileSize == 0) {
        return;
    }

    std::scoped_lock _{m_mtx};

    int newSlot = -1;
    if(findSlot(tilesX, tilesY, newSlot) <= 0) {
        return;
    }
    auto& slot = m_slots[slotIndex];
    auto& resource = slot.resource;

    m_commandContext.wait(0xFFFFFFFF);
    auto* cmdList = m_commandContext.cmd_list.Get();
    if (cmdList == nullptr) {
        spdlog::error("[VR] CommandContext has no command list for VRS image");
        return;
    }
    if (resource.lost || !resource.texture || slot.tilesX != tilesX || slot.tilesY != tilesY) {
        if(resource.texture && resource.frameLastUsed >= frameCount - 5) {
            resource.lost = true;
            slot.tilesX = tilesX;
            slot.tilesY = tilesY;
            return;
        }
        if (recreateResources(resource, tilesX, tilesY)) {
            slot.tilesX = tilesX;
            slot.tilesY = tilesY;
            spdlog::info("[VR] VRS image resources recreated for ({}x{}) in slot {}", tilesX, tilesY, slotIndex);
        } else {
            spdlog::error("[VR] Failed to recreate VRS image resources for ({}x{})", tilesX, tilesY);
            return;
        }
    }

    if (!updateContents(resource, tilesX, tilesY)) {
        spdlog::error("[VR] Failed to update VRS image contents for ({}x{})", tilesX, tilesY);
        return;
    }

    resource.transition(m_commandContext, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);


    if (m_commandContext.has_commands) {
        m_commandContext.execute();
    }
    m_commandContext.wait(0xFFFFFFFF);
    spdlog::info("[VR] VRS Image for ({}x{}) generated in slot {}", tilesX, tilesY, slotIndex);
    resource.dirty = false;
    
}

bool VariableRateShadingImage::recreateResources(ResourceSlot::D3D12ResourceWrapper& resource, UINT tilesX, UINT tilesY) const {
    if (!m_initialized) {
        return false;
    }

    resource.Reset();
    auto pDevice = g_framework->get_d3d12_hook()->get_device();
    if( pDevice == nullptr) {
        spdlog::error("[VR] D3D12 Device was null when recreating VRS image resources");
        return false;
    }
    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Alignment = 0;
    texDesc.Width = tilesX;
    texDesc.Height = tilesY;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8_UINT;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(pDevice->CreateCommittedResource(&defaultHeap,
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &texDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr,
                                                 IID_PPV_ARGS(resource.texture.GetAddressOf())))) {
        spdlog::error("[VR] Failed to create shading rate image ({}x{})", tilesX, tilesY);
        return false;
    }

    const UINT64 uploadSize = GetRequiredIntermediateSize(resource.texture.Get(), 0, 1);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    if (FAILED(pDevice->CreateCommittedResource(&uploadHeap,
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &uploadDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr,
                                                 IID_PPV_ARGS(resource.upload.GetAddressOf())))) {
        spdlog::error("[VR] Failed to create shading rate upload buffer ({} bytes)", uploadSize);
        resource.texture.Reset();
        return false;
    }

#if defined(_DEBUG)
    // Create RGB debug texture for visualization (R = X scale, G = Y scale, B unused).
    D3D12_RESOURCE_DESC debugTexDesc{};
    debugTexDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    debugTexDesc.Alignment = 0;
    debugTexDesc.Width = tilesX;
    debugTexDesc.Height = tilesY;
    debugTexDesc.DepthOrArraySize = 1;
    debugTexDesc.MipLevels = 1;
    debugTexDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    debugTexDesc.SampleDesc.Count = 1;
    debugTexDesc.SampleDesc.Quality = 0;
    debugTexDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    debugTexDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(pDevice->CreateCommittedResource(&defaultHeap,
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &debugTexDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr,
                                                 IID_PPV_ARGS(resource.debugTexture.GetAddressOf())))) {
        spdlog::error("[VR] Failed to create VRS debug texture ({}x{})", tilesX, tilesY);
        resource.texture.Reset();
        resource.upload.Reset();
        return false;
    }

    const UINT64 debugUploadSize = GetRequiredIntermediateSize(resource.debugTexture.Get(), 0, 1);
    auto debugUploadDesc = CD3DX12_RESOURCE_DESC::Buffer(debugUploadSize);
    if (FAILED(pDevice->CreateCommittedResource(&uploadHeap,
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &debugUploadDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr,
                                                 IID_PPV_ARGS(resource.debugUpload.GetAddressOf())))) {
        spdlog::error("[VR] Failed to create VRS debug upload buffer ({} bytes)", debugUploadSize);
        resource.texture.Reset();
        resource.upload.Reset();
        resource.debugTexture.Reset();
        return false;
    }

    // Create SRV heap and SRV for the debug texture so it can be sampled in a debug view.
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    if (FAILED(pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(resource.debugSrvHeap.GetAddressOf())))) {
        spdlog::error("[VR] Failed to create VRS debug SRV heap");
        resource.texture.Reset();
        resource.upload.Reset();
        resource.debugTexture.Reset();
        resource.debugUpload.Reset();
        return false;
    }

    resource.debugSrvCpuHandle = resource.debugSrvHeap->GetCPUDescriptorHandleForHeapStart();
    resource.debugSrvGpuHandle = resource.debugSrvHeap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = debugTexDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    pDevice->CreateShaderResourceView(resource.debugTexture.Get(), &srvDesc, resource.debugSrvCpuHandle);
#endif
    resource.state = D3D12_RESOURCE_STATE_COPY_DEST;
    resource.frameLastUsed = frameCount;
    resource.lost = false;
    return true;
}

bool VariableRateShadingImage::updateContents(ResourceSlot::D3D12ResourceWrapper& slot, UINT tilesX, UINT tilesY)
{
    if (!m_initialized || !slot.texture || !slot.upload) {
        return false;
    }

    const size_t requiredSize = static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY);
    std::vector<uint8_t> lTileData;
    lTileData.resize(requiredSize);

    populateTileData(tilesX, tilesY, lTileData);

    D3D12_SUBRESOURCE_DATA subresource{};
    subresource.pData = lTileData.data();
    subresource.RowPitch = static_cast<LONG_PTR>(tilesX);
    subresource.SlicePitch = static_cast<LONG_PTR>(requiredSize);

    slot.transition(m_commandContext, D3D12_RESOURCE_STATE_COPY_DEST);

    UpdateSubresources(m_commandContext.cmd_list.Get(), slot.texture.Get(), slot.upload.Get(), 0, 0, 1, &subresource);

    m_commandContext.has_commands = true;

#if defined(_DEBUG)
    // Also upload debug RGB visualization if the debug texture exists.
    if (slot.debugTexture && slot.debugUpload) {
        const size_t debugRequiredSize = static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY) * 4; // RGBA8
        std::vector<uint8_t> debugData(debugRequiredSize, 0);

        // Rebuild tile data to encode RG based on D3D12_SHADING_RATE for each texel.
        for (UINT y = 0; y < tilesY; ++y) {
            for (UINT x = 0; x < tilesX; ++x) {
                const size_t idx = static_cast<size_t>(y) * tilesX + x;
                const auto rate = static_cast<D3D12_SHADING_RATE>(lTileData[idx]);

                float xScale = 1.0f;
                float yScale = 1.0f;

                switch (rate) {
                case D3D12_SHADING_RATE_1X1: xScale = 0.25f; yScale = 0.25f; break;
                case D3D12_SHADING_RATE_1X2: xScale = 0.25f; yScale = 0.5f;  break;
                case D3D12_SHADING_RATE_2X1: xScale = 0.5f;  yScale = 0.25f; break;
                case D3D12_SHADING_RATE_2X2: xScale = 0.5f;  yScale = 0.5f;  break;
                case D3D12_SHADING_RATE_2X4: xScale = 0.5f;  yScale = 1.0f;  break;
                case D3D12_SHADING_RATE_4X2: xScale = 1.0f;  yScale = 0.5f;  break;
                case D3D12_SHADING_RATE_4X4: xScale = 1.0f;  yScale = 1.0f;  break;
                default:                      xScale = 0.25f; yScale = 0.25f; break;
                }

                const uint8_t r = static_cast<uint8_t>(std::roundf(std::clamp(xScale, 0.0f, 1.0f) * 255.0f));
                const uint8_t g = static_cast<uint8_t>(std::roundf(std::clamp(yScale, 0.0f, 1.0f) * 255.0f));

                debugData[idx * 4 + 0] = r;
                debugData[idx * 4 + 1] = g;
                debugData[idx * 4 + 2] = 0;   // B channel unused
                debugData[idx * 4 + 3] = 255; // A = 1.0
            }
        }

        D3D12_SUBRESOURCE_DATA debugSubresource{};
        debugSubresource.pData = debugData.data();
        debugSubresource.RowPitch = static_cast<LONG_PTR>(tilesX * 4);
        debugSubresource.SlicePitch = static_cast<LONG_PTR>(debugRequiredSize);

        UpdateSubresources(m_commandContext.cmd_list.Get(), slot.debugTexture.Get(), slot.debugUpload.Get(), 0, 0, 1, &debugSubresource);
        m_commandContext.has_commands = true;
    }
#endif
    return true;
}


void VariableRateShadingImage::populateTileData(UINT tilesX, UINT tilesY, std::vector<uint8_t>& outData ) {
    const float centerX = (static_cast<float>(tilesX) - 1.0f) * 0.5f;
    const float centerY = (static_cast<float>(tilesY) - 1.0f) * 0.5f;

    const float fineRadius = std::clamp(m_radii[0], 0.0f, 1.0f);
    const float mediumRadius = std::clamp(m_radii[1], 0.0f, 1.0f);
    const float fineRadiusSq = fineRadius * fineRadius;
    const float mediumRadiusSq = mediumRadius * mediumRadius;

    const D3D12_SHADING_RATE coarseRate = m_supportsAdditionalRates ? D3D12_SHADING_RATE_4X4 : D3D12_SHADING_RATE_2X2;
    const D3D12_SHADING_RATE rateTable[3] = {
        D3D12_SHADING_RATE_1X1,
        D3D12_SHADING_RATE_2X2,
        coarseRate
    };

    for (UINT y = 0; y < tilesY; ++y) {
        for (UINT x = 0; x < tilesX; ++x) {
            const float dx = (static_cast<float>(x) - centerX)/centerX;
            const float dy = (static_cast<float>(y) - centerY)/centerY;
            const float distanceSq = dx * dx + dy * dy;

            size_t zone = 0;
            if (distanceSq > fineRadiusSq) {
                zone = 1;
                if (distanceSq > mediumRadiusSq) {
                    zone = 2;
                }
            }

            outData[static_cast<size_t>(y) * tilesX + x] = static_cast<uint8_t>(rateTable[zone]);
        }
    }
}

int VariableRateShadingImage::findSlot(UINT tilesX, UINT tilesY, int& outSlot)
{
    for (int i = 0; i < kMaxSlots; ++i) {
        const auto& slot = m_slots[i];
        int         matchResolution = slot.MatchResolution(tilesX, tilesY);
        if (matchResolution == 0 && slot.IsReady()) {
            outSlot = i;
            return 0;
        } else if (matchResolution == 1 || matchResolution == 0) {
            outSlot = i;
            return 1;
        }
    }
    return -1;
}

void VariableRateShadingImage::on_device_reset()
{
    Reset();
}

void VariableRateShadingImage::on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc)
{
    Setup(pDevice4);
}

std::optional<std::string> VariableRateShadingImage::on_initialize()
{
    Setup(g_framework->get_d3d12_hook()->get_device());
    return Mod::on_initialize();
}


void VariableRateShadingImage::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }
    m_enabled->draw("Enable Static VRS Image");
//    m_g_buffer_vrs->draw("Enable G-Buffer VRS");
    if(m_fine_radius->draw("1x1 Radius")) {
        Update();
    }
    if(m_coarse_radius->draw("2x2 Radius")) {
        Update();
    }
}

void VariableRateShadingImage::on_config_load(const utility::Config& cfg, bool set_defaults)
{
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
    Update();
}

void VariableRateShadingImage::on_config_save(utility::Config& cfg)
{
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}


#if defined(_DEBUG)
const VariableRateShadingImage::ResourceSlot* VariableRateShadingImage::GetLargestDebugSlot() const {
    if (!m_initialized) {
        return nullptr;
    }

    UINT bestArea = 0;
    const ResourceSlot* bestSlot = nullptr;

    for (auto& slot : m_slots) {
        auto& resource = slot.resource;
        if(slot.tilesX == 0 || slot.tilesY == 0) {
            continue;
        }
        if (!resource.debugTexture || !resource.debugSrvHeap || resource.dirty) {
            continue;
        }

        const UINT area = slot.tilesX * slot.tilesY;
        if (area > bestArea) {
            bestArea = area;
            bestSlot = &slot;
        }
    }

    return bestSlot;
}
#endif
