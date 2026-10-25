#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <bitset>
#include <memory>
#include <shared_mutex>

#include <openvr.h>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl.h>

#include "utility/Patch.hpp"
#include "math/Math.hpp"
#include "vr/D3D11Component.hpp"
#include "vr/D3D12Component.hpp"
#include "vr/OverlayComponent.hpp"
#include "vr/runtimes/OpenXR.hpp"
#include "vr/runtimes/OpenVR.hpp"

#include "Mod.hpp"

#ifndef VR_DEFAULT_SYNC_INTERVAL
#define VR_DEFAULT_SYNC_INTERVAL 0
#endif

class VR : public Mod {
public:
    inline VR() {
        add_components_vr();
    }

    static const inline std::string s_action_trigger = "/actions/default/in/Trigger";
    static const inline std::string s_action_grip = "/actions/default/in/Grip";
    static const inline std::string s_action_joystick = "/actions/default/in/Joystick";
    static const inline std::string s_action_joystick_click = "/actions/default/in/JoystickClick";

    static const inline std::string s_action_a_button_left = "/actions/default/in/AButtonLeft";
    static const inline std::string s_action_b_button_left = "/actions/default/in/BButtonLeft";
    static const inline std::string s_action_a_button_touch_left = "/actions/default/in/AButtonTouchLeft";
    static const inline std::string s_action_b_button_touch_left = "/actions/default/in/BButtonTouchLeft";

    static const inline std::string s_action_a_button_right = "/actions/default/in/AButtonRight";
    static const inline std::string s_action_b_button_right = "/actions/default/in/BButtonRight";
    static const inline std::string s_action_a_button_touch_right = "/actions/default/in/AButtonTouchRight";
    static const inline std::string s_action_b_button_touch_right = "/actions/default/in/BButtonTouchRight";

    static const inline std::string s_action_dpad_up = "/actions/default/in/DPad_Up";
    static const inline std::string s_action_dpad_right = "/actions/default/in/DPad_Right";
    static const inline std::string s_action_dpad_down = "/actions/default/in/DPad_Down";
    static const inline std::string s_action_dpad_left = "/actions/default/in/DPad_Left";
    static const inline std::string s_action_system_button = "/actions/default/in/SystemButton";
    static const inline std::string s_action_thumbrest_touch_left = "/actions/default/in/ThumbrestTouchLeft";
    static const inline std::string s_action_thumbrest_touch_right = "/actions/default/in/ThumbrestTouchRight";
    static const inline std::string s_action_trigger_touch = "/actions/default/in/TriggerTouch";   // FC5: index resting on a trigger
    static std::shared_ptr<VR>& get();

    std::string_view get_name() const override { return "VR"; }

    // Called when the mod is initialized
    std::optional<std::string> on_initialize_d3d_thread() override;

//    void on_lua_state_created(sol::state& lua) override;

    void on_pre_imgui_frame() override;
    void on_present() override;
    void on_post_present() override;

//    void on_update_transform(void* transform) override;
//    void on_update_camera_controller(void* controller) override;
//    bool on_pre_gui_draw_element(void* gui_element, void* primitive_context) override;
//    void on_gui_draw_element(void* gui_element, void* primitive_context) override;
//    void on_pre_update_before_lock_scene(void* ctx) override;
//    void on_pre_lightshaft_draw(void* shaft, void* render_context) override;
//    void on_lightshaft_draw(void* shaft, void* render_context) override;

//    void on_pre_application_entry(void* entry, const char* name, size_t hash) override;
//    void on_application_entry(void* entry, const char* name, size_t hash) override;

    void on_draw_ui() override;
    void on_device_reset() override;

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    // Application entries
    void on_pre_update_hid(void* entry);
    void on_update_hid(void* entry);
    virtual bool on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) override;
    void on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) override;
    void on_xinput_get_capabilities(uint32_t* retval, uint32_t user_index, uint32_t flags, XINPUT_CAPABILITIES* capabilities) override;
    void on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) override;
    void on_begin_rendering(int frame);
//    void on_pre_end_rendering(void* entry);
    void on_end_rendering(void* entry);
