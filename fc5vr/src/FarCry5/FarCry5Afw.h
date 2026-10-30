#pragma once

#include <d3d11.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>

// AFW (alternate frame warping) for Far Cry 5: the game renders one eye per present, and the other
// (held) eye is rebuilt on the GPU from two sources before it is shown, as in Dishonored VR
// (docs: AFW_RENDERING_GUIDE.md):
//   the fresh eye   rendered this present, from the other viewpoint
//   the held eye    its own image from the previous present, from its own viewpoint
// Each source is carried into the held eye's view of NOW (the target) through its depth: a coarse
// depth-tested mesh first (seed map), then a per-pixel fixed-point refinement, then a decision ladder
// (world from the held eye where it is still valid, the player's weapon and hands and whatever moved
// from the fresh eye, the background extended over disocclusions).
//
// Far Cry 5 hands us every camera exactly (the mod writes them): each image records its eye's camera
// in the game world (rotation, position, FOV tangents) and its eye pose relative to the game camera.
// World points move by the world cameras (head, stick turns and walking alike); the player's own
// weapon and arms (nearer than the foreground limit) are fixed to the game camera, so they move by
// the eye poses relative to it.
namespace fc5::afw
{
    struct Camera
    {
        bool      valid{ false };
        int       frame{ -1 };
        glm::mat3 rot{ 1.0f };   // world -> eye view (right-handed, -Z forward)
        glm::vec3 pos{ 0.0f };   // eye position, world (metres)
        glm::vec4 tan{ -1, 1, 1, -1 }; // tangents: left, right, up, down
        glm::mat4 rel{ 1.0f };   // the eye in the game camera's view space (rotation + offset)
        // afwvehbody: in a vehicle, the vehicle's own raw frame at this image (world -> its axes, the game camera as its
        // origin), not the eyes' smoothed one (vehsmooth filters the bob out of the eyes but not out of the cab).
        bool      veh_valid{ false };
        glm::mat3 veh_rot{ 1.0f };
        glm::vec3 veh_pos{ 0.0f };
        // afwvehown: the vehicle model's own transform at this image (world -> its axes, its origin), when it was found: the
        // cab is fixed in the car, while the game camera springs inside it (launch, bumps).
        bool      veh_mesh{ false };
        glm::mat3 mesh_rot{ 1.0f };
        glm::vec3 mesh_pos{ 0.0f };
    };

    struct Stats
    {
        uint32_t rebuilds{ 0 }, refused{ 0 }, captures{ 0 }, depth_missing{ 0 };
        uint32_t fp_rebuilds{ 0 };   // afwfpeye: rebuilds that took the held eye's weapon from its own weapon pass
        float    gpu_ms{ 0.0f };
        float    last_error{ -1.0f };   // mean |rebuilt(N) - native(N+1)| of the same eye, 0..1 (flat test)
        float    last_error_heldcopy{ -1.0f }; // same, for the held image shown as it is (the control)
        std::string last_why{};
    };

    // Compiles the AFW shaders on worker threads, started when the mod loads (before the game's first
    // frame), so initialize() only creates them: D3DCompile of the six took ~4 s on the render thread.
    void precompile();
    bool initialize(ID3D11Device* device);
    bool ready();

    // PRESENT thread. Copies this present's game image and scene depth for `eye` (0 left, 1 right),
    // with the camera it was rendered from. depth: the game's scene depth (reversed Z, B/(z+A) metres).
    bool capture(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* color, ID3D11Texture2D* depth, const Camera& cam, float depth_a, float depth_b);

    // PRESENT thread. Rebuilds the held eye at `target` (the held eye's camera at the fresh image's
    // instant) into the module's output texture (the colour format and size) and returns it; nullptr
    // with the reason when it cannot (the caller shows the held image as it is).
    // out_format: DXGI_FORMAT_UNKNOWN = the captured colour's format.
    ID3D11Texture2D* rebuild(ID3D11DeviceContext* ctx, int held, const Camera& target, const char** why, DXGI_FORMAT out_format = DXGI_FORMAT_UNKNOWN);

    // Copies the eye's last captured image (the rotation-only fallback).
    bool copy_last(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* dst);
    ID3D11Texture2D* last_image(int eye);
    const Camera&    last_camera(int eye);

