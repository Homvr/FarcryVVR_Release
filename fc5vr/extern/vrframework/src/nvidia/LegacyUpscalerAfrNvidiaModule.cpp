#ifdef STREAMLINE_LEGACY
#include "LegacyUpscalerAfrNvidiaModule.h"
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif
#include <aer/ConstantsPool.h>
//#include "sl_matrix_helpers.h"
#include <math/sl_matrix_helpers_copy.h>
#include <Framework.hpp>
#include <experimental/DebugUtils.h>
#include <imgui.h>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

std::optional<std::string> LegacyUpscalerAfrNvidiaModule::on_initialize()
{
    InstallHooks();
    return Mod::on_initialize();
}

void LegacyUpscalerAfrNvidiaModule::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    m_enabled->draw("Enable NVIDIA AFR");
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_fix->draw("Motion Vector Reprojection Fix");
#endif
}

void LegacyUpscalerAfrNvidiaModule::on_config_load(const utility::Config& cfg, bool set_defaults)
{
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
}

void LegacyUpscalerAfrNvidiaModule::on_config_save(utility::Config& cfg)
{
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

void LegacyUpscalerAfrNvidiaModule::on_device_reset()
{
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_reprojection.on_device_reset();
#endif
}

void LegacyUpscalerAfrNvidiaModule::on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc)
{
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_reprojection.on_d3d12_initialize(pDevice4, desc);
#endif
}

void LegacyUpscalerAfrNvidiaModule::InstallHooks()
{
    static HMODULE p_hm_sl_interposter = nullptr;
    if (p_hm_sl_interposter) {
        return;
    }
    spdlog::info("Installing sl.interposer hooks");
    while ((p_hm_sl_interposter = GetModuleHandle("sl.interposer.dll")) == nullptr) {
        std::this_thread::yield();
    }

    auto slSetTagFn = GetProcAddress(p_hm_sl_interposter, "slSetTag");
    m_set_tag_hook  = std::make_unique<FunctionHook>((void**)slSetTagFn, (void*)&LegacyUpscalerAfrNvidiaModule::on_slSetTag);
    if (!m_set_tag_hook->create()) {
        spdlog::error("Failed to hook slSetTag");
    }

    auto slEvaluateFeatureFn = GetProcAddress(p_hm_sl_interposter, "slEvaluateFeature");
    m_evaluate_feature_hook  = std::make_unique<FunctionHook>((void**)slEvaluateFeatureFn, (void*)&LegacyUpscalerAfrNvidiaModule::on_slEvaluateFeature);
    if (!m_evaluate_feature_hook->create()) {
        spdlog::error("Failed to hook slEvaluateFeature");
    }

    auto slSetConstantsFn = GetProcAddress(p_hm_sl_interposter, "slSetConstants");
    m_set_constants_hook  = std::make_unique<FunctionHook>((void**)slSetConstantsFn, (void*)&LegacyUpscalerAfrNvidiaModule::on_slSetConstants);
    if (!m_set_constants_hook->create()) {
        spdlog::error("Failed to hook slSetConstants");
    }

    auto slSetFeatureConstantsFn = GetProcAddress(p_hm_sl_interposter, "slSetFeatureConstants");

    m_set_feature_constants_hook = std::make_unique<FunctionHook>((void**)slSetFeatureConstantsFn, (void*)&LegacyUpscalerAfrNvidiaModule::on_slSetFeatureConstants);
    if (!m_set_feature_constants_hook->create()) {
        spdlog::error("Failed to hook slSetOptions");
    }

    auto slAllocateResourcesFn = GetProcAddress(p_hm_sl_interposter, "slAllocateResources");
    m_allocate_resources_hook  = std::make_unique<FunctionHook>((void**)slAllocateResourcesFn, (void*)&LegacyUpscalerAfrNvidiaModule::on_slAllocateResources);
    if (!m_allocate_resources_hook->create()) {
        spdlog::error("Failed to hook slAllocateResources");
    }

    auto slFreeResourcesFn = GetProcAddress(p_hm_sl_interposter, "slFreeResources");
    m_free_resources_hook  = std::make_unique<FunctionHook>((void**)slFreeResourcesFn, (void*)&LegacyUpscalerAfrNvidiaModule::on_slFreeResources);
    if (!m_free_resources_hook->create()) {
        spdlog::error("Failed to hook slFreeResources");
    }
    spdlog::info("sl.interposer Hooks installed");
}