//    void on_pre_wait_rendering(void* entry);
    void on_wait_rendering(int frame);
    auto get_backbuffer_size() const {
        if( m_is_d3d12) {
            return m_d3d12.get_backbuffer_size();
        } else {
            return m_d3d11.get_backbuffer_size();
        }
    }

    //unsigned long frame_count_temp{0};
    float separation_ipd{0.068f};

    template<typename T = VRRuntime>
    T* get_runtime() const {
        return (T*)m_runtime.get();
    }

    auto get_hmd() const {
        return m_openvr->hmd;
    }

    auto& get_openvr_poses() const {
        return m_openvr->render_poses;
    }

    auto& get_overlay_component() {
        return m_overlay_component;
    }

    auto get_hmd_width() const {
        return get_runtime()->get_width();
    }

    auto get_hmd_height() const {
        return get_runtime()->get_height();
    }

    auto get_last_controller_update() const {
        return m_last_controller_update;
    }

//    int32_t get_frame_count() const;
//    int32_t get_game_frame_count() const;

    bool is_using_async_aer() const {
        // openVR has issues with 2 different poses at the same time for non steam native VR hmds like oculus
        // for these exceptions need to implement viewport cropping and viewport reprojection
        //TODO it looks like runtime does not fully support dx11 reprojeciton, I need to do it manually
        return m_native_stereo.load() || m_use_async_aer->value() || m_fc5_afw_active.load();
    }

    // Native stereo: the game renders both eyes every frame into its own textures. Every frame
    // is then synchronized and submitted, the "current" eye is always the left one, and both
    // eye textures are copied to their swapchains.
    bool is_native_stereo() const {
        return m_native_stereo.load();
    }

    void set_native_stereo(bool enabled) {
        m_native_stereo.store(enabled);
    }

    void set_native_stereo_eyes(ID3D12Resource* left, ID3D12Resource* right, D3D12_RESOURCE_STATES state) {
        std::scoped_lock _{m_native_eyes_mtx};
        m_native_eyes[0] = left;
        m_native_eyes[1] = right;
        m_native_eyes_state = state;
    }

    bool get_native_stereo_eyes(ID3D12Resource*& left, ID3D12Resource*& right, D3D12_RESOURCE_STATES& state) {
        std::scoped_lock _{m_native_eyes_mtx};
        left = m_native_eyes[0];
        right = m_native_eyes[1];
        state = m_native_eyes_state;
        return left != nullptr && right != nullptr;
    }

    void set_native_stereo_overlay(ID3D12Resource* overlay, D3D12_RESOURCE_STATES state) {
        std::scoped_lock _{m_native_eyes_mtx};
        m_native_overlay = overlay;
        m_native_overlay_state = state;
    }

    void set_native_stereo_overlay_scale(float scale) { m_native_overlay_scale.store(std::clamp(scale, 0.3f, 1.0f)); }
    float get_native_stereo_overlay_scale() const { return m_native_overlay_scale.load(); }

    bool composite_native_overlay(ID3D12Resource* left, ID3D12Resource* right, D3D12_RESOURCE_STATES eye_state, ID3D12Resource* overlay, D3D12_RESOURCE_STATES overlay_state) {
        ID3D12Resource* const eyes[2]{left, right};
        return m_d3d12.composite_native_overlay(eyes, eye_state, overlay, overlay_state);
    }

    bool get_native_stereo_overlay(ID3D12Resource*& overlay, D3D12_RESOURCE_STATES& state) {
        std::scoped_lock _{m_native_eyes_mtx};
        overlay = m_native_overlay;
        state = m_native_overlay_state;
        return overlay != nullptr;
    }

    Matrix4x4f get_eye_transform(VRRuntime::Eye eye);

    bool is_gui_enabled() const {
        return true;
    }

    // Functions that generally use a mutex or have more complex logic
    Matrix4x4f get_transform_offset();
    void set_transform_offset(const Matrix4x4f& offset);
    void recenter_view();

    glm::quat get_gui_rotation_offset();
    void set_gui_rotation_offset(const glm::quat& offset);
    void recenter_gui(const glm::quat& from);

    Vector4f get_current_offset();
    Matrix4x4f get_current_eye_transform(bool flip = false);
    Matrix4x4f get_current_projection_matrix(bool flip = false);

    auto& get_controllers() const {
        return m_controllers;
    }

    bool is_using_controllers() const {
        if (m_controllers_allowed == nullptr || m_motion_controls_inactivity_timer == nullptr) {
            return false;   // seen once at the first headset frames in Far Cry 5 (cause unknown)
        }
        return m_controllers_allowed->value() && is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= std::chrono::seconds((int32_t)m_motion_controls_inactivity_timer->value());
    }

    bool is_using_controllers_within(std::chrono::seconds seconds) const {
        if (m_controllers_allowed == nullptr) {
            return false;
        }
        return m_controllers_allowed->value() && is_hmd_active() && !m_controllers.empty() && (std::chrono::steady_clock::now() - m_last_controller_update) <= seconds;
    }

    bool is_hmd_active() const {
        return get_runtime()->ready();
    }
    
    bool is_openvr_loaded() const {
        return m_openvr != nullptr && m_openvr->loaded;
    }

    bool is_openxr_loaded() const {
        return m_openxr != nullptr && m_openxr->loaded;
    }

    bool is_using_hmd_oriented_audio() {
        return m_hmd_oriented_audio->value();
    }

    void toggle_hmd_oriented_audio() {
        m_hmd_oriented_audio->toggle();
    }

    const Matrix4x4f& get_last_render_matrix() {
        return m_render_camera_matrix;
    }

    Vector4f get_position(uint32_t index)  const;
    Vector4f get_velocity(uint32_t index)  const;
    Vector4f get_angular_velocity(uint32_t index)  const;
    Matrix4x4f get_rotation(uint32_t index)  const;
    Matrix4x4f get_transform(uint32_t index) const;
    vr::HmdMatrix34_t get_raw_transform(uint32_t index) const;

    const auto& get_eyes() const {
        return get_runtime()->eyes;
    }

    void apply_hmd_transform(glm::quat& rotation, Vector4f& position);
