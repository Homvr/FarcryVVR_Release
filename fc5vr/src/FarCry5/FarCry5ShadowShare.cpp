#include "FarCry5ShadowShare.h"
#include "FarCry5CamScan.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace fc5::shadowshare
{
    namespace
    {
        const char* kCompare = R"(
Texture2DArray<float> g_a : register(t0);
Texture2DArray<float> g_b : register(t1);
RWByteAddressBuffer   g_count : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h, n;
    g_a.GetDimensions(w, h, n);
    if (id.x >= w || id.y >= h || id.z >= n) return;
    if (abs(g_a[id] - g_b[id]) > 1e-5f) g_count.InterlockedAdd(0, 1);
}
)";
        ID3D11ComputeShader*       g_cs     = nullptr;
        bool                       g_failed = false;
        ID3D11Texture2D*           g_stash  = nullptr;
        D3D11_TEXTURE2D_DESC       g_desc{};
        ID3D11Resource*            g_stash_of = nullptr;
        ID3D11Buffer*              g_count  = nullptr;
        ID3D11UnorderedAccessView* g_count_uav = nullptr;
        constexpr int              kRing = 4;
        ID3D11Buffer*              g_staging[kRing]{};
        bool                       g_pending[kRing]{};
        uint32_t                   g_pair[kRing]{};
        uint64_t                   g_total[kRing]{};
        int                        g_slot = 0;

        DXGI_FORMAT srv_format(DXGI_FORMAT f)
        {
            switch (f) {
            case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
            case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
            case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            default: return DXGI_FORMAT_UNKNOWN;   // typed depth formats cannot be read: no check (never shared)
            }
        }

        bool init(ID3D11Device* dev)
        {
            if (g_cs || g_failed) return g_cs != nullptr;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static auto compile_fn = reinterpret_cast<CompileFn>(GetProcAddress(LoadLibraryA("d3dcompiler_47.dll"), "D3DCompile"));
            ID3DBlob* code = nullptr;
            ID3DBlob* err  = nullptr;
            if (!compile_fn || FAILED(compile_fn(kCompare, strlen(kCompare), "shadow_compare", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
                spdlog::error("[Shadow] compare shader failed: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                if (err) err->Release();
                g_failed = true;
                return false;
            }
            dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cs);
            code->Release();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            dev->CreateBuffer(&bd, nullptr, &g_count);
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            if (g_count) dev->CreateUnorderedAccessView(g_count, &ud, &g_count_uav);
            D3D11_BUFFER_DESC sd{};
            sd.ByteWidth = 16; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& s : g_staging) dev->CreateBuffer(&sd, nullptr, &s);
            g_failed = !(g_cs && g_count_uav && g_staging[kRing - 1]);
            spdlog::info("[Shadow] shadow-map comparison {}", g_failed ? "unavailable" : "ready");
            return !g_failed;
        }

        ID3D11ShaderResourceView* view(ID3D11Device* dev, ID3D11Resource* r, const D3D11_TEXTURE2D_DESC& d)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format                         = srv_format(d.Format);
            vd.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipLevels       = 1;
            vd.Texture2DArray.ArraySize       = d.ArraySize;
            ID3D11ShaderResourceView* v = nullptr;
            dev->CreateShaderResourceView(r, &vd, &v);
            return v;
        }
    }

    bool stash(ID3D11DeviceContext* ctx, ID3D11Resource* target)
    {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        bool ok = init(dev);
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(target)->GetDesc(&d);
        if (ok && srv_format(d.Format) == DXGI_FORMAT_UNKNOWN) ok = false;
        if (ok && (!g_stash || g_desc.Width != d.Width || g_desc.Height != d.Height || g_desc.ArraySize != d.ArraySize || g_desc.Format != d.Format)) {
            if (g_stash) g_stash->Release();
            g_stash = nullptr;
            D3D11_TEXTURE2D_DESC nd = d;
            nd.MipLevels = d.MipLevels; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
            dev->CreateTexture2D(&nd, nullptr, &g_stash);
            g_desc = d;
        }
        dev->Release();
        if (!ok || !g_stash) return false;
        ctx->CopyResource(g_stash, target);
        g_stash_of = target;
        return true;
    }

    void compare(ID3D11DeviceContext* ctx, ID3D11Resource* target, uint32_t pair_id)
    {
        if (!g_cs || !g_stash || target != g_stash_of || g_pending[g_slot]) return;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        ID3D11ShaderResourceView* va = view(dev, g_stash, g_desc);
        ID3D11ShaderResourceView* vb = view(dev, target, g_desc);
        dev->Release();
        if (va && vb) {
            ID3D11ComputeShader*       prev_cs = nullptr;
            ID3D11ShaderResourceView*  prev_srv[2]{};
            ID3D11UnorderedAccessView* prev_uav = nullptr;
            ctx->CSGetShader(&prev_cs, nullptr, nullptr);
            ctx->CSGetShaderResources(0, 2, prev_srv);
            ctx->CSGetUnorderedAccessViews(0, 1, &prev_uav);
            const UINT zero[4]{};
            ctx->ClearUnorderedAccessViewUint(g_count_uav, zero);
            ID3D11ShaderResourceView* srv[2]{ va, vb };
            ctx->CSSetShader(g_cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 2, srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &g_count_uav, nullptr);
            ctx->Dispatch((g_desc.Width + 7) / 8, (g_desc.Height + 7) / 8, g_desc.ArraySize);
            ctx->CSSetShader(prev_cs, nullptr, 0);
            ctx->CSSetShaderResources(0, 2, prev_srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &prev_uav, nullptr);
            if (prev_cs) prev_cs->Release();
            for (auto* p : prev_srv) if (p) p->Release();
            if (prev_uav) prev_uav->Release();
            ctx->CopyResource(g_staging[g_slot], g_count);
            g_pending[g_slot] = true;
            g_pair[g_slot]    = pair_id;
            g_total[g_slot]   = uint64_t(g_desc.Width) * g_desc.Height * g_desc.ArraySize;
            g_slot            = (g_slot + 1) % kRing;
        }
        if (va) va->Release();
        if (vb) vb->Release();
    }

    bool result(ID3D11DeviceContext* ctx, uint32_t& pair_id, uint64_t& differing, uint64_t& total)
    {
        for (int k = 0; k < kRing; ++k) {
            const int s = (g_slot + k) % kRing;   // oldest first
            if (!g_pending[s]) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (ctx->Map(g_staging[s], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return false;
            uint32_t n = 0;
            std::memcpy(&n, m.pData, 4);
            ctx->Unmap(g_staging[s], 0);
            g_pending[s] = false;
            pair_id      = g_pair[s];
            differing    = n;
            total        = g_total[s];
            return true;
        }
        return false;
    }
}

namespace fc5::shadowshare
{
    namespace
    {
        std::atomic<int> g_clog{ 0 };   // frames left to log
        std::atomic<int> g_csmscan{ 0 };   // csmscan: 1 = scan at the next left-eye cascade upload, >2 = frames left to track
        int              g_clog_frame = -1;
        struct Seen
        {
            int   frame = -10;
            float v[64]{};
        };
        Seen  g_csm[2], g_asm[2], g_cam[2];
        float g_csm_pair[64]{};   // the left eye's cascade rows of the current pair
        int   g_csm_pair_frame = -10;

        float max_diff(const float* a, const float* b, int n)
        {
            float m = 0.0f;
            for (int i = 0; i < n; ++i) m = std::max(m, std::abs(a[i] - b[i]));
            return m;
        }
        // Game camera from its view matrix (rows as uploaded: world -> view, right-handed, Z up): yaw/pitch in degrees, position.
        void cam_info(const float* v, float& yaw, float& pitch, float& px, float& py, float& pz)
        {
            const float fx = -v[8], fy = -v[9], fz = -v[10];
            yaw   = std::atan2(fy, fx) * 57.29578f;
            pitch = std::asin(std::clamp(fz, -1.0f, 1.0f)) * 57.29578f;
            // position = -R^T t (rows r0..r2 of the rotation, t in the w column)
            px = -(v[0] * v[3] + v[4] * v[7] + v[8] * v[11]);
            py = -(v[1] * v[3] + v[5] * v[7] + v[9] * v[11]);
            pz = -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]);
        }
    }

    void cascade_log_start(int frames)
    {
        g_clog = frames;
        g_clog_frame = -1;
        for (auto* s : { g_csm, g_asm, g_cam }) s[0].frame = s[1].frame = -10;
        spdlog::info("[Cascade] logging the sun's cascade and far-shadow buffers for {} frames", frames);
    }

    bool cascade_logging() { return g_clog.load() > 0; }

    void cascade_note(int frame, int call, uint32_t size, const float* d, const float* game_view)
    {
        if (g_clog.load() <= 0 || size < 224 || size > 512) return;
        if (frame != g_clog_frame) {
            g_clog_frame = frame;
            if (--g_clog <= 0) { spdlog::info("[Cascade] logging done"); return; }
        }
        const int eye = frame & 1;
        // CSM kind: ShadowMapSize at c10 = (S, S, 1/S, 1/S), CSMSlice0_0..2_2 in c0-c8 (CSunShadowProvider, CTileLightingBlended...).
        const float s = d[40];
        const bool  csm = s >= 256.0f && s <= 16384.0f && (std::abs(d[41] - s) < 0.5f || std::abs(d[41] * s - 1.0f) < 1e-3f);
        // ASM resolve kind (224 B): ASMLongRangeLightDir c11 unit, ASMLongRangeBlockerSearchVector c10, IsSunLow c13.w.
        const float l11 = std::sqrt(d[44] * d[44] + d[45] * d[45] + d[46] * d[46]);
        const uint32_t low = *reinterpret_cast<const uint32_t*>(d + 55);
        const bool  asmk = !csm && size == 256 && std::abs(l11 - 1.0f) < 0.01f && (low == 0 || low == 1 || low == 0xFFFFFFFFu);
        if (!csm && !asmk) return;
        float yaw = 0, pitch = 0, px = 0, py = 0, pz = 0;
        cam_info(game_view, yaw, pitch, px, py, pz);
        const float cam_move = g_cam[eye].frame == frame - 2 ? max_diff(game_view, g_cam[eye].v, 16) : -1.0f;
        if (g_cam[eye].frame != frame) { std::memcpy(g_cam[eye].v, game_view, 64); g_cam[eye].frame = frame; }
        if (csm) {
            auto& p = g_csm[eye];
            const bool same_frame = p.frame == frame;   // several buffers per frame carry the cascades
            if (eye == 0 && !same_frame && g_csmscan.load() > 0) {
                // csmscan: the CPU copies of slice 1's first row (as uploaded), then the copies that keep following it
                // (probes\camscan.txt, for camwatch: which code writes the cascade fit).
                const float v[3]{ d[12], d[13], d[14] };
                if (g_csmscan.load() == 1) {
                    fc5::camscan::scan(v);
                    g_csmscan = 2 + 200;
                } else {
                    fc5::camscan::track(v);
                    if (--g_csmscan <= 2) { fc5::camscan::report(0.9f); g_csmscan = 0; }
                }
            }
            const float rot  = p.frame >= 0 ? max_diff(d, p.v, 36) : -1.0f;
            float       move = 0.0f;   // largest change of a slice row's offset (w), in texture units
            for (int r = 0; r < 9; ++r) move = std::max(move, p.frame >= 0 ? std::abs(d[r * 4 + 3] - p.v[r * 4 + 3]) : -1.0f);
            float pair = -1.0f;
            if (eye == 0 && !same_frame) { std::memcpy(g_csm_pair, d, 36 * 4); g_csm_pair_frame = frame; }
            if (eye == 1 && g_csm_pair_frame == frame - 1) pair = max_diff(d, g_csm_pair, 36);
            if (!same_frame && (rot > 1e-5f || pair > 1e-5f || p.frame < 0)) {
                // Where each slice's square lies on the ground (1.7 m under the game camera): its centre relative to the
                // camera along the camera's heading (ahead) and to its right, and its width (1 / uv scale), in metres.
                std::string geo;
                const float yr = yaw / 57.29578f, fx = std::cos(yr), fy = std::sin(yr);
                for (int k = 0; k < 3; ++k) {
                    const float* a = d + k * 12;
                    const float* b = d + k * 12 + 4;
                    // Solve a.p = 0.5 - a.w, b.p = 0.5 - b.w, p.z = pz - 1.7 (Cramer on the 3x3 [a; b; (0 0 1)]).
                    const float r0 = 0.5f - a[3], r1 = 0.5f - b[3], z = pz - 1.7f;
                    const float det = a[0] * b[1] - a[1] * b[0];
                    if (std::abs(det) < 1e-12f) { geo += " slice ? "; continue; }
                    const float qx = ((r0 - a[2] * z) * b[1] - a[1] * (r1 - b[2] * z)) / det;
                    const float qy = (a[0] * (r1 - b[2] * z) - (r0 - a[2] * z) * b[0]) / det;
                    const float ox = qx - px, oy = qy - py;
                    const float ahead = ox * fx + oy * fy, right = ox * fy - oy * fx;
                    const float w = 1.0f / std::max(1e-6f, std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]));
                    geo += fmt::format(" slice{} {:.1f} m wide, centre {:.1f} m ahead {:.1f} m right;", k, w, ahead, right);
                }
                if (eye == 0 && (frame % 60) == 0) {
                    // Coverage on flat ground 1.7 m under the camera: for each distance, the cascade (0-2, '.' = none:
                    // far shadows) of points every 10 deg from 60 deg right to 60 deg left of the camera heading (angles counter-clockwise).
                    // A point is in a slice when its uv lies in the inner 80 % (the shader fades out the outer band).
                    std::string cov;
                    for (float r : { 5.0f, 10.0f, 20.0f, 30.0f, 45.0f, 60.0f }) {
                        cov += fmt::format(" {:.0f}m:", r);
                        for (int deg = -60; deg <= 60; deg += 10) {
                            const float a  = yr + deg / 57.29578f;
                            const float wx = px + r * std::cos(a), wy = py + r * std::sin(a), wz = pz - 1.7f;
                            char c = '.';
                            for (int k = 0; k < 3; ++k) {
                                const float* s = d + k * 12;
                                const float  u = s[0] * wx + s[1] * wy + s[2] * wz + s[3];
                                const float  v = s[4] * wx + s[5] * wy + s[6] * wz + s[7];
                                if (std::abs(2.0f * u - 1.0f) < 0.8f && std::abs(2.0f * v - 1.0f) < 0.8f) { c = char('0' + k); break; }
                            }
                            cov += c;
                        }
                    }
                    spdlog::info("[Cascade] f{} coverage (60 deg right .. 60 deg left of the heading, every 10):{}", frame, cov);
                }
                spdlog::info("[Cascade] f{} call{} {} {}B: cascades changed {:.5f} (offsets {:.5f}) since its last frame{}; game cam yaw {:.2f} pitch {:.2f} pos ({:.2f},{:.2f},{:.2f}) moved {:.5f}; "
                             "slice scales {:.6f} {:.6f} {:.6f};{}",
                             frame, call, eye ? "R" : "L", size, rot, move, pair >= 0 ? fmt::format(", right vs left of the pair {:.5f}", pair) : std::string{}, yaw, pitch, px, py, pz, cam_move,
                             std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), std::sqrt(d[12] * d[12] + d[13] * d[13] + d[14] * d[14]), std::sqrt(d[24] * d[24] + d[25] * d[25] + d[26] * d[26]), geo);
            }
            if (!same_frame) { std::memcpy(p.v, d, 36 * 4); p.frame = frame; }
        } else {
            auto& p = g_asm[eye];
            if (p.frame == frame) return;
            const float idx = p.frame >= 0 ? max_diff(d, p.v, 16) : -1.0f;
            const bool  other = p.frame < 0 || std::abs(d[35] - p.v[35]) > 1e-6f || std::abs(d[43] - p.v[43]) > 1e-6f || std::abs(d[55] - p.v[55]) > 1e-6f;
            if (idx > 1e-6f || other)
                spdlog::info("[Cascade] f{} call{} {} far shadows (ASM): index matrix changed {:.6f}; default shadow {:.3f}, time-of-day lerp {:.4f}, sun low {}; light ({:.3f},{:.3f},{:.3f}); game cam yaw {:.2f} pitch {:.2f}",
                             frame, call, eye ? "R" : "L", idx, d[35], d[43], *reinterpret_cast<const uint32_t*>(d + 55), d[44], d[45], d[46], yaw, pitch);
            std::memcpy(p.v, d, 56 * 4);
            p.frame = frame;
        }
    }
}

