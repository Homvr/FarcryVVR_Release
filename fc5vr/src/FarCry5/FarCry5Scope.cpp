#include "FarCry5Scope.h"
#include "FarCry5Stereo.h"
#include "FarCry5Aim.h"
#include "FarCry5Dlss.h"
#include "FarCry5Afw.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include <glm/gtc/matrix_access.hpp>
#include <spdlog/spdlog.h>

namespace fc5::scope { extern glm::mat4 g_clean_vrp[2]; }   // scope_keep_clean (end of this file)

namespace fc5::scope
{
    namespace
    {
        const char* kLens = R"(
Texture2D<float4>   g_src : register(t0);
Texture2D<float4>   g_scope : register(t1);
RWTexture2D<float4> g_dst : register(u0);
SamplerState        g_lin : register(s0);
cbuffer P : register(b0)
{
    float2 lens_c; float lens_r; float ring_r;
    float2 src_c;  float inv_mag; float ret_w;
    float2 vig_c;  float vig_r;  float vig_soft;
    float2 origin; float2 size;
    float2 scope_k; float scope_on; float outside;
};
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 p = id + (uint2)origin;
    if (p.x >= (uint)size.x || p.y >= (uint)size.y) return;
    const float2 q = float2(p) + 0.5f;
    const float2 o = q - lens_c;
    const float  d = length(o);
    if (d > ring_r) {
        if (outside > 0.0f) g_dst[p] = float4(0.0f, 0.0f, 0.0f, 1.0f);   // scopesrc 1: only the lens in this eye
        return;
    }
    float3 c = 0.0f;   // the tube's black ring
    if (d < lens_r) {
        // The image around the aim point, magnified: the lens centre shows the aim point. With the scope pass, the
        // scope call's own picture (centred on the aim point when it was drawn); else a zoom of this eye's image.
        if (scope_on > 1.5f) c = g_src.SampleLevel(g_lin, 0.5f + o * scope_k, 0).rgb;
        else if (scope_on > 0.0f) c = g_scope.SampleLevel(g_lin, 0.5f + o * scope_k, 0).rgb;
        else c = g_src.SampleLevel(g_lin, (src_c + o * inv_mag) / size, 0).rgb;
        // Eyebox: dark outside the circle the exit pupil leaves visible (the eye off the scope's axis).
        c *= saturate((vig_r - length(q - vig_c)) / max(vig_soft, 1.0f));
        // Soft lens edge.
        c *= saturate((lens_r - d) / 2.0f);
        if (ret_w > 0.0f) {
            // Reticle: thin crosshair with a gap round the centre, thick posts on the outer part, red centre dot.
            const float2 a = abs(o);
            const bool thin = (a.x < ret_w && a.y > 4.0f * ret_w) || (a.y < ret_w && a.x > 4.0f * ret_w);
            const bool post = (a.x < 3.0f * ret_w && a.y > 0.55f * lens_r) || (a.y < 3.0f * ret_w && a.x > 0.55f * lens_r);
            if (thin || post) c = 0.0f;
            if (d < 1.6f * ret_w) c = float3(1.0f, 0.12f, 0.05f);
        }
    }
    g_dst[p] = float4(c, 1.0f);
}
)";
        struct Cb { float v[20]; };
        ID3D11ComputeShader*       g_cs      = nullptr;
        ID3D11Buffer*              g_cb      = nullptr;
        ID3D11SamplerState*        g_smp     = nullptr;
        bool                       g_failed  = false;
        ID3D11Texture2D*           g_stash   = nullptr;
        ID3D11ShaderResourceView*  g_srv     = nullptr;
        D3D11_TEXTURE2D_DESC       g_desc{};
        ID3D11Texture2D*           g_uav_of  = nullptr;   // held
        ID3D11UnorderedAccessView* g_uav     = nullptr;

        bool init(ID3D11Device* dev)
        {
            if (g_cs || g_failed) return g_cs != nullptr;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!compile_fn || FAILED(compile_fn(kLens, strlen(kLens), "scope_lens", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[Scope] lens shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
            code->Release();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(Cb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&bd, nullptr, &g_cb);
            D3D11_SAMPLER_DESC sd{};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;
            dev->CreateSamplerState(&sd, &g_smp);
            g_failed = !(g_cs && g_cb && g_smp);
            spdlog::info("[Scope] lens shader {}", g_failed ? "unavailable" : "ready");
            return !g_failed;
        }

        // scopemodel: the scope picture on the gun's eyepiece (see FarCry5Scope.h, ModelLens). Where the game drew the scope's
        // glass this frame (the glow draws' mask, its centre and size from glass_stats), the picture fills that glass, centred
        // on it and rolled with the gun; otherwise on the disc fixed to the gun (a pixel whose view ray meets it). The edge
        // shows the scene at the field's edge (the glass's angular radius / magnification), the reticle at the centre.
        const char* kModel = R"(
Texture2D<float4>   g_src   : register(t0);
Texture2D<float4>   g_scope : register(t1);
Texture2D<float>    g_mask  : register(t2);
ByteAddressBuffer   g_stats : register(t3);   // glass mask pixels: count, sum x, sum y (render pixels)
RWTexture2D<float4> g_dst   : register(u0);
SamplerState        g_lin   : register(s0);
cbuffer P : register(b0)
{
    float4 disc_r, disc_u, vig;   // the disc's right and up axes (world); eyebox circle centre (xy), radius, soft edge (disc radii)
    float4 b0, b1, b2;
    float4 a0, a1, a2;
    float4 disc_c;   // centre (offset from the eye), radius
    float4 disc_n;   // normal towards the eye, soft edge
    float2 origin; float2 size;
    float2 k;      float inv_mag; float mode;
    float  r_t;    float fade;    float ret_t; float eyebox;
    float2 mscale; float2 moff;   // eye pixel -> render pixel of the mask
    float2 msize;  float rc; float rs;   // mask size; the gun's roll on screen (cos, sin)
    float  tpp;    float gmode;   float gmin; float pad;   // eye tangent per eye pixel; glass mask in use; its fewest pixels
};
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 p = id + (uint2)origin;
    if (p.x >= (uint)size.x || p.y >= (uint)size.y) return;
    const float2 q = float2(p) + 0.5f;
    float        m  = 0.0f;
    float2       o  = 0.0f;   // the point on the eyepiece in its radii (x right, y up, the gun's roll)
    float        rt = r_t;    // the field at the edge
    bool         rim = true;
    const uint   cnt = g_stats.Load(0);
    if (gmode > 0.5f && cnt > (uint)gmin) {
        // The glass as the game drew it.
        const float2 qr = q * mscale + moff;
        m = saturate(g_mask.SampleLevel(g_lin, qr / msize, 0) * 1.5f);
        if (m <= 0.002f) return;
        const float2 cr = float2(g_stats.Load(4), g_stats.Load(8)) / (float)cnt + 0.5f;
        const float  rr = sqrt((float)cnt / 3.14159265f);   // its radius (render pixels); the field at its edge stays r_t
        float2       os = (qr - cr) / rr;
        os.y = -os.y;
        o    = float2(os.x * rc + os.y * rs, -os.x * rs + os.y * rc);
        rim  = false;
    } else {
        // The disc fixed to the gun: the view ray against it.
        const float3 n  = float3(q.x / size.x * 2.0f - 1.0f, 1.0f - q.y / size.y * 2.0f, 1.0f);
        const float3 w  = float3(dot(a0.xyz, n), dot(a1.xyz, n), dot(a2.xyz, n));
        const float  dn = dot(w, disc_n.xyz);
        if (dn < -1e-6f) {
            const float t = dot(disc_c.xyz, disc_n.xyz) / dn;
            if (t > 0.0f) {
                const float3 h = w * t - disc_c.xyz;
                m = saturate((disc_c.w - length(h)) / disc_n.w);
                o = float2(dot(h, disc_r.xyz), dot(h, disc_u.xyz)) / disc_c.w;
            }
        }
        if (m <= 0.002f) return;   // not on the eyepiece: the eye image stays as it is
    }
    const float4 orig = g_src.Load(int3(p, 0));
    if (eyebox > 1.5f) {
        // scopelensdebug: the eyepiece magenta (green if from the glass as drawn), its centre a cross.
        float3 dc = lerp(orig.rgb, rim ? float3(1.0f, 0.0f, 1.0f) : float3(0.1f, 0.9f, 0.2f), 0.7f);
        if (min(abs(o.x), abs(o.y)) < 0.02f && length(o) < 0.25f) dc = float3(1.0f, 1.0f, 1.0f);
        g_dst[p] = float4(lerp(orig.rgb, dc, m), orig.a);
        return;
    }
    const float2 ts = o * rt * inv_mag;   // the scene's angle off the scope's axis seen there
    float3 c = 0.0f;
    if (mode > 0.5f) {
        const float2 uv = float2(0.5f + ts.x * k.x, 0.5f - ts.y * k.y);
        if (all(uv >= 0.0f) && all(uv <= 1.0f)) c = g_scope.SampleLevel(g_lin, uv, 0).rgb;
    } else {
        const float3 e = float3(ts, 1.0f);
        const float3 cl = float3(dot(b0.xyz, e), dot(b1.xyz, e), dot(b2.xyz, e));
        if (cl.z > 1e-4f) c = g_src.SampleLevel(g_lin, float2(cl.x / cl.z * 0.5f + 0.5f, 0.5f - cl.y / cl.z * 0.5f), 0).rgb;
    }
    // A thin dark rim at the disc's edge (the glass as drawn has its own), and the eye box when on.
    if (rim) c *= saturate((1.0f - length(o)) / 0.06f);
    if (eyebox > 0.5f) c *= saturate((vig.z - length(o - vig.xy)) / max(vig.w, 1e-3f));
    if (ret_t > 0.0f) {
        // Reticle: thin crosshair with a gap round the centre, thick posts on the outer part, red centre dot.
        const float2 a = abs(o);
        const bool thin = (a.x < ret_t && a.y > 4.0f * ret_t) || (a.y < ret_t && a.x > 4.0f * ret_t);
        const bool post = (a.x < 3.0f * ret_t && a.y > 0.55f) || (a.y < 3.0f * ret_t && a.x > 0.55f);
        if (thin || post) c = 0.0f;
        if (length(o) < 1.6f * ret_t) c = float3(1.0f, 0.12f, 0.05f);
    }
    g_dst[p] = float4(lerp(orig.rgb, c, m * fade), orig.a);
}
)";
        // The glass mask's pixel count and centre (render pixels): one atomic per group.
        const char* kStats = R"(
