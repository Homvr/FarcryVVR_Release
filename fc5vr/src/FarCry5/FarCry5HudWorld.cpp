// The HUD draws that belong to the world, put into each eye's picture (Homard 18 Oct: "the black elements are the
// directional hit", "improve in World pointers so that they are actually usable").
//  - hudmul: the hit indicators multiply the picture (colour = dst x src). Drawn on their own white image (hud_begin), that
//    image multiplies this eye's picture where the HUD panel hangs, as the game's draw multiplied the screen.
//  - hudmarkers: the world markers (objective icons, their distance, edge arrows) are placed by the game for its own camera
//    (120 deg wide, following the gun). Each eye pixel looks up its own direction in that camera's picture of the markers,
//    so a marker sits on its target in both eyes, shown at hudmarkdist metres.
//  - hudmarksize (Homard 18 Oct 21:13: "indicators where in the right place, but they were way too big"): each marker
//    scaled about its own icon. Every marker draw is drawn once more beforehand into a box of its own (the pixels it
//    covers). An icon starts a marker; a text draw (the distance) joins the marker just drawn when it touches its icon.
//    The marker is scaled about its icon's centre (the point the game puts on the target).
//  - hudmarksmooth (Homard 19 Oct: "they jump around a lot ... make them more stable when we move around"): two causes.
//    Boxes used to join any marker they touched (a text next to another marker's icon, chains through the joined box), so
//    with many markers and a small size a marker was scaled about another marker's icon whenever they came close, and
//    jumped as that changed. And each call's markers are placed by the game for its camera, which follows the gun (hand
//    tremor) and may be a call behind the one they are drawn with. Each marker's icon is now followed as a direction in
//    the world from call to call (matched to the nearest one of the last call), smoothed over hudmarksmooth seconds
//    (faster for big moves, snapped over 6 deg), and the marker is drawn there.
//  - Control hints (Homard 20 Oct: "the control instruction when getting in vehicles were considered as in world hud"): the
//    game draws them before the compass bar too, as a widget that starts with a shape (a HUD shader reading no texture, which
//    markers never use; hud_begin). The widget's draws, and a text draw not under a marker's icon, form screen-fixed groups:
//    put back into the HUD panel image pixel for pixel (kPanel; Homard 23 Oct 10:33: drawn into each eye picture where the
//    panel hangs, every second button icon of the truck's command list was missing). With DLSS upscaling (the panel composed
//    into the eye picture before this pass) they are still drawn into the eye picture where the panel shows them.
#include "FarCry5Stereo.h"
#include "FarCry5Record.h"
#include "FarCry5Dlss.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

namespace
{
    constexpr UINT kSlots = 64;   // marker draws per call with a box; more: drawn unscaled

    const char* kHudWorld = R"(
Texture2D<float4>         g_src    : register(t0);   // this eye's picture
Texture2D<float4>         g_mul    : register(t1);   // multiplying HUD draws, white = no change
Texture2D<float4>         g_mark   : register(t2);   // world markers, premultiplied
StructuredBuffer<float4>  g_groups : register(t3);   // [0].x = markers; per marker: box (x0, y0, x1, y1), anchor (x, y), shown at (x, y), in marker pixels
SamplerState              g_lin    : register(s0);
RWTexture2D<float4>       g_dst    : register(u0);
cbuffer P : register(b0) {
    uint   w; uint h; uint mul_on; uint mark_on;
    float4 tan;      // this eye's picture edges as tangents: left, right, up, down
    float4 eye;      // eye position from the head centre in head axes (m), xyz; w = the panel's distance (m)
    float4 panel;    // panel half size as tangents from the head centre (x, y), its centre height (tangent), hints already in the panel image (w)
    float4 moff;     // markers: NDC shift for their distance (x, y), the NDC depth used (z), size (w; 1 = the game's)
    float4 m[4];     // markers: rows of this eye's NDC -> the game camera's clip
    float4 msize;    // the marker image's size in pixels (x, y), 1 / scale (z), scaled (w)
};
float4 mark_at(float2 px)
{
    // A 2x2 box around the point (the markers are shrunk: about one marker pixel per eye pixel at 0.5).
    float2 d = 0.25f * msize.z / msize.xy;
    float2 c = px / msize.xy;
    return 0.25f * (g_mark.SampleLevel(g_lin, c + float2(-d.x, -d.y), 0) + g_mark.SampleLevel(g_lin, c + float2(d.x, -d.y), 0) +
                    g_mark.SampleLevel(g_lin, c + float2(-d.x, d.y), 0) + g_mark.SampleLevel(g_lin, c + float2(d.x, d.y), 0));
}
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float4 c  = g_src.Load(int3(p, 0));
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    // The HUD panel: this eye's ray through the pixel meets the panel's plane z = -eye.w (as the panel layer hangs).
    float2 t   = float2(lerp(tan.x, tan.y, uv.x), lerp(tan.z, tan.w, uv.y));
    float2 s   = (eye.xy + t * (eye.w + eye.z)) / eye.w;
    float2 huv = float2(0.5f + s.x / (2.0f * panel.x), 0.5f - (s.y - panel.z) / (2.0f * panel.y));
    if (mul_on != 0 && all(huv > 0.0f) && all(huv < 1.0f)) c.rgb *= g_mul.SampleLevel(g_lin, huv, 0).rgb;
    if (mark_on != 0) {
        float4 e = float4(uv.x * 2.0f - 1.0f + moff.x, 1.0f - uv.y * 2.0f + moff.y, moff.z, 1.0f);
        float4 g = float4(dot(m[0], e), dot(m[1], e), dot(m[2], e), dot(m[3], e));
        bool   on_panel = all(huv > 0.0f) && all(huv < 1.0f);
        float4 k = 0.0f;
        bool   hint = false;
        if (msize.w == 2.0f) {
            // More marker-phase draws than boxes this call (a long prompt list): all of it shown on the panel, as before
            // the markers came off it, rather than prompts put in the world (already in the panel image when panel.w is 1).
            if (on_panel && panel.w == 0.0f) k = g_mark.SampleLevel(g_lin, huv, 0);
            hint = true;
        } else if (msize.w == 1.0f && on_panel && panel.w == 0.0f) {
            // A screen-fixed group (a control hint: text, or an icon that is no marker's) is shown where the panel shows that
            // part of the HUD, at its own size, over any marker, wherever the game camera looks (not only in front of it).
            float2 hp  = huv * msize.xy;
            uint   cnt = (uint)g_groups[0].x;
            for (uint j = 0; j < cnt; ++j) {
                float4 b = g_groups[1 + 3 * j];
                if (g_groups[2 + 3 * j].z != 0.0f && all(hp >= b.xy) && all(hp <= b.zw)) { k = g_mark.SampleLevel(g_lin, huv, 0); hint = true; break; }
            }
        }
        if (!hint && g.w > 1e-6f) {
            float2 n   = g.xy / g.w;
            float2 muv = float2(n.x * 0.5f + 0.5f, 0.5f - n.y * 0.5f);
            if (msize.w == 0.0f) {
                if (all(muv > 0.0f) && all(muv < 1.0f)) k = g_mark.SampleLevel(g_lin, muv, 0);
            } else {
                // Each marker scaled about its anchor and shown at its smoothed place: the point shown here comes from
                // anchor + (here - shown) / size.
                float2 gp  = muv * msize.xy;
                uint   cnt = (uint)g_groups[0].x;
                for (uint j = 0; j < cnt; ++j) {
                    float4 b  = g_groups[1 + 3 * j];
                    float4 an = g_groups[2 + 3 * j];
                    if (an.z != 0.0f) continue;
                    float2 sh = g_groups[3 + 3 * j].xy;
                    float2 sp = an.xy + (gp - sh) * msize.z;
                    if (all(sp >= b.xy) && all(sp <= b.zw)) { k = mark_at(sp); break; }
                }
            }
        }
        c.rgb = k.rgb + c.rgb * (1.0f - saturate(k.a));
    }
    g_dst[p] = c;
}
)";

    // The hints put back into the HUD panel image itself (the panel layer), pixel for pixel, as if they had never left it:
    // the screen-fixed groups' pixels of the marker image over the panel (both premultiplied). all_on: every pixel of the
    // marker image (more draws than boxes). Homard 23 Oct 10:33 F6 (truck command list): drawn into each eye picture
    // instead, every second button icon was missing; with hudmarkers off (on the panel) all were there.
    const char* kPanel = R"(
