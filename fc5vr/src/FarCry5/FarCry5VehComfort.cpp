// Car comfort (Homard 24 Oct: "In the car there are too many bumps and movement this is nauseating. Maybe can you also make
// it that the camera follows vehicle only does it for air vehicles?"). Since 14 Oct airfollow put the view on the vehicle's
// own attitude in every vehicle: each bump's pitch and roll tilted the whole world in the headset.
//  - groundfollow on|off (default off, saved; F11 Comfort > Cinematics and vehicles): airfollow's pitch and roll only once the
//    vehicle is seen flying, or under a parachute; cars, quads and boats keep the horizon level (as before 14 Oct: the view
//    turns with the vehicle's heading, the cab tilts around you). On: every vehicle, as before 24 Oct.
//  - Flying (veh_flying, latched until out of the vehicle; vehfly on|off|auto to force it): while piloting, a climb of over
//    2 m/s, steeper than 50 deg, for 1 s (a helicopter lifting off), the model tilted over 30 deg for 2 s
//    (banking, a steep climb), or 30 m gained within 5 s (a plane after take-off). The older m_veh_aircraft test (2.5 m/s
//    up or down, or 10 deg of roll, for 20 updates) fires in cars at speed (Homard's 12 and 14 Oct drives), so it is not used.
//  - vehbumps ("Car bump smoothing": the eyes' height through an alpha-beta filter while driving) was removed 24 Oct (Homard).
//  - vehvignette 0-1 (default 0 = off, saved): in a vehicle the picture's edges darken while the view turns faster than
//    15 deg/s, the vehicle speeds up, brakes or swerves past 2 m/s2, or jolts up and down past 3 m/s2 (FarCry5Fade.cpp).
#include "FarCry5Stereo.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
    float lp_alpha(float cutoff_hz, float dt)
    {
        const float tau = 1.0f / (glm::two_pi<float>() * std::max(0.05f, cutoff_hz));
        return 1.0f / (1.0f + tau / dt);
    }
}

bool FarCry5Stereo::vehcomfort_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool  on = arg == "on" || arg == "1" || arg == "true";
    const float fv = arg.empty() ? 0.0f : std::strtof(arg.c_str(), nullptr);
    if (cmd == "groundfollow") { m_ground_follow = on; persist = true; return true; }                          // groundfollow on|off (saved)
    if (cmd == "vehvignette") { m_veh_vignette = std::clamp(fv, 0.0f, 1.0f); persist = true; return true; }    // vehvignette 0-1 (saved)
    if (cmd == "vehfly") { m_veh_fly_force = arg == "auto" ? -1 : on ? 1 : 0; return true; }                  // vehfly on|off|auto (test)
    return false;
}

