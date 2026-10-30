#include "FarCry5Dlss.h"
#include "FarCry5GpuTime.h"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <d3dcompiler.h>
#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace fc5::dlss
{
    namespace
    {
        std::atomic<bool> g_enabled{ true };
        std::atomic<int>  g_flags{ NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_MVJittered | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted };
        std::atomic<bool> g_recreate{ false };
        std::atomic<int>  g_debug{ 0 }; // 1: SMAA's input copied through unchanged (checks the colour path)
        std::string       g_status = "off";

        // This frame's motion-vector pass inputs (weak: the game keeps them alive).
        ID3D11Resource* g_depth       = nullptr;
        DXGI_FORMAT     g_depth_view  = DXGI_FORMAT_UNKNOWN;
        ID3D11Resource* g_encoded_mv  = nullptr;

        bool                  g_init_tried = false;
        bool                  g_ready      = false;
        ID3D11Device*         g_device     = nullptr;
        NVSDK_NGX_Parameter*  g_params     = nullptr;
        std::atomic<int>      g_preset{ 11 };   // preset K (Homard 13 Oct: preset M smeared the near ground)
        NVSDK_NGX_Handle*     g_feature[2]{};
        UINT                  g_feature_w[2]{}, g_feature_h[2]{}, g_feature_ow[2]{};
        // The previous size's feature per eye, kept: menus run DLSS at 1920x1080, the world at the eye size, and creating a
        // feature took 50-80 ms per eye at each switch (Homard's 27 Oct 21:35 log: 10 creations, 86-114 ms frames).
        NVSDK_NGX_Handle*     g_spare[2]{};
        UINT                  g_spare_w[2]{}, g_spare_h[2]{}, g_spare_ow[2]{};
        std::atomic<int>      g_spare_reuses{ 0 };

        // OFXR Bridge's DLSS frame generation (and its FSR / hybrid routes) takes the motion vectors and depth of the DLSS
        // evaluation whose output reaches an eye image: it tags the output texture (private data) and follows the tag through
        // full copies. The game's passes after DLSS (SMAA target, HUD) are draws, so no eye image ever carried it and OFXR
        // skipped every frame ("native_stereo_unavailable", 30 Oct). Each eye's tag is kept here and put on that eye's
        // swapchain image just before it is released (tag_xr_image). Layout and GUID from OFXR v663 d3d11_dlss_guides.
        struct OfxrReceipt { uint64_t owner, stream, publication; uint32_t width, height; };
        constexpr GUID kOfxrReceipt{ 0x62935ac7, 0x5aac, 0x47d9, { 0xaa, 0x59, 0x97, 0x61, 0x46, 0x3f, 0x96, 0x28 } };
        OfxrReceipt           g_receipt[2]{};
        std::atomic<bool>     g_ofxr_tags{ true };
        std::atomic<uint32_t> g_tags_set{ 0 }, g_tags_size{ 0 };

        void keep_ofxr_receipt(int eye, ID3D11Resource* output)
        {
            if (eye < 0 || eye > 1 || output == nullptr) return;
            OfxrReceipt r{};
            UINT        n = sizeof(r);
            if (FAILED(output->GetPrivateData(kOfxrReceipt, &n, &r)) || n != sizeof(r) || r.publication == 0) r = {};
            g_receipt[eye] = r;
        }

        ID3D11ComputeShader* g_decode_cs = nullptr; // encoded R8G8 -> pixel motion R16G16F
        ID3D11ComputeShader* g_cammv_cs = nullptr;  // exact camera motion (kCamMv)
    std::atomic<int>     g_trims{ 7 };   // all three on by default since 17 Oct (Homard)           // review P7 copy trims (set_trims)
    bool                 g_layer_last = false;   // the last compose used the HUD layer
    // HUD on a virtual screen (report A): cbuffer words 1..15 for the next compose (set_hud_screen).
    float                g_hud_screen[15]{};
    bool                 g_hud_screen_on = false;
    uint64_t             g_hud_screen_frames = 0;
    bool                 g_camonly_valid = false;
        ID3D11Buffer*        g_cammv_cb = nullptr;
        float                g_cam_rows[48]{};       // inv(this eye's camera) rows, then previous camera rows, then previous without the move
        bool                 g_cam_valid = false;
        float                g_body_rows[16]{};      // afwvehmv: clip -> previous clip for points fixed in the vehicle (camera-relative)
        float                g_body_lim = 0.0f;      // afwvehmv: nearer than this (m) the vehicle's motion (0 = off)
        std::atomic<int>     g_cammv_mode{ 1 };      // 0 game vectors, 1 camera + game where something moves, 2 camera only
        std::atomic<float>   g_cammv_sign{ 1.0f };
        std::atomic<float>   g_objmv_scale{ 1.0f };  // afwobjmv: object motion scale (2 in AFW)
    // (dlssmvnbr and dlssmvtrans were removed on 18 Oct: with dlssmvbest they did nothing, and dlssmvtrans never acted at all.)
        std::atomic<bool>    g_objmv_gun{ false };   // afwobjmvgun (research): the old way, the weapon pass scaled too
        std::atomic<int>     g_mv_compare{ 0 };
        std::atomic<int>     g_mv_stat{ 0 };   // cmd dlssmvstat N (test)
    // dlssmvbest (Homard 17 Oct: grass and trees blur when strafing): the game's vectors are a mix. Points its motion pass
    // reprojects from depth (ground, rocks, buildings) carry the camera's turn only (its CurrentToPrev has no move); what it
    // draws with its own motion (trees, grass, bushes) carries the move too (simulator dump, testruns6-10-17_foliage). Off,
    // mode 3 compared every pixel with the turn only and gave foliage the sideways move twice, and AFW's object scale cancelled
    // the move on foliage in mode 1. On: each pixel is compared with both camera motions; the nearer one is what the game's
    // vector already holds, and only the rest is added as the object's own motion.
    std::atomic<bool>    g_mv_best{ true };
    // dlssmvcheck (Homard 19 Oct: vehicles, people, animals): an object's own motion from the game is used only if this eye's
    // previous depth confirms it. A parked tractor's harrow carried a steady ~20 px a frame of motion in the game's vectors
    // while standing still (simulator, 19 Oct); its history then landed beside it. Where the previous depth matches the
    // point staying put and not the game's motion, it is taken as still.
    std::atomic<bool>    g_mv_check{ true };
    // dlssmvsway (Homard 30 Oct: foliage smeared even standing still, the sign and chains beside it sharp): with dlssmvbest the
    // game's object motion counted only above 2 px + 15 % of the camera motion, so leaves and grass swaying a pixel or two a
    // frame were reprojected as still. Their small vectors are precise (the 8-bit code is quartic: ~0.1 px steps at 1 px), so
    // with this the cut-off is these px + 5 %. 0 = off.
    std::atomic<float>   g_mv_sway{ 0.0f };
    std::atomic<int>     g_mv_dump{ 0 };   // cmd dlssmvdump (test): the next N evaluations' game, camera and final vectors and depth to probes
        ID3D11ComputeShader* g_depth_cs  = nullptr; // any depth SRV -> R32F
        ID3D11ComputeShader* g_copy_cs   = nullptr; // colour SRV -> typed UAV (output -> target is a copy)
        ID3D11ComputeShader* g_vis_cs    = nullptr; // motion vectors as an image (debug)

        struct Tex
        {
            ID3D11Texture2D*           tex = nullptr;
            ID3D11ShaderResourceView*  srv = nullptr;
            ID3D11UnorderedAccessView* uav = nullptr;
            UINT                       w = 0, h = 0;
            DXGI_FORMAT                fmt = DXGI_FORMAT_UNKNOWN;
            void release()
            {
                if (uav) uav->Release();
                if (srv) srv->Release();
                if (tex) tex->Release();
                uav = nullptr; srv = nullptr; tex = nullptr; w = h = 0;
            }
        };
        Tex g_mv, g_depth32, g_color_in, g_out, g_mv_cam, g_mv_camonly;
        Tex  g_prev_depth[2];          // dlssmvcheck: each eye's depth of its previous evaluation
        bool g_prev_depth_ok[2]{};
        Tex g_hi, g_pre_hud, g_bb_low, g_eye;   // upscaling: DLSS output, pre-HUD and final low images, composed eye
        UINT g_out_w = 0, g_out_h = 0;
        std::atomic<UINT> g_render_w{ 0 };
        bool g_have_hi = false;
        ID3D11ComputeShader* g_compose_cs = nullptr;
        ID3D11ComputeShader* g_compose_layer_cs = nullptr;
        ID3D11Buffer*        g_compose_cb       = nullptr;
        ID3D11ComputeShader* g_panel_cs = nullptr;
        ID3D11Buffer*        g_panel_cb = nullptr;
        Tex                  g_panel_base;
        ID3D11Texture2D*     g_panel_ui_tex = nullptr;   // the texture g_panel_ui_srv was made for (not owned)
        ID3D11ShaderResourceView* g_panel_ui_srv = nullptr;
        std::atomic<uint64_t> g_panel_frames{ 0 };
        std::atomic<uint64_t> g_layer_frames{ 0 }, g_layer_low_frames{ 0 }, g_diff_frames{ 0 };
        ID3D11ComputeShader* g_sharpen_cs = nullptr;
        ID3D11Buffer*        g_sharpen_cb = nullptr;
        std::atomic<float>   g_sharpness{ 0.05f };  // Homard 13 Oct: 0.05 (0.4 doubled frame-to-frame shimmer in VR: 18% of pixels vs 10%)
        Tex                  g_sharp;
        ID3D11SamplerState*  g_linear     = nullptr;
        // (The sun-shadow bias mask, dlssbias / dlssbiascam / dlssbiasrange, was removed on 16 Oct: only DLSS preset F reads
        // pInBiasCurrentColorMask, and the mod runs the transformer presets, so it cost two passes per eye for nothing.)

        bool make_tex(Tex& t, UINT w, UINT h, DXGI_FORMAT fmt)
        {
            if (t.tex && t.w == w && t.h == h && t.fmt == fmt) return true;
            t.release();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &t.tex))) return false;
            if (FAILED(g_device->CreateShaderResourceView(t.tex, nullptr, &t.srv))) return false;
            if (FAILED(g_device->CreateUnorderedAccessView(t.tex, nullptr, &t.uav))) return false;
            t.w = w; t.h = h; t.fmt = fmt;
            return true;
        }

        const char* kDecode = R"(
