// Knocked down: the view stays inside the game's view (downview; Homard 30 Oct 09:10: "while down and getting up there is visible
// culling, and it is jarring"). His log 09:10:35-49: knocked down, downedfoot kept him on foot (right), but the game camera was the
// down animation's (it ignores the mouse), so the eyes were 50-81 deg off it on average (yaw up to +116, pitch +30-60) and the game culled
// what the eyes saw ("[Cull] ... worst 90 deg"); getting up (09:10:46.8-47.3) the eyes' base stepped 15-96 cm per pair ([ViewJump], the
// body root 528+ frames old, viewroot weight 0).
//  - downview on|off (default on, saved; F11 Comfort > Cinematics and vehicles): the game's IsPawnDown for the player (FarCry5Lua.cpp,
//    in vehscript's 150 ms poll on the script thread, whatever gamestate and vehscript are; a fault turns downview off for the session).
//    While down and for 1.5 s after it ends:
//      the view's heading (body + head) is steered to the game camera's, like cinesteer: a camera jump (turned over 30 deg or moved over
//      1.5 m in one pair) is taken at once (one cut, like a snap turn); past 15 deg it is eased round (about 95% in 0.5 s, at most 90 deg/s)
//      until within 3 deg, then held, so the head turns freely within 15 deg. The horizon stays level (the pitch is the head's).
//      the eyes' base is eased to the game camera's position instead of stepping: its steady motion is carried (under 10 cm a pair), the
//      rest closed in about 0.3 s, at most 4 cm a pair. After the 1.5 s it is eased back the same way to the normal on-foot view, then
//      handed back (no jump).
//    While down (not the 1.5 s after) the aim loop sends nothing: the game ignores the mouse then, and the steer must not chase a camera
//    the aim moves (it would also count as "the camera is not following the mouse" and switch to the stick aim).
//  - downforce on|off (test, unsaved): fakes the down state (off: the game's again), to exercise it standing in the simulator (the aim
//    then holds still, so the game camera stays put while the head turns).
// Log: "[Down] downview: down ...", "... getting up ...", "... up, back to the body view after N s"; the per-second [Cull] line is
// written every second while active and names downview. Off: as before.
#include "FarCry5Stereo.h"
#include "FarCry5Lua.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace
{
    float wrap_pi(float a) { return std::remainder(a, glm::two_pi<float>()); }
}

bool FarCry5Stereo::downview_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "downview") {   // downview on|off (saved)
        m_down_view = on;
        if (!on && m_dv_active) down_view_release("downview off");
        persist = true;
        return true;
    }
    if (cmd == "downforce") {   // downforce on|off (test, unsaved)
        m_down_force = on ? 1 : -1;
        spdlog::info("[Down] downforce {}: {}", on ? "on" : "off", on ? "the down state faked (the aim holds still)" : "the game's down state again");
        return true;
    }
    return false;
}

void FarCry5Stereo::down_view_release(const char* why)
{
    const float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_dv_t0).count();
    spdlog::info("[Down] downview: up, back to the body view after {:.1f} s ({}; eyes vs the game camera's heading {:.1f} deg on average, worst {:.0f}, "
                 "{:+.0f} at the start; {} cuts, {} eased steers; the eyes' base corrected at most {:.1f} cm a pair)",
                 since, why, m_dv_n ? m_dv_dy_sum / float(m_dv_n) : 0.0f, m_dv_dy_max, m_dv_dy_start, m_dv_cuts, m_dv_steers, m_dv_corr_max * 100.0f);
    m_dv_active   = false;
    m_dv_down     = false;
    m_dv_steering = false;
    m_dv_pos_ok   = false;
    m_dv_off      = glm::vec3{ 0.0f };
}