Texture2D<float4>         g_src    : register(t0);   // the panel image (a copy)
Texture2D<float4>         g_mark   : register(t1);
StructuredBuffer<float4>  g_groups : register(t2);
RWTexture2D<float4>       g_dst    : register(u0);
cbuffer P : register(b0) { uint w; uint h; uint all_on; uint pad; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float4 c  = g_src.Load(int3(p, 0));
    float2 hp = float2(p) + 0.5f;
    bool   on = all_on != 0;
    if (!on) {
        uint cnt = (uint)g_groups[0].x;
        for (uint j = 0; j < cnt; ++j) {
            float4 b = g_groups[1 + 3 * j];
            if (g_groups[2 + 3 * j].z != 0.0f && all(hp >= b.xy) && all(hp <= b.zw)) { on = true; break; }
        }
    }
    if (on) {
        float4 k = g_mark.Load(int3(p, 0));
        c = k + c * (1.0f - saturate(k.a));
    }
    g_dst[p] = c;
}
)";

    // One marker draw's box: the pixels it covers (every 4th row and column, few atomics), as max x, max y, max (65535 - x),
    // max (65535 - y): all zero = nothing drawn.
    const char* kBox = R"(
RWBuffer<uint> g_box : register(u0);
void main(float4 pos : SV_Position)
{
    uint x = (uint)pos.x, y = (uint)pos.y;
    if ((x & 3) != 0 || (y & 3) != 0) return;
    InterlockedMax(g_box[0], x);
    InterlockedMax(g_box[1], y);
    InterlockedMax(g_box[2], 65535 - x);
    InterlockedMax(g_box[3], 65535 - y);
}
)";

    // The boxes of this call joined into markers: an icon (any draw that is not text) starts a marker, a text draw joins the
    // marker just started when it touches that marker's icon (within the margin), else starts its own. A marker's anchor is
    // its first box's centre (the icon). hudmarksmooth: each anchor is followed as a world direction (camera-relative,
    // rotation only) from call to call, matched to the nearest followed one within the snap angle, eased toward the new
    // direction over tau seconds (faster for bigger moves), and the marker is shown where that direction is now.
    const char* kGroups = R"(