Texture2D<float2> g_in : register(t0);
RWTexture2D<float2> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h, iw, ih;
    g_out.GetDimensions(w, h);
    g_in.GetDimensions(iw, ih);
    if (tid.x >= w || tid.y >= h) return;
    uint2 src = min(uint2((float2(tid) + 0.5f) * float2(iw, ih) / float2(w, h)), uint2(iw - 1, ih - 1));
    float2 x = g_in[src].xy - 0.498039216f;
    float2 ax = abs(x);
    float2 s = (x >= 0.0f) ? 1.0f : -1.0f;
    float2 t = 2.0f * ax;
    float2 quartic = s * (t * t * t * t);
    float2 lin = 4.83004713f * x - s * 1.41502368f;
    float2 decoded = (ax < 0.334370166f) ? quartic : lin;
    g_out[tid] = 0.100000001f * decoded * float2(w, h);
})";
        // Exact camera motion for DLSS: the game's own vectors are 8-bit at half resolution (made for its
        // motion blur), too coarse for head turns. Each pixel's position from depth and this eye's camera,
        // seen by this eye's previous camera, gives the motion; where the game's vector differs clearly
        // (something moving by itself: people, animals, swaying plants) the game's is kept.
        const char* kCamMv = R"(
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_game : register(t1);
Texture2D<uint2> g_sten : register(t2);
Texture2D<float> g_pdepth : register(t3);
RWTexture2D<float2> g_out : register(u0);
RWTexture2D<float2> g_camonly : register(u1);
cbuffer C : register(b0) { float4 inv0, inv1, inv2, inv3; float4 pr0, pr1, pr2, pr3; float4 prm; float4 prm2; float4 rr0, rr1, rr2, rr3; float4 bb0, bb1, bb2, bb3; float4 prm3; };
// prm3.y: dlssmvbest (each pixel's game vector compared with both the camera motion with and without the move).
// prm3.z: dlssmvcheck with this eye's previous depth bound at t3.
// prm3.w > 0: dlssmvsway, the object-motion cut-off in px under dlssmvbest (plus 5 % of the camera motion) instead of prm.z / prm.w.
// prm2.x: object motion scale (AFW); prm2.y > 0: stencil bits of the weapon pass (gun, hands), never scaled: its
// vectors come from the weapon camera's own previous view, which already spans this eye's whole history.
// prm2.z, prm2.w: unused (dlssmvnbr, dlssmvtrans until 18 Oct). rr: this eye's previous camera without its move (rotation only), as the
// game's own vectors see it (its CurrentToPrev has no translation: walking, leaning and driving are not in them).
float objScale(uint2 tid) { return (prm2.y > 0.5f && (g_sten[tid].y & (uint)prm2.y) != 0) ? 1.0f : prm2.x; }
// This eye's camera motion of pixel p (px), from its depth (rot: without the camera's move); false when it leaves the view volume.
bool camAt(int2 p, uint w, uint h, bool rot, out float2 cam)
{
    cam = float2(0.0f, 0.0f);
    p = clamp(p, int2(0, 0), int2(w - 1, h - 1));
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 q = float4(dot(inv0, c), dot(inv1, c), dot(inv2, c), dot(inv3, c));
    q /= q.w;
    q = rot ? float4(dot(rr0, q), dot(rr1, q), dot(rr2, q), dot(rr3, q)) : float4(dot(pr0, q), dot(pr1, q), dot(pr2, q), dot(pr3, q));
    if (q.w <= 1e-6f) return false;
    q.xy /= q.w;
    cam = (float2(q.x * 0.5f + 0.5f, 0.5f - q.y * 0.5f) - uv) * float2(w, h) * prm.x;
    return true;
}
// This eye's previous device depth expected at pixel p if it stayed put (full camera motion), and where it was then (px).
bool prevStill(int2 p, uint w, uint h, out float2 at, out float pd)
{
    at = float2(0.0f, 0.0f); pd = 0.0f;
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 q = float4(dot(inv0, c), dot(inv1, c), dot(inv2, c), dot(inv3, c));
    q /= q.w;
    q = float4(dot(pr0, q), dot(pr1, q), dot(pr2, q), dot(pr3, q));
    if (q.w <= 1e-6f) return false;
    pd = q.z / q.w;
    at = float2(q.x / q.w * 0.5f + 0.5f, 0.5f - q.y / q.w * 0.5f) * float2(w, h);
    return true;
}
// afwvehmv (prm3.x > 0): a point nearer than prm3.x metres, in a vehicle, is fixed in the vehicle's frame; its motion over this
// eye's history is the vehicle's (bb: clip -> the previous image's clip through that frame). The game's own vector for it is
// one game update long while an AFW eye's history is two: a frame of driving was missing (the doubled cab).
bool bodyAt(int2 p, uint w, uint h, out float2 mv)
{
    mv = float2(0.0f, 0.0f);
    float2 uv = (float2(p) + 0.5f) / float2(w, h);
    float d = max(g_depth[p], 1e-7f);
    float4 c = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, d, 1.0f);
    float4 q = float4(dot(inv0, c), dot(inv1, c), dot(inv2, c), dot(inv3, c));
    q /= q.w;
    if (dot(q.xyz, q.xyz) >= prm3.x * prm3.x) return false;
    float4 b = float4(dot(bb0, c), dot(bb1, c), dot(bb2, c), dot(bb3, c));
    if (b.w <= 1e-6f) return false;
    b.xy /= b.w;
    mv = (float2(b.x * 0.5f + 0.5f, 0.5f - b.y * 0.5f) - uv) * float2(w, h) * prm.x;
    return true;
}
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 mv = g_game[tid];
    float2 bm;
    if (prm3.x > 0.0f && !(prm2.y > 0.5f && (g_sten[tid].y & (uint)prm2.y) != 0) && bodyAt(int2(tid), w, h, bm)) {
        float2 cam0;
        g_out[tid] = bm;
        g_camonly[tid] = camAt(int2(tid), w, h, false, cam0) ? cam0 : float2(0.0f, 0.0f);
        return;
    }
    float2 camv = float2(0.0f, 0.0f);
    float2 cam;
    if (camAt(int2(tid), w, h, false, cam)) {
        camv = cam;
        if (prm3.y > 0.5f && prm.y > 0.5f) {
            // dlssmvbest. The game's vector holds the turn only where its motion pass reprojects depth (ground, rocks,
            // buildings) and the move as well where things are drawn with their own motion (trees, grass, bushes): whichever
            // of the two camera motions it is nearer to is what it already holds, and only the rest is the object's own.
            // AFW: the camera part of the game's vector already spans this eye's history (the mod gives the game this eye's
            // previous camera), but an object's own motion spans one game update while the history is two: with afwobjmv
            // (k = 2) only that rest is doubled. The weapon pass (gun, hands) has its own whole vector.
            const bool gun = prm2.y > 0.5f && (g_sten[tid].y & (uint)prm2.y) != 0;
            float2 rot = cam;
            const bool haveRot = camAt(int2(tid), w, h, true, rot);
            const float2 g = mv;
            const float2 ref = (!gun && haveRot && length(g - rot) < length(g - cam)) ? rot : cam;
            const float2 r = g - ref;
            const float  rel = prm3.w > 0.0f ? 0.05f : prm.y > 1.5f ? 0.0f : prm.w;   // mode 1: plus 15 % of the camera motion; mode 3: 1.5 px flat
            const float  thr = prm3.w > 0.0f ? prm3.w : prm.z;                          // dlssmvsway: swaying leaves pass
            bool moving = length(r) > thr + rel * length(ref);
            // A game vector at its 8-bit code's end (0.1 of the view per frame: a very fast turn) is clipped, not an object's motion.
            if (moving && !gun && any(abs(mv) >= 0.099f * float2(w, h))) moving = false;
            if (moving && !gun) {
                // The game's vector covers a 2x2 block at half resolution: while you move, a near grass blade or leaf gets
                // the vector of a blade or the ground beside it (parallax), tens of px off; standing, its own sway stays under
                // 5 px (simulator, 17 Oct). Only a vector no pixel of the block's neighbourhood explains, with or without the
                // move, is the object's own motion.
                const int2 b = int2(tid / 2) * 2 - 1;
                [loop] for (int i = 0; i < 16; ++i) {
                    const int2 n = b + int2(i & 3, i >> 2);
                    float2 cf, cr;
                    if (camAt(n, w, h, false, cf) && length(g - cf) <= thr + rel * length(cf)) { moving = false; break; }
                    if (haveRot && camAt(n, w, h, true, cr) && length(g - cr) <= thr + rel * length(cr)) { moving = false; break; }
                }
            }
            if (moving && !gun && prm3.z > 0.5f) {
                // dlssmvcheck: where was this point in this eye's previous image? Still: at `at`, with depth pd; moving as the
                // game says: at `at` plus its own motion. Only a clear match for staying put (within 1 % in depth) where the
                // game's place does not match drops the motion; flat surfaces that match both keep it.
                float2 at; float pd;
                if (prevStill(int2(tid), w, h, at, pd) && pd > 1e-6f) {
                    const float2 am = at + r * objScale(tid);
                    if (all(at >= 0.0f) && all(at < float2(w, h)) && all(am >= 0.0f) && all(am < float2(w, h))) {
                        const float eS = abs(g_pdepth[int2(at)] - pd) / pd;
                        const float eM = abs(g_pdepth[int2(am)] - pd) / pd;
                        if (eS < 0.01f && eM > 4.0f * eS + 0.02f) moving = false;
                    }
                }
            }
            mv = moving ? (gun ? mv : cam + r * objScale(tid)) : cam;
            g_out[tid] = mv;
            g_camonly[tid] = camv;
            return;
        }
        // dlssmvbest off: the merge as it ran from 13 Oct. The game's vector is compared with the turn-only camera motion
        // (the old dlssmvtrans test was `trans && !camAt(...)`, and HLSL evaluates both sides, so that switch never changed it).
        float2 ref = cam;
        if (!camAt(int2(tid), w, h, true, ref)) ref = cam;
        // AFW: the game's object motion spans one update, this eye's previous image prm2.x updates.
        const float k = objScale(tid);
        if (prm.y > 1.5f) {
            // Mode 3: the eye's camera motion plus the game's own residual (objects that move by themselves) only.
            float2 r = mv - ref;
            mv = cam + (length(r) > prm.z ? r * k : float2(0.0f, 0.0f));
        } else {
            const bool moving = prm.y > 0.5f && length(mv - ref) > prm.z + prm.w * length(ref);
            mv = moving ? (k != 1.0f ? cam + (mv - cam) * k : mv) : cam;
        }
    }
    g_out[tid] = mv;
    g_camonly[tid] = camv;
})";
        const char* kDepth = R"(
