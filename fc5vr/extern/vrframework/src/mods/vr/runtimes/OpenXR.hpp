#pragma once

#include <atomic>
#include <chrono>
#include <unordered_set>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
//#include <common/xr_linear.h>

#include "VRRuntime.hpp"

namespace runtimes{
struct OpenXR final : public VRRuntime {
    inline static double s_fc5_wait_ms{0.0}, s_fc5_end_ms{0.0}, s_fc5_copy_ms{0.0};   // Far Cry 5 submit timing (pacing log)
    // Far Cry 5 hitchlog: the same times as running totals in microseconds (never reset), read before and after each game call.
    inline static std::atomic<int64_t> s_fc5_wait_us{0}, s_fc5_begin_us{0}, s_fc5_end_us{0}, s_fc5_copy_us{0};
    // Far Cry 5 framelock (29 Oct, replaces xrhalf): every picture is shown for the same number of refreshes. -1 auto, 0 off,
    // 1 or 2 fixed. With two, xrWaitFrame is entered two display periods after the previous one returned, less 2 ms.
    inline static std::atomic<int> s_fc5_frame_lock{0};   // off by default (30 Oct)
    inline static std::atomic<int> s_fc5_lock_n{1};             // refreshes per picture in use
    inline static std::atomic<int> s_fc5_lock_over_pct{0};      // share of the last 2 s of pictures that took over a refresh (%)
    inline static std::atomic<int> s_fc5_lock_pairs{-1};        // the mod's rendering: 1 Stereo pairs (two game calls a frame), 0 one call (AFW), -1 not said
    inline static std::atomic<uint64_t> s_fc5_half_waits{0};
    inline static double s_fc5_half_ms{0.0};
    // posetime (FC5 log): each update_poses locates the views and the hands together; its count and display time, to tell
    // which pose sample the eyes and the controller of a frame came from.
    inline static std::atomic<uint64_t> s_fc5_pose_seq{0};
    inline static std::atomic<int64_t>  s_fc5_pose_time{0};
    // headsetrecenter (FC5): the runtime's own recenter (Meta button held, Virtual Desktop's recenter) moves its LOCAL space;
    // how many such changes were announced and the time the last one takes effect, for the mod's recentre to follow it.
    inline static std::atomic<uint32_t> s_fc5_local_space_changes{0};
    inline static std::atomic<int64_t>  s_fc5_local_space_change_time{0};
    inline static std::chrono::steady_clock::time_point s_fc5_last_wait_return{};

    enum class SwapchainIndex: uint8_t {
        AFR_LEFT_EYE = 0,
        AFR_RIGHT_EYE = 1,
        AFR_DEPTH_LEFT_EYE = 2,
        AFR_DEPTH_RIGHT_EYE = 3,
        FRAMEWORK_UI = 4,
        GAME_HUD = 5,   // Far Cry 5: the game's HUD on its own head-locked panel
        AFR_LEFT_EYE_B = 6,   // Far Cry 5: second image set per eye, used on alternate headset frames
        AFR_RIGHT_EYE_B = 7,
        END = 8,
    };

    // Far Cry 5 (VDXR black frames): when every game frame is a headset frame, each eye's 3-image
    // swapchain is rewritten every 3 frames, which Virtual Desktop's runtime does not guard. Two
    // swapchains per eye, alternated per headset frame, give each image 6 frames before reuse.
    std::atomic<bool> fc5_double_eyes{false};
    int               fc5_eye_set{0};
    uint32_t eye_swapchain(uint32_t i) const {
        return (i < 2 && fc5_eye_set == 1) ? i + (uint32_t)SwapchainIndex::AFR_LEFT_EYE_B : i;
    }

    // Far Cry 5, Meta XR Simulator: going from the flat-screen quad (menus, loading) back to the projection layer made the
    // NVIDIA driver log an error (nvlddmkm 153) at every such switch, and about one in five left the simulator's xrEndFrame
    // waiting forever. With the projection layer kept under the quad there is no switch and no error (simprojection).
    static inline std::atomic<bool> s_fc5_sim_projection{true};
    static bool fc5_simulator_runtime() {
        static const bool sim = [] {
            char rt[512]{};
            const DWORD n = GetEnvironmentVariableA("XR_RUNTIME_JSON", rt, sizeof(rt));
            std::string s{rt, n < sizeof(rt) ? n : 0};
            for (auto& c : s) c = (char)tolower((unsigned char)c);
            return s.find("simulator") != std::string::npos;
        }();
        return sim;
    }

