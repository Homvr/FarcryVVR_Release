#ifndef STREAMLINE_LEGACY
#include "UpscalerAfrNvidiaModule.h"
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif
#include "sl_matrix_helpers.h"
#include <Framework.hpp>
#include <experimental/DebugUtils.h>
#include <imgui.h>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

std::optional<std::string> UpscalerAfrNvidiaModule::on_initialize()
{
    InstallHooks();
    return Mod::on_initialize();
}

void UpscalerAfrNvidiaModule::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    m_enabled->draw("Enable NVIDIA AFR");
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_fix->draw("Motion Vector Reprojection Fix");
#endif
}

void UpscalerAfrNvidiaModule::on_config_load(const utility::Config& cfg, bool set_defaults)
{
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
}

void UpscalerAfrNvidiaModule::on_config_save(utility::Config& cfg)
{
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

void UpscalerAfrNvidiaModule::on_device_reset()
{
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_reprojection.on_device_reset();
#endif
}

void UpscalerAfrNvidiaModule::on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc)
{
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_reprojection.on_d3d12_initialize(pDevice4, desc);
#endif
}

void UpscalerAfrNvidiaModule::InstallHooks()
{
    static HMODULE p_hm_sl_interposter = nullptr;
    if (p_hm_sl_interposter) {
        return;
    }
    spdlog::info("Installing sl.interposer hooks");
    while ((p_hm_sl_interposter = GetModuleHandle("sl.interposer.dll")) == nullptr) {
        std::this_thread::yield();
    }

//    auto getNewFrameTokenFn = GetProcAddress(g_interposer, "slGetNewFrameToken");
//    m_get_new_frame_token_hook = std::make_unique<FunctionHook>((void **)getNewFrameTokenFn, (void *) &DlssDualView::on_slGetNewFrameToken);
//    if(!m_get_new_frame_token_hook->create()) {
//        spdlog::error("Failed to hook slGetNewFrameToken");
//    }

    auto slSetTagFn = GetProcAddress(p_hm_sl_interposter, "slSetTag");
    m_set_tag_hook  = std::make_unique<FunctionHook>((void**)slSetTagFn, (void*)&UpscalerAfrNvidiaModule::on_slSetTag);
    if (!m_set_tag_hook->create()) {
        spdlog::error("Failed to hook slSetTag");
    }

    auto slEvaluateFeatureFn = GetProcAddress(p_hm_sl_interposter, "slEvaluateFeature");
    m_evaluate_feature_hook  = std::make_unique<FunctionHook>((void**)slEvaluateFeatureFn, (void*)&UpscalerAfrNvidiaModule::on_slEvaluateFeature);
    if (!m_evaluate_feature_hook->create()) {
        spdlog::error("Failed to hook slEvaluateFeature");
    }

    auto slSetConstantsFn = GetProcAddress(p_hm_sl_interposter, "slSetConstants");
    m_set_constants_hook  = std::make_unique<FunctionHook>((void**)slSetConstantsFn, (void*)&UpscalerAfrNvidiaModule::on_slSetConstants);
    if (!m_set_constants_hook->create()) {
        spdlog::error("Failed to hook slSetConstants");
    }

    using get_feature_funciton = sl::Result (*)(sl::Feature feature, const char* functionName, void*& function);
    auto get_feature_function = (get_feature_funciton) GetProcAddress(p_hm_sl_interposter, "slGetFeatureFunction");
    void* dlssSetOptionsFn;
    get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", dlssSetOptionsFn);
    m_dlss_set_options_hook = std::make_unique<FunctionHook>((void**)dlssSetOptionsFn, (void*)&UpscalerAfrNvidiaModule::on_dlssSetOptions);
    if (!m_dlss_set_options_hook->create()) {
        spdlog::error("Failed to hook slSetOptions");
    }
//    void* slDLSSDSetOptionsFn;
//    get_feature_function(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", slDLSSDSetOptionsFn);
//    m_dlssrr_set_options_hook = std::make_unique<FunctionHook>((void**)slDLSSDSetOptionsFn, (void*)&UpscalerAfrNvidiaModule::on_dlssrrSetOptions);
//    if (!m_dlssrr_set_options_hook->create()) {
//        spdlog::error("Failed to hook slDLSSDSetOptions");
//    }

#ifdef STREAMLINE_DLSS_RR
    void* slDLSSDSetOptionsFn;
    get_feature_function(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", slDLSSDSetOptionsFn);
    m_dlssrr_set_options_hook = std::make_unique<FunctionHook>((void**)slDLSSDSetOptionsFn, (void*)&UpscalerAfrNvidiaModule::on_dlssrrSetOptions);
    if (!m_dlssrr_set_options_hook->create()) {
        spdlog::error("Failed to hook slDLSSDSetOptions");
    }
#endif

    auto slAllocateResourcesFn = GetProcAddress(p_hm_sl_interposter, "slAllocateResources");
    m_allocate_resources_hook  = std::make_unique<FunctionHook>((void**)slAllocateResourcesFn, (void*)&UpscalerAfrNvidiaModule::on_slAllocateResources);
    if (!m_allocate_resources_hook->create()) {
        spdlog::error("Failed to hook slAllocateResources");
    }

    auto slFreeResourcesFn = GetProcAddress(p_hm_sl_interposter, "slFreeResources");
    m_free_resources_hook  = std::make_unique<FunctionHook>((void**)slFreeResourcesFn, (void*)&UpscalerAfrNvidiaModule::on_slFreeResources);
    if (!m_free_resources_hook->create()) {
        spdlog::error("Failed to hook slFreeResources");
    }

//        REL::Relocation<uintptr_t>  getDllsFrameTokenFnAddr{REL::ID(1078687)};
//        m_onGetDllsFrameToken = std::make_unique<FunctionHook>(getDllsFrameTokenFnAddr.address(), (uintptr_t)&DlssDualView::on_ceGetNewFrameToken);
//        m_onGetDllsFrameToken->create();

    spdlog::info("sl.interposer Hooks installed");
}

/*sl::Result UpscalerAfrNvidiaModule::on_slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex)
{
    auto instance = UpscalerAfrNvidiaModule::Get();

    auto original_fn = instance->m_get_new_frame_token_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slGetNewFrameToken)>();
    auto vr          = VR::get();
    //    spdlog::info("slGetNewFrameToken called {} frame {}", vr->get_current_render_eye() == VRRuntime::Eye::RIGHT,*frameIndex);
    //
    //    if(vr->is_hmd_active()  && !vr->m_left_trigger_down && vr->get_current_render_eye() == VRRuntime::Eye::RIGHT && instance->m_afr_frame_token != nullptr) {
    //        token = instance->m_afr_frame_token;
    //        return sl::Result::eOk;
    //
    //    }
    //    if(instance->first_frame == 0) {
    //        instance->first_frame = *frameIndex;
    //    }
    auto half_frame = *frameIndex / 2;
    auto result     = original_fn(token, frameIndex);
    //    original_fn(instance->m_afr_frame_token, &half_frame);
    return result;
}*/

#ifdef MOTION_VECTOR_REPROJECTION
void UpscalerAfrNvidiaModule::ReprojectMotionVectors(const sl::FrameToken& frame, sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer) {
    if(!m_motion_vector_reprojection.isInitialized()) {
        return;
    }
    sl::ResourceTag* mvTag = nullptr;
    sl::ResourceTag* depthTag = nullptr;
    for (uint32_t i = 0; i < numInputs; i++) {
        if(inputs[i]->structType == sl::ResourceTag::s_structType && ((sl::ResourceTag*) inputs[i])->type == sl::kBufferTypeMotionVectors) {
            mvTag = (sl::ResourceTag*)inputs[i];
        } else if(inputs[i]->structType == sl::ResourceTag::s_structType && ((sl::ResourceTag*) inputs[i])->type == sl::kBufferTypeDepth) {
            depthTag = (sl::ResourceTag*)inputs[i];
        }
    }
    static auto vr = VR::get();

    if(mvTag && depthTag && mvTag->resource && mvTag->resource->native && depthTag->resource && depthTag->resource->native) {
        auto mv_resource        = mvTag->resource;
        auto mv_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto mv_native_resource = (ID3D12Resource*)mv_resource->native;

        auto depth_resource        = depthTag->resource;
        auto depth_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto depth_native_resource = (ID3D12Resource*)depth_resource->native;

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
}
#endif


namespace {
    void log_native_call(const char* call, uint32_t frame, uint32_t viewport, int view, const sl::Constants* constants = nullptr)
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) >= 60) {
            return;
        }
        if (constants != nullptr) {
            spdlog::info("[DLSS native] {} thread {} frame {} viewport {} view {} camera ({:.3f},{:.3f},{:.3f})", call, GetCurrentThreadId(), frame, viewport, view,
                         constants->cameraPos.x, constants->cameraPos.y, constants->cameraPos.z);
        } else {
            spdlog::info("[DLSS native] {} thread {} frame {} viewport {} view {}", call, GetCurrentThreadId(), frame, viewport, view);
        }
    }
}

