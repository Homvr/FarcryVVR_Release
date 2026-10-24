#pragma once

// Safety check for shadow sharing (pipeline review P6, user 2026-10-10: sharing gave black rogue shadows in the
// headset). On check pairs the right eye's call draws its own shadow map; the GPU counts the texels that differ
// from the left call's map (copied between the calls). Results come back a few pairs later without waiting.

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace fc5::shadowshare
{
    // Between the two calls of a check pair: keep the left call's map.
    bool stash(ID3D11DeviceContext* ctx, ID3D11Resource* target);
    // After the right call of that pair: count texels that differ from the stash (queued, read later).
    void compare(ID3D11DeviceContext* ctx, ID3D11Resource* target, uint32_t pair_id);
    // A finished comparison, if any: differing texels and the total compared. False when nothing new.
    bool result(ID3D11DeviceContext* ctx, uint32_t& pair_id, uint64_t& differing, uint64_t& total);

    // Research (cmd cascadelog N, low-sun rogue shadows 2026-10-11): for the next N frames, every constant buffer upload
    // that carries the sun's cascade matrices (CSMSlice*, ShadowMapSize) or the far-shadow (ASM) resolve values is
    // compared with the same eye's previous one; changes are logged next to the game camera (view matrix rows).
    void cascade_log_start(int frames);
    bool cascade_logging();
    void cascade_note(int frame, int call, uint32_t size, const float* d, const float* game_view);

    // Research (cmd giplanelog N): the indoor-lighting volume planes (CGIIndoorVolumesProvider, game camera view space)
    // taken to world space with the recorded game camera, compared with the same eye's previous frame, with that
    // frame's camera (built one camera late?) and with the other eye. A static building's planes must not move.
    void giplane_log_start(int frames);
    bool giplane_logging();
    void giplane_note(int frame, int call, const float* planes, int n, const float* game_view, bool fresh);
}

namespace fc5::shadowshare
{
    // The shadow research commands (cascadelog N, giplanelog N) (true when cmd was one of them).
    bool command(const std::string& cmd, float fv, bool on);
    // Research: csmhook [N] (hook in the sun-shadow render setup, logs N view blocks), csmwatch K.
    bool csm_command(const std::string& cmd, float fv, unsigned long render_tid);
    // shadowfitlog N, shadowfitscale S: the hook on the game's cascade fit (see FarCry5ShadowShare.cpp).
    bool fit_command(const std::string& cmd, float fv);
    bool fit_install();
    // shadowfit on|off (default on): the cascade fit widened to both eyes' view (+ 3 deg);
    // shadowfitmin <deg> test override. fit_set_need: the culling's half-angle tangents, each VR frame.
    void  fit_set_need(float tan_h, float tan_v);
    float fit_widen(float in, float aspect);
    void  fit_stats(uint64_t& calls, uint64_t& widened, float& in, float& out);
    bool  fit_command2(const std::string& cmd, float fv, bool on);
    bool  fit_enabled();
}
