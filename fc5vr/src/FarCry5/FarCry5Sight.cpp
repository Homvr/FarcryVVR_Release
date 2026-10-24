// Sight line aim (cmd sightaim, F11 Weapon; Homard 18 Oct: "the current reddot should be aligned with the middle of the
// reflex sight so that when aiming with the weapon aligned with right eye, the middle of the sight is where the bullet go").
//
// The shots used to follow the controller's axis (barrel_dir), and the gun was drawn on the controller turned by the
// weapon's model correction (modelyaw/modelpitch + weaponcal): nothing tied the gun model's sights to the shots, so with the
// eye behind the sight the shots landed off its centre by however far the model sat from that axis.
//
// Flat, the game aims down sights by putting the sight's centre on its camera's axis. The gun's pose in the camera's axes
// while the game aims down sights (read by gun_still from the weapon pass) therefore gives the sight line in the gun's own
// axes: through the camera's position (the eye point) along the camera's -Z. That line, carried by the drawn gun (the
// controller, the weapon correction and gunstill's held pose), is where the shots go with sightaim on: the red dot, the aim
// marker and the scope's picture (all placed at aim_origin + barrel_dir * distance) follow it too, so with the eye on the
// sight line the dot, the crosshair and the shots meet at the sight's centre, at any distance.
//
// The line is learned per weapon (saved as sightcal) whenever the game aims down sights; a weapon with none yet gets a short
// aim-down-sights press (sightlearn, like scopelearn's) the first time the eye comes behind its sights.
#include "FarCry5Stereo.h"
#include "FarCry5BulletSpeed.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

namespace
{
    using clock = std::chrono::steady_clock;

    void sight_mouse_right(bool down)
    {
        INPUT in{};
        in.type       = INPUT_MOUSE;
        in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        SendInput(1, &in, sizeof(INPUT));
    }
    float deg_between(const glm::vec3& a, const glm::vec3& b)
    {
        const float la = glm::length(a), lb = glm::length(b);
        if (la < 1e-6f || lb < 1e-6f) return 180.0f;
        return glm::degrees(std::acos(std::clamp(glm::dot(a / la, b / lb), -1.0f, 1.0f)));
    }
}

struct FarCry5Stereo::SightState
{
    struct Line
    {
        glm::vec3 o{ 0.0f };               // the eye point while aiming down sights, gun axes
        glm::vec3 d{ 0.0f, 0.0f, -1.0f };  // the sight line's direction, gun axes
        glm::vec3 u{ 0.0f, 1.0f, 0.0f };   // the camera's up then, gun axes (sighteye rolls the gun as the game does)
        int       reads{ 0 };
        bool      ok{ false };
    };
    std::unordered_map<uint32_t, Line> lines{};

    // Aim down sights under way (the game's LT): the camera's zoom and the gun's pose, per game update.
    bool              lt_was{ false };
    clock::time_point lt_t0{}, lt_up_t{};
    glm::mat4         q_last{ 1.0f }, q_start{ 1.0f };   // the last update's pose, the pose at the press
    bool              have_last{ false }, moved{ false };
    int               n{ 0 };
    glm::vec3         so{ 0.0f }, sd{ 0.0f }, su{ 0.0f };
    // The last still run broken off (by the game's scope view taking over, the frame it does): used if nothing came after.
    int               cand_n{ 0 }, cand_age{ 0 };
    bool              read_this_press{ false };
    glm::vec3         cso{ 0.0f }, csd{ 0.0f }, csu{ 0.0f };
    uint32_t          acc_sig{ 0 };

    // The probe: a short press of aim down sights for a weapon with no line yet.
    bool              probe{ false }, probe_mouse{ false };
    uint32_t          probe_sig{ 0 };
    clock::time_point probe_t0{};
    struct Tries { int n{ 0 }; clock::time_point last{}; };
    std::unordered_map<uint32_t, Tries> tries{};
    uint32_t          sig_was{ 0 };
    clock::time_point sig_t{};

    // A weapon gunstill does not hold (no standing pose: a gun whose left hand is not drawn) is drawn in the game's own pose,
    // which follows the game camera with a lag and kicks when firing: aiming along it, the camera steered to the aim moved
    // the gun and so the aim (the shooting checks settled after ~3 s instead of 0.5 s). Its calm pose instead: the game's
    // pose smoothed (~0.5 s) while not firing or aiming down sights, held while firing.
    struct Calm { glm::quat r{ 1.0f, 0.0f, 0.0f, 0.0f }; glm::vec3 t{ 0.0f }; bool ok{ false }; };
    std::unordered_map<uint32_t, Calm> calm{};
    clock::time_point last_fire{}, last_lt{};

