// View pivot (Homard 23 Oct 10:38, headset: "the walk is straight, but the camera still rotate/tilt a bit when moving the weapon
// sideway"). The eyes stand on the game camera's position. Since fireready (20 Oct) the game's aim follows the gun whenever a
// weapon is in hand, and the game camera sits on the player's neck, a few cm in front of the body root along the aim: turning
// the gun swung the eyes around the root with it, while the head held still.
//  - viewpivot on|off (default on, saved; F11 Motion control > View "The view stays put while the gun turns"): the camera's
//    horizontal offset from the root, taken in the aim's own heading at the update the root was seen, is turned from the aim's
//    heading now to the head's. The eyes stand where the camera would be with the aim on the head (as unarmed, where the aim
//    loop keeps it there), and the drawn gun and hands move with them. On foot only; not while the steady view holds its own
//    base. The shots keep the game camera (a parallel offset of a few cm).
//  - viewroot on|off (default on, saved; F11 Debug > Head tracking and view, 27 Oct): the eyes' base across the ground on the
//    body root plus the camera's offset from it with the aim level, turned to the head's heading (see view_pivot_shift).
//  - viewrootsmooth on|off (default on, saved; same tab; Homard 27 Oct 20:26: "when entering a building there was a lot of camera
//    movement"): viewroot used only while viewpivot's offset was fresh, and that offset is refused past 25 cm from the root (the
//    camera stood 26-31 cm from the root for seconds in his 21:09 log, most likely the game's aim pitched down with a gun held
//    low). About a second later the eyes snapped 20+ cm onto the game camera, and back once it came under 25 cm: little to see
//    outdoors, plain next to walls a metre away. Now viewroot needs only its own fresh root, allows a shift up to 45 cm (was 35), and a fall back to
//    the game camera or the way back onto the body is eased over 0.15 s. Off: as viewroot was. One [View] line per 5 s while used.
//  - rootsteady on|off (default on, saved; same tab; Homard 29 Oct 11:15 "quite judder movement when going uphill"): the body root
//    for viewroot and walkbob from its last three samples (root_steady_at), so samples taken with long and short steps in turn
//    no longer make the eyes step long and short in turn.
//  - smoothmotion on|off (default on, saved; F11 Comfort > Smooth motion; Homard 29 Oct "too juddery and janky for vr"): on foot,
//    the eyes' base through one tracking filter on the headset's display clock (motion_smooth); smoothtau S (test, 0.06 s).
//    One [Motion] smoothmotion line per 5 s compares the base before and after it. Since 29 Oct 12:49 ("a bit more sluggish"): the
//    body only (the offset turned with the head is added after it) and quick on starts and stops; smoothquick S (test, 0.025 s).
//  - bodyview on|off (default on, saved; F11 Comfort > Smooth motion "Body-based view"; Homard 29 Oct 13:49 "did you also brought all
//    the different parameters into the unified system"): viewpivot, viewroot, viewrootsmooth, rootsteady and walkbob in one switch.
//    Those five stay as test commands, no longer saved or in F11; viewpivotlag and walkbob's bob test knobs are retired.
//  - viewjumpcm N (log only, saved, default 4, 0 = off; 29 Oct, Homard's "smaller hiccups ... like rubberbanding"): [ViewJump] lines for
//    pairs whose eyes' base steps off a steady motion or whose horizon tilts (view_jump_note).
//  - viewlog N (test): N presents of the drawn eye (position, yaw / pitch / roll), the game camera, its offset from the root in
//    the aim's frame, the aim, the Extended View offset and the shift, one line per world upload.
#include "FarCry5Stereo.h"

