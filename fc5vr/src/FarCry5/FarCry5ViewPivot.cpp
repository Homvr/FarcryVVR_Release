// View pivot (Homard 23 Oct 10:38, headset: "the walk is straight, but the camera still rotate/tilt a bit when moving the weapon
// sideway"). The eyes stand on the game camera's position. Since fireready (20 Oct) the game's aim follows the gun whenever a
// weapon is in hand, and the game camera sits on the player's neck, a few cm in front of the body root along the aim: turning
// the gun swung the eyes around the root with it, while the head held still.
//  - viewpivot on|off (default on, saved; F11 Motion control > View "The view stays put while the gun turns"): the camera's
//    horizontal offset from the root, taken in the aim's own heading at the update the root was seen, is turned from the aim's
//    heading now to the head's. The eyes stand where the camera would be with the aim on the head (as unarmed, where the aim
//    loop keeps it there), and the drawn gun and hands move with them. On foot only; not while the steady view holds its own
//    base. The shots keep the game camera (a parallel offset of a few cm).
//  - viewlog N (test): N presents of the drawn eye (position, yaw / pitch / roll), the game camera, its offset from the root in
//    the aim's frame, the aim, the Extended View offset and the shift, one line per world upload.
#include "FarCry5Stereo.h"

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace
{
    float     yaw_of(const glm::vec3& f) { return std::atan2(-f.x, f.y); }   // as FarCry5Stereo.cpp: 0 = +y, counter-clockwise
    glm::vec2 rot2(float a, const glm::vec2& v) { return { std::cos(a) * v.x - std::sin(a) * v.y, std::sin(a) * v.x + std::cos(a) * v.y }; }
    bool      has_heading(const glm::vec3& f) { return std::sqrt(f.x * f.x + f.y * f.y) > 0.25f * std::max(1e-6f, glm::length(f)); }
}

bool FarCry5Stereo::pivot_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "viewpivot") { m_view_pivot = on; persist = true; return true; }   // viewpivot on|off (saved)
    if (cmd == "viewpivotlag") { m_view_pivot_lag = std::atoi(arg.c_str()); return true; }   // viewpivotlag K (test): updates back (2)
    if (cmd == "viewlog") { m_view_log = arg.empty() ? 600 : std::max(0, std::atoi(arg.c_str())); return true; }   // viewlog N (test)
    return false;
}

// The aim (the game camera with the Extended View offset it carries taken out) of this update: its heading and pitch.
void FarCry5Stereo::pivot_aim(float& yaw, float& pitch) const
{
    const glm::mat3 v2w_g = glm::transpose(m_mo.game_rot);
    glm::mat3       v2w_a = v2w_g;
    if (m_mo.ev_ok && (m_mo.ev_oy != 0.0f || m_mo.ev_op != 0.0f)) {
        // As airfollow: camera = A * Ry(yaw) * Rx(pitch).
        const glm::mat3 ry{ glm::rotate(glm::mat4{ 1.0f }, -m_mo.ev_oy, glm::vec3{ 0, 1, 0 }) };
        const glm::mat3 rx{ glm::rotate(glm::mat4{ 1.0f }, -m_mo.ev_op, glm::vec3{ 1, 0, 0 }) };
        v2w_a = v2w_g * rx * ry;
    }
    const glm::vec3 f = glm::normalize(v2w_a * glm::vec3{ 0, 0, -1 });
    yaw               = yaw_of(f);
    pitch             = std::asin(std::clamp(f.z, -1.0f, 1.0f));
}

// The world camera upload of this frame (after update_motion): its aim, for the root seen later in the frame and for the lag.
void FarCry5Stereo::pivot_note_aim(int frame)
{
    auto& a = m_pivot_aim[frame & 31];
    a.frame = frame;
    pivot_aim(a.yaw, a.pitch);
}

bool FarCry5Stereo::pivot_aim_at(int frame, float& yaw) const
{
    const auto& a = m_pivot_aim[frame & 31];
    if (a.frame != frame) return false;
    yaw = a.yaw;
    return true;
}

// The body root seen (body_hide): the camera's horizontal offset from it in the same update, as a heading and a length.
void FarCry5Stereo::pivot_note_root(int frame, const glm::vec3& camoff)
{
    const glm::vec2 h{ camoff.x, camoff.y };
    const float     len = glm::length(h);
    if (len > 0.25f || len < 1e-3f || std::abs(camoff.z) > 2.5f) return;   // not the player's root (a vault, a ladder, a takedown)
    m_pivot_off       = camoff;
    m_pivot_psi       = yaw_of(glm::vec3{ h, 0.0f });
    m_pivot_len       = len;
    m_pivot_off_frame = frame;
}