Buffer<uint>                g_box   : register(t0);
RWStructuredBuffer<float4>  g_out   : register(u0);
RWStructuredBuffer<float4>  g_state : register(u1);   // 64 followed directions (xyz), calls since last matched (w; 1000 = free)
cbuffer P : register(b0) {
    uint n; float margin; uint smooth_on; uint pad0;
    float dt; float tau; float snap_cos; float follow;   // seconds, seconds, cos(snap angle), the move (rad) followed at once
    float2 msize; float2 pad;
    uint4 masks;    // slots drawn from the glyph atlas (x: 0-31, y: 32-63), slots whose icon is no marker's (z, w)
    float4 G[4];    // camera-relative position -> the game camera's clip (rows)
    float4 Gi[4];   // its inverse
};
bool bit(uint2 m, uint i) { return ((i < 32 ? m.x >> i : m.y >> (i - 32)) & 1u) != 0; }
float3 dir_of(float2 px)
{
    float4 v = float4(px.x / msize.x * 2.0f - 1.0f, 1.0f - px.y / msize.y * 2.0f, 0.5f, 1.0f);
    float4 q = float4(dot(Gi[0], v), dot(Gi[1], v), dot(Gi[2], v), dot(Gi[3], v));
    return normalize(q.xyz / q.w);
}
bool px_of(float3 d, out float2 px)
{
    float4 v = float4(d * 10.0f, 1.0f);
    float4 c = float4(dot(G[0], v), dot(G[1], v), dot(G[2], v), dot(G[3], v));
    px = 0.0f;
    if (c.w <= 1e-6f) return false;
    float2 ndc = c.xy / c.w;
    px = float2((ndc.x * 0.5f + 0.5f) * msize.x, (0.5f - ndc.y * 0.5f) * msize.y);
    return true;
}
bool taken_at(uint2 m, uint s) { return bit(m, s); }
void take(inout uint2 m, uint s) { if (s < 32) m.x |= 1u << s; else m.y |= 1u << (s - 32); }
[numthreads(1, 1, 1)]
void main()
{
    // No local arrays: the group being built is kept in registers (lb its box, lf its first box, ls screen-fixed) and each
    // finished group goes straight to g_out (box, then anchor and the screen flag). Written into a local array at index
    // cnt - 1, a group's box kept only its first draw on the GPU (23 Oct, hudhinttest dump: an icon and its arrow joined,
    // the box was the icon's alone), so the second, fourth ... button icon of the truck's command list was left out.
    uint   cnt = 0;
    float4 lb = 0.0f, lf = 0.0f;
    bool   ls = false;
    // The game draws the world markers first; from the first screen-fixed group on, the rest of the call is HUD (the vehicle
    // command list, prompts, the ammo counter in a vehicle), whatever its pictures look like (Homard 21 Oct 18:55 F6: the
    // command list's button icons start each line and were taken for markers, their labels with them).
    bool   hud = false;
    for (uint i = 0; i < n && i < 64; ++i) {
        uint4 r = uint4(g_box[i * 4], g_box[i * 4 + 1], g_box[i * 4 + 2], g_box[i * 4 + 3]);
        if (r.z == 0 && r.x == 0) continue;
        float4 b    = float4(65535.0f - r.z - 4.0f, 65535.0f - r.w - 4.0f, r.x + 5.0f, r.y + 5.0f);
        bool   text = bit(masks.xy, i);
        bool   scr  = hud || bit(masks.zw, i);
        if (text && cnt > 0) {
            if (b.x - margin <= lf.z && b.z + margin >= lf.x && b.y - margin <= lf.w && b.w + margin >= lf.y) {
                // A marker's distance sits above its icon; a label beside an icon makes it a prompt.
                bool above = b.w <= lf.y + 0.25f * (lf.w - lf.y);
                if (!ls && (scr || !above)) { ls = true; hud = true; }
                if (ls || !scr) {
                    lb = float4(min(lb.xy, b.xy), max(lb.zw, b.zw));
                    continue;
                }
            }
        }
        // An icon touching the screen-fixed group just drawn (anywhere in it, within the margin) is part of that hint: the
        // compact vehicle hints have no shape and start with their text, then the button pictures around it (Homard 20 Oct
        // 14:19: the icons disappeared after a while).
        if (!text && cnt > 0 && ls) {
            if (b.x - margin <= lb.z && b.z + margin >= lb.x && b.y - margin <= lb.w && b.w + margin >= lb.y) {
                lb = float4(min(lb.xy, b.xy), max(lb.zw, b.zw));
                continue;
            }
        }
        if (cnt > 0) g_out[1 + 3 * (cnt - 1)] = lb;
        g_out[2 + 3 * cnt] = float4(0.5f * (b.xy + b.zw), 0, 0);
        if (cnt > 0) g_out[2 + 3 * (cnt - 1)].z = ls ? 1.0f : 0.0f;
        lb = b; lf = b; ls = text || scr; hud = hud || ls; cnt++;
    }
    if (cnt > 0) { g_out[1 + 3 * (cnt - 1)] = lb; g_out[2 + 3 * (cnt - 1)].z = ls ? 1.0f : 0.0f; }
    g_out[0] = float4(cnt, 0, 0, 0);
    uint2 taken = 0;
    for (uint k = 0; k < cnt; ++k) {
        float4 an    = g_out[2 + 3 * k];
        float2 shown = an.xy;
        if (smooth_on != 0 && an.z == 0.0f) {
            float3 d    = dir_of(an.xy);
            int    best = -1;
            float  bc   = snap_cos;
            for (uint s = 0; s < 64; ++s) {
                if (taken_at(taken, s) || g_state[s].w >= 30.0f) continue;
                float cs = dot(g_state[s].xyz, d);
                if (cs > bc) { bc = cs; best = (int)s; }
            }
            float3 sm = d;
            if (best >= 0) {
                float err = acos(saturate(bc));
                float t   = tau * clamp(1.0f - err / follow, 0.15f, 1.0f);
                float a   = 1.0f - exp(-dt / max(t, 1e-4f));
                sm = normalize(lerp(g_state[best].xyz, d, a));
            } else {
                // A new marker (or one that moved past the snap angle): the free entry seen least recently.
                float age = -1.0f;
                for (uint s2 = 0; s2 < 64; ++s2) {
                    if (!taken_at(taken, s2) && g_state[s2].w >= 30.0f && g_state[s2].w > age) { age = g_state[s2].w; best = (int)s2; }
                }
            }
            if (best >= 0) { take(taken, (uint)best); g_state[best] = float4(sm, 0.0f); }
            float2 p;
            if (px_of(sm, p)) shown = p;
        }
        g_out[3 + 3 * k] = float4(shown, 0, 0);
    }
    if (smooth_on != 0) {
        for (uint s3 = 0; s3 < 64; ++s3) {
            if (!taken_at(taken, s3)) g_state[s3].w = min(g_state[s3].w + 1.0f, 1000.0f);
        }
    }
}
)";

    struct Cb {
        UINT  w, h, mul_on, mark_on;
        float tan[4];
        float eye[4];
        float panel[4];
        float moff[4];
        float m[16];
        float msize[4];
    };
    struct GroupCb {
        UINT  n; float margin; UINT smooth_on; UINT pad0;
        float dt, tau, snap_cos, follow;
        float msize[2]; float pad[2];
        UINT  masks[4];   // text slots (0-31, 32-63), slots whose icon is no marker's (0-31, 32-63)
        float G[16];
        float Gi[16];
    };

    struct PanelCb { UINT w, h, all_on, pad; };

    ID3D11ComputeShader*       g_cs     = nullptr;
    ID3D11ComputeShader*       g_group_cs = nullptr;
    ID3D11ComputeShader*       g_panel_cs = nullptr;
    ID3D11Buffer*              g_panel_cb = nullptr;
    // The panel image's copies for the hints (kPanel): read copy and written copy, made for this panel texture.
    ID3D11Texture2D*           g_pfor   = nullptr;   // not held
    D3D11_TEXTURE2D_DESC       g_pdesc{};
    ID3D11Texture2D*           g_pstash = nullptr;
    ID3D11ShaderResourceView*  g_psrv   = nullptr;
    ID3D11Texture2D*           g_pmid   = nullptr;
    ID3D11UnorderedAccessView* g_puav   = nullptr;
    bool                       g_ptex_failed = false;
    ID3D11PixelShader*         g_box_ps = nullptr;
    ID3D11Buffer*              g_cb     = nullptr;
    ID3D11Buffer*              g_group_cb = nullptr;
    ID3D11SamplerState*        g_lin    = nullptr;
    ID3D11Buffer*              g_box_buf = nullptr;                 // kSlots x 4 uints
    ID3D11UnorderedAccessView* g_box_uav[kSlots]{};                 // one per slot (the box shader writes elements 0..3)
    ID3D11UnorderedAccessView* g_box_all = nullptr;                 // the whole buffer (cleared per call)
    ID3D11ShaderResourceView*  g_box_srv = nullptr;
    ID3D11Buffer*              g_group_buf = nullptr;               // 1 + 3 x kSlots float4
    ID3D11UnorderedAccessView* g_group_uav = nullptr;
    ID3D11ShaderResourceView*  g_group_srv = nullptr;
    ID3D11Buffer*              g_state_buf = nullptr;               // hudmarksmooth: kSlots followed directions (float4)
    ID3D11UnorderedAccessView* g_state_uav = nullptr;
    uint64_t                   g_box_text  = 0;                     // slots whose draw is text (the glyph atlas)
    uint64_t                   g_box_hint  = 0;                     // slots that are a control hint's widget, not a marker (hud_begin)
    // hudmarkdump: each slot's texture size, pixel shader and index count, logged with its box when a call has many marker draws.
    struct TexSize { UINT w, h; };
    TexSize                    g_slot_tex[kSlots]{};
    const void*                g_slot_ps[kSlots]{};
    UINT                       g_slot_draw[kSlots]{};
    UINT                       g_slot_inst[kSlots]{};               // instances (instanced draws), else 0
    int                        g_slot_kind[kSlots]{};               // m_hud_draw_kind
    UINT                       g_slot_ds[kSlots]{};                 // stencil: enabled (bit 8), function (bits 0-3), write mask (bits 16-23), reference (bits 24-31)
    ID3D11Buffer*              g_dump_stage = nullptr;
    int                        g_dump_frame = -1;                   // the call whose boxes are in g_dump_stage
    UINT                       g_dump_used  = 0;
    uint64_t                   g_dump_text = 0, g_dump_hint = 0;
    TexSize                    g_dump_tex[kSlots]{};
    const void*                g_dump_ps[kSlots]{};
    UINT                       g_dump_draw[kSlots]{};
    UINT                       g_dump_inst[kSlots]{};
    int                        g_dump_kind[kSlots]{};
    uint64_t                   g_dump_sig = 0;                      // the last dumped call's draws (sizes, kinds, text/hint), to dump new ones
    UINT                       g_dump_ds[kSlots]{};
    ID3D11Buffer*              g_dump_group_stage = nullptr;        // the groups of the same call
    int                        g_dumps_left = 0;                    // hudmarkdump N (off by default: the read-backs stall the frame)
    std::chrono::steady_clock::time_point g_dump_last{};
    std::chrono::steady_clock::time_point g_last_group{};
    // hudmarklog N (test): one marker's direction in the world per call, as found and as shown (read back a call later).
    ID3D11Buffer*              g_log_stage[2]{};
    glm::mat4                  g_log_ginv[2]{};
    int                        g_log_frame[2]{ -1, -1 };
    int                        g_log_next = 0;
    glm::vec2                  g_log_last{};
    bool                       g_log_have = false;
    glm::mat4                  g_game_vrp[8]{};                     // hudmarklag: the game camera of the last 8 calls
    int                        g_game_vrp_frame[8]{ -1, -1, -1, -1, -1, -1, -1, -1 };
    bool                       g_failed = false;
    ID3D11Texture2D*           g_for    = nullptr;   // the eye texture the copies below were made for (not held)
    D3D11_TEXTURE2D_DESC       g_desc{};
    ID3D11Texture2D*           g_stash  = nullptr;
    ID3D11ShaderResourceView*  g_srv    = nullptr;
    ID3D11Texture2D*           g_mid    = nullptr;
    ID3D11UnorderedAccessView* g_uav    = nullptr;
    bool                       g_tex_failed = false;
    int                        g_box_call   = -1;    // the call the boxes are for
    UINT                       g_box_used   = 0;     // slots used in it
    bool                       g_box_over   = false; // more marker draws than slots in it

    DXGI_FORMAT typeless_of(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default: return f;
        }
    }
    DXGI_FORMAT view_of(DXGI_FORMAT f)
    {
        switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
        }
    }

    ID3DBlob* compile(const char* src, const char* name, const char* target)
    {
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
        ID3DBlob* code = nullptr;
        ID3DBlob* err  = nullptr;
        if (!compile_fn || FAILED(compile_fn(src, strlen(src), name, nullptr, nullptr, "main", target, 0, 0, &code, &err))) {
            spdlog::error("[HUD] {} shader failed: {}", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
            if (err) err->Release();
            return nullptr;
        }
        if (err) err->Release();
        return code;
    }

    bool shaders(ID3D11Device* dev)
    {
        if (g_failed) return false;
        if (g_cs) return true;
        ID3DBlob* a = compile(kHudWorld, "hudworld", "cs_5_0");
        ID3DBlob* b = compile(kGroups, "hudgroups", "cs_5_0");
        ID3DBlob* c = compile(kBox, "hudbox", "ps_5_0");
        ID3DBlob* d = compile(kPanel, "hudpanel", "cs_5_0");
        if (a) dev->CreateComputeShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &g_cs);
        if (b) dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_group_cs);
        if (c) dev->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &g_box_ps);
        if (d) dev->CreateComputeShader(d->GetBufferPointer(), d->GetBufferSize(), nullptr, &g_panel_cs);
        for (auto* x : { a, b, c, d }) if (x) x->Release();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(Cb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&bd, nullptr, &g_cb);
        bd.ByteWidth = sizeof(GroupCb);
        dev->CreateBuffer(&bd, nullptr, &g_group_cb);
        bd.ByteWidth = sizeof(PanelCb);
        dev->CreateBuffer(&bd, nullptr, &g_panel_cb);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &g_lin);
        // The boxes: a typed uint buffer (atomics from the pixel shader), read by the grouping pass.
        D3D11_BUFFER_DESC xd{};
        xd.ByteWidth = kSlots * 16; xd.Usage = D3D11_USAGE_DEFAULT; xd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(dev->CreateBuffer(&xd, nullptr, &g_box_buf))) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            for (UINT i = 0; i < kSlots; ++i) { ud.Buffer.FirstElement = i * 4; ud.Buffer.NumElements = 4; dev->CreateUnorderedAccessView(g_box_buf, &ud, &g_box_uav[i]); }
            ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = kSlots * 4;
            dev->CreateUnorderedAccessView(g_box_buf, &ud, &g_box_all);
            D3D11_SHADER_RESOURCE_VIEW_DESC rd{};
            rd.Format = DXGI_FORMAT_R32_UINT; rd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; rd.Buffer.FirstElement = 0; rd.Buffer.NumElements = kSlots * 4;
            dev->CreateShaderResourceView(g_box_buf, &rd, &g_box_srv);
        }
        D3D11_BUFFER_DESC gd{};
        gd.ByteWidth = (1 + 3 * kSlots) * 16; gd.Usage = D3D11_USAGE_DEFAULT; gd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        gd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; gd.StructureByteStride = 16;
        if (SUCCEEDED(dev->CreateBuffer(&gd, nullptr, &g_group_buf))) {
            dev->CreateUnorderedAccessView(g_group_buf, nullptr, &g_group_uav);
            dev->CreateShaderResourceView(g_group_buf, nullptr, &g_group_srv);
        }
        // The followed directions start free (w = 1000).
        float init[kSlots * 4]{};
        for (UINT i = 0; i < kSlots; ++i) init[i * 4 + 3] = 1000.0f;
        D3D11_SUBRESOURCE_DATA id{ init, 0, 0 };
        gd.ByteWidth = kSlots * 16; gd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (SUCCEEDED(dev->CreateBuffer(&gd, &id, &g_state_buf))) dev->CreateUnorderedAccessView(g_state_buf, nullptr, &g_state_uav);
        const bool ok = g_cs && g_group_cs && g_box_ps && g_panel_cs && g_cb && g_group_cb && g_panel_cb && g_lin && g_box_all && g_box_srv && g_box_uav[kSlots - 1] && g_group_uav && g_group_srv && g_state_uav;
        if (!ok) {
            spdlog::error("[HUD] world pass: shaders or buffers missing, off");
            g_failed = true;
        }
        return ok;
    }

    bool ready(ID3D11Device* dev, ID3D11Texture2D* eye)
    {
        if (!shaders(dev)) return false;
        D3D11_TEXTURE2D_DESC d{};
        eye->GetDesc(&d);
        if (eye != g_for || d.Width != g_desc.Width || d.Height != g_desc.Height || d.Format != g_desc.Format) {
            if (g_srv) g_srv->Release();
            if (g_stash) g_stash->Release();
            if (g_uav) g_uav->Release();
            if (g_mid) g_mid->Release();
            g_srv = nullptr; g_stash = nullptr; g_uav = nullptr; g_mid = nullptr;
            g_for = eye; g_desc = d; g_tex_failed = false;
            D3D11_TEXTURE2D_DESC nd = d;
            nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
            nd.Format    = typeless_of(d.Format);
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = view_of(d.Format); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_stash))) dev->CreateShaderResourceView(g_stash, &sd, &g_srv);
            nd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = view_of(d.Format); ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_mid))) dev->CreateUnorderedAccessView(g_mid, &ud, &g_uav);
            g_tex_failed = !g_srv || !g_uav;
            spdlog::info("[HUD] world pass: eye picture {}x{} format {} ({})", d.Width, d.Height, (int)d.Format, g_tex_failed ? "no copies, off" : "ready");
        }
        return !g_tex_failed && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }

    // The panel image's copies (kPanel), made like it; view: the format its own shader view reads it with.
    bool panel_ready(ID3D11Device* dev, ID3D11Texture2D* panel, DXGI_FORMAT view)
    {
        D3D11_TEXTURE2D_DESC d{};
        panel->GetDesc(&d);
        if (panel != g_pfor || d.Width != g_pdesc.Width || d.Height != g_pdesc.Height || d.Format != g_pdesc.Format) {
            if (g_psrv) g_psrv->Release();
            if (g_pstash) g_pstash->Release();
            if (g_puav) g_puav->Release();
            if (g_pmid) g_pmid->Release();
            g_psrv = nullptr; g_pstash = nullptr; g_puav = nullptr; g_pmid = nullptr;
            g_pfor = panel; g_pdesc = d; g_ptex_failed = false;
            D3D11_TEXTURE2D_DESC nd = d;
            nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 }; nd.Usage = D3D11_USAGE_DEFAULT; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
            nd.Format    = typeless_of(d.Format);
            nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = view; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_pstash))) dev->CreateShaderResourceView(g_pstash, &sd, &g_psrv);
            nd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = view; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_pmid))) dev->CreateUnorderedAccessView(g_pmid, &ud, &g_puav);
            g_ptex_failed = !g_psrv || !g_puav;
            spdlog::info("[HUD] hints into the panel image: {}x{} format {} ({})", d.Width, d.Height, (int)d.Format, g_ptex_failed ? "no copies, drawn into the eyes" : "ready");
        }
        return !g_ptex_failed && d.SampleDesc.Count == 1;
    }
}

