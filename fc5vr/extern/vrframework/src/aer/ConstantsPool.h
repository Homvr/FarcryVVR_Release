#pragma once
#include <glm/ext/matrix_float4x4.hpp>
#include <openxr/openxr.h>
#include <openvr.h>

namespace GlobalPool
{
    static const int CONSTANTS_HISTORY_SIZE = 10;

    struct Constants
    {
        glm::mat4     projection{ 1.0 };
        glm::mat4     finalView{ 1.0f };
//        glm::mat4     cameraView{1.0f};
        struct OpenXR {
            XrPosef pose{};
            XrFovf  fov{};
        } openxr;

        struct OpenVR {
            vr::HmdMatrix34_t pose{};
        } openvr;

        struct NativeStereo {
            XrPosef pose[2]{};
            XrFovf  fov[2]{};
        } native_stereo;
    };

    extern Constants g_constants[CONSTANTS_HISTORY_SIZE];

    glm::mat4 get_correction_matrix(int frame, int past_frame);

    inline void submit_projection(glm::mat4& projection, int frame)
    {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].projection = projection;
    }

    inline void submit_final_view(glm::mat4& finalView, int frame)
    {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].finalView = finalView;
    }
//
//    inline void submit_camera_view(glm::mat4& cameraView, int frame)
//    {
//        g_constants[frame % CONSTANTS_HISTORY_SIZE].cameraView = cameraView;
//    }
//
//    inline const auto& get_camera_view(int frame)
//    {
//        return g_constants[frame % CONSTANTS_HISTORY_SIZE].cameraView;
//    }

    inline auto& get_projection(int frame)
    {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].projection;
    }

    inline const auto& get_final_view(int frame)
    {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].finalView;
    }

    inline void submit_openxr_pose(XrPosef& pose, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.pose = pose;
    }

    inline const auto& get_openxr_pose(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.pose;
    }

    inline void submit_openxr_fov(XrFovf& fov, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.fov = fov;
    }

    inline const auto& get_openxr_fov(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr.fov;
    }

    inline const auto& get_xr_constants(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openxr;
    }

    inline void submit_native_stereo_view(int eye, const XrPosef& pose, const XrFovf& fov, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].native_stereo.pose[eye] = pose;
        g_constants[frame % CONSTANTS_HISTORY_SIZE].native_stereo.fov[eye] = fov;
    }

    inline const auto& get_native_stereo_views(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].native_stereo;
    }

    inline void submit_openvr_pose(const vr::HmdMatrix34_t& pose, int frame) {
        g_constants[frame % CONSTANTS_HISTORY_SIZE].openvr.pose = pose;
    }

    inline const auto& get_openvr_pose(int frame) {
        return g_constants[frame % CONSTANTS_HISTORY_SIZE].openvr.pose;
    }

//    XrPosef calculate_xr_pose_compensation(int reference_frame, int current_frame);
}; // namespace GlobalPool
