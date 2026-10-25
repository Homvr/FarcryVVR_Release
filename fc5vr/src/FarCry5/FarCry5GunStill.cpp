// gunstill (Homard 16 Oct evening: "the gun still has a lot of animation while running and inside vehicles; gun animation
// should be limited to reloading"; F11 Comfort, default on).
//
// The motion-controlled gun is the game's weapon pass put on the controller: the game's own animation of the gun relative
// to its camera (running bob, the sprint pose, recoil, aiming down sights, vehicle bumps) moved the gun in the hand.
// Here the gun's pose in the game camera's axes (q = A (p - cam), the space the controller places) is held at this
// weapon's standing pose, learned while standing still with nothing going on, at all times except:
//  - while the game reloads: started by the pad's X (reload / interact) or by the game's left hand leaving its place on
//    the gun within 1.5 s of shooting (the game's own reloads after the last round), ended once the hand is back and the
//    pose has settled;
//  - while firing, only with gunstillfire game (gunstillrecoil, Homard 21:29 "keep the recoil animation": the correction
//    frozen at its value when firing began, so the game's kick shows on top of the held pose; off, the default since 20 Oct: the
//    game's pose less its 0.12 s average shows). gunstillfire hold (default since 24 Oct) keeps the gun held while firing too,
//    gunstillfire kick shows only a recoil kick taken from the game's (kick_of in gun_still);
//  - while a throw is under way (the game hides the gun and draws the throwable: FarCry5Throwable);
//  - before this weapon's pose is known (its first draw animation after it is taken out the first time).
// Parts that move on the gun (magazine, bolt, the right hand on the grip) keep their motion: only the whole gun is held.
// The pose is measured on the last weapon pass; in stereo the left eye's pass is the next game update (in AFW every
// frame is), so the pose is moved on by the last update's change (the right eye's pass is the same update: exact). The
// game writes the gun's transform after the pass's camera upload (simulator: 1059 of 1059 reads there a frame old), so
// it cannot be read for the pass itself.
// In a vehicle, gun_anchor_apply also puts the gun and the hands on the eyes' smoothed position (vehsmooth), so the
// vehicle's bob and vibration the view filters out are not left on the gun.
#include "FarCry5Stereo.h"
#include "FarCry5Gesture.h"
#include "ModPath.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <set>
#include <unordered_map>

#include <Windows.h>
#include <Xinput.h>

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

namespace
{
    using clock = std::chrono::steady_clock;

    struct Pose
    {
        glm::quat r{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 t{ 0.0f };
    };
    Pose to_pose(const glm::mat4& m) { return Pose{ glm::normalize(glm::quat_cast(glm::mat3{ m })), glm::vec3{ m[3] } }; }
    glm::mat4 to_mat(const Pose& p)
    {
        glm::mat4 m = glm::mat4_cast(p.r);
        m[3]        = glm::vec4{ p.t, 1.0f };
        return m;
    }
    float angle_deg(const glm::quat& a, const glm::quat& b) { return glm::degrees(2.0f * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f))); }
    // A change of pose taken r times (its turn about the same axis, its move along the same line).
    glm::mat4 pose_pow(const Pose& p, float r)
    {
        glm::quat q = p.r;
        if (q.w < 0.0f) q = -q;
        const float a = 2.0f * std::acos(std::clamp(q.w, -1.0f, 1.0f));
        const glm::vec3 v{ q.x, q.y, q.z };
        Pose out{};
        if (a > 1e-6f && glm::length(v) > 1e-9f) out.r = glm::angleAxis(a * r, glm::normalize(v));
        out.t = p.t * r;
        return to_mat(out);
    }

    // gunstillshot on|off (default on): the game's left hand leaving the gun is a reload only within 1.5 s of shooting.
    // Off: as before 19 Oct (any time on foot, except sprinting or faster than 3 m/s).
    std::atomic<bool> g_hand_needs_shot{ true };
    // gunstillpad on|off (default on): a press of X shows the game's animation only once the game's left hand leaves the gun
    // within 1 s of it (a reload); interact, a full magazine and the press itself keep the gun held. Off: from the press on.
    std::atomic<bool> g_pad_waits_hand{ true };
}

struct FarCry5Stereo::GunStillState
{
    struct Ref
    {
        Pose      pose{};
        bool      ok{ false }, settled{ false };
        glm::vec3 hand{ 0.0f };   // the game's left hand in the gun's own axes, standing
        bool      hand_ok{ false };
        // The first pose learned (in hand, standing): a dropped pose goes back to it instead of being learned anew from
        // whatever the game shows then (Homard 20 Oct 14:09: the AR-C relearned in the game's relaxed pose, 75 deg off).
        Pose      first{};
        glm::vec3 first_hand{ 0.0f };
        bool      first_ok{ false }, first_hand_ok{ false };
        bool      big_said{ false };
        glm::vec3 line{ 0.0f, 1.0f, 0.0f };   // twohandline: the game's weapon hand -> left hand direction, the gun's own axes
        bool      line_ok{ false };
    };
    // A dropped pose: back to the weapon's first learned one if it has one, else learned again.
    static void drop(Ref& r)
    {
        if (r.first_ok) {
            r.pose    = r.first;
            r.hand    = r.first_hand;
            r.hand_ok = r.first_hand_ok;
            r.ok = r.settled = true;
        } else {
            r = Ref{};
        }
    }
    std::unordered_map<uint32_t, Ref> refs{};   // by weapon signature (m_weapon_sig)
    bool                              loaded{ false };   // fc5vr_gunposes.txt read

    // The player's horizontal speed from the game camera.
    glm::vec3         spd_cam{ 0.0f };
    clock::time_point spd_t{};
    bool              spd_have{ false };
    float             speed{ 0.0f };

    // The measured pose, by game update (the measured pass's game camera tells updates apart).
    int       meas_frame{ -100 };
    glm::mat4 q_last{ 1.0f }, q_prev{ 1.0f };
    bool      have_prev{ false };

    // The game's left hand (gloves draw), in the game camera's axes, and its frame.
    glm::vec3 hand{ 0.0f };
    glm::vec3 hand_w{ 0.0f };   // the game's weapon (right) hand, same draw
    int       hand_frame{ -100 };

    uint32_t          sig{ 0 };
    clock::time_point sig_t{};
    clock::time_point last_fire{}, last_ads{}, last_throw{};
    clock::time_point last_slow{};   // the last time the player was slower than 1 m/s (a sprint press after it = sprinting)
    bool              sprinting{ false };
    uint64_t          hand_skips{ 0 };   // the game's left hand off the gun with no shot just before: not taken for a reload

    // Reload.
    bool              reload{ false };
    const char*       reload_why{ "" };
    clock::time_point reload_t0{}, active_t{};
    int64_t           pad_seen{ 0 };
    bool              pad_init{ false };
    clock::time_point pad_t{};   // the last press of X (gunstillpad: a reload once the hand leaves within 1 s)
    bool              away_on{ false }, away_said{ false };   // the game's standing gun clearly off the held pose, since away_t
    clock::time_point away_t{}, last_moving{};                 // last_moving: faster than 1 m/s or sprinting
    clock::time_point hand_off_t{};                            // since when the hand has been off its place, standing, no reload
    int               hand_off_n{ 0 };
    float             hand_off_cm{ 0.0f }, dev_cm{ 0.0f }, dev_deg{ 0.0f };

    float             alpha{ 0.0f };   // 1 = held at the standing pose, 0 = the game's pose
    // gunstillfire hold / kick: the kick's share (1 while firing, easing out over 0.15 s after); gunstillstat's firing line.
    float             kick_a{ 0.0f };
    uint64_t          fs_n{ 0 }, fs_gated{ 0 };
    float             fs_game_cm{ 0.0f }, fs_game_max_cm{ 0.0f }, fs_game_max_deg{ 0.0f }, fs_shown_max_cm{ 0.0f }, fs_shown_max_deg{ 0.0f };
    int               fs_frame{ -100 };
    bool              fire_on{ false };
    float             fire_beta{ 1.0f };   // gunstillrecoil: 0 = the correction frozen while firing, 1 = the full hold
    glm::mat4         fire_C{ 1.0f };
    clock::time_point alpha_t{};
    uint64_t          held_updates{ 0 }, reloads{ 0 };

    // gunstillstat: the correction given to each frame's pass, checked once that pass's pose is measured.
    int       applied_frame{ -100 };
    bool      applied_full{ false };
    glm::mat4 applied_C{ 1.0f };
    int       applied_C_frame{ -100 };   // the call that gave a correction (not the identity)
    uint64_t  pair_shared{ 0 };           // gunstillpair: right-eye passes given the left eye's hold
    int       pair_frame{ -100 };         // the right-eye frame that was given it (a second weapon view of that frame too)

    // The next update's guess (the Stereo left eye, every AFW frame): the times of the last three game updates (each one's
    // first call), the update q_prev was measured in, and each way's guess for the predicted pass, checked once it is measured.
    int               upd_u[3]{ -100, -100, -100 };
    clock::time_point upd_t[3]{};
    int               q_prev_upd{ -100 };
    int               pred_frame{ -100 };
    glm::mat4         pred_q[4]{ glm::mat4{ 1.0f }, glm::mat4{ 1.0f }, glm::mat4{ 1.0f }, glm::mat4{ 1.0f } };   // hold, linear, timed, half
    uint64_t          pr_n{ 0 }, pr_timed{ 0 };
    float             pr_cm[4]{}, pr_max_cm[4]{}, pr_deg[4]{}, pr_max_deg[4]{};
    float             pr_r_min{ 1e9f }, pr_r_max{ 0.0f };
    // gunstillstat per eye: the drawn gun off its standing pose, its change from that eye's last pass, and in Stereo the two
    // eyes of one update against each other (what one eye sees moving and the other not).
    uint64_t          ey_n[2]{}, ey_steps[2]{}, pd_n{ 0 };
    float             ey_cm[2]{}, ey_max_cm[2]{}, ey_max_deg[2]{}, ey_step_cm[2]{}, ey_step_max_cm[2]{}, ey_step_max_deg[2]{};
    float             pd_cm{ 0.0f }, pd_max_cm{ 0.0f }, pd_max_deg{ 0.0f };
    Pose              ey_prev[2]{};
    int               ey_prev_frame[2]{ -100, -100 };
    uint64_t  st_n{ 0 };
    float     st_drawn_cm{ 0.0f }, st_drawn_max_cm{ 0.0f }, st_drawn_max_deg{ 0.0f };
    float     last_drawn_cm{ 0.0f }, last_drawn_deg{ 0.0f };   // the last checked pass (gunstilllog)
    int       last_drawn_frame{ -100 };
    float     st_game_cm{ 0.0f }, st_game_max_cm{ 0.0f }, st_game_max_deg{ 0.0f };

