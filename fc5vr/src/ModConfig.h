#pragma once
//
// #include <glm/fwd.hpp>
// #include <glm/vec3.hpp>
// #include <glm/mat4x4.hpp>
// #include <ne/models/AW2GameFlow.h>
//
// namespace ModSettings
// {
//     struct HudScale
//     {
//         float x{ 1.0 };
//         float y{ 1.0 };
//         float z{ 25.0 };
//         float w{ 1.0 };
//     };
//
//     static_assert(offsetof(HudScale, y) == 4, "HudScale::y offset");
//     extern HudScale g_hudScale;
//
//     struct CrosshairTranslate
//     {
//         float x{ 50.0 };
//         float y{ 50.0 };
//     };
//
//     static_assert(offsetof(CrosshairTranslate, y) == 4, "Crosshair::y offset");
//
//     struct DebugAndCalibration
//     {
//         bool      calibrateJoystick{ false };
//         float     staticYaw{ 0.0 };
//         float     staticPitch{ 0.0 };
//         glm::vec3 cumulative{};
//         int       counter{ 0 };
//         std::vector<void*> debugActors{};
//         std::vector<glm::mat4> debugVectors{};
//         std::vector<uint64_t> debugVectorsID{};
//     };
//
//     extern DebugAndCalibration g_debugAndCalibration;
//
//     extern CrosshairTranslate g_crosshairTranslate;
//     extern AW2GameFlow        g_game_state;
//
//     enum AimCameraMode : unsigned int
//     {
//         Aim_CameraFirstPerson = 0,
//         Aim_CameraThirdPerson = 1,
//     };
//
//     enum AimControlMode : unsigned int
//     {
//         Aim_ControlHead = 0,
//         Aim_ControlMotion = 1,
//     };
//
//     struct AW2InternalSettings
//     {
//         float     motionControlSensitivityH{ 57.2958 / 2.0f };
//         float     motionControlSensitivityV{ 57.2958 / 2.0f };
//         bool      leftHandedControls{ false };
//         bool      motionVectorFix{ true };
//         bool      decoupledPitch{false};
//         int       cameraMode{ Aim_CameraThirdPerson };
//         int       aimCameraMode{ Aim_CameraFirstPerson };
//         int       aimControlMode{ Aim_ControlMotion };
//         glm::vec3 cameraOffset{0.2f, 1.65f, 0.2f};
//     };
//
//     extern AW2InternalSettings g_aw2_settings;
// } // namespace ModSettings