namespace fc5::shadowshare
{
    namespace
    {
        std::atomic<int> g_glog{ 0 };
        int              g_glog_frame = -1;
        struct WorldPlanes
        {
            int   frame = -10;
            int   n     = 0;
            float p[64][4]{};
            float view[16]{};   // the game view recorded with them
        };
        WorldPlanes g_gp[2];   // per eye (frame parity)

        // A plane n.x + d = 0 in a view space (x_v = V x_w, V rows as uploaded) taken to world space.
        void to_world(const float* v, const float* pl, float* out)
        {
            for (int k = 0; k < 3; ++k) out[k] = pl[0] * v[0 + k] + pl[1] * v[4 + k] + pl[2] * v[8 + k];
            out[3] = pl[3] + pl[0] * v[3] + pl[1] * v[7] + pl[2] * v[11];
        }
        // Distance between two world planes: normal angle (deg) and offset (m), the closest plane of the set.
        void closest(const float* w, const WorldPlanes& set, float& deg, float& off)
        {
            deg = 1e9f; off = 1e9f;
            for (int j = 0; j < set.n; ++j) {
                const float* q = set.p[j];
                const float  c = std::clamp(w[0] * q[0] + w[1] * q[1] + w[2] * q[2], -1.0f, 1.0f);
                const float  a = std::acos(c) * 57.29578f;
                const float  o = std::abs(w[3] - q[3]);
                if (a + o < deg + off) { deg = a; off = o; }
            }
        }
    }

