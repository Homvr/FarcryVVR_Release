#ifdef STREAMLINE_LEGACY
#pragma once
#include <Mod.hpp>
#include <DescriptorHeap.h>
#include <d3d12.h>
#include <mods/vr/d3d12/ComPtr.hpp>
#include <mods/vr/d3d12/CommandContext.hpp>
#include <sl.h>
#include <sl_dlss.h>
#ifdef STREAMLINE_DLSS_RR
#include <sl_dlss_d.h>
#endif
//#include <sl_deepdvc.h>
#include <utility/PointerHook.hpp>
#include <memory/FunctionHook.h>
#include <nvidia/MotionVectorReprojection.h>

class LegacyUpscalerAfrNvidiaModule : public Mod
{
public:
    inline static std::shared_ptr<LegacyUpscalerAfrNvidiaModule>& Get()
    {
        static auto instance = std::make_shared<LegacyUpscalerAfrNvidiaModule>();
        return instance;
    }

    // Mod interface implementation
    std::string_view get_name() const override { return "Streamline1_DLSS_AER"; }
    
    std::optional<std::string> on_initialize() override;
    void on_draw_ui() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    void on_device_reset() override;
    void on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc) override;

    LegacyUpscalerAfrNvidiaModule() = default;
    ~LegacyUpscalerAfrNvidiaModule() override = default;

private:
    void InstallHooks();

    // Motion vector reprojection component
#ifdef MOTION_VECTOR_REPROJECTION
    MotionVectorReprojection m_motion_vector_reprojection{};
    void* lastDepthResource{nullptr};
    uint32_t Depthstate{};
    void* lastMvecResource{nullptr};
    uint32_t MvecState{};
#endif

    std::unique_ptr<FunctionHook> m_get_new_frame_token_hook{nullptr};
    std::unique_ptr<FunctionHook> m_set_tag_hook{nullptr};
    std::unique_ptr<FunctionHook> m_evaluate_feature_hook{nullptr};
    std::unique_ptr<FunctionHook> m_set_constants_hook{nullptr};
    std::unique_ptr<FunctionHook> m_allocate_resources_hook{nullptr};
    std::unique_ptr<FunctionHook> m_free_resources_hook{nullptr};

//    std::unique_ptr<PointerHook> m_dlss_set_options_hook{nullptr};
    std::unique_ptr<FunctionHook> m_set_feature_constants_hook{nullptr};
    std::unique_ptr<FunctionHook> m_dlssrr_set_options_hook{nullptr};
    std::unique_ptr<FunctionHook> m_sl_dvc_set_options_hook{nullptr};

    uint32_t m_afr_viewport_id{1024 + 1};

    void ReprojectMotionVectors(void* cmdBuffer);

//    static sl::Result on_slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex = nullptr);
    static bool on_slSetTag(const sl::Resource *resource, sl::BufferType tag, uint32_t id = 0, const sl::Extent* extent = nullptr);
    static bool on_slEvaluateFeature(sl::CommandBuffer* cmdBuffer, sl::Feature feature, uint32_t frameIndex, uint32_t id);
    static bool on_slSetConstants(sl::Constants& values, uint32_t frameIndex, uint32_t id = 0);
    static bool on_slSetFeatureConstants(sl::Feature feature, const void *consts, uint32_t frameIndex, uint32_t id = 0);
    static bool on_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, uint32_t id);
    static bool on_slFreeResources(sl::Feature feature, uint32_t id);

    // ModValue settings
    const ModToggle::Ptr m_enabled{ ModToggle::create(generate_name("Enabled"), true) };
    const ModToggle::Ptr m_motion_vector_fix{ ModToggle::create(generate_name("MotionVectorFix"), true) };
    
    ValueList m_options{
        *m_enabled,
#ifdef MOTION_VECTOR_REPROJECTION
        *m_motion_vector_fix
#endif
    };
};
#endif