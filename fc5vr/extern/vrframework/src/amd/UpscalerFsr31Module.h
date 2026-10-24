#pragma once

#include <Mod.hpp>
#include <memory/FunctionHook.h>
#include <mods/VR.hpp>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <d3d12.h>
#include <mutex>
#include <nvidia/MotionVectorReprojection.h>
#include <vector>


/**
 * @brief FSR 3.1 Upscaler Module for VR Alternate Eye Rendering (AER) support
 * 
 * This module implements a "Pass-through + 1" strategy for FSR 3.1:
 * - Allows the game to manage the Primary Context (Left Eye)
 * - Secretly manages a Secondary Context (Right Eye)
 * - Prevents temporal history corruption in AER scenarios
 * 
 * FSR 3.1 separates Upscaling and Frame Generation into distinct contexts.
 * This module specifically targets the Upscaling context.
 */
class UpscalerFsr31Module : public Mod {
public:
    inline static std::shared_ptr<UpscalerFsr31Module>& Get() {
        static auto instance = std::make_shared<UpscalerFsr31Module>();
        return instance;
    }

    std::string_view get_name() const override { return "FSR31_AER"; }
    
    std::optional<std::string> on_initialize() override;
    void on_draw_ui() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    void on_device_reset() override;
    void on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc) override;

    UpscalerFsr31Module() = default;
    ~UpscalerFsr31Module() override;

private:
    bool install_hooks();
    void remove_hooks();

    static ffxReturnCode_t on_ffxCreateContext(ffxContext* context, const struct ffxApiHeader* desc, const struct ffxAllocationCallbacks* allocators);
    static ffxReturnCode_t on_ffxDispatch(ffxContext* context, const struct ffxApiHeader* desc);
    static ffxReturnCode_t on_ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* allocators);

    ffxContext m_context_right{nullptr};
    
    ffxContext m_primary_context_ptr{nullptr};

    MotionVectorReprojection m_motion_vector_reprojection{};


    safetyhook::InlineHook m_create_hook;
    safetyhook::InlineHook m_dispatch_hook;
    safetyhook::InlineHook m_destroy_hook;

    const ModToggle::Ptr m_enabled{ ModToggle::create(generate_name("Enabled"), true) };
    const ModToggle::Ptr m_motion_vector_fix{ ModToggle::create(generate_name("MotionVectorFix"), true) };

    ValueList m_options{
        *m_enabled,
        *m_motion_vector_fix
    };
    void      ReprojectMotionVectors(struct ffxDispatchDescUpscale* upscalerDesc);
};