    OpenXR() {
        this->custom_stage = SynchronizeStage::EARLY;
    }
    
    virtual ~OpenXR() {
        this->destroy();
    }

    struct Swapchain {
        XrSwapchain handle;
        int32_t width;
        int32_t height;
    };

    VRRuntime::Type type() const override { 
        return VRRuntime::Type::OPENXR;
    }

    std::string_view name() const override {
        return "OpenXR";
    }

    bool ready() const override {
        return VRRuntime::ready() && this->session_ready;
    }

    bool is_depth_allowed() const override {
        return this->enabled_extensions.contains(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    }

    bool is_cylinder_layer_allowed() const override {
        return this->enabled_extensions.contains(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    }


    inline int get_view_count() const {
        return this->pipeline_state.views.size();
    }

    inline int get_stage_views_count() const {
        return this->pipeline_state.stage_views.size();
    }

    inline bool should_render() const {
        return this->pipeline_state.frame_state.shouldRender == XR_TRUE;
    }

    inline auto& get_view_space_location() const {
        return this->pipeline_state.view_space_location;
    }

    inline auto& get_pipeline_state() {
        return this->pipeline_state;
    }

    inline void init_pipline_state(int views_size) {
        this->pipeline_state.views.resize(views_size, {XR_TYPE_VIEW});
        this->pipeline_state.stage_views.resize(views_size, {XR_TYPE_VIEW});
        this->pipeline_state.active_fov.resize(views_size, {});
    }

    VRRuntime::Error synchronize_frame(int frame) override;
    VRRuntime::Error update_poses(int frame) override;
    VRRuntime::Error update_render_target_size() override;
    uint32_t get_width() const override;
    uint32_t get_height() const override;

    VRRuntime::Error consume_events(std::function<void(void*)> callback) override;

    VRRuntime::Error update_matrices(float nearz, float farz) override;
    VRRuntime::Error update_input() override;

    void destroy() override;

    std::vector<DXGI_FORMAT> get_supported_swapchain_formats() const;
    bool is_supported_swapchain_format(DXGI_FORMAT format) const {
        const auto supported = this->get_supported_swapchain_formats();

        return std::find(supported.begin(), supported.end(), format) != supported.end();
    }
public: 
    // OpenXR specific methods
    std::string get_result_string(XrResult result) const;
    std::string get_structure_string(XrStructureType type) const;
    std::string get_path_string(XrPath path) const;
    XrPath get_path(const std::string& path) const;
    std::string get_current_interaction_profile() const;
    // Far Cry 5 probe: the interaction profile bound to each hand, the last action sync, and the raw
    // state of every action (only those active or non-zero, plus a count).
    std::string fc5_probe_actions() const;
    XrResult    fc5_last_sync{XR_SUCCESS};
    uint32_t    fc5_syncs{0};
    XrPath get_current_interaction_profile_path() const;

    std::optional<std::string> initialize_actions(const std::string& json_string);

    inline static glm::quat to_glm(const XrQuaternionf& q) {
    #ifndef GLM_FORCE_QUAT_DATA_XYZW
            return glm::quat{ q.w, q.x, q.y, q.z };
    #else
            return glm::quat{ q.x, q.y, q.z, q.w };
    #endif
    }

    static XrQuaternionf to_openxr(const glm::quat& q) {
        return XrQuaternionf{ q.x, q.y, q.z, q.w };
    }

    static XrVector3f to_openxr(const glm::vec3& v) {
        return XrVector3f{ v.x, v.y, v.z };
    }

    XrResult begin_frame(int frame);
    XrResult end_frame(const std::vector<XrCompositionLayerBaseHeader*>& quad_layers, int frame, bool has_depth = false);

    void begin_profile() {
        if (!this->profile_calls) {
            return;
        }

        this->profiler_start_time = std::chrono::high_resolution_clock::now();
    }

    void end_profile(std::string_view name) {
        if (!this->profile_calls) {
            return;
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto dur = std::chrono::duration<float, std::milli>(end_time - this->profiler_start_time).count();

        spdlog::info("{} took {} ms", name, dur);
    }

    bool is_action_active(XrAction action, VRRuntime::Hand hand) const;
    bool is_action_active(std::string_view action_name, VRRuntime::Hand hand) const;
    bool is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const;
    Vector2f get_action_axis(XrAction action, VRRuntime::Hand hand) const;
    std::string translate_openvr_action_name(std::string action_name) const;

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const;
    void display_bindings_editor();
    void save_bindings();

public: 
    // OpenXR specific fields
//TODO make it definition
    double prediction_scale{1.0};
    bool session_ready{false};
    bool frame_began{false};
    bool frame_synced{false};
#ifdef DEBUG_PROFILING_ENABLED
    bool profile_calls{true};
#else
    bool profile_calls{false};
#endif
    std::chrono::high_resolution_clock::time_point profiler_start_time{};

    std::recursive_mutex sync_mtx{};

    // Making it static because for some reason destroying it doesn't actually completely destroy everything.
    // So we must make sure it always exists if we ever re-initialize OpenXR.
    static inline XrInstance instance{XR_NULL_HANDLE};

    XrSession session{XR_NULL_HANDLE};
    XrSpace stage_space{XR_NULL_HANDLE};
    XrSpace view_space{XR_NULL_HANDLE}; // for generating view matrices
    XrSystemId system{XR_NULL_SYSTEM_ID};
    XrFormFactor form_factor{XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrViewConfigurationType view_config{XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
    XrEnvironmentBlendMode blend_mode{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    XrViewState stage_view_state{XR_TYPE_VIEW_STATE};
//    XrFrameState frame_state{XR_TYPE_FRAME_STATE};

    XrSessionState session_state{XR_SESSION_STATE_UNKNOWN};

//    XrSpaceLocation view_space_location{XR_TYPE_SPACE_LOCATION};

    std::unordered_set<std::string> enabled_extensions{};

    std::vector<XrViewConfigurationView> view_configs{};
    Swapchain swapchains[(uintptr_t)SwapchainIndex::END]{};
//    std::vector<XrView> views{};
//    std::vector<XrView> stage_views{};

    struct PipelineState {
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        XrSpaceLocation view_space_location{XR_TYPE_SPACE_LOCATION};
        std::vector<XrView> views{};
        std::vector<XrView> stage_views{};
        std::vector<XrFovf>  active_fov{};
    } pipeline_state{};

    int m_last_synchronized_frame{0};
    float resolution_scale{1.0f};


    struct Action {
        std::vector<XrAction> action_collection{};
    };

    struct ActionSet {
        XrActionSet handle;
        std::vector<XrAction> actions{};
        std::unordered_map<std::string, XrAction> action_map{}; // XrActions are handles so it's okay.
        std::unordered_map<XrAction, std::string> action_names{};

        std::unordered_set<XrAction> float_actions{};
        std::unordered_set<XrAction> vector2_actions{};
        std::unordered_set<XrAction> bool_actions{};
        std::unordered_set<XrAction> pose_actions{};
        std::unordered_set<XrAction> vibration_actions{};
    } action_set;

    struct VectorActivator {
        Vector2f value{};
        std::string action_name{};
    };

    struct VectorActivatorTrue {
        Vector2f value{};
        XrAction action{};
    };

    struct HandData {
        XrSpace space{XR_NULL_HANDLE};
        XrPath path{XR_NULL_PATH};
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
        
        // interaction profile -> action -> path map
        struct InteractionProfile {
            std::unordered_map<std::string, XrPath> path_map{};
            std::unordered_map<XrAction, std::vector<VectorActivatorTrue>> vector_activators{};
            std::unordered_map<XrAction, XrAction> action_vector_associations{};
        };

        std::unordered_map<std::string, InteractionProfile> profiles{};
        std::unordered_map<XrAction, bool> forced_actions{};
        std::unordered_map<XrAction, bool> prev_action_states{};

        bool active{false};

        struct UI {
            char new_path_name[XR_MAX_PATH_LENGTH]{};
            uint32_t new_path_name_length{0};
            int action_combo_index{0};

            int activator_combo_index{0};
            int modifier_combo_index{0};
            int output_combo_index{0};
            Vector2f output_vector2{};
        } ui;
    };

    std::array<HandData, 2> hands{};

public:
    struct InteractionBinding {
        std::string interaction_path_name{};
        std::string action_name{};
    };

    struct ActionVectorAssociation {
        VRRuntime::Hand hand{};
        std::string action_modifier{};
        std::string action_activator{};
        std::vector<VectorActivator> vector_activators{};
    };

    static inline std::vector<InteractionBinding> s_bindings_map {
        {"/user/hand/*/input/aim/pose", "pose"},
        {"/user/hand/*/input/trigger", "trigger"}, // oculus?
        {"/user/hand/*/input/squeeze", "grip"}, // oculus/vive/index

        {"/user/hand/left/input/x/click", "abuttonleft"}, // oculus?
        {"/user/hand/left/input/x/touch", "abutto ntouchleft"}, // oculus?

        {"/user/hand/right/input/x/click", "abuttonright"}, // oculus?
        {"/user/hand/right/input/x/touch", "abuttontouchright"}, // oculus?

        {"/user/hand/left/input/y/click", "bbuttonleft"}, // oculus?
        {"/user/hand/left/input/y/touch", "bbuttontouchleft"}, // oculus?

        {"/user/hand/right/input/y/click", "bbuttonright"}, // oculus?
        {"/user/hand/right/input/y/touch", "bbuttontouchright"}, // oculus?

        {"/user/hand/left/input/a/click", "abuttonleft"}, // oculus?
        {"/user/hand/left/input/a/touch", "abuttontouchleft"}, // oculus?

        {"/user/hand/right/input/a/click", "abuttonright"}, // oculus?
        {"/user/hand/right/input/a/touch", "abuttontouchright"}, // oculus?

        {"/user/hand/left/input/b/click", "bbuttonleft"}, // oculus?
        {"/user/hand/left/input/b/touch", "bbuttontouchleft"}, // oculus?

        {"/user/hand/right/input/b/click", "bbuttonright"}, // oculus?
        {"/user/hand/right/input/b/touch", "bbuttontouchright"}, // oculus?

        {"/user/hand/*/input/thumbstick", "joystick"}, // oculus?
        {"/user/hand/*/input/thumbstick/click", "joystickclick"}, // oculus?
        {"/user/hand/*/input/system/click", "systembutton"}, // oculus/vive/index
        {"/user/hand/*/input/menu/click", "systembutton"}, // oculus/vive/index

        {"/user/hand/left/input/thumbrest/touch", "thumbresttouchleft"}, // cv1/quest pro
        {"/user/hand/right/input/thumbrest/touch", "thumbresttouchright"}, // cv1/quest pro
        {"/user/hand/*/input/trigger/touch", "triggertouch"}, // oculus touch / index (FC5 D-pad modifier)

        {"/user/hand/*/input/trackpad", "touchpad"}, // vive & others
        {"/user/hand/*/input/trackpad/click", "touchpadclick"}, // vive & others
        {"/user/hand/*/output/haptic", "haptic"}, // most of them

        {"/user/hand/right/input/a/click", "re3_dodge"},
        {"/user/hand/right/input/trigger", "weapondial_start"},
    };

    static inline std::vector<ActionVectorAssociation> s_action_vector_associations {
        { 
            VRRuntime::Hand::RIGHT, "touchpad", "touchpadclick", {
            { {0.0, -1.0f}, "abutton" },
            { {1.0f, 0.0f}, "bbutton" },
            { {0.0f, 1.0f}, "joystickclick" }
        }},
        { 
            VRRuntime::Hand::LEFT, "touchpad", "touchpadclick", {
            { {0.0, -1.0f}, "abutton" },
            { {-1.0f, 0.0f}, "bbutton" },
            { {0.0f, 1.0f}, "joystickclick" }
        }},
    };

    static inline std::vector<std::string> s_supported_controllers {
        "/interaction_profiles/khr/simple_controller",
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/oculus/go_controller",
        "/interaction_profiles/valve/index_controller",
        "/interaction_profiles/microsoft/motion_controller",
        "/interaction_profiles/htc/vive_controller",
    };
};
}