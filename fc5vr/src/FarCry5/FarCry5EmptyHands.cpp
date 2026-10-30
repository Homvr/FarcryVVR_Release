// emptyhandstill (Homard 22 Oct 17:25: "when sprinting with no weapon in the hands, the hands change position and some
// animation takes control"; F11 Controls, default on, saved).
//
// With no weapon in hand the motion-controlled hands are the game's gloves draw with each hand moved onto its controller.
// The move put each hand's centre on its controller but kept the game's turn of the hand, and the right hand was placed with
// the whole weapon pass by its centre from the last gloves draw (one game update old). The game's sprint swings both arms in
// the camera's space, so the gloves twisted and the right one jumped round its controller.
//  - On foot with no weapon (not throwing), the right glove is moved onto its controller by its bones, as on the sling
//    (weapon_draw_impl's move_weapon_hand): no lag, its fingers follow the controller (fingergrip).
//  - Each empty hand's palm (its heaviest bone) has a standing pose in the game camera's axes, learned standing still with
//    nothing going on. While walking, running or sprinting (emptyhandstill always: at all times) each hand is turned about its
//    centre so its palm keeps that pose: on its controller it no longer turns with the game's animation. Eased in and out
//    over 0.25 s; not in a vehicle, while throwing, or within 1.5 s of X (interact, pick up), which keep the game's animation.
//  - The same hold with the weapon on the sling (the mod's sling gesture: the hands are empty, but the game still holds the
//    gun and plays its sprint pose with it; both hands are already moved by their bones there). Its own standing poses: the
//    game's hands with a slung gun stand otherwise than with none (simulator 23 Oct, barehands-1019: the slung SMG's sprint
//    turned the left palm 69-73 deg on its controller).
// Off: both as before 22 Oct.
//
// emptyhandalign (Homard 23 Oct: "they followed the controller correctly but palms and orientation were not aligned correctly
// with controller orientation when no weapon is held"; F11 Controls, default on, saved).
// A hand on a weapon is the game's grip pose placed with the weapon's calibration on the controller, so the palm lies as the
// real hand does on the controller's grip. An empty hand kept the game's own idle pose in the camera's axes (fist low, palm
// turned in and down), carried onto the controller: it turned with the controller but sat at a fixed wrong angle on it.
//  - Each palm's pose (the skinning matrix of its heaviest bone: the hand's turn from the mesh's rest pose) is set to one fixed
//    pose on its own controller: the right palm's on a pistol's grip (kEmptyGripDefault, emptyhandgrip), the left mirrored
//    across the controller's x (the rest pose's left hand is the right one mirrored across its own x). Standing or moving, so
//    it also holds the hands against the running and sprint swing as emptyhandstill does; the sling's slinghandtilt is not
//    applied on top. Eased in over 0.25 s; not in a vehicle or while throwing (as emptyhandstill).
//  - emptyhandpose yaw pitch roll: a turn on top in the controller's axes, for the headset (the left mirrored).
// Off: emptyhandstill as before.
//
// offhandstill (Homard 23 Oct 22:17: "while hands remain stable when sprinting, the left hand moves when sprinting while holding
// the weapon with right hand"; F11 Controls, default on, saved).
// With a weapon in the weapon hand and no two-handed hold, the other glove floats on its controller: its centre goes on the
// controller, its turn stays the game's in the game camera's axes (the grip pose on the fore-end, standing). Walking, running
// and above all sprinting the game swings that arm (its sprint pose takes the left hand 20-30 cm off the gun and turns it), so the
// glove twisted on its controller.
//  - That palm has a standing pose per weapon, learned standing still with the weapon in hand and nothing going on (no shot in
//    the last 0.8 s, no reload, X or throw). While walking, running or sprinting (offhandstill always: at all times) the glove is
//    turned about its centre so its palm keeps that pose: on its controller it no longer turns with the game's animation. Before
//    a weapon's pose is learned, the pose the palm had when the hold began is kept. Eased in and out over 0.25 s; not in a
//    vehicle, while throwing, while the game reloads or within 1.5 s of X (interact, pick up), which keep the game's animation.
//  - offhandlog N: N per-frame lines.
// Off: the game's turn of the free hand (before 24 Oct).
#include "FarCry5Stereo.h"
#include "FarCry5Gesture.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>