Texture2D<float> g_in : register(t0);
RWTexture2D<float> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    g_out[tid] = g_in[tid];
})";
        const char* kCopy = R"(
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    g_out[tid] = g_in[tid];
})";

        const char* kVis = R"(
Texture2D<float2> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 m = g_in[tid];
    g_out[tid] = float4(saturate(abs(m.x) * 0.1f), saturate(abs(m.y) * 0.1f), saturate(length(m) * 0.02f), 1);
})";

        const char* kCompose = R"(
Texture2D<float4> g_hi : register(t0);
Texture2D<float4> g_pre : register(t1);
Texture2D<float4> g_bb : register(t2);
SamplerState g_lin : register(s0);
RWTexture2D<float4> g_out : register(u0);
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 uv = (float2(tid) + 0.5f) / float2(w, h);
    float4 hi = g_hi[tid];
    float4 pre = g_pre.SampleLevel(g_lin, uv, 0);
    float4 bb = g_bb.SampleLevel(g_lin, uv, 0);
    float3 d = abs(bb.rgb - pre.rgb);
    float a = saturate((max(d.r, max(d.g, d.b)) - 0.012f) * 25.0f);
    g_out[tid] = float4(lerp(hi.rgb, bb.rgb, a), 1.0f);
})";


        // The HUD as its own layer (drawn by the game into a transparent target, premultiplied): over the
        // DLSS image, or over the stretched game image when DLSS gave nothing this frame (g_base_low).
        const char* kComposeLayer = R"(
Texture2D<float4> g_hi : register(t0);
Texture2D<float4> g_hud : register(t1);
Texture2D<float4> g_bb : register(t2);
SamplerState g_lin : register(s0);
RWTexture2D<float4> g_out : register(u0);
cbuffer P : register(b0) {
    uint g_base_low; uint g_screen; float g_dist; float g_yoff;   // HUD screen (hudscreen): on, distance (m), centre height (tangent)
    float4 g_tan;                                                   // this eye image's edges as tangents: left, right, up, down
    float4 g_eye;                                                   // eye position from the head centre in head axes (m), xyz
    float4 g_half;                                                  // the screen's half size as tangents seen from the head centre: x, y
};
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 uv = (float2(tid) + 0.5f) / float2(w, h);
    float3 base = g_base_low != 0 ? g_bb.SampleLevel(g_lin, uv, 0).rgb : g_hi[tid].rgb;
    float2 huv = uv;
    if (g_screen != 0) {
        // The HUD as a flat screen in front of the head: this eye's ray through the pixel meets the plane z = -g_dist.
        float2 t = float2(lerp(g_tan.x, g_tan.y, uv.x), lerp(g_tan.z, g_tan.w, uv.y));
        float  k = g_dist + g_eye.z;
        float2 s = (g_eye.xy + t * k) / g_dist;
        huv = float2(0.5f + s.x / (2.0f * g_half.x), 0.5f - (s.y - g_yoff) / (2.0f * g_half.y));
    }
    float4 hud = (all(huv > 0.0f) && all(huv < 1.0f)) ? g_hud.SampleLevel(g_lin, huv, 0) : float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_out[tid] = float4(hud.rgb + base * (1.0f - saturate(hud.a)), 1.0f);
})";
        // A flat panel (the F11 menu) drawn into this eye's image: each pixel's ray from the eye meets the panel's plane,
        // given in this eye's own axes, so both eyes see one panel at its real place and depth (no extra headset layer).
        const char* kPanel = R"(
Texture2D<float4> g_base : register(t0);
Texture2D<float4> g_ui : register(t1);
SamplerState g_lin : register(s0);
RWTexture2D<float4> g_out : register(u0);
cbuffer P : register(b0) {
    float4 g_tan;   // this eye image's edges as tangents: left, right, up, down
    float4 g_c;     // panel centre in eye axes (m)
    float4 g_ux;    // panel right axis divided by its half width
    float4 g_uy;    // panel up axis divided by its half height
    float4 g_n;     // panel normal
};
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    float2 uv = (float2(tid) + 0.5f) / float2(w, h);
    float3 base = g_base[tid].rgb;
    float3 d = float3(lerp(g_tan.x, g_tan.y, uv.x), lerp(g_tan.z, g_tan.w, uv.y), -1.0f);
    float  dn = dot(g_n.xyz, d);
    float4 ui = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (abs(dn) > 1e-6f) {
        float s = dot(g_n.xyz, g_c.xyz) / dn;
        if (s > 0.0f) {
            float3 p   = s * d - g_c.xyz;
            float2 puv = float2(0.5f + 0.5f * dot(p, g_ux.xyz), 0.5f - 0.5f * dot(p, g_uy.xyz));
            if (all(puv > 0.0f) && all(puv < 1.0f)) ui = g_ui.SampleLevel(g_lin, puv, 0);
        }
    }
    g_out[tid] = float4(ui.rgb + base * (1.0f - saturate(ui.a)), 1.0f);
})";
        // Contrast-adaptive sharpening (after AMD CAS): neighbours weighted by local contrast.
        const char* kSharpen = R"(