// hudmarksize: before a marker draw goes to the marker image, the same draw once more into its box (no colour written).
// Called from hud_begin with the game's state bound (shaders, buffers, viewport) and the draw's arguments.
void FarCry5Stereo::hud_mark_box_draw(ID3D11DeviceContext* ctx)
{
    const int fr = m_frame.load();
    if (g_box_call != fr) {
        g_box_call = fr;
        g_box_used = 0;
        g_box_over = false;
        g_box_text = 0;
        g_box_hint = 0;
    }
    if (std::abs(m_hud_mark_size.load() - 1.0f) < 0.01f && m_hud_mark_smooth.load() <= 0.0f) return;
    // A draw with no box is drawn into the marker image but belongs to no marker or hint, so it is never shown (the instanced
    // draws went this way until 23 Oct: the prompts' button pictures; Homard 23 Oct 08:54: "button prompt were still not
    // working"). Counted in the probe line.
    if (m_hud_draw_kind == 0) { m_hud_mark_unboxed++; return; }
    if (g_box_used >= kSlots) { g_box_over = true; return; }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = shaders(dev);
    dev->Release();
    if (!ok) return;
    if (g_box_used == 0) {
        const UINT zero[4]{};
        ctx->ClearUnorderedAccessViewUint(g_box_all, zero);
    }
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    ctx->PSSetShader(g_box_ps, nullptr, 0);
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &g_box_uav[g_box_used], nullptr);
    switch (m_hud_draw_kind) {
    case 1: call_orig<void>(m_draw_idx_hook, 12, ctx, m_hud_draw_count, m_hud_draw_start, m_hud_draw_base); break;
    case 2: call_orig<void>(m_draw_hook, 13, ctx, m_hud_draw_count, m_hud_draw_start); break;
    case 3: call_orig<void>(m_draw_idx_i_hook, 20, ctx, m_hud_draw_count, m_hud_draw_inst, m_hud_draw_start, m_hud_draw_base, m_hud_draw_start_inst); break;
    case 4: call_orig<void>(m_draw_i_hook, 21, ctx, m_hud_draw_count, m_hud_draw_inst, m_hud_draw_start, m_hud_draw_start_inst); break;
    case 5: call_orig<void>(m_dii_hook, 39, ctx, m_hud_draw_args, m_hud_draw_args_off); break;
    default: call_orig<void>(m_di_hook, 40, ctx, m_hud_draw_args, m_hud_draw_args_off); break;
    }
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
    ctx->PSSetShader(ps, nullptr, 0);
    if (ps) ps->Release();
    // Text (the distance under an icon) is drawn from the glyph atlas, the only 1024x512 texture among the markers.
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    D3D11_TEXTURE2D_DESC td{};
    if (srv) {
        ID3D11Resource* r = nullptr;
        srv->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim{};
        if (r) { r->GetType(&dim); if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(r)->GetDesc(&td); r->Release(); }
        srv->Release();
    }
    const bool text = td.Width == 1024 && td.Height == 512;
    if (text) g_box_text |= 1ull << g_box_used;
    // A control hint's widget (hud_begin), also its shapes drawn with no texture bound at all: the key caps around the letters
    // (Homard 21 Oct 12:50: "the icons are still not in the keybind menu when getting in vehicle").
    if (m_hud_mark_hint_now || !srv || m_hud_hint_test.load()) g_box_hint |= 1ull << g_box_used;
    // A picture that is not square is no marker's icon (those are 64x64, 96x96, 128x128): banners and prompts (180x48 at the
    // top of the view, 92x64, 304x24 bars). Homard 20 Oct 15:22: "objectives are not visible and some qte either".
    else if (!text && td.Width != td.Height) g_box_hint |= 1ull << g_box_used;
    g_slot_tex[g_box_used]  = { td.Width, td.Height };
    g_slot_ps[g_box_used]   = ps;
    g_slot_draw[g_box_used] = m_hud_draw_count;
    g_slot_inst[g_box_used] = m_hud_draw_kind == 3 || m_hud_draw_kind == 4 ? m_hud_draw_inst : 0;
    g_slot_kind[g_box_used] = m_hud_draw_kind;
    {
        ID3D11DepthStencilState* ds  = nullptr;
        UINT                     ref = 0;
        ctx->OMGetDepthStencilState(&ds, &ref);
        D3D11_DEPTH_STENCIL_DESC dd{};
        if (ds) { ds->GetDesc(&dd); ds->Release(); }
        g_slot_ds[g_box_used] = (dd.StencilEnable ? 0x100u : 0u) | (UINT(dd.FrontFace.StencilFunc) & 0xF) | (UINT(dd.StencilWriteMask) << 16) | ((ref & 0xFF) << 24);
    }
    g_box_used++;
}

