// meleehit (25 Oct, Homard 24 Oct 21:59: "Try to progress on colision-based melee as much as you can"): melee by hitting a body.
//  - The bodies: every skinned character draw of the world pass (a Character_* material with a bone palette) gives its object's
//    origin, the character's root at its feet; the draws of one frame are grouped by place (0.45 m across the ground, the lowest kept),
//    the player's own (right under the game camera) left out. Each body is a standing capsule from 0.2 to 1.8 m above its root,
//    meleeradius (0.30 m) round.
//  - The weapon: a segment from 0.25 m behind the weapon hand (a rifle's stock) to meleereach (0.55 m) ahead of it along the gun
//    (a machete, a bat, a rifle's barrel), or the bare hand (a fist, 8 cm wider) with no weapon in hand or the weapon on the sling.
//  - A hit: the segment enters a body's capsule while its faster end moves over meleespeed (2.5 m/s, relative to the game camera,
//    so walking into someone is no hit); then at most one every meleecool ms. meleehit 1 logs it ([Melee] hit ...) and pulses the
//    weapon hand (meleebuzz); meleehit 2 also turns the game's aim onto the point hit (meleeaim, like a throw: the eyes stay on the head
//    through Extended View; steps of up to meleeslew deg, 60, against firesplitslew's 12: a body 54 deg aside aimed in 66 ms, 230 ms at 12), and presses the game's melee (meleebutton r3 = the stick click, the game's melee / takedown; rt = the
//    fire trigger, a melee weapon's swing; both) for meleepressms once the aim is within meleegate deg, or after meleewait ms.
//  - meleestill on|off (default off since release 0.12.1, Homard 25 Oct 12:40; Homard 25 Oct 10:22 "better align melee weapon with controller and also desactivate any game animation
//    of the weapon and hands when swinging melee weapon"): a melee weapon (its game hand farther than meleeauto 0.62 m from the camera:
//    the game holds a machete or a bat low at its side; guns sit within 0.55 m; or set by hand: meleeweapons +sig -sig) is held at a
//    made pose in FarCry5GunStill.cpp: its length (the model's +Y, a gun's barrel axis) along the aim's axis turned up by meleepitch
//    (40 deg), right by meleeyaw, rolled by meleeroll, about the game's hand on it, so none of the game's idle, run or swing
//    animation shows. With it meleehit's weapon is the blade: 8 cm behind the hand to meleeblade (0.70 m) along it, and meleebutton
//    auto (the default until release 0.12.1, r3 since) sends RT (the weapon's own swing) instead of the stick click.
//  - Tests: meleetest x y z [s] (a body standing at x right, y ahead, z up from the game camera, for s seconds, default 20), meleepress
//    (the press now, straight ahead), meleelog N (N seconds of the bodies seen, one line a second).
#include "FarCry5Stereo.h"

#include <glm/gtc/matrix_access.hpp>
#include <spdlog/spdlog.h>
#include <Xinput.h>

#include <algorithm>
#include <cmath>

namespace
{
// The closest points of segments p0-p1 and q0-q1: their distance, and where on each (s on p, t on q, 0-1).
float seg_seg(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& q0, const glm::vec3& q1, float& s, float& t)
{
    const glm::vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const float     a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    if (a < 1e-8f && e < 1e-8f) { s = t = 0.0f; return glm::length(r); }
    if (a < 1e-8f) {
        s = 0.0f;
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = glm::dot(d1, r);
        if (e < 1e-8f) {
            t = 0.0f;
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = glm::dot(d1, d2), den = a * e - b * b;
            s = den > 1e-8f ? std::clamp((b * f - c * e) / den, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) { t = 0.0f; s = std::clamp(-c / a, 0.0f, 1.0f); }
            else if (t > 1.0f) { t = 1.0f; s = std::clamp((b - c) / a, 0.0f, 1.0f); }
        }
    }
    return glm::length((p0 + d1 * s) - (q0 + d2 * t));
}
constexpr float kCapLow = 0.2f, kCapHigh = 1.8f;   // the body capsule's axis above its root
int64_t ticks_now() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
const char* button_name(int b, bool melee_weapon)
{
    if (b == 3) b = melee_weapon ? 1 : 0;
    return b == 1 ? "RT" : b == 2 ? "the stick click and RT" : "the stick click";
}
}