    // While firing (gunstillrecoil off): the game's pose averaged.
    glm::mat4         q_lp{ 1.0f };
    int               q_lp_frame{ -100 };
    clock::time_point q_lp_t{};
    // gunstillstat: the drawn gun's change from one checked pass to the next (how much it shakes).
    Pose              step_prev{};
    int               step_prev_frame{ -100 };
    uint64_t          st_steps{ 0 };
    float             st_step_cm{ 0.0f }, st_step_max_cm{ 0.0f }, st_step_max_deg{ 0.0f };

    // Probe: the gun's object buffer read at the weapon pass's first draw (gun_still_first_draw).
    ID3D11Buffer* gun_cb{ nullptr };   // the gun mesh's object buffer of the last pass (a key only)
    int           first_frame{ -100 };
    bool          first_have{ false };
    float         first_obj[12]{}, prev_obj[12]{};
    uint64_t      first_current{ 0 }, first_stale{ 0 }, first_other{ 0 };

    // meleestill (FarCry5Melee.cpp): a melee weapon's game hand in the weapon's own axes (smoothed), per weapon.
    struct MeleeHand { glm::vec3 h{ 0.0f }; bool ok{ false }, said{ false }; };
    std::unordered_map<uint32_t, MeleeHand> melee{};
};

void FarCry5Stereo::gun_still_first_draw()
{
    if (!m_gst) return;
    auto&     s  = *m_gst;
    const int fr = m_frame.load();
    if (s.first_frame == fr) return;
    s.first_frame = fr;
    s.first_have  = false;
    if (!s.gun_cb) return;
    std::lock_guard _{ m_obj_mtx };
    if (auto it = m_obj_consts.find(s.gun_cb); it != m_obj_consts.end() && it->second.size >= 48) {
        std::memcpy(s.first_obj, it->second.f, sizeof(s.first_obj));
        s.first_have = true;
    }
}

void FarCry5Stereo::gun_still_gun_cb(ID3D11Buffer* cb)
{
    if (m_gst) m_gst->gun_cb = cb;
}

void FarCry5Stereo::gun_still_note_support(const glm::vec3& F, const glm::vec3& W)
{
    if (!m_gst) m_gst = new GunStillState{};
    m_gst->hand       = F;
    m_gst->hand_w     = W;
    m_gst->hand_frame = m_frame.load();
}

// twohandline (Homard 21 Oct 14:02: "the second hand of the uzi do not work properly"): the two-handed hold points the gun
// along the line from the weapon hand to the left hand. The game holds the Uzi's left hand 15 deg off its barrel's line (the
// rifles about 4), so that gun tilted by as much in a two-handed hold. This is the game's own hand line for the gun in hand,
// in the game camera's axes of its held pose; the hold lines it up with the player's hands instead of the barrel.
bool FarCry5Stereo::gun_still_hand_line(glm::vec3& g)
{
    if (!m_gst || !m_two_hand_line.load()) return false;
    const auto it = m_gst->refs.find(m_weapon_sig.load());
    if (it == m_gst->refs.end() || !it->second.ok || !it->second.line_ok) return false;
    g = glm::normalize(glm::mat3_cast(it->second.pose.r) * it->second.line);
    return g.z < -0.5f;   // forward-ish only (a bad measure is ignored)
}

void FarCry5Stereo::note_pad_buttons(uint16_t buttons)
{
    // The time of the last press of X (reload / interact, rising edge) and the last poll with the left stick pressed (sprint).
    static uint16_t was = 0;
    const int64_t   t   = clock::now().time_since_epoch().count();
    if ((buttons & XINPUT_GAMEPAD_X) && !(was & XINPUT_GAMEPAD_X)) m_pad_x_press = t;
    if (buttons & XINPUT_GAMEPAD_LEFT_THUMB) m_pad_sprint_press = t;   // held (stick fully forward in the new layout) counts all along
    was = buttons;
}

// offhandstill (FarCry5EmptyHands.cpp): a reload under way (the game's animation shown), or (not reload_only) a shot in the last 0.8 s.
bool FarCry5Stereo::gun_still_busy(bool reload_only) const
{
    return m_gst && (m_gst->reload || (!reload_only && clock::now() - m_gst->last_fire < std::chrono::milliseconds(800)));
}

// sprinthead (FarCry5Walk.cpp): sprinting as gunstill sees it (the left stick pressed since the player was last slower than 1 m/s).
bool FarCry5Stereo::player_sprinting() const { return m_gst && m_gst->sprinting; }

// The player's speed (world Z up, smoothed; frames sharing a camera, stereo pairs, are skipped) and sprint: every weapon pass,
// with a gun or not (emptyhandstill holds the empty hands while moving), gunstill on or off.
void FarCry5Stereo::gun_still_pace(const glm::vec3& cam)
{
    if (!m_gst) m_gst = new GunStillState{};
    auto&      s   = *m_gst;
    const auto now = clock::now();
    {
        const float     dt = s.spd_have ? std::chrono::duration<float>(now - s.spd_t).count() : 0.0f;
        const glm::vec2 d{ cam.x - s.spd_cam.x, cam.y - s.spd_cam.y };
        if (!s.spd_have || dt > 0.25f) {
            s.spd_cam = cam; s.spd_t = now; s.spd_have = true;
        } else if (glm::length(d) > 1e-5f && dt > 1e-3f) {
            s.speed += (std::min(glm::length(d) / dt, 20.0f) - s.speed) * std::min(1.0f, dt / 0.15f);
            s.spd_cam = cam; s.spd_t = now;
        } else if (dt > 0.1f) {
            s.speed *= 0.5f;
        }
    }
    // Sprinting: the sprint input (the left stick pressed, or pushed fully forward in the new layout) since the player was last
    // below 1 m/s.
    if (s.speed < 1.0f) s.last_slow = now;
    const bool was_sprinting = s.sprinting;
    s.sprinting              = clock::time_point{ clock::duration{ m_pad_sprint_press.load() } } > s.last_slow;
    if (s.sprinting != was_sprinting) spdlog::info("[Motion] gunstill: {} (speed {:.1f} m/s)", s.sprinting ? "sprinting" : "no longer sprinting", s.speed);
    if (s.sprinting || s.speed >= 1.0f) s.last_moving = now;
}

float FarCry5Stereo::gun_still_speed(bool& sprinting, float& since_moving_s) const
{
    if (!m_gst) { sprinting = false; since_moving_s = 1e6f; return 0.0f; }
    sprinting      = m_gst->sprinting;
    since_moving_s = m_gst->last_moving == clock::time_point{} ? 1e6f : std::chrono::duration<float>(clock::now() - m_gst->last_moving).count();
    return m_gst->speed;
}

