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
//// Forward declarations for FSR2 types
//// We define minimal struct layouts to avoid including the full SDK
//// These match the FFX SDK structures for hooking purposes
//
//struct FfxFsr2Context;
//struct FfxFsr2ContextDescription;
//struct FfxFsr2DispatchDescription;
//
//// FSR2 error codes
//typedef int32_t FfxErrorCode;
//constexpr FfxErrorCode FFX_OK = 0;
//constexpr FfxErrorCode FFX_ERROR_INVALID_POINTER = 0x80000000;
//constexpr FfxErrorCode FFX_ERROR_INVALID_ARGUMENT = 0x80000001;
//
//// FSR2 function signatures
//typedef FfxErrorCode(*PFN_ffxFsr2ContextCreate)(FfxFsr2Context* context, const FfxFsr2ContextDescription* contextDescription);
//typedef FfxErrorCode(*PFN_ffxFsr2ContextDispatch)(FfxFsr2Context* context, const FfxFsr2DispatchDescription* dispatchDescription);
//typedef FfxErrorCode(*PFN_ffxFsr2ContextDestroy)(FfxFsr2Context* context);
//typedef size_t(*PFN_ffxFsr2GetScratchMemorySize)(void);
//
///**
// * @brief FSR 2 Upscaler Module for VR Alternate Eye Rendering (AER) support
// *
// * This module implements a "Pass-through + 1" strategy:
// * - Allows the game to manage the Primary Context (Left Eye)
// * - Secretly manages a Secondary Context (Right Eye)
// * - Prevents temporal history corruption in AER scenarios
// *
// * The module hooks FSR2 API functions (ffxFsr2ContextCreate, ffxFsr2ContextDispatch,
// * ffxFsr2ContextDestroy) to intercept calls and route them to the appropriate
// * context based on the current VR eye being rendered.
// */
//class UpscalerFsr2Module : public Mod {
//public:
//    inline static std::shared_ptr<UpscalerFsr2Module>& Get() {
//        static auto instance = std::make_shared<UpscalerFsr2Module>();
//        return instance;
//    }
//
//    // Mod interface implementation
//    std::string_view get_name() const override { return "AMD FSR2 Upscaler AFR"; }
//
//    std::optional<std::string> on_initialize() override;
//    void on_draw_ui() override;
//    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
//    void on_config_save(utility::Config& cfg) override;
//    void on_device_reset() override;
//
//    UpscalerFsr2Module() = default;
//    ~UpscalerFsr2Module() override;
//
//private:
//    // Hook installation
//    bool install_hooks();
//    void remove_hooks();
//
//    // Hook callbacks
//    static FfxErrorCode on_ffxFsr2ContextCreate(FfxFsr2Context* context, const FfxFsr2ContextDescription* contextDescription);
//    static FfxErrorCode on_ffxFsr2ContextDispatch(FfxFsr2Context* context, const FfxFsr2DispatchDescription* dispatchDescription);
//    static FfxErrorCode on_ffxFsr2ContextDestroy(FfxFsr2Context* context);
//
//    // State management
//    std::mutex m_mutex;
//
//    // Right eye context storage
//    // We store raw bytes to avoid needing the full FSR2 context structure definition
//    std::vector<uint8_t> m_context_right_storage;
//    FfxFsr2Context* m_context_right{nullptr};
//
//    // Scratch memory for the right eye context
//    std::vector<uint8_t> m_scratch_memory_right;
//
//    // Track the game's primary context pointer for validation
//    FfxFsr2Context* m_primary_context{nullptr};
//
//    // Module handle for FSR2 DLL
//    HMODULE m_fsr2_module{nullptr};
//
//    // Function pointers
//    PFN_ffxFsr2GetScratchMemorySize m_pfn_get_scratch_memory_size{nullptr};
//
//    // Hooks
//    std::unique_ptr<FunctionHook> m_create_hook;
//    std::unique_ptr<FunctionHook> m_dispatch_hook;
//    std::unique_ptr<FunctionHook> m_destroy_hook;
//
//    // Original function pointers (set by hooks)
//    inline static PFN_ffxFsr2ContextCreate s_original_create{nullptr};
//    inline static PFN_ffxFsr2ContextDispatch s_original_dispatch{nullptr};
//    inline static PFN_ffxFsr2ContextDestroy s_original_destroy{nullptr};
//
//    // Singleton instance for static callbacks
//    inline static UpscalerFsr2Module* s_instance{nullptr};
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
