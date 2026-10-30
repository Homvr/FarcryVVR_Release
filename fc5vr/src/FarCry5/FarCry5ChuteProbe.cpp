// chuteprobe (log only, default on for now, saved; F11 Comfort > Cinematics and vehicles; the coordinator's 30 Oct note after Homard's
// "Can't you use the sound of parachute opening/closing to detect parachute otherwise ?"): is the open canopy a draw the mod can tell
// apart? While falling, under the game's wingsuit or parachute (chuteopen) and for 3 s after, each world draw within 8 m of the camera
// is keyed by its pixel shader and index count; a new key is logged once ("[ChuteProbe] new draw", its offset from the camera, "2-8 m
// above" marked, the shader's materials), and when the window closes a summary lists the keys by frames drawn with when each was
// first and last seen against the landing (a canopy should be drawn above the head and vanish at the landing). A skinned canopy may
// carry the player's root as its position (under the camera), so all heights are kept. Nothing on screen changes.
#include "FarCry5Stereo.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <vector>

// The world pass's draws (with vehicle_probe), only while the window is open.
void FarCry5Stereo::chute_probe(ID3D11DeviceContext* ctx, UINT count)
{
    if (!m_cp_on.load()) return;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return;
    ps->Release();   // only its address is kept, as a key
    ID3D11Buffer* b1 = nullptr;
    ctx->VSGetConstantBuffers(1, 1, &b1);
    if (!b1) return;
    ObjConsts c{};
    bool      have = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = m_obj_consts.find(b1); it != m_obj_consts.end()) { c = it->second; have = true; }
    }
    b1->Release();
    if (!have) return;
    const glm::vec3 rel = glm::vec3{ c.f[3], c.f[7], c.f[11] } - m_world_cam_pos;
    if (glm::length(rel) > 8.0f) return;
    const int   frame = m_frame.load();
    const float t     = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_cp_t0).count();
    const auto  key   = std::make_pair(static_cast<void*>(ps), count);
    {
        std::lock_guard _{ m_cp_mtx };
        if (auto it = m_cp_draws.find(key); it != m_cp_draws.end()) {
            if (it->second.last_frame != frame) {
                it->second.last_frame = frame;
                it->second.frames++;
                it->second.last = t;
            }
            return;
        }
    }
    std::string names;
    {
        std::lock_guard _{ m_ps_mtx };
        if (auto it = m_ps_names.find(ps); it != m_ps_names.end()) names = it->second.substr(0, 160);
    }
    std::lock_guard _{ m_cp_mtx };
    if (m_cp_draws.size() >= 400) return;
    ChuteDraw d;
    d.off        = rel;
    d.frames     = 1;
    d.first      = t;
    d.last       = t;
    d.last_frame = frame;
    d.names      = names;
    m_cp_draws.emplace(key, d);
    if (m_cp_logged++ < 80)
        spdlog::info("[ChuteProbe] new draw: ps {} {} indices at ({:+.2f}, {:+.2f}, {:+.2f}) m from the camera{} ({:.1f} s into the window; {}): {}", static_cast<void*>(ps), count,
                     rel.x, rel.y, rel.z, rel.z >= 2.0f && rel.z <= 8.0f ? ", 2-8 m above" : "", t,
                     m_co_suit ? "the game says wingsuit / parachute" : m_falling ? "falling" : "after", names);
}

// Once per update_vehicle: opens the window while falling or under a wingsuit / parachute, closes it 3 s after, then the summary.
void FarCry5Stereo::chute_probe_tick()
{
    const auto now  = std::chrono::steady_clock::now();
    const bool want = m_chute_probe.load() && (m_falling || m_co_suit || m_chute_vehicle);
    if (want) {
        if (!m_cp_on.load()) {
            std::lock_guard _{ m_cp_mtx };
            m_cp_draws.clear();
            m_cp_logged = 0;
            m_cp_t0     = now;
            m_cp_land_t = {};
            m_cp_on     = true;
        }
        m_cp_until = now + std::chrono::seconds(3);
        return;
    }
    if (!m_cp_on.load()) return;
    if (m_cp_land_t == std::chrono::steady_clock::time_point{}) m_cp_land_t = now;   // the fall or the wingsuit / parachute over
    if (now < m_cp_until && m_chute_probe.load()) return;
    m_cp_on = false;
    std::vector<std::pair<std::pair<void*, UINT>, ChuteDraw>> v;
    {
        std::lock_guard _{ m_cp_mtx };
        v.assign(m_cp_draws.begin(), m_cp_draws.end());
        m_cp_draws.clear();
    }
    const float land = std::chrono::duration<float>(m_cp_land_t - m_cp_t0).count();
    const float span = std::chrono::duration<float>(now - m_cp_t0).count();
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.frames > b.second.frames; });
    spdlog::info("[ChuteProbe] summary: {} distinct draws within 8 m over {:.1f} s, the fall or wingsuit / parachute over at {:.1f} s; the 15 drawn most:", v.size(), span, land);
    for (size_t i = 0; i < v.size() && i < 15; ++i) {
        const auto& d = v[i].second;
        spdlog::info("[ChuteProbe]   ps {} {} indices at ({:+.2f}, {:+.2f}, {:+.2f}){}: {} frames, seen {:.1f}-{:.1f} s{}: {}", v[i].first.first, v[i].first.second, d.off.x, d.off.y,
                     d.off.z, d.off.z >= 2.0f && d.off.z <= 8.0f ? " (2-8 m above)" : "", d.frames, d.first, d.last,
                     d.last < land - 0.2f ? " (gone before the end)" : d.last < land + 0.5f ? " (gone at the end)" : " (still drawn after)", d.names);
    }
}