    // Diagnostics: the last frame's sight line against the controller's axis.
    uint64_t  frames_on{ 0 }, frames_off{ 0 };
    const char* why_off{ "" };
    float     angle_deg{ 0.0f }, off_cm{ 0.0f }, err_deg{ 0.0f }, err_cm{ 0.0f }, dist_m{ 0.0f };
};

// Called by gun_still once per measured weapon pass: Q = the game's gun pose in its camera's axes (q = Q p).
// While the game aims down sights (its LT), once the gun has moved into the aim-down-sights pose and stays still, that pose
// gives the sight line. Two ways to end: six still updates in a row (iron sights, red dots, low scopes), or the gun mesh
// giving way to the game's 2D scope view right at the end of the animation (high scopes: the x12 rifle's gun is drawn
// still for 2-3 updates, then the overlay): the last still pose then.
void FarCry5Stereo::sight_note_pose(uint32_t sig, const glm::mat4& Q, bool new_update, bool calm)
{
    if (!m_sight) m_sight = new SightState{};
    auto&       s   = *m_sight;
    const auto  now = clock::now();
    // Aiming down sights: the pad's LT went to the game, or a probe's press is on (with the mouse in flat and simulator tests).
    const bool  lt  = m_lt_sent.load() || m_sight_probe_lt.load() || m_scope_probe_lt.load();
    const bool  firing_now = m_trigger.load() || m_dbg_trigger.load();
    if (firing_now) s.last_fire = now;
    if (lt) s.last_lt = now;
    {
        const glm::vec3 tq{ Q[3] };
        const bool front = tq.z < -0.03f && tq.z > -1.0f && std::abs(tq.x) < 0.5f && std::abs(tq.y) < 0.5f;
        if (new_update && sig != 0 && calm && front && !m_in_vehicle.load() && now - s.last_fire > std::chrono::milliseconds(800) &&
            now - s.last_lt > std::chrono::milliseconds(800)) {
            auto&           c = s.calm[sig];
            const glm::quat r = glm::normalize(glm::quat_cast(glm::mat3{ Q }));
            const float     k = c.ok ? 0.05f : 1.0f;   // ~0.5 s at 45 updates a second
            c.r  = c.ok ? glm::normalize(glm::slerp(c.r, r, k)) : r;
            c.t  = glm::mix(c.t, tq, k);
            c.ok = true;
        }
    }
    if (lt != s.lt_was) {
        s.lt_was = lt;
        if (!lt) { sight_finalize("aim down sights released"); s.lt_up_t = now; }
        s.lt_t0 = now; s.n = 0; s.cand_n = 0; s.read_this_press = false; s.have_last = false; s.acc_sig = sig; s.moved = false; s.q_start = Q;
    }
    if (!lt || !new_update || sig == 0) return;
    if (sig != s.acc_sig) {
        // Another mesh is now the largest (the game's 2D scope view, or the bolt cycling): the pose so far, if it was still.
        sight_finalize("the gun gave way to another mesh");
        s.acc_sig = 0;   // nothing more for this press
        return;
    }
    if (s.acc_sig == 0) return;
    s.cand_age++;
    auto break_run = [&] {
        if (s.n >= 2) { s.cand_n = s.n; s.cso = s.so; s.csd = s.sd; s.csu = s.su; s.cand_age = 0; }
        s.n = 0;
    };
    const bool firing = m_trigger.load() || m_dbg_trigger.load();
    // The gun held in front of the camera (not a draw or spawn animation). Its origin can sit at the camera's depth while
    // aiming down sights: the lever rifle 25368's is 2.1 cm ahead of it (simulator 23 Oct), which "3 cm ahead" refused on
    // every press, so it never got a sight line (Homard 22 Oct: odd angle, first shots off).
    const glm::vec3 t{ Q[3] };
    const bool front = t.z < 0.03f && t.z > -1.0f && std::abs(t.x) < 0.5f && std::abs(t.y) < 0.5f;
    if (firing || !calm || !front || m_in_vehicle.load()) {
        if (m_sight_log.load() > 0) {
            m_sight_log--;
            spdlog::info("[Sight] aim down sights, frame {} weapon {}: not read this update (firing {}, calm {}, in front {}, gun at ({:.3f}, {:.3f}, {:.3f}))", m_frame.load(), sig,
                         firing, calm, front, t.x, t.y, t.z);
        }
        break_run();
        s.have_last = false;
        return;
    }
    auto turn_deg = [](const glm::mat4& a, const glm::mat4& b) {
        const glm::quat qa = glm::normalize(glm::quat_cast(glm::mat3{ a })), qb = glm::normalize(glm::quat_cast(glm::mat3{ b }));
        return glm::degrees(2.0f * std::acos(std::clamp(std::abs(glm::dot(qa, qb)), 0.0f, 1.0f)));
    };
    // The aim-down-sights animation has happened: the gun moved 2 cm or 1 deg from where it was at the press.
    if (!s.moved && (glm::length(t - glm::vec3{ s.q_start[3] }) > 0.02f || turn_deg(Q, s.q_start) > 1.0f)) s.moved = true;
    // Still (the game's aim-down-sights sway is slow): 0.15 deg and 2 mm from the last update, else start again.
    const float dmm   = s.have_last ? 1000.0f * glm::length(t - glm::vec3{ s.q_last[3] }) : -1.0f;
    const float ddeg  = s.have_last ? turn_deg(Q, s.q_last) : -1.0f;
    const bool  still = s.have_last && ddeg <= 0.15f && dmm <= 2.0f;
    if (m_sight_log.load() > 0) {
        m_sight_log--;
        spdlog::info("[Sight] aim down sights, frame {} weapon {}: {:.0f} ms in, gun at ({:.3f}, {:.3f}, {:.3f}), {:.1f} mm / {:.2f} deg since the last update, moved {}, "
                     "still run {}, kept run {} ({} updates ago)", m_frame.load(), sig, std::chrono::duration<float, std::milli>(now - s.lt_t0).count(), t.x, t.y, t.z, dmm, ddeg,
                     s.moved, still ? s.n + 1 : 1, s.cand_n, s.cand_age);
    }
    if (!still) break_run();
    s.q_last    = Q;
    s.have_last = true;
    if (!s.moved || now - s.lt_t0 < std::chrono::milliseconds(200)) { s.n = 0; return; }
    const glm::mat4 Qi = glm::inverse(Q);
    const glm::vec3 o{ Qi * glm::vec4{ 0.0f, 0.0f, 0.0f, 1.0f } };
    const glm::vec3 d = glm::normalize(glm::mat3{ Qi } * glm::vec3{ 0.0f, 0.0f, -1.0f });
    const glm::vec3 u = glm::normalize(glm::mat3{ Qi } * glm::vec3{ 0.0f, 1.0f, 0.0f });
    if (s.n == 0) { s.so = o; s.sd = d; s.su = u; }
    else { s.so += o; s.sd += d; s.su += u; }
    ++s.n;
    if (s.n >= 6) sight_finalize("six still updates");
}

