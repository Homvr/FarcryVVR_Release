//#pragma once
//
//#include <Mod.hpp>
//#include <memory/FunctionHook.h>
//#include <mods/VR.hpp>
//
//#include <vector>
//#include <mutex>
//#include <d3d12.h>
//
//// Forward declarations for FSR3 types
//// We define minimal struct layouts to avoid including the full SDK
//// These match the FFX SDK structures for hooking purposes
//
//struct FfxFsr3Context;
//struct FfxFsr3ContextDescription;
//struct FfxFsr3DispatchUpscaleDescription;
//
//typedef int32_t FfxErrorCode;
//constexpr FfxErrorCode FFX_FSR3_OK = 0;
//
//// FSR3 function signatures
//typedef FfxErrorCode(*PFN_ffxFsr3ContextCreate)(FfxFsr3Context* context, const FfxFsr3ContextDescription* contextDescription);
//typedef FfxErrorCode(*PFN_ffxFsr3ContextDispatchUpscale)(FfxFsr3Context* context, const FfxFsr3DispatchUpscaleDescription* dispatchDescription);
//typedef FfxErrorCode(*PFN_ffxFsr3ContextDestroy)(FfxFsr3Context* context);
//typedef size_t(*PFN_ffxFsr3GetScratchMemorySize)(size_t maxContexts);
//
///**
// * @brief FSR 3 Upscaler Module for VR Alternate Eye Rendering (AER) support
// *
// * This module implements a "Pass-through + 1" strategy:
// * - Allows the game to manage the Primary Context (Left Eye)
// * - Secretly manages a Secondary Context (Right Eye)
// * - Prevents temporal history corruption in AER scenarios
// *
// * FSR3 includes frame generation capabilities in addition to upscaling.
// * This module focuses on the upscaling portion for VR compatibility.
// *
// * The module hooks FSR3 API functions (ffxFsr3ContextCreate, ffxFsr3ContextDispatch,
// * ffxFsr3ContextDestroy) to intercept calls and route them to the appropriate
// * context based on the current VR eye being rendered.
// */
//class UpscalerFsr3Module : public Mod {
//public:
//    inline static std::shared_ptr<UpscalerFsr3Module>& Get() {
//        static auto instance = std::make_shared<UpscalerFsr3Module>();
//        return instance;
//    }
//
//    // Mod interface implementation
//    std::string_view get_name() const override { return "AMD FSR3 Upscaler AFR"; }
//
//    std::optional<std::string> on_initialize() override;
//    void on_draw_ui() override;
//    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
//    void on_config_save(utility::Config& cfg) override;
//    void on_device_reset() override;
//
//    UpscalerFsr3Module() = default;
//    ~UpscalerFsr3Module() override;
//
//private:
//    // Hook installation
//    bool install_hooks();
//    void remove_hooks();
//
//    // Hook callbacks
//    static FfxErrorCode on_ffxFsr3ContextCreate(FfxFsr3Context* context, const FfxFsr3ContextDescription* contextDescription);
//    static FfxErrorCode on_ffxFsr3ContextDispatchUpscale(FfxFsr3Context* context, const FfxFsr3DispatchUpscaleDescription* dispatchDescription);
//    static FfxErrorCode on_ffxFsr3ContextDestroy(FfxFsr3Context* context);
//
////    std::mutex m_mutex;
//
//    std::vector<uint8_t> m_context_right_storage;
//    FfxFsr3Context* m_context_right{nullptr};
//
//    std::vector<uint8_t> m_scratch_memory_right;
//
//    FfxFsr3Context* m_primary_context{nullptr};
//
//    PFN_ffxFsr3GetScratchMemorySize m_pfn_get_scratch_memory_size{nullptr};
//
//    safetyhook::InlineHook m_create_hook;
//    safetyhook::InlineHook m_dispatch_hook;
//    safetyhook::InlineHook m_destroy_hook;
//
//    // ModValue settings
//    const ModToggle::Ptr m_enabled{ ModToggle::create(generate_name("Enabled"), true) };
//    const ModToggle::Ptr m_auto_detect{ ModToggle::create(generate_name("AutoDetect"), true) };
//
//    ValueList m_options{
//        *m_enabled,
//        *m_auto_detect
//    };
//};
