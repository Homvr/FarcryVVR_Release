//#include "UpscalerFsr2Module.h"
//
//#include <spdlog/spdlog.h>
//#include <imgui.h>
//
//// FSR2 DLL name - can be overridden by game via macro before including this file
//#ifndef FSR2_DLL_NAME
//#define FSR2_DLL_NAME "ffx_fsr2_api_dx12_x64.dll"
//#endif
//
//// FSR2 Context size estimate (conservative upper bound)
//// The actual size varies by backend but 64KB should be more than enough
//constexpr size_t FSR2_CONTEXT_SIZE = 65536;
//
//UpscalerFsr2Module::~UpscalerFsr2Module() {
//    remove_hooks();
//    s_instance = nullptr;
//}
//
//std::optional<std::string> UpscalerFsr2Module::on_initialize() {
//    spdlog::info("[FSR2] Initializing AMD FSR2 Upscaler AFR Module");
//
//    s_instance = this;
//
//    // Try to find the FSR2 DLL
//    m_fsr2_module = GetModuleHandleA(FSR2_DLL_NAME);
//    if (m_fsr2_module != nullptr) {
//        spdlog::info("[FSR2] Found FSR2 DLL: {}", FSR2_DLL_NAME);
//    } else {
//        spdlog::info("[FSR2] FSR2 DLL ({}) not loaded yet, will attempt hook installation later", FSR2_DLL_NAME);
//        // Not an error - game may load FSR2 later or not at all
//        return std::nullopt;
//    }
//
//    if (!install_hooks()) {
//        spdlog::warn("[FSR2] Failed to install hooks, module disabled");
//        return std::nullopt;
//    }
//
//    spdlog::info("[FSR2] Module initialized successfully");
//    return std::nullopt;
//}
//
//bool UpscalerFsr2Module::install_hooks() {
//    if (m_fsr2_module == nullptr) {
//        // Try to find the module
//        m_fsr2_module = GetModuleHandleA(FSR2_DLL_NAME);
//        if (m_fsr2_module != nullptr) {
//            spdlog::info("[FSR2] Found FSR2 DLL during hook installation: {}", FSR2_DLL_NAME);
//        } else {
//            spdlog::debug("[FSR2] FSR2 DLL ({}) still not loaded", FSR2_DLL_NAME);
//            return false;
//        }
//    }
//
//    // Get function addresses
//    auto pfn_create = (PFN_ffxFsr2ContextCreate)GetProcAddress(m_fsr2_module, "ffxFsr2ContextCreate");
//    auto pfn_dispatch = (PFN_ffxFsr2ContextDispatch)GetProcAddress(m_fsr2_module, "ffxFsr2ContextDispatch");
//    auto pfn_destroy = (PFN_ffxFsr2ContextDestroy)GetProcAddress(m_fsr2_module, "ffxFsr2ContextDestroy");
//    m_pfn_get_scratch_memory_size = (PFN_ffxFsr2GetScratchMemorySize)GetProcAddress(m_fsr2_module, "ffxFsr2GetScratchMemorySize");
//
//    if (pfn_create == nullptr || pfn_dispatch == nullptr || pfn_destroy == nullptr) {
//        spdlog::error("[FSR2] Failed to find FSR2 API functions");
//        spdlog::error("[FSR2]   ffxFsr2ContextCreate: {}", (void*)pfn_create);
//        spdlog::error("[FSR2]   ffxFsr2ContextDispatch: {}", (void*)pfn_dispatch);
//        spdlog::error("[FSR2]   ffxFsr2ContextDestroy: {}", (void*)pfn_destroy);
//        return false;
//    }
//
//    spdlog::info("[FSR2] Found FSR2 API functions:");
//    spdlog::info("[FSR2]   ffxFsr2ContextCreate: {:p}", (void*)pfn_create);
//    spdlog::info("[FSR2]   ffxFsr2ContextDispatch: {:p}", (void*)pfn_dispatch);
//    spdlog::info("[FSR2]   ffxFsr2ContextDestroy: {:p}", (void*)pfn_destroy);
//    spdlog::info("[FSR2]   ffxFsr2GetScratchMemorySize: {:p}", (void*)m_pfn_get_scratch_memory_size);
//
//    // Install hooks
//    m_create_hook = std::make_unique<FunctionHook>((uintptr_t)pfn_create, (uintptr_t)&on_ffxFsr2ContextCreate);
//    if (!m_create_hook->create()) {
//        spdlog::error("[FSR2] Failed to hook ffxFsr2ContextCreate");
//        return false;
//    }
//
//    m_dispatch_hook = std::make_unique<FunctionHook>((uintptr_t)pfn_dispatch, (uintptr_t)&on_ffxFsr2ContextDispatch);
//    if (!m_dispatch_hook->create()) {
//        spdlog::error("[FSR2] Failed to hook ffxFsr2ContextDispatch");
//        m_create_hook.reset();
//        return false;
//    }
//
//    m_destroy_hook = std::make_unique<FunctionHook>((uintptr_t)pfn_destroy, (uintptr_t)&on_ffxFsr2ContextDestroy);
//    if (!m_destroy_hook->create()) {
//        spdlog::error("[FSR2] Failed to hook ffxFsr2ContextDestroy");
//        m_dispatch_hook.reset();
//        m_create_hook.reset();
//        return false;
//    }
//
//    spdlog::info("[FSR2] All hooks installed successfully");
//    return true;
//}
//
//void UpscalerFsr2Module::remove_hooks() {
//    std::lock_guard<std::mutex> lock(m_mutex);
//
//    m_destroy_hook.reset();
//    m_dispatch_hook.reset();
//    m_create_hook.reset();
//
//    s_original_create = nullptr;
//    s_original_dispatch = nullptr;
//    s_original_destroy = nullptr;
//
//    // Clean up right eye context if it exists
//    if (m_context_right != nullptr && s_original_destroy != nullptr) {
//        // Note: We can't destroy here because hooks are being removed
//        // The context will be leaked, but this only happens on shutdown
//    }
//
//    m_context_right = nullptr;
//    m_context_right_storage.clear();
//    m_scratch_memory_right.clear();
//    m_primary_context = nullptr;
//}
//
//FfxErrorCode UpscalerFsr2Module::on_ffxFsr2ContextCreate(FfxFsr2Context* context, const FfxFsr2ContextDescription* contextDescription) {
//    if (s_instance == nullptr || s_original_create == nullptr) {
//        spdlog::error("[FSR2] on_ffxFsr2ContextCreate called but instance or original function is null");
//        return FFX_ERROR_INVALID_POINTER;
//    }
//
//    auto& inst = *s_instance;
//    std::lock_guard<std::mutex> lock(inst.m_mutex);
//
//    if (!inst.m_enabled->value()) {
//        spdlog::debug("[FSR2] Module disabled, passing through to original");
//        return s_original_create(context, contextDescription);
//    }
//
//    spdlog::info("[FSR2] ffxFsr2ContextCreate intercepted - creating dual contexts for AER");
//
//    // Call original to create the game's/left eye context
//    FfxErrorCode result = s_original_create(context, contextDescription);
//
//    if (result != FFX_OK) {
//        spdlog::error("[FSR2] Original ffxFsr2ContextCreate failed with error: {}", result);
//        return result;
//    }
//
//    // Store primary context pointer for validation in dispatch
//    inst.m_primary_context = context;
//
//    // Determine scratch memory size
//    size_t scratch_size = FSR2_CONTEXT_SIZE; // Default
//    if (inst.m_pfn_get_scratch_memory_size != nullptr) {
//        scratch_size = inst.m_pfn_get_scratch_memory_size();
//        spdlog::info("[FSR2] Got scratch memory size from API: {} bytes", scratch_size);
//    }
//
//    // Allocate storage for right eye context
//    inst.m_context_right_storage.resize(FSR2_CONTEXT_SIZE);
//    inst.m_context_right = reinterpret_cast<FfxFsr2Context*>(inst.m_context_right_storage.data());
//    memset(inst.m_context_right, 0, FSR2_CONTEXT_SIZE);
//
//    // Allocate scratch memory for right eye
//    inst.m_scratch_memory_right.resize(scratch_size);
//    memset(inst.m_scratch_memory_right.data(), 0, scratch_size);
//
//    // Create a copy of the context description for the right eye
//    // We need to modify the scratch buffer pointer
//    // Note: This is a simplified approach - the actual FfxFsr2ContextDescription structure
//    // is complex and game/SDK version dependent. A more robust implementation would
//    // need to properly handle the backend interface and callbacks.
//
//    // For now, we create the right eye context using the same description
//    // The key insight is that both contexts share the same backendInterface (device pointers)
//    // but have separate scratch memory and internal state
//
//    spdlog::info("[FSR2] Creating right eye context for AER support");
//
//    // Call original again to create right eye context
//    // Note: In a production implementation, we would need to carefully copy and modify
//    // the context description to use our scratch memory. This simplified version
//    // relies on the backend interface being shared correctly.
//    FfxErrorCode result_right = s_original_create(inst.m_context_right, contextDescription);
//
//    if (result_right != FFX_OK) {
//        spdlog::error("[FSR2] Failed to create right eye context: {}", result_right);
//        // Clean up and continue with single context (fallback to non-AER mode)
//        inst.m_context_right_storage.clear();
//        inst.m_context_right = nullptr;
//        inst.m_scratch_memory_right.clear();
//        // Don't return error - game can still work with single context
//    } else {
//        spdlog::info("[FSR2] Successfully created dual FSR2 contexts for AER");
//    }
//
//    return result;
//}
//
//FfxErrorCode UpscalerFsr2Module::on_ffxFsr2ContextDispatch(FfxFsr2Context* context, const FfxFsr2DispatchDescription* dispatchDescription) {
//    if (s_instance == nullptr || s_original_dispatch == nullptr) {
//        spdlog::error("[FSR2] on_ffxFsr2ContextDispatch called but instance or original function is null");
//        return FFX_ERROR_INVALID_POINTER;
//    }
//
//    auto& inst = *s_instance;
//
//    // Fast path: if module disabled or no right eye context, pass through
//    if (!inst.m_enabled->value() || inst.m_context_right == nullptr) {
//        return s_original_dispatch(context, dispatchDescription);
//    }
//
//    // Check if this is the context we're tracking
//    if (context != inst.m_primary_context) {
//        // Game might be using multiple upscaler instances - log warning and pass through
////        spdlog::warn_once("[FSR2] Dispatch called with unknown context {:p} (expected {:p}), passing through",
////            (void*)context, (void*)inst.m_primary_context);
//        return s_original_dispatch(context, dispatchDescription);
//    }
//
//    // Determine current eye from VR system
//    auto vr = VR::get();
//    if (vr == nullptr || !vr->is_hmd_active()) {
//        // VR not active, pass through to original
//        return s_original_dispatch(context, dispatchDescription);
//    }
//
//    VRRuntime::Eye current_eye = vr->get_current_render_eye();
//
//    FfxFsr2Context* target_context;
//    if (current_eye == VRRuntime::Eye::LEFT) {
//        // Use game's original context for left eye
//        target_context = context;
//        spdlog::debug("[FSR2] Dispatch: Using LEFT eye context (primary)");
//    } else {
//        // Use our secondary context for right eye
//        target_context = inst.m_context_right;
//        spdlog::debug("[FSR2] Dispatch: Using RIGHT eye context (secondary)");
//    }
//
//    return s_original_dispatch(target_context, dispatchDescription);
//}
//
//FfxErrorCode UpscalerFsr2Module::on_ffxFsr2ContextDestroy(FfxFsr2Context* context) {
//    if (s_instance == nullptr || s_original_destroy == nullptr) {
//        spdlog::error("[FSR2] on_ffxFsr2ContextDestroy called but instance or original function is null");
//        return FFX_ERROR_INVALID_POINTER;
//    }
//
//    auto& inst = *s_instance;
//    std::lock_guard<std::mutex> lock(inst.m_mutex);
//
//    spdlog::info("[FSR2] ffxFsr2ContextDestroy intercepted");
//
//    // Destroy our right eye context first (if it exists)
//    if (inst.m_context_right != nullptr) {
//        spdlog::info("[FSR2] Destroying right eye context");
//        FfxErrorCode result_right = s_original_destroy(inst.m_context_right);
//        if (result_right != FFX_OK) {
//            spdlog::warn("[FSR2] Failed to destroy right eye context: {}", result_right);
//        }
//        inst.m_context_right = nullptr;
//        inst.m_context_right_storage.clear();
//        inst.m_scratch_memory_right.clear();
//    }
//
//    // Clear primary context tracking
//    if (context == inst.m_primary_context) {
//        inst.m_primary_context = nullptr;
//    }
//
//    // Destroy the game's context
//    return s_original_destroy(context);
//}
//
//void UpscalerFsr2Module::on_draw_ui() {
//    if (!ImGui::CollapsingHeader(get_name().data())) {
//        return;
//    }
//
//    ImGui::TreePush("FSR2_AFR");
//
//    m_enabled->draw("Enabled");
//    ImGui::SameLine();
//    ImGui::TextDisabled("(?)");
//    if (ImGui::IsItemHovered()) {
//        ImGui::SetTooltip("Enable dual FSR2 context management for VR Alternate Eye Rendering (AER).\n"
//                         "This prevents temporal ghosting artifacts in VR.");
//    }
//
//    m_auto_detect->draw("Auto-detect FSR2");
//    ImGui::SameLine();
//    ImGui::TextDisabled("(?)");
//    if (ImGui::IsItemHovered()) {
//        ImGui::SetTooltip("Automatically detect and hook FSR2 when game loads the DLL.");
//    }
//
//    ImGui::Separator();
//    ImGui::Text("Status:");
//
//    if (m_fsr2_module != nullptr) {
//        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "FSR2 DLL: Loaded");
//    } else {
//        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "FSR2 DLL: Not detected");
//    }
//
//    if (m_create_hook && m_create_hook->is_valid()) {
//        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Hooks: Installed");
//    } else {
//        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Hooks: Not installed");
//    }
//
//    if (m_primary_context != nullptr) {
//        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Primary Context: Active");
//    } else {
//        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Primary Context: None");
//    }
//
//    if (m_context_right != nullptr) {
//        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Right Eye Context: Active");
//    } else {
//        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Right Eye Context: None");
//    }
//
//    // Manual hook installation button
//    if (m_fsr2_module == nullptr || !m_create_hook || !m_create_hook->is_valid()) {
//        if (ImGui::Button("Attempt Hook Installation")) {
//            if (install_hooks()) {
//                spdlog::info("[FSR2] Manual hook installation successful");
//            } else {
//                spdlog::warn("[FSR2] Manual hook installation failed");
//            }
//        }
//    }
//
//    ImGui::TreePop();
//}
//
//void UpscalerFsr2Module::on_config_load(const utility::Config& cfg, bool set_defaults) {
//    for (auto& opt : m_options) {
//        opt.get().config_load(cfg, set_defaults);
//    }
//}
//
//void UpscalerFsr2Module::on_config_save(utility::Config& cfg) {
//    for (auto& opt : m_options) {
//        opt.get().config_save(cfg);
//    }
//}
//
//void UpscalerFsr2Module::on_device_reset() {
//    std::lock_guard<std::mutex> lock(m_mutex);
//
//    spdlog::info("[FSR2] Device reset - cleaning up contexts");
//
//    // On device reset, FSR2 contexts become invalid
//    // We don't destroy them here because the DLL might do that itself
//    // Just clear our tracking
//    m_context_right = nullptr;
//    m_context_right_storage.clear();
//    m_scratch_memory_right.clear();
//    m_primary_context = nullptr;
//}