// The sight line from the still poses gathered in this press (at least two still updates in a row).
void FarCry5Stereo::sight_finalize(const char* how)
{
    auto&          s   = *m_sight;
    const uint32_t sig  = s.acc_sig;
    // This run if it has two still updates, else the run broken off at most two updates ago (the frame the scope view came).
    const bool      kept = s.n < 2 && s.cand_n >= 2 && s.cand_age <= 2;
    const int       n    = kept ? s.cand_n : s.n;
    const glm::vec3 so   = kept ? s.cso : s.so, sd = kept ? s.csd : s.sd, su = kept ? s.csu : s.su;
    s.n                  = 0;
    s.cand_n             = 0;
    if (sig == 0) return;
    if (n < 2) {
        if (!s.read_this_press) spdlog::info("[Sight] weapon {}: no sight line from this aim down sights ({}): the gun was never still for two updates", sig, how);
        return;
    }
    SightState::Line ln{};
    ln.o = so / float(n);
    ln.d = glm::normalize(sd);
    ln.u = glm::normalize(su - ln.d * glm::dot(su, ln.d));
    // Aiming down sights puts the eye over the gun's middle (both rifles: within 1 mm of its centre plane); the hip pose
    // (the game ignoring the press, e.g. during the draw animation) has the camera 20-30 cm to the side: not a sight line.
    const float side_cm = 100.0f * glm::dot(ln.o, glm::normalize(glm::cross(ln.d, ln.u)));
    if (std::abs(side_cm) > 4.0f) {
        spdlog::info("[Sight] weapon {}: pose not taken ({}): the eye point is {:.1f} cm to the side of the gun's middle, not an aim-down-sights pose", sig, how, side_cm);
        return;
    }
    s.read_this_press = true;
    auto& cur      = s.lines[sig];
    const bool had = cur.ok;
    const float moved_cm = had ? 100.0f * glm::length(glm::cross(ln.o - cur.o, cur.d)) : 0.0f;
    const float turned   = had ? deg_between(ln.d, cur.d) : 0.0f;
    if (had && moved_cm < 0.2f && turned < 0.05f) { cur.reads++; return; }   // the same line again
    ln.reads = cur.reads + 1;
    ln.ok    = true;
    cur      = ln;
    m_persist[fmt::format("sightcal {}", sig)] = fmt::format("sightcal {} {:.5f} {:.5f} {:.5f} {:.6f} {:.6f} {:.6f} {:.5f} {:.5f} {:.5f}", sig, ln.o.x, ln.o.y, ln.o.z, ln.d.x,
                                                             ln.d.y, ln.d.z, ln.u.x, ln.u.y, ln.u.z);
    save_settings();
    spdlog::info("[Sight] weapon {}: sight line {} from the game's aim-down-sights pose ({} still updates, {}; eye point ({:.3f}, {:.3f}, {:.3f}) and direction ({:.4f}, {:.4f}, {:.4f}) "
                 "in the gun's axes){} (saved)",
                 sig, had ? "read again" : "learned", n, how, ln.o.x, ln.o.y, ln.o.z, ln.d.x, ln.d.y, ln.d.z,
                 had ? fmt::format(", moved {:.2f} cm / {:.2f} deg from the saved one", moved_cm, turned) : std::string{});
}
// The drawn gun's pose in the game camera's axes: gunstill's held pose, else (a weapon it does not hold) the game's calm pose.
bool FarCry5Stereo::sight_gun_pose(uint32_t sig, glm::mat4& pose)
{
    if (gun_still_held(sig, pose)) return true;
    if (!m_sight) return false;
    const auto it = m_sight->calm.find(sig);
    if (it == m_sight->calm.end() || !it->second.ok) return false;
    pose    = glm::mat4_cast(it->second.r);
    pose[3] = glm::vec4{ it->second.t, 1.0f };
    return true;
}

