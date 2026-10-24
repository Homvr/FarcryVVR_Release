#pragma once

#include <cstdint>
#include <string_view>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <array>

#include <spdlog/spdlog.h>
#include <math/Math.hpp>

struct VRRuntime {
    enum class Error : int64_t {
        UNSPECIFIED = -1,
        SUCCESS = 0,
        // rest of the error codes will be from the specific VR runtime
    };

    enum class Type : uint8_t {
        NONE,
        OPENXR,
        OPENVR,
    };

    enum class Eye : uint8_t {
        LEFT,
        RIGHT,
    };

    enum class SynchronizeStage : int32_t { 
        EARLY,
        LATE,
        VERY_LATE
    };

    enum Hand : uint8_t {
        LEFT,
        RIGHT,
    };

    virtual ~VRRuntime() {};

    virtual std::string_view name() const {
        return "NONE";
    }

    virtual bool ready() const {
        return this->loaded;
    }

    virtual Type type() const { 
        return Type::NONE;
    }

    virtual void destroy() {
        this->loaded = false;
    }

    virtual Error synchronize_frame(int frame) {
        return Error::SUCCESS;
    }

    virtual Error update_poses(int frame) {
        return Error::SUCCESS;
    }

    virtual Error update_render_target_size() {
        return Error::SUCCESS;
    }

    virtual Error consume_events(std::function<void(void*)> callback) {
        return Error::SUCCESS;
    }

    virtual uint32_t get_width() const {
        return 0;
    }

    virtual uint32_t get_height() const {
        return 0;
    }

    virtual float get_ipd() const {
        return this->ipd;
    }

    virtual Error update_matrices(float nearz, float farz) {
        return Error::SUCCESS;
    }

    virtual SynchronizeStage get_synchronize_stage() const {
        return this->custom_stage;
    }

    virtual Error update_input() {
        return Error::SUCCESS;
    }

    virtual bool is_depth_allowed() const {
        return false;
    }

    virtual bool is_cylinder_layer_allowed() const {
        return false;
    }

    virtual void recenter_view(const glm::mat4& view)
    {
        m_center_stage = view;
    }

    bool is_openxr() const {
        return this->type() == Type::OPENXR;
    }

    bool is_openvr() const {
        return this->type() == Type::OPENVR;
    }

    bool loaded{false};
    bool wants_reinitialize{false};
    bool dll_missing{false};

    // in the case of OpenVR we always need at least one initial WaitGetPoses before the game will render
    // even if we don't have anything to submit yet, otherwise the compositor
    // will return VRCompositorError_DoNotHaveFocus
    bool needs_pose_update{true};
    bool got_first_poses{false};
    bool got_first_valid_poses{false};
    bool got_first_sync{false};
    bool handle_pause{false};
    bool wants_reset_origin{true};

    std::optional<std::string> error{};

//    std::array<Matrix4x4f, 2> projections{};
    std::array<Matrix4x4f, 2> eyes{};

    mutable std::shared_mutex projections_mtx{};
    mutable std::shared_mutex eyes_mtx{};
    mutable std::shared_mutex pose_mtx{};

    Vector4f raw_projections[2]{};
    Vector4f frustums[2]{};
    float eye_width_adjustment{1};
    float eye_height_adjustment{1};
    float ipd{0.064f};
    float diagonal_fov{0.0f};


    float view_bounds[2][4] = {0, 1, 0, 1, 0, 1, 0, 1};

    bool HORIZONTAL_SYMMETRIC = true;
    bool VERTICAL_SYMMETRIC = true;
    bool HORIZONTAL_MIRROR = false;
    bool VERTICAL_MATCHED = false;
    bool should_grow_rectangle_for_projection_cropping = true;
    float m_horizontal_fov_scale = 1.f;
    float m_vertical_fov_scale = 1.f;
    bool m_extended_fov_range = false;
    Matrix4x4f m_center_stage = Matrix4x4f{1.0f};
    float m_flat_screen_distance = 1.5f;
    // FC5: when set, the flat screen hangs in front of this pose (stage space) instead of the recentred one.
    bool       fc5_flat_anchor_on{ false };
    Matrix4x4f fc5_flat_anchor{ 1.0f };
    float      fc5_fov_margin{ 0.0f }; // radians added on every side of each eye's field of view
    float      fc5_fov_scale{ 1.0f };  // below 1: each eye's field of view narrowed (tangents times this), same pixel count

    SynchronizeStage custom_stage{SynchronizeStage::EARLY};
};