#include <mods/vr/runtimes/OpenXR.hpp>

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
    if (cmd == "bodyview") {   // bodyview on|off (saved): the five below together
        m_body_view = on;
        m_view_pivot = m_view_root = m_vr_smooth = m_root_steady = on;
        m_no_bob     = on;
        persist      = true;
        return true;
    }
    // Part of bodyview since 29 Oct: test commands, no longer saved (an old settings line is still read, then dropped at the next save).
    if (cmd == "viewpivot") { m_view_pivot = on; return true; }
    if (cmd == "viewroot") { m_view_root = on; return true; }
    if (cmd == "viewrootsmooth") { m_vr_smooth = on; return true; }
    if (cmd == "rootsteady") { m_root_steady = on; return true; }
    if (cmd == "smoothmotion") { m_smooth_motion = on; persist = true; return true; }   // smoothmotion on|off (saved)
    if (cmd == "smoothtau") { m_smooth_tau = std::clamp(float(std::atof(arg.c_str())), 0.01f, 0.5f); return true; }   // smoothtau S (test)
    if (cmd == "smoothquick") { m_smooth_quick = std::clamp(float(std::atof(arg.c_str())), 0.005f, 0.5f); return true; }   // smoothquick S (test)
    if (cmd == "aimease") { m_aim_ease_ms = std::clamp(std::atoi(arg.c_str()), 0, 1500); persist = true; return true; }   // aimease ms (saved)
    if (cmd == "viewlog") { m_view_log = arg.empty() ? 600 : std::max(0, std::atoi(arg.c_str())); return true; }   // viewlog N (test)
    if (cmd == "viewjumpcm") { m_vj_cm = std::clamp(float(std::atof(arg.c_str())), 0.0f, 100.0f); persist = true; return true; }   // viewjumpcm N (saved, 0 = off)
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
    // viewroot: the offset with the aim level and still (within 8 deg of level, under 1 deg of turn over the last 4 updates), in the
    // aim's heading frame: forward and right of the root.
    const auto& a = m_pivot_aim[frame & 31];
    const auto& o = m_pivot_aim[(frame - 8) & 31];
    if (a.frame == frame && o.frame == frame - 8 && std::abs(a.pitch) < glm::radians(8.0f) && std::abs(std::remainder(a.yaw - o.yaw, glm::two_pi<float>())) < glm::radians(1.0f) &&
        std::abs(a.pitch - o.pitch) < glm::radians(1.0f)) {
        const glm::vec2 f{ -std::sin(a.yaw), std::cos(a.yaw) }, r{ std::cos(a.yaw), std::sin(a.yaw) };
        const float     fw = glm::dot(h, f), rt = glm::dot(h, r);
        if (!m_vr_ref_ok) {
            m_vr_ref_fwd   = fw;
            m_vr_ref_right = rt;
            m_vr_ref_ok    = true;
            spdlog::info("[View] viewroot: the camera stands {:.1f} cm ahead and {:.1f} cm right of the body root with the aim level", fw * 100.0f, rt * 100.0f);
        } else {
            m_vr_ref_fwd   = 0.95f * m_vr_ref_fwd + 0.05f * fw;
            m_vr_ref_right = 0.95f * m_vr_ref_right + 0.05f * rt;
        }
    }
}