Texture2D<float>      g_mask : register(t0);
RWByteAddressBuffer   g_out  : register(u0);
cbuffer P : register(b0) { uint2 size; uint2 pad; };
groupshared uint s_n, s_x, s_y;
[numthreads(16, 16, 1)]
void main(uint2 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (gi == 0) { s_n = 0; s_x = 0; s_y = 0; }
    GroupMemoryBarrierWithGroupSync();
    if (id.x < size.x && id.y < size.y && g_mask[id] > 0.5f) {
        InterlockedAdd(s_n, 1);
        InterlockedAdd(s_x, id.x);
        InterlockedAdd(s_y, id.y);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && s_n > 0) {
        g_out.InterlockedAdd(0, s_n);
        g_out.InterlockedAdd(4, s_x);
        g_out.InterlockedAdd(8, s_y);
    }
}
)";
        struct CbModel { float m[3][4]; float b[3][4]; float a[3][4]; float disc_c[4]; float disc_n[4]; float v[12]; float g[12]; };
        ID3D11ComputeShader*       g_mcs     = nullptr;
        ID3D11Buffer*              g_mcb     = nullptr;
        ID3D11ComputeShader*       g_scs     = nullptr;   // glass stats
        ID3D11Buffer*              g_scb     = nullptr;
        ID3D11Buffer*              g_sbuf    = nullptr;
        ID3D11UnorderedAccessView* g_suav    = nullptr;
        ID3D11ShaderResourceView*  g_ssrv    = nullptr;
        ID3D11PixelShader*         g_mask_ps = nullptr;
        bool                       g_mfailed = false;
        ID3D11Texture2D*           g_gmask = nullptr;
        ID3D11RenderTargetView*    g_gmask_rtv = nullptr;
        ID3D11ShaderResourceView*  g_gmask_srv = nullptr;
        UINT                       g_gmask_w = 0, g_gmask_h = 0;

        bool init_model(ID3D11Device* dev)
        {
            if (g_mcs || g_mfailed) return !g_mfailed;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            auto compile = [&](const char* src, const char* name, const char* target) -> ID3DBlob* {
                ID3DBlob* code = nullptr;
                ID3DBlob* err  = nullptr;
                if (!compile_fn || FAILED(compile_fn(src, strlen(src), name, nullptr, nullptr, "main", target, 0, 0, &code, &err))) {
                    spdlog::error("[Scope] {} failed: {}", name, err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
                    if (err) err->Release();
                    return nullptr;
                }
                return code;
            };
            static const char* kMaskPs = "float4 main() : SV_Target0 { return float4(1.0f, 1.0f, 1.0f, 1.0f); }";
            if (ID3DBlob* b = compile(kModel, "scope_model", "cs_5_0")) { dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_mcs); b->Release(); }
            if (ID3DBlob* b = compile(kStats, "scope_glass_stats", "cs_5_0")) { dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_scs); b->Release(); }
            if (ID3DBlob* b = compile(kMaskPs, "scope_glass_mask", "ps_5_0")) { dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_mask_ps); b->Release(); }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(CbModel); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&bd, nullptr, &g_mcb);
            bd.ByteWidth = 16;
            dev->CreateBuffer(&bd, nullptr, &g_scb);
            D3D11_BUFFER_DESC rb{};
            rb.ByteWidth = 16; rb.Usage = D3D11_USAGE_DEFAULT; rb.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE; rb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            const UINT zero[4]{};
            D3D11_SUBRESOURCE_DATA init{ zero, 0, 0 };
            if (SUCCEEDED(dev->CreateBuffer(&rb, &init, &g_sbuf))) {
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
                dev->CreateUnorderedAccessView(g_sbuf, &ud, &g_suav);
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R32_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX; sd.BufferEx.NumElements = 4; sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
                dev->CreateShaderResourceView(g_sbuf, &sd, &g_ssrv);
            }
            g_mfailed = !(g_mcs && g_mcb && g_scs && g_scb && g_suav && g_ssrv && g_mask_ps);
            spdlog::info("[Scope] scope-on-the-model shaders {}", g_mfailed ? "unavailable" : "ready");
            return !g_mfailed;
        }
    }

    ID3D11PixelShader* mask_ps(ID3D11Device* dev)
    {
        return dev && init_model(dev) ? g_mask_ps : nullptr;
    }

    bool glass_mask(ID3D11Device* dev, UINT w, UINT h, ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv)
    {
        if (!dev || !w || !h) return false;
        if (!g_gmask || g_gmask_w != w || g_gmask_h != h) {
            for (auto** p : { (IUnknown**)&g_gmask_srv, (IUnknown**)&g_gmask_rtv, (IUnknown**)&g_gmask }) if (*p) { (*p)->Release(); *p = nullptr; }
            D3D11_TEXTURE2D_DESC nd{};
            nd.Width = w; nd.Height = h; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc.Count = 1;
            nd.Format = DXGI_FORMAT_R8_UNORM; nd.Usage = D3D11_USAGE_DEFAULT;
            nd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_gmask))) {
                dev->CreateRenderTargetView(g_gmask, nullptr, &g_gmask_rtv);
                dev->CreateShaderResourceView(g_gmask, nullptr, &g_gmask_srv);
            }
            g_gmask_w = w; g_gmask_h = h;
            spdlog::info("[Scope] scope glass mask {}x{}: {}", w, h, g_gmask_rtv && g_gmask_srv ? "ready" : "failed");
        }
        if (rtv) *rtv = g_gmask_rtv;
        if (srv) *srv = g_gmask_srv;
        return g_gmask_rtv && g_gmask_srv;
    }

    namespace
    {
        // The stats read back to the CPU without waiting (a ring of staging copies, each tagged with its eye frame).
        struct Readback { ID3D11Buffer* buf{ nullptr }; int frame{ -10 }; bool pending{ false }; };
        Readback g_rb[4];
        int      g_rb_next = 0;
    }

    void glass_readback(ID3D11DeviceContext* ctx, int frame)
    {
        if (!ctx || !g_sbuf) return;
        auto& r = g_rb[g_rb_next];
        if (!r.buf) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            dev->CreateBuffer(&bd, nullptr, &r.buf);
            dev->Release();
            if (!r.buf) return;
        }
        ctx->CopyResource(r.buf, g_sbuf);
        r.frame    = frame;
        r.pending  = true;
        g_rb_next  = (g_rb_next + 1) & 3;
    }

    bool glass_result(ID3D11DeviceContext* ctx, int& frame, uint32_t out[3])
    {
        if (!ctx) return false;
        // The oldest pending copy first; one that is not ready yet stops the search (the GPU finishes them in order).
        int best = -1;
        for (int i = 0; i < 4; ++i)
            if (g_rb[i].pending && (best < 0 || g_rb[i].frame < g_rb[best].frame)) best = i;
        if (best < 0) return false;
        auto&                    r = g_rb[best];
        D3D11_MAPPED_SUBRESOURCE m{};
        if (ctx->Map(r.buf, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return false;
        std::memcpy(out, m.pData, 12);
        ctx->Unmap(r.buf, 0);
        r.pending = false;
        frame     = r.frame;
        return true;
    }

    ID3D11ShaderResourceView* glass_stats(ID3D11DeviceContext* ctx, bool run_it)
    {
        if (!ctx || !g_ssrv || !g_suav) return nullptr;
        const UINT zero[4]{};
        ctx->ClearUnorderedAccessViewUint(g_suav, zero);
        if (!run_it || !g_gmask_srv || !g_scs) return g_ssrv;   // cleared: no glass this frame
        const UINT cb[4]{ g_gmask_w, g_gmask_h, 0, 0 };
        ctx->UpdateSubresource(g_scb, 0, nullptr, cb, 0, 0);
        ID3D11ComputeShader*       prev_cs  = nullptr;
        ID3D11ShaderResourceView*  prev_srv = nullptr;
        ID3D11UnorderedAccessView* prev_uav = nullptr;
        ID3D11Buffer*              prev_cb  = nullptr;
        ctx->CSGetShader(&prev_cs, nullptr, nullptr);
        ctx->CSGetShaderResources(0, 1, &prev_srv);
        ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
        ctx->CSGetConstantBuffers(0, 1, &prev_cb);
        ctx->CSSetShader(g_scs, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &g_gmask_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &g_suav, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &g_scb);
        ctx->Dispatch((g_gmask_w + 15) / 16, (g_gmask_h + 15) / 16, 1);
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
        return g_ssrv;
    }

    namespace
    {
        // For an eye image without UAV binding (the back buffer): a full copy to sample from (typeless, read as UNORM so an
        // sRGB back buffer is not decoded), and a UAV texture the lens square is drawn into and copied back from.
        ID3D11Texture2D*           g_bstash = nullptr;
        ID3D11ShaderResourceView*  g_bsrv   = nullptr;
        ID3D11Texture2D*           g_mid    = nullptr;
        ID3D11UnorderedAccessView* g_mid_uav = nullptr;
        D3D11_TEXTURE2D_DESC       g_bdesc{};
        bool                       g_bfailed = false;

        void family(DXGI_FORMAT f, DXGI_FORMAT& tl, DXGI_FORMAT& un)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                tl = DXGI_FORMAT_R8G8B8A8_TYPELESS; un = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                tl = DXGI_FORMAT_B8G8R8A8_TYPELESS; un = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                tl = DXGI_FORMAT_R10G10B10A2_TYPELESS; un = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                tl = DXGI_FORMAT_R16G16B16A16_TYPELESS; un = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
            default: tl = f; un = f; break;
            }
        }

        // A compute pass over a box of the eye image. t0 = a copy of the whole eye image, t1/t2 = the caller's, u0 = the
        // eye image itself (the DLSS eye image has UAV binding) or, for the swap chain's back buffer (DLAA at the eye's
        // size, no upscaling: no compute writes), a texture the box is drawn into and copied back from.
        bool run(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, ID3D11ComputeShader* cs, ID3D11Buffer* cb, const void* cbdata, float x0, float y0, float x1, float y1,
                 ID3D11ShaderResourceView* t1, ID3D11ShaderResourceView* t2, ID3D11ShaderResourceView* t3 = nullptr, ID3D11Texture2D* zsrc = nullptr)
        {
            if (!ctx || !eye || !cs || !cb || x1 <= x0 || y1 <= y0) return false;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return false;
            D3D11_TEXTURE2D_DESC d{};
            eye->GetDesc(&d);
            const bool via_copy = !(d.BindFlags & D3D11_BIND_UNORDERED_ACCESS);
            if (via_copy && !g_bfailed && (!g_bstash || g_bdesc.Width != d.Width || g_bdesc.Height != d.Height || g_bdesc.Format != d.Format)) {
                for (auto** p : { (IUnknown**)&g_bsrv, (IUnknown**)&g_bstash, (IUnknown**)&g_mid_uav, (IUnknown**)&g_mid }) if (*p) { (*p)->Release(); *p = nullptr; }
                DXGI_FORMAT tl{}, un{};
                family(d.Format, tl, un);
                D3D11_TEXTURE2D_DESC nd = d;
                nd.Format = tl; nd.MipLevels = 1; nd.ArraySize = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
                nd.SampleDesc = { 1, 0 };
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = un; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
                if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_bstash))) dev->CreateShaderResourceView(g_bstash, &vd, &g_bsrv);
                nd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = un; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_mid))) dev->CreateUnorderedAccessView(g_mid, &ud, &g_mid_uav);
                g_bdesc   = d;
                g_bfailed = !(g_bsrv && g_mid_uav) || d.SampleDesc.Count != 1;
                spdlog::info("[Scope] lens into the back buffer {}x{} format {} (via a copy): {}", d.Width, d.Height, (int)d.Format, g_bfailed ? "failed" : "ready");
            }
            if (!via_copy && (!g_stash || g_desc.Width != d.Width || g_desc.Height != d.Height || g_desc.Format != d.Format)) {
                if (g_srv) g_srv->Release();
                if (g_stash) g_stash->Release();
                g_srv = nullptr; g_stash = nullptr;
                D3D11_TEXTURE2D_DESC nd = d;
                nd.MipLevels = 1; nd.ArraySize = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
                if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_stash))) dev->CreateShaderResourceView(g_stash, nullptr, &g_srv);
                g_desc = d;
                spdlog::info("[Scope] lens source copy {}x{} format {}: {}", d.Width, d.Height, (int)d.Format, g_srv ? "ready" : "failed");
            }
            if (!via_copy && eye != g_uav_of) {
                if (g_uav) g_uav->Release();
                if (g_uav_of) g_uav_of->Release();
                g_uav    = nullptr;
                g_uav_of = eye;
                eye->AddRef();
                dev->CreateUnorderedAccessView(eye, nullptr, &g_uav);
            }
            dev->Release();
            ID3D11ShaderResourceView*  src = via_copy ? g_bsrv : g_srv;
            ID3D11UnorderedAccessView* uav = via_copy ? g_mid_uav : g_uav;
            if ((via_copy && g_bfailed) || !src || !uav) return false;
            const D3D11_BOX box{ UINT(x0), UINT(y0), 0, UINT(x1), UINT(y1), 1 };
            // The source sampled: the eye image itself, or (zsrc, same size and format) another image.
            ID3D11Texture2D* from = eye;
            if (zsrc) {
                D3D11_TEXTURE2D_DESC zd{};
                zsrc->GetDesc(&zd);
                if (zd.Width == d.Width && zd.Height == d.Height && zd.Format == d.Format && zd.SampleDesc.Count == 1) from = zsrc;
            }
            if (via_copy) {
                ctx->CopyResource(g_bstash, from);
                ctx->CopySubresourceRegion(g_mid, 0, UINT(x0), UINT(y0), 0, eye, 0, &box);
            } else {
                ctx->CopyResource(g_stash, from);
            }
            ctx->UpdateSubresource(cb, 0, nullptr, cbdata, 0, 0);
            ID3D11ComputeShader*       prev_cs = nullptr;
            ID3D11ShaderResourceView*  prev_srv[4]{};
            ID3D11UnorderedAccessView* prev_uav = nullptr;
            ID3D11Buffer*              prev_cb  = nullptr;
            ID3D11SamplerState*        prev_smp = nullptr;
            ctx->CSGetShader(&prev_cs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 4, prev_srv);
            ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
            ctx->CSGetConstantBuffers(0, 1, &prev_cb);
            ctx->CSGetSamplers(0, 1, &prev_smp);
            ctx->CSSetShader(cs, nullptr, 0);
            ID3D11ShaderResourceView* srvs[4]{ src, t1, t2, t3 };
            ctx->CSSetShaderResources(0, 4, srvs);
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->CSSetSamplers(0, 1, &g_smp);
            ctx->Dispatch((UINT(x1 - x0) + 7) / 8, (UINT(y1 - y0) + 7) / 8, 1);
            ID3D11ShaderResourceView*  null_srv[4]{};
            ID3D11UnorderedAccessView* null_uav = nullptr;
            ctx->CSSetShaderResources(0, 4, null_srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
            ctx->CSSetShader(prev_cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 4, prev_srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &prev_cb);
            ctx->CSSetSamplers(0, 1, &prev_smp);
            if (prev_cs) prev_cs->Release();
            for (auto* s : prev_srv) if (s) s->Release();
            if (prev_uav) prev_uav->Release();
            if (prev_cb) prev_cb->Release();
            if (prev_smp) prev_smp->Release();
            if (via_copy) ctx->CopySubresourceRegion(eye, 0, UINT(x0), UINT(y0), 0, g_mid, 0, &box);
            return true;
        }
    }

    bool draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, const Lens& lens)
    {
        if (!ctx || !eye) return false;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        const bool ok = init(dev);
        dev->Release();
        if (!ok) return false;
        D3D11_TEXTURE2D_DESC d{};
        eye->GetDesc(&d);
        // Only the ring's square is dispatched (the whole image for scopesrc 1: black outside the lens).
        const float W = float(d.Width), H = float(d.Height);
        float x0 = std::clamp(std::floor(lens.lens_c[0] - lens.ring_r), 0.0f, W), x1 = std::clamp(std::ceil(lens.lens_c[0] + lens.ring_r), 0.0f, W);
        float y0 = std::clamp(std::floor(lens.lens_c[1] - lens.ring_r), 0.0f, H), y1 = std::clamp(std::ceil(lens.lens_c[1] + lens.ring_r), 0.0f, H);
        if (lens.outside_black) { x0 = 0.0f; y0 = 0.0f; x1 = W; y1 = H; }
        const Cb cb{ { lens.lens_c[0], lens.lens_c[1], lens.lens_r, lens.ring_r, lens.src_c[0], lens.src_c[1], lens.inv_mag, lens.reticle_w, lens.vig_c[0], lens.vig_c[1], lens.vig_r,
                       lens.vig_soft, x0, y0, W, H, lens.scope_k[0], lens.scope_k[1], lens.scope_own ? 2.0f : (lens.scope_srv ? 1.0f : 0.0f), lens.outside_black ? 1.0f : 0.0f } };
        return run(ctx, eye, g_cs, g_cb, &cb, x0, y0, x1, y1, lens.scope_srv, nullptr);
    }

    bool draw_model(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye, const ModelLens& lens)
    {
        if (!ctx || !eye) return false;

        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        const bool ok = init(dev) && init_model(dev);
        dev->Release();
        if (!ok) return false;
        D3D11_TEXTURE2D_DESC d{};
        eye->GetDesc(&d);
        const float W = float(d.Width), H = float(d.Height);
        const float x0 = std::clamp(std::floor(lens.box[0]), 0.0f, W), x1 = std::clamp(std::ceil(lens.box[2]), 0.0f, W);
        const float y0 = std::clamp(std::floor(lens.box[1]), 0.0f, H), y1 = std::clamp(std::ceil(lens.box[3]), 0.0f, H);
        CbModel cb{};
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) { cb.m[r][c] = lens.m[r][c]; cb.b[r][c] = lens.b[r][c]; cb.a[r][c] = lens.a[r][c]; }
        for (int c = 0; c < 4; ++c) { cb.disc_c[c] = lens.disc_c[c]; cb.disc_n[c] = lens.disc_n[c]; }
        cb.disc_n[3] = std::max(1e-5f, lens.disc_n[3]);
        cb.v[0] = x0; cb.v[1] = y0; cb.v[2] = W; cb.v[3] = H;
        cb.v[4] = lens.k[0]; cb.v[5] = lens.k[1]; cb.v[6] = lens.inv_mag; cb.v[7] = lens.scope_srv ? 1.0f : 0.0f;
        cb.v[8] = lens.r_t; cb.v[9] = std::clamp(lens.fade, 0.0f, 1.0f); cb.v[10] = lens.ret_t; cb.v[11] = lens.debug ? 2.0f : (lens.eyebox ? 1.0f : 0.0f);
        cb.g[0] = lens.mscale[0]; cb.g[1] = lens.mscale[1]; cb.g[2] = lens.moff[0]; cb.g[3] = lens.moff[1];
        cb.g[4] = std::max(1.0f, lens.msize[0]); cb.g[5] = std::max(1.0f, lens.msize[1]); cb.g[6] = lens.roll[0]; cb.g[7] = lens.roll[1];
        cb.g[8] = lens.tpp; cb.g[9] = (lens.mask && lens.stats) ? 1.0f : 0.0f; cb.g[10] = lens.min_px; cb.g[11] = 0.0f;
        return run(ctx, eye, g_mcs, g_mcb, &cb, x0, y0, x1, y1, lens.scope_srv, lens.mask, lens.stats, lens.scope_srv ? nullptr : lens.zoom_src);
    }

    namespace
    {
        std::atomic<int> g_muted{ 0 };
        void**           g_slot  = nullptr;
        void*            g_saved = nullptr;

        HRESULT WINAPI muted_present(IDXGISwapChain*, UINT, UINT)
        {
            g_muted++;
            return S_OK;
        }

        bool set_slot(void** slot, void* fn)
        {
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
            *slot = fn;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
        }

        // A history texture, its copy as the eyes left it (put back after the scope call) and the scope's own copy.
        struct Hist
        {
            ID3D11Resource* res{ nullptr };
            ID3D11Resource* backup{ nullptr };
            ID3D11Resource* scope{ nullptr };
            bool            scope_valid{ false };
            bool            failed{ false };
        };
        std::vector<Hist>                   g_hist;
        std::unordered_set<ID3D11Resource*> g_hist_set;
        int                                 g_load_lo = 0, g_load_hi = 1 << 30;   // scopehistrange (research)

        ID3D11Resource* clone(ID3D11Device* dev, ID3D11Resource* r)
        {
            D3D11_RESOURCE_DIMENSION dim{};
            r->GetType(&dim);
            for (const UINT bind : { UINT(D3D11_BIND_SHADER_RESOURCE), 0u }) {
                if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D11_TEXTURE2D_DESC d{};
                    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
                    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind; d.CPUAccessFlags = 0; d.MiscFlags = 0;
                    ID3D11Texture2D* t = nullptr;
                    if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t))) return t;
                } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
                    D3D11_TEXTURE3D_DESC d{};
                    static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
                    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind; d.CPUAccessFlags = 0; d.MiscFlags = 0;
                    ID3D11Texture3D* t = nullptr;
                    if (SUCCEEDED(dev->CreateTexture3D(&d, nullptr, &t))) return t;
                } else {
                    return nullptr;
                }
            }
            return nullptr;
        }

        std::string describe(ID3D11Resource* r)
        {
            D3D11_RESOURCE_DIMENSION dim{};
            r->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d{};
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
                return fmt::format("{}x{} x{} format {}", d.Width, d.Height, d.ArraySize, (int)d.Format);
            }
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
                D3D11_TEXTURE3D_DESC d{};
                static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
                return fmt::format("{}x{}x{} format {}", d.Width, d.Height, d.Depth, (int)d.Format);
            }
            return fmt::format("dimension {}", (int)dim);
        }

        ID3D11Texture2D*          g_pic     = nullptr;
        ID3D11ShaderResourceView* g_pic_srv = nullptr;
        D3D11_TEXTURE2D_DESC      g_pic_desc{};

        DXGI_FORMAT typed(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return f;
            }
        }
    }

    bool mute_present(IDXGISwapChain* swap)
    {
        if (!swap || g_slot) return false;
        void** slot = &(*reinterpret_cast<void***>(swap))[8];
        void*  cur  = *slot;
        if (!set_slot(slot, reinterpret_cast<void*>(&muted_present))) return false;
        g_slot  = slot;
        g_saved = cur;
        g_muted = 0;
        return true;
    }

    int unmute_present()
    {
        if (g_slot) set_slot(g_slot, g_saved);
        g_slot = nullptr;
        return g_muted.exchange(0);
    }

    void hist_note(ID3D11Resource* res)
    {
        if (!res || g_hist_set.contains(res)) return;
        if (g_hist_set.size() >= 160) {
            // Not saved around the scope call: its content after the call is the scope's, and the left eye's call would keep
            // it as the right eye's history (a tinted or wrong right eye while scoped).
            static int told = 0;
            if (told++ < 3) spdlog::error("[Scope] history texture {} not covered by the scope call (160 already)", describe(res));
            return;
        }
        g_hist_set.insert(res);
        res->AddRef();
        g_hist.push_back({ res });
        spdlog::info("[Scope] history texture {} for the scope call: {}", g_hist.size(), describe(res));
    }

    // History textures the game has released (only the mod still holds them: after a load or a resize the game makes new
    // ones) are dropped with their copies, so the list holds the live ones and no time goes into copying dead ones.
    void hist_prune()
    {
        size_t dropped = 0;
        for (size_t i = 0; i < g_hist.size();) {
            auto& h = g_hist[i];
            h.res->AddRef();
            if (h.res->Release() > 1) { ++i; continue; }
            for (auto* r : { h.backup, h.scope }) if (r) r->Release();
            g_hist_set.erase(h.res);
            h.res->Release();
            g_hist[i] = g_hist.back();
            g_hist.pop_back();
            ++dropped;
        }
        if (dropped) spdlog::info("[Scope] {} history textures released by the game dropped ({} left)", dropped, g_hist.size());
    }

    size_t hist_count() { return g_hist.size(); }
    bool   hist_has(ID3D11Resource* res) { return res && g_hist_set.contains(res); }

    void hist_before(ID3D11DeviceContext* ctx, bool load_scope)
    {
        hist_prune();
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        for (size_t i = 0; i < g_hist.size(); ++i) {
            auto& h = g_hist[i];
            if (h.failed) continue;
            if (!h.backup) h.backup = clone(dev, h.res);
            if (!h.scope) h.scope = clone(dev, h.res);
            if (!h.backup || !h.scope) {
                h.failed = true;
                spdlog::error("[Scope] history texture {} could not be copied: left alone in the scope call", describe(h.res));
                continue;
            }
            ctx->CopyResource(h.backup, h.res);
            if (load_scope && h.scope_valid && int(i) >= g_load_lo && int(i) < g_load_hi) ctx->CopyResource(h.res, h.scope);
        }
        dev->Release();
    }

    void hist_range(int lo, int hi)
    {
        g_load_lo = lo;
        g_load_hi = hi;
        spdlog::info("[Scope] scopehistrange: history textures {} to {} start the scope call from the scope's own copy, the others from the eye's", lo, hi - 1);
    }

    void hist_list()
    {
        for (size_t i = 0; i < g_hist.size(); ++i)
            spdlog::info("[Scope] history texture #{}: {}{}{}", i, describe(g_hist[i].res), g_hist[i].scope_valid ? "" : " (no scope copy yet)", g_hist[i].failed ? " (not copied)" : "");
    }

    namespace
    {
        struct Roles
        {
            int                                           frame{ -100 };
            std::unordered_map<uint64_t, ID3D11Resource*> res;   // not held: only compared and looked up in g_hist
            std::unordered_map<uint64_t, int>             n;
        };
        Roles g_roles[2];   // [eye]
    }

    void hist_role(uint64_t key, ID3D11Resource* res, int frame)
    {
        auto& r = g_roles[frame & 1];
        if (r.frame != frame) {
            r.frame = frame;
            r.res.clear();
            r.n.clear();
        }
        const int k = r.n[key]++;
        r.res.emplace(key ^ (uint64_t(k & 63) * 0x9E3779B97F4A7C15ull), res);
    }

    int hist_after(ID3D11DeviceContext* ctx, int right_frame, bool swap_pairs)
    {
        std::unordered_map<ID3D11Resource*, Hist*> by_res;
        for (auto& h : g_hist) {
            if (h.failed || !h.backup || !h.scope) continue;
            ctx->CopyResource(h.scope, h.res);
            h.scope_valid = true;
            ctx->CopyResource(h.res, h.backup);
            by_res[h.res] = &h;
        }
        const Roles& L = g_roles[0];
        const Roles& R = g_roles[1];
        if (!swap_pairs || R.frame != right_frame || L.frame != right_frame - 1) return 0;
        // A slot that bound one texture in the left call and another in the right one alternates between them: the right
        // call wrote its output into the left call's texture, and the next left call (one call later to the game) binds the
        // right call's. Their contents are swapped, as if the scope call had not run.
        std::unordered_set<ID3D11Resource*> done;
        int                                 pairs = 0;
        for (const auto& [key, rres] : R.res) {
            const auto it = L.res.find(key);
            if (it == L.res.end() || it->second == rres || done.contains(rres) || done.contains(it->second)) continue;
            const auto hl = by_res.find(it->second);
            const auto hr = by_res.find(rres);
            if (hl == by_res.end() || hr == by_res.end() || describe(rres) != describe(it->second)) continue;
            ctx->CopyResource(rres, hl->second->backup);
            ctx->CopyResource(it->second, hr->second->backup);
            done.insert(rres);
            done.insert(it->second);
            pairs++;
        }
        return pairs;
    }

    bool capture(ID3D11DeviceContext* ctx, ID3D11Resource* src, UINT corner)
    {
        if (!ctx || !src) return false;
        D3D11_RESOURCE_DIMENSION dim{};
        src->GetType(&dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(src)->GetDesc(&d);
        if (d.SampleDesc.Count != 1 || d.ArraySize != 1) return false;
        const bool part = corner > 0 && corner <= d.Width && corner <= d.Height;
        if (part) {
            d.Width = d.Height = corner;
            d.MipLevels = 1;
        }
        if (!g_pic || g_pic_desc.Width != d.Width || g_pic_desc.Height != d.Height || g_pic_desc.Format != d.Format || g_pic_desc.MipLevels != d.MipLevels) {
            if (g_pic_srv) g_pic_srv->Release();
            if (g_pic) g_pic->Release();
            g_pic = nullptr; g_pic_srv = nullptr;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return false;
            D3D11_TEXTURE2D_DESC nd = d;
            nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
            if (SUCCEEDED(dev->CreateTexture2D(&nd, nullptr, &g_pic))) {
                D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format              = typed(d.Format);
                vd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
                vd.Texture2D.MipLevels = 1;
                if (FAILED(dev->CreateShaderResourceView(g_pic, &vd, &g_pic_srv))) { g_pic->Release(); g_pic = nullptr; }
            }
            dev->Release();
            g_pic_desc = d;
            spdlog::info("[Scope] pass picture {}x{} format {} (view format {}): {}", d.Width, d.Height, (int)d.Format, (int)typed(d.Format), g_pic_srv ? "ready" : "failed");
        }
        if (!g_pic) return false;
        if (part) {
            D3D11_BOX box{ 0, 0, 0, corner, corner, 1 };
            ctx->CopySubresourceRegion(g_pic, 0, 0, 0, 0, src, 0, &box);
        } else {
            ctx->CopyResource(g_pic, src);
        }
        return true;
    }

    ID3D11ShaderResourceView* picture() { return g_pic_srv; }
}

