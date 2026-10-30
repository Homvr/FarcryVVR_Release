// gesturezones on|off (debug, not saved; F11 Gestures "Debug: show the gesture zones"; Homard 24 Oct: "make interaction zone
// visible (with a colored transparent square or something)"): each gesture zone (FarCry5Gesture.cpp) drawn into both eyes'
// pictures as the see-through ball it is (a grip press counts when the hand's point is within the zone size of its centre),
// with a brighter rim at its outline, and a small white ball on each controller's tested point.
//  - Shaded as balls (Homard 24 Oct 12:16: "make them sphere rather than plane"): the near and far surfaces (the far one
//    fainter, seen through the ball) lit from above, with latitude / longitude lines every 30 deg on the zones.
//  - Colours: blue the shoulder (switch weapon), orange the sling, green the throwable. Brighter while that hand is inside,
//    white rim while the gesture holds (the sling: the weapon in hand; the throwable: in the hand). Half faded when it cannot
//    act now (no weapon in hand for the sling, a vehicle, a cinematic), faint grey when switched off.
//  - Each eye pixel's ray (the eye's XR pose and field of view, as hudmarkers) is met with each ball in tracking space, so
//    the balls sit where the gesture code looks for the hands, for this frame's head pose. Always on top of the scene (no
//    depth test): the zones are within reach of the body.
#include "FarCry5Stereo.h"
#include "FarCry5Gesture.h"
#include "FarCry5Record.h"

#include <Framework.hpp>

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <glm/gtc/quaternion.hpp>
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
    constexpr int kBalls = 8;   // the zones and the two hand points

    const char* kZones = R"(
Texture2D<float4>   g_src : register(t0);
RWTexture2D<float4> g_dst : register(u0);
cbuffer P : register(b0)
{
    uint   w; uint h; uint n; uint pad0;
    float4 tn;            // tangents of the eye's left, right, up and down edges
    float4 eye;           // the eye's position (tracking space)
    float4 ax; float4 ay; float4 az;   // the eye's axes (tracking space): right, up, back
    float4 ball[8];       // centre (tracking space), radius (m)
    float4 col[8];        // colour, fill opacity
    float4 rim[8];        // rim opacity, rim whiteness, grid lines (1 = on)
};
[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    float4       c  = g_src.Load(int3(p, 0));
    const float2 uv = (float2(p) + 0.5) / float2(w, h);
    const float3 d  = normalize(ax.xyz * lerp(tn.x, tn.y, uv.x) + ay.xyz * lerp(tn.z, tn.w, uv.y) - az.xyz);
    for (uint i = 0; i < n; ++i) {
        const float3 oc = eye.xyz - ball[i].xyz;
        const float  r  = ball[i].w;
        const float  b  = dot(oc, d);
        const float  m2 = dot(oc, oc) - b * b;   // the ray's closest approach to the centre, squared
        if (m2 >= r * r) continue;
        const float s = sqrt(r * r - m2);
        if (-b + s <= 0.0) continue;               // behind the eye
        const float m    = sqrt(m2) / r;           // 0 through the centre, 1 at the outline
        const float edge = 1.0 - smoothstep(0.985, 1.0, m);   // soft outline (no stair steps)
        // The far surface first (half as opaque), then the near one (none when the eye is inside the ball).
        for (int k = 1; k >= 0; --k) {
            const float t = k == 1 ? -b + s : -b - s;
            if (t <= 0.0) continue;
            const float3 nrm  = (oc + d * t) / r;  // the surface's normal (tracking space, y up)
            const float  lit  = 0.45 + 0.55 * saturate(dot(nrm, float3(-0.29, 0.91, 0.29)));
            const float  lat  = asin(clamp(nrm.y, -1.0, 1.0)), lon = atan2(nrm.z, nrm.x);
            const float  step = 0.5235988;          // 30 deg
            const float  gl   = abs(frac(lat / step + 0.5) - 0.5) * step * r;              // to the nearest latitude line (m)
            const float  gn   = abs(frac(lon / step + 0.5) - 0.5) * step * r * cos(lat);   // to the nearest longitude line (m)
            const float  grid = rim[i].z * (1.0 - smoothstep(0.0015, 0.003, min(gl, gn)));
            const float  a    = max(col[i].a * (k == 1 ? 0.5 : 1.0), grid * (k == 1 ? 0.25 : 0.55)) * edge;
            c.rgb = lerp(c.rgb, lerp(col[i].rgb * lit, float3(1.0, 1.0, 1.0), 0.35 * grid), a);
        }
        const float band = smoothstep(0.92, 0.97, m) * edge;   // the rim
        c.rgb = lerp(c.rgb, lerp(col[i].rgb, float3(1.0, 1.0, 1.0), rim[i].y), rim[i].x * band);
    }
    g_dst[p] = c;
}
)";

    struct Cb
    {
        UINT  w, h, n, pad0;
        float tn[4];
        float eye[4];
        float ax[4], ay[4], az[4];
        float ball[kBalls][4];
        float col[kBalls][4];
        float rim[kBalls][4];
    };

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
    bool                       g_was_on = false;

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
            if (!compile_fn || FAILED(compile_fn(kZones, strlen(kZones), "zones", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[Zones] shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "no D3DCompile");
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
            spdlog::info("[Zones] eye picture {}x{} format {} (samples {}): {}", d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, g_tex_failed ? "no copies (not drawn)" : "ready");
        }
        return !g_tex_failed && d.SampleDesc.Count == 1 && d.ArraySize == 1;
    }
}