    void giplane_log_start(int frames)
    {
        g_glog       = frames;
        g_glog_frame = -1;
        g_gp[0].frame = g_gp[1].frame = -10;
        spdlog::info("[GIPlanes] logging the indoor volume planes for {} frames", frames);
    }

    bool giplane_logging() { return g_glog.load() > 0; }

    void giplane_note(int frame, int call, const float* planes, int n, const float* game_view, bool fresh)
    {
        if (g_glog.load() <= 0) return;
        if (frame != g_glog_frame) {
            g_glog_frame = frame;
            if (--g_glog <= 0) { spdlog::info("[GIPlanes] logging done"); return; }
        }
        if (n <= 0) {   // the planes in use: unit normals packed from c0, as giplanes counts them
            n = 0;
            for (; n < 256; ++n) {
                const float* p = planes + n * 4;
                if (!std::isfinite(p[0]) || !std::isfinite(p[3]) || std::abs(p[0] * p[0] + p[1] * p[1] + p[2] * p[2] - 1.0f) > 2e-3f || std::abs(p[3]) > 1e5f) break;
            }
        }
        n = std::min(n, 64);
        const int eye = frame & 1;
        auto&     prev = g_gp[eye];
        WorldPlanes cur;
        cur.frame = frame;
        cur.n     = n;
        std::memcpy(cur.view, game_view, 64);
        for (int i = 0; i < n; ++i) to_world(game_view, planes + i * 4, cur.p[i]);
        float worst_deg = 0, worst_off = 0, lag_deg = 0, lag_off = 0, other_deg = 0, other_off = 0;
        int   matched = 0;
        if (prev.frame >= 0 && prev.n > 0) {
            for (int i = 0; i < n; ++i) {
                float d, o;
                closest(cur.p[i], prev, d, o);
                if (d > 20.0f) continue;   // a volume that came or went
                matched++;
                worst_deg = std::max(worst_deg, d); worst_off = std::max(worst_off, o);
                // The same planes taken to world with the previous frame's camera (built one camera late?).
                float w[4];
                to_world(prev.view, planes + i * 4, w);
                closest(w, prev, d, o);
                lag_deg = std::max(lag_deg, d); lag_off = std::max(lag_off, o);
            }
            // The other eye's latest record (the pair's other call)
            const auto& oth = g_gp[eye ^ 1];
            if (oth.frame >= frame - 2 && oth.n > 0) {
                for (int i = 0; i < n; ++i) {
                    float d, o;
                    closest(cur.p[i], oth, d, o);
                    if (d > 20.0f) continue;
                    other_deg = std::max(other_deg, d); other_off = std::max(other_off, o);
                }
            }
        }
        float yaw = std::atan2(-game_view[9], -game_view[8]) * 57.29578f;
        spdlog::info("[GIPlanes] f{} call{} {} {} planes (fresh {}): world planes vs this eye's last frame: worst {:.3f} deg {:.3f} m ({} matched); "
                     "with the last frame's camera {:.3f} deg {:.3f} m; vs the other eye {:.3f} deg {:.3f} m; game yaw {:.2f}",
                     frame, call, eye ? "R" : "L", n, fresh, worst_deg, worst_off, matched, lag_deg, lag_off, other_deg, other_off, yaw);
        prev = cur;
    }
}
namespace fc5::shadowshare
{
    bool command(const std::string& cmd, float fv, bool on)
    {
        if (cmd == "cascadelog") cascade_log_start((int)fv);
        else if (cmd == "giplanelog") giplane_log_start((int)fv);
        else if (cmd == "csmscan") { g_csmscan = 1; cascade_log_start(520); }
        else if (csm_command(cmd, fv, 0)) {}
        else if (fit_command(cmd, fv)) {}
        else if (fit_command2(cmd, fv, on)) {}
        else return false;
        return true;
    }
}

