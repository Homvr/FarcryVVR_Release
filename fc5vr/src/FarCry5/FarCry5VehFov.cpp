// vehfov: the game draws (and culls) vehicles with an 85-99 deg view while each eye needs ~108 deg: buildings and props
// pop at the sides of the view (Homard 14-19 Oct, cars, planes, parachutes). Parked in a vehicle, the mod looks for
// the float the game's view angle comes from (the fovscan machinery: candidates equal to the uploaded FOV, binary search
// by nudging half of them 2 % and watching the uploaded FOV move) and holds it at vehfov degrees while in that vehicle.
#include "FarCry5Stereo.h"
#include "FarCry5Lua.h"

#include <chrono>
#include <cmath>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

void FarCry5Stereo::veh_fov_tick()
{
    auto&       s    = m_fov_scan;
    const float want = m_veh_fov.load();
    // In the vehicle, any seat (a weapon taken out under a parachute counts as a seat change: that only pauses the search).
    const bool  in   = want > 0.0f && m_motion.load() && m_in_vehicle.load() && !m_menu_screen_on && m_player_views_last_frame != 0;
    if (!in) {
        if (m_vf_entry) {
            // Out of the vehicle (or switched off): the held address belongs to that vehicle's camera; let it go.
            if (m_vf_running) {
                fov_scan_abort();
                spdlog::info("[VehFov] scan stopped: left the vehicle");
            }
            if (m_vf_holding) spdlog::info("[VehFov] stopped holding the vehicle's view angle (left the vehicle)");
            if (m_vf_holding) fov_release(true);   // the game's own value back (it may be vehicle data shared by the next one)
            m_vf_entry = m_vf_running = m_vf_holding = false;
            m_vf_pass = m_vf_stable = 0;
        }
        return;
    }
    if (!m_vf_entry) {
        m_vf_entry = true;
        m_vf_pass = m_vf_stable = 0;
        if (s.state == 0) s.found = nullptr;   // a new vehicle: its own camera
        spdlog::info("[VehFov] in a vehicle: game view {:.1f} deg wide (FOV field {:.1f}); the view-angle search starts once the view angle holds still", m_game_hfov.load(), m_game_fov_deg);
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_veh_settle.load() > 0 && !m_vf_holding) return;   // getting in / changing seats: wait
    if (m_vf_holding) {
        if (!s.found) {
            spdlog::info("[VehFov] lost the held view angle");
            m_vf_holding = false;
            m_cull_fov   = 0.0f;
            return;
        }
        if (now - m_vf_log_t > std::chrono::seconds(2)) {
            m_vf_log_t = now;
            spdlog::info("[VehFov] holding {:.4g} ({}) at {}: game view now {:.1f} deg wide (FOV field {:.1f}), speed {:.1f} m/s", m_cull_fov.load(), m_vf_rad ? "radians" : "degrees",
                         (void*)s.found, m_game_hfov.load(), m_game_fov_deg, m_veh_speed);
        }
        return;
    }
    if (m_vf_running) {

        if (s.state != 0) return;
        m_vf_running = false;
        if (s.found) {
            m_cull_fov   = m_vf_rad ? glm::radians(want) : want;
            m_vf_holding = true;
            m_vf_log_t   = now;
            spdlog::info("[VehFov] pass {}: found the view angle at {}; holding it at {:.0f} deg", m_vf_pass, (void*)s.found, want);
        } else {
            spdlog::info("[VehFov] pass {}: not found", m_vf_pass);
        }
        return;
    }
    if (s.state != 0 || m_vf_pass >= 2) return;   // a manual fovscan is running, or both passes are done for this vehicle
    if (m_game_hfov.load() >= want - 1.0f) return;   // already as wide
    // The view angle steady for ~1 s (a parachute's is fixed; a car's follows its speed): nudges then show only our own changes.
    const float fov = m_game_fov_deg;
    m_vf_stable     = std::abs(fov - m_vf_last) < 0.01f ? m_vf_stable + 1 : 0;
    m_vf_last       = fov;
    if (m_vf_stable < 90) return;
    m_vf_stable = 0;
    // A: degrees near the camera; B: radians near the camera. Never anywhere in memory: nudging 874 unrelated floats equal to
    // 85 crashed the game in a plane (Homard 19 Oct 20:35, twice).
    m_vf_rad          = m_vf_pass == 1;
    s.any             = false;
    s.found           = nullptr;
    m_fov_scan_value  = m_vf_rad ? glm::radians(fov) : 0.0f;
    m_cull_fov        = 0.0f;
    s.state           = 1;
    m_vf_running      = true;
    m_vf_pass++;
    spdlog::info("[VehFov] pass {} ({}, {}): looking for the game's view angle {:.2f} deg (view {:.1f} deg wide, {:.1f} m/s)", m_vf_pass, m_vf_rad ? "radians" : "degrees",
                 s.any ? "anywhere" : "near the camera", fov, m_game_hfov.load(), m_veh_speed);
}

// vehplayerfov: the game's own SetPlayerFOV asked for 120 deg in a vehicle (no memory changed; see FarCry5Lua.cpp).
void FarCry5Stereo::veh_playerfov_tick()
{
    const bool on  = m_veh_playerfov.load() && m_motion.load() && !m_menu_screen_on && m_player_views_last_frame != 0;
    const bool veh = m_in_vehicle.load();
    const auto now = std::chrono::steady_clock::now();
    const float v  = m_game_hfov.load();
    auto ask = [&](const char* why) {
        m_vp_tries++;
        m_vp_t_ask = now;
        fc5::lua::queue_fov(120.0f, 0.3f, true);
        spdlog::info("[PlayerFov] {} with a {:.1f} deg game view: 120 deg asked (try {})", why, v, m_vp_tries);
    };
    // fovearly (Homard's 23 Oct 12:04 log, FOV audit): getting in, the ask used to wait for the entry to settle (85-91 deg culled for
    // ~2 s, the eyes looking down past its bottom); getting out, the exit animation narrowed the view again ~2 s later. Now: asked at
    // once on getting in or out, then checked every 0.3 s through the entry (until 3 s after it settled) and for 5 s after getting
    // out, and asked again while narrower than 110 deg. Settled in a vehicle: every 2 s while narrow (a parachute opens after the
    // fall, a seat change). On foot after the 5 s a narrower view is aiming down sights or a scope: left alone.
    if (veh != m_vp_in) {
        m_vp_in    = veh;
        m_vp_tries = 0;
        m_vp_until = now + std::chrono::seconds(veh ? 3 : 5);
        m_vp_t_ask = now - std::chrono::seconds(10);
        if (on && (!veh || v < 110.0f)) ask(veh ? "getting in" : "getting out");
        return;
    }
    if (!on) return;
    if (veh && m_veh_settle.load() > 0) m_vp_until = now + std::chrono::seconds(3);   // the entry animation: keep checking until it settled
    const bool  window = now < m_vp_until;
    const float since  = std::chrono::duration<float>(now - m_vp_t_ask).count();
    if (v >= 110.0f) {
        if (veh && !window) m_vp_tries = 0;   // wide: a later narrowing gets fresh tries
        return;
    }
    if (window && since >= 0.3f && m_vp_tries < 15) ask(veh ? (m_veh_settle.load() > 0 ? "getting in (entry animation)" : "just in a vehicle") : "just out of a vehicle");
    else if (veh && !window && since >= 2.0f && m_vp_tries < 3) ask("in a vehicle");
}