// The scope camera: at the ocular lens, looking at the aim point, upright as the eye (the picture lines up with the
// view around the scope); half field of view = the lens's half angle at the eye / magnification, plus 10 % for the eye
// moving before the picture is shown.
void FarCry5Stereo::scope_camera(const ScopeState& ds, int frame, ScopeCam& sc)
{
    sc                  = {};
    // A barrel-ray hit or convergence under 2 m (seen at times with the test controller at the eye) would swing the
    // narrow view onto the ground in front: the set convergence distance then.
    const float     hit  = fc5::aim::last_hit();
    const float     dist = hit > 2.0f ? hit : m_mo.converge > 2.0f ? m_mo.converge : std::max(2.0f, m_converge_m.load());
    const glm::vec3 aim  = m_mo.aim_origin + m_mo.barrel_dir * dist;
    glm::vec3       fwd = aim - ds.lens;
    const float     dl  = glm::length(fwd);
    fwd                 = dl > 0.5f ? fwd / dl : m_mo.barrel_dir;
    const glm::mat3 to_world = glm::transpose(ds.eye_rot);
    glm::vec3       right    = glm::cross(fwd, to_world[1]);
    if (glm::length(right) < 1e-3f) right = glm::cross(fwd, glm::vec3{ 0.0f, 0.0f, 1.0f });
    right              = glm::normalize(right);
    const glm::vec3 up = glm::cross(right, fwd);
    sc.rot             = glm::transpose(glm::mat3{ right, up, -fwd });
    sc.pos             = ds.lens;
    const float ahead  = -(ds.eye_rot * (ds.lens - ds.eye_pos)).z;
    sc.tan_half        = 1.1f * (ds.radius / std::max(0.01f, ahead)) / std::max(1.0f, ds.full_mag);
    if (scope_on_model() && ds.mvalid) {
        // The scope on the gun: from the picture on the eyepiece along the gun's scope axis, rolled with the gun.
        sc.rot      = glm::transpose(glm::mat3{ ds.mright, ds.mup, -ds.maxis });
        sc.pos      = ds.mlens;
        sc.tan_half = 1.1f * ds.mfield / std::max(1.0f, ds.full_mag);
    }
    sc.mag             = std::max(1.0f, ds.full_mag);
    sc.frame           = frame;
    sc.valid           = true;
}

// Step 3: before the scope's render call (render thread, between the right eye's call and the next frame). False =
// no call this pair. right_frame = the right eye's frame (m_frame is right_frame + 1 after its present).
bool FarCry5Stereo::scope_call_begin(int right_frame)
{
    const auto skip = [&](const char* why) {
        if (m_scope_run > 0) spdlog::info("[Scope] pass stops after {} calls in a row: {}", m_scope_run, why);
        m_scope_run = 0;
        return false;
    };
    if (m_scope_pass_broken) return false;
    if (scope_src_eye()) return skip("scopesrc 1 (the eye draws the scope view)");
    if (m_scope_hold > 0) { --m_scope_hold; return skip("the eye's history flushes after scopesrc 1"); }
    const ScopeState& ds = m_scope_dom_st;
    // Only while the dominant eye is behind a magnifying scope in this pair (its look-through test ran at this pair's frame).
    if (!ds.active || ds.frame < right_frame - 1 || ds.frame > right_frame || ds.full_mag <= 1.01f) return skip("the eye left the scope");
    if (scope_on_model() && ds.mvalid && ds.mfade <= 0.0f) return skip("the gun is angled away from the eye (picture off)");
    if (!m_scope_lens.load() || !m_frame_double.load() || !m_stereo2.load() || m_menu_screen_on || m_cine_on) return skip("lens off, menu or cinematic");
    if (!m_smaa_ps) return skip("SMAA shader not found yet");
    // scopeevery N: the call every Nth pair while scoped (the lens keeps the last picture in between).
    const int every = std::clamp(m_scope_every.load(), 1, 4);
    if (every > 1 && (m_scope_pair_n++ % uint32_t(every)) != 0) return false;
    auto* d3d  = g_framework->get_d3d11_hook().get();
    auto  swap = d3d ? d3d->get_swap_chain() : nullptr;
    auto  dev  = d3d ? d3d->get_device() : nullptr;
    if (!swap || !dev) return skip("no swap chain");

    scope_camera(ds, right_frame, m_scope_cam);

    if (!fc5::scope::mute_present(swap)) {
        m_scope_pass_broken = true;
        spdlog::error("[Scope] pass off: the swap chain's Present could not be muted");
        return skip("present not muted");
    }
    // scoperes N: the picture in an N x N corner of the render size (whole size when 0 or larger than it).
    const int res   = m_scope_res.load();
    m_scope_corner  = (res >= 128 && res <= int(m_main_w) && res <= int(m_main_h)) ? res : 0;
    m_cur_rt_w      = m_cur_rt_h = 0;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    const bool load_scope = m_scope_prev_frame < right_frame && m_scope_prev_frame >= right_frame - 2 * every;
    if (ctx) {
        fc5::scope::hist_before(ctx, load_scope);
        ctx->Release();
    }
    auto& s            = m_scope_saved;
    s.frame            = m_frame.load();
    s.views            = m_views_this_frame;
    s.player_views     = m_player_views_this_frame;
    s.modified         = m_modified_this_frame;
    s.weapon_draws     = m_weapon_draws_this_frame;
    s.weapon_cb_n      = m_weapon_cb_n;
    s.cine_wcb_n       = m_cine_wcb_n;
    s.world_cb_n       = m_world_cb_n;
    s.weapon_sig_frame = m_weapon_sig_frame;
    s.scene_cleared    = m_scene_cleared_this_frame;
    s.hud_drawn        = m_hud_drawn;
    s.weapon_cb        = m_weapon_cb;
    s.world_cb         = m_world_cb;
    m_scope_presents_before = m_presents;
    m_scope_world_views     = 0;
    m_frame        = right_frame;
    m_scope_call   = true;
    return true;
}

void FarCry5Stereo::scope_call_end(double cpu_ms)
{
    m_scope_call    = false;
    const int muted = fc5::scope::unmute_present();
    auto*     d3d   = g_framework->get_d3d11_hook().get();
    auto      dev   = d3d ? d3d->get_device() : nullptr;
    if (dev) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        if (ctx) {
            if (m_scope_corner > 0) scope_scissor_end(ctx);
            m_scope_hist_pairs += fc5::scope::hist_after(ctx, m_frame.load(), m_scope_hist_swap.load());
            ctx->Release();
        }
    }
    m_scope_corner = 0;
    const auto& s              = m_scope_saved;
    m_views_this_frame         = s.views;
    m_player_views_this_frame  = s.player_views;
    m_modified_this_frame      = s.modified;
    m_weapon_draws_this_frame  = s.weapon_draws;
    m_weapon_cb_n              = s.weapon_cb_n;
    m_cine_wcb_n               = s.cine_wcb_n;
    m_world_cb_n               = s.world_cb_n;
    m_weapon_sig_frame         = s.weapon_sig_frame;
    m_scene_cleared_this_frame = s.scene_cleared;
    m_hud_drawn                = s.hud_drawn;
    m_weapon_cb                = s.weapon_cb;
    m_world_cb                 = s.world_cb;
    m_frame                    = s.frame;
    m_scope_calls++;
    m_scope_calls_window++;
    m_scope_call_ms += cpu_ms;
    if (m_presents != m_scope_presents_before) {
        // A present reached the framework inside the call (the game presents another way): the frame order is gone.
        m_scope_pass_broken = true;
        spdlog::error("[Scope] pass off: the scope call presented for real ({} presents; muted {})", m_presents - m_scope_presents_before, muted);
    } else if (muted != 1) {
        static int told = 0;
        if (told++ < 5) spdlog::info("[Scope] scope call made {} presents (expected 1)", muted);
    }
    if (m_scope_world_views > 0) {
        m_scope_prev_vrp   = m_scope_cam.vrp;
        m_scope_prev_cam   = m_scope_cam.pos;
        m_scope_prev_frame = m_scope_cam.frame;
        m_scope_run++;
    } else {
        m_scope_run = 0;
    }
    static auto last = std::chrono::steady_clock::now();
    const auto  now  = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(5)) {
        last = now;
        spdlog::info("[Scope] pass, last 5 s: {} scope calls, CPU {:.2f} ms per call (mod buffer scan {:.2f}); pictures {}; weapon draws left out {}; history textures {}; lens drawn from the pass {}; "
                     "scoperes {} scopeskip {} scopeevery {}: draws left out {}, dispatches left out {}, draws scissored {}; scopehistswap {}: history pairs swapped back {}",
                     m_scope_calls_window, m_scope_calls_window ? m_scope_call_ms / m_scope_calls_window : 0.0, m_scope_calls_window ? m_scope_scan_ms / m_scope_calls_window : 0.0, m_scope_captures, m_scope_call_skips, fc5::scope::hist_count(),
                     m_scope_pass_draws, m_scope_res.load(), m_scope_skip.load(), m_scope_every.load(), m_scope_skipped_draws, m_scope_skipped_cs, m_scope_scissored,
                     m_scope_hist_swap.load() ? "on" : "off", m_scope_hist_pairs);
        m_scope_calls_window = 0;
        m_scope_call_ms      = 0.0;
        m_scope_scan_ms      = 0.0;
    }
}

// The SMAA draw of the scope call: its target is the scope's picture (before the HUD).
void FarCry5Stereo::scope_capture(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv)
{
    if (!rtv || !m_scope_cam.valid) return;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    if (fc5::scope::capture(ctx, r, UINT(m_scope_corner))) {
        m_scope_img_cam = m_scope_cam;
        m_scope_captures++;
    }
    if (r) r->Release();
}

void FarCry5Stereo::scope_hist_note(ID3D11Resource* res)
{
    fc5::scope::hist_note(res);
}

bool FarCry5Stereo::scope_hist_has(ID3D11Resource* res)
{
    return fc5::scope::hist_has(res);
}

