#pragma once

// lampeye (interior lamps and their shadows per eye, Homard 25 Oct, bunker save): the tiled lighting passes take each lamp
// from LightsLightingData (posCS and clipPlanes relative to the GAME camera) and its shadow map projection from ShadowData
// (also relative to the game camera), but place each pixel relative to the eye's camera (InvViewRotProjectionTexture is the
// eye's). Every lamp, its falloff and its shadow were off by the eye's offset from the game camera, a different amount in
// each eye. For each eye call a copy of both buffers is made on the GPU with every position moved by that offset, and the
// lighting dispatches read the copies.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace fc5::locallights
{
    struct Saved
    {
        bool                      on{ false };
        UINT                      slot_l{ 255 }, slot_s{ 255 };
        ID3D11ShaderResourceView* old_l{ nullptr };
        ID3D11ShaderResourceView* old_s{ nullptr };
    };
    // The context calls the mod hooks, made past the hooks (the mod's own pass is not one of the game's).
    struct Raw
    {
        void (*set_cs)(ID3D11DeviceContext*, ID3D11ComputeShader*);
        void (*dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
        void (*update)(ID3D11DeviceContext*, ID3D11Resource*, const void*);
    };
    // Before a dispatch that reads LightsLightingData at slot_l (and ShadowData at slot_s, 255 = none): binds this eye's
    // moved copies. delta = eye camera minus game camera (world, metres); key = this eye call.
    Saved swap(ID3D11DeviceContext* ctx, const Raw& raw, UINT slot_l, UINT slot_s, const float delta[3], int64_t key);
    void  restore(ID3D11DeviceContext* ctx, Saved& s);
    void  stats(uint64_t& swaps, uint64_t& builds, uint64_t& refused);
    bool  enabled();
    bool  command(const std::string& cmd, float fv, bool on);   // lampeye on|off
}