glm::mat4 FarCry5Stereo::gun_still(int frame, const glm::vec3& cam)
{
    if (!m_gst) m_gst = new GunStillState{};
    auto&      s   = *m_gst;
    if (!s.loaded) {
        s.loaded = true;
        gun_still_load();
    }
    if (m_grip_save_pending.exchange(false)) gun_still_save();   // a grip point just measured (gripbyhand)
    const auto now = clock::now();
    const float da = std::clamp(std::chrono::duration<float>(now - s.alpha_t).count(), 0.0f, 0.1f);
    s.alpha_t      = now;

    // Speed and sprint: gun_still_pace, called before this for every weapon pass.
    if (m_trigger.load() || m_dbg_trigger.load()) s.last_fire = now;
    if (m_lt_sent.load()) s.last_ads = now;
    const bool throwing = m_rb_down.load() || fc5::gesture::throw_phase() != fc5::gesture::ThrowPhase::Idle;
    if (throwing) s.last_throw = now;

    const auto& hm = m_hand_map;   // the pass the gun was measured in
    // gunstilllate: this pass's gun pose estimated from a part drawn before the gun mesh (weapon_draw_impl), this pass's camera.
    const bool est  = m_gst_est_frame == frame;
    const int  seen = est ? frame : m_gun_obj_seen;
    if (!m_motion.load() || seen < frame - 3 || hm.frame != seen) {
        s.alpha = 0.0f;
        return glm::mat4{ 1.0f };
    }
    glm::mat4 Om{ 1.0f };
    const float* obj = est ? m_gst_est_obj : m_gun_obj_now;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) Om[c][r] = obj[r * 4 + c];
    glm::mat4 A{ hm.rot };
    A[3]              = glm::vec4{ -(hm.rot * hm.cam), 1.0f };
    const glm::mat4 Q = A * Om;   // the gun's pose in the game camera's axes, as the game animates it

    // One step per game update (a stereo pair, or each frame in AFW): learning, the reload state, the update-to-update
    // change for the prediction.
    auto       upd        = [&](int f) { return afw_mode() ? f : (f & ~1); };
    if (upd(frame) != s.upd_u[0]) {
        for (int i = 2; i > 0; --i) { s.upd_u[i] = s.upd_u[i - 1]; s.upd_t[i] = s.upd_t[i - 1]; }
        s.upd_u[0] = upd(frame);
        s.upd_t[0] = now;
    }
    // (gunstilllate's guess from a part is not a measurement: the gun mesh's own pose is measured when it is drawn.)
    const bool new_update = !est && upd(m_gun_obj_seen) != upd(s.meas_frame);
    if (!est && s.meas_frame != m_gun_obj_seen) {
        if (new_update) {
            s.have_prev  = upd(m_gun_obj_seen) - upd(s.meas_frame) <= 2;
            s.q_prev     = s.q_last;
            s.q_prev_upd = upd(s.meas_frame);
        }
        // gunstillstat: each way's guess for a predicted pass against the pose the game then drew it with.
        if (s.pred_frame == m_gun_obj_seen) {
            float e_cm[4], e_deg[4];
            bool  sane = true;
            for (int k = 0; k < 4; ++k) {
                const Pose e = to_pose(glm::inverse(s.pred_q[k]) * Q);
                e_cm[k]      = 100.0f * glm::length(e.t);
                e_deg[k]     = angle_deg(e.r, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f });
                sane         = sane && e_cm[k] < 30.0f;   // not a weapon switch or a teleport
            }
            if (sane) {
                s.pr_n++;
                for (int k = 0; k < 4; ++k) {
                    s.pr_cm[k] += e_cm[k];
                    s.pr_deg[k] += e_deg[k];
                    s.pr_max_cm[k]  = std::max(s.pr_max_cm[k], e_cm[k]);
                    s.pr_max_deg[k] = std::max(s.pr_max_deg[k], e_deg[k]);
                }
            }
            s.pred_frame = -100;
        }
        // Probe (20 Oct, firing jolts in AFW): was the gun's transform already this pass's at the pass's first draw? Judged
        // only when the gun moved since the last measurement.
        if (s.first_have && s.first_frame == m_gun_obj_seen && s.meas_frame >= 0) {
            auto same = [](const float* a, const float* b) { for (int i = 0; i < 12; ++i) if (std::abs(a[i] - b[i]) > 1e-6f) return false; return true; };
            if (!same(m_gun_obj_now, s.prev_obj)) {
                if (same(s.first_obj, m_gun_obj_now)) s.first_current++;
                else if (same(s.first_obj, s.prev_obj)) s.first_stale++;
                else s.first_other++;
            }
        }
        std::memcpy(s.prev_obj, m_gun_obj_now, sizeof(s.prev_obj));
        s.q_last     = Q;
        s.meas_frame = m_gun_obj_seen;
        // gunstillstat: the gun as drawn in that pass (the correction given to it applied to its measured pose) against its
        // standing pose, and the game's own pose against it (what was taken out).
        if (s.applied_frame == m_gun_obj_seen && s.applied_full) {
            if (auto it = s.refs.find(m_weapon_sig.load()); it != s.refs.end() && it->second.ok) {
                const Pose drawn = to_pose(s.applied_C * Q), game = to_pose(Q);
                const float dc = 100.0f * glm::length(drawn.t - it->second.pose.t), dd = angle_deg(drawn.r, it->second.pose.r);
                const float gc = 100.0f * glm::length(game.t - it->second.pose.t), gd = angle_deg(game.r, it->second.pose.r);
                s.st_n++;
                s.st_drawn_cm += dc; s.st_drawn_max_cm = std::max(s.st_drawn_max_cm, dc); s.st_drawn_max_deg = std::max(s.st_drawn_max_deg, dd);
                s.last_drawn_cm = dc; s.last_drawn_deg = dd; s.last_drawn_frame = m_gun_obj_seen;
                if (m_gun_obj_seen - s.step_prev_frame <= 2) {
                    const float sc = 100.0f * glm::length(drawn.t - s.step_prev.t), sd = angle_deg(drawn.r, s.step_prev.r);
                    s.st_steps++;
                    s.st_step_cm += sc; s.st_step_max_cm = std::max(s.st_step_max_cm, sc); s.st_step_max_deg = std::max(s.st_step_max_deg, sd);
                }
                s.step_prev       = drawn;
                s.step_prev_frame = m_gun_obj_seen;
                const int e = m_gun_obj_seen & 1;
                s.ey_n[e]++;
                s.ey_cm[e] += dc;
                s.ey_max_cm[e]  = std::max(s.ey_max_cm[e], dc);
                s.ey_max_deg[e] = std::max(s.ey_max_deg[e], dd);
                if (m_gun_obj_seen - s.ey_prev_frame[e] <= 4) {
                    const float sc = 100.0f * glm::length(drawn.t - s.ey_prev[e].t), sd = angle_deg(drawn.r, s.ey_prev[e].r);
                    s.ey_steps[e]++;
                    s.ey_step_cm[e] += sc;
                    s.ey_step_max_cm[e]  = std::max(s.ey_step_max_cm[e], sc);
                    s.ey_step_max_deg[e] = std::max(s.ey_step_max_deg[e], sd);
                }
                if (!afw_mode() && e == 1 && s.ey_prev_frame[0] == m_gun_obj_seen - 1) {
                    const float pc = 100.0f * glm::length(drawn.t - s.ey_prev[0].t), pd = angle_deg(drawn.r, s.ey_prev[0].r);
                    s.pd_n++;
                    s.pd_cm += pc;
                    s.pd_max_cm  = std::max(s.pd_max_cm, pc);
                    s.pd_max_deg = std::max(s.pd_max_deg, pd);
                }
                s.ey_prev[e]       = drawn;
                s.ey_prev_frame[e] = m_gun_obj_seen;
                s.st_game_cm += gc;  s.st_game_max_cm = std::max(s.st_game_max_cm, gc);   s.st_game_max_deg = std::max(s.st_game_max_deg, gd);
            }
        }
    }

    const uint32_t sig = m_weapon_sig.load();
    if (sig != s.sig) {
        if (s.sig != 0 && sig != 0) {
            bool grip = false;
            {
                std::lock_guard _{ m_weapon_cal_mtx };
                auto it = m_grip_hand.find(sig);
                grip    = it != m_grip_hand.end() && it->second.ok;
            }
            const auto r = s.refs.find(sig);
            spdlog::info("[Motion] gunstill: weapon {} -> {} (pose {}, grip point {})", s.sig, sig, r != s.refs.end() && r->second.settled ? "known" : "not known yet",
                         grip ? "known" : "not measured yet");
        }
        // A weapon change ends a "reload" (Homard's 14:26 and 14:27 log: shots, then a switch within 1.5 s, its holster
        // animation shown as a reload for 0.5 s): the new weapon is held at its own pose.
        if (s.reload && s.sig != 0) {
            s.reload = false;
            spdlog::info("[Motion] gunstill: reload over (weapon {} -> {}): the new weapon held at its standing pose", s.sig, sig);
        }
        s.sig   = sig;
        s.sig_t = now;
    }
    auto&       ref     = s.refs[sig];
    const float out_s   = std::chrono::duration<float>(now - s.sig_t).count();
    const bool  vehicle = m_in_vehicle.load();
    const Pose  qp      = to_pose(Q);
    // sightaim (FarCry5Sight.cpp): the game's pose while it aims down sights gives the weapon's sight line.
    sight_note_pose(sig, Q, new_update, !s.reload && !throwing);

    if (new_update) {
        // The game's left hand in the gun's own axes (same pass as the gun), against where it sits standing.
        const bool      hand_fresh = s.hand_frame == m_gun_obj_seen;
        const glm::vec3 hand_g     = hand_fresh ? glm::vec3{ glm::inverse(Q) * glm::vec4{ s.hand, 1.0f } } : glm::vec3{ 0.0f };
        s.hand_off_cm              = (hand_fresh && ref.hand_ok) ? 100.0f * glm::length(hand_g - ref.hand) : 0.0f;
        s.dev_cm                   = ref.ok ? 100.0f * glm::length(qp.t - ref.pose.t) : 0.0f;
        s.dev_deg                  = ref.ok ? angle_deg(qp.r, ref.pose.r) : 0.0f;
        const bool hand_off        = s.hand_off_cm > m_gst_hand_cm.load();
        s.hand_off_n               = hand_off ? s.hand_off_n + 1 : 0;

        // Reload: the pad's X (reload / interact), or the game's left hand off the gun on foot (the game's own reloads).
        const int64_t press = m_pad_x_press.load();
        if (!s.pad_init) s.pad_seen = press;   // a press from before the first held gun (menus) is no reload
        s.pad_init          = true;
        const bool    pad   = press != s.pad_seen && press != 0;
        s.pad_seen          = press;
        // Sprinting (17 Oct, Homard: "weapon were still moving when sprinting, and hands too"): the game's sprint pose takes its
        // left hand 20-30 cm off the gun, which read as a reload (his 11:52-11:55 log: 1 s "reloads" over and over), so the
        // sprint pose was shown. While sprinting (the left stick pressed since last below 1 m/s) or faster than 3 m/s, the
        // hand counts only right after shooting (the game's own reload after the last round); pad X always counts.
        // (The sprint flag and last_moving: gun_still_pace.)
        const bool fast_move = s.sprinting || s.speed > 3.0f;
        const bool fired     = now - s.last_fire < std::chrono::milliseconds(1500);
        // gunstillshot (Homard 19 Oct, "the gun jumps out of the hand and snaps back"): standing still, the game also takes its
        // left hand 23-43 cm off the gun for about a second with no shot and no reload press (his 12:49-13:08 log: 41 of the 45
        // hand-off "reloads", most with the weapon on the sling, speed 0; it lowers the gun 7-10 cm / 12-22 deg), and each
        // showed the game's animation for 1 s. Now the hand counts only right after shooting, at any speed; pad X always counts.
        const bool need_shot = g_hand_needs_shot.load() || fast_move;
        // Not in the first second of a weapon (its draw animation after a switch right after shooting).
        const bool by_hand   = !vehicle && s.hand_off_n >= 2 && (!need_shot || fired) && out_s > 1.0f;
        if (!vehicle && s.hand_off_n == 2 && need_shot && !fired && !s.reload) {
            s.hand_skips++;
            spdlog::info("[Motion] gunstill: the game's left hand left the gun with no shot in the last 1.5 s (hand {:.0f} cm from its place, gun {:.0f} cm / {:.0f} deg "
                         "from its standing pose; speed {:.1f} m/s{}): no reload, the gun stays held",
                         s.hand_off_cm, s.dev_cm, s.dev_deg, s.speed, s.sprinting ? ", sprinting" : "");
        }
        // gunstillpad (Homard 19 Oct 14:22-14:33 log: X pressed to interact or right after a sprint showed the game's pose of that
        // moment, 15-59 deg off, for 0.8 s with the hand still on the gun): X counts once the hand leaves within 1 s; with no
        // hand to watch for this weapon, once the gun leaves its standing pose (4 cm / 8 deg).
        if (pad) s.pad_t = now;
        const bool hand_seen_now = hand_fresh && ref.hand_ok;
        const bool pad_go        = !g_pad_waits_hand.load() ? pad
                                   : (now - s.pad_t < std::chrono::seconds(1) && s.pad_t != clock::time_point{} &&
                                      (hand_seen_now ? s.hand_off_n >= 2 : (s.dev_cm > 4.0f || s.dev_deg > 8.0f)));
        if (!s.reload && (pad_go || by_hand)) {
            if (pad_go) s.pad_t = clock::time_point{};
            s.reload     = true;
            s.reload_why = pad_go ? "pad X" : "the game's left hand left the gun";
            s.reload_t0  = now;
            s.active_t   = now;
            s.reloads++;
            spdlog::info("[Motion] gunstill: reload ({}; hand {:.0f} cm from its place, gun {:.0f} cm / {:.0f} deg from its standing pose; speed {:.1f} m/s{}): the game's animation shown",
                         s.reload_why, s.hand_off_cm, s.dev_cm, s.dev_deg, s.speed, s.sprinting ? ", sprinting" : "");
        }
        if (s.reload) {
            // Under way while the hand is off the gun (or, with no hand to watch, the gun away from its standing pose).
            const bool hand_seen = hand_fresh && ref.hand_ok;
            const bool busy      = hand_seen ? hand_off : (s.dev_cm > 4.0f || s.dev_deg > 8.0f);
            if (busy) s.active_t = now;
            const float t     = std::chrono::duration<float>(now - s.reload_t0).count();
            const float quiet = std::chrono::duration<float>(now - s.active_t).count();
            // A press with nothing following (interact, a full magazine) ends after 0.8 s; a reload once quiet for 0.3 s.
            const bool  ended = t > 6.0f || (s.active_t == s.reload_t0 ? t > 0.8f : (quiet > 0.3f && t > 0.5f));
            if (ended) {
                s.reload = false;
                spdlog::info("[Motion] gunstill: reload over after {:.1f} s: the gun held at its standing pose again", t);
                if (t > 6.0f) {
                    // No reload lasts 6 s: the held pose is wrong for this weapon (the gun stayed away from it). Learned again.
                    GunStillState::drop(ref);
                    spdlog::info("[Motion] gunstill: weapon {}'s standing pose dropped (the game's gun stayed {:.0f} cm / {:.0f} deg from it): {}", sig, s.dev_cm, s.dev_deg, ref.first_ok ? "back to its first learned pose" : "learned again");
                }
            }
        }
        // The hand off its place for 6 s on foot, below a walk, with no reload: this weapon's learned pose or hand place is wrong
        // (until 19 Oct such a hand started a "reload" that reached 6 s and dropped it). Learned again.
        if (!hand_off || vehicle || s.reload || fast_move || s.speed >= 1.0f) s.hand_off_t = now;
        else if (ref.ok && now - s.hand_off_t > std::chrono::seconds(6)) {
            spdlog::info("[Motion] gunstill: weapon {}'s standing pose dropped (the game's left hand stayed {:.0f} cm from its place for 6 s): {}", sig, s.hand_off_cm, ref.first_ok ? "back to its first learned pose" : "learned again");
            GunStillState::drop(ref);
            s.hand_off_t = now;
        }

        // Learning this weapon's standing pose: on foot, still (and 1.5 s after moving or sprinting: the game ends its sprint pose
        // after the player stops), nothing going on, the pose not changing.
        const bool quiet_now = !vehicle && s.speed < 0.4f && now - s.last_moving > std::chrono::milliseconds(1500) && !s.reload && !throwing && out_s > 1.0f &&
                               now - s.last_fire > std::chrono::milliseconds(800) &&
                               now - s.last_ads > std::chrono::milliseconds(800) && now - s.last_throw > std::chrono::milliseconds(1500) &&
                               now - clock::time_point{ clock::duration{ m_pad_x_press.load() } } > std::chrono::milliseconds(2500);
        const Pose prev   = to_pose(s.q_prev);
        const bool steady = s.have_prev && glm::length(qp.t - prev.t) < 0.015f && angle_deg(qp.r, prev.r) < 2.0f;
        // Only a gun held in front, both of the game's hands drawn: at load the game shows the weapon still in its spawn
        // animation, 55-60 cm below the camera and beside it (Homard 17 Oct 15:43: learned there, the gun then flipped between
        // that pose and the game's every 6 s). Held guns sit 0.15-0.45 m ahead and 0.1-0.3 m below (rifle, SMG).
        // And not before the weapon has been out 3 s (the draw or spawn animation over; the simulator learned the rifle 13 and
        // 45 cm off its standing pose 1 s after loading).
        const bool in_hands = hand_fresh && qp.t.z < -0.10f && qp.t.y > -0.45f && out_s > 3.0f;
        // A pose clearly not the held one (4 cm / 6 deg) counts only once it has lasted 1 s: right after a sprint the game still
        // held the gun turned 76-85 deg for a moment, which was taken for the standing pose (Homard 17 Oct 21:18, 21:22).
        const bool away = ref.ok && (glm::length(qp.t - ref.pose.t) > 0.04f || angle_deg(qp.r, ref.pose.r) > 6.0f);
        // And not while the game's left hand is off its place: the game lowering the gun for a moment is not where it is held.
        // Nor with the weapon on the sling (the game then shows it relaxed, Homard 20 Oct 14:09: the AR-C learned 75 deg off), and a
        // learned pose follows only corrections up to 10 cm / 15 deg: larger ones are another stance of the game's, not a better pose.
        const bool big = ref.settled && (glm::length(qp.t - ref.pose.t) > 0.10f || angle_deg(qp.r, ref.pose.r) > 15.0f);
        if (big && quiet_now && steady && in_hands && !hand_off && !m_mo.slung && !ref.big_said) {
            ref.big_said = true;
            spdlog::info("[Motion] gunstill: weapon {}: the game holds it {:.0f} cm / {:.0f} deg from its learned pose standing; not followed (another stance)", sig,
                         100.0f * glm::length(qp.t - ref.pose.t), angle_deg(qp.r, ref.pose.r));
        }
        if (!big) ref.big_said = false;
        const bool calm = quiet_now && steady && in_hands && !hand_off && !m_mo.slung && !big;
        if (!(calm && away)) s.away_on = false;
        else if (!s.away_on) {
            s.away_on   = true;
            s.away_t    = now;
            s.away_said = false;
        }
        const bool away_wait = calm && away && ref.settled && now - s.away_t < std::chrono::seconds(1);
        if (calm && !away_wait) {
            // ~1 s at 45 updates/s; from a first guess, or when the held pose has been clearly off for 1 s, ~0.15 s.
            const float k = (ref.settled && !away) ? 0.03f : (ref.ok ? 0.15f : 1.0f);
            if (away && ref.settled && !s.away_said) {
                s.away_said = true;
                spdlog::info("[Motion] gunstill: weapon {} held {:.0f} cm / {:.0f} deg from where the game holds it standing: moved there", sig,
                             100.0f * glm::length(qp.t - ref.pose.t), angle_deg(qp.r, ref.pose.r));
            }
            if (!ref.ok) ref.pose = qp;
            ref.pose.t = glm::mix(ref.pose.t, qp.t, k);
            ref.pose.r = glm::normalize(glm::slerp(ref.pose.r, qp.r, k));
            if (hand_fresh) {
                ref.hand    = ref.hand_ok ? glm::mix(ref.hand, hand_g, k) : hand_g;
                ref.hand_ok = true;
                // twohandline: the weapon hand -> left hand direction in the gun's axes.
                const glm::vec3 d = hand_g - glm::vec3{ glm::inverse(Q) * glm::vec4{ s.hand_w, 1.0f } };
                // Only a left hand ahead of the weapon hand (a handgun's support hand beside or behind it gives no barrel-like line:
                // weapon 18912 measured 138 deg off in the simulator).
                const bool ahead = glm::length(d) > 0.08f && (glm::mat3_cast(ref.pose.r) * glm::normalize(d)).z < -0.7f;
                if (ahead) {
                    const bool first = !ref.line_ok;
                    ref.line         = first ? glm::normalize(d) : glm::normalize(glm::mix(ref.line, glm::normalize(d), k));
                    ref.line_ok      = true;
                    if (first) {
                        const glm::vec3 g = glm::normalize(glm::mat3_cast(ref.pose.r) * ref.line);
                        spdlog::info("[Motion] gunstill: weapon {}: the game's hands line {:.1f} deg right, {:.1f} deg up of its barrel's line (two-handed hold lines it up with your hands)",
                                     sig, glm::degrees(std::atan2(g.x, -g.z)), glm::degrees(std::atan2(g.y, std::sqrt(g.x * g.x + g.z * g.z))));
                    }
                }
            }
            // Settled once it has reached the game's standing pose: from a first guess taken while moving it eases over a few
            // updates. Marked settled after the first step, it stopped 15 % of the way and moved the rest only after 1 s more
            // standing still (Homard 14:28: weapon 42000 learned at 14:28:09, moved 4 cm / 10 deg at 14:28:36).
            const bool reached = glm::length(qp.t - ref.pose.t) < 0.01f && angle_deg(qp.r, ref.pose.r) < 1.5f;
            if (!ref.settled && reached)
                spdlog::info("[Motion] gunstill: weapon {}'s standing pose learned (gun at ({:.2f}, {:.2f}, {:.2f}) in the camera's axes{})", sig, qp.t.x, qp.t.y,
                             qp.t.z, hand_fresh ? ", left hand on it" : ", no left hand seen");
            if (!ref.settled && reached && !ref.first_ok) {
                ref.first         = ref.pose;
                ref.first_hand    = ref.hand;
                ref.first_hand_ok = ref.hand_ok;
                ref.first_ok      = true;
                gun_still_save();   // the next session starts with it
            }
            ref.ok      = true;
            ref.settled = ref.settled || reached;
        } else if (!ref.ok && out_s > 1.2f && !s.reload && !throwing && !vehicle && in_hands && !m_mo.slung && !fast_move && s.speed < 1.5f && steady &&
                   now - s.last_moving > std::chrono::milliseconds(500)) {
            // Never still yet: a first guess once the draw animation is over (taken again when standing still). Not while
            // sprinting, on the sling or with the gun moving (Homard 20 Oct 16:11 / 16:19: the AR-15's guess was taken while
            // sprinting with it on the sling and held it at a wrong angle for the rest of the session).
            ref.pose = qp;
            ref.ok   = true;
            spdlog::info("[Motion] gunstill: weapon {}'s pose taken while moving (learned again when you stand still)", sig);
        }
        if (s.alpha > 0.0f) s.held_updates++;
        if (m_gun_still_log.load() > 0) {
            m_gun_still_log--;
            spdlog::info("[GunStill] frame {} weapon {} | speed {:.1f} m/s{} | gun {:.1f} cm / {:.1f} deg from its standing pose ({}) | left hand {:.1f} cm off ({}) | {} | held {:.2f} | drawn gun {:.1f} cm / {:.1f} deg from it ({})", frame, sig,
                         s.speed, vehicle ? ", vehicle" : "", s.dev_cm, s.dev_deg, ref.ok ? (ref.settled ? "learned" : "first guess") : "none", s.hand_off_cm,
                         hand_fresh ? (ref.hand_ok ? "seen" : "no place yet") : "not drawn", s.reload ? "reload" : (throwing ? "throw" : "steady"), s.alpha, s.last_drawn_cm, s.last_drawn_deg, s.last_drawn_frame == m_gun_obj_seen ? (s.applied_full ? "held" : "easing") : "not checked");
        }
    }

    // meleestill (FarCry5Melee.cpp, Homard 25 Oct 10:22: "better align melee weapon with controller and also desactivate any game
    // animation of the weapon and hands when swinging melee weapon"): a melee weapon is never learned (the game holds it 55-60 cm
    // below the camera, out of the learning's reach), so the game's idle, run and swing animations all showed. Its held pose is
    // made instead: the weapon turned so its length (the model's +Y, the barrel's axis on guns) points along meleepitch / meleeyaw /
    // meleeroll from the aim's axis, about the game's hand on it, which stays at its grip point; held at all times but a throw.
    bool melee_hold = false;
    if (m_mel_still.load() && melee_weapon(sig) && !m_mo.slung) {
        auto&     mh = s.melee[sig];
        glm::vec3 c{ 0.0f };
        bool      c_ok = false;
        {
            std::lock_guard _{ m_weapon_cal_mtx };
            if (auto it = m_grip_hand.find(sig); it != m_grip_hand.end() && it->second.ok) { c = it->second.c; c_ok = true; }
        }
        const bool hand_now = m_frame.load() - m_wpn_hand_frame <= 3;
        if (!c_ok && hand_now) { c = m_wpn_hand_cam; c_ok = true; }
        // The game's hand on it (else its grip point, where the game holds that hand on average) in the weapon's axes.
        if (new_update && (hand_now || c_ok)) {
            const glm::vec3 h = glm::vec3{ glm::inverse(Q) * glm::vec4{ hand_now ? m_wpn_hand_cam : c, 1.0f } };
            if (glm::length(h) < 0.6f) {
                mh.h  = mh.ok ? glm::mix(mh.h, h, 0.05f) : h;
                mh.ok = true;
            }
        }
        if (mh.ok && c_ok) {
            const glm::quat R = melee_hold_rot();
            ref.pose          = Pose{ R, c - R * mh.h };
            ref.ok            = true;
            melee_hold        = true;
            if (!mh.said) {
                mh.said             = true;
                const glm::vec3 len = glm::mat3_cast(qp.r) * glm::vec3{ 0.0f, 1.0f, 0.0f };
                spdlog::info("[Melee] meleestill: weapon {} held as a melee weapon (the game held its length toward ({:.2f}, {:.2f}, {:.2f}) in the camera's axes, its hand at "
                             "({:.2f}, {:.2f}, {:.2f}) in the weapon's; now meleepitch {:.0f}, meleeyaw {:.0f}, meleeroll {:.0f})",
                             sig, len.x, len.y, len.z, mh.h.x, mh.h.y, mh.h.z, m_mel_pitch.load(), m_mel_yaw.load(), m_mel_roll.load());
            }
        }
    }
    // Held unless a reload or a throw is under way, the pose is unknown, or the gun is far from it (another mesh).
    const bool hold = melee_hold ? !throwing
                                 : m_gun_still.load() && ref.ok && !s.reload && !throwing && glm::length(qp.t - ref.pose.t) < 0.6f;
    s.alpha         = std::clamp(s.alpha + (hold ? 1.0f : -1.0f) * da / std::max(0.02f, m_gst_ease.load()), 0.0f, 1.0f);
    const int  prev_applied = s.applied_frame;
    const bool prev_full    = s.applied_full;
    s.applied_frame = frame;
    s.applied_full  = false;
    if (s.alpha <= 0.0f) {
        s.fire_on   = false;
        s.fire_beta = 1.0f;
        return glm::mat4{ 1.0f };
    }
    // gunstillpair: in Stereo the right eye's pass (the same game update as the left eye's) takes the left eye's hold, so both
    // eyes show the same gun (the left one's is guessed from the last update; the right eye's alone was exact, and the gun
    // shook in one eye only).
    const bool shared_again = s.pair_frame == frame && prev_applied == frame && s.applied_C_frame == frame;
    if (m_gst_pair.load() && !afw_mode() && (frame & 1) && ((prev_applied == frame - 1 && s.applied_C_frame == frame - 1) || shared_again) && upd(frame) == upd(frame - 1)) {
        s.applied_full    = prev_full;
        s.applied_C_frame = frame;
        if (!shared_again) s.pair_shared++;
        s.pair_frame = frame;
        m_gs_frames_held++;
        return s.applied_C;
    }

    // This pass is the next game update when its camera is not the measured one: the gun guessed from the last change
    // (gunstillpredict; by default moved on by the whole change).
    glm::mat4 Qn = Q;
    if (upd(frame) != upd(m_gun_obj_seen)) m_gst_stale_frame = frame;   // gunstilllate: placed again at the gun mesh's draw
    if (!est && upd(frame) != upd(m_gun_obj_seen) && s.have_prev) {
        const glm::mat4 D     = Q * glm::inverse(s.q_prev);
        const Pose      dp    = to_pose(D);
        const bool      small_d = glm::length(dp.t) < 0.05f && angle_deg(dp.r, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }) < 10.0f;
        // timed: the change over the span it took, scaled to this update's span (the game animates by its frame time).
        const int step = afw_mode() ? 1 : 2;
        float     r    = 1.0f;
        if (s.upd_u[0] == upd(frame) && s.upd_u[1] == upd(m_gun_obj_seen) && s.upd_u[2] == s.q_prev_upd && s.upd_u[0] - s.upd_u[1] == step &&
            s.upd_u[1] - s.upd_u[2] == step) {
            const float dt0 = std::chrono::duration<float>(s.upd_t[0] - s.upd_t[1]).count(), dt1 = std::chrono::duration<float>(s.upd_t[1] - s.upd_t[2]).count();
            if (dt1 > 1e-3f && dt0 > 0.0f) {
                r = std::clamp(dt0 / dt1, 0.25f, 3.0f);
                s.pr_timed++;
                s.pr_r_min = std::min(s.pr_r_min, r);
                s.pr_r_max = std::max(s.pr_r_max, r);
            }
        }
        s.pred_q[0]  = Q;
        s.pred_q[1]  = small_d ? D * Q : Q;
        s.pred_q[2]  = small_d ? pose_pow(dp, r) * Q : Q;
        s.pred_q[3]  = small_d ? pose_pow(dp, 0.5f) * Q : Q;
        s.pred_frame = frame;
        Qn           = s.pred_q[std::clamp(m_gst_predict.load(), 0, 3)];
    }
    glm::mat4 Cf = to_mat(ref.pose) * glm::inverse(Qn);
    // gunstillfire hold (default) / kick (Homard 20 Oct: "remove all kind of animation except reloading"; 23 Oct 22:17: "there were
    // still involuntary movement when sprinting and shooting"): the gun stays held while firing. The smoothed way below showed the
    // game's pose less its 0.12 s average from the trigger to up to 3 s after it: besides the kick, the fast part of everything the
    // game does with the gun then (into and out of its firing stance, out of the sprint pose when firing from a sprint, the aim's
    // swing from the head to the barrel). A pass held from a guess (Stereo's left eye without gunstilllate, every AFW frame) is
    // held from the last pose while firing, not moved on by the last change: a kick moved on overshoots (10-13 cm in AFW).
    const int  fire_mode = m_gst_fire.load();
    const bool fire_now  = now - s.last_fire < std::chrono::milliseconds(400);
    if (fire_mode != 0) {
        s.fire_on   = false;
        s.fire_beta = 1.0f;
        if (fire_now && upd(frame) != upd(m_gun_obj_seen)) {
            Qn = Q;
            Cf = to_mat(ref.pose) * glm::inverse(Qn);
        }
        // kick: the game's gun averaged over 0.12 s while firing and while the kick eases out, from the pose firing began with.
        const bool track = fire_now || s.kick_a > 0.0f;
        bool       fresh = false;
        if (!track) {
            s.q_lp       = Q;
            s.q_lp_frame = m_gun_obj_seen;
            s.q_lp_t     = now;
        } else if (s.q_lp_frame != m_gun_obj_seen) {
            const float k = std::clamp(std::chrono::duration<float>(now - s.q_lp_t).count() / 0.12f, 0.0f, 1.0f);
            const Pose  a = to_pose(s.q_lp), b = to_pose(Q);
            s.q_lp        = to_mat(Pose{ glm::normalize(glm::slerp(a.r, b.r, k)), glm::mix(a.t, b.t, k) });
            s.q_lp_frame  = m_gun_obj_seen;
            s.q_lp_t      = now;
            fresh         = true;
        }
        s.kick_a = fire_now ? 1.0f : std::max(0.0f, s.kick_a - da / 0.15f);
        // gunstillfire kick: the game's gun off that average (the fast part of its firing animation) in the game camera's axes, cut
        // down to a recoil kick on the held pose: back toward you, up, and the muzzle turned up, each clamped (gunstillkick back lift
        // deg). None while the game holds its gun far round from the standing pose (its sprint pose turns it 76-85 deg) or moves it a
        // lot at once (out of the sprint pose, into aiming down sights): those are no kick.
        auto kick_of = [&](const Pose& held, const glm::mat4& Qx, float a, bool log) {
            const Pose lp = to_pose(s.q_lp);
            const Pose dp = to_pose(glm::inverse(s.q_lp) * Qx);   // the gun's own axes
            const glm::vec3 t  = lp.r * dp.t;                      // the game camera's axes (x right, y up, z back)
            glm::quat       rc = glm::normalize(lp.r * dp.r * glm::inverse(lp.r));
            if (rc.w < 0.0f) rc = -rc;
            const float     ang = 2.0f * std::acos(std::clamp(rc.w, -1.0f, 1.0f));
            const glm::vec3 ax{ rc.x, rc.y, rc.z };
            const glm::vec3 rv  = (ang > 1e-6f && glm::length(ax) > 1e-9f) ? glm::normalize(ax) * ang : glm::vec3{ 0.0f };
            const float size_cm = 100.0f * glm::length(t), size_deg = glm::degrees(ang), stance_deg = angle_deg(lp.r, held.r);
            const float g = std::clamp((12.0f - size_cm) / 6.0f, 0.0f, 1.0f) * std::clamp((25.0f - size_deg) / 13.0f, 0.0f, 1.0f) *
                            std::clamp((30.0f - stance_deg) / 10.0f, 0.0f, 1.0f);
            if (log && g < 1.0f) s.fs_gated++;
            const float back = std::clamp(t.z, 0.0f, m_gst_kick_back.load()) * g * a;
            const float lift = std::clamp(t.y, 0.0f, m_gst_kick_lift.load()) * g * a;
            const float up   = std::clamp(rv.x, 0.0f, glm::radians(m_gst_kick_deg.load())) * g * a;
            Pose out = held;
            out.t += glm::vec3{ 0.0f, lift, back };
            out.r = glm::normalize(glm::angleAxis(up, glm::vec3{ 1.0f, 0.0f, 0.0f }) * held.r);
            if (log && m_gst_kick_log.load() > 0) {
                m_gst_kick_log--;
                spdlog::info("[GunKick] frame {} | the game's gun off its 0.12 s average: ({:+.1f}, {:+.1f}, {:+.1f}) cm, turn ({:+.1f}, {:+.1f}, {:+.1f}) deg (x right, y up, "
                             "z back) | its average {:.0f} deg from the standing pose | kept {:.2f} x {:.2f} | shown: back {:.1f} cm, up {:.1f} cm, muzzle up {:.1f} deg",
                             frame, 100.0f * t.x, 100.0f * t.y, 100.0f * t.z, glm::degrees(rv.x), glm::degrees(rv.y), glm::degrees(rv.z), stance_deg, g, a, 100.0f * back,
                             100.0f * lift, glm::degrees(up));
            }
            return out;
        };
        Pose target = ref.pose;
        if (fire_mode == 2 && s.kick_a > 0.0f) target = kick_of(ref.pose, Qn, s.kick_a, fresh);
        if (fire_mode == 2) Cf = to_mat(target) * glm::inverse(Qn);
    }
    // gunstillrecoil (Homard 16 Oct 21:29 "keep the recoil animation"): while firing (and 0.4 s after) the correction is
    // frozen at its value when firing began, so the game's kick of the gun shows on top of the held pose; afterwards it eases
    // back to the full hold over 0.25 s.
    // While firing (gunstillrecoil off, Homard 20 Oct 09:11 "very rapid but important weapon movement when pressing trigger"):
    // the correction follows a 0.12 s average of the game's pose, without extrapolation. The game writes the gun's pose only
    // right before drawing it, so the hold works from the previous frame's pose: the kick (2-7 cm per shot) then shook the
    // held gun by up to 10-13 cm / 21-25 deg from one frame to the next in AFW (simulator, rifle bursts). Around the average
    // the kick shows as the game animates it, small and smooth: 2-5.5 cm / 3-7 deg from frame to frame at most.
    const bool smooth = fire_mode == 0 && !m_gst_recoil.load();
    // Smoothed firing lasts while the game still holds the gun off its standing pose (2 cm / 3 deg): its shots and its way out of
    // the firing stance outlast the trigger (simulator: kicks 0.75 s after release, then an 8 cm / 15 deg one-frame exit).
    const bool stance = smooth && s.fire_on && (glm::length(qp.t - ref.pose.t) > 0.02f || angle_deg(qp.r, ref.pose.r) > 3.0f) && now - s.last_fire < std::chrono::seconds(3);
    const bool firing = fire_mode == 0 && (m_gst_recoil.load() || smooth) && (now - s.last_fire < std::chrono::milliseconds(400) || stance);
    if (smooth) {
        if (!s.fire_on) {
            s.q_lp       = Q;
            s.q_lp_frame = m_gun_obj_seen;
            s.q_lp_t     = now;
        } else if (s.q_lp_frame != m_gun_obj_seen) {
            const float k = std::clamp(std::chrono::duration<float>(now - s.q_lp_t).count() / 0.12f, 0.0f, 1.0f);
            const Pose  a = to_pose(s.q_lp), b = to_pose(Q);
            s.q_lp        = to_mat(Pose{ glm::normalize(glm::slerp(a.r, b.r, k)), glm::mix(a.t, b.t, k) });
            s.q_lp_frame  = m_gun_obj_seen;
            s.q_lp_t      = now;
        }
        if (firing) s.fire_C = to_mat(ref.pose) * glm::inverse(s.q_lp);
    } else if (firing && !s.fire_on) s.fire_C = Cf;
    if (fire_mode == 0) {
        s.fire_on   = firing;
        s.fire_beta = firing ? 0.0f : std::min(1.0f, s.fire_beta + da / 0.25f);
    }
    if (s.fire_beta < 1.0f) {
        const Pose a = to_pose(s.fire_C), b = to_pose(Cf);
        Cf           = to_mat(Pose{ glm::normalize(glm::slerp(a.r, b.r, s.fire_beta)), glm::mix(a.t, b.t, s.fire_beta) });
    }
    // gunstillstat's firing line: in the passes placed with their own gun pose (gunstilllate's, Stereo's right eye without
    // gunstillpair) while firing, what the game did with its gun from the standing pose and how far the drawn gun was from it.
    if ((fire_now || s.kick_a > 0.0f || s.fire_on) && m_gun_obj_seen == frame && s.fs_frame != frame) {
        s.fs_frame       = frame;
        s.fs_n++;
        const Pose  shown = to_pose(Cf * Q);
        const float gc = 100.0f * glm::length(qp.t - ref.pose.t), gd = angle_deg(qp.r, ref.pose.r);
        const float sc = 100.0f * glm::length(shown.t - ref.pose.t), sd = angle_deg(shown.r, ref.pose.r);
        s.fs_game_cm += gc;
        s.fs_game_max_cm   = std::max(s.fs_game_max_cm, gc);
        s.fs_game_max_deg  = std::max(s.fs_game_max_deg, gd);
        s.fs_shown_max_cm  = std::max(s.fs_shown_max_cm, sc);
        s.fs_shown_max_deg = std::max(s.fs_shown_max_deg, sd);
    }
    const glm::quat cq = glm::normalize(glm::quat_cast(glm::mat3{ Cf }));
    glm::mat4       C  = glm::mat4_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, cq, s.alpha));
    C[3]               = glm::vec4{ glm::vec3{ Cf[3] } * s.alpha, 1.0f };
    s.applied_C        = C;
    s.applied_C_frame  = frame;
    s.applied_full     = s.alpha >= 1.0f;
    m_gs_frames_held++;
    return C;
}