// Once per pair from update_vehicle, before it keeps this pair's position: whether the vehicle flies, and the motion the
// vignette answers to.
void FarCry5Stereo::veh_comfort_step(std::chrono::steady_clock::time_point now, const glm::vec3& pos, bool mesh_near)
{
    const float dt = m_veh_pos_valid ? std::chrono::duration<float>(now - m_veh_last_t).count() : 0.0f;
    if (!(dt > 0.0005f && dt < 0.25f) || glm::length(pos - m_veh_last_pos) > 20.0f) {
        m_vc_init = false;
        return;
    }
    const glm::vec3 vel = (pos - m_veh_last_pos) / dt;
    // The vehicle's tilt from level (pitch and roll together): its aim frame (the game camera less the head look, airfollow's
    // target, read in every vehicle while piloting with head look), else the model under the camera, else the camera's roll.
    float tilt = std::abs(m_cine_roll);
    if (m_air_target_valid) {
        const glm::vec3 up = glm::transpose(m_air_A) * glm::vec3{ 0.0f, 1.0f, 0.0f };
        tilt               = glm::degrees(std::acos(std::clamp(glm::normalize(up).z, -1.0f, 1.0f)));
    } else if (mesh_near && glm::length(m_veh_mesh_x) > 1e-3f && glm::length(m_veh_mesh_y) > 1e-3f) {
        const glm::vec3 up = glm::cross(glm::normalize(m_veh_mesh_x), glm::normalize(m_veh_mesh_y));
        if (glm::length(up) > 0.5f) tilt = glm::degrees(std::acos(std::clamp(std::abs(glm::normalize(up).z), 0.0f, 1.0f)));
    }
    m_vfly_tilt = tilt;
    if (!m_veh_flying && m_veh_driving.load() && !m_chute_vehicle) {
        const bool climb = vel.z > 2.0f && vel.z > 1.2f * m_veh_speed;   // steeper than 50 deg: no car climbs that
        m_vfly_climb_t   = climb ? m_vfly_climb_t + dt : std::max(0.0f, m_vfly_climb_t - dt);
        m_vfly_tilt_t    = tilt > 30.0f ? m_vfly_tilt_t + dt : std::max(0.0f, m_vfly_tilt_t - dt);
        m_vfly_zt += dt;
        if (m_vfly_zt >= 0.25f || m_vfly_zn == 0) {
            m_vfly_zt                = 0.0f;
            m_vfly_z[m_vfly_zn % 20] = pos.z;
            m_vfly_zn++;
        }
        float low = pos.z;
        for (int i = 0; i < std::min(m_vfly_zn, 20); ++i) low = std::min(low, m_vfly_z[i]);
        const char* why = m_vfly_climb_t > 1.0f ? "climbing with little speed over the ground" : m_vfly_tilt_t > 2.0f ? "tilted over 30 deg for 2 s"
                        : pos.z - low > 30.0f    ? "30 m gained within 5 s"
                                                 : nullptr;
        if (why) {
            m_veh_flying = true;
            spdlog::info("[VehComfort] flying ({}: up {:.1f} m/s, {:.1f} m/s over the ground, tilt {:.0f} deg, {:.0f} m above the last 5 s' lowest): the view follows its pitch and roll{}",
                         why, vel.z, m_veh_speed, tilt, pos.z - low, m_ground_follow.load() ? "" : " (groundfollow off: not before)");
        }
    }
    // The view's turn rate (its body frame: level or the vehicle's attitude) and the vehicle's acceleration, for the vignette.
    const glm::quat q = glm::normalize(glm::quat_cast(m_mo.body_rot));
    if (!m_vc_init) {
        m_vc_init = true;
        m_vc_vel  = vel;
        m_vc_acc  = glm::vec3{ 0.0f };
        m_vc_q    = q;
        m_vc_rate = 0.0f;
        return;
    }
    glm::quat dq = q * glm::inverse(m_vc_q);
    if (dq.w < 0.0f) dq = -dq;
    const float turn = glm::degrees(2.0f * std::acos(std::clamp(dq.w, -1.0f, 1.0f)));
    m_vc_q           = q;
    if (turn < 20.0f) m_vc_rate += lp_alpha(4.0f, dt) * (turn / dt - m_vc_rate);   // over 20 deg in one pair: a cut, not a turn
    const glm::vec3 v_prev = m_vc_vel;
    m_vc_vel += lp_alpha(8.0f, dt) * (vel - m_vc_vel);
    m_vc_acc += lp_alpha(4.0f, dt) * ((m_vc_vel - v_prev) / dt - m_vc_acc);
    const float acc_h = glm::length(glm::vec2{ m_vc_acc.x, m_vc_acc.y });
    m_vig_target      = std::clamp(std::max({ (m_vc_rate - 15.0f) / 45.0f, (acc_h - 2.0f) / 6.0f, (std::abs(m_vc_acc.z) - 3.0f) / 10.0f }), 0.0f, 1.0f);
    static int beat = 0;
    if (++beat % 450 == 0)
        spdlog::info("[VehComfort] {}: flying {}{}, tilt {:.0f} deg; view turn {:.0f} deg/s, accel {:.1f} across / {:.1f} up m/s2, vignette {:.2f} -> {:.2f} (vehvignette {:.2f}); "
                     "groundfollow {}, airfollow blend {:.2f}",
                     m_veh_driving.load() ? "driving" : "riding", veh_flying(), m_veh_fly_force.load() >= 0 ? " (forced)" : "", tilt, m_vc_rate, acc_h, m_vc_acc.z, m_vig_target,
                     m_vig_level, m_veh_vignette.load(), m_ground_follow.load() ? "on" : "off", m_air_blend);
}

// F11 Comfort > Cinematics and vehicles, under "In vehicles, the view" (groundfollow is one of its choices since 24 Oct).
void FarCry5Stereo::draw_vehcomfort_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                         const std::function<bool(const char*, float&, float, float, const char*, float)>& slider)
{
    float g = m_veh_vignette.load();
    if (slider("Vehicle comfort vignette", g, 0.0f, 1.0f, g <= 0.001f ? "off" : "%.2f", 0.05f)) {
        m_veh_vignette = g;
        keep("vehvignette", fmt::format("vehvignette {:.2f}", g));
    }
    hint("In a vehicle, the edges of the picture darken while it turns, speeds up, brakes or bumps hard, and clear again when the ride is calm. More = a smaller clear area. "
         "0 = off (default).");
}
