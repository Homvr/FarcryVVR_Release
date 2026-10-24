// shoreblur (F11 Experimental > Research 15 Oct > Water, cmd shoreblur on|off, shoreblurpx N): the pale stepped line where the
// water meets the bank (Homard 17 Oct 14:16 "white lines at the junction between water and shore", 14:18 "if you could smooth the
// white lines"). It is the game's own (the untouched game shows it at the same place, testruns\2026-10-17_waterbridge): the lake
// surface ends at the edge of the lake grid, drawn from the 2048x2048 lake images, so the edge comes in steps with a bright rim,
// and the headset shows it bigger and sharper than a flat screen. On: near that edge only, pale (white, grey) pixels brighter
// than the water next to them are pulled to the water's colour; everything else (grass, reeds, the bank) is left sharp. The edge is taken from WaterTessellatedUVTexture (the eye pixels the lake grid covers, UV 1,1 where it does not),
// read by the reflection passes after the lake grid has drawn it for this eye.
#include "FarCry5Stereo.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

#include <spdlog/spdlog.h>

namespace
{
    const char* kShore = R"(
Texture2D<float4>   g_src  : register(t0);
Texture2D<float2>   g_mask : register(t1);
RWTexture2D<float4> g_dst  : register(u0);
cbuffer P : register(b0) { uint w; uint h; uint mw; uint mh; float rad; float strength; float pad0; float pad1; };
bool water(int2 q)
{
    q = clamp(q, int2(0, 0), int2(int(mw) - 1, int(mh) - 1));
    const float2 uv = g_mask.Load(int3(q, 0));
    return !((uv.x >= 0.9999 && uv.y >= 0.9999) || (uv.x <= 1e-5 && uv.y <= 1e-5));
}
float4 src(int2 q) { return g_src.Load(int3(clamp(q, int2(0, 0), int2(int(w) - 1, int(h) - 1)), 0)); }
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 c  = src(int2(p));
    const float2 sc = float2(mw, mh) / float2(w, h);
    const int2   mp = int2((float2(p) + 0.5) * sc);
    const bool   cw = water(mp);
    const int    R  = max(1, int(ceil(rad * sc.x)));
    // Quick look on a ring first: most pixels are far from any water edge.
    bool near_edge = false;
    [unroll] for (int k = 0; k < 8; ++k) {
        const float a  = 0.785398 * k;
        const int2  o  = int2(round(float2(cos(a), sin(a)) * R));
        const int2  o2 = int2(round(float2(cos(a), sin(a)) * (R * 0.5)));
        if (water(mp + o) != cw || water(mp + o2) != cw) near_edge = true;
    }
    if (!near_edge || !cw) { g_dst[p] = c; return; }   // the rim is on the water side; the bank (sand, grass) is never changed
    float best = 1e9;
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx)
            if (water(mp + int2(dx, dy)) != cw) best = min(best, float(dx * dx + dy * dy));
    if (best > 1e8) { g_dst[p] = c; return; }
    const float d   = sqrt(best) / max(sc.x, 1e-3);
    const float wgt = strength * saturate(1.0 - d / (rad + 1.0));
    // Only the pale rim is changed (Homard 17 Oct 15:57: "only the white halo around grass, not the grass itself"): the
    // reference is the darkest water colour on a ring around the pixel; a pixel brighter than it and nearly colourless
    // (white, grey) moves toward it. Grass, reeds and the bank (coloured, or darker than the water) stay as they are.
    float3 ref  = c.rgb;
    float  refl = 1e9;
    [unroll] for (int k2 = 0; k2 < 16; ++k2) {
        const float a2 = 0.392699 * k2;
        for (int r2 = 1; r2 <= 2; ++r2) {
            const int2 o = int2(round(float2(cos(a2), sin(a2)) * (rad + 2.0) * r2));
            if (!water(int2((float2(int2(p) + o) + 0.5) * sc))) continue;
            const float3 q = src(int2(p) + o).rgb;
            const float  l = dot(q, float3(0.299, 0.587, 0.114));
            if (l < refl) { refl = l; ref = q; }
        }
    }
    if (refl > 1e8) { g_dst[p] = c; return; }
    const float lc   = dot(c.rgb, float3(0.299, 0.587, 0.114));
    const float mx   = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    const float sat  = mx > 1e-4 ? (mx - mn) / mx : 0.0;
    const float halo = saturate((lc - refl - 0.03) / 0.10) * saturate((0.30 - sat) / 0.15);
    g_dst[p] = float4(lerp(c.rgb, ref, wgt * halo), c.a);
}
)";

    struct Cb { UINT w, h, mw, mh; float rad, strength, pad0, pad1; };

    constexpr int kMaxCs = 8;
    std::array<std::atomic<ID3D11ComputeShader*>, kMaxCs> g_mask_cs{};   // passes that read WaterTessellatedUVTexture
    std::array<std::atomic<UINT>, kMaxCs>                 g_mask_slot{};
    std::atomic<int>                                      g_mask_cs_n{ 0 };

    ID3D11ShaderResourceView*  g_mask_srv   = nullptr;   // this eye's mask (held), taken at the first reading pass
    int                        g_mask_frame = -1;
    ID3D11ComputeShader*       g_cs         = nullptr;
    ID3D11Buffer*              g_cb         = nullptr;
    bool                       g_failed     = false;
    ID3D11Texture2D*           g_for        = nullptr;   // the eye texture the copies below were made for (not held)
    D3D11_TEXTURE2D_DESC       g_desc{};
    ID3D11Texture2D*           g_stash      = nullptr;
    ID3D11ShaderResourceView*  g_srv        = nullptr;
    ID3D11Texture2D*           g_mid        = nullptr;
    ID3D11UnorderedAccessView* g_uav        = nullptr;
    bool                       g_tex_failed = false;
    uint64_t                   g_drawn = 0, g_no_mask = 0, g_captures = 0;
    UINT                       g_mw = 0, g_mh = 0;

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

    void release_textures()
    {
        if (g_srv) g_srv->Release();
        if (g_stash) g_stash->Release();
        if (g_uav) g_uav->Release();
        if (g_mid) g_mid->Release();
        g_srv = nullptr; g_stash = nullptr; g_uav = nullptr; g_mid = nullptr;
    }

    bool ready(ID3D11Device* dev, ID3D11Texture2D* eye)
    {
        if (g_failed) return false;
        if (!g_cs) {
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!compile_fn || FAILED(compile_fn(kShore, strlen(kShore), "shoreblur", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
                spdlog::error("[Shore] shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
            code->Release();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(Cb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&bd, nullptr, &g_cb);
            if (!g_cs || !g_cb) { g_failed = true; return false; }
        }
        D3D11_TEXTURE2D_DESC d{};
        eye->GetDesc(&d);
        if (eye != g_for || d.Width != g_desc.Width || d.Height != g_desc.Height || d.Format != g_desc.Format) {
            release_textures();
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
            spdlog::info("[Shore] eye picture {}x{} format {} (samples {}): {}", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, g_tex_failed ? "no copies (off)" : "ready");
        }
        return !g_tex_failed && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }
}

// From note_compute_shader (shader creation): a pass that reads the lake grid's per-eye mask, and the mask's slot.
void FarCry5Stereo::shore_note_cs(ID3D11ComputeShader* cs, UINT slot)
{
    const int i = g_mask_cs_n.load();
    if (!cs || i >= kMaxCs) return;
    g_mask_slot[i] = slot;
    g_mask_cs[i]   = cs;
    g_mask_cs_n    = i + 1;
}

// At a dispatch (render thread, shoreblur on): the first pass of this eye's call that reads the mask hands it over.
void FarCry5Stereo::shore_capture(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs)
{
    if (g_mask_frame == m_frame.load()) return;
    const int n = g_mask_cs_n.load();
    for (int i = 0; i < n; ++i) {
        if (g_mask_cs[i].load() != cs) continue;
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->CSGetShaderResources(g_mask_slot[i].load(), 1, &srv);
        if (!srv) return;
        if (g_mask_srv) g_mask_srv->Release();
        g_mask_srv   = srv;
        g_mask_frame = m_frame.load();
        g_captures++;
        return;
    }
}

// Before the lens, the dot and the fade: this eye's picture, blurred across the water's edge.
void FarCry5Stereo::shore_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    if (!m_shore_blur.load() || !ctx || !eye || m_menu_screen_on) return;
    if (!g_mask_srv || g_mask_frame != m_frame.load()) { g_no_mask++; return; }
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = ready(dev, eye);
    dev->Release();
    if (!ok) return;
    ID3D11Resource* mr = nullptr;
    g_mask_srv->GetResource(&mr);
    ID3D11Texture2D* mt = nullptr;
    if (mr) { mr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&mt)); mr->Release(); }
    if (!mt) return;
    D3D11_TEXTURE2D_DESC md{};
    mt->GetDesc(&md);
    mt->Release();
    g_mw = md.Width; g_mh = md.Height;
    const Cb cb{ g_desc.Width, g_desc.Height, md.Width, md.Height, std::clamp(m_shore_blur_px.load(), 1.0f, 8.0f), 1.0f, 0.0f, 0.0f };
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    ctx->CopyResource(g_stash, eye);
    ID3D11ComputeShader*       prev_cs     = nullptr;
    ID3D11ShaderResourceView*  prev_srv[2] = {};
    ID3D11UnorderedAccessView* prev_uav    = nullptr;
    ID3D11Buffer*              prev_cb     = nullptr;
    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 2, prev_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
    ctx->CSGetConstantBuffers(0, 1, &prev_cb);
    ID3D11ShaderResourceView* srvs[2]{ g_srv, g_mask_srv };
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch((g_desc.Width + 7) / 8, (g_desc.Height + 7) / 8, 1);
    ID3D11ShaderResourceView*  null_srv[2] = {};
    ID3D11UnorderedAccessView* null_uav    = nullptr;
    ctx->CSSetShaderResources(0, 2, null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(prev_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, prev_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &prev_cb);
    if (prev_cs) prev_cs->Release();
    for (auto* s : prev_srv) if (s) s->Release();
    if (prev_uav) prev_uav->Release();
    if (prev_cb) prev_cb->Release();
    ctx->CopyResource(eye, g_mid);
    g_drawn++;
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
        last = std::chrono::steady_clock::now();
        spdlog::info("[Shore] edge blur: {} eye pictures ({} px), {} without this eye's water mask; mask {}x{} from {} pass(es), {} captures",
                     g_drawn, m_shore_blur_px.load(), g_no_mask, g_mw, g_mh, g_mask_cs_n.load(), g_captures);
    }
}