// scopehistswap: in the eyes' calls, which texture a history slot of shader sh binds (the game's binding, before the
// per-eye swaps).
void FarCry5Stereo::scope_hist_role(const void* sh, UINT slot, bool uav, ID3D11Resource* res)
{
    if (m_scope_call || !res || !m_scope_hist_swap.load()) return;
    const uint64_t key = (uint64_t(uintptr_t(sh)) * 1000003ull) ^ (uint64_t(slot) << 1) ^ (uav ? 1 : 0);
    fc5::scope::hist_role(key, res, m_frame.load());
}

namespace
{
    // scoperes: the game's rasterizer states with the scissor turned on (game state held -> clone), and what the game
    // has bound in this scope call (read back at each draw: the RS calls are not hooked).
    struct RsClone
    {
        ID3D11RasterizerState* clone{ nullptr };
        bool                   game_scissor{ false };
    };
    std::unordered_map<ID3D11RasterizerState*, RsClone> g_rs;
    std::unordered_set<ID3D11RasterizerState*>          g_rs_clones;
    ID3D11RasterizerState*                              g_game_rs      = nullptr;
    bool                                                g_game_known   = false;
    D3D11_RECT                                          g_game_rect{};
    UINT                                                g_game_rects   = 0;
    D3D11_RECT                                          g_applied{};
    bool                                                g_applied_on   = false;
    bool                                                g_changed      = false;

    bool same_rect(const D3D11_RECT& a, const D3D11_RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }

    const RsClone* clone_for(ID3D11DeviceContext* ctx, ID3D11RasterizerState* game)
    {
        if (auto it = g_rs.find(game); it != g_rs.end()) return &it->second;
        D3D11_RASTERIZER_DESC d{ D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE };
        if (game) game->GetDesc(&d);
        RsClone c{};
        c.game_scissor = d.ScissorEnable != FALSE;
        d.ScissorEnable = TRUE;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            dev->CreateRasterizerState(&d, &c.clone);
            dev->Release();
        }
        if (!c.clone) return nullptr;
        if (game) game->AddRef();   // held, so its address is not reused while it is in the map
        g_rs_clones.insert(c.clone);
        return &(g_rs[game] = c);
    }

    // scopeskip: which effect a shader belongs to, from the names its reflection gave (bits as m_scope_skip).
    std::unordered_map<void*, uint32_t> g_skip_bits;
    std::unordered_set<void*>           g_skip_named;   // shaders already named in the log
    int                                 g_skip_logged[6]{};

    uint32_t effect_bits(const std::string& names)
    {
        uint32_t b = 0;
        if (names.find("CSSLRContextProvider") != std::string::npos) b |= 2;
        if (names.find("CSSBCProvider") != std::string::npos) b |= 4;
        if (names.find("CVolumetricFogProvider") != std::string::npos) b |= 8;
        // No motion blur or bloom bits (simulator 2026-10-11): the game has no pass of its own named for motion blur, and
        // the bloom provider (CPostFxHDRBloomEffectsParameterProvider) is also on the tone-mapping pass (the lens went green).
        return b;
    }

    void log_skip(uint32_t bits, const std::string& names, const char* stage)
    {
        static const char* kWhat[6]{ "sun shadows", "reflections", "ambient occlusion", "volumetric fog", "motion blur", "bloom" };
        for (int i = 1; i < 6; ++i) {
            if (!(bits & (1u << i)) || g_skip_logged[i] >= 8) continue;
            g_skip_logged[i]++;
            spdlog::info("[Scope] scopeskip {}: {} left out of the scope view: {}", kWhat[i], stage, names.substr(0, 200));
        }
    }
}

bool FarCry5Stereo::scope_skip_ps(ID3D11PixelShader* ps)
{
    auto it = g_skip_bits.find(ps);
    if (it == g_skip_bits.end()) {
        std::string names;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto n = m_ps_names.find(ps); n != m_ps_names.end()) names = n->second;
        }
        it = g_skip_bits.emplace(ps, effect_bits(names)).first;
    }
    const uint32_t hit = it->second & uint32_t(m_scope_skip.load());
    if (hit && g_skip_named.insert(ps).second) {
        std::lock_guard _{ m_ps_mtx };
        if (auto n = m_ps_names.find(ps); n != m_ps_names.end()) log_skip(hit, n->second, "pixel shader");
    }
    return hit != 0;
}

bool FarCry5Stereo::scope_skip_cs(ID3D11ComputeShader* cs)
{
    auto it = g_skip_bits.find(cs);
    if (it == g_skip_bits.end()) {
        std::string names;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto n = m_cs_names.find(cs); n != m_cs_names.end()) names = n->second;
        }
        it = g_skip_bits.emplace(cs, effect_bits(names)).first;
    }
    const uint32_t hit = it->second & uint32_t(m_scope_skip.load());
    if (hit && g_skip_named.insert(cs).second) {
        std::lock_guard _{ m_ps_mtx };
        if (auto n = m_cs_names.find(cs); n != m_cs_names.end()) log_skip(hit, n->second, "compute shader");
    }
    return hit != 0;
}

// scoperes, at each draw of the scope call: into a target of the screen's shape (full, half, quarter... size) the draw
// is scissored to the scope's corner (plus a 48 px margin for blurs reading beside it), intersected with the game's own
// scissor; any other target gets the game's state back.
void FarCry5Stereo::scope_scissor(ID3D11DeviceContext* ctx)
{
    ID3D11RasterizerState* cur = nullptr;
    ctx->RSGetState(&cur);
    if (!g_rs_clones.contains(cur)) {
        g_game_rs    = cur;
        g_game_known = true;
    }
    UINT       cnt = 0;
    D3D11_RECT r{};
    ctx->RSGetScissorRects(&cnt, nullptr);
    if (cnt > 0) {
        UINT one = 1;
        ctx->RSGetScissorRects(&one, &r);
    }
    if (!(g_applied_on && cnt == 1 && same_rect(r, g_applied))) {
        g_game_rect  = r;
        g_game_rects = cnt;
        g_applied_on = false;
    }
    const UINT W = m_main_w, H = m_main_h, w = m_cur_rt_w, h = m_cur_rt_h;
    bool       screen_shape = false;
    D3D11_RECT want{};
    if (W > 0 && H > 0 && w >= 16 && h >= 16 && w <= W && h <= H) {
        const float kx = float(W) / float(w), ky = float(H) / float(h);
        if (std::abs(kx - ky) < 0.02f * kx && kx <= 64.0f) {   // half, quarter... sizes match; square targets (lake, cube faces) do not
            screen_shape   = true;
            const float c  = float(m_scope_corner + 48);
            want           = { 0, 0, LONG(std::min(float(w), std::ceil(c / kx))), LONG(std::min(float(h), std::ceil(c / ky))) };
        }
    }
    const RsClone* rc = screen_shape ? clone_for(ctx, g_game_rs) : nullptr;
    if (rc) {
        D3D11_RECT rect = want;
        if (rc->game_scissor && g_game_rects > 0) {
            rect.left   = std::max(want.left, g_game_rect.left);
            rect.top    = std::max(want.top, g_game_rect.top);
            rect.right  = std::max(rect.left, std::min(want.right, g_game_rect.right));
            rect.bottom = std::max(rect.top, std::min(want.bottom, g_game_rect.bottom));
        }
        if (cur != rc->clone) ctx->RSSetState(rc->clone);
        if (!g_applied_on || !same_rect(rect, g_applied)) {
            ctx->RSSetScissorRects(1, &rect);
            g_applied    = rect;
            g_applied_on = true;
        }
        g_changed = true;
        m_scope_scissored++;
    } else if (g_rs_clones.contains(cur) || g_applied_on) {
        ctx->RSSetState(g_game_rs);
        if (g_game_rects > 0) ctx->RSSetScissorRects(1, &g_game_rect);
        else ctx->RSSetScissorRects(0, nullptr);
        g_applied_on = false;
    }
    if (cur) cur->Release();
}

// After the scope call: the game's own rasterizer state and scissor back, as it last set them.
void FarCry5Stereo::scope_scissor_end(ID3D11DeviceContext* ctx)
{
    if (g_changed && g_game_known) {
        ctx->RSSetState(g_game_rs);
        if (g_game_rects > 0) ctx->RSSetScissorRects(1, &g_game_rect);
        else ctx->RSSetScissorRects(0, nullptr);
    }
    g_changed    = false;
    g_game_known = false;
    g_applied_on = false;
    g_game_rs    = nullptr;
}

namespace
{
    void mouse_right(bool down)
    {
        INPUT in{};
        in.type       = INPUT_MOUSE;
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in, sizeof(INPUT));
    }
}

// Look-through test for the eye of this frame, once per frame at its first camera upload (before the weapon pass draws,
// so the gun can be left out of that eye).
// scopeauto (default): the eye is behind the gun on its sight line: within 11 cm of the barrel's line (15 cm to let go),
// behind the muzzle, within 40 cm of the controller (50 cm) and looking along the barrel within 30 deg (40 deg). The
// lens is then put on the eye's own line of sight to the aim point, 7 cm in front of the eye: no per-weapon offset.
// scopeauto off: the calibrated ocular lens of scopecal (within scopedist / scopeangle of its axis), as before.
// A weapon starts the lens only with a magnification above 1x: its scopecal entry, or what scopelearn measured from the
// game's own aim-down-sights zoom.
void FarCry5Stereo::scope_frame(int frame, const glm::vec3& eye_cam, const glm::mat3& eye_rot)
{
    if (!m_scope.load()) {
        m_scope_st.active = false;
        m_scope_probe_lt  = false;
        if (m_scope_probe_mouse) { mouse_right(false); m_scope_probe_mouse = false; }
        return;
    }
    if (m_scope_st.frame == frame) return;
    m_scope_frames++;
    ScopeState st{};
    st.frame   = frame;
    st.eye_pos = eye_cam;
    st.eye_rot = eye_rot;
    const int eye = frame & 1;
    ScopeCal  cal{};
    bool      have_cal = false;
    st.sig = m_weapon_sig.load();
    {
        std::lock_guard _{ m_scope_mtx };
        if (auto it = m_scope_cal.find(st.sig); it != m_scope_cal.end()) {
            cal          = it->second;
            have_cal     = true;
            st.cal_found = true;
        } else if (m_scope_any.load()) {
            cal      = m_scope_default;
            have_cal = true;
        }
        // A magnifying entry needs sight glass now: a gun whose weapon pass draws no reticle or scope glass (Weapon_Reticle*,
        // Weapon_ReticleGlow) has iron sights, whatever its entry (Homard 24 Oct: the M60 "without any sights is detected as
        // having a scope": the weapon id is the largest mesh, shared with a scoped one learned x3.3 on 11 Oct, and iron sights
        // can zoom past scopelearn's x3.0 too). It counts as a 1x entry meanwhile.
        if (have_cal && cal.mag > 1.01f && st.sig != 0) {
            if (!optic_now(st.sig)) {
                if (m_optic_warned.insert(st.sig).second)
                    spdlog::info("[Scope] weapon {}: its entry says x{:.1f}, but it draws no sight glass now (iron sights): no scope picture", st.sig, cal.mag);
                cal.mag = 1.0f;
            } else {
                m_optic_warned.erase(st.sig);
            }
        }
    }
    // The weapon id (largest mesh of the weapon pass) changes for a moment while a rifle cycles its bolt or aims down
    // sights: the last magnifying entry stays in effect for 1.5 s through a change to an id without one (scopesticky: only while
    // the dominant eye was behind the scope, not for a gun taken next).
    {
        const auto tnow = std::chrono::steady_clock::now();
        if (have_cal && cal.mag > 1.01f) {
            m_scope_sticky     = cal;
            m_scope_sticky_sig = st.sig;
            m_scope_sticky_at  = tnow;
        } else if (!have_cal && m_scope_sticky_sig != 0 && tnow - m_scope_sticky_at < std::chrono::milliseconds(1500) && (!m_scope_sticky_eye.load() || m_scope_dom_was)) {
            cal      = m_scope_sticky;
            have_cal = true;
        }
    }
    const bool known = have_cal;   // an entry exists (magnifying or not)
    // Magnifying optics only (Homard 2026-10-11): an entry at 1x (red dot, holographic) never starts the lens.
    if (have_cal && cal.mag <= 1.01f) have_cal = false;
    const bool can    = m_motion.load() && m_mo.ctrl_valid && !m_menu_screen_on && !m_veh_driving.load() && !m_cine_on;
    const bool autom  = m_scope_auto.load();
    const bool was    = m_scope_on_eye[eye];
    bool       sight  = false;   // scopeauto: this eye is on the gun's sight line (whatever the weapon)
    bool       on     = false;
    if (can && autom) {
        const glm::vec3 D     = m_mo.barrel_dir;
        const glm::vec3 e     = eye_cam - (m_mo.aim_origin + m_mo.draw_shift);   // the shots' line at the drawn muzzle (gunanchor moves it with the gun; sightaim: the sight line)
        const float     along = -glm::dot(e, D);   // how far behind the muzzle
        const float     perp  = glm::length(e + D * along);
        const float     ctrl  = glm::length(eye_cam - m_mo.ctrl_pos_world);
        const glm::vec3 fwd   = -glm::transpose(eye_rot)[2];
        const float     head  = glm::degrees(std::acos(std::clamp(glm::dot(fwd, D), -1.0f, 1.0f)));
        sight = was ? (perp < 0.15f && ctrl < 0.50f && head < 40.0f && along > 0.05f) : (perp < 0.11f && ctrl < 0.40f && head < 30.0f && along > 0.10f);
        // The lens on the eye's line of sight to the aim point (same aim distance as the scope camera).
        const float     hit  = fc5::aim::last_hit();
        const float     dist = hit > 2.0f ? hit : m_mo.converge > 2.0f ? m_mo.converge : std::max(2.0f, m_converge_m.load());
        const glm::vec3 aim  = m_mo.barrel_origin + D * dist;
        glm::vec3       u    = aim - eye_cam;
        u                    = glm::length(u) > 1e-3f ? glm::normalize(u) : D;
        st.axis   = u;
        st.lens   = eye_cam + u * 0.07f;
        st.radius = have_cal || known ? cal.radius : m_scope_default.radius;
        st.dist   = perp;
        st.angle  = head;
        on        = (sight && have_cal) || (m_scope_force.load() && have_cal);
        // scopemodel: the eyepiece fixed to the gun (the entry's offset in controller axes), facing back along the line to
        // the aim point (the scope zeroed there).
        const ScopeCal& mc   = have_cal || known ? cal : m_scope_default;
        glm::vec3       moff{ mc.off[0], mc.off[1], mc.off[2] };
        // Entries written before the eyepiece was measured carry the old default (7.5 cm straight above the controller, 14 cm
        // ahead of the eye on the rifle): the measured default instead, until scopelenshere sets the gun's own.
        if (std::abs(moff.x) < 1e-4f && std::abs(moff.y - 0.075f) < 1e-4f && std::abs(moff.z) < 1e-4f) moff = { m_scope_default.off[0], m_scope_default.off[1], m_scope_default.off[2] };
        // The glass's place on the gun as learned from the game's own drawing (scopesteady), once known.
        const UINT  lkey = st.cal_found ? st.sig : (m_scope_sticky_sig ? m_scope_sticky_sig : st.sig);
        float       lrad = 0.0f;
        if (m_scope_steady.load()) {
            if (auto it = m_glass_learn.find(lkey); it != m_glass_learn.end() && it->second.n >= 20) { moff = it->second.off; lrad = it->second.radius; }
        }
        st.mlens   = m_mo.ctrl_pos_world + m_mo.ctrl_rot_world * moff;
        // sightaim: the scope's axis is the gun's sight line (the game centres the scope on it when aiming down sights), so an
        // eyepiece only guessed (scopecal's default offset, no glass learned yet) is put on it. A learned glass stays where the
        // game draws it (x12 rifle, simulator 18 Oct: its line, read just before the game's 2D scope view, runs ~6 mm under it).
        if (m_mo.sight_aim && lrad <= 0.005f) {
            const glm::vec3 on = m_mo.aim_origin + m_mo.draw_shift;   // the line where the gun is drawn (gunanchor)
            st.mlens           = on + D * glm::dot(st.mlens - on, D);
        }
        glm::vec3 ma = aim - st.mlens;
        st.maxis   = glm::length(ma) > 1e-3f ? glm::normalize(ma) : D;
        // The picture turns and rolls with the gun (Homard 2026-10-12: "it should also rotate with the gun, really glued"):
        // its right and up are the gun model's (the weapon placement's axes), square to the scope's axis.
        const glm::mat3 gun = m_mo.ctrl_rot_world * m_hand_map.corr;
        glm::vec3       gr  = gun[0] - st.maxis * glm::dot(gun[0], st.maxis);
        if (glm::length(gr) < 1e-3f) gr = glm::cross(st.maxis, glm::vec3{ 0.0f, 0.0f, 1.0f });
        st.mright  = glm::normalize(gr);
        st.mup     = glm::cross(-st.maxis, st.mright);
        // On the eyepiece's face, the eyepiece's size, so it sits in the scope's ring at any eye distance (Homard's headset
        // hold 2026-10-12: the eyepiece 23 cm ahead of the eye, not 6.6; the 3 cm shift with its size grown for 6.6 cm made
        // the picture larger than the ring there, hovering in front of the scope). scopelensfwd can still move it along the axis.
        const float ahead_m = std::clamp(m_scope_lens_fwd.load(), 0.0f, 0.2f);
        st.mlens  += st.maxis * ahead_m;
        st.mradius = lrad > 0.005f ? lrad : mc.radius;
        // The field at the picture's edge: fixed per optic (the eyepiece's size at the 6.6 cm eye relief, divided by the
        // magnification when shown), whatever the eye's distance or the glass's size on screen. Following the eye's distance
        // made the view zoom in and out (Homard, headset 2026-10-12 21:35), and a field wider than the rendered picture left
        // its square edge visible in the round glass.
        st.mfield  = st.mradius / std::max(0.02f, m_scope_relief.load());
        // Off once the gun is angled away from the eye's line (Homard: "off as soon as the gun angle is too far one way
        // or the other"): full up to 60 % of scopecut, fading out to nothing at scopecut.
        const glm::vec3 to_lens = st.mlens - eye_cam;
        const float     ang     = glm::length(to_lens) > 1e-4f ? glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(to_lens), st.maxis), -1.0f, 1.0f))) : 180.0f;
        const float     cut     = std::clamp(m_scope_cut_deg.load(), 2.0f, 45.0f);
        st.mangle  = ang;
        st.mfade   = 1.0f - std::clamp((ang - 0.6f * cut) / (0.4f * cut), 0.0f, 1.0f);
        st.mvalid  = true;
        st.cpos    = m_mo.ctrl_pos_world;
        st.crot    = m_mo.ctrl_rot_world;
        // On the gun, the scope is on while the gun points at the eye (the fade), not by the sight-line test: at a real hold
        // (eyepiece 23 cm ahead, the eye ~10 cm from the barrel's line) that test flickered on and off every few seconds.
        if (scope_on_model()) on = (have_cal && st.mfade > 0.0f) || (m_scope_force.load() && have_cal);
    } else if (can && have_cal) {
        st.lens   = m_mo.ctrl_pos_world + m_mo.ctrl_rot_world * glm::vec3{ cal.off[0], cal.off[1], cal.off[2] };
        st.axis   = m_mo.barrel_dir;
        st.radius = cal.radius;
        const glm::vec3 v     = st.lens - eye_cam;
        st.dist               = glm::length(v);
        const float     axial = glm::dot(v, st.axis);
        st.angle              = st.dist > 1e-4f ? glm::degrees(std::acos(std::clamp(axial / st.dist, -1.0f, 1.0f))) : 0.0f;
        const float     cm    = st.dist * 100.0f;
        on = was ? (cm < m_scope_off_cm.load() && st.angle < m_scope_off_deg.load()) : (cm < m_scope_on_cm.load() && st.angle < m_scope_on_deg.load());
        on = (on && axial > 0.0f) || m_scope_force.load();
    }
    const bool usable = can && (have_cal || (autom && sight));
    m_scope_on_eye[eye]    = autom && !scope_on_model() ? sight : on;
    // Automatic dominant eye: the one nearer the sight line (scopeauto) or the lens axis.
    m_scope_angle_eye[eye] = usable ? (autom ? st.dist * 100.0f : st.angle) : 1e6f;
    int dom = m_scope_eye.load();
    if (dom == 2) dom = m_scope_angle_eye[0] <= m_scope_angle_eye[1] ? 0 : 1;
    st.active   = on && eye == dom;
    st.dominant = eye == dom;
    if (eye == dom) {
        const auto now = std::chrono::steady_clock::now();
        if (st.active && !m_scope_dom_was) m_scope_since = now;
        m_scope_dom_was = st.active;
        // Magnification ramps from 1 to the optic's over 0.15 s (Better Scopes ramps its zoom too).
        // (No ramp on the gun: each start zoomed in from x1, seen as zooming in and out.)
        const float t = scope_on_model() ? 1.0f : st.active ? std::chrono::duration<float>(now - m_scope_since).count() / 0.15f : 0.0f;
        // scopezoomstick: the gun's own zoom on its optic (the right stick while held with two hands, the picture on).
        const float optic = have_cal ? std::max(1.0f, cal.mag) * scope_zoom_tick(st.sig, std::max(1.0f, cal.mag), st.active) : 1.0f;
        st.mag        = 1.0f + (optic - 1.0f) * std::clamp(t, 0.0f, 1.0f);
        st.full_mag   = optic;
        if (m_trigger_was) m_scope_fire_t = now;
        // Through a scope (the picture on the gun shown) or any sight (red dot, iron sights: the eye on the gun's sight line):
        // the sight spread.
        sight_spread_tick((st.active && (!scope_on_model() || st.mfade > 0.3f)) || sight);
        scope_learn_tick(can && !m_in_vehicle.load(), can && (autom ? sight : on), st.sig, known);
    }
    const bool now_on = autom ? sight : on;
    if (now_on != was) {
        if (autom)
            spdlog::info("[Scope] {} eye {} the gun's sight line: {:.1f} cm from the barrel's line, head {:.1f} deg off the barrel (weapon {}, {}){}", eye ? "right" : "left",
                         sight ? "on" : "off", st.dist * 100.0f, st.angle, st.sig, have_cal ? fmt::format("x{:.1f}", cal.mag) : (known ? "not magnifying" : "no entry yet"),
                         eye == dom ? " (dominant eye)" : "");
        else
            spdlog::info("[Scope] {} eye {} the scope: {:.1f} cm from the lens, {:.1f} deg off its axis (weapon {}, {}){}", eye ? "right" : "left", on ? "behind" : "away from",
                         st.dist * 100.0f, st.angle, st.sig, st.cal_found ? "calibrated" : (have_cal ? "default entry" : "no entry"), eye == dom ? " (dominant eye)" : "");
    }
    if (m_scope_log.load() > 0) {
        m_scope_log--;
        spdlog::info("[Scope] frame {} eye {}: weapon {} entry {} lens ({:.3f} {:.3f} {:.3f}) eye ({:.3f} {:.3f} {:.3f}) dist {:.1f} cm angle {:.1f} deg sight {} on {} active {} mag {:.2f}",
                     frame, eye, st.sig, st.cal_found ? "own" : (have_cal ? "default" : "none"), st.lens.x, st.lens.y, st.lens.z, eye_cam.x, eye_cam.y, eye_cam.z, st.dist * 100.0f,
                     st.angle, sight, on, st.active, st.mag);
    }
    m_scope_st = st;
    if (eye == dom) m_scope_dom_st = st;   // for the scope call after the right eye (scopepass)
}