sl::Result UpscalerAfrNvidiaModule::on_slSetTag(sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    static auto            instance    = UpscalerAfrNvidiaModule::Get();
    static auto            original_fn = instance->m_set_tag_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slSetTag)>();
    static auto            vr          = VR::get();
    // spdlog::error("UNEXPECTED CALL TO slSetTag");
    // exit(1);
    if (s_native_views.load()) {
        log_native_call("slSetTag", 0, (uint32_t)viewport, s_native_view);
        s_native_tags[std::clamp(s_native_view + 1, 0, 2)].fetch_add(1);
        if (s_native_view == 1) {
            sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
            return original_fn(afr_viewport_handle, tags, numTags, cmdBuffer);
        }
        return original_fn(viewport, tags, numTags, cmdBuffer);
    }
    if(vr->m_render_frame_count % 2 == 0 && instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        return original_fn(afr_viewport_handle, tags, numTags, cmdBuffer);
    }
    auto result = original_fn(viewport, tags, numTags, cmdBuffer);
    return result;
}

namespace {
    bool supported_afr_feature(sl::Feature feature)
    {
#ifdef STREAMLINE_DLSS_RR
        if(feature == sl::kFeatureDLSS_RR) {
            return true;
        }
#endif
        return feature == sl::kFeatureDLSS;
    }
}