//    void apply_hmd_transform(::REJoint* camera_joint);
    
    bool is_hand_behind_head(VRRuntime::Hand hand, float sensitivity = 0.2f) const;
    bool is_action_active_any_joystick(vr::VRActionHandle_t action) const;
    bool is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle) const;
    Vector2f get_joystick_axis(vr::VRInputValueHandle_t handle) const;


    inline vr::VRActionHandle_t get_action_handle(std::string_view action_path) {
        if (auto it = m_action_handles.find(action_path.data()); it != m_action_handles.end()) {
            return it->second;
        }

        return vr::k_ulInvalidActionHandle;
    }

    Vector2f get_left_stick_axis() const;
    Vector2f get_right_stick_axis() const;

    void trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source = vr::k_ulInvalidInputValueHandle);
    
//    auto get_action_set() const { return m_action_set; }
//    auto& get_active_action_set() const { return m_active_action_set; }
//    auto get_action_trigger() const { return m_action_trigger; }
//    auto get_action_grip() const { return m_action_grip; }
//    auto get_action_joystick() const { return m_action_joystick; }
//    auto get_action_joystick_click() const { return m_action_joystick_click; }
//    auto get_action_a_button() const { return m_action_a_button; }
//    auto get_action_b_button() const { return m_action_b_button; }
//    auto get_action_weapon_dial() const { return m_action_weapon_dial; }
//    auto get_action_minimap() const { return m_action_minimap; }
//    auto get_action_block() const { return m_action_block; }
//    auto get_action_dpad_up() const { return m_action_dpad_up; }
//    auto get_action_dpad_down() const { return m_action_dpad_down; }
//    auto get_action_dpad_left() const { return m_action_dpad_left; }
//    auto get_action_dpad_right() const { return m_action_dpad_right; }
//    auto get_action_heal() const { return m_action_heal; }


    int get_left_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 2 : 1;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[1] : m_controllers[0]) : -1;
        }

        return -1;
    }


    int get_right_controller_index() const {
        const auto wants_swap = m_swap_controllers->value();

        if (m_runtime->is_openxr()) {
            return wants_swap ? 1 : 2;
        } else if (m_runtime->is_openvr()) {
            return !m_controllers.empty() ? (wants_swap ? m_controllers[0] : m_controllers[1]) : -1;
        }

        return -1;
    }

    auto get_left_joystick() const { return m_left_joystick; }
    auto get_right_joystick() const { return m_right_joystick; }

    const auto& get_action_handles() const { return m_action_handles;}

    auto get_ui_scale() const { return m_ui_scale_option->value(); }
    const auto& get_raw_projections() const { return get_runtime()->raw_projections; }

    void unhide_crosshair() {
        m_last_crosshair_hide = std::chrono::steady_clock::now();
    }

    void update_action_states();

private:
    Vector4f get_position_unsafe(uint32_t index) const;
    Vector4f get_velocity_unsafe(uint32_t index) const;
    Vector4f get_angular_velocity_unsafe(uint32_t index) const;

private:
    // Hooks