// The drawn gun's sight line in the controller's axes (o: the aim-down-sights eye point, d: along the line). False without
// a learned line for the weapon in hand or its held pose.
bool FarCry5Stereo::sight_line(glm::vec3& o_ctrl, glm::vec3& d_ctrl)
{
    if (!m_sight) return false;
    const uint32_t sig = m_weapon_sig.load();
    const auto     it  = m_sight->lines.find(sig);
    if (it == m_sight->lines.end() || !it->second.ok) { m_sight->why_off = "no sight line for this weapon yet"; return false; }
    if (m_hand_map.empty) { m_sight->why_off = "no weapon in hand"; return false; }
    glm::mat4 H{ 1.0f };
    if (!sight_gun_pose(sig, H)) { m_sight->why_off = "no held pose for this weapon (gunstill)"; return false; }
    const auto& ln = it->second;
    o_ctrl         = m_hand_map.corr * (glm::vec3{ H * glm::vec4{ ln.o, 1.0f } } - m_hand_map.grip0);
    d_ctrl         = glm::normalize(m_hand_map.corr * glm::mat3{ H } * ln.d);
    return true;
}

// update_motion: the shots' line. In: the muzzle and (aim_dir) the controller's axis; out: the sight line when sightaim can.
void FarCry5Stereo::sight_motion(const glm::vec3& barrel_origin, glm::vec3& aim_origin, glm::vec3& aim_dir)
{
    if (!m_sight) m_sight = new SightState{};
    auto& s = *m_sight;
    aim_origin    = barrel_origin;
    m_mo.sight_aim = false;
    glm::vec3 o{}, d{};
    if (!sight_line(o, d)) {
        s.frames_off++;
        return;
    }
    const glm::vec3 S = m_mo.ctrl_pos_world + m_mo.ctrl_rot_world * o;   // the eye point on the sight line
    const glm::vec3 D = glm::normalize(m_mo.ctrl_rot_world * d);
    // The line beside the muzzle (the aim march starts there, as from the muzzle before).
    const glm::vec3 P   = S + D * glm::dot(barrel_origin - S, D);
    const float     ang = deg_between(D, aim_dir);
    const float     off = glm::length(barrel_origin - P);
    // What the controller's axis gave, seen from the sight's eye point: how far off the sight's centre the shots landed.
    const float     dist = m_mo.converge > 0.5f ? m_mo.converge : 25.0f;
    const glm::vec3 told = barrel_origin + aim_dir * dist;
    s.angle_deg = ang;
    s.off_cm    = 100.0f * off;
    s.err_deg   = deg_between(told - S, D);
    s.err_cm    = 100.0f * glm::length(glm::cross(told - S, D));
    s.dist_m    = dist;
    if (ang > 20.0f || off > 0.35f) {
        // Not this gun's line (another weapon's, or a pose far from the held one): the controller's axis.
        s.why_off = "sight line implausible (over 20 deg or 35 cm from the barrel)";
        s.frames_off++;
        return;
    }
    if (m_sight_log.load() > 0) {
        m_sight_log--;
        spdlog::info("[Sight] frame {} weapon {}: sight line {:.2f} deg from the controller's axis, {:.1f} cm from the muzzle; the old aim {:.2f} deg ({:.1f} cm at {:.1f} m) off the sight's centre{}",
                     m_frame.load(), m_weapon_sig.load(), ang, 100.0f * off, s.err_deg, s.err_cm, dist, m_sight_aim.load() ? " (sightaim on: shots on the sight line)" : " (sightaim off)");
    }
    if (!m_sight_aim.load()) { s.why_off = "sightaim off"; s.frames_off++; return; }
    aim_origin     = P;
    aim_dir        = D;
    m_mo.sight_aim = true;
    s.frames_on++;
}