// Once per stereo pair, after update_vehicle (the present thread): the down state and the view's heading.
void FarCry5Stereo::update_downview()
{
    using clk       = std::chrono::steady_clock;
    const auto now  = clk::now();
    const int  gd   = fc5::lua::pawn_down();
    const int  gst  = fc5::lua::pawn_stance();   // 4 = downed (with vehscript on)
    const bool down = m_down_view.load() && (m_down_force.load() == 1 || (m_down_force.load() < 0 && (gd == 1 || gst == 4)));
    if (m_dv_active && (!m_down_view.load() || m_in_vehicle.load() || m_cine_on)) {
        down_view_release(!m_down_view.load() ? "downview off" : m_in_vehicle.load() ? "in a vehicle" : "a cutscene");
        return;
    }
    const float eyes = m_body_yaw + m_body_yaw_pending + m_head_rel_yaw;
    if (down && !m_dv_down && !m_in_vehicle.load() && !m_cine_on) {
        m_dv_down = true;
        if (!m_dv_active) {
            m_dv_active   = true;
            m_dv_t0       = now;
            m_dv_pos_ok   = false;   // the eyes' base starts from where it is (down_view_base)
            m_dv_steering = false;
            m_dv_yaw_ok   = false;
            m_dv_cuts = m_dv_steers = m_dv_n = m_dv_cut_logs = 0;
            m_dv_dy_sum = m_dv_dy_max = m_dv_corr_max = 0.0f;
            m_dv_dy_start = glm::degrees(wrap_pi(m_cine_game_yaw - eyes));
        }
        spdlog::info("[Down] downview: down ({}), the view follows the game camera's heading (eyes {:+.0f} deg off it now, the camera pitched {:+.0f}); "
                     "the eyes' base eased to the game camera; the aim holds still",
                     m_down_force.load() == 1 ? "downforce" : gd == 1 ? "the game's IsPawnDown" : "the game's stance 4", glm::degrees(wrap_pi(m_cine_game_yaw - eyes)), m_cine_pitch);
    } else if (!down && m_dv_down) {
        m_dv_down = false;
        m_dv_up_t = now;
        spdlog::info("[Down] downview: getting up after {:.1f} s down: the view follows the game camera 1.5 s more, then eases back to the body view",
                     std::chrono::duration<float>(now - m_dv_t0).count());
    }
    if (!m_dv_active) return;
    const bool follow = m_dv_down || now - m_dv_up_t < std::chrono::milliseconds(1500);
    const bool usable = m_motion.load() && m_body_valid && !m_menu_screen_on && m_player_views_last_frame != 0;
    const glm::vec3 gcam{ m_game_cam_pos[0], m_game_cam_pos[1], m_game_cam_pos[2] };
    if (!follow || !usable) {
        m_dv_steering = false;
        m_dv_yaw_ok   = false;
        m_dv_t        = now;
        return;
    }
    const float dt   = m_dv_yaw_ok ? std::clamp(std::chrono::duration<float>(now - m_dv_t).count(), 0.0f, 0.1f) : 0.0f;
    const float dy   = wrap_pi(m_cine_game_yaw - eyes);
    const float turn = wrap_pi(m_cine_game_yaw - m_dv_prev_yaw);
    const bool  cut  = m_dv_yaw_ok && (glm::length(gcam - m_dv_prev_gcam) > 1.5f || (std::abs(m_cine_pitch) < 75.0f && std::abs(turn) > glm::radians(30.0f)));
    m_dv_t           = now;
    m_dv_prev_yaw    = m_cine_game_yaw;
    m_dv_prev_gcam   = gcam;
    m_dv_n++;
    m_dv_dy_sum += std::abs(glm::degrees(dy));
    m_dv_dy_max  = std::max(m_dv_dy_max, std::abs(glm::degrees(dy)));
    if (!m_dv_yaw_ok || cut) {
        // The first step (a fresh start) and a camera jump: the whole way at once, one cut like a snap turn.
        if (std::abs(dy) > glm::radians(3.0f)) {
            m_body_yaw_pending += dy;
            request_jump_reset();   // the view jumped: each eye's DLSS history starts over
            m_dv_cuts++;
            if (m_dv_cut_logs++ < 10)
                spdlog::info("[Down] downview: cut, the view {:+.0f} deg onto the game camera's heading ({})", glm::degrees(dy),
                             cut ? fmt::format("the camera turned {:.0f} deg in one step", glm::degrees(turn)) : std::string{ "start" });
        }
        m_dv_steering = false;
        m_dv_yaw_ok   = true;
        return;
    }
    if (!m_dv_steering && std::abs(dy) > glm::radians(15.0f)) m_dv_steering = true;
    if (m_dv_steering) {
        // Exponential: exp(-3) = 5% left after 0.5 s; the step capped at 90 deg/s.
        const float k    = 1.0f - std::exp(-3.0f * dt / 0.5f);
        const float cap  = glm::radians(90.0f) * dt;
        const float step = std::clamp(dy * k, -cap, cap);
        m_body_yaw_pending += step;
        if (std::abs(dy - step) < glm::radians(3.0f)) {
            m_dv_steering = false;
            m_dv_steers++;
        }
    }
}