//    void on_view_get_size(REManagedObject* scene_view, float* result) override;
//    static void inputsystem_update_hook(void* ctx, REManagedObject* input_system);
//    void on_camera_get_projection_matrix(REManagedObject* camera, Matrix4x4f* result) override;
//    static Matrix4x4f* gui_camera_get_projection_matrix_hook(REManagedObject* camera, Matrix4x4f* result);
//    void on_camera_get_view_matrix(REManagedObject* camera, Matrix4x4f* result) override;

//    bool on_pre_overlay_layer_update(sdk::renderer::layer::Overlay* layer, void* render_context) override;
//    bool on_pre_overlay_layer_draw(sdk::renderer::layer::Overlay* layer, void* render_context) override;

//    bool on_pre_post_effect_layer_update(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
//    bool on_pre_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
//    void on_post_effect_layer_draw(sdk::renderer::layer::PostEffect* layer, void* render_context) override;
    uint32_t m_previous_distortion_type{};
    bool m_set_next_post_effect_distortion_type{false};

//    bool on_pre_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
//    void on_scene_layer_update(sdk::renderer::layer::Scene* layer, void* render_context) override;
//    bool on_pre_scene_layer_draw(sdk::renderer::layer::Scene* layer, void* render_context) override;
//    bool m_set_next_scene_layer_data{false};
//
//    struct SceneLayerData {
//        SceneLayerData() = default;
//        SceneLayerData(sdk::renderer::SceneInfo* info)
//            : scene_info(info)
//        {
//            if (scene_info != nullptr) {
//                this->view_projection_matrix = scene_info->view_projection_matrix;
//            }
//        }
//
//        sdk::renderer::SceneInfo* scene_info{};
//        Matrix4x4f view_projection_matrix{};
//    };
//
//    std::array<SceneLayerData, 5> m_scene_layer_data {};

    static void wwise_listener_update_hook(void* listener);

    //static float get_sharpness_hook(void* tonemapping);

    // initialization functions
    std::optional<std::string> initialize_openvr();
    std::optional<std::string> initialize_openvr_input();
    std::optional<std::string> initialize_openxr();
    std::optional<std::string> initialize_openxr_input();
    std::optional<std::string> initialize_openxr_swapchains();
    std::optional<std::string> hijack_resolution();
    std::optional<std::string> hijack_input();
    std::optional<std::string> hijack_camera();
    std::optional<std::string> hijack_wwise_listeners(); // audio hook

    std::optional<std::string> reinitialize_openvr() {
        spdlog::info("Reinitializing OpenVR");
        std::scoped_lock _{m_openvr_mtx};

        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        m_openvr.reset();

        // Reinitialize openvr input, hopefully this fixes the issue
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openvr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenVR: {}", *e);
        }

        return e;
    }

    std::optional<std::string> reinitialize_openxr() {
        spdlog::info("Reinitializing OpenXR");
        std::scoped_lock _{m_openvr_mtx};

        if (m_is_d3d12) {
            m_d3d12.openxr().destroy_swapchains();
        } else {
            m_d3d11.openxr().destroy_swapchains();
        }

        m_openxr.reset();
        m_runtime.reset();
        m_runtime = std::make_shared<VRRuntime>();
        
        m_controllers.clear();
        m_controllers_set.clear();

        auto e = initialize_openxr();

        if (e) {
            spdlog::error("Failed to reinitialize OpenXR: {}", *e);
        }

        return e;
    }

    bool detect_controllers();
    bool is_any_action_down();
public:
    void update_hmd_state(int frame);
private:
    void update_camera(); // if not in firstperson mode
    void update_camera_origin(); // every frame
    void update_audio_camera();
    void update_render_matrix();
    void restore_audio_camera(); // after wwise listener update
    void restore_camera(); // After rendering
    void set_lens_distortion(bool value);
    void disable_bad_effects();
    void fix_temporal_effects();

    // input functions
    // Purpose: "Emulate" OpenVR input to the game
    // By setting things like input flags based on controller state
//    void openvr_input_to_re2_re3(REManagedObject* input_system);
    void openvr_input_to_re_engine(); // generic, can be used on any game

    // Sets overlay layer to return instantly
    // causes world-space gui elements to render properly
    Patch::Ptr m_overlay_draw_patch{};
    
    mutable std::recursive_mutex m_openvr_mtx{};
    mutable std::recursive_mutex m_wwise_mtx{};
    mutable std::shared_mutex m_gui_mtx{};
    mutable std::shared_mutex m_rotation_mtx{};

    vr::VRTextureBounds_t m_right_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };
    vr::VRTextureBounds_t m_left_bounds{ 0.0f, 0.0f, 1.0f, 1.0f };

    glm::vec3 m_overlay_rotation{-1.550f, 0.0f, -1.330f};
    glm::vec4 m_overlay_position{0.0f, 0.06f, -0.07f, 1.0f};

