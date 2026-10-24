#include "mods/VR.hpp"

#include "OpenVR.hpp"

namespace runtimes {
VRRuntime::Error OpenVR::synchronize_frame(int frame) {
    if (this->got_first_poses && !this->is_hmd_active) {
        return VRRuntime::Error::SUCCESS;
    }

    vr::VRCompositor()->SetTrackingSpace(vr::TrackingUniverseStanding);
    auto ret = vr::VRCompositor()->WaitGetPoses(this->real_render_poses.data(), vr::k_unMaxTrackedDeviceCount, this->real_game_poses.data(), vr::k_unMaxTrackedDeviceCount);

    if (ret == vr::VRCompositorError_None) {
        this->got_first_valid_poses = true;
        this->got_first_sync = true;
    }

    return (VRRuntime::Error)ret;
}

VRRuntime::Error OpenVR::update_poses(int frame) {
    if (!this->ready()) {
        return VRRuntime::Error::SUCCESS;
    }

    std::unique_lock _{ this->pose_mtx };

    memcpy(this->render_poses.data(), this->real_render_poses.data(), sizeof(this->render_poses));
    this->needs_pose_update = false;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenVR::update_render_target_size() {
    this->hmd->GetRecommendedRenderTargetSize(&this->w, &this->h);

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenVR::get_width() const {
    return this->w;
}

uint32_t OpenVR::get_height() const {
    return this->h;
}

VRRuntime::Error OpenVR::consume_events(std::function<void(void*)> callback) {
    // Process OpenVR events
    vr::VREvent_t event{};
    while (this->hmd->PollNextEvent(&event, sizeof(event))) {
        if (callback) {
            callback(&event);
        }

        switch ((vr::EVREventType)event.eventType) {
            // Detect whether video settings changed
            case vr::VREvent_SteamVRSectionSettingChanged: {
                spdlog::info("VR: VREvent_SteamVRSectionSettingChanged");
                update_render_target_size();
            } break;

            // Detect whether SteamVR reset the standing/seated pose
            case vr::VREvent_SeatedZeroPoseReset: [[fallthrough]];
            case vr::VREvent_StandingZeroPoseReset: {
                spdlog::info("VR: VREvent_SeatedZeroPoseReset");
                this->wants_reset_origin = true;
            } break;

            case vr::VREvent_DashboardActivated: {
                this->handle_pause = true;
            } break;

            default:
                spdlog::info("VR: Unknown event: {}", (uint32_t)event.eventType);
                break;
        }
    }

    return VRRuntime::Error::SUCCESS;
}



VRRuntime::Error OpenVR::update_matrices(float nearz, float farz){
    std::unique_lock __{ this->eyes_mtx };
    const auto local_left = this->hmd->GetEyeToHeadTransform(vr::Eye_Left);
    const auto local_right = this->hmd->GetEyeToHeadTransform(vr::Eye_Right);

    this->eyes[vr::Eye_Left] = glm::rowMajor4(Matrix4x4f{ *(Matrix3x4f*)&local_left } );
    this->eyes[vr::Eye_Right] = glm::rowMajor4(Matrix4x4f{ *(Matrix3x4f*)&local_right } );

    auto get_mat = [&](vr::EVREye eye) {
        const auto& vr = VR::get();
        std::array<float, 4> tan_half_fov{};

        if (HORIZONTAL_SYMMETRIC) {
            tan_half_fov[0] = std::max(std::max(-this->raw_projections[0][0], this->raw_projections[0][1]),
                                       std::max(-this->raw_projections[1][0], this->raw_projections[1][1]));
            tan_half_fov[1] = -tan_half_fov[0];
        } else if (HORIZONTAL_MIRROR) {
            const auto max_outer = std::max(-this->raw_projections[0][0], this->raw_projections[1][1]);
            const auto max_inner = std::max(this->raw_projections[0][1], -this->raw_projections[1][0]);
            tan_half_fov[0] = eye == 0 ? max_outer : max_inner;
            tan_half_fov[1] = eye == 0 ? -max_inner : -max_outer;
        } else {
            tan_half_fov[0] = -this->raw_projections[eye][0];
            tan_half_fov[1] = -this->raw_projections[eye][1];
        }

        if (VERTICAL_SYMMETRIC) {
            tan_half_fov[2] = std::max(std::max(-this->raw_projections[0][2], this->raw_projections[0][3]),
                                       std::max(-this->raw_projections[1][2], this->raw_projections[1][3]));
            tan_half_fov[3] = -tan_half_fov[2];
        } else if (VERTICAL_MATCHED) {

            tan_half_fov[2] = std::max(-this->raw_projections[0][2], -this->raw_projections[1][2]);
            tan_half_fov[3] = -std::max(this->raw_projections[0][3], this->raw_projections[1][3]);
        } else {
            tan_half_fov[2] = -this->raw_projections[eye][2];
            tan_half_fov[3] = -this->raw_projections[eye][3];
        }
        view_bounds[eye][0] = 0.5f + 0.5f * this->raw_projections[eye][0] / tan_half_fov[0];
        view_bounds[eye][1] = 0.5f - 0.5f * this->raw_projections[eye][1] / tan_half_fov[1];
        // note the swapped up / down indices from the raw projection values:
        view_bounds[eye][2] = 0.5f + 0.5f * this->raw_projections[eye][3] / tan_half_fov[3];
        view_bounds[eye][3] = 0.5f - 0.5f * this->raw_projections[eye][2] / tan_half_fov[2];

        // if we've derived the right eye, we have up to date view bounds for both so adjust the render target if necessary
        if (eye == 1) {
            if (should_grow_rectangle_for_projection_cropping) {
                eye_width_adjustment = 1 / std::max(view_bounds[0][1] - view_bounds[0][0], view_bounds[1][1] - view_bounds[1][0]);
                eye_height_adjustment = 1 / std::max(view_bounds[0][3] - view_bounds[0][2], view_bounds[1][3] - view_bounds[1][2]);
            } else {
                eye_width_adjustment = 1;
                eye_height_adjustment = 1;
            }
//            SPDLOG_INFO("Eye texture proportion scale: {} by {}", eye_width_adjustment, eye_height_adjustment);
        }

        const auto left =   -tan_half_fov[0];
        const auto right =  -tan_half_fov[1];
        const auto top =    tan_half_fov[2];
        const auto bottom = tan_half_fov[3];

        // signs : at this point we expect right [1] and bottom [3] to be negative
//        SPDLOG_INFO("Original FOV for {} eye: {}, {}, {}, {}", eye == 0 ? "left" : "right", -this->raw_projections[eye][0], -this->raw_projections[eye][1],
//                                                                                            -this->raw_projections[eye][2], -this->raw_projections[eye][3]);
//        SPDLOG_INFO("Derived FOV for {} eye:  {}, {}, {}, {}",  eye == 0 ? "left" : "right", left, right, top, bottom);
//        SPDLOG_INFO("Derived texture bounds {} eye: {}, {}, {}, {}", eye == 0 ? "left" : "right", view_bounds[eye][0], view_bounds[eye][1], view_bounds[eye][2], view_bounds[eye][3]);
        // spdlog::info("frustum is [l={}, r={}, t={}, b={}]", left, right, top, bottom);
        return Vector4f {left, right, top, bottom };
    };

    auto calc_projection_matrix = [&](Vector4f& frustum) {
        float sum_rl = (frustum[1] + frustum[0]);
        float sum_tb = (frustum[2] + frustum[3]);
        float inv_rl = (1.0f / (frustum[0] - frustum[1]));
        float inv_tb = (1.0f / (frustum[2] - frustum[3]));

        return Matrix4x4f {
            (2.0f * inv_rl), 0.0f, 0.0f, 0.0f,
            0.0f, (2.0f * inv_tb), 0.0f, 0.0f,
            (sum_rl * inv_rl), (sum_tb * inv_tb), nearz/(nearz-farz),               -1.0f,
            0.0f, 0.0f, (nearz*farz)/(farz-nearz),         0.0f
        };
    };
    // if we've not yet derived an eye projection matrix, or we've changed the projection, derive it here
    // Hacky way to check for an uninitialised eye matrix - is there something better, is this necessary?
//    if (this->should_recalculate_eye_projections || this->last_eye_matrix_nearz != nearz || this->projections[vr::Eye_Left][2][3] == 0) {
    this->hmd->GetProjectionRaw(vr::Eye_Left, &this->raw_projections[vr::Eye_Left][0], &this->raw_projections[vr::Eye_Left][1], &this->raw_projections[vr::Eye_Left][2], &this->raw_projections[vr::Eye_Left][3]);
    this->hmd->GetProjectionRaw(vr::Eye_Right, &this->raw_projections[vr::Eye_Right][0], &this->raw_projections[vr::Eye_Right][1], &this->raw_projections[vr::Eye_Right][2], &this->raw_projections[vr::Eye_Right][3]);

    this->frustums[vr::Eye_Left] = get_mat(vr::Eye_Left);
    this->frustums[vr::Eye_Right] = get_mat(vr::Eye_Right);
//    this->projections[vr::Eye_Left] = calc_projection_matrix(this->frustums[vr::Eye_Left]);
//    this->projections[vr::Eye_Right] = calc_projection_matrix(this->frustums[vr::Eye_Right]);
//        this->should_recalculate_eye_projections = false;
//        this->last_eye_matrix_nearz = nearz;
//    }
    // don't allow the eye matrices to be derived again until after the next frame sync
//    this->should_update_eye_matrices = false;
    this->ipd = glm::distance(this->eyes[0][3], this->eyes[1][3]);
    this->diagonal_fov = glm::degrees(2.0f * std::atan(std::sqrt(frustums[0][0]*frustums[0][0] + frustums[0][2]*frustums[0][2])));

    return VRRuntime::Error::SUCCESS;
}

void OpenVR::destroy() {
    if (this->loaded) {
        vr::VR_Shutdown();
    }
}
}
