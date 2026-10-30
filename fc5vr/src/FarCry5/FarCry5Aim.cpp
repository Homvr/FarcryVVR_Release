#include "FarCry5Aim.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

#include <cstring>

namespace fc5::aim
{
namespace
{
    const char* kSrc = R"HLSL(
Texture2D<float> sceneDepth : register(t0);
cbuffer R : register(b0) {
    float4 r0, r1, r2, r3;   // view rotation-projection rows (clip = M * p, p camera-relative)
    float4 org;              // ray origin, camera-relative (xyz)
    float4 dir;              // ray direction (xyz), depth a (w)
    float4 prm;              // depth b, width, height, nearest scene depth counted (m: nearer is the gun)
    float4 lim;              // first and last distance marched (m); arc: gravity (z), time step (w)
};
float4 vsmain(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.5, 1.0); }
float psmain(float4 pos : SV_Position) : SV_Target {
    float t = lim.x;
    [loop] for (int i = 0; i < 160; ++i) {
        float3 p = org.xyz + dir.xyz * t + float3(0.0, 0.0, -0.5 * lim.z * t * t);
        float4 c = float4(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w, dot(r3.xyz, p) + r3.w);
        if (c.w > 0.05) {
            float2 n = c.xy / c.w;
            if (all(abs(n) < 1.0)) {
                int2 px = int2((n.x * 0.5 + 0.5) * prm.y, (0.5 - n.y * 0.5) * prm.z);
                float z = sceneDepth.Load(int3(px, 0));
                if (z > 0.0) {
                    float sd = prm.x / (z + dir.w);   // the scene's view distance at that pixel
                    if (c.w >= sd && sd > prm.w) return t;
                }
            }
        }
        if (lim.z > 0.0) t += lim.w;
        else { t *= 1.045; t += 0.02; }
        if (t > lim.y) break;
    }
    return -1.0;
}
)HLSL";

    ID3D11VertexShader*       g_vs{ nullptr };
    ID3D11PixelShader*        g_ps{ nullptr };
    ID3D11Buffer*             g_cb{ nullptr };
    // Marches with their own results: 0 the barrel ray, 1 meleeswing's hand-motion ray, 2 throwv2's trajectory.
    struct Chan
    {
        ID3D11Texture2D*        rt{ nullptr };
        ID3D11RenderTargetView* rtv{ nullptr };
        ID3D11Texture2D*        staging[3]{};
        bool                    pending[3]{};
        int                     slot{ 0 };
        float                   last{ -1.0f };
        uint64_t                seq{ 0 }, slot_seq[3]{}, mark_seq{ 0 };   // dispatches, and the one each slot holds; mark(): results after it
        int                     since_n{ 0 };
        float                   since_min{ -1.0f };
    };
    Chan                      g_ch[3]{};
    ID3D11ShaderResourceView* g_srv{ nullptr };
    ID3D11Texture2D*          g_srv_of{ nullptr };
    ID3DDeviceContextState*   g_state{ nullptr };
    bool                      g_failed{ false };

    bool init(ID3D11Device* dev)
    {
        if (g_ps || g_failed) {
            return g_ps != nullptr;
        }
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto      fn    = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
        ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
        bool      ok = fn && SUCCEEDED(fn(kSrc, std::strlen(kSrc), "fc5aim", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vs, &err)) &&
                  SUCCEEDED(fn(kSrc, std::strlen(kSrc), "fc5aim", nullptr, nullptr, "psmain", "ps_5_0", 0, 0, &ps, &err));
        if (!ok && err) {
            spdlog::error("[Aim] shader: {}", static_cast<const char*>(err->GetBufferPointer()));
        }
        ok = ok && SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
        if (vs) vs->Release();
        if (ps) ps->Release();
        if (err) err->Release();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 8 * 16;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok                = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format           = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET;
        for (auto& ch : g_ch) ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &ch.rt)) && SUCCEEDED(dev->CreateRenderTargetView(ch.rt, nullptr, &ch.rtv));
        td.BindFlags      = 0;
        td.Usage          = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& ch : g_ch) for (auto& s : ch.staging) ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &s));
        ID3D11Device1* dev1 = nullptr;
        if (ok && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1)))) {
            const D3D_FEATURE_LEVEL lvl = dev->GetFeatureLevel();
            ok = SUCCEEDED(dev1->CreateDeviceContextState(0, &lvl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state));
            dev1->Release();
        } else {
            ok = false;
        }
        if (!ok) {
            spdlog::error("[Aim] barrel-ray depth march unavailable: shots converge at the fixed distance");
            g_failed = true;
            if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        }
        return ok;
    }
}