// After the throw arc, before vehfade: the zones over this eye's picture.
void FarCry5Stereo::gesture_zones_draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* eye)
{
    const bool on = fc5::gesture::zone_show() && m_motion.load() && !m_menu_screen_on;
    if (on != g_was_on) spdlog::info("[Zones] gesture zones {}", on ? "shown" : "hidden");
    g_was_on = on;
    if (!on || !ctx || !eye) return;
    const int   fr = m_frame.load();
    const auto& v  = m_xr_views[fr & 7];
    if (v.frame != fr) return;
    const int e = fr & 1;
    // The head for the zones: between the eyes, turned as the eyes are (they are parallel on a Quest).
    const glm::vec3 p0{ v.pos[0][0], v.pos[0][1], v.pos[0][2] }, p1{ v.pos[1][0], v.pos[1][1], v.pos[1][2] };
    const glm::quat q0{ v.rot[0][3], v.rot[0][0], v.rot[0][1], v.rot[0][2] }, q1{ v.rot[1][3], v.rot[1][0], v.rot[1][1], v.rot[1][2] };   // w x y z
    glm::mat4       head = glm::mat4_cast(glm::slerp(q0, q1, 0.5f));
    head[3]              = glm::vec4{ 0.5f * (p0 + p1), 1.0f };

    Cb cb{};
    for (int k = 0; k < 4; ++k) cb.tn[k] = std::tan(v.fov[e][k]);
    const glm::vec3 pe = e ? p1 : p0;
    const glm::mat3 re = glm::mat3_cast(e ? q1 : q0);
    for (int k = 0; k < 3; ++k) {
        cb.eye[k] = pe[k];
        cb.ax[k]  = re[0][k];
        cb.ay[k]  = re[1][k];
        cb.az[k]  = re[2][k];
    }
    fc5::gesture::ZoneView zv[kBalls - 2]{};
    const int              nz = fc5::gesture::zone_views(head, m_left_handed.load(), zv, kBalls - 2);
    // By gesture: the shoulder (switch weapon), the sling, the throwable, the sidearm on the hip.
    static const float kHue[4][3]{ { 0.25f, 0.6f, 1.0f }, { 1.0f, 0.55f, 0.1f }, { 0.25f, 1.0f, 0.4f }, { 0.75f, 0.35f, 1.0f } };
    UINT n = 0;
    for (int i = 0; i < nz; ++i) {
        const auto& z = zv[i];
        const float* hue = kHue[std::min(i, 3)];
        float fill = 0.16f, rim = 0.75f, white = 0.0f;
        float rgb[3]{ hue[0], hue[1], hue[2] };
        if (!z.on) {
            for (float& c : rgb) c = 0.6f;
            fill = 0.06f;
            rim  = 0.3f;
        } else if (!z.available) {
            fill = 0.1f;
            rim  = 0.4f;
        } else {
            if (z.inside) { fill = 0.32f; rim = 0.95f; }
            if (z.held) { fill = std::max(fill, 0.3f); rim = 1.0f; white = 0.8f; }
        }
        cb.ball[n][0] = z.centre.x; cb.ball[n][1] = z.centre.y; cb.ball[n][2] = z.centre.z; cb.ball[n][3] = z.radius;
        cb.col[n][0]  = rgb[0]; cb.col[n][1] = rgb[1]; cb.col[n][2] = rgb[2]; cb.col[n][3] = fill;
        cb.rim[n][0]  = rim; cb.rim[n][1] = white; cb.rim[n][2] = 1.0f;
        n++;
    }
    // The point of each controller the zones test: a 2 cm white ball.
    for (int h = 0; h < 2; ++h) {
        glm::vec3 hp{};
        if (!fc5::gesture::hand_point(h, hp)) continue;
        cb.ball[n][0] = hp.x; cb.ball[n][1] = hp.y; cb.ball[n][2] = hp.z; cb.ball[n][3] = 0.02f;
        cb.col[n][0]  = 1.0f; cb.col[n][1] = 1.0f; cb.col[n][2] = 1.0f; cb.col[n][3] = 0.85f;
        cb.rim[n][0]  = 1.0f; cb.rim[n][1] = 0.0f;
        n++;
    }
    cb.n = n;
    if (n == 0) return;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    const bool ok = ready(dev, eye);
    dev->Release();
    if (!ok) return;
    cb.w = g_desc.Width;
    cb.h = g_desc.Height;
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(g_cb), 0u, static_cast<const D3D11_BOX*>(nullptr),
                    static_cast<const void*>(&cb), 0u, 0u);   // past the mod's hook, which swaps camera matrices it recognises
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
    if ((++g_drawn % 900) == 1) {
        // Each zone from the head, in the head's level frame (right, up, forward; cm), and its state.
        const glm::vec3 hp{ head[3] };
        const glm::vec3 r = glm::normalize(glm::vec3{ head[0].x, 0.0f, head[0].z }), f = glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, r);
        std::string     s;
        for (int i = 0; i < nz; ++i) {
            const glm::vec3 o = zv[i].centre - hp;
            s += fmt::format("{}{} {:+.0f} {:+.0f} {:+.0f} cm r {:.0f} ({}{}{})", i ? "; " : "", fc5::gesture::name(i), glm::dot(o, r) * 100.0f, o.y * 100.0f, glm::dot(o, f) * 100.0f,
                             zv[i].radius * 100.0f, zv[i].on ? (zv[i].available ? "can act" : "not now") : "off", zv[i].inside ? ", hand inside" : "", zv[i].held ? ", held" : "");
        }
        spdlog::info("[Zones] {} eye pictures drawn ({} balls, eye picture {}x{}); from the head (right, up, forward): {}", g_drawn, n, g_desc.Width, g_desc.Height, s);
    }
}