// The weapon's held (standing) pose in the game camera's axes, once learned: where gunstill draws the gun (sightaim).
bool FarCry5Stereo::gun_still_held(uint32_t sig, glm::mat4& pose)
{
    if (!m_gst || !m_gun_still.load()) return false;
    const auto it = m_gst->refs.find(sig);
    if (it == m_gst->refs.end() || !it->second.ok) return false;
    pose = to_mat(it->second.pose);
    return true;
}

bool FarCry5Stereo::gun_still_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "gunstill") { m_gun_still = on; persist = true; return true; }
    if (cmd == "gunstilllog") { m_gun_still_log = arg.empty() ? 90 : (int)fv; return true; }
    if (cmd == "gunstillhand") { m_gst_hand_cm = std::clamp(fv, 2.0f, 40.0f); return true; }   // cm the left hand leaves its place for a reload
    if (cmd == "gunstillease") { m_gst_ease = std::clamp(fv, 0.02f, 1.0f); return true; }      // s to the game's pose and back
    if (cmd == "gunstillrecoil") { m_gst_recoil = on; persist = true; return true; }   // with gunstillfire game only
    if (cmd == "gunstillfire") {   // hold (default) | kick | game: the gun while firing (game = gunstillrecoil's way, before 24 Oct)
        m_gst_fire = arg == "game" ? 0 : arg == "kick" ? 2 : 1;
        persist    = true;
        spdlog::info("[Motion] gunstillfire {}: {}", arg == "game" ? "game" : arg == "kick" ? "kick" : "hold",
                     arg == "game" ? "the game's firing animation as gunstillrecoil sets it (before 24 Oct)" : arg == "kick" ? "only a recoil kick shows on the held gun" : "the gun stays held while firing");
        return true;
    }
    if (cmd == "gunstillkick") {   // test: the kick's limits, back and up in m, the muzzle's turn in deg
        float b = fv, l = m_gst_kick_lift.load(), d = m_gst_kick_deg.load();
        rest >> l >> d;
        m_gst_kick_back = std::clamp(b, 0.0f, 0.15f);
        m_gst_kick_lift = std::clamp(l, 0.0f, 0.10f);
        m_gst_kick_deg  = std::clamp(d, 0.0f, 30.0f);
        return true;
    }
    if (cmd == "gunkicklog") { m_gst_kick_log = arg.empty() ? 120 : (int)fv; return true; }   // test: one [GunKick] line per update while firing (kick)
    if (cmd == "gunstillpair") { m_gst_pair = on; persist = true; return true; }   // Stereo: the right eye takes the left eye's hold (both eyes the same gun)
    if (cmd == "gunstilllate") { m_gst_late = on; persist = true; return true; }   // the guessed pass's weapon camera placed again at the gun mesh's draw (both eyes exact)
    if (cmd == "gunstillpredict") {   // test: the next update's gun guessed by hold | linear | timed | half
        m_gst_predict = arg == "hold" ? 0 : arg == "timed" ? 2 : arg == "half" ? 3 : 1;
        return true;
    }   // the game's recoil kick kept whole while firing (off: smoothed)
    if (cmd == "twohandblock") {   // twohandblock <sig> <sig> ... | none: the weapons never held with two hands
        std::set<uint32_t> s;
        auto               add = [&](const std::string& w) { if (const unsigned long v = std::strtoul(w.c_str(), nullptr, 10); v != 0) s.insert(uint32_t(v)); };
        add(arg);
        for (std::string w; rest >> w;) add(w);
        {
            std::lock_guard _{ m_weapon_cal_mtx };
            m_two_hand_block = std::move(s);
        }
        persist = true;
        return true;
    }
    if (cmd == "twohandline") { m_two_hand_line = on; persist = true; return true; }   // the two-handed hold lines the gun's hand line up with your hands
    if (cmd == "fireready") { m_fire_ready = on; persist = true; return true; }   // the game's aim kept on the barrel with the weapon in hand (motion_post_present)
    if (cmd == "snapready") { m_snap_ready = on; persist = true; return true; }   // snap turns synced with the culled camera with the weapon in hand too
    if (cmd == "vehpitchguard") { m_veh_pitch_guard = on; return true; }   // a car's pitch read against the head look is kept (vehicle follow)
    if (cmd == "settlepitch") { m_settle_pitch = on; return true; }   // the culled camera looks up / down with the head while getting in
    if (cmd == "wheelforce") { m_wheel_force = on; return true; }   // test: the aim loop as with the weapon wheel open
    if (cmd == "wheellook") { m_wheel_look = on; return true; }   // the culled camera on the head while the weapon wheel is open
    if (cmd == "bodystarthead") { m_body_start_head = on; return true; }   // the body frame starts from the head (else the controller)
    if (cmd == "aimbacklimit") { m_aim_back_limit = arg == "off" ? 0.0f : std::clamp(fv, 0.0f, 180.0f); return true; }   // the game's aim at most N deg round from the head (0/off: no limit)
    if (cmd == "splitevpitch") { m_split_ev_pitch = std::clamp(fv, 69.0f, 160.0f); return true; }   // firing offset pitch limit (handed deg)
    if (cmd == "splitevmax") { m_split_ev_max = std::clamp(fv, 0.0f, 360.0f); return true; }   // firing offset limit (handed deg; x0.75 applied)
    if (cmd == "gunstillshot") { g_hand_needs_shot = on; return true; }
    if (cmd == "gunstillpad") { g_pad_waits_hand = on; return true; }                  // X shows the game's animation once the hand leaves the gun                // the left hand off the gun is a reload only right after shooting
    if (cmd == "gunstillwrong") {
        // Test: the current weapon's held pose moved D m down and D/2 m back, and its left hand's place 12 cm (Homard 17 Oct
        // 15:43: the pose learned 36 cm low and 23 cm back, the hand's place with it), to show how the hold recovers.
        if (!m_gst) return true;
        auto it = m_gst->refs.find(m_gst->sig);
        if (it == m_gst->refs.end() || !it->second.ok) {
            spdlog::info("[Motion] gunstillwrong: no held pose for this weapon yet");
            return true;
        }
        const float d = arg.empty() ? 0.36f : fv;
        it->second.pose.t += glm::vec3{ 0.0f, -d, 0.5f * d };
        it->second.hand += glm::vec3{ 0.0f, 0.0f, 0.12f };
        spdlog::info("[Motion] gunstillwrong: weapon {}'s held pose moved {:.2f} m down and {:.2f} m back, its left hand's place 12 cm", m_gst->sig, d, 0.5f * d);
        return true;
    }
    if (cmd == "gunstillreset") { gun_still_forget(); return true; }               // learn every weapon's pose again (F11 Comfort button)
    if (cmd == "gunstillstat") {
        // Since the last gunstillstat: held passes, the drawn gun's and the game's gun's distance from the standing pose.
        if (!m_gst) m_gst = new GunStillState{};
        auto& s = *m_gst;
        const float n = float(std::max<uint64_t>(1, s.st_n));
        spdlog::info("[GunStill] stat: {} held passes | drawn gun from its standing pose: mean {:.2f} cm, max {:.2f} cm / {:.2f} deg | the game's gun: mean {:.2f} cm, "
                     "max {:.2f} cm / {:.2f} deg | reloads {} | speed {:.1f} m/s{} | hand-offs not taken for a reload (no shot just before) {} | gun transform at the pass's "
                     "first draw: {} current, {} a frame old, {} other | drawn gun's change pass to pass: mean {:.2f} cm, max {:.2f} cm / {:.2f} deg",
                     s.st_n, s.st_drawn_cm / n, s.st_drawn_max_cm, s.st_drawn_max_deg, s.st_game_cm / n, s.st_game_max_cm, s.st_game_max_deg, s.reloads, s.speed, s.sprinting ? " (sprinting)" : "", s.hand_skips,
                     s.first_current, s.first_stale, s.first_other, s.st_step_cm / float(std::max<uint64_t>(1, s.st_steps)), s.st_step_max_cm, s.st_step_max_deg);
        // Per eye (L = the game call's pass, R = the extra call's in Stereo): mean / max off the standing pose, the change from that
        // eye's own last pass, and in Stereo the two eyes of one update against each other; then each way of guessing the next
        // update's gun against the pose the game drew it with (the predicted passes: Stereo left eye, every AFW frame).
        auto u   = [](uint64_t v) { return float(std::max<uint64_t>(1, v)); };
        auto eye = [&](int e) {
            return fmt::format("{} passes, off {:.3f} cm mean, {:.2f} cm / {:.2f} deg max, change from its last pass {:.3f} cm mean, {:.2f} cm / {:.2f} deg max", s.ey_n[e], s.ey_cm[e] / u(s.ey_n[e]),
                               s.ey_max_cm[e], s.ey_max_deg[e], s.ey_step_cm[e] / u(s.ey_steps[e]), s.ey_step_max_cm[e], s.ey_step_max_deg[e]);
        };
        static const char* kWay[]{ "hold", "linear", "timed", "half" };
        std::string ways;
        for (int k = 0; k < 4; ++k)
            ways += fmt::format(" | {}{} {:.3f} cm / {:.3f} deg mean, {:.2f} cm / {:.2f} deg max", kWay[k], k == m_gst_predict.load() ? " (used)" : "", s.pr_cm[k] / u(s.pr_n),
                                s.pr_deg[k] / u(s.pr_n), s.pr_max_cm[k], s.pr_max_deg[k]);
        spdlog::info("[GunStill] per eye ({}, gunstillpair {}, {} right passes shared): L {} | R {} | L vs R of one update: {} pairs, {:.3f} cm mean, {:.2f} cm / {:.2f} deg max",
                     afw_mode() ? "AFW" : "Stereo", m_gst_pair.load() ? "on" : "off", s.pair_shared, eye(0), eye(1), s.pd_n, s.pd_cm / u(s.pd_n), s.pd_max_cm, s.pd_max_deg);
        spdlog::info("[GunStill] guesses of the next update's gun, {} predicted passes ({} timed, span ratio {:.2f}..{:.2f}){}", s.pr_n, s.pr_timed, s.pr_timed ? s.pr_r_min : 0.0f,
                     s.pr_r_max, ways);
        // gunstillfire: the firing passes placed with their own gun pose (from the trigger to 0.4 s after it, the kick's ease out,
        // gunstillfire game's stance time).
        spdlog::info("[GunStill] firing (gunstillfire {}): {} exact passes | the game's gun from the standing pose: mean {:.2f} cm, max {:.2f} cm / {:.2f} deg | drawn gun "
                     "from it: max {:.2f} cm / {:.2f} deg | kick cut down (sprint pose, large moves) in {} updates",
                     m_gst_fire.load() == 0 ? "game" : m_gst_fire.load() == 2 ? "kick" : "hold", s.fs_n, s.fs_game_cm / float(std::max<uint64_t>(1, s.fs_n)), s.fs_game_max_cm,
                     s.fs_game_max_deg, s.fs_shown_max_cm, s.fs_shown_max_deg, s.fs_gated);
        s.fs_n = s.fs_gated = 0;
        s.fs_game_cm = s.fs_game_max_cm = s.fs_game_max_deg = s.fs_shown_max_cm = s.fs_shown_max_deg = 0.0f;
        // gunstilllate: the per-eye numbers above are measured with the camera each pass was first placed with (its draws before
        // the gun mesh); with gunstilllate on, the gun mesh and the draws after it use the camera placed again (the pass's own pose).
        const uint64_t lt = m_gst_late_n + m_gst_late_miss + m_gst_late_fail;
        spdlog::info("[GunStill] gunstilllate parts: {} passes placed at their first part (the gun's pose from a part's, learned the update before), {} "
                     "without a known part | that guess against the gun mesh's own pose: {:.3f} cm / {:.3f} deg mean, {:.2f} cm / {:.2f} deg max, {} over 0.5 mm (or the coordinates' float rounding) / 0.1 deg",
                     m_gst_est_n, m_gst_est_none, m_gst_est_cm / double(std::max<uint64_t>(1, m_gst_est_chk)), m_gst_est_deg / double(std::max<uint64_t>(1, m_gst_est_chk)),
                     m_gst_est_max_cm, m_gst_est_max_deg, m_gst_est_bad);
        spdlog::info("[GunStill] gunstilllate parts (test): part draws {} without kept object constants (last b1 {} B), {} with; {} looked up "
                     "in a guessed pass; {} gun meshes reached the learning, {} parts learned there",
                     m_gst_dbg_noobj, m_gst_dbg_noobj_size, m_gst_dbg_seen, m_gst_dbg_tries, m_gst_dbg_mesh, m_gst_dbg_learn);
        m_gst_dbg_noobj = m_gst_dbg_seen = m_gst_dbg_tries = m_gst_dbg_mesh = m_gst_dbg_learn = 0;
        m_gst_est_n = m_gst_est_none = m_gst_est_chk = m_gst_est_bad = 0;
        m_gst_est_cm = m_gst_est_deg = 0.0;
        m_gst_est_max_cm = m_gst_est_max_deg = 0.0f;
        spdlog::info("[GunStill] gunstilllate {}: {} passes placed again at the gun mesh's draw, {} not (another camera buffer bound or a stereo recording), {} failed "
                     "maps | draws before the gun mesh: {:.1f} mean ({:.1f} gloves), {} max",
                     m_gst_late.load() ? "on" : "off", m_gst_late_n, m_gst_late_miss, m_gst_late_fail, double(m_gst_late_pre_sum) / double(std::max<uint64_t>(1, lt)),
                     double(m_gst_late_pre_gloves_sum) / double(std::max<uint64_t>(1, lt)), m_gst_late_pre_max);
        m_gst_late_n = m_gst_late_miss = m_gst_late_fail = m_gst_late_pre_sum = m_gst_late_pre_gloves_sum = m_gst_late_pre_max = 0;
        for (int e = 0; e < 2; ++e) {
            s.ey_n[e] = s.ey_steps[e] = 0;
            s.ey_cm[e] = s.ey_max_cm[e] = s.ey_max_deg[e] = s.ey_step_cm[e] = s.ey_step_max_cm[e] = s.ey_step_max_deg[e] = 0.0f;
        }
        s.pd_n = s.pair_shared = s.pr_n = s.pr_timed = 0;
        s.pd_cm = s.pd_max_cm = s.pd_max_deg = 0.0f;
        for (int k = 0; k < 4; ++k) s.pr_cm[k] = s.pr_max_cm[k] = s.pr_deg[k] = s.pr_max_deg[k] = 0.0f;
        s.pr_r_min = 1e9f;
        s.pr_r_max = 0.0f;
        s.st_steps = 0;
        s.st_step_cm = s.st_step_max_cm = s.st_step_max_deg = 0.0f;
        s.st_n = 0;
        s.st_drawn_cm = s.st_drawn_max_cm = s.st_drawn_max_deg = s.st_game_cm = s.st_game_max_cm = s.st_game_max_deg = 0.0f;
        return true;
    }
    return false;
}

