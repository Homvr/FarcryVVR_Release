// vehfade: each eye's picture darkened (to black) while getting in or out of a vehicle, until the camera the game culls
// with is back on the head (Homard 14 Oct: "culling everywhere" when getting in; "just make a fade to black").
// vehvignette (FarCry5VehComfort.cpp): the same pass darkens only the picture's edges, by the angle from the eye's own
// view axis (its XR field of view), while the vehicle moves hard.
#include "FarCry5Stereo.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

namespace
{
    const char* kFade = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);
cbuffer P : register(b0) { float k; uint w; uint h; float vig; float4 tn; float r0; float r1; float2 pad; };
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    const float4 c = g_src.Load(int3(p, 0));
    float m = k;
    if (vig > 0.0) {
        // tn: the tangents of the eye's left, right, up and down edges; the angle from its view axis.
        const float2 uv = (float2(p) + 0.5) / float2(w, h);
        const float  tx = lerp(tn.x, tn.y, uv.x), ty = lerp(tn.z, tn.w, uv.y);
        m *= 1.0 - smoothstep(r0, r1, atan(sqrt(tx * tx + ty * ty)));
    }
    g_dst[p] = float4(c.rgb * m, c.a);
}
)";

    struct Cb { float k; UINT w, h; float vig; float tn[4]; float r0, r1, pad[2]; };

    ID3D11ComputeShader*       g_cs     = nullptr;
    ID3D11Buffer*              g_cb     = nullptr;
    bool                       g_failed = false;
    ID3D11Texture2D*           g_for    = nullptr;   // the eye texture the copies below were made for (not held)
    D3D11_TEXTURE2D_DESC       g_desc{};
    ID3D11Texture2D*           g_stash  = nullptr;
    ID3D11ShaderResourceView*  g_srv    = nullptr;
    ID3D11Texture2D*           g_mid    = nullptr;
    ID3D11UnorderedAccessView* g_uav    = nullptr;
    bool                       g_tex_failed = false;
    uint64_t                   g_drawn  = 0;

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
            if (!compile_fn || FAILED(compile_fn(kFade, strlen(kFade), "fade", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[Fade] shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
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
            spdlog::info("[Fade] eye picture {}x{} format {} (samples {}): {}", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, g_tex_failed ? "no copies (black only)" : "ready");
        }
        return !g_tex_failed && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }
}

// Once per present (on_post_present): the fade's target and level. Black while the entry / exit animation plays (the
// vehicle "settle"), then held until the culled camera is back on the head (cull_coverage: 6 frames with the eye's centre
// within 4 deg of the game camera's) or 1 s; to black in 0.15 s, back in 0.35 s.
void FarCry5Stereo::fade_tick(float dt)
{
    const auto now  = std::chrono::steady_clock::now();
    bool       want = m_veh_fade.load() && m_motion.load() && ((m_in_vehicle.load() && m_veh_settle.load() > 0 && m_veh_settle_entry) || (!m_in_vehicle.load() && m_exit_settle > 0));
    if (m_fade_was_want && !want && m_veh_fade.load()) {
        m_fade_hold     = true;
        m_fade_hold_t0  = now;
        m_fade_cov_good = 0;
    }
    if (want && !m_fade_was_want) spdlog::info("[Fade] getting {} a vehicle: picture to black", m_in_vehicle.load() ? "into" : "out of");
    m_fade_was_want = want;
    if (m_fade_hold) {
        const bool done = m_fade_cov_good >= 6 || now - m_fade_hold_t0 > std::chrono::seconds(1);
        if (done) {
            spdlog::info("[Fade] back in after {:.2f} s ({})", std::chrono::duration<float>(now - m_fade_hold_t0).count(),
                         m_fade_cov_good >= 6 ? "the culled camera is on the head" : "time limit");
            m_fade_hold = false;
        } else {
            want = true;
        }
    }
    if (m_menu_screen_on || !m_veh_fade.load()) { want = false; m_fade_hold = false; }
    m_fade_level = want ? std::min(1.0f, m_fade_level + dt / 0.15f) : std::max(0.0f, m_fade_level - dt / 0.35f);
    // vehvignette: in a vehicle, in in 0.15 s, out in 0.6 s.
    const bool  vig_on = m_veh_vignette.load() > 0.001f && m_motion.load() && m_in_vehicle.load() && m_veh_settle.load() == 0 && !m_menu_screen_on;
    const float vig_t  = vig_on ? m_vig_target : 0.0f;
    m_vig_level        = vig_t > m_vig_level ? std::min(vig_t, m_vig_level + dt / 0.15f) : std::max(vig_t, m_vig_level - dt / 0.6f);
}

// After the aim marker, before the F11 menu: the eye picture times (1 - level).
void FarCry5Stereo::fade_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    const float vig = std::clamp(m_veh_vignette.load(), 0.0f, 1.0f) * m_vig_level;   // vehvignette
    if (!ctx || !eye || (m_fade_level <= 0.001f && vig <= 0.01f)) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = ready(dev, eye);
    dev->Release();
    if (!ok) return;
    Cb cb{ 1.0f - std::clamp(m_fade_level, 0.0f, 1.0f), g_desc.Width, g_desc.Height, 0.0f, { -1.0f, 1.0f, 1.0f, -1.0f }, 0.0f, 0.0f, { 0.0f, 0.0f } };
    if (vig > 0.01f) {
        // The clear area's half-angle from 60 deg (a touch of shade at the far edges) down to 25 deg, its edge 12 deg wide.
        const int   fr = m_frame.load();
        const auto& v  = m_xr_views[fr & 7];
        const int   e  = fr & 1;
        if (v.frame == fr)
            for (int i = 0; i < 4; ++i) cb.tn[i] = std::tan(v.fov[e][i]);
        cb.vig = 1.0f;
        cb.r0  = glm::radians(60.0f - 35.0f * vig);
        cb.r1  = cb.r0 + glm::radians(12.0f);
    }
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    ctx->CopyResource(g_stash, eye);
    ID3D11ComputeShader*       prev_cs  = nullptr;
    ID3D11ShaderResourceView*  prev_srv = nullptr;
    ID3D11UnorderedAccessView* prev_uav = nullptr;
    ID3D11Buffer*              prev_cb  = nullptr;
    ctx->CSGetShader(&prev_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &prev_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
    ctx->CSGetConstantBuffers(0, 1, &prev_cb);
    ctx->CSSetShader(g_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &g_cb);
    ctx->Dispatch((g_desc.Width + 7) / 8, (g_desc.Height + 7) / 8, 1);
    ID3D11ShaderResourceView*  null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &null_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
    ctx->CSSetShader(prev_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &prev_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &prev_cb);
    if (prev_cs) prev_cs->Release();
    if (prev_srv) prev_srv->Release();
    if (prev_uav) prev_uav->Release();
    if (prev_cb) prev_cb->Release();
    ctx->CopyResource(eye, g_mid);
    if ((++g_drawn % 90) == 1) spdlog::info("[Fade] {} eye pictures darkened (level now {:.2f}, vignette {:.2f})", g_drawn, m_fade_level, vig);
}