// Research (2026-10-16, cascade fit): a hook inside the game's sun-shadow render setup (FC_m64.dll+0x5842f0, the
// instruction after the view block's address is in rcx) logs the view blocks it walks: the light matrix columns
// (+0x1c0..0x1f0), the extents (+0x218..0x220), the cascade count (+0x2770). csmwatch K puts a hardware write watch on
// cascade K's block (+0x10 + K*0xd20 + 0x1b0) of the first view: the writer is the code that fits the cascades.
#include <safetyhook/easy.hpp>
#include <safetyhook/mid_hook.hpp>

namespace fc5::shadowshare
{
    namespace
    {
        safetyhook::MidHook      g_csm_hook{};
        std::atomic<uintptr_t>   g_csm_view{ 0 };
        std::atomic<int>         g_csm_hook_log{ 0 };

        bool readable(uintptr_t p, size_t n)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!p || VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
            return p + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }

        void csm_probe(safetyhook::Context& c)
        {
            const uintptr_t v = c.rcx;
            if (!g_csm_view.load()) g_csm_view = v;
            if (g_csm_hook_log.load() <= 0 || !readable(v, 0x2780)) return;
            g_csm_hook_log--;
            const float*   f = reinterpret_cast<const float*>(v);
            const uint32_t n = *reinterpret_cast<const uint32_t*>(v + 0x2770);
            std::string    cas;
            for (uint32_t k = 0; k < std::min<uint32_t>(n, 4); ++k) {
                const float* m = reinterpret_cast<const float*>(v + 0x10 + k * 0xd20 + 0x1b0);
                cas += fmt::format(" | cascade {} +0x{:x}: c0 ({:.4g} {:.4g} {:.4g} {:.4g}) c1 ({:.4g} {:.4g} {:.4g} {:.4g}) c3 ({:.4g} {:.4g} {:.4g} {:.4g})", k, 0x10 + k * 0xd20 + 0x1b0,
                                   m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[12], m[13], m[14], m[15]);
            }
            spdlog::info("[CsmHook] view {} (renderer {}): light c0 ({:.4g} {:.4g} {:.4g} {:.4g}) c3 ({:.4g} {:.4g} {:.4g} {:.4g}); +0x218 {:.5g} {:.5g} {:.5g}; cascades {}{}",
                         (void*)v, (void*)c.r12, f[0x1c0 / 4], f[0x1c0 / 4 + 1], f[0x1c0 / 4 + 2], f[0x1c0 / 4 + 3], f[0x1f0 / 4], f[0x1f0 / 4 + 1], f[0x1f0 / 4 + 2], f[0x1f0 / 4 + 3],
                         f[0x218 / 4], f[0x21c / 4], f[0x220 / 4], n, cas);
        }
    }

    bool csm_command(const std::string& cmd, float fv, unsigned long render_tid)
    {
        if (cmd == "csmhook") {
            if (!g_csm_hook) {
                auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
                // Expected bytes at +0x5844ab: 48 8b 11 (mov rdx, [rcx]); refuse anything else (another game version).
                if (!base || !readable(reinterpret_cast<uintptr_t>(base + 0x5844ab), 3) || base[0x5844ab] != 0x48 || base[0x5844ac] != 0x8b || base[0x5844ad] != 0x11) {
                    spdlog::error("[CsmHook] unexpected code at FC_m64.dll+0x5844ab: hook not installed");
                    return true;
                }
                g_csm_hook = safetyhook::create_mid(base + 0x5844ab, csm_probe);
                spdlog::info("[CsmHook] hook {}", g_csm_hook ? "installed" : "failed");
            }
            g_csm_hook_log = fv > 0.0f ? (int)fv : 4;
            return true;
        }
        if (cmd == "csmwatch") {
            const uintptr_t v = g_csm_view.load();
            if (!v) { spdlog::info("[CsmHook] no view block seen yet (csmhook first)"); return true; }
            const int k = std::clamp((int)fv, 0, 3);
            void* a = reinterpret_cast<void*>(v + 0x10 + k * 0xd20 + 0x1b0);
            spdlog::info("[CsmHook] write watch on cascade {} of view {} at {}", k, (void*)v, a);
            fc5::camscan::watch_addresses({ a, reinterpret_cast<void*>(v + 0x1c0), reinterpret_cast<void*>(v + 0x218) }, render_tid, false);
            return true;
        }
        return false;
    }
}

