#pragma once

// poseprobe (FarCry5PoseProbe.cpp, FC2 VR item 1 route B, 23 Oct): who reads which bone of which skeleton pose, through the game's
// lazy bone getter (FC_m64+0x7253310: rcx = pose, edx = bone; model transforms at [pose+0x10] + bone x 32).

#include <Windows.h>

#include <atomic>
#include <cstdint>

#include <glm/glm.hpp>

namespace fc5::poseprobe
{
    // The getter hooked (entry bytes checked) and N frames of calls recorded; the report follows a few frames later.
    bool start(int frames, const std::atomic<int>* frame);
    bool armed();
    // From the palette writer (FC_m64+0x6ac03f0, render thread): rcx = the instance's skin matrices, rdx = the mesh's bone remap.
    void note_skin(uintptr_t rcx, uintptr_t rdx);
    // A weapon-pass skinned draw: its index count and slot 1's object matrix (3x4 rows).
    void note_draw(unsigned count, const float* obj12);
    // Each present: the report when due. hand_world: the game's drawn weapon hand (the gloves' centre), when hand_ok.
    void present(int frame, unsigned long render_tid, const glm::vec3& hand_world, bool hand_ok, const glm::vec3& grip_world, bool grip_ok);   // grip: gunstill's grip point, when no hand
    bool install_getter(const std::atomic<int>* frame);   // the getter's hook (also skelhand's)
    float rot_angle_deg(const float* a, const float* b);   // two 3x3 rotations (9 floats each, any layout)
    float quat_angle_deg(const float* a, const float* b);  // two quaternions (any component order)
}

// skelhand 0|1|2 (FarCry5PoseProbe.cpp): the player's weapon hand (bone 194) or shoulder (191) moved in the game's own skeleton.
#include <string>
namespace fc5::skelhand
{
    bool        set_mode(int mode, const std::atomic<int>* frame);
    int         mode();
    // The render thread's model-space move for the next updates (D: the arms' root^-1 x the mod's placement x root).
    void        set_target(int frame, const glm::mat4& D, bool ok);
    bool        player_ok();
    std::string status();
    void        note_skin(uintptr_t rcx, uintptr_t rdx);   // from the palette writer: the player's pose checked against the arms' skin
    // skelhand 3: the gun's world matrix where the game sets it: the world-space move, the drawn gun (to learn its entity), a weapon change.
    void        set_world_target(int frame, const glm::mat4& Mw, bool ok);
    void        note_gun(int frame, const glm::mat4& Om);
    void        relearn_gun();
    void        set_arms(bool on);   // skelhandarms: both arms by two-bone IK onto the hands moved with the gun (mode 3)
    bool        arms();
    int         used_target_frame();
    int         drawn_target_frame();   // the placement frame of the gun matrix last matched by raw_gun (the drawn update)   // skelhandlate: the render frame whose placement the gun's last update was given
    // The game's own matrix (before the move) of the gun body drawn with this object matrix (row-major 3x4); false if not one we moved.
    bool        raw_gun(const float* drawn12, float* raw12);
    std::string gun_status();
    // throwarc's probe: from a throw's release (the throwing hand's world position), every entity the writer (+0x6a10660) sets near the
    // hand is followed for 2.5 s; throw_fit then gives the launch of the one that flew furthest (velocity at launch, gravity).
    void        throw_probe_arm(const glm::vec3& hand);
    struct ThrowFit { bool ok{ false }; glm::vec3 p0{ 0.0f }, v0{ 0.0f }; float g{ 0.0f }, t_launch{ 0.0f }, dist{ 0.0f }; int samples{ 0 }, tracked{ 0 }; std::string path; };
    ThrowFit    throw_fit();
}