    // Flat test: keeps the last rebuild and, at the next capture of that eye, measures how far it is
    // from the native image (and the plain held copy, as the control).
    void set_measure(bool on);
    void request_dump(int n);   // the next n measured rebuilds to BMP (rebuilt, native truth, held as it is)
    void set_debug_tint(bool on);
    void set_foreground_limit(float metres);
    // The weapon and hands told by these stencil bits of the scene depth (the weapon pass writes 64); 0 = by the depth limit.
    void set_foreground_stencil(int bits);
    int  foreground_stencil();
    void set_foreground_push(float metres);   // afwfgpush: the weapon pass pushes its vertices this far along the view ray (c52.z); taken back
    void set_gun_own(bool on);   // afwgunown: the rebuilt eye colours its gun from its own last image first (else the other eye's)
    bool gun_own();
    void set_held_gun(bool on);   // afwheldgun (research): the held eye's own gun allowed in the fallback (the doubled gun of 13 Oct)

    // afwfpeye: the held eye's own first-person pass (hands, weapon), drawn by the mod with the held eye's camera into
    // a depth image (ss x ss texels per eye pixel). Where it has the weapon, the rebuild shows the weapon there, with
    // the colour of the same gun point in the fresh image (or in the held eye's last image); everywhere else only the
    // world is rebuilt (the fresh eye's gun is never moved into the held eye). srv nullptr = off (the old rebuild).
    struct FpInput
    {
        ID3D11ShaderResourceView* srv{ nullptr };   // R32F, the weapon pass's own depth (clear = no weapon)
        uint32_t                  w{ 0 }, h{ 0 };
        int                       ss{ 1 };
        float                     clear{ 0.0f };
        float                     pa{ 0.0f }, pb{ 0.0f };   // metres = pb / (z + pa)
        float                     push{ 0.0f };             // the weapon pass's depth push (m), taken back
        glm::vec4                 tan{};                    // its view tangents (left, right, up, down)
        glm::mat3                 to_fresh{ 1.0f };         // held view -> fresh view (through the gun's own space)
        glm::vec3                 to_fresh_o{ 0.0f };
        bool                      fresh_ok{ false };
        glm::mat3                 to_held{ 1.0f };          // held view now -> the held eye's last image's view
        glm::vec3                 to_held_o{ 0.0f };
        bool                      held_ok{ false };
    };
    void set_fp_input(const FpInput& in);
    // afwvehnear: in a vehicle the cabin and body move with the player; nearer than this (m) only the fresh image is used
    // (the held eye's previous image put them a frame of driving behind: a doubled vehicle). 0 = off.
    void set_vehicle_near(float metres);
    // afwvehbody: in a vehicle the eyes hang on the vehicle's frame (attitude, seat at the game camera); nearer than this (m)
    // both images are carried with it (the cab is fixed there), so the held eye's own cab is used again. 0 = off.
    void set_vehicle_body(float metres);
    // Experiments from the BioShock VR mod's rebuild (24 Oct; F11 Experimental > AFW), bits: 1 afwfoot (a refined point whose
    // remaining step crosses a depth break is refused), 2 afwrail (thin near things retried from the nearest texel beside the seed);
    // 4 afwfoliage (29 Oct): a held point in a porous patch (needles, leaves, grass) is not taken for moved while the fresh eye
    // has a speck of the plant within 6 texels.
    void set_experiments(int bits);
    void set_enabled(bool on);
    bool enabled();
    float foreground_limit();
    bool  debug_tint();

    // A colour texture to a 24-bit BMP (diagnostics).
    void save_image(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& path);
    Stats& stats();
    void   shutdown();
}

// Stereo pair check: the two eye images the headset shows at one moment, shrunk on the GPU, read back
// without stalling and compared on a worker thread (block matching in each eye's own view angles).
namespace fc5::pair
{
    struct Result
    {
        bool  valid{ false };
        int   frame{ -1 };
        float mean_abs{ 0.0f };       // mean |L - R| of the shrunk luma, 0..1 (not aligned)
        float disp_med{ 0.0f }, disp_p10{ 0.0f }, disp_p90{ 0.0f };   // horizontal disparity, milliradians of view tangent (left minus right)
        float neg_frac{ 0.0f };       // share of tiles with clearly negative disparity (swapped eyes)
        float vert_med{ 0.0f }, vert_p90{ 0.0f };                    // vertical offset (mrad), median and |P90|
        int   tiles{ 0 };
        float change[2]{ -1.0f, -1.0f };   // each eye's mean |change| since its previous checked image
        const char* verdict{ "none" };
    };
    // eye 0/1 image with its view tangents (left, right, up, down); the pair goes to the GPU when both are in.
    void capture(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* image, const float tan[4], int frame);
    void poll(ID3D11DeviceContext* ctx);   // each present: collects finished readbacks, starts the worker
    Result last();
}