void march(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* depth, const Ray& ray, int chan)
{
    if (depth == nullptr || !init(dev)) {
        return;
    }
    auto& ch = g_ch[chan < 0 || chan > 2 ? 0 : chan];
    // Collect a finished result first (never wait on the GPU).
    for (int k = 0; k < 3; ++k) {
        const int s = (ch.slot + 1 + k) % 3;
        if (!ch.pending[s]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(ch.staging[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
            ch.last = *static_cast<const float*>(m.pData);
            if (ch.slot_seq[s] > ch.mark_seq) {
                ch.since_n++;
                if (ch.last > 0.0f && (ch.since_min < 0.0f || ch.last < ch.since_min)) ch.since_min = ch.last;
            }
            ctx->Unmap(ch.staging[s], 0);
            ch.pending[s] = false;
        }
    }
    if (ch.pending[ch.slot]) {
        return;   // all slots in flight
    }
    D3D11_TEXTURE2D_DESC dd{};
    depth->GetDesc(&dd);
    if (g_srv_of != depth) {
        if (g_srv) { g_srv->Release(); g_srv = nullptr; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format              = dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(dev->CreateShaderResourceView(depth, &sd, &g_srv))) {
            return;
        }
        g_srv_of = depth;
    }

    float cb[8][4]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            cb[r][c] = ray.view_rot_proj[c][r];
        }
    }
    cb[4][0] = ray.origin_rel.x; cb[4][1] = ray.origin_rel.y; cb[4][2] = ray.origin_rel.z;
    cb[5][0] = ray.dir.x; cb[5][1] = ray.dir.y; cb[5][2] = ray.dir.z; cb[5][3] = ray.depth_a;
    cb[6][0] = ray.depth_b; cb[6][1] = float(dd.Width); cb[6][2] = float(dd.Height); cb[6][3] = ray.near_skip;
    cb[7][0] = ray.start; cb[7][1] = ray.stop; cb[7][2] = ray.gravity; cb[7][3] = ray.tstep;

    ID3D11DeviceContext1*   ctx1 = nullptr;
    ID3DDeviceContextState* prev = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1)))) {
        return;
    }
    ctx1->SwapDeviceContextState(g_state, &prev);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, cb, sizeof(cb));
        ctx->Unmap(g_cb, 0);
    }
    D3D11_VIEWPORT vp{ 0, 0, 1, 1, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &ch.rtv, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->PSSetShaderResources(0, 1, &g_srv);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    ctx1->Release();

    ctx->CopyResource(ch.staging[ch.slot], ch.rt);
    ch.pending[ch.slot]  = true;
    ch.slot_seq[ch.slot] = ++ch.seq;
    ch.slot             = (ch.slot + 1) % 3;
}

float last_hit(int chan) { return g_ch[chan < 0 || chan > 2 ? 0 : chan].last; }
void  mark(int chan)
{
    auto& ch     = g_ch[chan < 0 || chan > 2 ? 0 : chan];
    ch.mark_seq  = ch.seq;
    ch.since_n   = 0;
    ch.since_min = -1.0f;
}
float hit_since_mark(int chan, int& results)
{
    const int c = chan < 0 || chan > 2 ? 0 : chan;
    results     = g_ch[c].since_n;
    return g_ch[c].since_min;
}
}
