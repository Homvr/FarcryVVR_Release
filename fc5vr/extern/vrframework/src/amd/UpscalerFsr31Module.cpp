#include "UpscalerFsr31Module.h"

#include <experimental/DebugUtils.h>
#include <imgui.h>
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif
#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>
// FSR3.1 DLL name - can be overridden by game via macro before including this file
#ifndef FSR31_DLL_NAME
#define FSR31_DLL_NAME "amd_fidelityfx_dx12.dll" 
#endif

UpscalerFsr31Module::~UpscalerFsr31Module() {
    remove_hooks();
}

std::optional<std::string> UpscalerFsr31Module::on_initialize() {
    spdlog::info("[FSR3.1] Initializing AMD FSR 3.1 Upscaler AFR Module");
    if (!install_hooks()) {
        spdlog::warn("[FSR3.1] Failed to install hooks, module disabled");
        return std::nullopt;
    }
    spdlog::info("[FSR3.1] Module initialized successfully");
    return std::nullopt;
}

bool UpscalerFsr31Module::install_hooks() {
    // Try to find the FSR3.1 DLL
    auto m_fsr3_module = GetModuleHandleA(FSR31_DLL_NAME);
    if (m_fsr3_module != nullptr) {
        spdlog::info("[FSR3.1] Found FSR3.1 DLL: {}", FSR31_DLL_NAME);
    } else {
        spdlog::info("[FSR3.1] FSR3.1 DLL ({}) not loaded yet, will attempt hook installation later", FSR31_DLL_NAME);
        // Not an error - game may load FSR3 later or not at all
        return false;
    }

//    typedef ffxReturnCode_t(*PFN_ffxCreateContext)(ffxContext* context, const ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* allocators);
//    typedef ffxReturnCode_t(*PFN_ffxDispatch)(ffxContext* context, const ffxDispatchDescHeader* desc);
//    typedef ffxReturnCode_t(*PFN_ffxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* allocators);

    auto pfn_create = GetProcAddress(m_fsr3_module, "ffxCreateContext");
    auto pfn_dispatch = GetProcAddress(m_fsr3_module, "ffxDispatch");
    auto pfn_destroy = GetProcAddress(m_fsr3_module, "ffxDestroyContext");

    if (!pfn_create || !pfn_dispatch || !pfn_destroy) {
        spdlog::error("[FSR3.1] Failed to find FSR3.1 API functions");
        spdlog::error("[FSR3.1]   ffxCreateContext: {}", (void*)pfn_create);
        spdlog::error("[FSR3.1]   ffxDispatch: {}", (void*)pfn_dispatch);
        spdlog::error("[FSR3.1]   ffxDestroyContext: {}", (void*)pfn_destroy);
        return false;
    }

    spdlog::info("[FSR3.1] Found FSR3.1 API functions:");
    spdlog::info("[FSR3.1]   ffxCreateContext: {:p}", (void*)pfn_create);
    spdlog::info("[FSR3.1]   ffxDispatch: {:p}", (void*)pfn_dispatch);
    spdlog::info("[FSR3.1]   ffxDestroyContext: {:p}", (void*)pfn_destroy);

    // Install hooks
    m_create_hook = safetyhook::create_inline((uintptr_t)pfn_create, (uintptr_t)&on_ffxCreateContext);
    if (!m_create_hook) {
        spdlog::error("[FSR3.1] Failed to hook ffxCreateContext");
        return false;
    }

    m_dispatch_hook = safetyhook::create_inline((uintptr_t)pfn_dispatch, (uintptr_t)&on_ffxDispatch);
    if (!m_dispatch_hook) {
        spdlog::error("[FSR3.1] Failed to hook ffxDispatch");
        m_create_hook.reset();
        return false;
    }

    m_destroy_hook = safetyhook::create_inline((uintptr_t)pfn_destroy, (uintptr_t)&on_ffxDestroyContext);
    if (!m_destroy_hook) {
        spdlog::error("[FSR3.1] Failed to hook ffxDestroyContext");
        m_dispatch_hook.reset();
        m_create_hook.reset();
        return false;
    }

    spdlog::info("[FSR3.1] All hooks installed successfully");
    return true;
}

void UpscalerFsr31Module::remove_hooks() {
    m_create_hook.reset();
    m_dispatch_hook.reset();
    m_destroy_hook.reset();
    m_context_right = {nullptr};
    m_primary_context_ptr = nullptr;
}

ffxReturnCode_t UpscalerFsr31Module::on_ffxCreateContext(ffxContext* context, const ffxApiHeader* desc, const ffxAllocationCallbacks* allocators) {
    static auto instance = Get();
    
    // Call original first to create the primary context
    auto result = instance->m_create_hook.call<ffxReturnCode_t>(context, desc, allocators);
    
    // Check if this is an FSR Upscale context creation
    if (result == FFX_API_RETURN_OK && desc && desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
        spdlog::info("[FSR3.1] ffxCreateContext intercepted for FSR Upscale - creating dual contexts for AER");
        
        instance->m_primary_context_ptr = *context;
        
        // Create right eye context
        // We reuse the same description and allocators
        // The backend creation is handled internally by FSR 3.1 using the description
        auto result_right = instance->m_create_hook.call<ffxReturnCode_t>(&instance->m_context_right, desc, allocators);
        
        if (result_right != FFX_API_RETURN_OK) {
            spdlog::error("[FSR3.1] Failed to create right eye context: {}", result_right);
            instance->m_context_right = {nullptr};
        } else {
            spdlog::info("[FSR3.1] Successfully created right eye context for AER");
        }
    }
    
    return result;
}