namespace
{
    using clock = std::chrono::steady_clock;
    float angle_deg(const glm::quat& a, const glm::quat& b) { return glm::degrees(2.0f * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f))); }
}

struct FarCry5Stereo::EmptyHandsState
{
    clock::time_point t{}, last_throw{}, sum_t{};
    bool              bones{ false };     // this frame: the empty right glove moved by its bones
    bool              active{ false };    // this frame: no weapon or the weapon on the sling, on foot, the switch on
    int               state{ 0 };         // which standing poses: 0 no weapon, 1 the weapon on the sling
    bool              want{ false };      // this frame: the hold asked for
    bool              calm{ false };      // this frame: standing still, nothing going on (the standing pose learned)
    float             alpha{ 0.0f };      // 1 = held at the standing pose, 0 = the game's
    bool              align{ false };     // this frame: emptyhandalign turns the palms (active, the switch on)
    float             align_alpha{ 0.0f };// 1 = the palms at the grip pose, 0 = the game's
    glm::quat         learn_sum{ 0.0f, 0.0f, 0.0f, 0.0f };   // emptyhandlearn: the right palm's poses summed
    int               learn_n{ 0 };
    glm::quat         ref[2][2]{ { glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f } },
                                 { glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f } } };   // [state][side]
    bool              ref_ok[2][2]{};
    glm::quat         last[2]{ glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f } };   // the last pose not held (no standing pose yet)
    bool              last_ok[2]{};
    int               turn_frame[2]{ -100, -100 };
    glm::quat         turn[2]{ glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f } };
    float             dev[2]{};           // the game's palm from the standing pose (deg), this frame
    float             left_over[2]{};     // the drawn palm from it (deg), this frame
    // The last second, while the hold was asked for or easing.
    int               n_frames{ 0 }, n_held{ 0 }, n_sprint{ 0 };
    float             max_speed{ 0.0f }, max_dev[2]{}, max_left[2]{};
    bool              said_bones{ false };
    // offhandstill: the free hand with a weapon in the other.
    bool              off_active{ false }, off_want{ false }, off_calm{ false };
    float             off_alpha{ 0.0f };
    uint32_t          off_sig{ 0 };
    clock::time_point off_sig_t{};
    std::unordered_map<uint32_t, glm::quat> off_ref{};   // per weapon: the free palm standing (game camera's axes)
    glm::quat         off_last{ 1.0f, 0.0f, 0.0f, 0.0f };
    bool              off_last_ok{ false };
    int               off_turn_frame{ -100 };
    glm::quat         off_turn{ 1.0f, 0.0f, 0.0f, 0.0f };
    float             off_dev{ 0.0f }, off_left{ 0.0f };   // this frame: the game's palm from the target (deg), the drawn one
    int               off_n{ 0 }, off_held{ 0 }, off_sprint{ 0 };
    float             off_max_speed{ 0.0f }, off_max_dev{ 0.0f }, off_max_left{ 0.0f };
    clock::time_point off_sum_t{};
};