Texture2D<float4> g_in : register(t0);
RWTexture2D<float4> g_out : register(u0);
cbuffer P : register(b0) { float g_sharp; float3 pad; };
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint w, h;
    g_out.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    int2 p = int2(tid);
    int2 mx2 = int2(w - 1, h - 1);
    float3 c = g_in[p].rgb;
    float3 n = g_in[clamp(p + int2(0, -1), 0, mx2)].rgb;
    float3 s = g_in[clamp(p + int2(0, 1), 0, mx2)].rgb;
    float3 e = g_in[clamp(p + int2(1, 0), 0, mx2)].rgb;
    float3 o = g_in[clamp(p + int2(-1, 0), 0, mx2)].rgb;
    float3 mn = min(c, min(min(n, s), min(e, o)));
    float3 mx = max(c, max(max(n, s), max(e, o)));
    float3 amp = sqrt(saturate(min(mn, 1.0f - mx) / max(mx, 1e-4f)));
    float3 wgt = -amp * lerp(0.125f, 0.2f, g_sharp);
    float3 r = (c + wgt * (n + s + e + o)) / (1.0f + 4.0f * wgt);
    g_out[tid] = float4(saturate(r), g_in[p].a);
})";

        void sharpen(ID3D11DeviceContext* ctx, Tex& src, Tex& dst)
        {
            if (!g_sharpen_cb) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                g_device->CreateBuffer(&bd, nullptr, &g_sharpen_cb);
            }
            D3D11_MAPPED_SUBRESOURCE m{};
            if (g_sharpen_cb && SUCCEEDED(ctx->Map(g_sharpen_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float v[4]{ g_sharpness.load(), 0, 0, 0 };
                std::memcpy(m.pData, v, 16);
                ctx->Unmap(g_sharpen_cb, 0);
            }
            ID3D11ComputeShader* prev_cs = nullptr;
            ID3D11Buffer*        prev_cb = nullptr;
            ctx->CSGetShader(&prev_cs, nullptr, nullptr);
            ctx->CSGetConstantBuffers(0, 1, &prev_cb);
            ctx->CSSetShader(g_sharpen_cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_sharpen_cb);
            ctx->CSSetShaderResources(0, 1, &src.srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &dst.uav, nullptr);
            ctx->Dispatch((dst.w + 7) / 8, (dst.h + 7) / 8, 1);
            ID3D11ShaderResourceView*  ns = nullptr;
            ID3D11UnorderedAccessView* nu = nullptr;
            ctx->CSSetShaderResources(0, 1, &ns);
            ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
            ctx->CSSetShader(prev_cs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &prev_cb);
            if (prev_cs) prev_cs->Release();
            if (prev_cb) prev_cb->Release();
        }

        ID3D11ComputeShader* compile(const char* src, const char* name)
        {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            if (!compile_fn) return nullptr;
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (FAILED(compile_fn(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[DLSS] shader {} failed: {}", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                return nullptr;
            }
            ID3D11ComputeShader* cs = nullptr;
            g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
            code->Release();
            return cs;
        }

        bool init(ID3D11DeviceContext* ctx)
        {
            if (g_init_tried) return g_ready;
            g_init_tried = true;
            ctx->GetDevice(&g_device);
            wchar_t dir[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, dir);
            auto r = NVSDK_NGX_D3D11_Init_with_ProjectID("6b3f5c2e-1d4a-4f7b-9c8e-2a1b3c4d5e6f", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dir, g_device);
            if (NVSDK_NGX_FAILED(r)) {
                spdlog::info("[DLSS] NGX init with a project id failed (0x{:x}): trying an application id", (uint32_t)r);
                r = NVSDK_NGX_D3D11_Init(231313132ull, dir, g_device);
            }
            if (NVSDK_NGX_FAILED(r)) {
                g_status = fmt::format("NGX init failed 0x{:x}", (uint32_t)r);
                spdlog::error("[DLSS] {}", g_status);
                return false;
            }
            r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
            if (NVSDK_NGX_FAILED(r) || !g_params) {
                g_status = fmt::format("NGX parameters failed 0x{:x}", (uint32_t)r);
                spdlog::error("[DLSS] {}", g_status);
                return false;
            }
            int available = 0;
            NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
            if (!available) {
                g_status = "DLSS not available on this GPU/driver";
                spdlog::error("[DLSS] {}", g_status);
                return false;
            }
            g_decode_cs = compile(kDecode, "fc5_dlss_decode_mv");
            g_cammv_cs  = compile(kCamMv, "fc5_dlss_camera_mv");
            g_depth_cs  = compile(kDepth, "fc5_dlss_depth");
            g_copy_cs   = compile(kCopy, "fc5_dlss_copy");
            g_vis_cs    = compile(kVis, "fc5_dlss_vis");
            g_compose_cs = compile(kCompose, "fc5_dlss_compose");
            g_compose_layer_cs = compile(kComposeLayer, "fc5_dlss_compose_layer");
            g_panel_cs = compile(kPanel, "fc5_dlss_panel");
            g_sharpen_cs = compile(kSharpen, "fc5_dlss_sharpen");
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            g_device->CreateSamplerState(&sd, &g_linear);
            if (!g_decode_cs || !g_depth_cs || !g_copy_cs) {
                g_status = "shader compile failed";
                return false;
            }
            g_ready  = true;
            g_status = "ready";
            spdlog::info("[DLSS] NGX ready (DLSS super sampling available)");
            return true;
        }

        void run_cs(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, ID3D11ShaderResourceView* in, ID3D11UnorderedAccessView* out, UINT w, UINT h)
        {
            ID3D11ComputeShader*       prev_cs  = nullptr;
            ID3D11ShaderResourceView*  prev_srv = nullptr;
            ID3D11UnorderedAccessView* prev_uav = nullptr;
            ctx->CSGetShader(&prev_cs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 1, &prev_srv);
            ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &in);
            ctx->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
            ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            ID3D11ShaderResourceView*  null_srv = nullptr;
            ID3D11UnorderedAccessView* null_uav = nullptr;
            ctx->CSSetShaderResources(0, 1, &null_srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
            ctx->CSSetShader(prev_cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &prev_srv);
            UINT keep = (UINT)-1;
            ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, &keep);
            if (prev_cs) prev_cs->Release();
            if (prev_srv) prev_srv->Release();
            if (prev_uav) prev_uav->Release();
        }

        DXGI_FORMAT typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }
    }

    void set_debug(int mode) { g_debug = mode; }
    void set_flags(int flags) { g_flags = flags; g_recreate = true; }
    void set_preset(int preset) { g_preset = preset; g_recreate = true; }
    int  preset() { return g_preset.load(); }
    void set_enabled(bool on) { g_enabled = on; if (!on) g_status = "off"; if (on && !g_ready) g_init_tried = false; }
    bool enabled() { return g_enabled.load(); }

    void tag_xr_image(int eye, ID3D11Texture2D* image)
    {
        if (!g_ofxr_tags.load() || eye < 0 || eye > 1 || image == nullptr) return;
        if (g_receipt[eye].publication == 0) {
            // OFXR knows only DLSS features created after it hooked NGX, which it does once the XR session runs; the
            // mod's features may be older (the menu). Untagged outputs while the headset gets images: new features,
            // at most 3 times.
            static uint32_t untagged = 0, retries = 0;
            static const bool ofxr = GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll") != nullptr;
            if (ofxr && g_ready && ++untagged >= 600 && retries < 3) {
                untagged = 0;
                retries++;
                g_recreate = true;
                spdlog::info("[DLSS] ofxrguides: no OFXR tag on the DLSS output after 600 eye images: DLSS features recreated ({} of 3)", retries);
            }
            return;
        }
        D3D11_TEXTURE2D_DESC d{};
        image->GetDesc(&d);
        // OFXR accepts a tag only on an image of the DLSS output's size (a stretched eye image is not that output).
        if (d.Width != g_receipt[eye].width || d.Height != g_receipt[eye].height) { g_tags_size++; return; }
        if (SUCCEEDED(image->SetPrivateData(kOfxrReceipt, sizeof(OfxrReceipt), &g_receipt[eye]))) g_tags_set++;
    }
    void set_ofxr_tags(bool on) { g_ofxr_tags = on; }
    bool ofxr_tags() { return g_ofxr_tags.load(); }
    std::string ofxr_tag_stats()
    {
        // Diagnosis (30 Oct): whether OFXR hooked NGX's D3D11 entries (an inline hook starts with a jump) and what its guide
        // statistics say (OFXR_DlssGuideStatisticsV2, provider API v2 of v663).
        std::string hooks;
        for (const wchar_t* m : { L"_nvngx.dll", L"nvngx_dlss.dll" }) {
            const HMODULE h = GetModuleHandleW(m);
            for (const char* fn : { "NVSDK_NGX_D3D11_CreateFeature", "NVSDK_NGX_D3D11_CreateFeature1", "NVSDK_NGX_D3D11_EvaluateFeature" }) {
                const auto* p = h ? reinterpret_cast<const uint8_t*>(GetProcAddress(h, fn)) : nullptr;
                hooks += fmt::format(" {}{}={}", m[0] == L'_' ? "core:" : "dlss:", fn + 16, p ? fmt::format("{:02x}", *p) : std::string("-"));
            }
        }
        struct Stats { uint32_t size, version; uint64_t published, depth, invalid, copies, copy_failures, resolves, missing, stale, queue_mismatch,
                       matched, temporal, invalid_rej, used, last_used; uint32_t status; uint64_t fsr_attempts, fsr_used, fsr_failures; uint32_t fsr_status; };
        std::string stats = " no OFXR stats";
        if (const HMODULE layer = GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll")) {
            using Fn = int (*)(Stats*);
            if (auto f = reinterpret_cast<Fn>(GetProcAddress(layer, "OFXR_GetDlssGuideStatisticsV2"))) {
                Stats s{};
                s.size = sizeof(s);
                s.version = 2;
                const int r = f(&s);
                stats = fmt::format(" OFXR guides (r {}): published {}, with depth {}, invalid {}, copy failures {}, resolves {}, missing streams {}, used {}, status {}", r,
                                    s.published, s.depth, s.invalid, s.copy_failures, s.resolves, s.missing, s.used, s.status);
            }
        }
        return fmt::format("{} eye images tagged, {} of another size; last publications L {} R {};{}{}", g_tags_set.exchange(0), g_tags_size.exchange(0),
                           g_receipt[0].publication, g_receipt[1].publication, hooks, stats);
    }
    std::string status() { return g_status; }

    void set_sharpness(float s) { g_sharpness = std::clamp(s, 0.0f, 1.0f); }
    float sharpness() { return g_sharpness.load(); }
    void set_output_size(UINT w, UINT h) { g_out_w = w; g_out_h = h; }
    bool upscaling() { return g_enabled.load() && g_out_w > 0; }
    float upscale_ratio()
    {
        const UINT rw = g_render_w.load();
        return (upscaling() && rw > 0 && g_out_w > rw) ? float(g_out_w) / float(rw) : 1.0f;
    }

    void after_smaa(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* target)
    {
        if (!g_have_hi || !target) return;
        if ((g_trims.load() & 2) && g_layer_last) return;   // trim 2: the HUD is its own layer, the pre-HUD copy is not read
        ID3D11Resource* r = nullptr;
        target->GetResource(&r);
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};
        target->GetDesc(&vd);
        if (make_tex(g_pre_hud, d.Width, d.Height, typed(vd.Format))) ctx->CopyResource(g_pre_hud.tex, r);
        r->Release();
    }

    ID3D11Texture2D* compose(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, ID3D11ShaderResourceView* hud_layer)
    {
        if (!backbuffer) return nullptr;
        // With the HUD as a layer, a frame without a DLSS image still gets the HUD (over the stretched
        // game image); the difference method needs both the DLSS image and the pre-HUD copy.
        const bool layer = hud_layer != nullptr && g_compose_layer_cs != nullptr && g_hi.tex != nullptr;
        g_layer_last     = layer;
        if (!layer && (!g_have_hi || !g_pre_hud.tex || !g_compose_cs)) return nullptr;
        D3D11_TEXTURE2D_DESC d{};
        backbuffer->GetDesc(&d);
        if (!make_tex(g_bb_low, d.Width, d.Height, typed(d.Format))) return nullptr;
        // The stretched game image is only read when there is no DLSS image (trim 4 skips the copy otherwise).
        if (!((g_trims.load() & 4) && layer && g_have_hi)) ctx->CopyResource(g_bb_low.tex, backbuffer);
        if (!make_tex(g_eye, g_hi.w, g_hi.h, typed(d.Format))) return nullptr;
        ID3D11ComputeShader*       prev_cs = nullptr;
        ctx->CSGetShader(&prev_cs, nullptr, nullptr);
        ID3D11ShaderResourceView*  srv[3]{ g_hi.srv, layer ? hud_layer : g_pre_hud.srv, g_bb_low.srv };
        if (layer) {
            const UINT low = g_have_hi ? 0u : 1u;
            if (!g_compose_cb) {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                ID3D11Device* dev = nullptr;
                ctx->GetDevice(&dev);
                if (dev) { dev->CreateBuffer(&bd, nullptr, &g_compose_cb); dev->Release(); }
            }
            uint32_t cbv[16]{};
            cbv[0] = low;
            if (g_hud_screen_on) {
                std::memcpy(&cbv[1], g_hud_screen, 15 * sizeof(float));   // g_screen flag + parameters (set_hud_screen)
                g_hud_screen_frames++;
            }
            g_hud_screen_on = false;   // given for one compose only
            if (g_compose_cb) ctx->UpdateSubresource(g_compose_cb, 0, nullptr, cbv, 0, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_compose_cb);
            ctx->CSSetShader(g_compose_layer_cs, nullptr, 0);
            (low ? g_layer_low_frames : g_layer_frames)++;
        } else {
            ctx->CSSetShader(g_compose_cs, nullptr, 0);
            g_diff_frames++;
        }
        ctx->CSSetShaderResources(0, 3, srv);
        ctx->CSSetSamplers(0, 1, &g_linear);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_eye.uav, nullptr);
        ctx->Dispatch((g_eye.w + 7) / 8, (g_eye.h + 7) / 8, 1);
        ID3D11ShaderResourceView*  null_srv[3]{};
        ID3D11UnorderedAccessView* null_uav = nullptr;
        ctx->CSSetShaderResources(0, 3, null_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ctx->CSSetShader(prev_cs, nullptr, 0);
        if (prev_cs) prev_cs->Release();
        if (layer) { ID3D11Buffer* none = nullptr; ctx->CSSetConstantBuffers(0, 1, &none); }
        return g_eye.tex;
    }

    void set_camera(const float* inv_rows, const float* prev_rows, const float* prev_rot_rows)
    {
        std::memcpy(g_cam_rows, inv_rows, 16 * sizeof(float));
        std::memcpy(g_cam_rows + 16, prev_rows, 16 * sizeof(float));
        std::memcpy(g_cam_rows + 32, prev_rot_rows, 16 * sizeof(float));
        g_cam_valid = true;
        g_body_lim  = 0.0f;   // set_body_motion follows when it applies to this camera
    }
    void set_body_motion(const float* rows, float metres)
    {
        std::memcpy(g_body_rows, rows, 16 * sizeof(float));
        g_body_lim = metres;
    }
    void set_hud_screen(const float tan[4], const float eye[3], float dist, float half_x, float half_y, float yoff)
    {
        uint32_t one = 1;
        std::memcpy(&g_hud_screen[0], &one, 4);   // g_screen
        g_hud_screen[1] = dist;
        g_hud_screen[2] = yoff;
        for (int k = 0; k < 4; ++k) g_hud_screen[3 + k] = tan[k];
        for (int k = 0; k < 3; ++k) g_hud_screen[7 + k] = eye[k];
        g_hud_screen[10] = 0.0f;
        g_hud_screen[11] = half_x;
        g_hud_screen[12] = half_y;
        g_hud_screen[13] = g_hud_screen[14] = 0.0f;
        g_hud_screen_on = true;
    }
    uint64_t hud_screen_frames() { return g_hud_screen_frames; }
    bool draw_panel(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, ID3D11Texture2D* ui, const float params[20])
    {
        if (!ctx || !eye || !ui || !g_panel_cs || !g_eye.tex || eye != g_eye.tex || !g_eye.uav) return false;
        if (ui != g_panel_ui_tex) {
            if (g_panel_ui_srv) g_panel_ui_srv->Release();
            g_panel_ui_srv = nullptr;
            g_panel_ui_tex = ui;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) { dev->CreateShaderResourceView(ui, nullptr, &g_panel_ui_srv); dev->Release(); }
        }
        if (!g_panel_ui_srv) return false;
        if (!g_panel_cb) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev) { dev->CreateBuffer(&bd, nullptr, &g_panel_cb); dev->Release(); }
            if (!g_panel_cb) return false;
        }
        if (!make_tex(g_panel_base, g_eye.w, g_eye.h, g_eye.fmt)) return false;
        ctx->CopyResource(g_panel_base.tex, g_eye.tex);
        ctx->UpdateSubresource(g_panel_cb, 0, nullptr, params, 0, 0);
        ID3D11ComputeShader* prev_cs = nullptr;
        ctx->CSGetShader(&prev_cs, nullptr, nullptr);
        ID3D11ShaderResourceView* srv[2]{ g_panel_base.srv, g_panel_ui_srv };
        ctx->CSSetShader(g_panel_cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g_panel_cb);
        ctx->CSSetShaderResources(0, 2, srv);
        ctx->CSSetSamplers(0, 1, &g_linear);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_eye.uav, nullptr);
        ctx->Dispatch((g_eye.w + 7) / 8, (g_eye.h + 7) / 8, 1);
        ID3D11ShaderResourceView*  null_srv[2]{};
        ID3D11UnorderedAccessView* null_uav = nullptr;
        ID3D11Buffer*              null_cb  = nullptr;
        ctx->CSSetShaderResources(0, 2, null_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &null_cb);
        ctx->CSSetShader(prev_cs, nullptr, 0);
        if (prev_cs) prev_cs->Release();
        g_panel_frames++;
        return true;
    }
    uint64_t panel_frames() { return g_panel_frames.load(); }
    void set_trims(int bits) { g_trims = bits & 7; }
    int trims() { return g_trims.load(); }
    void set_camera_motion(int mode, float sign) { g_cammv_mode = std::clamp(mode, 0, 3); g_cammv_sign = sign < 0.0f ? -1.0f : 1.0f; }
    int camera_motion() { return g_cammv_mode.load(); }
    void set_object_motion_scale(float s) { g_objmv_scale = s; }
    void set_object_motion_gun(bool on) { g_objmv_gun = on; }
    void compare_motion(int n) { g_mv_compare = n; }
    void motion_stat(int n) { g_mv_stat = n; }
    void set_motion_best(bool on) { g_mv_best = on; }
    void set_motion_check(bool on) { g_mv_check = on; }
    bool motion_check() { return g_mv_check.load(); }
    void set_motion_sway(float px) { g_mv_sway = std::clamp(px, 0.0f, 2.0f); }
    float motion_sway() { return g_mv_sway.load(); }
    bool motion_best() { return g_mv_best.load(); }
    std::atomic<int> g_mv_dump_total{ 0 };
    void motion_dump(int n) { g_mv_dump_total = n; g_mv_dump = n; }

    std::string compose_stats()
    {
        return fmt::format("HUD layer over DLSS {}, over the stretched image {}, difference {}", g_layer_frames.exchange(0), g_layer_low_frames.exchange(0), g_diff_frames.exchange(0));
    }

    ID3D11ShaderResourceView* output_srv() { return g_have_hi ? g_hi.srv : nullptr; }

    void new_frame()
    {
        g_have_hi = false;
        g_depth      = nullptr;
        g_encoded_mv = nullptr;
    }

    void note_motion_pass(ID3D11Resource* depth, DXGI_FORMAT depth_view_format, ID3D11Resource* encoded_mv)
    {
        g_depth      = depth;
        g_depth_view = depth_view_format;
        g_encoded_mv = encoded_mv;
    }

    std::atomic<int> g_dlaa_phases{ 8 };
    void set_dlaa_phases(int n) { g_dlaa_phases = std::clamp(n, 4, 32); }
    int  dlaa_phases() { return g_dlaa_phases.load(); }

    void jitter_for(int frame, float& x_px, float& y_px)
    {
        // Halton (2, 3), 8 phases per eye: the same eye sees phases 0..7 on its own frames.
        // More phases when upscaling (each output pixel needs several input samples).
        const int phases = g_out_w > 0 ? 16 : std::clamp(g_dlaa_phases.load(), 4, 32);   // dlaaphases (default 8)
        const int n = ((frame >> 1) % phases) + 1;
        auto halton = [](int i, int b) {
            float f = 1.0f, r = 0.0f;
            while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
            return r;
        };
        x_px = halton(n, 2) - 0.5f;
        y_px = halton(n, 3) - 0.5f;
    }

    bool resolve(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* color, ID3D11RenderTargetView* target, int eye, float jx, float jy, bool reset)
    {
        if (!g_enabled.load() || !color || !target) return false;
        if (!init(ctx)) return false;
        if (!g_depth || !g_encoded_mv) { g_status = "no motion-vector pass this frame"; return false; }

        ID3D11Resource* color_res = nullptr;
        color->GetResource(&color_res);
        ID3D11Resource* target_res = nullptr;
        target->GetResource(&target_res);
        D3D11_TEXTURE2D_DESC cd{}, td{};
        static_cast<ID3D11Texture2D*>(color_res)->GetDesc(&cd);
        static_cast<ID3D11Texture2D*>(target_res)->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC cvd{};
        color->GetDesc(&cvd);
        D3D11_RENDER_TARGET_VIEW_DESC tvd{};
        target->GetDesc(&tvd);
        bool ok = false;
        fc5::gputime::begin(ctx, fc5::gputime::kDlssPrep);
        bool in_eval = false;
        ID3D11Texture2D* mv_in = nullptr;   // the motion DLSS reads (g_mv, or the camera motion with trim 1)
        do {
            if (cd.Width != td.Width || cd.Height != td.Height) { g_status = "colour and target sizes differ"; break; }
            const UINT w = cd.Width, h = cd.Height;
            const DXGI_FORMAT work = typed(cvd.Format);
            // Inputs in formats DLSS reads: colour copied to a typed texture, depth to R32F, motion decoded.
            if (!make_tex(g_color_in, w, h, work) || !make_tex(g_out, w, h, work) || !make_tex(g_depth32, w, h, DXGI_FORMAT_R32_FLOAT) ||
                !make_tex(g_mv, w, h, DXGI_FORMAT_R16G16_FLOAT)) {
                g_status = fmt::format("texture allocation failed (format {})", (int)work);
                break;
            }
            mv_in = g_mv.tex;
            run_cs(ctx, g_copy_cs, color, g_color_in.uav, w, h);
            if (g_debug.load() == 1) {
                ctx->CopyResource(target_res, g_color_in.tex);
                g_status = "debug: colour pass-through";
                ok = true;
                break;
            }
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC dv{};
                dv.Format = g_depth_view != DXGI_FORMAT_UNKNOWN ? g_depth_view : DXGI_FORMAT_R32_FLOAT;
                dv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                dv.Texture2D.MipLevels = 1;
                ID3D11ShaderResourceView* dsrv = nullptr;
                if (FAILED(g_device->CreateShaderResourceView(g_depth, &dv, &dsrv))) { g_status = fmt::format("depth view failed (format {})", (int)dv.Format); break; }
                run_cs(ctx, g_depth_cs, dsrv, g_depth32.uav, w, h);
                dsrv->Release();
            }
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC mv{};
                mv.Format = DXGI_FORMAT_R8G8_UNORM;
                mv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                mv.Texture2D.MipLevels = 1;
                ID3D11ShaderResourceView* msrv = nullptr;
                if (FAILED(g_device->CreateShaderResourceView(g_encoded_mv, &mv, &msrv))) { g_status = "motion view failed"; break; }
                run_cs(ctx, g_decode_cs, msrv, g_mv.uav, w, h);
                msrv->Release();
            }
            if (g_cammv_mode.load() != 0 && g_cam_valid && g_cammv_cs && make_tex(g_mv_cam, w, h, DXGI_FORMAT_R16G16_FLOAT)) {
                if (!g_cammv_cb) {
                    D3D11_BUFFER_DESC bd{};
                    bd.ByteWidth = 19 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    g_device->CreateBuffer(&bd, nullptr, &g_cammv_cb);
                }
                if (g_cammv_cb) {
                    float cb[76]{};
                    std::memcpy(cb, g_cam_rows, 32 * sizeof(float));
                    std::memcpy(cb + 40, g_cam_rows + 32, 16 * sizeof(float));   // rr0-rr3 after prm, prm2
                    std::memcpy(cb + 56, g_body_rows, 16 * sizeof(float));       // afwvehmv: bb0-bb3
                    cb[72] = g_body_lim;                                          // prm3.x
                    cb[73] = g_mv_best.load() ? 1.0f : 0.0f;                     // prm3.y: dlssmvbest
                    cb[32] = g_cammv_sign.load();
                    cb[33] = g_cammv_mode.load() == 1 ? 1.0f : g_cammv_mode.load() == 3 ? 2.0f : 0.0f;
                    cb[34] = g_cammv_mode.load() == 3 ? 1.5f : 2.0f;    // px: differences below this are the game vectors' coarseness
                    cb[35] = 0.15f;   // plus 15 % of the camera motion
                    cb[36] = g_objmv_scale.load();
                    // AFW object motion scale: the weapon pass (stencil 0x40) keeps its own vectors (they already span the eye's history).
                    ID3D11ShaderResourceView* ssrv = nullptr;
                    if ((cb[36] != 1.0f || g_mv_best.load() || g_body_lim > 0.0f) && g_depth && !g_objmv_gun.load()) {
                        D3D11_TEXTURE2D_DESC dd{};
                        static_cast<ID3D11Texture2D*>(g_depth)->GetDesc(&dd);
                        DXGI_FORMAT sf = DXGI_FORMAT_UNKNOWN;
                        if (dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS || dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) sf = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
                        if (dd.Format == DXGI_FORMAT_R24G8_TYPELESS || dd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) sf = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
                        if (sf != DXGI_FORMAT_UNKNOWN && (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
                            sv.Format = sf; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
                            if (FAILED(g_device->CreateShaderResourceView(g_depth, &sv, &ssrv))) ssrv = nullptr;
                        }
                    }
                    cb[37] = ssrv ? 64.0f : 0.0f;
                    // dlssmvcheck: this eye's depth of its previous evaluation (not after a history reset or a size change).
                    const int  pe   = eye & 1;
                    const bool pchk = g_mv_check.load() && g_mv_best.load() && !reset && g_prev_depth_ok[pe] && g_prev_depth[pe].w == w && g_prev_depth[pe].h == h;
                    cb[74] = pchk ? 1.0f : 0.0f;                                  // prm3.z
                    cb[75] = std::max(0.0f, g_mv_sway.load());                    // prm3.w: dlssmvsway
                    ctx->UpdateSubresource(g_cammv_cb, 0, nullptr, cb, 0, 0);
                    ID3D11ComputeShader*       prev_cs = nullptr;
                    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
                    ID3D11ShaderResourceView*  srv[4]{ g_depth32.srv, g_mv.srv, ssrv, pchk ? g_prev_depth[pe].srv : nullptr };
                    ctx->CSSetShader(g_cammv_cs, nullptr, 0);
                    ctx->CSSetShaderResources(0, 4, srv);
                    if (ssrv) ssrv->Release();   // the binding keeps it alive until unbound below
                    ctx->CSSetConstantBuffers(0, 1, &g_cammv_cb);
                    ID3D11UnorderedAccessView* uavs[2]{ g_mv_cam.uav, make_tex(g_mv_camonly, w, h, DXGI_FORMAT_R16G16_FLOAT) ? g_mv_camonly.uav : nullptr };
                    ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
                    g_camonly_valid = uavs[1] != nullptr;
                    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                    ID3D11ShaderResourceView*  null_srv[4]{};
                    ID3D11UnorderedAccessView* null_uav[2]{};
                    ID3D11Buffer*              null_cb  = nullptr;
                    ctx->CSSetShaderResources(0, 4, null_srv);
                    ctx->CSSetUnorderedAccessViews(0, 2, null_uav, nullptr);
                    // This evaluation's depth is the next one's previous depth for this eye.
                    if (make_tex(g_prev_depth[pe], w, h, DXGI_FORMAT_R32_FLOAT)) {
                        ctx->CopyResource(g_prev_depth[pe].tex, g_depth32.tex);
                        g_prev_depth_ok[pe] = true;
                    }
                    ctx->CSSetConstantBuffers(0, 1, &null_cb);
                    ctx->CSSetShader(prev_cs, nullptr, 0);
                    if (prev_cs) prev_cs->Release();
                    if (g_mv_compare.load() > 0) {
                        // Test: the game's vector next to the camera one at a few points (pixels).
                        g_mv_compare--;
                        D3D11_TEXTURE2D_DESC sd{};
                        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R16G16_FLOAT; sd.SampleDesc.Count = 1;
                        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ID3D11Texture2D* sg = nullptr;
                        ID3D11Texture2D* sc = nullptr;
                        g_device->CreateTexture2D(&sd, nullptr, &sg);
                        g_device->CreateTexture2D(&sd, nullptr, &sc);
                        if (sg && sc) {
                            ctx->CopyResource(sg, g_mv.tex);
                            ctx->CopyResource(sc, g_mv_cam.tex);
                            D3D11_MAPPED_SUBRESOURCE mg{}, mc{};
                            if (SUCCEEDED(ctx->Map(sg, 0, D3D11_MAP_READ, 0, &mg)) && SUCCEEDED(ctx->Map(sc, 0, D3D11_MAP_READ, 0, &mc))) {
                                std::string line;
                                for (const float fy : { 0.35f, 0.45f, 0.6f }) {
                                    for (const float fx : { 0.2f, 0.5f, 0.8f }) {
                                        const UINT x = UINT(fx * w), y = UINT(fy * h);
                                        auto rd  = [&](const D3D11_MAPPED_SUBRESOURCE& m) {
                                            const uint16_t* p = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch) + x * 2;
                                            return std::pair{ glm::unpackHalf1x16(p[0]), glm::unpackHalf1x16(p[1]) };
                                        };
                                        const auto g = rd(mg), c = rd(mc);
                                        line += fmt::format(" ({:.2f},{:.2f}) game ({:.2f},{:.2f}) camera ({:.2f},{:.2f}) |", fx, fy, g.first, g.second, c.first, c.second);
                                    }
                                }
                                spdlog::info("[DLSS] motion vectors (px):{}", line);
                                ctx->Unmap(sg, 0);
                                ctx->Unmap(sc, 0);
                            }
                        }
                        if (sg) sg->Release();
                        if (sc) sc->Release();
                    }
                    if (g_mv_dump.load() > 0 && g_camonly_valid) {
                        // Test (cmd dlssmvdump N): the game's decoded vectors, the camera motion with the move, the vectors DLSS gets
                        // (R16G16F, px) and the depth (R32F) of this eye, raw rows, plus the camera rows: probes\mvdump_<eye>_*.raw
                        // (tools\dlss\mvdump.py reads them).
                        // More than one pair (dlssmvdump N > 2): each evaluation's files get its number (mvdump_q<k>_...), so
                        // consecutive frames can be compared (an object's real move against its game vector).
                        const int   left = --g_mv_dump;
                        const int   total = g_mv_dump_total.load();
                        const std::string pre = total > 2 ? fmt::format("mvdump_q{:02}_", total - 1 - left) : std::string("mvdump_");
                        const char* names[4]{ "game", "cam", "final", "depth" };
                        Tex*        texs[4]{ &g_mv, &g_mv_camonly, &g_mv_cam, &g_depth32 };
                        for (int i = 0; i < 4; ++i) {
                            D3D11_TEXTURE2D_DESC sd{};
                            texs[i]->tex->GetDesc(&sd);
                            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.BindFlags = 0; sd.MiscFlags = 0;
                            ID3D11Texture2D* st = nullptr;
                            if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &st))) continue;
                            ctx->CopyResource(st, texs[i]->tex);
                            D3D11_MAPPED_SUBRESOURCE m{};
                            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                                if (std::FILE* f = std::fopen(fmt::format("D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/{}{}_{}_{}x{}.raw", pre, eye, names[i], sd.Width, sd.Height).c_str(), "wb")) {
                                    for (UINT y = 0; y < sd.Height; ++y) std::fwrite(static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, 1, size_t(sd.Width) * 4, f);
                                    std::fclose(f);
                                }
                                ctx->Unmap(st, 0);
                            }
                            st->Release();
                        }
                        if (std::FILE* f = std::fopen(fmt::format("D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/{}{}_rows.txt", pre, eye).c_str(), "w")) {
                            for (int i = 0; i < 48; ++i) std::fprintf(f, "%.9g%c", g_cam_rows[i], (i % 4 == 3) ? '\n' : ' ');
                            std::fprintf(f, "mode %d best %d check %d sign %g\n", g_cammv_mode.load(), g_mv_best.load() ? 1 : 0, g_mv_check.load() ? 1 : 0, g_cammv_sign.load());
                            std::fclose(f);
                        }
                        spdlog::info("[DLSS] mvdump eye {}: game, camera and final vectors and depth written to probes", eye);
                    }
                    if (g_mv_stat.load() > 0 && g_camonly_valid) {
                        // Test (cmd dlssmvstat N, the next N evaluations): on how much of the eye the final vector is not the
                        // head motion (the game's vector was taken), and the mean distance between the two there.
                        g_mv_stat--;
                        D3D11_TEXTURE2D_DESC sd{};
                        sd.Width = w; sd.Height = h; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R16G16_FLOAT; sd.SampleDesc.Count = 1;
                        sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ID3D11Texture2D* sf = nullptr;
                        ID3D11Texture2D* sc = nullptr;
                        g_device->CreateTexture2D(&sd, nullptr, &sf);
                        g_device->CreateTexture2D(&sd, nullptr, &sc);
                        if (sf && sc) {
                            ctx->CopyResource(sf, g_mv_cam.tex);
                            ctx->CopyResource(sc, g_mv_camonly.tex);
                            D3D11_MAPPED_SUBRESOURCE mf{}, mc{};
                            if (SUCCEEDED(ctx->Map(sf, 0, D3D11_MAP_READ, 0, &mf)) && SUCCEEDED(ctx->Map(sc, 0, D3D11_MAP_READ, 0, &mc))) {
                                uint64_t n = 0, used = 0;
                                double   dist = 0.0, cam = 0.0;
                                // Where: a map at 1/16 size, white = the game's vector taken (probes/mvstat_<eye>.pgm, the last one kept).
                                const UINT           mw = w / 16, mh = h / 16;
                                std::vector<uint8_t> map(size_t(mw) * mh, 0);
                                for (UINT y = 0; y < h; y += 2) {
                                    const uint16_t* pf = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mf.pData) + y * mf.RowPitch);
                                    const uint16_t* pc = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mc.pData) + y * mc.RowPitch);
                                    for (UINT x = 0; x < w; x += 2) {
                                        const float fx = glm::unpackHalf1x16(pf[x * 2]), fy = glm::unpackHalf1x16(pf[x * 2 + 1]);
                                        const float cx = glm::unpackHalf1x16(pc[x * 2]), cy = glm::unpackHalf1x16(pc[x * 2 + 1]);
                                        const float d  = std::hypot(fx - cx, fy - cy);
                                        n++;
                                        cam += std::hypot(cx, cy);
                                        if (d > 0.01f) {
                                            used++;
                                            dist += d;
                                            if (x / 16 < mw && y / 16 < mh) { auto& m = map[size_t(y / 16) * mw + x / 16]; m = uint8_t(std::min(255, m + 4)); }
                                        }
                                    }
                                }
                                if (std::FILE* f = std::fopen(fmt::format("D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/mvstat_{}.pgm", eye).c_str(), "wb")) {
                                    std::fprintf(f, "P5\n%u %u\n255\n", mw, mh);
                                    std::fwrite(map.data(), 1, map.size(), f);
                                    std::fclose(f);
                                }
                                spdlog::info("[DLSS] mvstat eye {}: game vector on {:.2f}% of the image (mean {:.1f} px from the head motion there), head motion mean {:.1f} px, dlssmvbest {}",
                                             eye, n ? 100.0 * used / n : 0.0, used ? dist / used : 0.0, n ? cam / n : 0.0, g_mv_best.load() ? "on" : "off");
                                ctx->Unmap(sf, 0);
                                ctx->Unmap(sc, 0);
                            }
                        }
                        if (sf) sf->Release();
                        if (sc) sc->Release();
                    }
                    if (g_trims.load() & 1) mv_in = g_mv_cam.tex;   // trim 1: DLSS reads the camera motion as it is
                    else ctx->CopyResource(g_mv.tex, g_mv_cam.tex);
                }
            }
            if (g_debug.load() == 2) {
                // Zero motion: tells whether the decoded vectors are what blurs the result.
                const float zero[4]{};
                ctx->ClearUnorderedAccessViewFloat(g_mv.uav, zero);
            }
            if (g_debug.load() == 3) {
                // Show |motion| in pixels as grey (x10) instead of the image.
                run_cs(ctx, g_vis_cs, g_mv.srv, g_out.uav, w, h);
                ctx->CopyResource(target_res, g_out.tex);
                g_status = "debug: motion vectors";
                ok = true;
                break;
            }
            if (g_recreate.exchange(false)) {
                for (auto& f : g_feature) { if (f) NVSDK_NGX_D3D11_ReleaseFeature(f); f = nullptr; }
                for (auto& f : g_spare) { if (f) NVSDK_NGX_D3D11_ReleaseFeature(f); f = nullptr; }
            }
            g_render_w = w;
            const bool up = g_out_w > w && g_out_h > h;
            const UINT ow = up ? g_out_w : w, oh = up ? g_out_h : h;
            if (up && !make_tex(g_hi, ow, oh, work)) { g_status = "upscale texture allocation failed"; break; }
            if (g_feature[eye] && (g_feature_w[eye] != w || g_feature_h[eye] != h || g_feature_ow[eye] != ow) && g_spare[eye] &&
                g_spare_w[eye] == w && g_spare_h[eye] == h && g_spare_ow[eye] == ow) {
                // Back to the kept size: swap the two (its history is old, so it starts again).
                std::swap(g_feature[eye], g_spare[eye]);
                std::swap(g_feature_w[eye], g_spare_w[eye]);
                std::swap(g_feature_h[eye], g_spare_h[eye]);
                std::swap(g_feature_ow[eye], g_spare_ow[eye]);
                if (g_spare_reuses.fetch_add(1) < 8)
                    spdlog::info("[DLSS] feature for eye {} reused: {}x{} -> {}x{} (kept from before; {}x{} kept in its place)", eye, w, h, ow, oh, g_spare_w[eye], g_spare_h[eye]);
                reset = true;
            }
            if (!g_feature[eye] || g_feature_w[eye] != w || g_feature_h[eye] != h || g_feature_ow[eye] != ow) {
                if (g_spare[eye]) NVSDK_NGX_D3D11_ReleaseFeature(g_spare[eye]);
                g_spare[eye]    = g_feature[eye];   // kept for a switch back (a menu and the world)
                g_spare_w[eye]  = g_feature_w[eye];
                g_spare_h[eye]  = g_feature_h[eye];
                g_spare_ow[eye] = g_feature_ow[eye];
                g_feature[eye]  = nullptr;
                NVSDK_NGX_DLSS_Create_Params cp{};
                cp.Feature.InWidth = w; cp.Feature.InHeight = h;
                cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
                const float ratio = (float)w / (float)ow;
                cp.Feature.InPerfQualityValue = !up ? NVSDK_NGX_PerfQuality_Value_DLAA
                                              : ratio > 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                                              : ratio > 0.55f ? NVSDK_NGX_PerfQuality_Value_Balanced
                                                              : NVSDK_NGX_PerfQuality_Value_MaxPerf;
                cp.InFeatureCreateFlags = g_flags.load();
                // The DLSS model: 0 = the library's default, 10 = J, 11 = K (DLSS 4 transformer, needs a 310.x nvngx_dlss.dll).
                const int preset = g_preset.load();
                for (const char* p : { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                       NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                       NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality }) {
                    NVSDK_NGX_Parameter_SetUI(g_params, p, (unsigned int)preset);
                }
                const auto r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature[eye], g_params, &cp);
                if (NVSDK_NGX_FAILED(r)) { g_status = fmt::format("feature creation failed 0x{:x}", (uint32_t)r); g_feature[eye] = nullptr; break; }
                g_feature_w[eye] = w; g_feature_h[eye] = h; g_feature_ow[eye] = ow;
                spdlog::info("[DLSS] feature for eye {} created: {}x{} -> {}x{} ({}), colour format {}, preset {}, flags 0x{:x}", eye, w, h, ow, oh, up ? "upscaling" : "DLAA", (int)work, g_preset.load(),
                             g_flags.load());
                reset = true;
            }
            fc5::gputime::end(ctx, fc5::gputime::kDlssPrep);
            fc5::gputime::begin(ctx, fc5::gputime::kDlssEval);
            in_eval = true;
            NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
            ep.Feature.pInColor  = g_color_in.tex;
            ep.Feature.pInOutput = up ? g_hi.tex : g_out.tex;
            ep.pInDepth          = g_depth32.tex;
            ep.pInMotionVectors  = mv_in;
            ep.InJitterOffsetX   = jx;
            ep.InJitterOffsetY   = jy;
            ep.InRenderSubrectDimensions = { w, h };
            ep.InReset           = reset ? 1 : 0;
            ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
            {
                // ofxrguides: OFXR refuses a DLSS evaluation without a frame time as its frame generation's guides
                // ("invalid publication", 30 Oct); 0 was passed. The time since this eye's last evaluation, 1-100 ms.
                static LARGE_INTEGER last[2]{}, freq{};
                LARGE_INTEGER now{};
                if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&now);
                const float ms = last[eye].QuadPart ? float(double(now.QuadPart - last[eye].QuadPart) * 1000.0 / double(freq.QuadPart)) : 11.1f;
                last[eye] = now;
                ep.InFrameTimeDeltaInMsec = std::clamp(ms, 1.0f, 100.0f);
            }
            const auto r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_feature[eye], g_params, &ep);
            if (NVSDK_NGX_FAILED(r)) { g_status = fmt::format("evaluation failed 0x{:x}", (uint32_t)r); break; }
            keep_ofxr_receipt(eye, ep.Feature.pInOutput);
            if (up) {
                // The game's own low-resolution path goes on (SMAA, then the HUD); the eye image is
                // composed at present from the DLSS output and the HUD.
                if (g_sharpness.load() > 0.0f && g_sharpen_cs && make_tex(g_sharp, ow, oh, work)) {
                    sharpen(ctx, g_hi, g_sharp);
                    ctx->CopyResource(g_hi.tex, g_sharp.tex);
                }
                g_have_hi = true;
                g_status  = fmt::format("DLSS {}x{} -> {}x{} per eye", w, h, ow, oh);
                ok        = false;
                break;
            }
            // DLSS output (sharpened) -> the SMAA target (same size; formats of one family copy directly).
            if (g_sharpness.load() > 0.0f && g_sharpen_cs && make_tex(g_sharp, w, h, work)) {
                sharpen(ctx, g_out, g_sharp);
                ctx->CopyResource(target_res, g_sharp.tex);
            } else {
                ctx->CopyResource(target_res, g_out.tex);
            }
            g_status = fmt::format("DLAA {}x{} per eye", w, h);
            ok = true;
        } while (false);
        fc5::gputime::end(ctx, in_eval ? fc5::gputime::kDlssEval : fc5::gputime::kDlssPrep);
        if (color_res) color_res->Release();
        if (target_res) target_res->Release();
        return ok;
    }
}