bool FarCry5Stereo::melee_command(const std::string& cmd, const std::string& arg, std::istream& rest, bool& persist)
{
    auto num = [&](float lo, float hi, float def) { return arg.empty() ? def : std::clamp(float(std::atof(arg.c_str())), lo, hi); };
    if (cmd == "meleehit") {
        const int m = arg == "log" ? 1 : arg == "on" || arg == "hit" ? 2 : arg == "off" ? 0 : std::clamp(std::atoi(arg.c_str()), 0, 2);
        m_mel_mode = m;
        persist    = true;
        spdlog::info("[Melee] meleehit {} ({})", m, m == 0 ? "off" : m == 1 ? "log only" : "the game's melee pressed at a hit");
        return true;
    }
    if (cmd == "meleereach") { m_mel_reach = num(0.0f, 1.5f, 0.55f); persist = true; return true; }
    if (cmd == "meleeradius") { m_mel_radius = num(0.1f, 0.8f, 0.30f); persist = true; return true; }
    if (cmd == "meleebutton") { m_mel_button = arg == "rt" ? 1 : arg == "both" ? 2 : arg == "r3" ? 0 : 3; persist = true; return true; }
    if (cmd == "meleestill") { m_mel_still = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleepitch") { m_mel_pitch = num(-180.0f, 180.0f, 40.0f); persist = true; return true; }
    if (cmd == "meleeyaw") { m_mel_yaw = num(-180.0f, 180.0f, 0.0f); persist = true; return true; }
    if (cmd == "meleeroll") { m_mel_roll = num(-180.0f, 180.0f, 0.0f); persist = true; return true; }
    if (cmd == "meleeblade") { m_mel_blade = num(0.1f, 1.5f, 0.70f); persist = true; return true; }
    if (cmd == "meleeauto") { m_mel_auto_m = num(0.3f, 2.0f, 0.62f); persist = true; return true; }
    if (cmd == "meleeweapons") {
        // meleeweapons +sig -sig ...: weapons set by hand as melee weapons (+) or not (-); meleeweapons auto: none set.
        std::lock_guard _{ m_mel_mtx };
        m_mel_force_on.clear();
        m_mel_force_off.clear();
        for (std::string t = arg; !t.empty(); t.clear(), rest >> t) {
            if (t.size() < 2 || (t[0] != '+' && t[0] != '-')) continue;
            const uint32_t sig = uint32_t(std::strtoul(t.c_str() + 1, nullptr, 10));
            if (sig != 0) (t[0] == '+' ? m_mel_force_on : m_mel_force_off).insert(sig);
        }
        persist = true;
        spdlog::info("[Melee] meleeweapons: {} set as melee weapons, {} as not", m_mel_force_on.size(), m_mel_force_off.size());
        return true;
    }
    if (cmd == "meleepressms") { m_mel_press_ms = int(num(30.0f, 600.0f, 120.0f)); persist = true; return true; }
    if (cmd == "meleewait") { m_mel_wait_ms = int(num(0.0f, 500.0f, 120.0f)); persist = true; return true; }
    if (cmd == "meleegate") { m_mel_gate = num(1.0f, 90.0f, 15.0f); persist = true; return true; }
    if (cmd == "meleecool") { m_mel_cool_ms = int(num(150.0f, 3000.0f, 650.0f)); persist = true; return true; }
    if (cmd == "meleebuzz") { m_mel_buzz_amp = num(0.0f, 1.0f, 1.0f); persist = true; return true; }
    if (cmd == "meleeaim") { m_mel_aim_on = arg != "off" && arg != "0"; persist = true; return true; }
    if (cmd == "meleeslew") { m_mel_slew = num(1.0f, 90.0f, 60.0f); persist = true; return true; }
    if (cmd == "meleelog") { m_mel_log = int(num(0.0f, 600.0f, 10.0f)); return true; }
    if (cmd == "meleetest") {
        // meleetest x y z [s]: a test body; meleetest off: none.
        float x = 0.0f, y = 0.0f, z = 0.0f, s = 20.0f;
        int   n = 0;
        if (arg != "off" && !arg.empty()) {
            x = float(std::atof(arg.c_str()));
            if (rest >> y >> z) n = 3;
            if (n == 3 && !(rest >> s)) s = 20.0f;
        }
        std::lock_guard _{ m_mel_mtx };
        if (n >= 3) {
            // x right, y ahead, z up: turned with the game camera's heading now (world axes from then on).
            const glm::vec3 f   = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
            const float     yaw = std::atan2(-f.x, f.y);
            m_mel_test_rel      = glm::vec3{ std::cos(yaw), std::sin(yaw), 0.0f } * x + glm::vec3{ -std::sin(yaw), std::cos(yaw), 0.0f } * y + glm::vec3{ 0.0f, 0.0f, z };
            m_mel_test_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(std::clamp(s, 0.5f, 600.0f) * 1000.0f));
            spdlog::info("[Melee] test body at ({:.2f}, {:.2f}, {:.2f}) from the game camera for {:.0f} s", x, y, z, s);
        } else {
            m_mel_test_until = {};
            spdlog::info("[Melee] test body removed");
        }
        return true;
    }
    if (cmd == "meleepress") {
        // The press now, straight ahead of the game camera (no aim): the game's melee as the pad would send it.
        m_mel_press_until = ticks_now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(m_mel_press_ms.load())).count();
        m_mel_presses++;
        spdlog::info("[Melee] meleepress: {} held {} ms", button_name(m_mel_button.load(), m_mel_held_melee.load()), m_mel_press_ms.load());
        return true;
    }
    return false;
}