sl::Result UpscalerAfrNvidiaModule::on_slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_evaluate_feature_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slEvaluateFeature)>();
    static auto vr          = VR::get();
//
//    constexpr sl::BufferType kBufferTypeReactiveMaskHint = 36;
//
//    // Filter out kBufferTypeReactiveMaskHint inputs
//    std::vector<sl::BaseStructure*> filtered_inputs;
//    for (uint32_t i = 0; i < numInputs; i++) {
//        if (inputs[i]->structType == sl::ResourceTag::s_structType) {
//            auto resourceTag = (sl::ResourceTag*)inputs[i];
//            if (resourceTag->type == kBufferTypeReactiveMaskHint) {
//                continue; // Skip this input
//            }
//        }
//        filtered_inputs.push_back(inputs[i]);
//    }
//
//    uint32_t filteredNumInputs = static_cast<uint32_t>(filtered_inputs.size());
#ifdef MOTION_VECTOR_REPROJECTION
    //TODO might process same resource twice per frame we to track last processed resource per frame

    if(supported_afr_feature(feature)) {
        /**
        * DLSS uses per-pixel motion vectors as a key component of its core algorithm. The motion vectors map a
        * pixel from the current frame to its position in the previous frame. That is, when the motion vector for
        * the pixel is added to the pixel's current location, the result is the location the pixel occupied in the
        * previous frame
        * MV = PAST - CURRENT
        * DLSS internally expect mvector to be in UV space notmalized to [-1, 1] it's not NDC space
        */
        instance->ReprojectMotionVectors(frame, inputs, numInputs, cmdBuffer);
    }
#endif

    s_evaluate_calls.fetch_add(1);
    const auto native_stereo = s_native_views.load();
    if (native_stereo && supported_afr_feature(feature)) {
        log_native_call("slEvaluateFeature", (uint32_t)frame, 0, s_native_view);
        s_native_evaluates[std::clamp(s_native_view + 1, 0, 2)].fetch_add(1);
    }
    if(((native_stereo && s_native_view == 1) || (!native_stereo && frame % 2 == 0 && instance->m_enabled->value())) && supported_afr_feature(feature)) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        std::vector<sl::BaseStructure*> afr_inputs{};
        afr_inputs.resize(numInputs);
        //TODO  viewport is always at index 0
        for (uint32_t i = 0; i < numInputs; i++) {
            if (inputs[i]->structType == sl::ViewportHandle::s_structType) {
                afr_inputs[i] = &afr_viewport_handle;
            } else {
                afr_inputs[i] = inputs[i];
            }
        }
        return original_fn(feature, frame, afr_inputs.data(), numInputs, cmdBuffer);
    }
    auto result = original_fn(feature, frame, inputs, numInputs, cmdBuffer);
    return result;
}