// The eyes' base (after viewpivot, walkbob and smoothmotion; before gunanchor) while downview is active: eased to the game camera while
// down and 1.5 s after, then back to the normal base; one offset per pair, the same for the weapon and the world uploads.
glm::vec3 FarCry5Stereo::down_view_base(int frame, const glm::vec3& cam, const glm::vec3& base_cam)
{
    if (!m_dv_active) return base_cam;
    const int pair = afw_mode() ? frame : (frame & ~1);
    if (pair == m_dv_pair && m_dv_pos_ok) return base_cam + m_dv_off;
    m_dv_pair      = pair;
    const auto now = std::chrono::steady_clock::now();
    const bool follow = m_dv_down || now - m_dv_up_t < std::chrono::milliseconds(1500);
    const glm::vec3 target = follow ? cam : base_cam;
    if (!m_dv_pos_ok) {
        m_dv_pos_ok    = true;
        m_dv_pos       = base_cam;
        m_dv_prev_cam  = cam;
        m_dv_prev_base = base_cam;
        m_dv_pos_t     = now;
    }
    const float dt = std::clamp(std::chrono::duration<float>(now - m_dv_pos_t).count(), 0.0f, 0.1f);
    m_dv_pos_t     = now;
    // The target's own steady motion is carried; a jump of it (over 10 cm a pair) is eased like the rest of the gap.
    const glm::vec3 tstep = target - (follow ? m_dv_prev_cam : m_dv_prev_base);
    if (glm::length(tstep) < 0.10f) m_dv_pos += tstep;
    m_dv_prev_cam  = cam;
    m_dv_prev_base = base_cam;
    glm::vec3 gap  = target - m_dv_pos;
    if (glm::length(gap) > 3.0f) {
        m_dv_pos = target;   // a teleport (a respawn, a load): no easing across it
        gap      = glm::vec3{ 0.0f };
    }
    glm::vec3   corr = gap * (1.0f - std::exp(-3.0f * dt / 0.3f));   // ~95% in 0.3 s
    const float cl   = glm::length(corr);
    if (cl > 0.04f) corr *= 0.04f / cl;                               // at most 4 cm of correction a pair
    m_dv_pos += corr;
    m_dv_corr_max = std::max(m_dv_corr_max, glm::length(corr));
    m_dv_off      = m_dv_pos - base_cam;
    if (!follow && glm::length(m_dv_off) < 0.01f) {
        const glm::vec3 out = base_cam + m_dv_off;
        down_view_release("eased back");
        return out;
    }
    return base_cam + m_dv_off;
}

void FarCry5Stereo::draw_downview_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool v = m_down_view.load();
    if (ImGui::Checkbox("Knocked down: the view stays with the game's camera##downview", &v)) {
        m_down_view = v;
        if (!v && m_dv_active) down_view_release("downview off");
        keep("downview", v ? "downview on" : "downview off");
    }
    hint("downview. On (default; Homard 30 Oct: \"while down and getting up there is visible culling, and it is jarring\"): knocked down, the game's camera is "
         "the animation's and the game draws only what it sees. Your view now turns to face where it looks (past 15 deg, eased; at once on a camera cut), the "
         "horizon kept level, and your position follows it smoothly; getting up, it eases back to the normal view without a jump. Off: as before.");
}