void FarCry5Stereo::empty_hands_frame()
{
    if (!m_ehs) m_ehs = new EmptyHandsState{};
    auto&       e   = *m_ehs;
    const auto  now = clock::now();
    const float da  = e.t == clock::time_point{} ? 0.0f : std::clamp(std::chrono::duration<float>(now - e.t).count(), 0.0f, 0.1f);
    e.t             = now;
    bool        sprint = false;
    float       since_moving = 1e6f;
    const float speed        = gun_still_speed(sprint, since_moving);
    // throwv2 (Homard 24 Oct: "remove all animation that might interfere with the throwing"): the hands stay on their controllers through a throw.
    const bool  throwing     = !fc5::gesture::throw_v2() && (m_rb_down.load() || fc5::gesture::throw_phase() != fc5::gesture::ThrowPhase::Idle);
    if (throwing) e.last_throw = now;
    const float since_x     = std::chrono::duration<float>(now - clock::time_point{ clock::duration{ m_pad_x_press.load() } }).count();
    const float since_throw = e.last_throw == clock::time_point{} ? 1e6f : std::chrono::duration<float>(now - e.last_throw).count();
    const int   mode        = m_empty_still.load();
    const bool  on_foot     = m_motion.load() && !m_in_vehicle.load() && !m_menu_screen_on;
    const bool  slung       = m_mo.slung && !m_unarmed;
    const bool  align_sw    = m_empty_align.load();
    // shoulderdraw (FarCry5WeaponSwitch.cpp): while a weapon switch hides the weapon, the right hand is an empty hand.
    const bool  switching   = weapon_switch_hiding() && !slung;
    const bool  active      = (mode != 0 || align_sw) && (m_unarmed || slung || switching) && on_foot && !throwing;
    const bool  bones       = active && (m_unarmed || switching);
    if (bones != e.bones && (bones || e.said_bones)) {
        e.said_bones = true;
        spdlog::info("[Motion] emptyhandstill: {}", bones ? "no weapon, on foot: the right glove moved onto its controller by its bones"
                                                          : "the right glove placed with the weapon pass again (a weapon, a vehicle, a throw or the switch off)");
    }
    e.bones           = bones;
    if (active != e.active && (active || e.said_bones)) {
        e.said_bones = true;
        if (slung || e.state == 1) spdlog::info("[Motion] emptyhandstill: {}", active ? "the weapon on the sling: the hands held while moving" : "the hands no longer held (sling)");
    }
    e.active          = active;
    const int state   = slung ? 1 : 0;
    if (state != e.state) {
        e.state = state;
        for (int s = 0; s < 2; ++s) e.last_ok[s] = false;   // the other state's pose is not this one's fallback
    }
    const bool moving = sprint || speed >= 1.0f || since_moving < 0.5f;
    e.want            = active && mode != 0 && since_x > 1.5f && since_throw > 1.0f && (mode == 2 || moving);
    e.calm            = active && speed < 0.4f && since_moving > 1.5f && since_x > 2.5f && since_throw > 1.5f;
    e.alpha           = std::clamp(e.alpha + (e.want ? 1.0f : -1.0f) * da / 0.25f, 0.0f, 1.0f);
    if (!active) e.alpha = 0.0f;
    e.align           = active && align_sw;
    e.align_alpha     = e.align ? std::min(1.0f, e.align_alpha + da / 0.25f) : 0.0f;

    if (m_empty_log.load() > 0) {
        m_empty_log--;
        spdlog::info("[EmptyHands] frame {} | speed {:.1f} m/s{} | {} | bones {} hold {} ({:.2f}) align {:.2f} calm {} | right: game {:.1f} deg from {}{}, drawn {:.1f} | left: game {:.1f}{}, drawn {:.1f}",
                     m_frame.load(), speed, sprint ? " sprinting" : "", !active ? "off" : e.state == 1 ? "sling" : "no weapon", bones, e.want, e.alpha, e.align_alpha, e.calm, e.dev[0],
                     e.align ? "the grip pose" : "standing",
                     e.ref_ok[e.state][0] ? "" : " (no standing pose yet)", e.left_over[0], e.dev[1], e.ref_ok[e.state][1] ? "" : " (no standing pose yet)", e.left_over[1]);
    }
    // A line a second while the hold is asked for or easing: how far the game turned the hands, how far they were drawn turned.
    if (e.want || e.alpha > 0.0f || e.align) {
        e.n_frames++;
        if (e.alpha >= 1.0f) e.n_held++;
        if (sprint) e.n_sprint++;
        e.max_speed = std::max(e.max_speed, speed);
        for (int s = 0; s < 2; ++s) { e.max_dev[s] = std::max(e.max_dev[s], e.dev[s]); e.max_left[s] = std::max(e.max_left[s], e.left_over[s]); }
    }
    if (now - e.sum_t > std::chrono::seconds(1)) {
        if (e.n_frames > 0)
            spdlog::info("[Motion] {} ({}): last second {} frames held ({} fully, {} sprinting, up to {:.1f} m/s); the game turned the right hand up to {:.0f} deg "
                         "and the left {:.0f} deg from their {} pose, drawn up to {:.1f} / {:.1f} deg from it",
                         e.align ? "emptyhandalign" : "emptyhandstill", e.state == 1 ? "sling" : "no weapon", e.n_frames, e.n_held, e.n_sprint, e.max_speed, e.max_dev[0],
                         e.max_dev[1], e.align ? "grip" : "standing", e.max_left[0], e.max_left[1]);
        e.sum_t    = now;
        e.n_frames = e.n_held = e.n_sprint = 0;
        e.max_speed = 0.0f;
        for (int s = 0; s < 2; ++s) e.max_dev[s] = e.max_left[s] = 0.0f;
    }
    for (int s = 0; s < 2; ++s) e.dev[s] = e.left_over[s] = 0.0f;

    // offhandstill: a weapon in hand and the other hand free (floating on its controller), on foot.
    const int      off_mode = m_off_still.load();
    const uint32_t wsig     = m_weapon_sig.load();
    if (wsig != e.off_sig) {
        e.off_sig     = wsig;
        e.off_sig_t   = now;
        e.off_last_ok = false;
    }
    const float out_s    = std::chrono::duration<float>(now - e.off_sig_t).count();
    const bool  reload   = gun_still_busy(true);
    const bool  off_on   = off_mode != 0 && !m_unarmed && !m_mo.slung && m_mo.other_valid && !m_two_hand.load() && on_foot && !throwing && wsig != 0;
    if (off_on != e.off_active) spdlog::info("[Motion] offhandstill: {}", off_on ? "a weapon in hand, the other hand free: held while moving" : "no longer held");
    e.off_active = off_on;
    e.off_want   = off_on && !reload && since_x > 1.5f && since_throw > 1.0f && (off_mode == 2 || moving);
    e.off_calm   = off_on && !gun_still_busy() && speed < 0.4f && since_moving > 1.5f && since_x > 2.5f && since_throw > 1.5f && out_s > 3.0f;
    e.off_alpha  = off_on ? std::clamp(e.off_alpha + (e.off_want ? 1.0f : -1.0f) * da / 0.25f, 0.0f, 1.0f) : 0.0f;
    if (m_off_log.load() > 0 && off_on) {
        m_off_log--;
        spdlog::info("[OffHand] frame {} | speed {:.1f} m/s{} | weapon {} | hold {} ({:.2f}) calm {}{} | free hand: game {:.1f} deg from {}, drawn {:.1f}", m_frame.load(), speed,
                     sprint ? " sprinting" : "", wsig, e.off_want, e.off_alpha, e.off_calm, reload ? " reload" : "", e.off_dev,
                     e.off_ref.contains(wsig) ? "its standing pose" : (e.off_last_ok ? "the pose the hold began with" : "itself (nothing to hold to yet)"), e.off_left);
    }
    if (e.off_want || e.off_alpha > 0.0f) {
        e.off_n++;
        if (e.off_alpha >= 1.0f) e.off_held++;
        if (sprint) e.off_sprint++;
        e.off_max_speed = std::max(e.off_max_speed, speed);
        e.off_max_dev   = std::max(e.off_max_dev, e.off_dev);
        e.off_max_left  = std::max(e.off_max_left, e.off_left);
    }
    if (now - e.off_sum_t > std::chrono::seconds(1)) {
        if (e.off_n > 0)
            spdlog::info("[Motion] offhandstill (weapon {}): last second {} frames held ({} fully, {} sprinting, up to {:.1f} m/s); the game turned the free hand up to {:.0f} deg "
                         "from its {} pose, drawn up to {:.1f} deg from it",
                         wsig, e.off_n, e.off_held, e.off_sprint, e.off_max_speed, e.off_max_dev, e.off_ref.contains(wsig) ? "standing" : "held", e.off_max_left);
        e.off_sum_t = now;
        e.off_n = e.off_held = e.off_sprint = 0;
        e.off_max_speed = e.off_max_dev = e.off_max_left = 0.0f;
    }
    e.off_dev = e.off_left = 0.0f;
}