#ifdef MOTION_VECTOR_REPROJECTION
void LegacyUpscalerAfrNvidiaModule::ReprojectMotionVectors(sl::CommandBuffer* cmdBuffer) {
    if(!m_motion_vector_reprojection.isInitialized() || !lastDepthResource || !lastMvecResource) {
        lastDepthResource = nullptr;
        Depthstate = 0;
        lastMvecResource = nullptr;
        MvecState = 0;
        return;
    }
    static auto vr = VR::get();

    {
        auto mv_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto mv_native_resource = (ID3D12Resource*)lastMvecResource;
        auto depth_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto depth_native_resource = (ID3D12Resource*)lastDepthResource;

        auto command_list    = (ID3D12GraphicsCommandList*)cmdBuffer;
        if(m_enabled->value() && m_motion_vector_fix->value()) {
            m_motion_vector_reprojection.ProcessMotionVectors(mv_native_resource, mv_state, depth_native_resource, depth_state, vr->m_render_frame_count, command_list);
        }
#ifdef _DEBUG
        static auto shader_debug_overlay = ShaderDebugOverlay::Get();
        if(DebugUtils::config.debugShaders && ShaderDebugOverlay::ValidateResource(mv_native_resource, shader_debug_overlay->m_motion_vector_buffer)) {
            ShaderDebugOverlay::CopyResource(command_list, mv_native_resource, shader_debug_overlay->m_motion_vector_buffer[vr->m_render_frame_count % 2].Get(), mv_state, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
#endif
    }
    lastDepthResource = nullptr;
    Depthstate = 0;
    lastMvecResource = nullptr;
    MvecState = 0;
}
#endif

bool LegacyUpscalerAfrNvidiaModule::on_slSetTag(const sl::Resource *resource, sl::BufferType tag, uint32_t id, const sl::Extent* extent)
{
    static auto            instance    = LegacyUpscalerAfrNvidiaModule::Get();
    static auto            original_fn = instance->m_set_tag_hook->get_original<decltype(LegacyUpscalerAfrNvidiaModule::on_slSetTag)>();
    static auto            vr          = VR::get();
    SCOPE_PROFILER();
#ifdef MOTION_VECTOR_REPROJECTION
    if(tag == sl::eBufferTypeDepth) {
        instance->lastDepthResource = resource ? resource->native : nullptr;
        instance->Depthstate = resource ? resource->state : 0;
    } else if(tag == sl::eBufferTypeMVec) {
        instance->lastMvecResource = resource ? resource->native : nullptr;
        instance->MvecState = resource ? resource->state : 0;
    }
#endif
    if(vr->m_render_frame_count % 2 == 0 && instance->m_enabled->value()) {
        return original_fn(resource, tag, instance->m_afr_viewport_id, extent);
    }
    auto result = original_fn(resource, tag, id, extent);
    return result;
}

namespace {
    bool supported_afr_feature(sl::Feature feature)
    {
        return feature == sl::eFeatureDLSS;
    }
}

bool LegacyUpscalerAfrNvidiaModule::on_slEvaluateFeature(sl::CommandBuffer* cmdBuffer, sl::Feature feature, uint32_t frameIndex, uint32_t id)
{
    static auto instance    = LegacyUpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_evaluate_feature_hook->get_original<decltype(LegacyUpscalerAfrNvidiaModule::on_slEvaluateFeature)>();
    if(!supported_afr_feature(feature)) {
        return original_fn(cmdBuffer, feature, frameIndex, id);
    }
    SCOPE_PROFILER();

#ifdef MOTION_VECTOR_REPROJECTION
    //TODO might process same resource twice per frame we to track last processed resource per frame
    {
        /**
        * DLSS uses per-pixel motion vectors as a key component of its core algorithm. The motion vectors map a
        * pixel from the current frame to its position in the previous frame. That is, when the motion vector for
        * the pixel is added to the pixel's current location, the result is the location the pixel occupied in the
        * previous frame
        * MV = PAST - CURRENT
        * DLSS internally expect mvector to be in UV space notmalized to [-1, 1] it's not NDC space
        */
        instance->ReprojectMotionVectors(cmdBuffer);

    }
#endif
    static auto            vr          = VR::get();
    if(vr->m_render_frame_count % 2 == 0 && instance->m_enabled->value()) {
        return original_fn(cmdBuffer, feature, frameIndex, instance->m_afr_viewport_id);
    }
    auto result = original_fn(cmdBuffer, feature, frameIndex, id);
    return result;
}

bool LegacyUpscalerAfrNvidiaModule::on_slSetConstants(sl::Constants& values, uint32_t frameIndex, uint32_t id)
{
    SCOPE_PROFILER();
    static auto instance = LegacyUpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_set_constants_hook->get_original<decltype(LegacyUpscalerAfrNvidiaModule::on_slSetConstants)>();
#ifdef MOTION_VECTOR_REPROJECTION
    instance->m_motion_vector_reprojection.m_mvecScale.x = values.mvecScale.x;
    instance->m_motion_vector_reprojection.m_mvecScale.y = values.mvecScale.y;
#endif
#ifdef _DEBUG
    if(DebugUtils::config.debugShaders) {
        ShaderDebugOverlay::SetMvecScale(values.mvecScale.x, values.mvecScale.y);
    }
#endif
    static auto            vr          = VR::get();
    if(vr->m_render_frame_count % 2 == 0 && instance->m_enabled->value()) {
        return original_fn(values, frameIndex, instance->m_afr_viewport_id);
    }
    auto result = original_fn(values, frameIndex, id);
    return result;
}

bool LegacyUpscalerAfrNvidiaModule::on_slSetFeatureConstants(sl::Feature feature, const void *consts, uint32_t frameIndex, uint32_t id)
{
    static auto     instance         = LegacyUpscalerAfrNvidiaModule::Get();
    static auto     original_fn      = instance->m_set_feature_constants_hook->get_original<decltype(LegacyUpscalerAfrNvidiaModule::on_slSetFeatureConstants)>();
    if(supported_afr_feature(feature) && instance->m_enabled->value()) {
        original_fn(feature, consts, frameIndex, instance->m_afr_viewport_id);
    }
    return original_fn(feature, consts, frameIndex, id);
}

bool LegacyUpscalerAfrNvidiaModule::on_slFreeResources(sl::Feature feature, uint32_t id)
{
    static auto instance    = LegacyUpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_free_resources_hook->get_original<decltype(LegacyUpscalerAfrNvidiaModule::on_slFreeResources)>();
    if(supported_afr_feature(feature) && instance->m_enabled->value()) {
        original_fn(feature, instance->m_afr_viewport_id);
    }
    spdlog::info("slFreeResources called for feature {:x} viewport {:x}", (UINT)feature, (UINT)id);
    return original_fn(feature, id);
}

bool LegacyUpscalerAfrNvidiaModule::on_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, uint32_t id)
{
    static auto instance    = LegacyUpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_allocate_resources_hook->get_original<decltype(LegacyUpscalerAfrNvidiaModule::on_slAllocateResources)>();
    if(supported_afr_feature(feature) && instance->m_enabled->value()) {
        original_fn(cmdBuffer, feature, instance->m_afr_viewport_id);
    }
    spdlog::info("slAllocateResources called for feature {:x} viewport {:x}", (UINT)feature, (UINT)id);
    return original_fn(cmdBuffer, feature, id);
}
#endif