// meleestill: a melee weapon by hand (meleeweapons), else by where the game holds its hand (its grip point, gripbyhand).
bool FarCry5Stereo::melee_weapon(uint32_t sig)
{
    if (sig == 0) return false;
    {
        std::lock_guard _{ m_mel_mtx };
        if (m_mel_force_on.contains(sig)) return true;
        if (m_mel_force_off.contains(sig)) return false;
    }
    std::lock_guard _{ m_weapon_cal_mtx };
    const auto it = m_grip_hand.find(sig);
    return it != m_grip_hand.end() && it->second.ok && glm::length(it->second.c) > m_mel_auto_m.load();
}

// The made pose's turn, the weapon's axes to the game camera's: its length (+Y) along -Z (the aim's axis, as a gun's barrel) with
// its +Z up, rolled about it, turned right by meleeyaw and up by meleepitch.
glm::quat FarCry5Stereo::melee_hold_rot() const
{
    const glm::quat base  = glm::angleAxis(glm::radians(-90.0f), glm::vec3{ 1.0f, 0.0f, 0.0f });   // +Y -> -Z, +Z -> +Y
    const glm::quat roll  = glm::angleAxis(glm::radians(m_mel_roll.load()), glm::vec3{ 0.0f, 0.0f, -1.0f });
    const glm::quat yaw   = glm::angleAxis(glm::radians(-m_mel_yaw.load()), glm::vec3{ 0.0f, 1.0f, 0.0f });
    const glm::quat pitch = glm::angleAxis(glm::radians(m_mel_pitch.load()), glm::vec3{ 1.0f, 0.0f, 0.0f });
    return glm::normalize(pitch * yaw * roll * base);
}

// world_character_draw: a skinned character draw of the world pass; origin = its object's translation (world).
void FarCry5Stereo::melee_note_character(const glm::vec3& origin)
{
    const glm::vec3 rel = origin - m_world_cam_pos;
    const float     horiz = std::sqrt(rel.x * rel.x + rel.y * rel.y);
    if (horiz > 20.0f || std::abs(rel.z) > 6.0f) return;   // (20 m: meleelog shows who is around)
    if (horiz < 0.5f && rel.z < 0.3f) return;   // the player's own (its root 4 cm from the camera, 1.6 m below; Homard's 25 Oct 11:18 log: a "body" 0.33 m away hit by his own hand)
    const int       frame = m_frame.load();
    std::lock_guard _{ m_mel_mtx };
    if (frame != m_mel_cur_frame) {
        if (m_mel_cur_frame >= 0 && frame > m_mel_cur_frame) {
            m_mel_bodies       = m_mel_cur;
            m_mel_bodies_frame = m_mel_cur_frame;
        }
        m_mel_cur.clear();
        m_mel_cur_frame = frame;
    }
    for (auto& b : m_mel_cur) {
        if (glm::length(glm::vec2{ b.rel.x - rel.x, b.rel.y - rel.y }) < 0.45f && std::abs(b.rel.z - rel.z) < 2.0f) {
            b.rel.z = std::min(b.rel.z, rel.z);   // pieces hung higher on the body (a hat, a holster) stand over the root
            return;
        }
    }
    if (m_mel_cur.size() < 48) m_mel_cur.push_back(MeleeBody{ rel, false });
}