bool FarCry5Stereo::off_hand_active() const { return m_ehs && m_ehs->off_active; }

// The free hand's turn this frame, from its palm h (game camera's axes, the game's pose): once per frame, so every mesh of that
// hand (a glove piece and the bare skin under it) turns the same.
glm::quat FarCry5Stereo::off_hand_turn(const glm::quat& h)
{
    const glm::quat id{ 1.0f, 0.0f, 0.0f, 0.0f };
    if (!m_ehs) return id;
    auto&     e  = *m_ehs;
    const int fr = m_frame.load();
    if (e.off_turn_frame == fr) return e.off_turn;
    e.off_turn_frame = fr;
    auto it          = e.off_ref.find(e.off_sig);
    if (e.off_calm) {
        // ~1 s to follow a new standing pose (the game's idle sway averaged out).
        if (it == e.off_ref.end()) {
            it = e.off_ref.emplace(e.off_sig, h).first;
            spdlog::info("[Motion] offhandstill: weapon {}: the free hand's standing pose learned", e.off_sig);
        } else {
            it->second = glm::normalize(glm::slerp(it->second, glm::dot(it->second, h) < 0.0f ? -h : h, 0.03f));
        }
    }
    if (e.off_alpha <= 0.0f) {
        e.off_last    = h;
        e.off_last_ok = true;
    }
    const glm::quat target = it != e.off_ref.end() ? it->second : e.off_last_ok ? e.off_last : h;
    glm::quat       full   = glm::normalize(target * glm::inverse(h));
    if (full.w < 0.0f) full = -full;
    const glm::quat d = e.off_alpha >= 1.0f ? full : e.off_alpha <= 0.0f ? id : glm::normalize(glm::slerp(id, full, e.off_alpha));
    e.off_dev        = angle_deg(h, target);
    e.off_left       = angle_deg(d * h, target);
    e.off_turn       = d;
    return d;
}