public:
    float m_nearz{ 0.1f };
    float m_farz{ 3000.0f };
private:

    std::shared_ptr<VRRuntime> m_runtime{std::make_shared<VRRuntime>()}; // will point to the real runtime if it exists
    std::shared_ptr<runtimes::OpenVR> m_openvr{std::make_shared<runtimes::OpenVR>()};
    std::shared_ptr<runtimes::OpenXR> m_openxr{std::make_shared<runtimes::OpenXR>()};

    Matrix4x4f m_transform_offset{ glm::identity<Matrix4x4f>() };
    glm::quat m_gui_rotation_offset{ glm::identity<glm::quat>() };

    std::vector<int32_t> m_controllers{};
    std::unordered_set<int32_t> m_controllers_set{};

    // Action set handles
    vr::VRActionSetHandle_t m_action_set{};
    vr::VRActiveActionSet_t m_active_action_set{};

    // Action handles
    vr::VRActionHandle_t m_action_trigger{ };
    vr::VRActionHandle_t m_action_grip{ };
    vr::VRActionHandle_t m_action_joystick{};
    vr::VRActionHandle_t m_action_joystick_click{};

    vr::VRActionHandle_t m_action_a_button_right{};
    vr::VRActionHandle_t m_action_a_button_touch_right{};
    vr::VRActionHandle_t m_action_b_button_right{};
    vr::VRActionHandle_t m_action_b_button_touch_right{};

    vr::VRActionHandle_t m_action_a_button_left{};
    vr::VRActionHandle_t m_action_a_button_touch_left{};
    vr::VRActionHandle_t m_action_b_button_left{};
    vr::VRActionHandle_t m_action_b_button_touch_left{};

    vr::VRActionHandle_t m_action_dpad_up{};
    vr::VRActionHandle_t m_action_dpad_right{};
    vr::VRActionHandle_t m_action_dpad_down{};
    vr::VRActionHandle_t m_action_dpad_left{};
    vr::VRActionHandle_t m_action_system_button{};
    vr::VRActionHandle_t m_action_haptic{};
    vr::VRActionHandle_t m_action_thumbrest_touch_left{};
    vr::VRActionHandle_t m_action_thumbrest_touch_right{};
    vr::VRActionHandle_t m_action_trigger_touch{};
    
    std::unordered_map<std::string, std::reference_wrapper<vr::VRActionHandle_t>> m_action_handles {
        { s_action_trigger, m_action_trigger },
        { s_action_grip, m_action_grip },
        { s_action_joystick, m_action_joystick },
        { s_action_joystick_click, m_action_joystick_click },

        { s_action_a_button_left, m_action_a_button_left },
        { s_action_b_button_left, m_action_b_button_left },
        { s_action_a_button_touch_left, m_action_a_button_touch_left },
        { s_action_b_button_touch_left, m_action_b_button_touch_left },

        { s_action_a_button_right, m_action_a_button_right },
        { s_action_b_button_right, m_action_b_button_right },
        { s_action_a_button_touch_right, m_action_a_button_touch_right },
        { s_action_b_button_touch_right, m_action_b_button_touch_right },

        { s_action_dpad_up, m_action_dpad_up },
        { s_action_dpad_right, m_action_dpad_right },
        { s_action_dpad_down, m_action_dpad_down },
        { s_action_dpad_left, m_action_dpad_left },

        { s_action_system_button, m_action_system_button },
        { s_action_thumbrest_touch_left, m_action_thumbrest_touch_left },
        { s_action_thumbrest_touch_right, m_action_thumbrest_touch_right },
        { s_action_trigger_touch, m_action_trigger_touch },

        // Out
        { "/actions/default/out/Haptic", m_action_haptic },
    };

    // Input sources
    vr::VRInputValueHandle_t m_left_joystick{};
    vr::VRInputValueHandle_t m_right_joystick{};

    // Input system history
    std::bitset<64> m_button_states_down{};
    std::bitset<64> m_button_states_on{};
    std::bitset<64> m_button_states_up{};
    std::chrono::steady_clock::time_point m_last_controller_update{};
    std::chrono::steady_clock::time_point m_last_xinput_update{};
    std::chrono::steady_clock::time_point m_last_xinput_spoof_sent{};
    std::chrono::steady_clock::time_point m_last_interaction_display{};
    std::chrono::steady_clock::time_point m_last_crosshair_hide{};
    uint32_t m_backbuffer_inconsistency_start{};
    std::chrono::nanoseconds m_last_input_delay{};
    std::chrono::nanoseconds m_avg_input_delay{};

    uint32_t m_lowest_xinput_user_index{};

    HANDLE m_present_finished_event{CreateEvent(nullptr, TRUE, FALSE, nullptr)};

    Vector4f m_raw_projections[2]{};

    vrmod::D3D11Component m_d3d11{};
    vrmod::D3D12Component m_d3d12{};
    vrmod::OverlayComponent m_overlay_component{};

    Vector4f m_original_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_original_camera_matrix{ glm::identity<Matrix4x4f>() };

    Vector4f m_original_audio_camera_position{ 0.0f, 0.0f, 0.0f, 0.0f };
    glm::quat m_original_audio_camera_rotation{ glm::identity<glm::quat>() };

    Matrix4x4f m_render_camera_matrix{ glm::identity<Matrix4x4f>() };