// The game's aim for the press: the point hit, while aiming and pressing.
bool FarCry5Stereo::melee_aim(glm::vec3& want) const
{
    if (m_mel_state == 0 || !m_mel_aim_on.load() || m_mel_mode.load() < 2) return false;
    const glm::vec3 d = m_mel_target - m_world_cam_pos;
    if (glm::length(d) < 0.05f) return false;
    want = glm::normalize(d);
    return true;
}

void FarCry5Stereo::melee_hit_frame()
{
    const auto now  = std::chrono::steady_clock::now();
    const int  mode = m_mel_mode.load();
    // The press: once the aim is on the point hit (or after meleewait), held meleepressms, the aim kept 150 ms more.
    if (m_mel_state == 1) {
        const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_mel_state_t).count();
        if (m_mel_aimed || !m_mel_aim_on.load() || since >= m_mel_wait_ms.load()) {
            m_mel_press_until = ticks_now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(m_mel_press_ms.load())).count();
            m_mel_presses++;
            spdlog::info("[Melee] pressed {} ms after the hit ({}; the game's aim {:.1f} deg from the point hit): {} for {} ms", since, m_mel_aimed ? "the game's aim on the point hit" : m_mel_aim_on.load() ? "meleewait: the aim not there yet" : "meleeaim off", m_mel_aim_err,
                         button_name(m_mel_button.load(), m_mel_held_melee.load()), m_mel_press_ms.load());
            m_mel_state   = 2;
            m_mel_state_t = now;
        }
    } else if (m_mel_state == 2 && now - m_mel_state_t > std::chrono::milliseconds(m_mel_press_ms.load() + 150)) {
        m_mel_state = 0;
    }
    const int e = m_frame.load() & 1;   // this present's eye
    if (mode == 0 || !m_motion.load() || !m_mo.valid || !m_mo.ctrl_valid || m_menu_screen_on || m_cine_on || in_vehicle()) {
        m_mel_have[0] = m_mel_have[1] = false;
        if (mode == 0) m_mel_state = 0;
        return;
    }
    // The bodies of the last whole frame (or the frame being drawn, if newer), and the test body.
    std::vector<MeleeBody> bodies;
    {
        std::lock_guard _{ m_mel_mtx };
        const int frame = m_frame.load();
        if (frame - m_mel_bodies_frame <= 6) bodies = m_mel_bodies;
        if (now < m_mel_test_until) bodies.push_back(MeleeBody{ m_mel_test_rel, true });
    }
    // The weapon's segment from the game camera: the stock end behind the hand to the tip ahead (the bare hand: a fist).
    const bool      armed = !m_unarmed && !m_mo.slung;
    // meleestill: a melee weapon's blade, along its made pose's length (the game camera's axes are the controller's).
    const bool      blade = armed && m_mel_still.load() && melee_weapon(m_weapon_sig.load());
    m_mel_held_melee      = armed && melee_weapon(m_weapon_sig.load());
    const glm::vec3 hand  = (m_mo.slung ? m_mo.hand_pos_world : m_mo.ctrl_pos_world) - m_world_cam_pos;
    glm::vec3       dir   = glm::length(m_mo.barrel_dir) > 0.5f ? glm::normalize(m_mo.barrel_dir) : glm::vec3{ 0.0f, 1.0f, 0.0f };
    if (blade) dir = glm::normalize(m_mo.ctrl_rot_world * (glm::mat3_cast(melee_hold_rot()) * glm::vec3{ 0.0f, 1.0f, 0.0f }));
    const float     reach = !armed ? 0.0f : blade ? m_mel_blade.load() : m_mel_reach.load();
    const glm::vec3 p0    = armed ? hand - dir * (blade ? 0.08f : 0.25f) : hand;   // the bare hand: a fist, meleeradius + 8 cm round the controller
    const glm::vec3 p1    = hand + dir * reach;
    // The speed against this eye's last present (the faster of the hand and the tip).
    bool        have = m_mel_have[e];
    const float dt   = std::chrono::duration<float>(now - m_mel_t[e]).count();
    if (have && !(dt > 1e-3f && dt < 0.2f)) have = false;
    float v = 0.0f;
    if (have) {
        const glm::vec3 hand0 = armed ? m_mel_p0[e] + (m_mel_p1[e] - m_mel_p0[e]) * (0.25f / (reach + 0.25f)) : m_mel_p0[e];
        m_mel_v_hand = glm::length(hand - hand0) / dt;
        m_mel_v_tip  = glm::length(p1 - m_mel_p1[e]) / dt;
        v            = std::max(m_mel_v_hand, m_mel_v_tip);
    }
    m_mel_v = have ? 0.5f * m_mel_v + 0.5f * v : 0.0f;
    if (m_mel_inside.size() != bodies.size()) m_mel_inside.assign(bodies.size(), 0);   // the bodies changed: no entry from before counts
    const float r = m_mel_radius.load() + (armed ? 0.0f : 0.08f);
    int         best = -1;
    float       best_d = 1e9f, best_s = 0.0f, best_t = 0.0f;
    for (size_t i = 0; i < bodies.size(); ++i) {
        const glm::vec3 q0 = bodies[i].rel + glm::vec3{ 0.0f, 0.0f, kCapLow }, q1 = bodies[i].rel + glm::vec3{ 0.0f, 0.0f, kCapHigh };
        float           s = 0.0f, t = 0.0f;
        float           d = seg_seg(p0, p1, q0, q1, s, t);
        if (have) {   // swept: the segment's midpoint path since this eye's last present (a fast swing moves 10-20 cm)
            float s2 = 0.0f, t2 = 0.0f;
            const float d2 = seg_seg(0.5f * (m_mel_p0[e] + m_mel_p1[e]), 0.5f * (p0 + p1), q0, q1, s2, t2);
            if (d2 < d) { d = d2; t = t2; s = 0.5f; }
        }
        const bool in = d < r;
        if (in && !m_mel_inside[i] && d < best_d) { best = int(i); best_d = d; best_s = s; best_t = t; }
        m_mel_inside[i] = in ? 1 : 0;
    }
    const float speed = std::max(v, m_mel_v);
    if (best >= 0 && have && speed > m_melee_speed.load()) {
        m_mel_swings++;
        const bool cool = now - m_mel_last_hit > std::chrono::milliseconds(m_mel_cool_ms.load());
        const auto& b   = bodies[best];
        const float h   = kCapLow + best_t * (kCapHigh - kCapLow);
        if (cool) {
            m_mel_hits++;
            m_mel_last_hit = now;
            // The point hit on the body's axis, between the hips and the head (the game's melee aims at the body, not the feet).
            m_mel_target   = m_world_cam_pos + b.rel + glm::vec3{ 0.0f, 0.0f, std::clamp(h, 0.9f, 1.6f) };
            spdlog::info("[Melee] hit{}: {}{} {:.1f} m/s into the body at ({:.2f}, {:.2f}, {:.2f}) from the camera ({:.2f} m across the ground), {:.2f} m above its root, "
                         "{:.2f} m from its axis, {} of the {} ({} {:.2f} m); {} bodies near; {}",
                         b.test ? " (test body)" : "", armed ? "the weapon" : "the hand", blade ? fmt::format(" ({}, the blade)", m_weapon_sig.load()) : std::string{}, speed, b.rel.x, b.rel.y, b.rel.z, glm::length(glm::vec2{ b.rel }), h, best_d,
                         best_s > 0.8f ? "the tip" : best_s < 0.35f && armed ? "the stock end" : "the middle", armed ? "weapon" : "hand", armed ? "reach" : "fist", armed ? reach : r, bodies.size(),
                         mode == 2 ? "the game's melee next" : "log only (meleehit 1)");
            m_mel_buzz = m_mel_buzz_amp.load();   // (also in log only: the detection can be felt in the headset)
            if (mode == 2) {
                m_mel_state   = 1;
                m_mel_state_t = now;
                m_mel_aimed   = false;
            }
        }
    }
    if (const int left = m_mel_log.load(); left > 0 && now - m_mel_log_t > std::chrono::seconds(1)) {
        m_mel_log_t = now;
        m_mel_log   = left - 1;
        std::string list;
        for (const auto& b : bodies) list += fmt::format(" ({:.2f}, {:.2f}, {:.2f}){}", b.rel.x, b.rel.y, b.rel.z, b.test ? " test" : "");
        spdlog::info("[Melee] {} bodies near:{}; {} hand ({:.2f}, {:.2f}, {:.2f}) along ({:.2f}, {:.2f}, {:.2f}) {:.1f} m/s (hand {:.1f}, tip {:.1f}); {} hits, {} presses", bodies.size(), list,
                     armed ? "weapon" : "bare", hand.x, hand.y, hand.z, dir.x, dir.y, dir.z, speed, m_mel_v_hand, m_mel_v_tip, m_mel_hits, m_mel_presses);
    }
    m_mel_p0[e]   = p0;
    m_mel_p1[e]   = p1;
    m_mel_t[e]    = now;
    m_mel_have[e] = true;
}