bool FarCry5Stereo::empty_hands_bones() const { return m_ehs && m_ehs->bones; }
bool FarCry5Stereo::empty_hands_active() const { return m_ehs && m_ehs->active; }
bool FarCry5Stereo::empty_hands_holding() const { return m_ehs && m_ehs->active && (m_ehs->want || m_ehs->alpha > 0.0f || m_ehs->align); }
bool FarCry5Stereo::empty_hands_aligning() const { return m_ehs && m_ehs->align; }

// emptyhandalign: that glove side's palm on its own controller (rest pose -> controller axes). The left is the right mirrored
// across x on both sides (the controller's and the rest pose's): (w, x, y, z) -> (w, x, -y, -z).
glm::quat FarCry5Stereo::empty_hand_target(int side) const
{
    glm::quat g{ m_empty_grip[0].load(), m_empty_grip[1].load(), m_empty_grip[2].load(), m_empty_grip[3].load() };
    if (glm::length(g) < 1e-3f) g = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    const glm::quat e = glm::angleAxis(glm::radians(m_empty_pose[0].load()), glm::vec3{ 0, 1, 0 }) * glm::angleAxis(glm::radians(m_empty_pose[1].load()), glm::vec3{ 1, 0, 0 }) *
                        glm::angleAxis(glm::radians(m_empty_pose[2].load()), glm::vec3{ 0, 0, -1 });
    glm::quat t = glm::normalize(e * glm::normalize(g));
    if (side == 1) t = glm::quat{ t.w, t.x, -t.y, -t.z };
    return t;
}

// emptyhandlearn: the right palm on its controller while it holds a weapon, averaged over 60 draws, becomes the grip pose.
void FarCry5Stereo::empty_hand_learn(const glm::quat& rel_right)
{
    if (!m_ehs) m_ehs = new EmptyHandsState{};
    auto& e = *m_ehs;
    if (e.learn_n == 0) e.learn_sum = glm::quat{ 0.0f, 0.0f, 0.0f, 0.0f };
    const glm::quat q = (e.learn_n > 0 && glm::dot(e.learn_sum, rel_right) < 0.0f) ? -rel_right : rel_right;
    e.learn_sum       = glm::quat{ e.learn_sum.w + q.w, e.learn_sum.x + q.x, e.learn_sum.y + q.y, e.learn_sum.z + q.z };
    e.learn_n++;
    if (--m_empty_learn > 0) return;
    glm::quat g = glm::normalize(e.learn_sum);
    if (g.w < 0.0f) g = -g;
    const float agree = glm::length(e.learn_sum) / float(std::max(1, e.learn_n));   // 1 = every draw the same pose
    e.learn_n         = 0;
    m_empty_grip[0]   = g.w;
    m_empty_grip[1]   = g.x;
    m_empty_grip[2]   = g.y;
    m_empty_grip[3]   = g.z;
    const glm::vec3 eu = glm::degrees(glm::eulerAngles(g));
    spdlog::info("[Motion] emptyhandlearn: weapon {}: the right palm on its controller {{ {:.4f}f, {:.4f}f, {:.4f}f, {:.4f}f }} (x {:.1f} y {:.1f} z {:.1f} deg; "
                 "agreement {:.4f}) is the empty hands' grip pose now (not saved: \"emptyhandgrip {:.4f} {:.4f} {:.4f} {:.4f}\" keeps it)",
                 m_weapon_sig.load(), g.w, g.x, g.y, g.z, eu.x, eu.y, eu.z, agree, g.w, g.x, g.y, g.z);
}