// scopezoomstick, once per eye pair (the scope's eye): the zoom factor of gun sig on its optic. While the scope picture is
// on and the gun is held with two hands, each push of the right stick up or down jumps to the next magnification of
// x1, x4, x8, x16, x32 (Homard 24 Oct: "change the zoom so the increments are x1, x4, x8, x16, etc.."); the stick comes back
// near the centre before the next step. Saved per gun at each step.
float FarCry5Stereo::scope_zoom_tick(UINT sig, float optic, bool active)
{
    float k = 1.0f;
    {
        std::lock_guard _{ m_scope_mtx };
        if (auto it = m_scope_zoom.find(sig); it != m_scope_zoom.end()) k = it->second;
    }
    const float y    = m_zoom_stick_y.load(), x = m_zoom_stick_x.load();
    const bool  push = std::abs(y) > 0.6f && std::abs(y) > std::abs(x);
    if (std::abs(y) < 0.3f) m_scope_zoom_latch = false;
    if (sig != 0 && active && push && !m_scope_zoom_latch && m_scope_zoom_stick.load() && m_two_hand.load() && !m_menu_screen_on) {
        m_scope_zoom_latch = true;
        // x1 is drawn at x1.05: at x1 the scope picture stops (a magnifying optic only), the gun's dark glass would show.
        static constexpr int kSteps[] = { 1, 4, 8, 16, 32 };
        const float eff = optic * k;
        int n = y > 0.0f ? kSteps[4] : kSteps[0];
        if (y > 0.0f) { for (int s : kSteps) if ((float)s > eff + 0.01f) { n = s; break; } }
        else { for (int i = 4; i >= 0; --i) if ((float)kSteps[i] < eff - 0.06f) { n = kSteps[i]; break; } }
        const float nk  = std::max(1.05f, (float)n) / optic;
        if (std::abs(nk - k) > 1e-4f) {
            k = nk;
            {
                std::lock_guard _{ m_scope_mtx };
                m_scope_zoom[sig] = k;
            }
            m_persist[fmt::format("scopezoom {}", sig)] = fmt::format("scopezoom {:.4f} {}", k, sig);
            save_settings();
            spdlog::info("[Scope] scopezoomstick: weapon {} zoom x{} (x{:.2f} of its optic, saved)", sig, n, k);
        }
    }
    return k;
}

// scopelearn: the magnification of the gun in hand from the game's own aim-down-sights zoom (its camera narrows by the
// optic's magnification: x3.9 / x6 for Homard's scoped rifle, headset 2026-10-11). Learned when the player aims down
// sights, or by a short press of aim down sights (0.45 s) the first time the eye comes to the sight line of a gun with
// no entry. From x3.0 the gun gets a magnifying entry (scopecal, saved); below, a 1x entry (never starts the lens).
void FarCry5Stereo::scope_learn_tick(bool on_foot, bool at_scope, UINT sig, bool known)
{
    using clock    = std::chrono::steady_clock;
    const auto now = clock::now();
    if (sig != m_scope_sig_last) {
        m_scope_sig_last  = sig;
        m_scope_sig_since = now;
    }
    const bool  lt  = m_lt_sent.load();
    const float fov = m_game_hfov.load();
    if (lt != m_scope_lt_was) {
        m_scope_lt_was = lt;
        if (!lt) m_scope_lt_up_since = now;
    }
    // The game camera's width without aiming down sights, once settled after a release.
    if (!lt && now - m_scope_lt_up_since > std::chrono::milliseconds(500) && fov > 1.0f) m_scope_hip_fov = fov;
    auto& a = m_scope_ads;
    if (a.state == 0) {
        if (!m_scope_learn.load() || sig == 0 || !on_foot || m_scope_hip_fov <= 1.0f) return;
        // sightlearn's own short press is not read: it lets go as soon as it has the sight line (or when the weapon id changes,
        // as a sniper's does while it zooms), before the zoom ends (Homard's sniper 24474 read x1.83 that way, 21 Oct 13:59).
        if (m_sight_probe_lt.load()) return;
        // A gun with a 1x entry is read again while the player aims down sights; a magnifying read then replaces the entry.
        bool upgrade = false;
        if (known) {
            if (!lt) return;
            std::lock_guard _{ m_scope_mtx };
            const auto it = m_scope_cal.find(sig);
            if (it == m_scope_cal.end() || it->second.mag > 1.01f) return;
            upgrade = true;
        }
        // A read that saw no zoom (the press landed while the game ignored it) is tried again 3 s later, three tries per weapon.
        const auto tried = m_scope_probed.find(sig);
        const bool again = tried == m_scope_probed.end() || (tried->second.n < 3 && now - tried->second.last > std::chrono::seconds(3));
        const bool probe = !known && at_scope && !lt && again && now - m_scope_sig_since > std::chrono::seconds(1) && ads_probe_ok(sig);   // adsprobe
        if (!probe && !lt) return;
        a         = {};
        a.state   = 1;
        a.t0      = now;
        a.drop    = now;
        a.hip     = m_scope_hip_fov;
        a.min     = probe ? m_scope_hip_fov : fov;
        a.sig     = sig;
        a.probe   = probe;
        a.upgrade = upgrade;
        a.optic   = optic_now(sig);   // the gun as held before the press (while the game aims down sights its scope view may replace it)
        if (probe) {
            auto& t = m_scope_probed[sig];
            ++t.n;
            t.last = now;
            m_scope_probe_lt = true;
            // A game reading the mouse instead of the pad (no pad read for 1 s: flat and simulator tests) aims down sights with
            // the right mouse button.
            m_scope_probe_mouse = GetTickCount64() - m_pad_last_poll.load() > 1000;
            if (m_scope_probe_mouse) mouse_right(true);
            spdlog::info("[Scope] scopelearn: weapon {} has no scope entry; aiming down sights until its zoom settles (at most 1 s) to read it (game camera {:.1f} deg)", sig, a.hip);
        }
        return;
    }
    if (fov > 1.0f && fov < a.min - 0.05f) { a.min = fov; a.drop = now; }   // still narrowing
    if (sig != 0 && sig != a.sig && a.extra_n < 4) a.extra[a.extra_n++] = sig;   // the ids the weapon shows while aiming down sights
    const float held = std::chrono::duration<float>(now - a.t0).count();
    // The probe ends once the zoom has settled (no narrowing for 0.2 s, at least 0.35 s in), at most after 1 s.
    const bool  settled = held > 0.35f && std::chrono::duration<float>(now - a.drop).count() > 0.2f;
    const bool  end  = a.probe ? (settled || held > 1.0f || !on_foot) : (!lt || !on_foot);
    if (!end) return;
    m_scope_probe_lt = false;
    if (m_scope_probe_mouse) { mouse_right(false); m_scope_probe_mouse = false; }
    a.state          = 0;
    // A press let go before the zoom settled says nothing about the optic.
    if (!a.probe && !settled) {
        if (!a.upgrade) spdlog::info("[Scope] scopelearn: weapon {}: aim down sights let go before the zoom settled ({:.2f} s, camera {:.1f} -> {:.1f} deg): not read", a.sig, held, a.hip, a.min);
        return;
    }
    if (held < 0.2f || a.hip <= 1.0f || a.min >= a.hip - 0.5f) {
        if (a.upgrade) return;
        const auto tried = m_scope_probed.find(a.sig);
        if (tried != m_scope_probed.end()) tried->second.last = now;
        spdlog::info("[Scope] scopelearn: weapon {}: no zoom read ({:.2f} s, camera {:.1f} -> {:.1f} deg){}", a.sig, held, a.hip, a.min,
                     tried != m_scope_probed.end() && tried->second.n < 3 ? fmt::format(", try {} of 3, again in 3 s", tried->second.n) : std::string{});
        return;
    }
    const float zoom = std::tan(glm::radians(a.hip) * 0.5f) / std::tan(glm::radians(a.min) * 0.5f);
    // Aiming down sights with a red dot narrows the camera too (x2.26 on the lodge-night SMG, simulator 2026-10-11; the
    // scoped rifle x3.9): from x3.0 on it is a magnifying scope, if the gun draws sight glass (the M60's iron sights zoom
    // x3.3, Homard 24 Oct).
    bool optic = a.optic || optic_now(a.sig);
    for (int i = 0; i < a.extra_n && !optic; ++i) optic = optic_now(a.extra[i]);
    if (zoom >= 3.0f && !optic) spdlog::info("[Scope] scopelearn: weapon {}: zooms x{:.2f} but draws no sight glass: iron sights, not a scope", a.sig, zoom);
    const float mag  = zoom >= 3.0f && optic ? std::round(zoom * 10.0f) / 10.0f : 1.0f;
    if (a.upgrade && mag <= 1.0f) return;   // the 1x entry was right
    ScopeCal    c    = m_scope_default;
    c.mag            = mag;
    bool extra_set[4]{};
    {
        std::lock_guard _{ m_scope_mtx };
        // An entry set meanwhile (by hand) is kept; a 1x entry being read again is replaced (with the ids seen with it at 1x).
        const auto it = m_scope_cal.find(a.sig);
        if (it != m_scope_cal.end() && !(a.upgrade && it->second.mag <= 1.01f)) return;
        m_scope_cal[a.sig] = c;
        for (int i = 0; i < a.extra_n; ++i) {
            const auto e = m_scope_cal.find(a.extra[i]);
            if (e != m_scope_cal.end() && !(a.upgrade && e->second.mag <= 1.01f)) continue;
            m_scope_cal[a.extra[i]] = c;
            extra_set[i]            = true;
        }
    }
    for (int i = 0; i < a.extra_n; ++i)
        if (extra_set[i]) m_persist[fmt::format("scopecal {}", a.extra[i])] = fmt::format("scopecal {:.4f} {:.4f} {:.4f} {:.4f} {:.2f} {}", c.off[0], c.off[1], c.off[2], c.radius, c.mag, a.extra[i]);
    m_persist[fmt::format("scopecal {}", a.sig)] = fmt::format("scopecal {:.4f} {:.4f} {:.4f} {:.4f} {:.2f} {}", c.off[0], c.off[1], c.off[2], c.radius, c.mag, a.sig);
    save_settings();
    spdlog::info("[Scope] scopelearn: weapon {}: aim down sights narrows the game camera {:.1f} -> {:.1f} deg = x{:.2f}: {} (saved{})", a.sig, a.hip, a.min, zoom,
                 mag > 1.0f ? fmt::format("magnifying scope, x{:.1f}", mag) : std::string{ "not magnifying (iron sights, red dot or holographic)" },
                 a.upgrade ? ", replaces its 1x entry" : "");
}
// The lens into this frame's eye image (DLSS output), before the aim dot (which the reticle replaces in that eye).
void FarCry5Stereo::scope_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img)
{
    const int fr = m_frame.load();
    if (!m_scope.load() || !m_scope_lens.load() || !eye_img) return;
    if (!m_scope_st.active || m_scope_st.frame != fr || m_hist[0].cur_frame != fr || m_menu_screen_on) return;
    D3D11_TEXTURE2D_DESC d{};
    eye_img->GetDesc(&d);
    const float W = float(d.Width), H = float(d.Height);
    // scopesrc 1: this eye was drawn from the scope camera; the lens is placed with its own camera.
    const bool      src_eye = scope_src_eye() && m_scope_src_frame == fr;
    const glm::mat4 eye_vrp = src_eye ? m_scope_eye_vrp : m_hist[0].cur_vrp;
    const glm::vec3 eye_cam = src_eye ? m_scope_eye_cam : m_hist[0].cur_cam;
    auto px = [&](const glm::vec3& w, glm::vec2& out) {
        const glm::vec4 cl = eye_vrp * glm::vec4{ w - eye_cam, 1.0f };
        if (cl.w <= 0.005f) return false;
        out = { (cl.x / cl.w * 0.5f + 0.5f) * W, (0.5f - cl.y / cl.w * 0.5f) * H };
        return true;
    };
    const ScopeState& st = m_scope_st;
    const glm::mat3   to_world = glm::transpose(st.eye_rot);
    glm::vec2         c{}, edge{}, aim{};
    if (!px(st.lens, c) || !px(st.lens + to_world[0] * st.radius, edge)) { m_scope_fails++; return; }
    const float     hit = fc5::aim::last_hit();
    const glm::vec3 p   = m_mo.aim_origin + m_mo.barrel_dir * (hit > 0.5f ? hit : m_mo.converge);
    if (!px(p, aim)) aim = c;
    fc5::scope::Lens l{};
    l.lens_c[0] = c.x; l.lens_c[1] = c.y;
    l.lens_r    = glm::length(edge - c);
    if (l.lens_r < 4.0f || l.lens_r > 2.0f * W) { m_scope_fails++; return; }
    l.ring_r    = l.lens_r * 1.15f + 2.0f;
    l.src_c[0]  = aim.x; l.src_c[1] = aim.y;
    l.inv_mag   = 1.0f / std::max(1.0f, st.mag);
    l.reticle_w = m_scope_reticle.load() ? std::max(1.0f, 1.2f * W / 3072.0f) : 0.0f;
    if (m_scope_eyebox.load()) {
        // The eye's offset from the scope's axis, in the eye's view (x right, y up): the visible circle moves the
        // other way, so the dark crescent is on the side the eye has moved to.
        const glm::vec3 e    = st.eye_pos - st.lens;
        const glm::vec3 perp = e - st.axis * glm::dot(e, st.axis);
        const glm::vec3 ev   = st.eye_rot * perp;
        const float     box  = std::max(0.1f, m_scope_eyebox_cm.load()) / 100.0f;
        l.vig_c[0]  = c.x - ev.x / box * l.lens_r;
        l.vig_c[1]  = c.y + ev.y / box * l.lens_r;
        l.vig_r     = l.lens_r;
        l.vig_soft  = 0.3f * l.lens_r;
    }
    if (src_eye) {
        // The lens shows this eye's own DLSS image (the scope camera's view, centred on the aim point); black around it.
        glm::vec2   edge_y{};
        const float ahead = -(st.eye_rot * (st.lens - st.eye_pos)).z;
        const auto& sc    = m_scope_src_cam;
        if (ahead > 0.01f && sc.tx > 0.0f && sc.ty > 0.0f && px(st.lens + to_world[1] * st.radius, edge_y)) {
            const float t_lens = st.radius / ahead;
            const float ry     = std::max(1.0f, glm::length(edge_y - c));
            l.scope_srv        = fc5::dlss::output_srv();
            l.scope_own        = l.scope_srv == nullptr;   // no DLSS image: the eye image itself (with its HUD)
            l.scope_k[0]       = (t_lens / l.lens_r) / (sc.mag * 2.0f * sc.tx);
            l.scope_k[1]       = (t_lens / ry) / (sc.mag * 2.0f * sc.ty);
            l.outside_black    = true;
        }
    } else if (m_scope_pass.load() && !m_scope_pass_broken) {
        // Step 3: the scope call's picture, once the pass has run a few pairs in a row (its history settled) and the
        // magnification has ramped up to the optic's; the zoom of this eye's image until then.
        const ScopeCam& ic  = m_scope_img_cam;
        const int       age = fr - ic.frame;
        glm::vec2       edge_y{};
        const float     ahead = -(st.eye_rot * (st.lens - st.eye_pos)).z;
        if (fc5::scope::picture() && ic.valid && age >= 0 && age <= 2 * std::clamp(m_scope_every.load(), 1, 4) && m_scope_run >= 4 && scope_pic_mag_ok(st.mag, st.full_mag, ic.mag) && ic.tx > 0.0f && ic.ty > 0.0f && ahead > 0.01f &&
            px(st.lens + to_world[1] * st.radius, edge_y)) {
            // A lens pixel's angle in this eye (tangent per pixel), shrunk by the magnification, in the picture's tangents.
            const float t_lens = st.radius / ahead;
            const float ry     = std::max(1.0f, glm::length(edge_y - c));
            l.scope_srv        = fc5::scope::picture();
            l.scope_k[0]       = (t_lens / l.lens_r) / (st.mag * 2.0f * ic.tx);
            l.scope_k[1]       = (t_lens / ry) / (st.mag * 2.0f * ic.ty);
        }
    }
    if (fc5::scope::draw(ctx, eye_img, l)) {
        m_scope_draws++;
        if (l.scope_srv || l.scope_own) m_scope_pass_draws++;
        m_scope_drawn_frame = fr;
        static int logged = 0;
        if (logged < 3 || m_scope_log.load() > 0) {
            logged++;
            spdlog::info("[Scope] lens drawn, frame {}: centre ({:.0f}, {:.0f}) radius {:.0f} px, aim point ({:.0f}, {:.0f}), x{:.2f}, image {}x{}", fr, c.x, c.y, l.lens_r, aim.x, aim.y,
                         st.mag, d.Width, d.Height);
        }
    } else {
        m_scope_fails++;
    }
}