// shadowfit (2026-10-16): the game's cascade sphere fit (FC_m64.dll+0x6b280e0, reached through 0x55f030 from the cascade
// setup at 0x6b7ea61) turns one field-of-view value (xmm0 = [5th argument]) and the camera's aspect ([r9+0x18]) into
// horizontal / vertical angles (call 0x39e520 at +0x6b281a3), then fits one bounding sphere per split. The mod gives the
// game camera a 120 deg view so its object culling covers both eyes; the cascades are fitted to that same 120 deg, which
// uses them up within ~10-15 m. A hook on that call can hand the fit a narrower view (the eyes' own) without touching culling.
namespace fc5::shadowshare
{
    namespace
    {
        safetyhook::MidHook g_fit_hook{};
        std::atomic<int>    g_fit_log{ 0 };
        std::atomic<float>  g_fit_scale{ 1.0f };   // shadowfit: the fit's field of view times this (1 = the game's)
        std::atomic<uint64_t> g_fit_calls{ 0 };
        std::atomic<float>  g_fit_last_in{ 0.0f }, g_fit_last_out{ 0.0f };

        void fit_probe(safetyhook::Context& c)
        {
            g_fit_calls++;
            const float in = c.xmm0.f32[0];
            float       out = fit_widen(in, c.xmm1.f32[0]);
            const float s = g_fit_scale.load();
            if (s > 0.0f && s < 1.0f && std::isfinite(in) && in > 0.0f) out = in * s;   // research: narrower
            c.xmm0.f32[0] = out;
            g_fit_last_in = in;
            g_fit_last_out = out;
            if (g_fit_log.load() > 0) {
                g_fit_log--;
                // The 5th..8th stack arguments: [rbp+0xc0] fov pointer, [rbp+0xc8] split array, [rbp+0xd0] matrix, [rbp+0xd8].
                const uintptr_t splits = readable(c.rbp + 0xc8, 8) ? *reinterpret_cast<const uintptr_t*>(c.rbp + 0xc8) : 0;
                std::string     sp;
                if (splits && readable(splits, 16)) {
                    const float* s4 = reinterpret_cast<const float*>(splits);
                    sp = fmt::format("{:.4g} {:.4g} {:.4g} {:.4g}", s4[0], s4[1], s4[2], s4[3]);
                }
                const float aspect = readable(c.rbx + 0x18, 4) ? *reinterpret_cast<const float*>(c.rbx + 0x18) : -1.0f;
                const float farv = readable(c.rbx + 0x22c, 4) ? *reinterpret_cast<const float*>(c.rbx + 0x22c) : -1.0f;
                spdlog::info("[ShadowFit] fit call: view value {:.5g} -> {:.5g} (xmm1 {:.5g}, camera +0x18 {:.5g}, +0x22c {:.5g}); splits {}", in, out, c.xmm1.f32[0], aspect, farv, sp);
            }
        }
    }