// The turn for that side's hand this frame, from its palm h (game camera's axes, the game's pose): once per side and frame, so
// every mesh of that hand in this frame (a glove piece and the bare skin under it) turns the same.
glm::quat FarCry5Stereo::empty_hand_turn(int side, const glm::quat& h, const glm::quat& pre)
{
    const glm::quat id{ 1.0f, 0.0f, 0.0f, 0.0f };
    if (!m_ehs || side < 0 || side > 1) return id;
    auto&     e  = *m_ehs;
    const int fr = m_frame.load();
    if (e.turn_frame[side] == fr) return e.turn[side];
    e.turn_frame[side] = fr;
    const char* name   = side == 0 ? "right" : "left";
    glm::quat&  ref    = e.ref[e.state][side];
    bool&       ref_ok = e.ref_ok[e.state][side];
    if (e.calm) {
        // ~1 s to follow a new standing pose (the game's idle sway averaged out).
        if (!ref_ok) {
            ref    = h;
            ref_ok = true;
            spdlog::info("[Motion] emptyhandstill: the {} hand's standing pose learned ({})", name, e.state == 1 ? "weapon on the sling" : "no weapon");
        } else {
            ref = glm::normalize(glm::slerp(ref, glm::dot(ref, h) < 0.0f ? -h : h, 0.03f));
        }
    }
    if (e.alpha <= 0.0f) {
        e.last[side]    = h;
        e.last_ok[side] = true;
    }
    if (e.align) {
        // emptyhandalign: drawn, the palm is pre * turn * h on its controller; the turn makes that the grip pose.
        const glm::quat tgt  = glm::normalize(glm::inverse(pre) * empty_hand_target(side));
        glm::quat       full = glm::normalize(tgt * glm::inverse(h));
        if (full.w < 0.0f) full = -full;
        const glm::quat d = e.align_alpha >= 1.0f ? full : e.align_alpha <= 0.0f ? id : glm::normalize(glm::slerp(id, full, e.align_alpha));
        e.dev[side]       = angle_deg(h, tgt);
        e.left_over[side] = angle_deg(d * h, tgt);
        e.turn[side]      = d;
        return d;
    }
    const glm::quat target = ref_ok ? ref : e.last_ok[side] ? e.last[side] : h;
    glm::quat       full   = glm::normalize(target * glm::inverse(h));
    if (full.w < 0.0f) full = -full;
    const glm::quat d = e.alpha >= 1.0f ? full : e.alpha <= 0.0f ? id : glm::normalize(glm::slerp(id, full, e.alpha));
    e.dev[side]       = angle_deg(h, target);
    e.left_over[side] = angle_deg(d * h, target);
    e.turn[side]      = d;
    return d;
}

