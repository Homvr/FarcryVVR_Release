#pragma once

#include <d3d11.h>

#include <cstdint>

// Scope lens (research\fc5_scopes_report.md 6.1, steps S1-S2): drawn into one finished eye image after DLSS.
// Inside the lens circle a magnified crop of that same image around the aim point (source C of the report), a
// black tube ring around it, an optional reticle at the lens centre and a dark crescent when the eye is off the
// scope's axis. All in pixels of the eye image.
namespace fc5::scope
{
    struct Lens
    {
        float lens_c[2]{};    // lens centre
        float lens_r{ 0 };    // lens radius
        float ring_r{ 0 };    // outer radius of the black ring
        float src_c[2]{};     // the crop's centre in the image (the aim point)
        float inv_mag{ 1 };   // 1 / magnification
        float reticle_w{ 0 }; // reticle line half-width (0 = no reticle)
        float vig_c[2]{};     // centre of the visible circle (eyebox)
        float vig_r{ 1e9f };  // its radius (huge = no crescent)
        float vig_soft{ 1 };  // its soft edge width
        // Step 3 (scopepass): the scope call's picture instead of the zoom (null = the zoom). A lens pixel at offset o
        // from the lens centre samples it at uv 0.5 + o * scope_k (its centre is the aim point it was drawn for).
        ID3D11ShaderResourceView* scope_srv{ nullptr };
        float scope_k[2]{};
        bool  scope_own{ false };      // scopesrc 1 without a DLSS image: the eye image itself is the scope's view
        bool  outside_black{ false };  // scopesrc 1: everything outside the tube ring black
    };

    // RENDER thread, at present. eye: a texture with UAV binding (the DLSS eye image). False if it cannot draw.
    bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, const Lens& lens);

    // scopemodel (Homard 2026-10-12: "I want them to appear on the model of the weapon, not as a separate screen"): the
    // scope picture on the gun's eyepiece, a disc fixed to the gun facing back along the scope's axis. The picture is laid
    // on the disc, centred on it with the reticle at its centre (Homard 12:55: "not glued to the scope" when it was a
    // circle round the eye's line of sight), and shaded from one side when the eye is off the scope's axis.
    struct ModelLens
    {
        float m[3][4]{};                 // the disc's right and up axes (world); eye box circle centre x, y, radius, soft edge (disc radii)
        float b[3][4]{};                 // scope tangent (x, y, 1) -> eye clip (x, y, w), rows (zoom without the pass picture)
        float a[3][4]{};                 // eye ndc (x, y, 1) -> world direction from the eye, rows
        float disc_c[4]{};               // the disc's centre (world offset from the eye), its radius (m)
        float disc_n[4]{};               // its normal (towards the eye), its soft edge (m)
        float box[4]{};                  // x0 y0 x1 y1, eye pixels
        float k[2]{};                    // pass picture uv = 0.5 + tangent * k (y flipped)
        float inv_mag{ 1 };
        float r_t{ 0.25f };              // the field at the disc's edge (scene tangent before the magnification)
        float fade{ 1 };                 // 1 = full, 0 = off (the gun angled away from the eye)
        float ret_t{ 0 };                // reticle half-width, disc radii (0 = none)
        bool  eyebox{ true };
        bool  debug{ false };            // scopelensdebug: the disc magenta, its centre a green cross
        ID3D11ShaderResourceView* scope_srv{ nullptr };   // the pass picture (null = zoom of the eye image)
        ID3D11Texture2D*          zoom_src{ nullptr };    // the zoom's source instead of the eye image (AFW: the other eye's last image; b maps into it)
        // The scope's glass as the game drew it this frame (glow draws' mask, render size) and its stats (glass_stats);
        // with too few pixels (or none) the disc fixed to the gun is used.
        ID3D11ShaderResourceView* mask{ nullptr };
        ID3D11ShaderResourceView* stats{ nullptr };
        float mscale[2]{}, moff[2]{};    // eye pixel -> mask (render) pixel
        float msize[2]{};                // the mask's size
        float roll[2]{ 1, 0 };           // the gun's roll on screen (cos, sin)
        float tpp{ 0 };                  // eye tangent per eye pixel
        float min_px{ 200 };             // fewest glass pixels to use it
    };
    bool draw_model(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, const ModelLens& lens);
    // The glass mask (render size of the target the glow draws go to), the pixel shader that writes it, and its stats:
    // pixel count and centre, computed on the GPU (no readback; cleared when run_it is false). Returns the stats view.
    bool glass_mask(ID3D11Device* dev, UINT w, UINT h, ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv);
    ID3D11PixelShader* mask_ps(ID3D11Device* dev);
    ID3D11ShaderResourceView* glass_stats(ID3D11DeviceContext* ctx, bool run_it);
    // The stats to the CPU, no waiting: glass_readback after glass_stats (tagged with the eye frame), glass_result gives the
    // oldest copy that is ready (count, sum x, sum y in render pixels) and its frame.
    void glass_readback(ID3D11DeviceContext* ctx, int frame);
    bool glass_result(ID3D11DeviceContext* ctx, int& frame, uint32_t out[3]);

    // Step 3 helpers (render thread). The scope call's present goes to a stub while muted (no swap chain present, no
    // OpenXR frame, no frame advance); mute() returns false if the swap chain's Present slot cannot be changed.
    bool mute_present(IDXGISwapChain* swap);
    int  unmute_present();   // presents the game made while muted
    // The game's history textures (found by the per-eye history code): saved before the scope call and put back after
    // it; the scope keeps its own copies (loaded before its call when load_scope).
    void hist_note(ID3D11Resource* res);
    void hist_before(ID3D11DeviceContext* ctx, bool load_scope);
    // scopehistswap: the texture each history slot binds in the eyes' calls (key: shader, slot and order in the call),
    // per eye. The game alternates most histories between two textures each call; the scope call is one call more, so
    // after it the next left call binds the texture the right call bound. With swap_pairs those pairs swap contents
    // after the call (returns how many), so each eye's call finds what it would have found without the scope call.
    void hist_role(uint64_t key, ID3D11Resource* res, int frame);
    int  hist_after(ID3D11DeviceContext* ctx, int right_frame, bool swap_pairs);
    size_t hist_count();
    bool hist_has(ID3D11Resource* res);   // saved and put back around the scope call
    void hist_range(int lo, int hi);
    void hist_list();
    // The scope call's picture (after SMAA, before the HUD), and its view. corner > 0: only the top-left corner x corner.
    bool capture(ID3D11DeviceContext* ctx, ID3D11Resource* src, UINT corner = 0);
    ID3D11ShaderResourceView* picture();
    // AFW (scopeothereye): each eye's image before the lens and the dots, its frame and rotation-projection (FarCry5Scope.cpp).
    extern ID3D11Texture2D* g_clean[2];
    extern int              g_clean_frame[2];
}
