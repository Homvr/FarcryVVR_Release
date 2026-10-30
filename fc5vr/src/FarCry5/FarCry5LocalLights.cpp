#include "FarCry5LocalLights.h"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <atomic>
#include <cstring>

namespace fc5::locallights
{
    namespace
    {
        // The game's layouts (disassembly of the tiled lighting #02297990 and its variants):
        // LightingData (144 B): posCS.xyz rcpRadius2 | color attenuation | direction flags | transform | spotFactors unused
        //   iesProfileIndex | clipPlanes[4] (n.xyz, w: inside where dot(n, p) - w > 0). flags: 1 spot (projective shadow),
        //   4 or 8 shadowed, 16 clip planes, >> 9 the ShadowData index.
        // ShadowProjTextureData (96 B): params0..3 the projection's columns for x, y, z, 1 of a camera-relative position
        //   (spot), or params0.xyz the light's camera-relative position (point, cube shadow); params4, projectorScaleOffset.
        const char* kHlsl = R"(
struct L144 { float4 a[9]; };
struct S96  { float4 p[6]; };
StructuredBuffer<L144>   InL : register(t0);
StructuredBuffer<S96>    InS : register(t1);
RWStructuredBuffer<L144> OutL : register(u0);
RWStructuredBuffer<S96>  OutS : register(u1);
cbuffer P : register(b0) { float3 D; uint CountL; uint CountS; uint HasS; uint2 Pad; };
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (i >= CountL) return;
    L144 l = InL[i];
    const uint flags = asuint(l.a[2].w);
    // The pixel this eye places at p_eye is the game's p_eye + D: every camera-relative value goes the other way.
    l.a[0].xyz -= D;
    [unroll] for (uint k = 5; k < 9; ++k) l.a[k].w -= dot(l.a[k].xyz, D);
    OutL[i] = l;
    if (HasS != 0 && (flags & 12u) != 0u) {
        const uint si = flags >> 9;
        if (si < CountS) {
            S96 s = InS[si];
            if ((flags & 1u) != 0u) s.p[3] += D.x * s.p[0] + D.y * s.p[1] + D.z * s.p[2];
            else s.p[0].xyz -= D;
            OutS[si] = s;
        }
    }
}
)";

        struct Copy
        {
            ID3D11Buffer*              buf{ nullptr };
            ID3D11ShaderResourceView*  srv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
            UINT                       bytes{ 0 }, stride{ 0 };
        };
        ID3D11ComputeShader* g_cs = nullptr;
        ID3D11Buffer*        g_cb = nullptr;
        bool                 g_failed = false;
        Copy                 g_l, g_s;
        ID3D11Resource*      g_src_l = nullptr;
        ID3D11Resource*      g_src_s = nullptr;
        int64_t              g_key = -1;
        std::atomic<bool>    g_on{ true };
        std::atomic<uint64_t> g_swaps{ 0 }, g_builds{ 0 }, g_refused{ 0 };

        bool make_copy(ID3D11Device* dev, Copy& c, UINT bytes, UINT stride)
        {
            if (c.buf && c.bytes == bytes && c.stride == stride) return true;
            if (c.srv) { c.srv->Release(); c.srv = nullptr; }
            if (c.uav) { c.uav->Release(); c.uav = nullptr; }
            if (c.buf) { c.buf->Release(); c.buf = nullptr; }
            D3D11_BUFFER_DESC d{ bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride };
            if (FAILED(dev->CreateBuffer(&d, nullptr, &c.buf)) || !c.buf) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = bytes / stride;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = DXGI_FORMAT_UNKNOWN; uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = bytes / stride;
            dev->CreateShaderResourceView(c.buf, &sv, &c.srv);
            dev->CreateUnorderedAccessView(c.buf, &uv, &c.uav);
            c.bytes = bytes; c.stride = stride;
            return c.srv && c.uav;
        }

        bool setup(ID3D11Device* dev)
        {
            if (g_cs) return true;
            if (g_failed) return false;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            auto      fn   = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!fn || FAILED(fn(kHlsl, strlen(kHlsl), "fc5_lampeye", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[LampEye] shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no compiler");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
            code->Release();
            D3D11_BUFFER_DESC cd{ 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
            dev->CreateBuffer(&cd, nullptr, &g_cb);
            if (!g_cs || !g_cb) { g_failed = true; return false; }
            spdlog::info("[LampEye] ready");
            return true;
        }

        // The structured buffer behind an SRV and its layout (0 when it is not a structured buffer of that stride).
        ID3D11Buffer* source(ID3D11ShaderResourceView* v, UINT stride, UINT& bytes)
        {
            bytes = 0;
            if (!v) return nullptr;
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            if (!r) return nullptr;
            D3D11_RESOURCE_DIMENSION dim{};
            r->GetType(&dim);
            if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) { r->Release(); return nullptr; }
            auto*             b = static_cast<ID3D11Buffer*>(r);
            D3D11_BUFFER_DESC d{};
            b->GetDesc(&d);
            if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || d.StructureByteStride != stride) { r->Release(); return nullptr; }
            bytes = d.ByteWidth;
            return b;   // a reference: the caller releases it
        }
    }

    Saved swap(ID3D11DeviceContext* ctx, const Raw& raw, UINT slot_l, UINT slot_s, const float delta[3], int64_t key)
    {
        Saved s{};
        if (!ctx || slot_l > 127) return s;
        ID3D11ShaderResourceView* vl = nullptr;
        ID3D11ShaderResourceView* vs = nullptr;
        ctx->CSGetShaderResources(slot_l, 1, &vl);
        if (slot_s <= 127) ctx->CSGetShaderResources(slot_s, 1, &vs);
        UINT          bl = 0, bs = 0;
        ID3D11Buffer* rl = source(vl, 144, bl);
        ID3D11Buffer* rs = source(vs, 96, bs);
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        bool ok = rl && dev && setup(dev) && make_copy(dev, g_l, bl, 144) && (!rs || make_copy(dev, g_s, bs, 96));
        if (dev) dev->Release();
        if (!ok) {
            g_refused++;
            if (rl) rl->Release();
            if (rs) rs->Release();
            if (vl) vl->Release();
            if (vs) vs->Release();
            return s;
        }
        if (g_key != key || g_src_l != rl || g_src_s != rs) {
            // This eye call's copies: the shadow buffer whole first (entries no lamp points at stay as they are), then the moved values.
            g_key   = key;
            g_src_l = rl;
            g_src_s = rs;
            g_builds++;
            if (rs) ctx->CopyResource(g_s.buf, rs);
            ID3D11ComputeShader*       old_cs = nullptr;
            ID3D11ShaderResourceView*  old_t[2]{};
            ID3D11UnorderedAccessView* old_u[2]{};
            ID3D11Buffer*              old_b = nullptr;
            ctx->CSGetShader(&old_cs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 2, old_t);
            ctx->CSGetUnorderedAccessViews(0, 2, old_u);
            ctx->CSGetConstantBuffers(0, 1, &old_b);
            struct { float d[3]; UINT nl; UINT ns; UINT has; UINT pad[2]; } p{ { delta[0], delta[1], delta[2] }, bl / 144, rs ? bs / 96 : 0u, rs ? 1u : 0u, { 0, 0 } };
            raw.update(ctx, g_cb, &p);
            ID3D11ShaderResourceView*  t[2]{ vl, vs };
            ID3D11UnorderedAccessView* u[2]{ g_l.uav, rs ? g_s.uav : nullptr };
            raw.set_cs(ctx, g_cs);
            ctx->CSSetShaderResources(0, 2, t);
            ctx->CSSetUnorderedAccessViews(0, 2, u, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &g_cb);
            raw.dispatch(ctx, (bl / 144 + 63) / 64, 1, 1);
            ID3D11UnorderedAccessView* nu[2]{};
            ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
            raw.set_cs(ctx, old_cs);
            ctx->CSSetShaderResources(0, 2, old_t);
            ctx->CSSetUnorderedAccessViews(0, 2, old_u, nullptr);
            ctx->CSSetConstantBuffers(0, 1, &old_b);
            if (old_cs) old_cs->Release();
            for (auto* v : old_t) if (v) v->Release();
            for (auto* v : old_u) if (v) v->Release();
            if (old_b) old_b->Release();
        }
        rl->Release();
        if (rs) rs->Release();
        // The dispatch reads the copies (the game's views are put back after it).
        s.on = true; s.slot_l = slot_l; s.slot_s = rs ? slot_s : 255; s.old_l = vl; s.old_s = rs ? vs : nullptr;
        if (!rs && vs) vs->Release();
        ctx->CSSetShaderResources(slot_l, 1, &g_l.srv);
        if (rs) ctx->CSSetShaderResources(slot_s, 1, &g_s.srv);
        g_swaps++;
        return s;
    }

    void restore(ID3D11DeviceContext* ctx, Saved& s)
    {
        if (!s.on) return;
        ctx->CSSetShaderResources(s.slot_l, 1, &s.old_l);
        if (s.slot_s <= 127) ctx->CSSetShaderResources(s.slot_s, 1, &s.old_s);
        if (s.old_l) s.old_l->Release();
        if (s.old_s) s.old_s->Release();
        s = Saved{};
    }

    void stats(uint64_t& swaps, uint64_t& builds, uint64_t& refused)
    {
        swaps   = g_swaps.exchange(0);
        builds  = g_builds.exchange(0);
        refused = g_refused.exchange(0);
    }

    bool enabled() { return g_on.load(); }

    bool command(const std::string& cmd, float, bool on)
    {
        if (cmd != "lampeye") return false;
        g_on = on;
        spdlog::info("[LampEye] {}", on ? "on" : "off");
        return true;
    }
}