//    sdk::helpers::NativeObject m_via_hid_gamepad{ "via.hid.GamePad" };
protected:
    inline void add_components_vr() {
        m_components = {
            &m_overlay_component
        };
    }


    // options
public:
    int m_engine_frame_count{0};
    bool controllers_setting_missing() const { return m_controllers_allowed == nullptr; }
    // Far Cry 5 probe (defined in the game module): the controller input chain in one line.
    std::string fc5_probe_input();
    // AFW: when set for this present, the async path submits this texture (8-bit RGBA, the eye size)
    // as the held eye instead of that eye's last image. The game module owns it and its pose slot.
    ID3D11Texture2D* m_afw_held_texture{nullptr};
    // Far Cry 5 AFW: every frame is submitted (fresh eye + the other eye rebuilt for this pose).
    std::atomic<bool> m_fc5_afw_active{false};
    std::atomic<int> m_fc5_black_frame{-1};
    // Far Cry 5 AFW: both eyes of this present drawn/rebuilt for one pose sample; the layer views take
    // these poses (x y z, qx qy qz qw) and FOVs (left right up down) instead of the per-frame slots.
    bool  m_fc5_views_set{false};
    float m_fc5_view_pose[2][7]{};
    float m_fc5_view_fov[2][4]{};
    ID3D11Texture2D* m_fc5_eye_override{nullptr};
    // Far Cry 5 pipeline review: P2 = no blocking 5 s eye probe; P7 = no back-buffer copy into the eye texture when the eye image replaces it.
    std::atomic<bool> m_fc5_no_eye_probe{true};
    std::atomic<bool> m_fc5_skip_bb_copy{true};
    // HUD/F11 report F1: the F11 layer is submitted on every frame (transparent while closed). B1: panel HUD swapchain at the HUD's size.
    // F11 menu drawn into the eye images instead of its own layer (cmd f11image): the switch, whether the mod draws it
    // this frame (DLSS eye images, not the flat menu screen), and the menu quad of the last end-frame (pose x y z qx qy qz qw,
    // width, height; in view space when m_fc5_f11_quad_view) for the mod to draw.
    std::atomic<bool> m_fc5_f11_in_image{true};
    std::atomic<bool> m_fc5_f11_image_active{false};
    float m_fc5_f11_quad[9]{};
    bool  m_fc5_f11_quad_view{false};
    bool  m_fc5_f11_quad_valid{false};
    static inline bool s_fc5_hud_full_res{false};      // read from fc5vr_settings.txt at start ("hudfullres on"); applies at swapchain creation
    static inline uint32_t s_fc5_hud_res[2]{0, 0};     // the game render size = the HUD texture size
    // Far Cry 5 (black-flicker test): flush the immediate context before xrEndFrame.
    std::atomic<bool> m_fc5_xr_flush{false};
    // Far Cry 5, Meta XR Simulator only (xrflushsim, default on): the same flush. Without it the simulator's xrEndFrame
    // waited forever on the game's unflushed GPU work at the first 3D frames after a load (16 hangs 11-16 Oct).
    std::atomic<bool> m_fc5_xr_flush_sim{true};
    // Far Cry 5 HUD panel: this frame's HUD image (null: no panel), its distance, width and height (m).
    ID3D11Texture2D* m_fc5_hud_texture{nullptr};
    // Far Cry 5: the eye images' size sent to the headset (DLSS output / resolution multiplier); 0 = the back buffer's.
    static inline uint32_t s_fc5_eye_size[2]{0, 0};
    // Far Cry 5 HUD panel diagnostics: frames offered with a HUD texture, layers submitted, end-frame passes.
    static inline uint32_t s_fc5_hud_offered{0}, s_fc5_hud_submits{0}, s_fc5_end_frames{0};
    float            m_fc5_hud_distance{1.2f}, m_fc5_hud_width{1.4f}, m_fc5_hud_height{-0.05f}; // this frame's eye image when it is not the back buffer (DLSS upscaling)
    int m_render_frame_count{0};
    int m_presenter_frame_count{0};
    bool m_skip_next_present{false};