// The shift of the eyes' base from the game camera (zero when off or not on foot), once per stereo pair. The camera's heading
// from the root now: the one measured at the last root (an update or two back) turned by the aim's turn since, both taken
// viewpivotlag updates earlier (the neck follows the aim about 2 updates late: simulator 23 Oct, 2 deg rms vs 4-8 from the aim).
glm::vec3 FarCry5Stereo::view_pivot_shift(int frame, const glm::vec3& cam)
{
    const int pair = afw_mode() ? frame : (frame & ~1);
    if (pair == m_pivot_pair) { m_pivot_applied = m_pivot_ok; return m_pivot_shift; }
    const auto  now = std::chrono::steady_clock::now();
    const float dt  = m_pivot_pair >= 0 ? std::chrono::duration<float>(now - m_vr_t).count() : 1.0f;
    m_vr_t        = now;
    m_pivot_pair  = pair;
    m_pivot_shift = glm::vec3{ 0.0f };
    m_ms_head     = glm::vec3{ 0.0f };
    pivot_aim(m_pivot_aim_yaw, m_pivot_aim_pitch);
    if (has_heading(m_mo.head_world)) m_pivot_head_yaw = yaw_of(m_mo.head_world);
    const int  s    = m_pivot_off_frame;
    const bool est  = m_mo.valid && s >= 0 && pair - s <= 90 && pair >= s;   // estimated with viewpivot off too (viewlog)
    const bool foot = m_mo.valid && m_view_pivot.load() && !m_in_vehicle.load() && !m_cine_on && !m_menu_screen_on;
    const bool ok   = est && foot;
    const bool smooth = m_vr_smooth.load();
    if (est) {
        const int step = afw_mode() ? 1 : 2, back = step * std::clamp(m_view_pivot_lag.load(), 0, 8);
        float     a = 0, b = 0, turn = 0;
        if (pivot_aim_at(pair - back, a) && pivot_aim_at(s - back, b)) turn = std::remainder(a - b, glm::two_pi<float>());
        else if (pivot_aim_at(s, b)) turn = std::remainder(m_pivot_aim_yaw - b, glm::two_pi<float>());
        else turn = std::remainder(m_pivot_aim_yaw - m_pivot_psi, glm::two_pi<float>());   // no history: the aim now
        m_pivot_psi_now   = m_pivot_psi + turn;
        auto fwd          = [](float y) { return glm::vec2{ -std::sin(y), std::cos(y) }; };
        const glm::vec2 d = m_pivot_len * (fwd(m_pivot_head_yaw) - fwd(m_pivot_psi_now));
        if (ok) {
            m_pivot_shift = glm::vec3{ d, 0.0f };
            m_ms_head     = glm::vec3{ m_pivot_len * fwd(m_pivot_head_yaw), 0.0f };   // the camera's offset from the neck, on the head's heading
        }
    }
    // viewroot: the root now (the last one moved on by its own pace) plus the level offset turned to the head's heading.
    // viewrootsmooth: on foot with a fresh root, whether or not viewpivot's offset is (it is refused past 25 cm from the root).
    const int  rf    = m_body_root_frame, pf = m_body_root_prev_frame;
    const bool fresh = rf >= 0 && pair - rf >= 0 && pair - rf <= 6 && pf < rf && rf - pf <= 8;
    bool       used  = false;
    glm::vec2  sh{ 0.0f }, hd{ 0.0f };
    if ((smooth ? foot : ok) && m_view_root.load() && m_vr_ref_ok) {
        if (fresh) {
            const glm::vec3 v    = (m_body_root - m_body_root_prev) / float(rf - pf);   // per frame
            glm::vec3       root = m_body_root + v * float(pair - rf);
            root_steady_at(float(pair), root);   // rootsteady: from the last three
            const float     hy   = m_pivot_head_yaw;
            const glm::vec2 f{ -std::sin(hy), std::cos(hy) }, r{ std::cos(hy), std::sin(hy) };
            hd                   = m_vr_ref_fwd * f + m_vr_ref_right * r;   // the camera's offset from the body, on the head's heading
            const glm::vec2 want = glm::vec2{ root } + hd;
            sh                   = want - glm::vec2{ cam };
            used                 = glm::length(sh) < (smooth ? 0.45f : 0.35f) && glm::length(glm::vec2{ v }) < 0.5f;
            if (!used) m_vr_far++;
        } else {
            m_vr_stale++;
        }
        if (used) m_vr_used++;
        else m_vr_fallback++;
        if (used && !est) m_vr_gate++;   // as viewroot was, these pairs snapped back onto the game camera
    }
    if (!smooth) {
        if (used) {
            m_pivot_shift = glm::vec3{ sh, 0.0f };
            m_ms_head     = glm::vec3{ hd, 0.0f };
        }
        m_vr_w  = used ? 1.0f : 0.0f;
        m_vr_sh = sh;
    } else {
        // Eased onto the body and off it (0.15 s); the last body-based shift held while it eases out. Not on foot: at once, but a fall held
        // as a vehicle eases out too (chutelevel: the camera can be 10-25 cm ahead of the body when the fall starts).
        if (used) {
            m_vr_sh = sh;
            m_vr_hd = hd;
        }
        const float target    = used ? 1.0f : 0.0f;
        const bool  fall_ease = m_chute_level.load() && m_chute_vehicle && m_in_vehicle.load() && m_mo.valid && !m_cine_on && !m_menu_screen_on;
        if ((!foot && !fall_ease) || !m_view_root.load() || dt > 0.5f) m_vr_w = target;
        else m_vr_w += (target - m_vr_w) * (1.0f - std::exp(-std::max(dt, 0.0f) / 0.15f));
        if (m_vr_w > 1e-3f) {
            m_pivot_shift = glm::mix(m_pivot_shift, glm::vec3{ m_vr_sh, 0.0f }, m_vr_w);
            m_ms_head     = glm::mix(m_ms_head, glm::vec3{ m_vr_hd, 0.0f }, m_vr_w);
        }
    }
    const bool applied = ok || (smooth && foot && m_vr_w > 1e-3f);
    m_pivot_ok      = applied;
    m_pivot_applied = applied;
    // One line per 5 s while viewroot is used: how often it fell back, and the largest pair-to-pair step of the eyes' base
    // against the body root (both move when walking; a snap onto the game camera and back shows in full).
    if (fresh && m_mo.valid) {
        const glm::vec3 v    = (m_body_root - m_body_root_prev) / float(rf - pf);
        glm::vec3       root = m_body_root + v * float(pair - rf);
        root_steady_at(float(pair), root);
        const glm::vec2 rel = glm::vec2{ cam } + (applied ? glm::vec2{ m_pivot_shift } : glm::vec2{ 0.0f }) - glm::vec2{ root };
        if (m_vr_rel_ok && dt < 0.5f) m_vr_step_max = std::max(m_vr_step_max, glm::length(rel - m_vr_prev_rel));
        m_vr_prev_rel = rel;
        m_vr_rel_ok   = true;
    } else {
        m_vr_rel_ok = false;
    }
    if (now - m_vr_log_t > std::chrono::seconds(5)) {
        const uint64_t u = m_vr_used - m_vr_log_used, fb = m_vr_fallback - m_vr_log_fb;
        if (u + fb > 0)
            spdlog::info("[View] viewroot last 5 s: {} pairs on the body, {} fell back ({} root old, {} too far or fast), {} with viewpivot's offset old "
                         "(viewrootsmooth {}: {}), largest step of the eyes' base {:.1f} cm",
                         u, fb, m_vr_stale - m_vr_log_stale, m_vr_far - m_vr_log_far, m_vr_gate - m_vr_log_gate, smooth ? "on" : "off",
                         smooth ? "kept on the body" : "on the game camera", m_vr_step_max * 100.0f);
        m_vr_log_t     = now;
        m_vr_log_used  = m_vr_used;
        m_vr_log_fb    = m_vr_fallback;
        m_vr_log_gate  = m_vr_gate;
        m_vr_log_stale = m_vr_stale;
        m_vr_log_far   = m_vr_far;
        m_vr_step_max  = 0.0f;
    }
    return m_pivot_shift;
}