// Every weapon's learned pose forgotten (cmd gunstillreset, F11 Comfort button): each is learned again once it is in hand,
// standing still.
void FarCry5Stereo::gun_still_forget()
{
    if (!m_gst) return;
    m_gst->refs.clear();
    gun_still_save();   // the file emptied too
    spdlog::info("[Motion] gunstill: every weapon's pose forgotten; each is learned again in hand, standing still");
}

// Each weapon's first learned pose, kept across sessions (bin\fc5vr_gunposes.txt, one line per weapon: signature, position,
// rotation quaternion x y z w, the left hand's place, whether it was seen). Homard 20 Oct 16:19: a session that never stood
// still with a weapon only had a first guess for it.
void FarCry5Stereo::gun_still_load()
{
    if (!m_gst) return;
    std::ifstream f{ fc5::mod_file("fc5vr_gunposes.txt") };
    std::string   line;
    int           n = 0;
    int           grips = 0;
    while (std::getline(f, line)) {
        std::istringstream ls{ line };
        if (line.rfind("grip ", 0) == 0) {
            std::string tag;
            uint32_t    gsig = 0;
            glm::vec3   c{ 0.0f };
            if ((ls >> tag >> gsig >> c.x >> c.y >> c.z) && gsig != 0 && glm::length(c) < 1.0f) {
                std::lock_guard _{ m_weapon_cal_mtx };
                auto& gh = m_grip_hand[gsig];
                gh.c     = c;
                gh.n     = 90;
                gh.ok    = true;
                ++grips;
            }
            continue;
        }
        uint32_t           sig = 0;
        Pose               p{};
        glm::vec3          h{ 0.0f };
        int                hok = 0;
        if (!(ls >> sig >> p.t.x >> p.t.y >> p.t.z >> p.r.x >> p.r.y >> p.r.z >> p.r.w >> h.x >> h.y >> h.z >> hok) || sig == 0) continue;
        if (glm::length(p.r) < 0.5f || !(p.t.z < -0.10f && p.t.y > -0.45f)) continue;   // the same checks as learning in hand
        auto& r         = m_gst->refs[sig];
        r.pose          = Pose{ glm::normalize(p.r), p.t };
        r.hand          = h;
        r.hand_ok       = hok != 0;
        r.first         = r.pose;
        r.first_hand    = h;
        r.first_hand_ok = r.hand_ok;
        r.ok = r.settled = r.first_ok = true;
        ++n;
    }
    if (n || grips) spdlog::info("[Motion] gunstill: {} weapons' poses and {} grip points read from fc5vr_gunposes.txt", n, grips);
}

