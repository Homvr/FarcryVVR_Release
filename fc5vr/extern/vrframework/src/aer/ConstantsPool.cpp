#include "ConstantsPool.h"
#include "sl_consts.h"
#include <glm/ext/matrix_transform.hpp>
#include <mods/vr/runtimes/OpenXR.hpp>
#include <math/sl_matrix_helpers_copy.h>

namespace GlobalPool
{
    using namespace sl;
    Constants g_constants[CONSTANTS_HISTORY_SIZE]{};
    static glm::mat4 permutationRHToLH = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f));

    float4x4 rebuildOrthogonalLHRotationMatrix(const glm::mat4& cur_view) {
        float3 cameraRight = { cur_view[0].x, cur_view[0].y, cur_view[0].z };
        float3 cameraFwd    = { cur_view[2].x, cur_view[2].y, cur_view[2].z };
        float3 cameraPos  = { cur_view[3].x, cur_view[3].y, cur_view[3].z };
        float3 cameraUp;
        vectorNormalize(cameraRight);
        vectorNormalize(cameraFwd);
        vectorCrossProduct(cameraUp, cameraFwd, cameraRight);
        vectorNormalize(cameraUp);
        float4x4 cameraViewToWorld = {};
        cameraViewToWorld[0] = float4(cameraRight.x, cameraRight.y, cameraRight.z, 0.f);
        cameraViewToWorld[1] = float4(cameraUp.x,    cameraUp.y,    cameraUp.z,    0.f);
        cameraViewToWorld[2] = float4(cameraFwd.x,   cameraFwd.y,   cameraFwd.z,   0.f);
        cameraViewToWorld[3] = float4(cameraPos.x,   cameraPos.y,   cameraPos.z,   1.f);
        return cameraViewToWorld;
    }

    float4x4 getFinalViewToWorldMatrix(int frame) {
//        const glm::mat4 finalViewRH  = get_final_view(frame);
//        auto cur_view = permutationRHToLH * finalViewRH * permutationRHToLH;
//        auto cur_view = finalViewRH;
        return rebuildOrthogonalLHRotationMatrix(get_final_view(frame));
    }


    glm::mat4 get_correction_matrix(int frame, int past_frame) {
        //[Important] this call happening with same frame count as engine, however in real it is one off frame ( GOW )

        auto cameraViewToClip = *(float4x4*)&get_projection(frame);
        auto cameraViewToClipPrev = *(float4x4*)&get_projection(past_frame);
        auto cameraViewToWorld = getFinalViewToWorldMatrix(frame);
        auto cameraViewToWorldPrev = getFinalViewToWorldMatrix(past_frame);

        float4x4 clipToCameraView;
        matrixFullInvert(clipToCameraView, cameraViewToClip);

        float4x4 cameraViewToPrevCameraView;
        calcCameraToPrevCamera(cameraViewToPrevCameraView, cameraViewToWorld, cameraViewToWorldPrev);

        float4x4 clipToPrevCameraView;
        matrixMul(clipToPrevCameraView, clipToCameraView, cameraViewToPrevCameraView);
        glm::mat4 clipToPrevClip;
        matrixMul(*(float4x4*)&clipToPrevClip, clipToPrevCameraView, cameraViewToClipPrev);
        return clipToPrevClip;
    }


//    XrPosef calculate_xr_pose_compensation(int reference_frame, int current_frame)
//    {
//
//        //TODO we calculating CamerToWorld difference but may be we need to calculate HMD to World.
//        auto reference_pose = get_openxr_pose(reference_frame);
//        auto reference_camera_matrix = rebuildOrthogonalLHRotationMatrix(get_camera_view(reference_frame));
//        auto current_camera_matrix = rebuildOrthogonalLHRotationMatrix(get_camera_view(current_frame));
//        float4x4 reprojection_matrix;
//        calcCameraToPrevCamera(reprojection_matrix, current_camera_matrix, reference_camera_matrix);
//        auto glm_pose = glm::mat4_cast(runtimes::OpenXR::to_glm(reference_pose.orientation));
//        glm_pose[3] = glm::vec4(reference_pose.position.x, reference_pose.position.y, reference_pose.position.z, 1.0f);
//
//        float4x4 compensated_mat;
//        matrixMul(compensated_mat, reprojection_matrix, *(float4x4*)&glm_pose);
//        glm::mat4 final_mat = *(glm::mat4*)&compensated_mat;
//        XrPosef compensated_pose;
//        compensated_pose.position.x = final_mat[3].x;
//        compensated_pose.position.y = final_mat[3].y;
//        compensated_pose.position.z = final_mat[3].z;
//        auto compensated_orientation = glm::normalize(glm::quat_cast(final_mat));
//        compensated_pose.orientation = runtimes::OpenXR::to_openxr(compensated_orientation);
//        return compensated_pose;
//    }



    //    glm::mat4 XrPoseToMat4(const XrPosef& pose) {
    //        glm::quat q(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
    //        glm::vec3 p(pose.position.x, pose.position.y, pose.position.z);
    //
    //        glm::mat4 rotation = glm::toMat4(q);
    //        glm::mat4 translation = glm::translate(glm::mat4(1.0f), p);
    //
    //        return translation * rotation;
    //    }
    //
    //    // Convert glm::mat4 to XrPosef
    //    XrPosef Mat4ToXrPose(const glm::mat4& mat) {
    //        glm::vec3 scale;
    //        glm::quat rotation;
    //        glm::vec3 translation;
    //        glm::vec3 skew;
    //        glm::vec4 perspective;
    //
    //        // Decompose the matrix
    //        glm::decompose(mat, scale, rotation, translation, skew, perspective);
    //
    //        XrPosef result;
    //        result.orientation.x = rotation.x;
    //        result.orientation.y = rotation.y;
    //        result.orientation.z = rotation.z;
    //        result.orientation.w = rotation.w;
    //        result.position.x = translation.x;
    //        result.position.y = translation.y;
    //        result.position.z = translation.z;
    //        return result;
    //    }

}