// The pad (the game's input thread): the press's buttons, and the hit's pulse once.
void FarCry5Stereo::melee_pad(WORD& buttons, BYTE& rt, float& buzz)
{
    buzz = m_mel_buzz.exchange(0.0f);
    if (ticks_now() >= m_mel_press_until.load()) return;
    int b = m_mel_button.load();
    if (b == 3) b = m_mel_held_melee.load() ? 1 : 0;   // auto: a melee weapon swings with RT
    if (b != 1) buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b != 0) rt = 255;
}

void FarCry5Stereo::draw_melee_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    // meleestill: melee weapons in the hand (with or without meleehit).
    bool st = m_mel_still.load();
    if (ImGui::Checkbox("Melee weapons: lined up with the controller, no game animation##meleestill", &st)) {
        m_mel_still = st;
        keep("meleestill", st ? "meleestill on" : "meleestill off");
    }
    hint("A machete, a shovel or a bat is held at a fixed pose on your controller, its length along the angles below, and the game's own "
         "idle, running and swing animations of the weapon and your hands are taken out: the weapon moves only with your hand. Off (default): "
         "the game's animations show.");
    if (st) {
        float p = m_mel_pitch.load(), y = m_mel_yaw.load(), r = m_mel_roll.load();
        if (ImGui::SliderFloat("Melee weapon: tilt up##meleepitch", &p, -90.0f, 135.0f, "%.0f deg")) {
            m_mel_pitch = p;
            keep("meleepitch", fmt::format("meleepitch {:.0f}", p));
        }
        if (ImGui::SliderFloat("Melee weapon: turn right##meleeyaw", &y, -90.0f, 90.0f, "%.0f deg")) {
            m_mel_yaw = y;
            keep("meleeyaw", fmt::format("meleeyaw {:.0f}", y));
        }
        if (ImGui::SliderFloat("Melee weapon: roll##meleeroll", &r, -180.0f, 180.0f, "%.0f deg")) {
            m_mel_roll = r;
            keep("meleeroll", fmt::format("meleeroll {:.0f}", r));
        }
        if (ImGui::Button("Default angles##meleeangles")) {
            m_mel_pitch = 40.0f;
            m_mel_yaw   = 0.0f;
            m_mel_roll  = 0.0f;
            keep("meleepitch", "meleepitch 40");
            keep("meleeyaw", "meleeyaw 0");
            keep("meleeroll", "meleeroll 0");
        }
        hint("0 tilt: the weapon points where a gun's barrel would. Tilt it up until the blade continues your fist; roll turns the edge.");
        // The weapon in hand now: a melee weapon or not (found by where the game holds it, or set here).
        const uint32_t sig = m_weapon_sig.load();
        if (sig != 0 && !m_unarmed) {
            int pick = 0;
            {
                std::lock_guard _{ m_mel_mtx };
                pick = m_mel_force_on.contains(sig) ? 1 : m_mel_force_off.contains(sig) ? 2 : 0;
            }
            const bool now = melee_weapon(sig);
            ImGui::Text("Weapon in hand (%u): %s", sig, now ? "a melee weapon" : "not a melee weapon");
            bool c = ImGui::RadioButton("Found by itself##meleeweapon", &pick, 0);
            ImGui::SameLine();
            c |= ImGui::RadioButton("Melee weapon##meleeweapon", &pick, 1);
            ImGui::SameLine();
            c |= ImGui::RadioButton("Not one##meleeweapon", &pick, 2);
            if (c) {
                std::string line = "meleeweapons";
                {
                    std::lock_guard _{ m_mel_mtx };
                    m_mel_force_on.erase(sig);
                    m_mel_force_off.erase(sig);
                    if (pick == 1) m_mel_force_on.insert(sig);
                    if (pick == 2) m_mel_force_off.insert(sig);
                    for (auto s : m_mel_force_on) line += fmt::format(" +{}", s);
                    for (auto s : m_mel_force_off) line += fmt::format(" -{}", s);
                }
                keep("meleeweapons", line == "meleeweapons" ? "meleeweapons auto" : line);
            }
            hint("Found by itself: a weapon the game holds low at your side (machete, shovel, bat) counts as a melee weapon.");
        }
    }
    int m = m_mel_mode.load();
    ImGui::TextUnformatted("Melee by hitting a body");
    bool ch = ImGui::RadioButton("Off##meleehit", &m, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("Log only##meleehit", &m, 1);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("Hit##meleehit", &m, 2);
    if (ch) {
        m_mel_mode = m;
        keep("meleehit", fmt::format("meleehit {}", m));
    }
    hint("Swing your weapon (or your empty hand) into someone, faster than a push: Hit presses the game's melee at that moment, aimed at where "
         "you hit them, with a pulse in the hand. Log only: each hit is felt as the pulse and written to the log ([Melee]), the game does nothing. "
         "A gun counts from just behind your hand to about half a metre ahead of it, a melee weapon along its blade.");
    if (m == 0) return;
    int b = m_mel_button.load();
    ImGui::TextUnformatted("Melee sends");
    bool bc = ImGui::RadioButton("Auto##meleebutton", &b, 3);
    ImGui::SameLine();
    bc |= ImGui::RadioButton("Stick click (melee)##meleebutton", &b, 0);
    ImGui::SameLine();
    bc |= ImGui::RadioButton("Trigger##meleebutton", &b, 1);
    ImGui::SameLine();
    bc |= ImGui::RadioButton("Both##meleebutton", &b, 2);
    if (bc) {
        m_mel_button = b;
        keep("meleebutton", b == 1 ? "meleebutton rt" : b == 2 ? "meleebutton both" : b == 0 ? "meleebutton r3" : "meleebutton auto");
    }
    hint("Stick click (default): the game's melee and takedown. Auto: the fire trigger with a melee weapon in hand (the game swings a machete, "
         "a shovel or a bat with it), the stick click with anything else.");
    float reach = m_mel_reach.load();
    if (ImGui::SliderFloat("Gun length for hits##meleereach", &reach, 0.0f, 1.2f, "%.2f m")) {
        m_mel_reach = reach;
        keep("meleereach", fmt::format("meleereach {:.2f}", reach));
    }
    float bl = m_mel_blade.load();
    if (ImGui::SliderFloat("Melee weapon length for hits##meleeblade", &bl, 0.1f, 1.5f, "%.2f m")) {
        m_mel_blade = bl;
        keep("meleeblade", fmt::format("meleeblade {:.2f}", bl));
    }
    float sp = m_melee_speed.load();
    if (ImGui::SliderFloat("Swing faster than##meleespeed", &sp, 0.5f, 10.0f, "%.1f m/s")) {
        m_melee_speed = sp;
        keep("meleespeed", fmt::format("meleespeed {:.1f}", sp));
    }
    hint("A hit needs the weapon to move at least this fast (your real swings are 3 to 16 m/s); slower contact, like walking into someone, does nothing.");
}
