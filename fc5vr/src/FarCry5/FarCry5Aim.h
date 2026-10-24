#pragma once

#include <d3d11.h>
#include <glm/glm.hpp>

// Where the barrel ray meets the scene: a one-pixel GPU march through the scene depth of the frame
// just drawn. The game fires from its camera, so the camera is aimed at that point and the shot lands
// where the barrel points, near or far. The answer arrives one or two frames later (non-blocking read).
namespace fc5::aim
{
    struct Ray
    {
        glm::vec3 origin_rel{ 0.0f };   // barrel origin minus the view's camera position (world axes, m)
        glm::vec3 dir{ 0, 1, 0 };       // world
        glm::mat4 view_rot_proj{ 1.0f }; // the drawn view's rotation-projection (camera-relative)
        float     depth_a{ 1e-5f }, depth_b{ 0.1f };  // metres = b / (z + a); z = 0 is the sky
        float     near_skip{ 0.0f };   // scene depths nearer than this (m) are the held gun, not the scene (sightaim: the sight line runs through the sight)
        float     start{ 0.3f }, stop{ 400.0f };   // the distances marched (m)
    };

    // PRESENT thread. depth: the scene depth snapshot (typeless 32-bit depth with an SRV binding).
    // chan: 0 the barrel ray, 1 meleeswing's hand-motion ray (each with its own result).
    void  march(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* depth, const Ray& ray, int chan = 0);
    // The newest result: distance along the ray in metres, or < 0 when nothing was hit (sky) or none yet.
    float last_hit(int chan = 0);
    // Results of marches dispatched after mark(): how many, and the nearest hit among them (< 0: none).
    void  mark(int chan);
    float hit_since_mark(int chan, int& results);
}