// The shift of the eyes' base from the game camera (zero when off or not on foot), once per stereo pair. The camera's heading
// from the root now: the one measured at the last root (an update or two back) turned by the aim's turn since, both taken
// viewpivotlag updates earlier (the neck follows the aim about 2 updates late: simulator 23 Oct, 2 deg rms vs 4-8 from the aim).
glm::vec3 FarCry5Stereo::view_pivot_shift(int frame)
{
    const int pair = afw_mode() ? frame : (frame & ~1);
    if (pair == m_pivot_pair) { m_pivot_applied = m_pivot_ok; return m_pivot_shift; }
    m_pivot_pair  = pair;
    m_pivot_shift = glm::vec3{ 0.0f };
    pivot_aim(m_pivot_aim_yaw, m_pivot_aim_pitch);
    if (has_heading(m_mo.head_world)) m_pivot_head_yaw = yaw_of(m_mo.head_world);
    const int  s  = m_pivot_off_frame;
    const bool est = m_mo.valid && s >= 0 && pair - s <= 90 && pair >= s;   // estimated with viewpivot off too (viewlog)
    const bool ok  = est && m_view_pivot.load() && !m_in_vehicle.load() && !m_cine_on && !m_menu_screen_on;
    if (est) {
        const int step = afw_mode() ? 1 : 2, back = step * std::clamp(m_view_pivot_lag.load(), 0, 8);
        float     a = 0, b = 0, turn = 0;
        if (pivot_aim_at(pair - back, a) && pivot_aim_at(s - back, b)) turn = std::remainder(a - b, glm::two_pi<float>());
        else if (pivot_aim_at(s, b)) turn = std::remainder(m_pivot_aim_yaw - b, glm::two_pi<float>());
        else turn = std::remainder(m_pivot_aim_yaw - m_pivot_psi, glm::two_pi<float>());   // no history: the aim now
        m_pivot_psi_now   = m_pivot_psi + turn;
        auto fwd          = [](float y) { return glm::vec2{ -std::sin(y), std::cos(y) }; };
        const glm::vec2 d = m_pivot_len * (fwd(m_pivot_head_yaw) - fwd(m_pivot_psi_now));
        if (ok) m_pivot_shift = glm::vec3{ d, 0.0f };
    }
    m_pivot_ok      = ok;
    m_pivot_applied = ok;
    return m_pivot_shift;
}

void FarCry5Stereo::view_log_line(int frame, const glm::mat3& eye_rot, const glm::vec3& eye_cam, const glm::vec3& cam, const glm::vec3& base_cam)
{
    if (m_view_log.load() <= 0 || m_view_log_frame == frame) return;
    m_view_log_frame = frame;
    m_view_log--;
    const glm::mat4 er{ eye_rot };
    const glm::vec3 ef = -glm::vec3{ glm::row(er, 2) }, eright = glm::vec3{ glm::row(er, 0) };
    const glm::mat4 gr{ m_mo.game_rot };
    const glm::vec3 gf = -glm::vec3{ glm::row(gr, 2) }, gright = glm::vec3{ glm::row(gr, 0) };
    const glm::vec3 gun = m_mo.ctrl_rot_world * glm::vec3{ 0.0f, 0.0f, -1.0f };
    float           ay = 0, ap = 0;
    pivot_aim(ay, ap);
    auto deg = [](float r) { return glm::degrees(r); };
    auto asn = [](float s) { return glm::degrees(std::asin(std::clamp(s, -1.0f, 1.0f))); };
    spdlog::info("[ViewLog] frame {} eye {:.4f} {:.4f} {:.4f} ypr {:.3f} {:.3f} {:.3f} | cam {:.4f} {:.4f} {:.4f} ypr {:.3f} {:.3f} {:.3f} | aim {:.3f} {:.3f} ev {:.3f} {:.3f} | "
                 "head {:.3f} gun {:.2f} | off {:.4f} {:.4f} {:.4f} age {} | shift {:.4f} {:.4f} {:.4f} | pivot {} split {}",
                 frame, eye_cam.x, eye_cam.y, eye_cam.z, deg(yaw_of(ef)), asn(ef.z), asn(eright.z), cam.x, cam.y, cam.z, deg(yaw_of(gf)), asn(gf.z), asn(gright.z), deg(ay), deg(ap),
                 m_mo.ev_ok ? deg(m_mo.ev_oy) : 0.0f, m_mo.ev_ok ? deg(m_mo.ev_op) : 0.0f, deg(yaw_of(m_mo.head_world)), deg(yaw_of(gun)), m_pivot_len, deg(m_pivot_psi_now),
                 m_pivot_off.z, m_pivot_off_frame >= 0 ? frame - m_pivot_off_frame : -1, base_cam.x - cam.x, base_cam.y - cam.y, base_cam.z - cam.z, m_pivot_applied ? "on" : "off",
                 m_split_on ? "on" : "off");
}

// F11 Debug > Head tracking and view (Homard 24 Oct; was Motion control > View).
void FarCry5Stereo::draw_pivot_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool v = m_view_pivot.load();
    if (ImGui::Checkbox("The view stays put while the gun turns", &v)) {
        m_view_pivot = v;
        keep("viewpivot", v ? "viewpivot on" : "viewpivot off");
    }
    hint("The game's camera sits on the player's neck and turns with the game's aim, which follows the gun. On (default): the eyes stand where the camera "
         "would be with the aim on your head, so turning the gun no longer swings the view. Off: as before 23 Oct.");
}