// hudmarklog: this call's markers copied out; the copy of the call before read (ready by now) and one marker logged as a world
// direction (yaw, pitch in degrees) where the game drew it and where it was shown.
void FarCry5Stereo::hud_mark_log(ID3D11DeviceContext* ctx, const glm::mat4& ginv, float mw, float mh)
{
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    for (auto*& s : g_log_stage) {
        if (s) continue;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (1 + 3 * kSlots) * 16; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        dev->CreateBuffer(&bd, nullptr, &s);
    }
    dev->Release();
    const int i = g_log_next, j = i ^ 1;
    if (!g_log_stage[i] || !g_log_stage[j]) return;
    ctx->CopyResource(g_log_stage[i], g_group_buf);
    g_log_ginv[i]  = ginv;
    g_log_frame[i] = m_frame.load();
    g_log_next     = j;
    if (g_log_frame[j] < 0) return;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(call_orig<HRESULT>(m_map_hook, 14, ctx, g_log_stage[j], 0, D3D11_MAP_READ, 0, &m))) return;
    const float* f   = static_cast<const float*>(m.pData);
    const int    cnt = (int)f[0];
    auto dir = [&](float px, float py) {
        const glm::vec4 v{ px / mw * 2.0f - 1.0f, 1.0f - py / mh * 2.0f, 0.5f, 1.0f };
        const glm::vec4 q = g_log_ginv[j] * v;
        const glm::vec3 d = glm::normalize(glm::vec3{ q } / q.w);
        return glm::vec2{ glm::degrees(std::atan2(d.y, d.x)), glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f))) };
    };
    // The marker followed: the one nearest the last one logged (at first the one nearest the middle of the game's view).
    int   best = -1;
    float bd   = 1e9f;
    for (int k = 0; k < std::min(cnt, (int)kSlots); ++k) {
        const glm::vec2 d = dir(f[4 + 12 * k + 4], f[4 + 12 * k + 5]);
        const float     e = g_log_have ? glm::length(d - g_log_last) : glm::length(glm::vec2{ f[4 + 12 * k + 4] - 0.5f * mw, f[4 + 12 * k + 5] - 0.5f * mh });
        if (e < bd) { bd = e; best = k; }
    }
    if (best >= 0) {
        const float*    g     = f + 4 + 12 * best;
        const glm::vec2 raw   = dir(g[4], g[5]), shown = dir(g[8], g[9]);
        g_log_last = raw;
        g_log_have = true;
        spdlog::info("[HUDMark] call {} markers {}: drawn at yaw {:.3f} pitch {:.3f}, shown at yaw {:.3f} pitch {:.3f} (lag {}, steadiness {:.2f}); box ({:.0f},{:.0f})-({:.0f},{:.0f}) "
                     "anchor ({:.0f},{:.0f}) {}", g_log_frame[j], cnt, raw.x, raw.y, shown.x, shown.y, m_hud_mark_lag.load(), m_hud_mark_smooth.load(), g[0], g[1], g[2], g[3],
                     g[4], g[5], g[6] != 0.0f ? "on the panel (hint)" : "in the world");
    }
    call_orig<void>(m_unmap_hook, 15, ctx, g_log_stage[j], 0);
    if (--m_hud_mark_log <= 0) g_log_have = false;   // the next log picks its marker afresh
}