// rootsteady: the body root at frame `at` from its last three samples: the middle of the last two, moved on by the pace over all
// three. Samples that come with long and short steps in turn (a, b, a, b) give the same step every pair, (a + b) / 2; a prediction
// from the last two alone moves 2a - b and 2b - a in turn. False (out untouched) when off or without three close samples.
bool FarCry5Stereo::root_steady_at(float at, glm::vec3& out) const
{
    const int f0 = m_body_root_prev2_frame, f1 = m_body_root_prev_frame, f2 = m_body_root_frame;
    if (!m_root_steady.load() || f0 < 0 || !(f0 < f1 && f1 < f2) || f2 - f0 > 12) return false;
    const glm::vec3 v = (m_body_root - m_body_root_prev2) / float(f2 - f0);   // per frame, over the last two steps
    if (glm::length(glm::vec2{ v }) > 0.5f || std::abs(v.z) > 0.5f) return false;   // a teleport, a load: as before
    out = 0.5f * (m_body_root + m_body_root_prev) + v * (at - 0.5f * float(f1 + f2));
    return true;
}

// smoothmotion: the eyes' base z (on foot: the body-based position and walkbob's height) through an alpha-beta tracker, once per
// picture, its time step the headset's display time between pictures (the pose sample's; the wall clock if there is none). The
// prediction moves on by the pace times that step; the gain pulls it onto z over smoothtau and corrects the pace, so a steady pace
// is followed with no lag and steps that come long and short in turn are evened out. Both eyes of a stereo pair get the pair's point.
// Only the body is filtered: the offset turned with the head (m_ms_head, from view_pivot_shift) is taken off before and added after.
// An error the same way two pictures running (over 1.5 cm each) is a start, a stop or a turn: pulled in over smoothquick instead.
// Returns the base unchanged where it does not apply (off, not on foot, a fresh start).
glm::vec3 FarCry5Stereo::motion_smooth(int frame, const glm::vec3& base)
{
    const int pair = afw_mode() ? frame : (frame & ~1);
    if (pair == m_ms_pair) return m_ms_used ? m_ms_out : base;
    const glm::vec3 head = m_pivot_applied ? m_ms_head : glm::vec3{ 0.0f };
    const glm::vec3 z    = base - head;
    const bool    foot = m_mo.valid && !m_in_vehicle.load() && !m_cine_on && !m_menu_screen_on;
    const auto    now  = std::chrono::steady_clock::now();
    const int64_t T    = runtimes::OpenXR::s_fc5_pose_time.load();
    float         dt   = m_ms_pair >= 0 ? std::chrono::duration<float>(now - m_ms_t).count() : 1.0f;
    const bool    disp = m_ms_T > 0 && T > m_ms_T && T - m_ms_T < 250000000;
    if (disp) dt = float(double(T - m_ms_T) * 1e-9);
    const bool gap = m_ms_pair < 0 || pair < m_ms_pair || pair - m_ms_pair > 8 || !(dt > 1e-4f) || dt > 0.25f;
    m_ms_pair      = pair;
    m_ms_t         = now;
    m_ms_T         = T;
    // The log: steps off a steady motion (scaled to the step's time) before and after the filter, over 4 cm.
    auto off_steady = [&](const glm::vec3& step, const glm::vec3& last) { return glm::length(step - last * (dt / std::max(m_ms_dt, 1e-4f))); };
    if (!foot || gap) {
        m_ms_ok = m_ms_used = m_ms_raw_ok = false;
        m_ms_err_prev = glm::vec3{ 0.0f };
        if (foot) {
            m_ms_x     = z;
            m_ms_v     = glm::vec3{ 0.0f };
            m_ms_ok    = true;
            m_ms_raw   = z;
            m_ms_raw_ok = true;
            m_ms_dt    = dt;
            m_ms_raw_step = m_ms_out_step = glm::vec3{ 0.0f };
        }
        return base;
    }
    const glm::vec3 raw_step = z - m_ms_raw;
    if (!m_ms_ok) {   // just on foot: start on z, at the pace of the last step
        m_ms_x  = z;
        m_ms_v  = m_ms_raw_ok && glm::length(raw_step) < 0.75f ? raw_step / dt : glm::vec3{ 0.0f };
        m_ms_ok = true;
    } else {
        const glm::vec3 pred = m_ms_x + m_ms_v * dt;
        const glm::vec3 err  = z - pred;
        if (glm::length(err) > 0.75f) {   // a vault, a ladder, a load: start again on z
            m_ms_x = z;
            m_ms_v = glm::vec3{ 0.0f };
            m_ms_err_prev = glm::vec3{ 0.0f };
            m_ms_resets++;
        } else {
            const bool quick = glm::dot(err, m_ms_err_prev) > 0.0f && glm::length(err) > 0.015f && glm::length(m_ms_err_prev) > 0.015f;
            const float tau  = quick ? std::min(m_smooth_quick.load(), m_smooth_tau.load()) : m_smooth_tau.load();
            if (quick) m_ms_quick_n++;
            m_ms_err_prev = err;
            const float a = 1.0f - std::exp(-dt / std::max(tau, 0.005f));
            const float b = a * a / (2.0f - a);
            const glm::vec3 x_was = m_ms_x;
            m_ms_x = pred + a * err;
            m_ms_v += (b / dt) * err;
            if (glm::length(m_ms_v) > 30.0f) m_ms_v *= 30.0f / glm::length(m_ms_v);
            const glm::vec3 out_step = m_ms_x - x_was;
            m_ms_pairs++;
            if (disp) m_ms_disp++;
            const float dr = off_steady(raw_step, m_ms_raw_step), dout = off_steady(out_step, m_ms_out_step);
            if (dr > 0.04f) { m_ms_raw_n++; m_ms_raw_max = std::max(m_ms_raw_max, dr); }
            if (dout > 0.04f) { m_ms_out_n++; m_ms_out_max = std::max(m_ms_out_max, dout); }
            m_ms_lag_max  = std::max(m_ms_lag_max, glm::length(m_ms_x - z));
            m_ms_out_step = out_step;
        }
    }
    m_ms_raw      = z;
    m_ms_raw_step = raw_step;
    m_ms_raw_ok   = true;
    m_ms_dt       = dt;
    m_ms_used     = m_smooth_motion.load();
    if (now - m_ms_log_t > std::chrono::seconds(5)) {
        if (m_ms_pairs > 0)
            spdlog::info("[Motion] smoothmotion {} last 5 s: {} pictures on foot ({} timed by the headset's display clock), {} restarts, {} pulled in quickly (starts, "
                         "stops, turns); the body's position off a steady motion by over 4 cm: before the filter {} (largest {:.1f} cm), after {} (largest {:.1f} cm); "
                         "furthest from the game's position {:.1f} cm (the offset turned with the head now {:.1f} cm, kept out of the filter)",
                         m_smooth_motion.load() ? "on" : "off", m_ms_pairs, m_ms_disp, m_ms_resets, m_ms_quick_n, m_ms_raw_n, m_ms_raw_max * 100.0f, m_ms_out_n,
                         m_ms_out_max * 100.0f, m_ms_lag_max * 100.0f, glm::length(head) * 100.0f);
        m_ms_log_t = now;
        m_ms_pairs = m_ms_disp = m_ms_resets = m_ms_raw_n = m_ms_out_n = m_ms_quick_n = 0;
        m_ms_raw_max = m_ms_out_max = m_ms_lag_max = 0.0f;
    }
    m_ms_out = m_ms_x + head;
    return m_ms_used ? m_ms_out : base;
}

