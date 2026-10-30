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
}

// throwarc's probe (FarCry5PoseProbe.cpp): from a throw's release (the throwing hand's world position), every entity the game's writer
// (+0x6a10660) sets near the hand is followed for 2.5 s; fit then gives the launch of the one that flew furthest (velocity at launch, gravity).
#include <string>
namespace fc5::throwprobe
{
    void        arm(const glm::vec3& hand);
    struct ThrowFit { bool ok{ false }; glm::vec3 p0{ 0.0f }, v0{ 0.0f }; float g{ 0.0f }, t_launch{ 0.0f }, dist{ 0.0f }; int samples{ 0 }, tracked{ 0 }, rejected{ 0 }; std::string path; };
    ThrowFit    fit(const glm::vec3& head_dir);   // head_dir: the head's direction at the release (world)
}