// sighteye (test): the test controller placed so that the drawn gun sits before the right eye as the game holds it while
// aiming down sights (the eye m_sight_eye_back m behind its eye point): the eye on the sight line, looking along it.
bool FarCry5Stereo::sight_test_ctrl(glm::mat3& rot_world, glm::vec3& pos_world)
{
    if (!m_sight_eye_test.load() || !m_test_ctrl.load() || !m_sight) return false;
    const uint32_t sig = m_weapon_sig.load();
    const auto     it  = m_sight->lines.find(sig);
    glm::mat4      H{ 1.0f };
    if (it == m_sight->lines.end() || !it->second.ok || !sight_gun_pose(sig, H)) return false;
    const auto& ln = it->second;
    // The aim-down-sights pose's rotation (camera axes -> gun axes has columns x, u, -d).
    const glm::vec3 x  = glm::normalize(glm::cross(ln.u, -ln.d));
    const glm::mat3 Ri{ x, ln.u, -ln.d };          // camera -> gun
    const glm::mat3 Ra = glm::transpose(Ri);       // gun -> camera
    // Right eye axes -> world, pitched down by sighteye's angle (the gun aimed lower, the eye still on its sight line).
    const glm::mat3 E  = glm::transpose(m_sight_eye_rot[1]) * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-m_sight_eye_pitch.load()), glm::vec3{ 1, 0, 0 }) };
    const glm::mat3 Hr{ H };
    rot_world = E * Ra * glm::transpose(Hr) * glm::transpose(m_hand_map.corr);
    const glm::vec3 oc = m_hand_map.corr * (glm::vec3{ H * glm::vec4{ ln.o, 1.0f } } - m_hand_map.grip0);
    const glm::vec3 f  = E * glm::vec3{ 0.0f, 0.0f, -1.0f };
    pos_world          = m_sight_eye_cam[1] + f * m_sight_eye_back.load() - rot_world * oc;
    return true;
}