ffxReturnCode_t UpscalerFsr31Module::on_ffxDispatch(ffxContext* context, const ffxApiHeader* desc) {
    static auto instance = Get();

    static auto vr = VR::get();
    // Check if this is the primary upscaling context and if we should intervene
    if (instance->m_enabled->value() && 
        instance->m_context_right != nullptr &&
        *context == instance->m_primary_context_ptr &&
        desc && desc->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE /*&& vr->is_hmd_active()*/) {

        instance->ReprojectMotionVectors((ffxDispatchDescUpscale*)desc);

        ffxContext* target_context;
        if (vr->m_render_frame_count % 2 == 0) {
            target_context = context;
        } else {
            target_context = &instance->m_context_right;
        }
        
        return instance->m_dispatch_hook.call<ffxReturnCode_t>(target_context, desc);
    }

    return instance->m_dispatch_hook.call<ffxReturnCode_t>(context, desc);
}


void UpscalerFsr31Module::ReprojectMotionVectors(ffxDispatchDescUpscale* upscalerDesc) {
    if(!m_motion_vector_reprojection.isInitialized() || !upscalerDesc) {
        return;
    }
    auto& mvTag = upscalerDesc->motionVectors;
    auto& depthTag = upscalerDesc->depth;
    static auto vr = VR::get();

    if(mvTag.resource && depthTag.resource && upscalerDesc->commandList) {
        auto mv_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto mv_native_resource = (ID3D12Resource*)mvTag.resource;
        auto depth_native_resource        = (ID3D12Resource*)depthTag.resource;
        auto depth_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto command_list    = (ID3D12GraphicsCommandList*)upscalerDesc->commandList;
#ifdef MOTION_VECTOR_REPROJECTION
        if(m_enabled->value() && m_motion_vector_fix->value()) {
            m_motion_vector_reprojection.m_mvecScale.x = upscalerDesc->motionVectorScale.x / (float)mvTag.description.width;
            m_motion_vector_reprojection.m_mvecScale.y = upscalerDesc->motionVectorScale.y / (float)mvTag.description.height;
            m_motion_vector_reprojection.ProcessMotionVectors(mv_native_resource, mv_state, depth_native_resource, depth_state, vr->m_render_frame_count, command_list);
        }
#endif
#ifdef _DEBUG
        static auto shader_debug_overlay = ShaderDebugOverlay::Get();
        if(DebugUtils::config.debugShaders && ShaderDebugOverlay::ValidateResource(mv_native_resource, shader_debug_overlay->m_motion_vector_buffer)) {
            ShaderDebugOverlay::SetMvecScale(upscalerDesc->motionVectorScale.x / (float)mvTag.description.width , upscalerDesc->motionVectorScale.y / (float)mvTag.description.height);
            ShaderDebugOverlay::CopyResource(command_list, mv_native_resource, shader_debug_overlay->m_motion_vector_buffer[vr->m_render_frame_count % 2].Get(), mv_state, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
#endif
    }
}

ffxReturnCode_t UpscalerFsr31Module::on_ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* allocators) {
    static auto instance = Get();
    
    if (*context == instance->m_primary_context_ptr) {
        spdlog::info("[FSR3.1] Destroying primary context, also destroying right eye context");
        if (instance->m_context_right != nullptr) {
            auto result_right = instance->m_destroy_hook.call<ffxReturnCode_t>(&instance->m_context_right, allocators);
            if (result_right != FFX_API_RETURN_OK) {
                spdlog::warn("[FSR3.1] Failed to destroy right eye context: {}", result_right);
            }
            instance->m_context_right = {nullptr};
        }
        instance->m_primary_context_ptr = nullptr;
    }
    
    return instance->m_destroy_hook.call<ffxReturnCode_t>(context, allocators);
}

void UpscalerFsr31Module::on_draw_ui() {
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }
    
    ImGui::TreePush("FSR31_AFR");
    
    m_enabled->draw("Enabled");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enable dual FSR 3.1 context management for VR Alternate Eye Rendering (AER).\n"
                         "This prevents temporal ghosting artifacts in VR.");
    }


    m_motion_vector_fix->draw("Motion Vector Reprojection Fix");
//    m_auto_detect->draw("Auto-detect FSR 3.1");
//    ImGui::SameLine();
//    ImGui::TextDisabled("(?)");
//    if (ImGui::IsItemHovered()) {
//        ImGui::SetTooltip("Automatically detect and hook FSR 3.1 when game loads the DLL.");
//    }
    ImGui::TreePop();
}

void UpscalerFsr31Module::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (auto& opt : m_options) {
        opt.get().config_load(cfg, set_defaults);
    }
}

void UpscalerFsr31Module::on_config_save(utility::Config& cfg) {
    for (auto& opt : m_options) {
        opt.get().config_save(cfg);
    }
}

void UpscalerFsr31Module::on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc)
{
    m_motion_vector_reprojection.on_d3d12_initialize(pDevice4, desc);
}

void UpscalerFsr31Module::on_device_reset() {
    spdlog::info("[FSR3.1] Device reset - cleaning up contexts");
    m_context_right = {nullptr};
    m_primary_context_ptr = {nullptr};
    m_motion_vector_reprojection.on_device_reset();
}