private:
    int m_last_frame_count{-1};
    int m_left_eye_frame_count{0};
    int m_right_eye_frame_count{0};

    bool m_submitted{false};
    //bool m_disable_sharpening{true};

    bool m_needs_camera_restore{false};
    bool m_needs_audio_restore{false};
    bool m_in_render{false};
    bool m_in_lightshaft{false};
    bool m_positional_tracking{true};
    bool m_is_d3d12{false};
    bool m_backbuffer_inconsistency{false};
    bool m_init_finished{false};
    bool m_spoofed_gamepad_connection{false};
public:
    bool m_has_hw_scheduling{false}; // hardware accelerated GPU scheduling
private:

    // on the backburner
    bool m_depth_aided_reprojection{false};

    // == 1 or == 0
    uint8_t m_left_eye_interval{0};
    uint8_t m_right_eye_interval{1};

    std::atomic<bool> m_native_stereo{false};
    std::mutex m_native_eyes_mtx{};
    ID3D12Resource* m_native_eyes[2]{};
    D3D12_RESOURCE_STATES m_native_eyes_state{D3D12_RESOURCE_STATE_COPY_DEST};
    ID3D12Resource* m_native_overlay{nullptr};
    std::atomic<float> m_native_overlay_scale{1.0f};
    D3D12_RESOURCE_STATES m_native_overlay_state{D3D12_RESOURCE_STATE_COPY_DEST};

    static std::string actions_json;
    static std::string binding_rift_json;
    static std::string bindings_oculus_touch_json;
    static std::string binding_vive;
    static std::string bindings_vive_controller;
    static std::string bindings_knuckles;

    const std::unordered_map<std::string, std::string> m_binding_files {
        { "actions.json", actions_json },
        { "binding_rift.json", binding_rift_json },
        { "bindings_oculus_touch.json", bindings_oculus_touch_json },
        { "binding_vive.json", binding_vive },
        { "bindings_vive_controller.json", bindings_vive_controller },
        { "bindings_knuckles.json", bindings_knuckles }
    };

    inline static const std::vector<std::string> s_sync_interval_options {
        "Early",
        "Late",
        "VeryLate"
    };

    const ModCombo::Ptr m_sync_interval{ ModCombo::create(generate_name("SyncInterval"), s_sync_interval_options, VR_DEFAULT_SYNC_INTERVAL) };

    const ModKey::Ptr m_recenter_view_key{ ModKey::create(generate_name("RecenterViewKey")) };
    const ModToggle::Ptr m_decoupled_pitch{ ModToggle::create(generate_name("DecoupledPitch"), false) };
    const ModToggle::Ptr m_use_async_aer{ ModToggle::create(generate_name("AsyncAER"), false) };
    const ModToggle::Ptr m_use_custom_view_distance{ ModToggle::create(generate_name("UseCustomViewDistance"), false) };
    const ModToggle::Ptr m_hmd_oriented_audio{ ModToggle::create(generate_name("HMDOrientedAudio"), true) };
    const ModSlider::Ptr m_view_distance{ ModSlider::create(generate_name("CustomViewDistance"), 10.0f, 3000.0f, 500.0f) };
    const ModSlider::Ptr m_motion_controls_inactivity_timer{ ModSlider::create(generate_name("MotionControlsInactivityTimer"), 30.0f, 100.0f, 60.0f) };
    const ModSlider::Ptr m_joystick_deadzone{ ModSlider::create(generate_name("JoystickDeadzone"), 0.01f, 0.9f, 0.15f) };
    const ModSlider::Ptr m_ui_scale_option{ ModSlider::create(generate_name("2DUIScale"), 1.0f, 100.0f, 12.0f) };
    const ModSlider::Ptr m_ui_distance_option{ ModSlider::create(generate_name("2DUIDistance"), 0.01f, 100.0f, 1.0f) };
    const ModSlider::Ptr m_world_scale_option{ ModSlider::create(generate_name("WorldSpaceScale"), 0.5f, 2.0f, 1.0f) };
    const ModSlider::Ptr m_resolution_scale{ ModSlider::create(generate_name("OpenXRResolutionScale"), 0.1f, 5.0f, 1.0f) };
    const ModSlider::Ptr m_horizontal_fov_scale{ ModSlider::create(generate_name("HorizontalFOVScale"), 0.0f, 1.0f, 1.0f) };
    const ModSlider::Ptr m_vertical_fov_scale{ ModSlider::create(generate_name("VerticalFOVScale"), 0.0f, 1.0f, 1.0f) };
    const ModToggle::Ptr m_extended_fov_rage{ ModToggle::create(generate_name("ExtendedScaleFovRange"), false) };
    const ModSlider::Ptr m_flat_screen_distance{ ModSlider::create(generate_name("FlatSCreenDistance"), 0.0f, 2.0f, 1.5f) };

    const ModToggle::Ptr m_force_fps_settings{ ModToggle::create(generate_name("ForceFPS"), true) };
    const ModToggle::Ptr m_force_aa_settings{ ModToggle::create(generate_name("ForceAntiAliasing"), true) };
    const ModToggle::Ptr m_force_motionblur_settings{ ModToggle::create(generate_name("ForceMotionBlur"), true) };
    const ModToggle::Ptr m_force_vsync_settings{ ModToggle::create(generate_name("ForceVSync"), true) };
    const ModToggle::Ptr m_force_lensdistortion_settings{ ModToggle::create(generate_name("ForceLensDistortion"), true) };
    const ModToggle::Ptr m_force_volumetrics_settings{ ModToggle::create(generate_name("ForceVolumetrics"), true) };
    const ModToggle::Ptr m_force_lensflares_settings{ ModToggle::create(generate_name("ForceLensFlares"), true) };
    const ModToggle::Ptr m_force_dynamic_shadows_settings{ ModToggle::create(generate_name("ForceDynamicShadows"), true) };
    const ModToggle::Ptr m_allow_engine_overlays{ ModToggle::create(generate_name("AllowEngineOverlays"), true) };
    const ModToggle::Ptr m_desktop_fix{ ModToggle::create(generate_name("DesktopRecordingFix"), true) };
    const ModToggle::Ptr m_desktop_fix_skip_present{ ModToggle::create(generate_name("DesktopRecordingFixSkipPresent"), true) };
    const ModToggle::Ptr m_enable_asynchronous_rendering{ ModToggle::create(generate_name("AsyncRendering_V2"), true) };
    const ModToggle::Ptr m_swap_controllers{ ModToggle::create(generate_name("SwapControllerInputs"), false) };

    const ModToggle::Ptr m_controllers_allowed{ ModToggle::create(generate_name("ControllersAllowed"), true) };


    bool m_disable_projection_matrix_override{ false };
    bool m_disable_gui_camera_projection_matrix_override{ false };
    bool m_disable_view_matrix_override{false};
    bool m_disable_backbuffer_size_override{false};
    bool m_disable_temporal_fix{false};
    bool m_disable_post_effect_fix{false};

    ValueList m_options{
        *m_recenter_view_key,
//        *m_decoupled_pitch,
        *m_use_async_aer,
//        *m_use_custom_view_distance,
//        *m_hmd_oriented_audio,
//        *m_view_distance,
          *m_motion_controls_inactivity_timer,
//        *m_joystick_deadzone,
//        *m_force_fps_settings,
//        *m_force_aa_settings,
//        *m_force_motionblur_settings,
//        *m_force_vsync_settings,
//        *m_force_lensdistortion_settings,
//        *m_force_volumetrics_settings,
//        *m_force_lensflares_settings,
//        *m_force_dynamic_shadows_settings,
//        *m_ui_scale_option,
//        *m_ui_distance_option,
        *m_world_scale_option,
//        *m_allow_engine_overlays,
        *m_resolution_scale,
        *m_horizontal_fov_scale,
        *m_vertical_fov_scale,
        *m_extended_fov_rage,
        *m_desktop_fix,
        *m_desktop_fix_skip_present,
        *m_sync_interval,
//        *m_enable_asynchronous_rendering
    };

    bool m_use_rotation{true};

    friend class vrmod::D3D11Component;
    friend class vrmod::D3D12Component;
    friend class vrmod::OverlayComponent;
public:
    VRRuntime::Eye get_current_render_eye() const;
};