void FarCry5Stereo::gun_still_save()
{
    if (!m_gst) return;
    std::ofstream f{ fc5::mod_file("fc5vr_gunposes.txt"), std::ios::trunc };
    int           n = 0;
    for (const auto& [sig, r] : m_gst->refs) {
        if (!r.first_ok) continue;
        f << sig << ' ' << r.first.t.x << ' ' << r.first.t.y << ' ' << r.first.t.z << ' ' << r.first.r.x << ' ' << r.first.r.y << ' ' << r.first.r.z << ' ' << r.first.r.w << ' '
          << r.first_hand.x << ' ' << r.first_hand.y << ' ' << r.first_hand.z << ' ' << (r.first_hand_ok ? 1 : 0) << '\n';
        ++n;
    }
    // gripbyhand's measured grip points ("grip" lines): measured only once a weapon has been out 120 frames plus 90 draws, so
    // each first draw of a session sat 19-25 cm off the hand for about 2 s, then jumped (Homard 21 Oct 12:49: "the delay to the
    // correct position when switching weapon").
    int g = 0;
    {
        std::lock_guard _{ m_weapon_cal_mtx };
        for (const auto& [sig, gh] : m_grip_hand) {
            if (!gh.ok) continue;
            f << "grip " << sig << ' ' << gh.c.x << ' ' << gh.c.y << ' ' << gh.c.z << '\n';
            ++g;
        }
    }
    spdlog::info("[Motion] gunstill: {} weapons' poses and {} grip points saved to fc5vr_gunposes.txt", n, g);
}

// twohandblock: weapons never held with two hands (Homard 21 Oct 17:52, the Uzi: its two-handed hold stayed wrong with
// twohandline). Saved as "twohandblock <sig> <sig> ..." in fc5vr_settings.txt.
bool FarCry5Stereo::two_hand_blocked(uint32_t sig)
{
    std::lock_guard _{ m_weapon_cal_mtx };
    return m_two_hand_block.contains(sig);
}

std::string FarCry5Stereo::two_hand_block_line()
{
    std::lock_guard _{ m_weapon_cal_mtx };
    std::string     s = "twohandblock";
    if (m_two_hand_block.empty()) s += " none";
    for (uint32_t v : m_two_hand_block) s += " " + std::to_string(v);
    return s;
}