    bool fit_install()
    {
        if (g_fit_hook) return true;
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        // Expected at +0x6b281a3: E8 rel32 (call 0x39e520). Anything else (another game version): no hook.
        if (!base || !readable(reinterpret_cast<uintptr_t>(base + 0x6b281a3), 5) || base[0x6b281a3] != 0xE8 ||
            base + 0x6b281a8 + *reinterpret_cast<const int32_t*>(base + 0x6b281a4) != base + 0x39e520) {
            spdlog::error("[ShadowFit] unexpected code at FC_m64.dll+0x6b281a3: no hook");
            return false;
        }
        g_fit_hook = safetyhook::create_mid(base + 0x6b281a3, fit_probe);
        spdlog::info("[ShadowFit] hook on the cascade fit {}", g_fit_hook ? "installed" : "failed");
        return (bool)g_fit_hook;
    }

    bool fit_command(const std::string& cmd, float fv)
    {
        if (cmd == "shadowfitlog") { fit_install(); g_fit_log = fv > 0.0f ? (int)fv : 4; return true; }
        if (cmd == "shadowfitscale") {
            g_fit_scale = std::clamp(fv, 0.3f, 1.0f);
            if (g_fit_scale.load() < 1.0f) fit_install();
            spdlog::info("[ShadowFit] cascade fit view scale {:.3f}", g_fit_scale.load());
            return true;
        }
        return false;
    }
}

