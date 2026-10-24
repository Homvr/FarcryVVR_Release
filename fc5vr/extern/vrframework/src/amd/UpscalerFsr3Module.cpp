//#include "UpscalerFsr3Module.h"
//
//#include <imgui.h>
//#include <safetyhook/easy.hpp>
//#include <spdlog/spdlog.h>
//
//// FSR3 DLL name - can be overridden by game via macro before including this file
//#ifndef FSR3_DLL_NAME
//#define FSR3_DLL_NAME "amd_fidelityfx_dx12.dll"
//#endif
//
//// FSR3 Context size estimate (conservative upper bound)
//// FSR3 contexts are larger than FSR2 due to frame generation support
//constexpr size_t FSR3_CONTEXT_SIZE = 131072;  // 128KB
//
//UpscalerFsr3Module::~UpscalerFsr3Module() {
//    remove_hooks();
//}
//
//std::optional<std::string> UpscalerFsr3Module::on_initialize() {
//    spdlog::info("[FSR3] Initializing AMD FSR3 Upscaler AFR Module");
//    if (!install_hooks()) {
//        spdlog::warn("[FSR3] Failed to install hooks, module disabled");
//        return std::nullopt;
//    }
//
//    spdlog::info("[FSR3] Module initialized successfully");
//    return std::nullopt;
//}
//
//bool UpscalerFsr3Module::install_hooks() {
//    // Try to find the FSR3 DLL
//    auto m_fsr3_module = GetModuleHandleA(FSR3_DLL_NAME);
//    if (m_fsr3_module != nullptr) {
//        spdlog::info("[FSR3] Found FSR3 DLL: {}", FSR3_DLL_NAME);
//    } else {
//        spdlog::info("[FSR3] FSR3 DLL ({}) not loaded yet, will attempt hook installation later", FSR3_DLL_NAME);
//        // Not an error - game may load FSR3 later or not at all
//        return false;
//    }
//
//
//    auto pfn_create = (PFN_ffxFsr3ContextCreate)GetProcAddress(m_fsr3_module, "ffxFsr3ContextCreate");
//
//    auto pfn_dispatch = (PFN_ffxFsr3ContextDispatchUpscale)GetProcAddress(m_fsr3_module, "ffxFsr3ContextDispatchUpscale");
//
//    auto pfn_destroy = (PFN_ffxFsr3ContextDestroy)GetProcAddress(m_fsr3_module, "ffxFsr3ContextDestroy");
//
//    m_pfn_get_scratch_memory_size = (PFN_ffxFsr3GetScratchMemorySize)GetProcAddress(m_fsr3_module, "ffxFsr3GetScratchMemorySize");
//
//    if (pfn_create == nullptr || pfn_dispatch == nullptr || pfn_destroy == nullptr) {
//        spdlog::error("[FSR3] Failed to find FSR3 API functions");
//        spdlog::error("[FSR3]   ffxFsr3ContextCreate: {}", (void*)pfn_create);
//        spdlog::error("[FSR3]   ffxFsr3ContextDispatchUpscale: {}", (void*)pfn_dispatch);
//        spdlog::error("[FSR3]   ffxFsr3ContextDestroy: {}", (void*)pfn_destroy);
//        return false;
//    }
//
//    spdlog::info("[FSR3] Found FSR3 API functions:");
//    spdlog::info("[FSR3]   ffxFsr3ContextCreate: {:p}", (void*)pfn_create);
//    spdlog::info("[FSR3]   ffxFsr3ContextDispatchUpscale: {:p}", (void*)pfn_dispatch);
//    spdlog::info("[FSR3]   ffxFsr3ContextDestroy: {:p}", (void*)pfn_destroy);
//    spdlog::info("[FSR3]   ffxFsr3GetScratchMemorySize: {:p}", (void*)m_pfn_get_scratch_memory_size);
//
//    // Install hooks
//    m_create_hook = safetyhook::create_inline((uintptr_t)pfn_create, (uintptr_t)&on_ffxFsr3ContextCreate);
//    if (!m_create_hook) {
//        spdlog::error("[FSR3] Failed to hook ffxFsr3ContextCreate");
//        return false;
//    }
//
//
//    m_dispatch_hook = safetyhook::create_inline((uintptr_t)pfn_dispatch, (uintptr_t)&on_ffxFsr3ContextDispatchUpscale);
//    if (!m_dispatch_hook) {
//        spdlog::error("[FSR3] Failed to hook ffxFsr3ContextDispatchUpscale");
//        m_create_hook.reset();
//        return false;
//    }
//
//
//    m_destroy_hook = safetyhook::create_inline((uintptr_t)pfn_destroy, (uintptr_t)&on_ffxFsr3ContextDestroy);
//    if (!m_destroy_hook) {
//        spdlog::error("[FSR3] Failed to hook ffxFsr3ContextDestroy");
//        m_dispatch_hook.reset();
//        m_create_hook.reset();
//        return false;
//    }
//
//
//    spdlog::info("[FSR3] All hooks installed successfully");
//    return true;
//}
//
//void UpscalerFsr3Module::remove_hooks() {
////    std::lock_guard<std::mutex> lock(m_mutex);
//
//    m_destroy_hook.reset();
//    m_dispatch_hook.reset();
//    m_create_hook.reset();
//
//    m_context_right = nullptr;
//    m_context_right_storage.clear();
//    m_scratch_memory_right.clear();
//    m_primary_context = nullptr;
//}
//
//FfxErrorCode UpscalerFsr3Module::on_ffxFsr3ContextCreate(FfxFsr3Context* context, const FfxFsr3ContextDescription* contextDescription) {
//    static auto instance = Get();
//
////    std::lock_guard<std::mutex> lock(instance->m_mutex);
//
//    spdlog::info("[FSR3] ffxFsr3ContextCreate intercepted - creating dual contexts for AER");
//
//    auto result = instance->m_create_hook.call<FfxErrorCode>(context, contextDescription);
//
//    if (result != FFX_FSR3_OK) {
//        spdlog::error("[FSR3] Original ffxFsr3ContextCreate failed with error: {}", result);
//        return result;
//    }
//
//    instance->m_primary_context = context;
//
//    size_t scratch_size = FSR3_CONTEXT_SIZE; // Default
//    if (instance->m_pfn_get_scratch_memory_size != nullptr) {
//        scratch_size = instance->m_pfn_get_scratch_memory_size(1);
//        spdlog::info("[FSR3] Got scratch memory size from API: {} bytes", scratch_size);
//    }
//
//    instance->m_context_right_storage.resize(FSR3_CONTEXT_SIZE);
//    instance->m_context_right = reinterpret_cast<FfxFsr3Context*>(instance->m_context_right_storage.data());
//    memset(instance->m_context_right, 0, FSR3_CONTEXT_SIZE);
//
//    instance->m_scratch_memory_right.resize(scratch_size);
//    memset(instance->m_scratch_memory_right.data(), 0, scratch_size);
//
//    spdlog::info("[FSR3] Creating right eye context for AER support");
//
//    auto result_right = instance->m_create_hook.call<FfxErrorCode>(instance->m_context_right, contextDescription);
//
//    if (result_right != FFX_FSR3_OK) {
//        spdlog::error("[FSR3] Failed to create right eye context: {}", result_right);
//        instance->m_context_right_storage.clear();
//        instance->m_context_right = nullptr;
//        instance->m_scratch_memory_right.clear();
//    } else {
//        spdlog::info("[FSR3] Successfully created dual FSR3 contexts for AER");
//    }
//
//    return result;
//}
//
//FfxErrorCode UpscalerFsr3Module::on_ffxFsr3ContextDispatchUpscale(FfxFsr3Context* context, const FfxFsr3DispatchUpscaleDescription* dispatchDescription) {
//
//    static auto instance = Get();
//    static auto vr = VR::get();
//
//    if (!instance->m_enabled->value() || instance->m_context_right == nullptr || context != instance->m_primary_context || !vr->is_hmd_active()) {
//        return instance->m_dispatch_hook.call<FfxErrorCode>(context, dispatchDescription);
//    }
//
//
//    FfxFsr3Context* target_context;
//    if (vr->m_render_frame_count % 2 == 0) {
//        target_context = context;
//    } else {
//        target_context = instance->m_context_right;
//    }
//
//    return instance->m_dispatch_hook.call<FfxErrorCode>(target_context, dispatchDescription);
//}
//
//FfxErrorCode UpscalerFsr3Module::on_ffxFsr3ContextDestroy(FfxFsr3Context* context) {
//    static auto instance = Get();
//    spdlog::info("[FSR3] ffxFsr3ContextDestroy intercepted");
//
//    // Destroy our right eye context first (if it exists)
//    if (instance->m_context_right != nullptr) {
//        spdlog::info("[FSR3] Destroying right eye context");
//        auto result_right = instance->m_destroy_hook.call<FfxErrorCode>(instance->m_context_right);
//        if (result_right != FFX_FSR3_OK) {
//            spdlog::warn("[FSR3] Failed to destroy right eye context: {}", result_right);
//        }
//        instance->m_context_right = nullptr;
//        instance->m_context_right_storage.clear();
//        instance->m_scratch_memory_right.clear();
//    }
//
//    // Clear primary context tracking
//    if (context == instance->m_primary_context) {
//        instance->m_primary_context = nullptr;
//    }
//
//    // Destroy the game's context
//    return instance->m_destroy_hook.call<FfxErrorCode>(context);
//}
//
//void UpscalerFsr3Module::on_draw_ui() {
//    if (!ImGui::CollapsingHeader(get_name().data())) {
//        return;
//    }
//
//    ImGui::TreePush("FSR3_AFR");
//
//    m_enabled->draw("Enabled");
//    ImGui::SameLine();
//    ImGui::TextDisabled("(?)");
//    if (ImGui::IsItemHovered()) {
//        ImGui::SetTooltip("Enable dual FSR3 context management for VR Alternate Eye Rendering (AER).\n"
//                         "This prevents temporal ghosting artifacts in VR.");
//    }
//
//    m_auto_detect->draw("Auto-detect FSR3");
//    ImGui::SameLine();
//    ImGui::TextDisabled("(?)");
//    if (ImGui::IsItemHovered()) {
//        ImGui::SetTooltip("Automatically detect and hook FSR3 when game loads the DLL.");
//    }
//    ImGui::TreePop();
//}
//
//void UpscalerFsr3Module::on_config_load(const utility::Config& cfg, bool set_defaults) {
//    for (auto& opt : m_options) {
//        opt.get().config_load(cfg, set_defaults);
//    }
//}
//
//void UpscalerFsr3Module::on_config_save(utility::Config& cfg) {
//    for (auto& opt : m_options) {
//        opt.get().config_save(cfg);
//    }
//}
//
//void UpscalerFsr3Module::on_device_reset() {
////    std::lock_guard<std::mutex> lock(m_mutex);
//
//    spdlog::info("[FSR3] Device reset - cleaning up contexts");
//    m_context_right = nullptr;
//    m_context_right_storage.clear();
//    m_scratch_memory_right.clear();
//    m_primary_context = nullptr;
//}