// Once per frame (update_motion): the aim-down-sights probe for a weapon with no sight line, the first time the right
// (or dominant) eye comes behind the gun's sights.
void FarCry5Stereo::sight_tick()
{
    if (!m_sight) m_sight = new SightState{};
    if (m_vel_scan_at >= 0 && m_frame.load() >= m_vel_scan_at) {
        // velshot (bullet speed research, FarCry5BulletSpeed.cpp): the scan a few frames after the shot.
        m_vel_scan_at = -1;
        const float d[3]{ m_mo.aim_world.x, m_mo.aim_world.y, m_mo.aim_world.z };
        fc5::bullet::scan(d, m_vel_args[0], m_vel_args[1], m_vel_args[2]);
    }
    auto&          s   = *m_sight;
    const auto     now = clock::now();
    const uint32_t sig = m_weapon_sig.load();
    if (sig != s.sig_was) { s.sig_was = sig; s.sig_t = now; }
    auto stop = [&](const char* why) {
        if (!s.probe) return;
        s.probe          = false;
        m_sight_probe_lt = false;
        if (s.probe_mouse) { sight_mouse_right(false); s.probe_mouse = false; }
        const auto it = s.lines.find(s.probe_sig);
        spdlog::info("[Sight] sightlearn: aim-down-sights press for weapon {} over after {:.2f} s ({}): {}", s.probe_sig, std::chrono::duration<float>(now - s.probe_t0).count(), why,
                     it != s.lines.end() && it->second.ok ? "sight line known" : "no sight line read");
    };
    const bool can = m_sight_learn.load() && m_motion.load() && m_mo.ctrl_valid && !m_mo.slung && !m_in_vehicle.load() && !m_menu_screen_on && !m_cine_on && sig != 0 &&
                     !m_hand_map.empty;
    if (s.probe) {
        const auto it   = s.lines.find(s.probe_sig);
        const bool done = it != s.lines.end() && it->second.ok && it->second.reads > 0 && now - s.probe_t0 > std::chrono::milliseconds(300);
        if (done) stop("line read");
        else if (!can || sig != s.probe_sig) stop("interrupted");
        else if (now - s.probe_t0 > std::chrono::milliseconds(1500)) stop("1.5 s");
        return;
    }
    if (!can || m_scope_probe_lt.load() || m_lt_sent.load() || now - s.sig_t < std::chrono::seconds(1)) return;
    if (const auto it = s.lines.find(sig); it != s.lines.end() && it->second.ok) return;
    glm::mat4 H{ 1.0f };
    if (!sight_gun_pose(sig, H)) return;
    if (m_trigger.load() || m_dbg_trigger.load()) return;
    auto& tr = s.tries[sig];
    if (tr.n >= 3 || (tr.n > 0 && now - tr.last < std::chrono::seconds(3))) return;
    // The eye behind the gun: the right eye (the scope's eye) within 11 cm of the shots' line, behind the muzzle, looking
    // along it within 30 deg (scopeauto's test).
    const int       e     = m_scope_eye.load() == 0 ? 0 : 1;
    const glm::vec3 eye   = m_sight_eye_cam[e];
    const glm::vec3 D     = m_mo.barrel_dir;
    const glm::vec3 v     = eye - m_mo.aim_origin;
    const float     along = -glm::dot(v, D);
    const float     perp  = glm::length(v + D * along);
    const glm::vec3 fwd   = -glm::transpose(m_sight_eye_rot[e])[2];
    if (!(perp < 0.11f && along > 0.10f && deg_between(fwd, D) < 30.0f && glm::length(eye - m_mo.ctrl_pos_world) < 0.5f)) return;
    tr.n++;
    tr.last          = now;
    s.probe          = true;
    s.probe_sig      = sig;
    s.probe_t0       = now;
    m_sight_probe_lt = true;
    // A game reading the mouse instead of the pad (no pad read for 1 s: flat and simulator tests): the right mouse button.
    s.probe_mouse = GetTickCount64() - m_pad_last_poll.load() > 1000;
    if (s.probe_mouse) sight_mouse_right(true);
    spdlog::info("[Sight] sightlearn: weapon {} has no sight line yet; aiming down sights for a moment to read it (try {} of 3, eye {:.1f} cm from the barrel's line)", sig, tr.n, 100.0f * perp);
}