// scopemodel, a weapon draw (render thread, before it): the scope glass's glow draws (vertex shaders reading
// Weapon_ReticleGlow). In the scoped eye the draw goes ahead with the mask shader into the glass mask instead of the
// game's targets (depth tested against the gun; reddot_end puts the game's state back after it). Elsewhere, on a gun
// with a magnifying scope, it is left out (true): it paints a red disc on the eyepiece outside aiming down sights.
bool FarCry5Stereo::scope_glow_draw(ID3D11DeviceContext* ctx)
{
    if (m_scope_call) return false;
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    bool glow = false;
    {
        std::lock_guard _{ m_ps_mtx };
        glow = m_glow_vs.contains(vs);
    }
    vs->Release();
    if (!glow) return false;
    m_optic_this_frame = true;   // sight glass: this weapon may magnify (scope_frame)
    const int fr = m_frame.load();
    if (m_scope_glass.load() && m_scope_st.active && m_scope_st.frame == fr && m_scope_st.mvalid) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        ID3D11Resource* res = nullptr;
        if (dsv) dsv->GetResource(&res);
        else if (rtvs[0]) rtvs[0]->GetResource(&res);
        D3D11_TEXTURE2D_DESC td{};
        if (res) { static_cast<ID3D11Texture2D*>(res)->GetDesc(&td); res->Release(); }
        ID3D11RenderTargetView* mrtv = nullptr;
        ID3D11PixelShader*      mps  = fc5::scope::mask_ps(dev);
        const bool ok = td.Width > 0 && td.SampleDesc.Count == 1 && mps && fc5::scope::glass_mask(dev, td.Width, td.Height, &mrtv, nullptr);
        dev->Release();
        if (!ok) {
            for (auto* v : rtvs) if (v) v->Release();
            if (dsv) dsv->Release();
            m_scope_glow_hidden++;
            return true;   // no mask: at least no red disc
        }
        if (m_scope_glass_frame != fr) {
            const float zero[4]{};
            ctx->ClearRenderTargetView(mrtv, zero);
            m_scope_glass_frame = fr;
        }
        UINT           nvp = 1;
        D3D11_VIEWPORT vp{};
        ctx->RSGetViewports(&nvp, &vp);
        m_scope_glass_vp[0] = vp.TopLeftX; m_scope_glass_vp[1] = vp.TopLeftY; m_scope_glass_vp[2] = vp.Width; m_scope_glass_vp[3] = vp.Height;
        m_scope_glass_wh[0] = (float)td.Width; m_scope_glass_wh[1] = (float)td.Height;
        // The same save and restore as the red dot's mask (never both on one draw).
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) m_reddot_saved.rtv[i] = rtvs[i];
        m_reddot_saved.dsv = dsv;
        ctx->OMGetBlendState(&m_reddot_saved.blend, m_reddot_saved.factor, &m_reddot_saved.mask);
        m_reddot_saved.ninst = 16;
        ctx->PSGetShader(&m_reddot_saved.ps, m_reddot_saved.inst, &m_reddot_saved.ninst);
        ctx->OMSetRenderTargets(1, &mrtv, dsv);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->PSSetShader(mps, nullptr, 0);
        m_reddot_swapped = true;
        m_scope_glass_draws++;
        return false;
    }
    bool magnifying = false;
    {
        std::lock_guard _{ m_scope_mtx };
        auto it = m_scope_cal.find(m_weapon_sig.load());
        if (it == m_scope_cal.end() && m_scope_sticky_sig) it = m_scope_cal.find(m_scope_sticky_sig);
        magnifying = it != m_scope_cal.end() && it->second.mag > 1.01f;
    }
    if (magnifying) {
        m_scope_glow_hidden++;
        return true;
    }
    return false;
}

// scopesteady: the glass stats read back (a frame or two late) become the glass's place on the gun: its centre's ray
// against the gun-fixed disc's plane gives the point, its pixel count the radius; averaged per weapon over steady frames
// (not firing for 0.6 s, the gun pointing at the eye), saved as scopeglassat.
void FarCry5Stereo::scope_glass_learn(ID3D11DeviceContext* ctx)
{
    int      fr = 0;
    uint32_t r[3]{};
    for (int guard = 0; guard < 4 && fc5::scope::glass_result(ctx, fr, r); ++guard) {
        const GlassSnap& gs = m_glass_snap[fr & 7];
        if (gs.frame != fr || !gs.steady || r[0] < 400 || gs.sig == 0) continue;
        const float     cx = float(r[1]) / float(r[0]) + 0.5f, cy = float(r[2]) / float(r[0]) + 0.5f;
        const float     rr = std::sqrt(float(r[0]) / 3.14159265f);
        const float     sx = std::max(1e-4f, gs.vp[2] / gs.W), sy = std::max(1e-4f, gs.vp[3] / gs.H);
        const float     qx = (cx - gs.vp[0]) / sx, qy = (cy - gs.vp[1]) / sy;
        const glm::mat4 inv = glm::inverse(gs.vrp);
        const glm::vec3 a0{ inv[0] }, a1{ inv[1] }, a2 = 0.5f * glm::vec3{ inv[2] } + glm::vec3{ inv[3] };
        auto ray = [&](float px, float py) {
            glm::vec3 d = a0 * (px / gs.W * 2.0f - 1.0f) + a1 * (1.0f - py / gs.H * 2.0f) + a2;
            if (glm::dot(d, gs.lens - gs.eye) < 0.0f) d = -d;
            return glm::normalize(d);
        };
        const glm::vec3 dir = ray(qx, qy);
        const float     den = glm::dot(dir, gs.axis);
        if (std::abs(den) < 1e-3f) continue;
        const float t = glm::dot(gs.lens - gs.eye, gs.axis) / den;
        if (t < 0.03f || t > 1.5f) continue;
        const glm::vec3 p      = gs.eye + dir * t;
        const glm::vec3 off    = glm::transpose(gs.ctrl_rot) * (p - gs.ctrl_pos);
        const glm::vec3 edge   = ray(qx + rr / sx, qy);
        const float     radius = std::acos(std::clamp(glm::dot(dir, edge), -1.0f, 1.0f)) * t;
        if (radius < 0.006f || radius > 0.05f) continue;
        auto& L = m_glass_learn[gs.sig];
        if (L.n > 0 && glm::length(off - L.off) > 0.08f) continue;   // not this gun's glass (a jump): ignored
        const float a = L.n < 20 ? 1.0f / float(L.n + 1) : 0.05f;
        L.off += (off - L.off) * a;
        L.radius += (radius - L.radius) * a;
        L.n++;
        m_glass_samples++;
        if (L.n == 20 || L.n % 900 == 0) {
            m_persist[fmt::format("scopeglassat {}", gs.sig)] = fmt::format("scopeglassat {:.4f} {:.4f} {:.4f} {:.4f} {}", L.off.x, L.off.y, L.off.z, L.radius, gs.sig);
            save_settings();
            spdlog::info("[Scope] steady: weapon {} glass learned on the gun at ({:.4f} {:.4f} {:.4f}) m, radius {:.1f} mm ({} samples; saved)", gs.sig, L.off.x, L.off.y,
                         L.off.z, L.radius * 1000.0f, L.n);
        }
    }
}

// scopemodel: the scope picture on the gun's eyepiece in this eye's image. True = the scope is on the model (the lens in
// front of the eye is not drawn), whether or not anything was drawn this frame.
bool FarCry5Stereo::scope_draw_model(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img)
{
    if (!scope_on_model()) return false;
    const int fr = m_frame.load();
    if (!m_scope.load() || !m_scope_lens.load() || !eye_img) return true;
    if (!m_scope_st.active || m_scope_st.frame != fr || !m_scope_st.mvalid || m_scope_st.mfade <= 0.0f || m_hist[0].cur_frame != fr || m_menu_screen_on) return true;
    scope_draw_model_at(ctx, eye_img, m_scope_st, m_hist[0].cur_vrp, m_hist[0].cur_cam, m_scope_st.eye_rot, fr, fr & 1, false);
    return true;
}

// AFW: the held (rebuilt) eye gets the scope picture too. Without it the dominant eye showed the picture only in the frames
// it was drawn in, the gun's own black scope in between (Homard 18 Oct 22:59: "in AFW they are still a bit doubled").
// The scope state of that eye's last drawn frame, moved with the gun since (controller then -> now); its camera is the one
// the eye is rebuilt for; the picture zooms the fresh eye's image of this frame.
void FarCry5Stereo::scope_draw_held(ID3D11DeviceContext* ctx, ID3D11Texture2D* img, int held, const glm::mat3& rot, const glm::vec3& pos, const glm::vec4& tan)
{
    if (!scope_on_model() || !m_scope.load() || !m_scope_lens.load() || !img || !m_scope_held.load() || m_menu_screen_on) return;
    const int fr = m_frame.load();
    ScopeState st = m_scope_dom_st;
    if (!st.active || !st.dominant || (st.frame & 1) != held || fr - st.frame > 3 || !st.mvalid || st.mfade <= 0.0f || !m_mo.ctrl_valid) return;
    // The gun-fixed geometry carried with the controller since that frame.
    const glm::mat3 dr = m_mo.ctrl_rot_world * glm::transpose(st.crot);
    st.mlens   = m_mo.ctrl_pos_world + dr * (st.mlens - st.cpos);
    st.maxis   = glm::normalize(dr * st.maxis);
    st.mright  = glm::normalize(dr * st.mright);
    st.mup     = glm::normalize(dr * st.mup);
    st.eye_pos = pos;
    st.eye_rot = rot;
    // The held eye's rotation-projection (world offset -> clip; reversed Z, any depth: only x, y, w are used).
    const float l = tan.x, r = tan.y, u = tan.z, dn = tan.w;
    glm::mat4   P{ 0.0f };
    P[0][0] = 2.0f / (r - l); P[2][0] = (r + l) / (r - l);
    P[1][1] = 2.0f / (u - dn); P[2][1] = (u + dn) / (u - dn);
    P[3][2] = 0.05f; P[2][3] = -1.0f;
    const glm::mat4 vrp = P * glm::mat4{ rot };
    if (scope_draw_model_at(ctx, img, st, vrp, pos, rot, fr, held, true)) m_scope_held_draws++;
}