// emptyhandstill on|off|always (saved), emptyhandlog N, emptyhandreset (the standing poses learned again); emptyhandalign on|off,
// emptyhandgrip w x y z | default, emptyhandpose yaw pitch roll (all saved), emptyhandlearn, emptyhandpalmlog N.
bool FarCry5Stereo::empty_hands_command(const std::string& cmd, const std::string& arg, std::istringstream& ls, bool from_settings, bool& persist_changed)
{
    if (cmd.rfind("emptyhand", 0) != 0 && cmd.rfind("offhand", 0) != 0) return false;
    const bool  on = arg == "on" || arg == "1" || arg == "true";
    const float fv = arg.empty() ? 0.0f : std::strtof(arg.c_str(), nullptr);
    if (cmd == "offhandstill") {
        m_off_still    = arg == "always" ? 2 : on ? 1 : 0;
        m_persist[cmd] = arg == "always" ? "offhandstill always" : on ? "offhandstill on" : "offhandstill off";
        if (!from_settings) persist_changed = true;
        spdlog::info("[Motion] offhandstill {}", arg == "always" ? "always" : on ? "on (while moving)" : "off");
    } else if (cmd == "offhandlog") {
        m_off_log = arg.empty() ? 90 : std::max(0, (int)fv);
    } else if (cmd == "offhandreset") {
        if (m_ehs) {
            m_ehs->off_ref.clear();
            m_ehs->off_last_ok = false;
        }
        spdlog::info("[Motion] offhandstill: the free hand's standing poses will be learned again");
    } else if (cmd == "emptyhandstill") {
        m_empty_still = arg == "always" ? 2 : on ? 1 : 0;
        m_persist[cmd] = arg == "always" ? "emptyhandstill always" : on ? "emptyhandstill on" : "emptyhandstill off";
        if (!from_settings) persist_changed = true;
        spdlog::info("[Motion] emptyhandstill {}", arg == "always" ? "always" : on ? "on (while moving)" : "off");
    } else if (cmd == "emptyhandlog") {
        // emptyhandlog N: N per-frame lines, and the hands' wrist -> fingers direction in their own controller's frame for the
        // same number of gloves draws (handaxis: constant while a hand keeps its pose on its controller).
        m_empty_log     = arg.empty() ? 90 : std::max(0, (int)fv);
        m_hand_axis_log = m_empty_log.load();
    } else if (cmd == "emptyhandalign") {
        m_empty_align  = on;
        m_persist[cmd] = on ? "emptyhandalign on" : "emptyhandalign off";
        if (!from_settings) persist_changed = true;
        spdlog::info("[Motion] emptyhandalign {}", on ? "on" : "off");
    } else if (cmd == "emptyhandgrip") {
        float v[4]{ 1.0f, 0.0f, 0.0f, 0.0f };
        if (arg == "default") {
            for (int i = 0; i < 4; ++i) v[i] = kEmptyGripDefault[i];
            m_persist.erase(cmd);
        } else {
            v[0] = fv;
            for (int i = 1; i < 4; ++i) ls >> v[i];
            glm::quat g{ v[0], v[1], v[2], v[3] };
            if (!(glm::length(g) > 0.5f)) return true;
            g = glm::normalize(g);
            v[0] = g.w; v[1] = g.x; v[2] = g.y; v[3] = g.z;
            m_persist[cmd] = fmt::format("emptyhandgrip {:.4f} {:.4f} {:.4f} {:.4f}", v[0], v[1], v[2], v[3]);
        }
        for (int i = 0; i < 4; ++i) m_empty_grip[i] = v[i];
        if (!from_settings) persist_changed = true;
        spdlog::info("[Motion] emptyhandgrip ({:.4f}, {:.4f}, {:.4f}, {:.4f}){}", v[0], v[1], v[2], v[3], arg == "default" ? " (default)" : "");
    } else if (cmd == "emptyhandpose") {
        float v[3]{ fv, 0.0f, 0.0f };
        ls >> v[1] >> v[2];
        for (int i = 0; i < 3; ++i) m_empty_pose[i] = std::clamp(v[i], -180.0f, 180.0f);
        if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f) m_persist.erase(cmd);
        else m_persist[cmd] = fmt::format("emptyhandpose {:.1f} {:.1f} {:.1f}", m_empty_pose[0].load(), m_empty_pose[1].load(), m_empty_pose[2].load());
        if (!from_settings) persist_changed = true;
        spdlog::info("[Motion] emptyhandpose: yaw {:.1f} pitch {:.1f} roll {:.1f} deg on top of the grip pose (left mirrored)", m_empty_pose[0].load(), m_empty_pose[1].load(),
                     m_empty_pose[2].load());
    } else if (cmd == "emptyhandlearn") {
        m_empty_learn = 60;
        spdlog::info("[Motion] emptyhandlearn: the right palm's pose on its controller is taken over the next 60 draws with a weapon in hand");
    } else if (cmd == "emptyhandpalmlog") {
        m_empty_palm_log = arg.empty() ? 30 : std::max(0, (int)fv);
    } else if (cmd == "emptyhandreset") {
        if (m_ehs) for (int s = 0; s < 2; ++s) m_ehs->ref_ok[0][s] = m_ehs->ref_ok[1][s] = m_ehs->last_ok[s] = false;
        spdlog::info("[Motion] emptyhandstill: the standing poses will be learned again");
    } else {
        return false;
    }
    return true;
}