void FarCry5Stereo::hud_world_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    if (!ctx || !eye || m_menu_screen_on || m_hud_mode.load() != 1) return;
    const int fr = m_frame.load();
    if (m_hud_game_vrp_frame == fr) { g_game_vrp[fr & 7] = m_hud_game_vrp; g_game_vrp_frame[fr & 7] = fr; }
    // hudmarklag (test): the markers taken as placed with the game camera of that many calls before.
    const int lag      = std::clamp(m_hud_mark_lag.load(), 0, 6);
    const int gfr      = fr - lag;
    const bool mul_on  = m_hud_mul.load() && m_hud_mul_srv && m_hud_mul_cleared_frame == fr;
    // The markers in the world need this call's eye camera and the game camera the game placed them with; the hints on the
    // panel need neither (shown without the world markers in an eye picture that lacks them).
    const bool mark_on    = m_hud_markers.load() && m_hud_mark_srv && m_hud_mark_cleared_frame == fr;
    const bool mark_world = mark_on && m_hist[0].cur_frame == fr && g_game_vrp_frame[gfr & 7] == gfr;
    const glm::mat4& game_vrp = g_game_vrp[gfr & 7];
    if (!mul_on && !mark_on) return;
    const auto& v = m_xr_views[fr & 7];
    if (v.frame != fr) return;
    const int e = fr & 1;

    Cb cb{};
    cb.mul_on  = mul_on ? 1u : 0u;
    cb.mark_on = mark_on ? 1u : 0u;
    for (int k = 0; k < 4; ++k) cb.tan[k] = std::tan(v.fov[e][k]);
    const glm::vec3 mid = 0.5f * (glm::vec3{ v.pos[0][0], v.pos[0][1], v.pos[0][2] } + glm::vec3{ v.pos[1][0], v.pos[1][1], v.pos[1][2] });
    const glm::quat q{ v.rot[e][3], v.rot[e][0], v.rot[e][1], v.rot[e][2] };   // w x y z
    const glm::vec3 off = glm::inverse(q) * (glm::vec3{ v.pos[e][0], v.pos[e][1], v.pos[e][2] } - mid);
    const float     dist = std::max(0.3f, m_hud_distance.load());
    cb.eye[0] = off.x; cb.eye[1] = off.y; cb.eye[2] = off.z; cb.eye[3] = dist;
    // The panel as the HUD layer hangs it: hudscale metres wide at huddist, hudheight up, the HUD image's aspect.
    D3D11_TEXTURE2D_DESC hd{};
    if (m_hud_tex) m_hud_tex->GetDesc(&hd);
    const float half_x = 0.5f * std::max(0.1f, m_hud_width.load()) / dist;
    cb.panel[0] = half_x;
    cb.panel[1] = hd.Width > 0 ? half_x * float(hd.Height) / float(hd.Width) : half_x;
    cb.panel[2] = m_hud_height.load() / dist;
    // Scaled or smoothed markers: this call's boxes, all of its marker draws boxed.
    const float size   = std::clamp(m_hud_mark_size.load(), 0.2f, 2.0f);
    const float smooth = std::clamp(m_hud_mark_smooth.load(), 0.0f, 1.0f);
    const bool  scaled = mark_on && (std::abs(size - 1.0f) >= 0.01f || smooth > 0.0f) && g_box_call == fr && g_box_used > 0 && !g_box_over;
    if (mark_on) {
        // This eye's NDC -> world direction (rotation-only camera) -> the game camera's clip (all zero without them: no point
        // in front of the game camera, no world marker drawn).
        const glm::mat4 mm = mark_world ? game_vrp * glm::inverse(m_hist[0].cur_vrp) : glm::mat4(0.0f);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) cb.m[r * 4 + c] = mm[c][r];
        // Shown at hudmarkdist: each eye looks up the direction from the head centre to where its ray is at that distance.
        const float d = std::max(1.0f, m_hud_mark_dist.load());
        cb.moff[0] = 2.0f * (off.x / d) / std::max(1e-3f, cb.tan[1] - cb.tan[0]);
        cb.moff[1] = 2.0f * (off.y / d) / std::max(1e-3f, cb.tan[2] - cb.tan[3]);
        cb.moff[2] = 0.5f;
        D3D11_TEXTURE2D_DESC md{};
        if (m_hud_mark_tex) m_hud_mark_tex->GetDesc(&md);
        cb.msize[0] = (float)md.Width;
        cb.msize[1] = (float)md.Height;
        cb.msize[2] = 1.0f / size;
        cb.msize[3] = scaled ? 1.0f : (g_box_call == fr && g_box_over) ? 2.0f : 0.0f;   // 2: over the box limit, all on the panel
        if (g_box_call == fr && g_box_over) m_hud_mark_over++;
    }

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = ready(dev, eye);
    // The hints (screen-fixed groups) go into the panel image itself when the panel is its own layer, drawn this frame after
    // this pass (DLSS upscaling composes the panel into the eye picture before it: there they are drawn into the eye).
    const bool hint_pass = mark_on && (scaled || cb.msize[3] == 2.0f);
    D3D11_SHADER_RESOURCE_VIEW_DESC hsd{};
    if (m_hud_srv) m_hud_srv->GetDesc(&hsd);
    const bool panel_hints = ok && hint_pass && !fc5::dlss::upscaling() && m_hud_tex && m_hud_srv && panel_ready(dev, m_hud_tex, hsd.Format);
    dev->Release();
    if (!ok) return;
    cb.w = g_desc.Width;
    cb.h = g_desc.Height;
    cb.panel[3] = panel_hints ? 1.0f : 0.0f;
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(g_cb), 0u, static_cast<const D3D11_BOX*>(nullptr),
                    static_cast<const void*>(&cb), 0u, 0u);   // past the mod's hook, which swaps camera matrices it recognises
    ID3D11ComputeShader*       prev_cs = nullptr;
    ID3D11ShaderResourceView*  prev_srv[4]{};
    ID3D11UnorderedAccessView* prev_uav = nullptr;
    ID3D11Buffer*              prev_cb  = nullptr;
    ID3D11SamplerState*        prev_s   = nullptr;
    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 4, prev_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
    ctx->CSGetConstantBuffers(0, 1, &prev_cb);
    ctx->CSGetSamplers(0, 1, &prev_s);
    if (scaled) {
        // The boxes joined into markers, each followed in the world (one thread).
        const auto now = std::chrono::steady_clock::now();
        const float dt = g_last_group.time_since_epoch().count() == 0 ? 0.0f : std::clamp(std::chrono::duration<float>(now - g_last_group).count(), 0.0f, 0.1f);
        g_last_group = now;
        GroupCb gc{};
        gc.n         = g_box_used;
        gc.margin    = 0.02f * cb.msize[1];
        gc.masks[0]  = UINT(g_box_text);
        gc.masks[1]  = UINT(g_box_text >> 32);
        gc.masks[2]  = UINT(g_box_hint);
        gc.masks[3]  = UINT(g_box_hint >> 32);
        gc.smooth_on = smooth > 0.0f && mark_world ? 1u : 0u;
        gc.dt        = dt;
        gc.tau       = smooth;
        gc.snap_cos  = std::cos(glm::radians(6.0f));
        gc.follow    = glm::radians(3.0f);
        gc.msize[0]  = cb.msize[0];
        gc.msize[1]  = cb.msize[1];
        const glm::mat4 gi = mark_world ? glm::inverse(game_vrp) : glm::mat4(1.0f);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) { gc.G[r * 4 + c] = game_vrp[c][r]; gc.Gi[r * 4 + c] = gi[c][r]; }
        // Past the mod's own UpdateSubresource hook: it takes this copy of the game camera's matrix for a camera constant buffer
        // and puts the eye's in its place (each eye then had its own wrong "world" here, 15 deg apart).
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(g_group_cb), 0u, static_cast<const D3D11_BOX*>(nullptr),
                        static_cast<const void*>(&gc), 0u, 0u);
        ctx->CSSetShader(g_group_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &g_box_srv);
        ID3D11UnorderedAccessView* uavs[2]{ g_group_uav, g_state_uav };
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &g_group_cb);
        ctx->Dispatch(1, 1, 1);
        ID3D11ShaderResourceView*  null_srv = nullptr;
        ID3D11UnorderedAccessView* null_uav[2]{};
        ctx->CSSetShaderResources(0, 1, &null_srv);
        ctx->CSSetUnorderedAccessViews(0, 2, null_uav, nullptr);
        if (m_hud_mark_log.load() > 0 && mark_world) hud_mark_log(ctx, gi, cb.msize[0], cb.msize[1]);
        // hudmarkdump: a call with many marker draws (control hints on screen) logs every draw's box, texture and shader and
        // the groups made of them, a call later, and the call after it is scanned (hudscan).
        if (g_dump_frame >= 0 && g_dump_stage) {
            D3D11_MAPPED_SUBRESOURCE dm{};
            if (SUCCEEDED(call_orig<HRESULT>(m_map_hook, 14, ctx, g_dump_stage, 0, D3D11_MAP_READ, 0, &dm))) {
                const UINT* bx = static_cast<const UINT*>(dm.pData);
                spdlog::info("[HUDDump] call {}: {} marker draws (game image {}x{})", g_dump_frame, g_dump_used, (int)cb.msize[0], (int)cb.msize[1]);
                for (UINT s = 0; s < g_dump_used; ++s) {
                    const UINT* r = bx + s * 4;
                    spdlog::info("[HUDDump]   #{} {} tex {}x{} PS {} indices {}{} box ({},{})-({},{}) stencil {:#x}", s, (g_dump_text >> s) & 1 ? "text" : (g_dump_hint >> s) & 1 ? "hint" : "icon",
                                 g_dump_tex[s].w, g_dump_tex[s].h, g_dump_ps[s], g_dump_draw[s],
                                 g_dump_kind[s] >= 3 ? fmt::format(" x{} ({})", g_dump_inst[s], g_dump_kind[s] >= 5 ? "indirect" : "instanced") : std::string{},
                                 65535 - r[2], 65535 - r[3], r[0], r[1], g_dump_ds[s]);
                }
                call_orig<void>(m_unmap_hook, 15, ctx, g_dump_stage, 0);
            }
            // The groups made of them: where each is shown (in the world, or on the panel as a hint).
            if (g_dump_group_stage && SUCCEEDED(call_orig<HRESULT>(m_map_hook, 14, ctx, g_dump_group_stage, 0, D3D11_MAP_READ, 0, &dm))) {
                const float* f   = static_cast<const float*>(dm.pData);
                const int    cnt = std::min((int)f[0], (int)kSlots);
                for (int k = 0; k < cnt; ++k) {
                    const float* g = f + 4 + 12 * k;
                    spdlog::info("[HUDDump]   group {} box ({:.0f},{:.0f})-({:.0f},{:.0f}) {}", k, g[0], g[1], g[2], g[3], g[6] != 0.0f ? "on the panel (hint)" : "in the world");
                }
                call_orig<void>(m_unmap_hook, 15, ctx, g_dump_group_stage, 0);
            }
            g_dump_frame = -1;
        }
        // Dumped (hudmarkdump N): a call with 5 or more marker draws unlike the last one dumped (each prompt once, not the ammo
        // corner every time), 3 s apart. Off by default (Homard's 23 Oct 12:04 session: its blocking read-backs ran for 25 s of
        // play and dropped frames).
        if (const int nd = m_hud_mark_dump.exchange(0); nd > 0) { g_dumps_left = nd; g_dump_sig = 0; }
        uint64_t sig = g_box_text * 31 + g_box_hint;
        for (UINT s = 0; s < g_box_used; ++s) sig = sig * 1000003ull + (uint64_t(g_slot_tex[s].w) << 20) + (uint64_t(g_slot_tex[s].h) << 4) + UINT(g_slot_kind[s]);
        const auto dnow = std::chrono::steady_clock::now();
        if (g_box_used >= 5 && sig != g_dump_sig && g_dumps_left > 0 && dnow - g_dump_last > std::chrono::seconds(3)) {
            if (!g_dump_stage) {
                D3D11_BUFFER_DESC sd{};
                sd.ByteWidth = kSlots * 16; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Device* d2 = nullptr;
                ctx->GetDevice(&d2);
                if (d2) {
                    d2->CreateBuffer(&sd, nullptr, &g_dump_stage);
                    sd.ByteWidth = (1 + 3 * kSlots) * 16; sd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; sd.StructureByteStride = 16;
                    d2->CreateBuffer(&sd, nullptr, &g_dump_group_stage);
                    d2->Release();
                }
            }
            if (g_dump_stage) {
                ctx->CopyResource(g_dump_stage, g_box_buf);
                if (g_dump_group_stage) ctx->CopyResource(g_dump_group_stage, g_group_buf);
                g_dump_sig   = sig;
                g_dump_frame = fr;
                g_dump_used  = g_box_used;
                g_dump_text  = g_box_text;
                g_dump_hint  = g_box_hint;
                std::copy(std::begin(g_slot_tex), std::end(g_slot_tex), g_dump_tex);
                std::copy(std::begin(g_slot_ps), std::end(g_slot_ps), g_dump_ps);
                std::copy(std::begin(g_slot_draw), std::end(g_slot_draw), g_dump_draw);
                std::copy(std::begin(g_slot_ds), std::end(g_slot_ds), g_dump_ds);
                g_dumps_left--;
                g_dump_last = dnow;
                m_hud_scan_left = 1;
            }
        }
    }
    if (panel_hints) {
        // The hints into the panel image: its copy read, the merged image written, copied back.
        PanelCb pc{ g_pdesc.Width, g_pdesc.Height, scaled ? 0u : 1u, 0u };
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(g_panel_cb), 0u, static_cast<const D3D11_BOX*>(nullptr),
                        static_cast<const void*>(&pc), 0u, 0u);
        ctx->CopyResource(g_pstash, m_hud_tex);
        ID3D11ShaderResourceView* psrv[3]{ g_psrv, m_hud_mark_srv, scaled ? g_group_srv : nullptr };
        ctx->CSSetShader(g_panel_cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, psrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_puav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &g_panel_cb);
        ctx->Dispatch((g_pdesc.Width + 7) / 8, (g_pdesc.Height + 7) / 8, 1);
        ID3D11ShaderResourceView*  null_psrv[3]{};
        ID3D11UnorderedAccessView* null_puav = nullptr;
        ctx->CSSetShaderResources(0, 3, null_psrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &null_puav, nullptr);
        ctx->CopyResource(m_hud_tex, g_pmid);
        m_hud_mark_panel++;
    }
    ctx->CopyResource(g_stash, eye);
    ID3D11ShaderResourceView* srv[4]{ g_srv, mul_on ? m_hud_mul_srv : nullptr, mark_on ? m_hud_mark_srv : nullptr, scaled ? g_group_srv : nullptr };
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 4, srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->CSSetSamplers(0, 1, &g_lin);
    ctx->Dispatch((g_desc.Width + 7) / 8, (g_desc.Height + 7) / 8, 1);
    ID3D11ShaderResourceView*  null_srv[4]{};
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 4, null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(prev_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 4, prev_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &prev_cb);
    ctx->CSSetSamplers(0, 1, &prev_s);
    if (prev_cs) prev_cs->Release();
    for (auto* s : prev_srv) if (s) s->Release();
    if (prev_uav) prev_uav->Release();
    if (prev_cb) prev_cb->Release();
    if (prev_s) prev_s->Release();
    ctx->CopyResource(eye, g_mid);
    m_hud_world_passes++;
    if (scaled) m_hud_mark_scaled++;
}