bool FarCry5Stereo::scope_draw_model_at(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img, const ScopeState& st, const glm::mat4& vrp, const glm::vec3& eye,
                                        const glm::mat3& eye_rot, int fr, int eye_idx, bool held)
{
    D3D11_TEXTURE2D_DESC d{};
    eye_img->GetDesc(&d);
    const float       W = float(d.Width), H = float(d.Height);
    // The eyepiece must face the eye (the eye behind it).
    const glm::vec3 to_eye = eye - st.mlens;
    if (glm::dot(to_eye, -st.maxis) <= 0.005f) return true;
    // scoperecoiloff (Homard 2026-10-13: the picture glitches during the recoil animation): no magnified picture while
    // firing and 0.6 s after; the gun's own scope shows then.
    if (m_scope_recoil_off.load() && (m_trigger_was || std::chrono::steady_clock::now() - m_scope_fire_t < std::chrono::milliseconds(600))) return true;

    // The scope camera: the pass picture's (once the pass has run a few pairs in a row and the magnification has ramped
    // up), else this frame's (the zoom of this eye's own image).
    ScopeCam now{};
    scope_camera(st, fr, now);
    const ScopeCam& ic  = m_scope_img_cam;
    const int       age = fr - ic.frame;
    const bool use_pic = m_scope_pass.load() && !m_scope_pass_broken && fc5::scope::picture() && ic.valid && age >= 0 && age <= 2 * std::clamp(m_scope_every.load(), 1, 4) &&
                         m_scope_run >= 4 && scope_pic_mag_ok(st.mag, st.full_mag, ic.mag) && ic.tx > 0.0f && ic.ty > 0.0f;
    const ScopeCam& sc = use_pic ? ic : now;

    // Eye ndc (x, y, 1) -> world direction from the eye: the x, y, z rows of inverse(vrp) at a depth inside the view.
    const glm::mat4 inv = glm::inverse(vrp);
    glm::mat3       A{ glm::vec3(inv[0]), glm::vec3(inv[1]), 0.5f * glm::vec3(inv[2]) + glm::vec3(inv[3]) };
    const glm::vec3 eye_fwd = -glm::transpose(eye_rot)[2];
    if (glm::dot(A * glm::vec3{ 0.0f, 0.0f, 1.0f }, eye_fwd) < 0.0f) A = -A;   // w < 0 at that depth: the other sign
    const glm::mat3 flip{ glm::vec3{ 1, 0, 0 }, glm::vec3{ 0, 1, 0 }, glm::vec3{ 0, 0, -1 } };
    const glm::mat3 M   = flip * sc.rot * A;                     // -> scope camera (x right, y up, z forward)
    const glm::mat3 toW = glm::transpose(sc.rot) * flip;         // scope (x, y, forward) -> world
    fc5::scope::ModelLens l{};
    for (int r = 0; r < 3; ++r) {
        l.m[r][0] = M[0][r]; l.m[r][1] = M[1][r]; l.m[r][2] = M[2][r];
        l.a[r][0] = A[0][r]; l.a[r][1] = A[1][r]; l.a[r][2] = A[2][r];
        const int i = r == 2 ? 3 : r;   // clip x, y, w
        for (int c = 0; c < 3; ++c) l.b[r][c] = vrp[0][i] * toW[c][0] + vrp[1][i] * toW[c][1] + vrp[2][i] * toW[c][2];
    }
    // AFW has no scope pass, so the picture is a zoom. This eye's own image is the gun's scope there once the aim is on the
    // scope's line (sightaim): black (Homard's headset, 18 Oct 21:00). The other eye (6 cm aside) sees past the scope: its
    // last image is zoomed instead, by direction (the picture at infinity: no parallax worth having at the target).
    if (!use_pic && afw_mode() && m_scope_other_eye.load()) {
        // The other eye's image as it was before the lens, the red dot and the aim marker went in (scope_keep_clean).
        const int other = eye_idx ^ 1;
        if (fc5::scope::g_clean[other] && fr - fc5::scope::g_clean_frame[other] <= 3 && fr >= fc5::scope::g_clean_frame[other]) {
            const glm::mat4& ov = fc5::scope::g_clean_vrp[other];
            for (int r = 0; r < 3; ++r) {
                const int i = r == 2 ? 3 : r;
                for (int c = 0; c < 3; ++c) l.b[r][c] = ov[0][i] * toW[c][0] + ov[1][i] * toW[c][1] + ov[2][i] * toW[c][2];
            }
            l.zoom_src = fc5::scope::g_clean[other];
            m_scope_other_draws++;
        }
    }
    // The disc on the gun, relative to this eye, facing back along the scope camera's axis; the picture's right and up
    // are the scope camera's, so the disc's centre shows the aim point.
    const glm::vec3 dc = st.mlens - eye;
    const glm::vec3 dr = st.mright, du = st.mup, dfwd = st.maxis;   // the gun's axes (the picture rolls with it)
    l.disc_c[0] = dc.x; l.disc_c[1] = dc.y; l.disc_c[2] = dc.z; l.disc_c[3] = st.mradius;
    l.disc_n[0] = -dfwd.x; l.disc_n[1] = -dfwd.y; l.disc_n[2] = -dfwd.z; l.disc_n[3] = 0.03f * st.mradius;
    l.m[0][0] = dr.x; l.m[0][1] = dr.y; l.m[0][2] = dr.z;
    l.m[1][0] = du.x; l.m[1][1] = du.y; l.m[1][2] = du.z;
    {
        // Eye box: the eye's offset from the scope's axis (the line through the eyepiece's centre), in the disc's axes; the
        // visible circle moves the other way by one disc radius per scopeeyeboxcm, so the dark side is the side the eye moved to.
        const glm::vec3 e    = eye - st.mlens;
        const glm::vec3 perp = e - dfwd * glm::dot(e, dfwd);
        const float     box  = std::max(0.1f, m_scope_eyebox_cm.load()) / 100.0f;
        l.m[2][0] = -glm::dot(perp, dr) / box;
        l.m[2][1] = -glm::dot(perp, du) / box;
        l.m[2][2] = 1.1f;
        l.m[2][3] = 0.5f;
    }
    // The field (exit pupil) round the axis: the eyepiece's own angular radius when the eye is at the eye relief the
    // scope camera takes (radius over the lens's distance ahead), as the picture is drawn for.
    l.r_t     = st.mfield;
    l.fade    = st.mfade;
    l.inv_mag = 1.0f / std::max(1.0f, st.mag);
    l.k[0]    = 0.5f / std::max(1e-4f, sc.tx);
    l.k[1]    = 0.5f / std::max(1e-4f, sc.ty);
    l.eyebox  = false;   // no eye box on the gun: at a real hold (eye 20+ cm behind the eyepiece, a few cm off its axis) it darkened most of the picture
    l.debug   = m_scope_lens_debug.load();
    // Only the disc's box is dispatched: its centre and edge points projected into this eye.
    auto px = [&](const glm::vec3& off, glm::vec2& out) {
        const glm::vec4 cl = vrp * glm::vec4{ off, 1.0f };
        if (cl.w <= 0.005f) return false;
        out = { (cl.x / cl.w * 0.5f + 0.5f) * W, (0.5f - cl.y / cl.w * 0.5f) * H };
        return true;
    };
    glm::vec3 u = glm::cross(st.maxis, glm::vec3{ 0.0f, 0.0f, 1.0f });
    if (glm::length(u) < 1e-3f) u = glm::cross(st.maxis, glm::vec3{ 1.0f, 0.0f, 0.0f });
    u                  = glm::normalize(u);
    const glm::vec3 v  = glm::normalize(glm::cross(st.maxis, u));
    float           x0 = W, y0 = H, x1 = 0.0f, y1 = 0.0f;
    bool            all = true;
    for (int k = 0; k < 8; ++k) {
        const float a = k * 0.7853982f;
        glm::vec2   q{};
        if (!px(dc + (u * std::cos(a) + v * std::sin(a)) * st.mradius * 1.05f, q)) { all = false; break; }
        x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y);
    }
    // The reticle's half width: about 1.2 eye pixels, in disc radii.
    const float rpx = all ? std::max(4.0f, 0.5f * std::max(x1 - x0, y1 - y0) / 1.05f) : 200.0f;
    l.ret_t = m_scope_reticle.load() ? std::max(1.0f, 1.2f * W / 3072.0f) / rpx : 0.0f;
    if (!all || l.debug) { x0 = 0.0f; y0 = 0.0f; x1 = W; y1 = H; }   // (a disc reaching behind the eye: the whole image)
    l.box[0] = x0 - 4.0f; l.box[1] = y0 - 4.0f; l.box[2] = x1 + 4.0f; l.box[3] = y1 + 4.0f;
    // The glass as the game drew it this frame: the picture goes there (its centre and size from the GPU stats), rolled with
    // the gun as it is seen on screen; the shader falls back to the disc when the glass has too few pixels.
    // The rebuilt AFW frames (held) use the glass as drawn in this eye's last drawn frame (the mask is only written in the scoped
    // eye): the picture where the drawn frames put it. The gun-fixed disc at a learned or default place differed (Homard 19 Oct:
    // still doubled at 12:53, worse at 14:34 with both frames on the learned place).
    const bool glass = m_scope_glass.load() && (held ? (m_scope_glass_frame >= 0 && fr - m_scope_glass_frame <= 2) : m_scope_glass_frame == fr);
    if (glass) {
        ID3D11Device* gdev = nullptr;
        ctx->GetDevice(&gdev);
        if (gdev) {
            fc5::scope::glass_mask(gdev, (UINT)m_scope_glass_wh[0], (UINT)m_scope_glass_wh[1], nullptr, &l.mask);
            gdev->Release();
        }
        l.stats     = fc5::scope::glass_stats(ctx, l.mask != nullptr);
        l.mscale[0] = m_scope_glass_vp[2] / W; l.mscale[1] = m_scope_glass_vp[3] / H;
        l.moff[0]   = m_scope_glass_vp[0];     l.moff[1]   = m_scope_glass_vp[1];
        l.msize[0]  = m_scope_glass_wh[0];     l.msize[1]  = m_scope_glass_wh[1];
        glm::vec2 p0{}, p1{};
        if (px(dc, p0) && px(dc + st.mright * 0.01f, p1) && glm::length(p1 - p0) > 1e-3f) {
            const float ang = std::atan2(-(p1.y - p0.y), p1.x - p0.x);   // the gun's right on screen (y up)
            l.roll[0] = std::cos(ang); l.roll[1] = std::sin(ang);
        }
        const glm::vec3 d0 = glm::normalize(A * glm::vec3{ 0.0f, 0.0f, 1.0f }), d1 = glm::normalize(A * glm::vec3{ 2.0f / W, 0.0f, 1.0f });
        l.tpp    = std::acos(std::clamp(glm::dot(d0, d1), -1.0f, 1.0f));
        l.min_px = 200.0f;
        // For learning the glass's place on the gun (scopesteady): its stats to the CPU, with this frame's geometry.
        if (l.stats && !held) {
            fc5::scope::glass_readback(ctx, fr);
            auto& gs    = m_glass_snap[fr & 7];
            gs.frame    = fr;
            gs.vrp      = vrp;
            gs.eye      = eye;
            gs.lens     = st.mlens;
            gs.axis     = st.maxis;
            gs.ctrl_pos = m_mo.ctrl_pos_world;
            gs.ctrl_rot = m_mo.ctrl_rot_world;
            for (int k = 0; k < 4; ++k) gs.vp[k] = m_scope_glass_vp[k];
            gs.W        = W;
            gs.H        = H;
            gs.sig      = st.cal_found ? st.sig : (m_scope_sticky_sig ? m_scope_sticky_sig : st.sig);
            gs.steady   = !m_trigger_was && std::chrono::steady_clock::now() - m_scope_fire_t > std::chrono::milliseconds(600) && st.mfade > 0.5f;
        }
        // While firing (and 0.6 s after), once the glass's place is learned, the picture is drawn on the gun-fixed disc at
        // that place: steady while the gun model kicks. Otherwise on the glass as drawn (exact at any distance; it keeps
        // the learned place up to date).
        auto       learned = m_glass_learn.find(m_glass_snap[fr & 7].sig);
        const bool firing  = m_trigger_was || std::chrono::steady_clock::now() - m_scope_fire_t < std::chrono::milliseconds(600);
        if (m_scope_steady.load() && firing && learned != m_glass_learn.end() && learned->second.n >= 20) {
            l.mask  = nullptr;
            l.stats = nullptr;
        } else {
            l.box[0] = 0.0f; l.box[1] = 0.0f; l.box[2] = W; l.box[3] = H;   // where the glass is is known on the GPU only
        }
    }
    if (!held) scope_glass_learn(ctx);
    l.scope_srv = use_pic ? fc5::scope::picture() : nullptr;
    if (fc5::scope::draw_model(ctx, eye_img, l)) {
        m_scope_draws++;
        m_scope_model_draws++;
        if (use_pic) m_scope_pass_draws++;
        if (!held) m_scope_drawn_frame = fr;
        static int logged = 0;
        if (logged < 3 || m_scope_log.load() > 0) {
            logged++;
            spdlog::info("[Scope] on the model, frame {}: eyepiece {:.1f} cm ahead of the eye, {:.1f} cm off the scope's axis; box ({:.0f},{:.0f})-({:.0f},{:.0f}) of {}x{}, x{:.2f}, {}", fr,
                         glm::dot(dc, st.maxis) * 100.0f, glm::length(to_eye - st.maxis * glm::dot(to_eye, st.maxis)) * 100.0f, l.box[0], l.box[1], l.box[2], l.box[3], d.Width,
                         d.Height, st.mag, use_pic ? "pass picture" : "zoom of the eye image");
        }
    } else {
        m_scope_fails++;
    }
    return true;
}

