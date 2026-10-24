#include "FarCry5MenuBlit.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace fc5::menublit
{
namespace
{
    const char* kSrc = R"(
struct V { float4 pos : SV_Position; float2 t : TEXCOORD0; };
V vsmain(uint id : SV_VertexID)
{
    V o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos    = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.t      = p;
    return o;
}

// Pass 1: the video frame from its planes, as the game's YUV shaders (#D406E828, #4C856BAF) convert them (BT.601,
// limited range). pa: chroma uv scale (xy), plane value in alpha (z: bit 0 luma, 1 Cr, 2 Cb), w unused.
// pb: ColorMultiplier / ColorAdd registers of the game's constants (x, y; negative: none).
cbuffer P : register(b0) { float4 pa; float4 pb; };
cbuffer G : register(b1) { float4 g[40]; };
Texture2D    ty  : register(t0);
Texture2D    tcr : register(t1);
Texture2D    tcb : register(t2);
SamplerState smp : register(s0);
float plane(Texture2D t, float2 uv, uint bit) { float4 v = t.SampleLevel(smp, uv, 0); return ((uint)pa.z & bit) ? v.a : v.r; }
float4 psconv(V i) : SV_Target
{
    const float y  = saturate(plane(ty, i.t, 1) * 1.164384 - 0.073059);
    const float cr = plane(tcr, i.t * pa.xy, 2);
    const float cb = plane(tcb, i.t * pa.xy, 4);
    float3 rgb = saturate(float3(y + 1.402 * cr - 0.701, y - 0.71414 * cr - 0.34414 * cb + 0.52914, y + 1.772 * cb - 0.886));
    if (pb.x >= 0 && pb.y >= 0) rgb *= saturate(g[(uint)pb.x].w + g[(uint)pb.y].w);   // the game's fade (alpha)
    return float4(rgb, 1);
}

// Pass 2: cover: uv = cover.xy + t * cover.zw (t = 0..1 over the target). fit: the whole frame in the rectangle
// fit.xy + t * fit.zw of the target, the rest a dimmed, blurred cover of it (prm.y dim, prm.z blur mip, prm.w tap step).
cbuffer C : register(b0) { float4 cover; float4 fit; float4 prm; };
Texture2D src : register(t0);
float4 psfit(V i) : SV_Target
{
    const float2 uc = cover.xy + i.t * cover.zw;
    if (prm.x < 0.5) return float4(src.SampleLevel(smp, uc, 0).rgb, 1);
    const float2 uf = (i.t - fit.xy) / fit.zw;
    if (all(uf >= 0.0) && all(uf <= 1.0)) return float4(src.SampleLevel(smp, uf, 0).rgb, 1);
    float3 b = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) b += src.SampleLevel(smp, uc + float2(x, y) * prm.w, prm.z).rgb;
    return float4(b * (prm.y / 25.0), 1);
}
)";

    ID3D11VertexShader*       g_vs{ nullptr };
    ID3D11PixelShader*        g_ps_conv{ nullptr };
    ID3D11PixelShader*        g_ps_fit{ nullptr };
    ID3D11Buffer*             g_cb{ nullptr };
    ID3D11SamplerState*       g_smp{ nullptr };
    ID3DDeviceContextState*   g_state{ nullptr };
    ID3D11Texture2D*          g_rgb{ nullptr };   // the converted frame, with mips for the blurred bands
    ID3D11ShaderResourceView* g_rgb_srv{ nullptr };
    ID3D11RenderTargetView*   g_rgb_rtv{ nullptr };
    UINT                      g_rgb_w{ 0 }, g_rgb_h{ 0 };
    bool                      g_failed{ false };
    int                       g_logged{ 0 };

    bool init(ID3D11Device* dev)
    {
        if (g_ps_fit || g_failed) return g_ps_fit != nullptr;
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto      fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
        ID3DBlob *vs = nullptr, *pc = nullptr, *pf = nullptr, *err = nullptr;
        bool      ok = fn && SUCCEEDED(fn(kSrc, std::strlen(kSrc), "fc5menu", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vs, &err)) &&
                  SUCCEEDED(fn(kSrc, std::strlen(kSrc), "fc5menu", nullptr, nullptr, "psconv", "ps_5_0", 0, 0, &pc, &err)) &&
                  SUCCEEDED(fn(kSrc, std::strlen(kSrc), "fc5menu", nullptr, nullptr, "psfit", "ps_5_0", 0, 0, &pf, &err));
        if (!ok && err) spdlog::error("[Menu] video shader: {}", static_cast<const char*>(err->GetBufferPointer()));
        ok = ok && SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &g_ps_conv)) &&
             SUCCEEDED(dev->CreatePixelShader(pf->GetBufferPointer(), pf->GetBufferSize(), nullptr, &g_ps_fit));
        for (auto* b : { vs, pc, pf, err }) if (b) b->Release();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 48;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, &g_smp));
        ID3D11Device1* dev1 = nullptr;
        if (ok && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
            const D3D_FEATURE_LEVEL lvl = dev->GetFeatureLevel();
            ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &lvl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state));
            dev1->Release();
        } else {
            ok = false;
        }
        if (!ok) {
            spdlog::error("[Menu] menu video redraw unavailable");
            g_failed = true;
            if (g_ps_fit) { g_ps_fit->Release(); g_ps_fit = nullptr; }
        }
        return ok;
    }

    bool size_of(ID3D11ShaderResourceView* v, D3D11_TEXTURE2D_DESC& d, DXGI_FORMAT& view_format)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        v->GetDesc(&vd);
        view_format = vd.Format;
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        ID3D11Texture2D* t = nullptr;
        const bool ok = r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t;
        if (ok) { t->GetDesc(&d); t->Release(); }
        if (r) r->Release();
        return ok;
    }

    void put(ID3D11DeviceContext* ctx, const float* data, size_t bytes)
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, data, bytes);
            ctx->Unmap(g_cb, 0);
        }
    }

    // The first few frames: the game's ColorMultiplier / ColorAdd (read back, a stall once), to check the fade.
    void log_game_constants(ID3D11DeviceContext* ctx, ID3D11Device* dev, ID3D11Buffer* game_cb, int mul, int add)
    {
        D3D11_BUFFER_DESC bd{};
        game_cb->GetDesc(&bd);
        if (bd.ByteWidth < UINT(std::max(mul, add) + 1) * 16) { spdlog::info("[Menu] video draw constants: {} bytes, too small", bd.ByteWidth); return; }
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth      = bd.ByteWidth;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* st = nullptr;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &st))) return;
        ctx->CopyResource(st, game_cb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const float* f = static_cast<const float*>(m.pData);
            spdlog::info("[Menu] video draw: ColorMultiplier ({:.2f}, {:.2f}, {:.2f}, {:.2f}), ColorAdd ({:.2f}, {:.2f}, {:.2f}, {:.2f})", f[mul * 4], f[mul * 4 + 1],
                         f[mul * 4 + 2], f[mul * 4 + 3], f[add * 4], f[add * 4 + 1], f[add * 4 + 2], f[add * 4 + 3]);
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
}