namespace fc5::shadowshare
{
    namespace
    {
        std::atomic<bool>  g_fit_on{ true };      // shadowfit on|off (default on): the fit covers both eyes' view (+ 3 deg)
        std::atomic<float> g_fit_need_h{ 0.0f };  // the culling's half-angle tangents (both eyes + its margin), from the frame
        std::atomic<float> g_fit_need_v{ 0.0f };
        std::atomic<float> g_fit_min_deg{ 0.0f }; // shadowfitmin <deg>: test, the fit's view at least this (any interpretation)
        std::atomic<uint64_t> g_fit_widened{ 0 };
    }

    // Called each VR frame by the culling code with the tangents it covers (left/right and up/down, the larger side).
    void fit_set_need(float tan_h, float tan_v)
    {
        g_fit_need_h = tan_h;
        g_fit_need_v = tan_v;
        static bool tried = false;
        if (!tried && g_fit_on.load()) { tried = true; fit_install(); }
    }

    // The fit's view value (radians) widened so its frustum holds the culling's: the value may be the vertical or the
    // horizontal angle (0x39e520 derives the other one with the aspect), so the tangent covers both readings.
    float fit_widen(float in, float aspect)
    {
        float t = 0.0f;
        if (g_fit_on.load()) {
            const float a = (std::isfinite(aspect) && aspect > 0.2f && aspect < 5.0f) ? aspect : 1.0f;
            t = std::max(g_fit_need_h.load(), g_fit_need_v.load()) * std::max(a, 1.0f / a);
        }
        if (const float m = g_fit_min_deg.load(); m > 0.0f) t = std::max(t, std::tan(m * 0.5f / 57.29578f));
        if (!(t > 0.0f) || !std::isfinite(t)) return in;
        const float want = std::min(2.0f * std::atan(t), 2.6f);   // never past ~149 deg
        if (want > in) { g_fit_widened++; return want; }
        return in;
    }

    void fit_stats(uint64_t& calls, uint64_t& widened, float& in, float& out)
    {
        calls = g_fit_calls.exchange(0);
        widened = g_fit_widened.exchange(0);
        in = g_fit_last_in.load();
        out = g_fit_last_out.load();
    }

    bool fit_command2(const std::string& cmd, float fv, bool on)
    {
        if (cmd == "shadowfit") { g_fit_on = on; if (on) fit_install(); spdlog::info("[ShadowFit] cascades fitted to the culled view (both eyes): {}", on); return true; }
        if (cmd == "shadowfitmin") { g_fit_min_deg = std::clamp(fv, 0.0f, 150.0f); if (fv > 0.0f) fit_install(); spdlog::info("[ShadowFit] fit view at least {:.0f} deg (test)", g_fit_min_deg.load()); return true; }
        return false;
    }

    bool fit_enabled() { return g_fit_on.load(); }
}