// Scope commands (own chain, outside the long one). True when cmd was a scope command.
bool FarCry5Stereo::scope_command(const std::string& cmd, const std::string& arg, std::istringstream& ls, bool from_settings, bool& persist_changed)
{
    if (cmd.rfind("scope", 0) != 0) return false;
    const bool  on = arg == "on" || arg == "1" || arg == "true";
    const float fv = arg.empty() ? 0.0f : std::strtof(arg.c_str(), nullptr);
    auto persist = [&](const std::string& key, const std::string& line) {
        m_persist[key] = line;
        if (!from_settings) persist_changed = true;
    };
    if (cmd == "scope") { m_scope = on; persist(cmd, on ? "scope on" : "scope off"); if (!on) m_scope_st.active = false; }
    else if (cmd == "scopelens") { m_scope_lens = on; persist(cmd, on ? "scopelens on" : "scopelens off"); }
    else if (cmd == "scopereticle") { m_scope_reticle = on; persist(cmd, on ? "scopereticle on" : "scopereticle off"); }
    else if (cmd == "scopeeyebox") { m_scope_eyebox = on; persist(cmd, on ? "scopeeyebox on" : "scopeeyebox off"); }
    else if (cmd == "scopehidegun") { m_scope_hide_gun = on; persist(cmd, on ? "scopehidegun on" : "scopehidegun off"); }
    else if (cmd == "scopemodel") {
        m_scope_model = on;
        persist(cmd, on ? "scopemodel on" : "scopemodel off");
        if (on && m_scope_src_was) {
            // scopesrc 1 was drawing the scope view in the eye: the same flush as scopesrc 1 -> 0.
            m_dlss_reset_eyes = 3;
            m_scope_hold      = 30;
        }
        m_scope_src_was = false;
        m_scope_run     = 0;
    }
    else if (cmd == "scopelensfwd") {
        // scopelensfwd CM: the picture this far in front of the gun's eyepiece (default 0: on its face).
        m_scope_lens_fwd = std::clamp(fv, 0.0f, 20.0f) / 100.0f;
        persist(cmd, fmt::format("scopelensfwd {:.1f}", m_scope_lens_fwd.load() * 100.0f));
    }
    else if (cmd == "scopecut") {
        // scopecut DEG: the picture fades out from 60 % of this angle between the gun's axis and the eye's line to it (default 20).
        m_scope_cut_deg = std::clamp(fv, 2.0f, 45.0f);
        persist(cmd, fmt::format("scopecut {:.1f}", m_scope_cut_deg.load()));
    }    else if (cmd == "scopeglass") { m_scope_glass = on; persist(cmd, on ? "scopeglass on" : "scopeglass off"); }   // the picture on the glass as drawn (off = the gun-fixed disc)
    else if (cmd == "scoperecoiloff") { m_scope_recoil_off = on; persist(cmd, on ? "scoperecoiloff on" : "scoperecoiloff off"); }
    else if (cmd == "scopezoomstick") { m_scope_zoom_stick = on; persist(cmd, on ? "scopezoomstick on" : "scopezoomstick off"); }
    else if (cmd == "scopezoom") {
        // scopezoom K [weapon]: the gun's zoom factor on its optic (saved by scopezoomstick); "scopezoom reset [weapon]" back to 1.
        UINT sig = 0;
        ls >> sig;
        if (sig == 0) sig = m_weapon_sig.load();
        const float k = arg == "reset" ? 1.0f : std::clamp(fv, 0.05f, 40.0f);
        if (sig != 0 && k > 0.0f) {
            {
                std::lock_guard _{ m_scope_mtx };
                if (k == 1.0f) m_scope_zoom.erase(sig); else m_scope_zoom[sig] = k;
            }
            if (k == 1.0f) {
                m_persist.erase(fmt::format("scopezoom {}", sig));
                if (!from_settings) persist_changed = true;
            } else {
                persist(fmt::format("scopezoom {}", sig), fmt::format("scopezoom {:.3f} {}", k, sig));
            }
            if (!from_settings) spdlog::info("[Scope] weapon {}: zoom x{:.2f} on its optic", sig, k);
        }
    }
    else if (cmd == "scopesteady") { m_scope_steady = on; persist(cmd, on ? "scopesteady on" : "scopesteady off"); }   // the picture at the glass's learned place (steady when firing)
    else if (cmd == "scopeglassat") {
        // scopeglassat X Y Z R SIG (saved, written by the mod): the glass's learned place on the gun (controller axes) and radius.
        float y = 0.0f, z = 0.0f, rad = 0.0f;
        UINT  sig = 0;
        ls >> y >> z >> rad >> sig;
        if (arg == "clear") { m_glass_learn.clear(); for (auto it = m_persist.begin(); it != m_persist.end();) it = it->first.rfind("scopeglassat ", 0) == 0 ? m_persist.erase(it) : std::next(it); if (!from_settings) persist_changed = true; }
        else if (sig != 0) {
            auto& L = m_glass_learn[sig];
            L.off = { fv, y, z }; L.radius = rad; L.n = std::max(L.n, 20);
            persist(fmt::format("scopeglassat {}", sig), fmt::format("scopeglassat {:.4f} {:.4f} {:.4f} {:.4f} {}", fv, y, z, rad, sig));
        }
    }
    else if (cmd == "sightspread") { m_sight_spread = std::clamp(fv, 0.0f, 1.0f); persist(cmd, fmt::format("sightspread {:.2f}", m_sight_spread.load())); }
    else if (cmd == "scopelensdebug") m_scope_lens_debug = on;   // test: the eyepiece disc magenta, the field's edge green
    else if (cmd == "scopelenshere") {
        // scopelenshere [cm]: with the eye looking through the scope, the gun in hand gets its eyepiece on the eye's line of
        // sight to the aim point, cm (default 6.6) ahead of the eye (saved in its scopecal entry, in controller axes).
        if (!arg.empty() && fv > 1.0f) m_scope_relief = std::clamp(fv, 2.0f, 20.0f) / 100.0f;
        const ScopeState& ds  = m_scope_dom_st;
        const UINT        sig = m_weapon_sig.load();
        if (sig == 0 || !m_mo.ctrl_valid || ds.frame < m_frame.load() - 4) {
            spdlog::info("[Scope] scopelenshere: no gun or no recent look-through state");
        } else {
            const float     hit  = fc5::aim::last_hit();
            const float     dist = hit > 2.0f ? hit : m_mo.converge > 2.0f ? m_mo.converge : std::max(2.0f, m_converge_m.load());
            const glm::vec3 aim  = m_mo.aim_origin + m_mo.barrel_dir * dist;
            const glm::vec3 u    = glm::normalize(aim - ds.eye_pos);
            const glm::vec3 lens = ds.eye_pos + u * m_scope_relief.load();
            const glm::vec3 off  = glm::transpose(m_mo.ctrl_rot_world) * (lens - m_mo.ctrl_pos_world);
            ScopeCal c = m_scope_default;
            {
                std::lock_guard _{ m_scope_mtx };
                if (auto it = m_scope_cal.find(sig); it != m_scope_cal.end()) c = it->second;
                c.off[0] = off.x; c.off[1] = off.y; c.off[2] = off.z;
                m_scope_cal[sig] = c;
            }
            persist(fmt::format("scopecal {}", sig), fmt::format("scopecal {:.4f} {:.4f} {:.4f} {:.4f} {:.2f} {}", c.off[0], c.off[1], c.off[2], c.radius, c.mag, sig));
            spdlog::info("[Scope] scopelenshere: weapon {} eyepiece at ({:.4f} {:.4f} {:.4f}) m in controller axes, {:.1f} cm ahead of the eye", sig, off.x, off.y, off.z,
                         m_scope_relief.load() * 100.0f);
        }
    }
    else if (cmd == "scopeauto") { m_scope_auto = on; persist(cmd, on ? "scopeauto on" : "scopeauto off"); }
    else if (cmd == "scopesticky") { m_scope_sticky_eye = on; persist(cmd, on ? "scopesticky on" : "scopesticky off"); }
    else if (cmd == "scopelearn") {
        m_scope_learn = on;
        persist(cmd, on ? "scopelearn on" : "scopelearn off");
        if (!on) m_scope_probe_lt = false;
        m_scope_probed.clear();
    }
    else if (cmd == "scopepass") {
        m_scope_pass = on;
        persist(cmd, on ? "scopepass on" : "scopepass off");
        if (on) m_scope_pass_broken = false;   // a new try after the pass turned itself off
        m_scope_run = 0;
    }
    else if (cmd == "scoperes") {
        // scoperes N: the scope picture N x N render pixels in a corner (0 = the whole render size).
        const int n = arg.empty() || arg == "off" ? 0 : std::clamp(int(fv), 0, 4096);
        m_scope_res = n < 128 ? 0 : n;
        persist(cmd, fmt::format("scoperes {}", m_scope_res.load()));
        m_scope_run = 0;
    }
    else if (cmd == "scopeskip") {
        // scopeskip MASK: 1 sun shadows, 2 reflections, 4 ambient occlusion, 8 volumetric fog.
        m_scope_skip = arg.empty() || arg == "off" ? 0 : std::clamp(int(std::strtol(arg.c_str(), nullptr, 0)), 0, 63) & 15;
        persist(cmd, fmt::format("scopeskip {}", m_scope_skip.load()));
    }
    else if (cmd == "scopesrc") {
        // scopesrc 0|1: 1 = the dominant eye draws the scope view itself (no third call; black around the lens).
        m_scope_src = (on || arg == "1") ? 1 : 0;
        persist(cmd, fmt::format("scopesrc {}", m_scope_src.load()));
        if (m_scope_src.load() == 0 && m_scope_src_was) {
            m_dlss_reset_eyes = 3;
            // The eye's own history still holds the scope view it drew. With the scope call right after that eye in every
            // pair it never flushed: the whole eye stayed tinted (purple-brown night sky) until the scope left the eye
            // (tester 3, simulator 2026-10-12). A short run of pairs without the call lets it flush first (300 ms was enough).
            m_scope_hold = 30;
        }
        m_scope_src_was = false;
        m_scope_run     = 0;
    }
    else if (cmd == "scopeevery") {
        m_scope_every = std::clamp(arg.empty() ? 1 : int(fv), 1, 4);
        persist(cmd, fmt::format("scopeevery {}", m_scope_every.load()));
    }
    else if (cmd == "scopeheld") { m_scope_held = on; persist(cmd, on ? "scopeheld on" : "scopeheld off"); }
    else if (cmd == "scopeothereye") { m_scope_other_eye = on; persist(cmd, on ? "scopeothereye on" : "scopeothereye off"); }
    else if (cmd == "scopeeye") {
        m_scope_eye = arg == "left" ? 0 : arg == "auto" ? 2 : 1;
        persist(cmd, fmt::format("scopeeye {}", arg == "left" || arg == "auto" ? arg : "right"));
    }
    else if (cmd == "scopeany") m_scope_any = on;
    else if (cmd == "scopeforce") m_scope_force = on;
    else if (cmd == "scopelog") m_scope_log = arg.empty() ? 10 : (int)fv;
    else if (cmd == "scopehistrange") {
        // scopehistrange LO HI (research): only history textures LO..HI-1 start the scope call from the scope's own copy;
        // "scopehistrange" alone = all of them (default). scopehistlist logs the list.
        int lo = 0, hi = 1 << 30;
        std::istringstream rs{ arg };
        if (!(rs >> lo >> hi)) { lo = 0; hi = 1 << 30; }
        fc5::scope::hist_range(lo, hi);
    }
    else if (cmd == "scopehistlist") fc5::scope::hist_list();
    else if (cmd == "scopehistswap") { m_scope_hist_swap = on; persist(cmd, on ? "scopehistswap on" : "scopehistswap off"); }
    else if (cmd == "scopeeyeboxcm") m_scope_eyebox_cm = std::clamp(fv, 0.1f, 10.0f);
    else if (cmd == "scopedist") { float off = 0.0f; ls >> off; m_scope_on_cm = std::clamp(fv, 1.0f, 100.0f); m_scope_off_cm = std::max(m_scope_on_cm.load(), off); }
    else if (cmd == "scopeangle") { float off = 0.0f; ls >> off; m_scope_on_deg = std::clamp(fv, 0.5f, 90.0f); m_scope_off_deg = std::max(m_scope_on_deg.load(), off); }
    else if (cmd == "scopecal" || cmd == "scopedefault") {
        // scopecal x y z radius mag [weapon]: the ocular lens centre from the controller (m: right, up, back), its
        // radius (m) and the magnification, for this weapon (persisted); "scopecal clear [weapon]" removes it.
        // scopedefault x y z radius mag: the entry weapons without one use while scopeany is on (test).
        ScopeCal c{};
        UINT     sig = 0;
        if (arg == "clear") {
            ls >> sig;
            if (sig == 0) sig = m_weapon_sig.load();
            { std::lock_guard _{ m_scope_mtx }; m_scope_cal.erase(sig); }
            m_persist.erase(fmt::format("scopecal {}", sig));
            if (!from_settings) persist_changed = true;
            spdlog::info("[Scope] weapon {}: scope entry removed", sig);
            return true;
        }
        c.off[0] = fv;
        ls >> c.off[1] >> c.off[2] >> c.radius >> c.mag >> sig;
        c.radius = std::clamp(c.radius, 0.003f, 0.1f);
        c.mag    = std::clamp(c.mag, 1.0f, 40.0f);
        if (cmd == "scopedefault") {
            std::lock_guard _{ m_scope_mtx };
            m_scope_default = c;
        } else {
            if (sig == 0) sig = m_weapon_sig.load();
            if (sig == 0) { spdlog::info("[Scope] scopecal: no weapon in hand"); return true; }
            { std::lock_guard _{ m_scope_mtx }; m_scope_cal[sig] = c; }
            persist(fmt::format("scopecal {}", sig), fmt::format("scopecal {:.4f} {:.4f} {:.4f} {:.4f} {:.2f} {}", c.off[0], c.off[1], c.off[2], c.radius, c.mag, sig));
        }
        spdlog::info("[Scope] {}: lens ({:.3f}, {:.3f}, {:.3f}) m, radius {:.1f} mm, x{:.1f}{}", cmd == "scopedefault" ? "default entry" : fmt::format("weapon {}", sig), c.off[0], c.off[1],
                     c.off[2], c.radius * 1000.0f, c.mag, from_settings ? " (settings)" : "");
        return true;
    }
    else if (cmd == "scopestatus") {
        const auto& st = m_scope_st;
        spdlog::info("[Scope] status: scope {} lens {} reticle {} eyebox {} hidegun {} eye {}; weapon {} ({}); last frame {} active {} dist {:.1f} cm angle {:.1f} deg mag {:.2f}; "
                     "frames {} lens draws {} failed {} gun draws left out {}",
                     m_scope.load(), m_scope_lens.load(), m_scope_reticle.load(), m_scope_eyebox.load(), m_scope_hide_gun.load(), m_scope_eye.load(), m_weapon_sig.load(),
                     st.cal_found ? "calibrated" : "no entry", st.frame, st.active, st.dist * 100.0f, st.angle, st.mag, m_scope_frames, m_scope_draws, m_scope_fails, m_scope_gun_skips);
        spdlog::info("[Scope] pass {}{}: scope calls {}, pictures {}, lens drawn from the pass {}, calls in a row {}, history textures {}, last picture after frame {}", m_scope_pass.load(),
                     m_scope_pass_broken ? " (turned itself off)" : "", m_scope_calls, m_scope_captures, m_scope_pass_draws, m_scope_run, fc5::scope::hist_count(), m_scope_img_cam.frame);
        spdlog::info("[Scope] on the model {} (glass as drawn {}: glow draws into the mask {}, glow draws left out {}, last mask frame {}): pictures on the eyepiece {}; picture of the last scoped eye ({:.3f} {:.3f} {:.3f}) radius {:.1f} cm, valid {}, gun {:.1f} deg off the eye's line (shown {:.0f} %, scopecut {:.1f}), scopelensfwd {:.1f} cm",
                     scope_on_model(), m_scope_glass.load(), m_scope_glass_draws, m_scope_glow_hidden, m_scope_glass_frame, m_scope_model_draws, m_scope_dom_st.mlens.x, m_scope_dom_st.mlens.y, m_scope_dom_st.mlens.z, m_scope_dom_st.mradius * 100.0f, m_scope_dom_st.mvalid, m_scope_dom_st.mangle, m_scope_dom_st.mfade * 100.0f, m_scope_cut_deg.load(), m_scope_lens_fwd.load() * 100.0f);
        spdlog::info("[Scope] pass cost cuts: scoperes {} scopeskip {} scopeevery {}; draws left out {}, dispatches left out {}, draws scissored {}", m_scope_res.load(),
                     m_scope_skip.load(), m_scope_every.load(), m_scope_skipped_draws, m_scope_skipped_cs, m_scope_scissored);
        spdlog::info("[Scope] AFW picture from the other eye's image (scopeothereye {}): {} pictures; held (rebuilt) eye pictures (scopeheld {}): {}", m_scope_other_eye.load(), m_scope_other_draws, m_scope_held.load(), m_scope_held_draws);
    }
    else {
        spdlog::info("[FC5 cmd] unknown scope command '{}'", cmd);
        return true;
    }
    spdlog::info("[FC5 cmd] {} {}", cmd, arg);
    return true;
}

// AFW (scopeothereye): each eye's image kept before the lens, the red dot and the aim marker are drawn into it, while a scope
// is in use; the scoped eye's picture zooms the other eye's (scope_draw_model).
namespace fc5::scope
{
    ID3D11Texture2D* g_clean[2]{};
    int              g_clean_frame[2]{ -10, -10 };
    glm::mat4        g_clean_vrp[2]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };
}

void FarCry5Stereo::scope_keep_clean(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye_img)
{
    using namespace fc5::scope;
    if (!eye_img || !m_scope.load() || !m_scope_other_eye.load() || !scope_on_model() || !afw_mode()) return;
    const int fr = m_frame.load();
    if (m_hist[0].cur_frame != fr) return;
    if (!(m_scope_dom_st.frame >= fr - 4 && m_scope_dom_st.full_mag > 1.01f && m_scope_dom_st.mfade > 0.0f)) return;   // no scope in use
    const int            e = fr & 1;
    D3D11_TEXTURE2D_DESC d{};
    eye_img->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return;
    if (g_clean[e]) {
        D3D11_TEXTURE2D_DESC c{};
        g_clean[e]->GetDesc(&c);
        if (c.Width != d.Width || c.Height != d.Height || c.Format != d.Format) { g_clean[e]->Release(); g_clean[e] = nullptr; }
    }
    if (!g_clean[e]) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC nd = d;
        nd.MipLevels = 1; nd.ArraySize = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&nd, nullptr, &g_clean[e]))) g_clean[e] = nullptr;
        dev->Release();
        spdlog::info("[Scope] AFW: eye {} image kept for the other eye's scope picture {}x{} format {}: {}", e, d.Width, d.Height, (int)d.Format, g_clean[e] ? "ready" : "failed");
        if (!g_clean[e]) return;
    }
    ctx->CopyResource(g_clean[e], eye_img);
    g_clean_frame[e] = fr;
    g_clean_vrp[e]   = m_hist[0].cur_vrp;
}