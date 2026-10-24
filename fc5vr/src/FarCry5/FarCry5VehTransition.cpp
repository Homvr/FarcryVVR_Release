// Getting in and out of vehicles, comfort (Homard 23 Oct, after the 12:04 headset run: "the camera movements were very
// unpleasant in a lot of moments, especially when transitioning between vehicles"). His log: each entry turned the view
// ~40 deg/s for 2 s, each exit added a seat-change settle and recentre 0.6 s before the real one, and bailing out of the
// plane turned the view ~180 deg in half a second. With vehentryfollow the view's heading was held on the game camera's
// during the animation (m_body_yaw_pending = camera - (body + head)): turning the head did not turn the view for 1-3 s.
//  - vehfollowhead on|off (default on, saved; F11 Comfort > Cinematics and vehicles): the body frame turns by the animation
//    camera's own turn since the settle started; the head still turns the view on top. Getting out, the exit ends with the
//    centre moved under the head (no turn of the view: on foot the aim loop brings the game camera to the head).
//  - vehfollowsnap on|off (default on, saved): that turn in snaps of the snap-turn angle (15-45 deg, at most one per 0.25 s),
//    like a snap turn, instead of a smooth artificial turn; the eyes stay within that angle of the camera the game culls with.
//  - vehseatguard on|off (default on, saved): a seat-change settle is not finished while no vehicle is under the camera
//    (update_vehicle); getting out has one settle, not two.
// Getting in still ends with one cut to the vehicle's front and a recentre (as before); the log line "[Vehicle] settle cut"
// gives its size.
#include "FarCry5Stereo.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

bool FarCry5Stereo::vehtrans_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "vehfollowhead") { m_veh_follow_head = on; persist = true; return true; }   // vehfollowhead on|off (saved)
    if (cmd == "vehfollowsnap") { m_veh_follow_snap = on; persist = true; return true; }   // vehfollowsnap on|off (saved)
    if (cmd == "vehseatguard") { m_veh_seat_guard = on; persist = true; return true; }     // vehseatguard on|off (saved)
    return false;
}

// A settle starts (getting in, a seat change, getting out): the follow takes its start at its first step.
void FarCry5Stereo::veh_follow_start()
{
    m_vf_init      = false;
    m_vf_turn      = 0.0f;
    m_vf_snaps     = 0;
    m_vf_seat_held = 0;
}

// One step of the view following the enter / exit animation (vehentryfollow), applied between pairs as m_body_yaw_pending.
void FarCry5Stereo::veh_follow_step(std::chrono::steady_clock::time_point now)
{
    if (!m_veh_follow_head.load()) {
        // As before 23 Oct: the view's heading held on the game camera's.
        m_body_yaw_pending = std::remainder(m_cine_game_yaw - (m_body_yaw + m_head_rel_yaw), glm::two_pi<float>());
        return;
    }
    if (!m_vf_init) {
        m_vf_init     = true;
        m_vf_body0    = m_body_yaw + m_body_yaw_pending;
        m_vf_cam_last = m_cine_game_yaw;
        m_vf_turn     = 0.0f;
        m_vf_snap_t   = now - std::chrono::seconds(1);
    }
    m_vf_turn += std::remainder(m_cine_game_yaw - m_vf_cam_last, glm::two_pi<float>());   // unwrapped: a turn past 180 deg is kept
    m_vf_cam_last = m_cine_game_yaw;
    const float d = std::remainder(m_vf_body0 + m_vf_turn - m_body_yaw, glm::two_pi<float>());
    if (!m_veh_follow_snap.load()) {
        m_body_yaw_pending = d;
        return;
    }
    const float step = glm::radians(std::clamp(m_snap_deg.load(), 15.0f, 45.0f));
    if (std::abs(d) >= step && now - m_vf_snap_t >= std::chrono::milliseconds(250)) {
        m_body_yaw_pending = d;   // the whole way to the animation's heading, in one cut
        m_vf_snap_t        = now;
        m_vf_snaps++;
        request_jump_reset();     // the view jumped: each eye's DLSS history starts over
    }
}

void FarCry5Stereo::draw_vehtrans_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool v = m_veh_follow_head.load();
    if (ImGui::Checkbox("Vehicles: getting in or out, the head still turns the view", &v)) {
        m_veh_follow_head = v;
        keep("vehfollowhead", v ? "vehfollowhead on" : "vehfollowhead off");
    }
    hint("On (default): during the enter / exit animation the view turns with the animation's camera and your head still turns it on top; getting out ends without a "
         "turn. Off: as before 23 Oct (the view held on the game's camera: turning the head did not turn the view during the animation, then a recentre).");
    v = m_veh_follow_snap.load();
    if (ImGui::Checkbox("Vehicles: the enter / exit animation's turn in snaps", &v)) {
        m_veh_follow_snap = v;
        keep("vehfollowsnap", v ? "vehfollowsnap on" : "vehfollowsnap off");
    }
    hint("On (default): the animation's turn comes in steps of your snap-turn angle, like a snap turn, instead of the world turning smoothly around you. Off: a smooth turn.");
    v = m_veh_seat_guard.load();
    if (ImGui::Checkbox("Vehicles: getting out, no extra recentre", &v)) {
        m_veh_seat_guard = v;
        keep("vehseatguard", v ? "vehseatguard on" : "vehseatguard off");
    }
    hint("Getting out first looks like a change of seat to the mod. On (default): that is not settled while no vehicle is under you, so getting out has one settle. "
         "Off: as before 23 Oct (a cut and a recentre 0.6 s before the exit's own).");
}