// viewjump: once per stereo pair, the eyes' base (the view before the head is added) against a steady motion. Walking, its step per pair
// is the walk times the pair's time; a step that differs from the last one (scaled to the time) by more than viewjumpcm is a jump the
// head did not make (a snap onto another base, a root or camera jump), and a tilt of its horizon is a pitch or roll the head did not
// make (snap turns only turn its heading). Counted, the largest of each kept with what was on, one line per 5 s.
void FarCry5Stereo::view_jump_note(int frame, const glm::mat3& base, const glm::vec3& base_cam)
{
    const float thr = m_vj_cm.load() * 0.01f;
    if (!(thr > 0.0f)) { m_vj_ok = false; return; }
    const int pair = afw_mode() ? frame : (frame & ~1);
    if (pair == m_vj_pair) return;
    const auto  now = std::chrono::steady_clock::now();
    const float dt  = m_vj_pair >= 0 ? std::chrono::duration<float>(now - m_vj_t).count() : 1.0f;
    m_vj_pair       = pair;
    m_vj_t          = now;
    const glm::vec3 step = base_cam - m_vj_cam;
    const glm::vec3 up   = glm::vec3{ glm::row(glm::mat4{ base }, 1) };   // the base frame's up, in the world
    auto what = [&](float v, const char* unit) {
        const char* where = m_cine_on ? "cutscene" : !m_in_vehicle.load() ? "on foot" : m_chute_vehicle ? "falling (held as a vehicle)" : m_veh_driving.load() ? "driving" : "riding";
        return fmt::format("{:.1f} {} at frame {} ({}; pair {:.1f} ms, last {:.1f} ms; base {:.2f} m/s; off by {:.1f} cm across, {:+.1f} cm up; root {} frames old, "
                           "rootsteady {}; viewroot weight {:.2f}, bob {:+.1f} cm, airfollow blend {:.2f}{}{})", v, unit,
                           pair, where, dt * 1000.0f, m_vj_dt * 1000.0f, glm::length(step) / std::max(dt, 1e-3f), glm::length(glm::vec2{ m_vj_dev }) * 100.0f,
                           m_vj_dev.z * 100.0f, pair - m_body_root_frame, m_root_steady.load() ? "on" : "off", m_vr_w, m_bob_applied ? m_bob_out * 100.0f : 0.0f,
                           m_air_blend, m_snap_sync_pend != 0.0f ? ", snap in flight" : "", m_exit_settle > 0 || m_veh_settle.load() > 0 ? ", vehicle settle" : "");
    };
    if (m_vj_ok && dt < 0.25f && m_vj_dt > 1e-4f && m_vj_dt < 0.25f) {
        m_vj_pairs++;
        // Off a steady motion, scaled to the pair's time; in a vehicle at speed a quarter of the step is allowed (the game's own update time varies).
        m_vj_dev         = step - m_vj_step * (dt / m_vj_dt);
        const float dev  = glm::length(m_vj_dev);
        const float lim  = std::max(thr, m_in_vehicle.load() ? 0.25f * glm::length(step) : 0.0f);
        if (dev > lim && glm::length(step) < 5.0f) {
            m_vj_n++;
            if (dev > m_vj_max) { m_vj_max = dev; m_vj_max_what = what(dev * 100.0f, "cm"); }
            if (dev > 3.0f * lim) spdlog::info("[ViewJump] the eyes' base stepped {}", what(dev * 100.0f, "cm"));
        }
        const float tilt = glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(up), glm::normalize(m_vj_up)), -1.0f, 1.0f)));
        if (tilt > 1.5f) {
            m_vj_tilt_n++;
            if (tilt > m_vj_tilt_max) { m_vj_tilt_max = tilt; m_vj_tilt_what = what(tilt, "deg"); }
            if (tilt > 6.0f) spdlog::info("[ViewJump] the horizon tilted {}", what(tilt, "deg"));
        }
    }
    m_vj_ok   = true;
    m_vj_cam  = base_cam;
    m_vj_step = step;
    m_vj_dt   = dt;
    m_vj_up   = up;
    if (now - m_vj_log_t > std::chrono::seconds(5)) {
        if (m_vj_n + m_vj_tilt_n > 0)
            spdlog::info("[ViewJump] last 5 s: {} of {} pairs stepped over {:.0f} cm off a steady motion (largest {}); {} tilted the horizon over 1.5 deg (largest {})", m_vj_n,
                         m_vj_pairs, thr * 100.0f, m_vj_n ? m_vj_max_what : std::string{ "-" }, m_vj_tilt_n, m_vj_tilt_n ? m_vj_tilt_what : std::string{ "-" });
        m_vj_log_t = now;
        m_vj_n = m_vj_tilt_n = m_vj_pairs = 0;
        m_vj_max = m_vj_tilt_max = 0.0f;
    }
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

// F11 Debug > Head tracking and view (Homard 24 Oct; was Motion control > View). The body-based view's switches (viewpivot, viewroot,
// viewrootsmooth, rootsteady, walkbob) are one since 29 Oct: "Body-based view" in Comfort > Smooth motion (bodyview).
void FarCry5Stereo::draw_pivot_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    ImGui::TextDisabled("Body-based view (Comfort > Smooth motion): %llu pairs, %llu fell back", (unsigned long long)m_vr_used, (unsigned long long)m_vr_fallback);
    int ms = m_aim_ease_ms.load();
    if (ImGui::SliderInt("Taking a weapon in hand: the game's aim turns over##aimease", &ms, 0, 1000, ms == 0 ? "at once (off)" : "%d ms")) {
        m_aim_ease_ms = ms;
        keep("aimease", fmt::format("aimease {}", ms));
    }
    hint("Default 300 ms (Homard 27 Oct: \"too much camera movement\" when taking a weapon from the sling or the hip). The game's camera sits on the "
         "player's neck; when a weapon comes into your hand the game's aim goes from your head to the barrel. Spread over this time, the neck moves a "
         "little at a time instead of jolting the view. The trigger still turns it at once. 0: in one go, as before.");
}