bool FarCry5Stereo::sight_command(const std::string& cmd, const std::string& arg, std::istringstream& ls, bool from_settings, bool& persist_changed)
{
    if (cmd.rfind("sight", 0) != 0 || cmd == "sightspread") return false;
    if (!m_sight) m_sight = new SightState{};
    auto&       s  = *m_sight;
    const bool  on = arg == "on" || arg == "1" || arg == "true";
    const float fv = arg.empty() ? 0.0f : std::strtof(arg.c_str(), nullptr);
    auto persist = [&](const std::string& key, const std::string& line) {
        m_persist[key] = line;
        if (!from_settings) persist_changed = true;
    };
    if (cmd == "sightaim") { m_sight_aim = on; persist(cmd, on ? "sightaim on" : "sightaim off"); }
    else if (cmd == "sightlearn") { m_sight_learn = on; persist(cmd, on ? "sightlearn on" : "sightlearn off"); }
    else if (cmd == "sightlog") m_sight_log = arg.empty() ? 10 : (int)fv;
    else if (cmd == "sighteye") {
        m_sight_eye_test = on || (fv > 0.0f && arg != "off");
        float b = 0.0f, p = 0.0f;
        if (on) ls >> b >> p;
        m_sight_eye_back  = std::clamp(b, 0.0f, 0.5f);
        m_sight_eye_pitch = std::clamp(p, -60.0f, 60.0f);
        spdlog::info("[Sight] sighteye {}{}", m_sight_eye_test.load() ? "on" : "off",
                     m_sight_eye_test.load() ? fmt::format(" ({:.2f} m behind the eye point, aimed {:.1f} deg down)", m_sight_eye_back.load(), m_sight_eye_pitch.load()) : std::string{});
    } else if (cmd == "sightcal") {
        // sightcal <sig> ox oy oz dx dy dz ux uy uz (saved by the learning)
        const uint32_t sig = (uint32_t)std::strtoul(arg.c_str(), nullptr, 10);
        SightState::Line ln{};
        ls >> ln.o.x >> ln.o.y >> ln.o.z >> ln.d.x >> ln.d.y >> ln.d.z >> ln.u.x >> ln.u.y >> ln.u.z;
        if (sig != 0 && !ls.fail() && glm::length(ln.d) > 0.5f) {
            ln.d  = glm::normalize(ln.d);
            ln.u  = glm::normalize(ln.u);
            ln.ok = true;
            s.lines[sig] = ln;
            m_persist[fmt::format("sightcal {}", sig)] = fmt::format("sightcal {} {:.5f} {:.5f} {:.5f} {:.6f} {:.6f} {:.6f} {:.5f} {:.5f} {:.5f}", sig, ln.o.x, ln.o.y, ln.o.z, ln.d.x,
                                                                     ln.d.y, ln.d.z, ln.u.x, ln.u.y, ln.u.z);
            if (!from_settings) persist_changed = true;
        }
    } else if (cmd == "sightforget") {
        // This weapon's line (or every weapon's with "all") dropped: learned again at the next aim down sights.
        if (arg == "all") {
            for (auto& [k, v] : s.lines) m_persist.erase(fmt::format("sightcal {}", k));
            s.lines.clear();
        } else {
            const uint32_t sig = m_weapon_sig.load();
            s.lines.erase(sig);
            m_persist.erase(fmt::format("sightcal {}", sig));
        }
        s.tries.clear();
        persist_changed = true;
    } else if (cmd == "sightprobe") {
        // Test: sightlearn's aim-down-sights press now, wherever the eye is (the simulator's test controller is not at the eye).
        if (!s.probe && m_weapon_sig.load() != 0) {
            s.probe          = true;
            s.probe_sig      = m_weapon_sig.load();
            s.probe_t0       = clock::now();
            m_sight_probe_lt = true;
            s.probe_mouse    = GetTickCount64() - m_pad_last_poll.load() > 1000;
            if (s.probe_mouse) sight_mouse_right(true);
            spdlog::info("[Sight] sightprobe: aiming down sights for weapon {} (test)", s.probe_sig);
        }
    } else if (cmd == "sightstatus") {
        const uint32_t sig = m_weapon_sig.load();
        const auto     it  = s.lines.find(sig);
        spdlog::info("[Sight] status: sightaim {} sightlearn {}; weapon {} {}; this frame {}; shots on the sight line {} frames, on the controller's axis {} ({}); "
                     "sight line {:.2f} deg from the controller's axis, {:.1f} cm from the muzzle; the controller's axis lands {:.2f} deg / {:.1f} cm off the sight's centre at {:.1f} m; "
                     "{} weapons with a line",
                     m_sight_aim.load(), m_sight_learn.load(), sig, it != s.lines.end() && it->second.ok ? fmt::format("line known (read {} times)", it->second.reads) : "no line",
                     m_mo.sight_aim ? "sight line" : "controller's axis", s.frames_on, s.frames_off, s.why_off, s.angle_deg, s.off_cm, s.err_deg, s.err_cm, s.dist_m, s.lines.size());
    } else {
        spdlog::info("[Sight] unknown command '{}'", cmd);
    }
    return true;
}