// TODO vr->get_current_render_eye() == VRRuntime::Eye::RIGHT in real is left eye texture
sl::Result UpscalerAfrNvidiaModule::on_slSetConstants(sl::Constants& values, const sl::FrameToken& frame, sl::ViewportHandle& viewport)
{
    static auto instance = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_set_constants_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slSetConstants)>();
    s_constants_calls.fetch_add(1);
#ifdef MOTION_VECTOR_REPROJECTION
    instance->m_motion_vector_reprojection.m_mvecScale.x = values.mvecScale.x;
    instance->m_motion_vector_reprojection.m_mvecScale.y = values.mvecScale.y;
#endif

#ifdef _DEBUG
    if(DebugUtils::config.debugShaders) {
        ShaderDebugOverlay::SetMvecScale(values.mvecScale.x, values.mvecScale.y);
    }
#endif


    static auto vr = VR::get();
    if (s_native_views.load()) {
        const int view = s_native_view >= 0 ? s_native_view : (s_native_view_of_constants ? s_native_view_of_constants(values) : -1);
        log_native_call("slSetConstants", (uint32_t)frame, (uint32_t)viewport, view, &values);
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        if (view == 1) {
            return original_fn(values, frame, afr_viewport_handle);
        }
        // The game sets one set of constants per frame; both views share the frame's jitter,
        // motion vector scale, depth range and reset, so the second viewport gets the same.
        original_fn(values, frame, afr_viewport_handle);
        return original_fn(values, frame, viewport);
    }
    if(frame % 2 == 0 && instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        return original_fn(values, frame, afr_viewport_handle);
    }
    auto result = original_fn(values, frame, viewport);
    return result;
}

//sl::Result UpscalerAfrNvidiaModule::on_slDVCSetOptions(const sl::ViewportHandle &viewport, const sl::DeepDVCOptions &options) {
//    static auto     instance         = UpscalerAfrNvidiaModule::Get();
//    static auto     original_fn      = instance->m_sl_dvc_set_options_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slDVCSetOptions)>();
//    {
//        sl::ViewportHandle afr_viewport_handle{instance->afr_viewport_id};
//        original_fn(afr_viewport_handle, options);
//    }
//    return original_fn(viewport, options);
//}

#ifdef STREAMLINE_DLSS_RR
sl::Result UpscalerAfrNvidiaModule::on_dlssrrSetOptions(const sl::ViewportHandle &viewport, const sl::DLSSDOptions &options) {
    static auto     instance         = UpscalerAfrNvidiaModule::Get();
    static auto     original_fn      = instance->m_dlssrr_set_options_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_dlssrrSetOptions)>();
    if(instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        original_fn(afr_viewport_handle, options);
    }
    return original_fn(viewport, options);
}
#endif

sl::Result UpscalerAfrNvidiaModule::on_dlssSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options)
{
    static auto     instance         = UpscalerAfrNvidiaModule::Get();
    static auto     original_fn      = instance->m_dlss_set_options_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_dlssSetOptions)>();
    if(instance->m_enabled->value() || s_native_views.load()) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        original_fn(afr_viewport_handle, options);
    }
    return original_fn(viewport, options);
}

sl::Result UpscalerAfrNvidiaModule::on_slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_free_resources_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slFreeResources)>();
    if(supported_afr_feature(feature) && (instance->m_enabled->value() || s_native_views.load())) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        original_fn(feature, afr_viewport_handle);
    }
    spdlog::info("slFreeResources called for feature {:x} viewport {:x}", (UINT)feature, (UINT)viewport);
    return original_fn(feature, viewport);
}

sl::Result UpscalerAfrNvidiaModule::on_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_allocate_resources_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slAllocateResources)>();
    if(supported_afr_feature(feature) && (instance->m_enabled->value() || s_native_views.load())) {
        sl::ViewportHandle afr_viewport_handle{instance->m_afr_viewport_id};
        original_fn(cmdBuffer, feature, afr_viewport_handle);
    }
    spdlog::info("slAllocateResources called for feature {:x} viewport {:x}", (UINT)feature, (UINT)viewport);
    return original_fn(cmdBuffer, feature, viewport);
}
#endif