bool video(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* const yuv[3], ID3D11Buffer* game_cb, int mul, int add, bool fit)
{
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    ID3D11Texture2D* rt = nullptr;
    D3D11_TEXTURE2D_DESC rd{};
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt))) && rt) rt->GetDesc(&rd);
    if (res) res->Release();
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    bool done = false;
    do {
        D3D11_TEXTURE2D_DESC yd{}, crd{}, cbd{};
        DXGI_FORMAT          yf{}, crf{}, cbf{};
        if (!rt || !dev || rd.SampleDesc.Count != 1 || !init(dev)) break;
        if (!size_of(yuv[0], yd, yf) || !size_of(yuv[1], crd, crf) || !size_of(yuv[2], cbd, cbf) || !crd.Width || !crd.Height) break;
        if (!g_rgb || g_rgb_w != yd.Width || g_rgb_h != yd.Height) {
            if (g_rgb_rtv) { g_rgb_rtv->Release(); g_rgb_rtv = nullptr; }
            if (g_rgb_srv) { g_rgb_srv->Release(); g_rgb_srv = nullptr; }
            if (g_rgb) { g_rgb->Release(); g_rgb = nullptr; }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = yd.Width; td.Height = yd.Height; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_rgb))) break;
            if (FAILED(dev->CreateShaderResourceView(g_rgb, nullptr, &g_rgb_srv)) || FAILED(dev->CreateRenderTargetView(g_rgb, nullptr, &g_rgb_rtv))) {
                if (g_rgb_srv) { g_rgb_srv->Release(); g_rgb_srv = nullptr; }
                g_rgb->Release();
                g_rgb = nullptr;
                break;
            }
            g_rgb_w = yd.Width;
            g_rgb_h = yd.Height;
            spdlog::info("[Menu] video frame {}x{} (planes: luma format {}, Cr {}x{} format {}, Cb format {})", yd.Width, yd.Height, (int)yf, crd.Width, crd.Height, (int)crf, (int)cbf);
        }
        if (game_cb && g_logged < 6) log_game_constants(ctx, dev, game_cb, mul, add);
        ID3D11DeviceContext1*   ctx1 = nullptr;
        ID3DDeviceContextState* prev = nullptr;
        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) break;
        ctx1->SwapDeviceContextState(g_state, &prev);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &g_smp);
        // Pass 1: the frame from its planes. The chroma planes are half size, padded (960x544 for 1920x1080).
        {
            const auto alpha = [](DXGI_FORMAT f) { return f == DXGI_FORMAT_A8_UNORM; };
            const float pa[8]{ float(yd.Width) * 0.5f / float(crd.Width), float(yd.Height) * 0.5f / float(crd.Height),
                               float((alpha(yf) ? 1 : 0) | (alpha(crf) ? 2 : 0) | (alpha(cbf) ? 4 : 0)), 0.0f,
                               game_cb ? float(mul) : -1.0f, game_cb ? float(add) : -1.0f, 0.0f, 0.0f };
            put(ctx, pa, sizeof(pa));
            D3D11_VIEWPORT vp{ 0, 0, float(yd.Width), float(yd.Height), 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetRenderTargets(1, &g_rgb_rtv, nullptr);
            ID3D11Buffer* cbs[2]{ g_cb, game_cb };
            ctx->PSSetConstantBuffers(0, 2, cbs);
            ctx->PSSetShader(g_ps_conv, nullptr, 0);
            ctx->PSSetShaderResources(0, 3, yuv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none[3]{};
            ctx->PSSetShaderResources(0, 3, none);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            ctx->GenerateMips(g_rgb_srv);
        }
        // Pass 2: onto the game's target. Cover: the image's aspect cut from the middle of the frame. Fit: the whole frame,
        // as large as the image allows (a 16:9 frame on the tall headset image: full width, centred), the bands above and
        // below it a dimmed, blurred cover of the same frame.
        {
            const float img_a = float(rd.Width) / float(rd.Height), src_a = float(yd.Width) / float(yd.Height);
            float       map[12]{ 0, 0, 1, 1, 0, 0, 1, 1, fit ? 1.0f : 0.0f, 0.45f, 5.0f, 32.0f / float(yd.Width) };
            if (img_a < src_a) {
                map[2] = img_a / src_a; map[0] = 0.5f - map[2] * 0.5f;
                map[7] = img_a / src_a; map[5] = 0.5f - map[7] * 0.5f;
            } else {
                map[3] = src_a / img_a; map[1] = 0.5f - map[3] * 0.5f;
                map[6] = src_a / img_a; map[4] = 0.5f - map[6] * 0.5f;
            }
            put(ctx, map, sizeof(map));
            D3D11_VIEWPORT vp{ 0, 0, float(rd.Width), float(rd.Height), 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            ID3D11Buffer* cbs[2]{ g_cb, nullptr };
            ctx->PSSetConstantBuffers(0, 2, cbs);
            ctx->PSSetShader(g_ps_fit, nullptr, 0);
            ctx->PSSetShaderResources(0, 1, &g_rgb_srv);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->PSSetShaderResources(0, 1, &none);
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        ctx1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        ctx1->Release();
        done = true;
        if (g_logged++ < 3)
            spdlog::info("[Menu] video frame {}x{} redrawn {} the {}x{} image (format {})", yd.Width, yd.Height, fit ? "whole into" : "covering", rd.Width, rd.Height, (int)rd.Format);
    } while (false);
    if (dev) dev->Release();
    if (rt) rt->Release();
    rtv->Release();
    return done;
}
}
