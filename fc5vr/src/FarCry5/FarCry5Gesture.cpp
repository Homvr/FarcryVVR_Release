#include "FarCry5Gesture.h"

#include <windows.h>
#include <Xinput.h>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <istream>
#include <mutex>

namespace fc5::gesture
{
namespace
{
    using clock = std::chrono::steady_clock;

    // Where a zone sits on the body. The shoulders hang from the neck and turn with the body's heading (see shoulder()), the sling
    // from the head and the body's heading (see sling_pose()).
    enum class Anchor { WeaponShoulder, OtherShoulder, Sling, Chest };

    // Tap: a grip press in the zone taps pad buttons once and that press is not the grip's usual action.
    // Hold: a grip press in the zone holds something until the grip is released (the sling: the weapon in hand);
    //       the grip keeps its usual action meanwhile (aiming down sights, holding the gun).
    enum class Kind { Tap, Hold };

    struct Def
    {
        const char* name;       // command name: "<name> on|off", "<name>size <m>"
        const char* label;      // F11 Controls checkbox
        const char* hint;       // F11 Controls hint
        Kind        kind;
        Hand        hand;
        Anchor      anchor;
        uint16_t    buttons;    // Tap: pad buttons tapped once per grip press
        int         tap_ms;     // how long they are held (short: the game's Y held is "heal")
        float       radius;     // default zone size (m)
        bool        on;         // default
        bool        needs_weapon;   // only with a weapon in the game's hands
    };

    // FC5 default pad layout: Y tapped = switch to the previous weapon (held = heal), as right B sends it.
    const Def kDefs[]{
        { "shoulderswitch", "Right hand at the right shoulder + grip: switch weapon",
          "Reach to your right shoulder (as if taking a gun from your back) and press the grip: the game switches to your other weapon (pad Y, tapped). "
          "That grip press does not aim or hold the gun. Left-handed: the left hand at the left shoulder.",
          Kind::Tap, kWeaponHand, Anchor::WeaponShoulder, XINPUT_GAMEPAD_Y, 120, 0.20f, true, false },
        { "sling", "Sling: the weapon hangs in front of you until you grab it",
          "The current weapon hangs across your chest. Put your right hand on its grip and hold the grip button: the weapon is in your hand while you hold it, "
          "and goes back on the sling when you let go. On the sling it does not fire. Off: the weapon is always in your hand (as before).",
          Kind::Hold, kWeaponHand, Anchor::Sling, 0, 0, 0.18f, true, true },
        { "throwable", "Throwable on the left chest: take it with the left grip, let go while swinging to throw",
          "The selected grenade (or other throwable) sits on the left of your chest. Put your left hand there and hold the grip: it is in your hand. "
          "Swing and let go of the grip: the game throws it the way your hand was moving (let go without a swing: where you look). "
          "That grip press is not the left grip's usual modifier or two-handed hold. Off (default, experimental): nothing on the chest; throw with the "
          "game's own button (left grip + right B).",
          Kind::Hold, kOtherHand, Anchor::Chest, 0, 0, 0.22f, false, false },   // off by default (release 0.12.1, Homard 25 Oct 12:40)
    };
    constexpr int kCount = int(sizeof(kDefs) / sizeof(kDefs[0]));
    constexpr int kSling = 1;
    constexpr int kThrow = 2;

    struct State
    {
        std::atomic<bool>  on{ true };
        std::atomic<float> radius{ 0.2f };
        std::atomic<float> dist{ -1.0f };
        bool               owned{ false };   // the current grip press began in the zone (Hold: the thing is held)
        bool               inside{ false };  // the hand in the zone (with a little hysteresis), for the entry buzz
        clock::time_point  tap_until{};
        std::atomic<bool>  shown_avail{ false }, shown_inside{ false }, shown_held{ false };   // for gesturezones (the render thread)
    };
    State g_state[kCount];
    bool  g_init = false;
    bool  g_grip_was[2]{};

    // Shoulder from the eyes when looking straight ahead, metres: to the side, below the eyes, behind them; it hangs from the neck and
    // turns with the body's heading (see shoulder()). Default: the shoulder joint behind and below the ear, ~0.18 m from the body's centre
    // line, ~0.25 m below the eyes, ~0.13 m behind them (Homard 24 Oct 13:29).
    constexpr float    kShDefault[3]{ 0.18f, 0.25f, 0.13f };
    std::atomic<float> g_sh_side{ kShDefault[0] }, g_sh_down{ kShDefault[1] }, g_sh_back{ kShDefault[2] };
    std::atomic<bool>  g_test_grip{ false };
    std::atomic<bool>  g_zone_buzz{ true };   // gesturebuzz: a light buzz when the free hand enters a zone
    std::atomic<bool>  g_zone_show{ false };  // gesturezones (debug): the zones drawn in both eyes
    std::atomic<float> g_hand_pt[2][3]{};     // each hand's tested point at the last poll (tracking space), for gesturezones
    std::atomic<bool>  g_hand_pt_ok[2]{};

    // Sling: where the weapon's grip hangs from the head (body frame: right, down, forward, metres) and how the
    // weapon is turned there (degrees: barrel to the left, barrel down, roll). Defaults: grip by the right hip,
    // the barrel across the belly towards the left, pointing down (a two-point sling's carry).
    constexpr float    kSlPosDefault[3]{ 0.09f, 0.50f, 0.10f }, kSlRotDefault[3]{ 70.0f, -30.0f, 0.0f };
    std::atomic<float> g_sl_pos[3]{ kSlPosDefault[0], kSlPosDefault[1], kSlPosDefault[2] };
    std::atomic<float> g_sl_rot[3]{ kSlRotDefault[0], kSlRotDefault[1], kSlRotDefault[2] };
    // The sling zone's centre from the weapon's grip (body frame: right, down, forward, metres); 0: around the grip.
    std::atomic<float> g_sl_zone[3]{};
    // The body's heading (tracking space, radians): follows the head's once it turns more than kBodyLag away,
    // so a glance aside does not swing the weapon.
    std::atomic<float> g_body_yaw{ 0.0f };
    bool               g_body_init = false;
    constexpr float    kBodyLag = 0.61f;   // 35 deg
    std::atomic<bool>  g_slung{ false };

    // Throwable: where it sits from the head (body frame: to the other hand's side, down, forward, metres). Default: the
    // left chest pocket, 10 cm left of the centre line, 30 cm below the eyes, 8 cm in front of them.
    constexpr float    kThPosDefault[3]{ 0.10f, 0.30f, 0.08f };
    std::atomic<float> g_th_pos[3]{ kThPosDefault[0], kThPosDefault[1], kThPosDefault[2] };
    // Release: a swing slower than throwmin (m/s) puts the throwable back. The camera has throwwait ms to reach the throw's
    // direction (within throwgate deg) before RB goes anyway; it is held on that direction throwkeep ms after RB.
    // throwhold on: RB is held from the grab (the game raises the throwable in the left hand) and released to throw;
    // off: RB is tapped (throwtap ms) at the release.
    std::atomic<float> g_th_min{ 1.0f }, g_th_gate{ 2.0f };
    std::atomic<int>   g_th_wait{ 300 }, g_th_keep{ 700 }, g_th_tap{ 120 };
    std::atomic<bool>  g_th_hold{ true }, g_th_log{ true };
    std::atomic<int>   g_th_phase{ 0 };
    clock::time_point  g_th_since{};
    std::mutex         g_th_mtx;
    glm::vec3          g_th_dir{ 0.0f, 0.0f, -1.0f };   // tracking space, unit
    float              g_th_speed{ 0.0f };
    // The other hand's recent path (tracking space), for the release velocity.
    struct Sample { clock::time_point t; glm::vec3 p; };
    constexpr int     kHist = 48;
    Sample            g_hist[kHist]{};
    int               g_hist_n = 0, g_hist_at = 0;
    // throwtest vx vy vz [ms [hold_ms]] (simulator): the other hand grabs the throwable, then swings through the chest zone at that
    // velocity (head frame: right, up, back; m/s) for ms (after hold_ms in the hand) and lets go.
    // padtap <hex buttons> <ms> (test): pad buttons pressed for ms (which game input cancels a held throw?).
    std::atomic<int>   g_padtap_buttons{ 0 };
    clock::time_point  g_padtap_until{};
    std::atomic<bool>  g_tt_req{ false };
    std::atomic<float> g_tt_v[3]{};
    std::atomic<int>   g_tt_ms{ 150 }, g_tt_hold{ 600 };
    int                g_tt_step = 0;   // 0 off, 1 grab, 2 swing, 3 released
    clock::time_point  g_tt_t0{};
    glm::vec3          g_tt_p0{ 0.0f }, g_tt_vw{ 0.0f };

    // throwv2 (see the header): the hold, the trajectory, RB held for the wind-up, the peek.
    std::atomic<bool>  g_v2{ false }, g_v2_right{ false }, g_v2_rb_arm{ true }, g_preaim{ true }, g_rock{ false };   // throwrock off by default: a throwable the game draws only when thrown looks like "none left" to the peek
    std::atomic<bool>  g_rock_now{ false };   // this throwable is a rock (throwrock)
    std::atomic<bool>  g_v2_held{ false }, g_v2_arc{ false }, g_v2_rb{ false }, g_th_launch{ false };
    bool               g_v2_armed = false, g_v2_was_armed = false;
    // Simulator 25 Oct: with RB held the game shows "Cancel attack (X)"; X, LB and Y each kept the knife (no object flew), A, B, the sticks did not.
    std::atomic<bool>  g_peek_on{ true }, g_peek_again{ false }, g_peek_seen{ false }, g_peeking{ false };
    std::atomic<int>   g_peek_cancel{ 0x4000 }, g_peek_cancel_ms{ 150 };
    std::atomic<bool>  g_t2_req{ false };   // throwtest2 hold_ms arm_ms (test)
    std::atomic<int>   g_t2_hold{ 800 }, g_t2_arm{ 1500 };
    bool               g_t2_on = false;
    clock::time_point  g_t2_t0{};
    glm::vec3          g_t2_p0{ 0.0f };
    int                g_peek_step = 0;   // 0 off, 1 RB held until seen, 2 cancel tapped (RB held), 3 RB let go
    clock::time_point  g_peek_t{}, g_allowed_since{}, g_peek_quiet{};
    bool               g_allowed_was = false, g_wheel_was = false, g_peek_failed = false;
    int                g_peek_fails = 0;
    bool               g_peek_twice = false;   // the peek saw nothing: X with RB twice before RB goes

    // The hand a gesture's zone answers to: the throwable's is the weapon hand with throwv2hand right (the zone stays on the left chest).
    Hand def_hand(int i) { return (i == kThrow && g_v2.load() && g_v2_right.load()) ? kWeaponHand : kDefs[i].hand; }

    // throwv2zone (saved, default 0.12 m): the throwable's zone with throwv2, smaller than throwablesize's 0.22 (headset 25 Oct 11:15, the
    // right hand: presses meant for the sling 0.30 m off its centre went to the throwable 0.18-0.21 m off, then thrown; the throwable's own
    // grabs were 0.02-0.04 m off).
    std::atomic<float> g_v2_zone{ 0.12f };
    float zone_radius(int i) { return (i == kThrow && g_v2.load()) ? std::min(g_state[i].radius.load(), g_v2_zone.load()) : g_state[i].radius.load(); }

    // Haptic pulses: a light tick when a free hand enters a zone, a firmer one when a gesture fires.
    constexpr float kEnterAmp = 0.25f, kEnterSec = 0.03f, kFireAmp = 0.6f, kFireSec = 0.08f;
    void buzz(Output& o, int hand, float amp, float sec)
    {
        if (amp > o.buzz[hand]) {
            o.buzz[hand]   = amp;
            o.buzz_s[hand] = sec;
        }
    }

    void init()
    {
        if (g_init) return;
        g_init = true;
        for (int i = 0; i < kCount; ++i) {
            g_state[i].on     = kDefs[i].on;
            g_state[i].radius = kDefs[i].radius;
        }
    }

    glm::vec3 level_right(const glm::mat4& head)
    {
        const glm::vec3 r{ head[0].x, 0.0f, head[0].z };
        return glm::length(r) > 1e-3f ? glm::normalize(r) : glm::vec3{ 1.0f, 0.0f, 0.0f };
    }

    // The neck's pivot from the eyes (head frame: down, back; m): the shoulders hang from it in the body's frame, so turning or
    // tilting the head does not move them (Homard 24 Oct 13:29: "it should be the right shoulder ... and not move when you look on
    // the right or left"). Body frame: x right, y up, +z back at body yaw 0; with the head level and facing the body's heading this is
    // the old point (side / below the eyes / behind the eyes), so gestureshoulder keeps its meaning. side = +1 the right shoulder, -1 the left.
    constexpr float kNeckDown = 0.10f, kNeckBack = 0.08f;
    glm::vec3 shoulder(const glm::mat4& head, float side)
    {
        const glm::vec3 neck = glm::vec3{ head[3] } + glm::mat3{ head } * glm::vec3{ 0.0f, -kNeckDown, kNeckBack };
        const glm::mat3 body = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f }) };
        return neck + body * glm::vec3{ side * g_sh_side.load(), -(g_sh_down.load() - kNeckDown), g_sh_back.load() - kNeckBack };
    }

    // Heading of the head, radians, 0 = facing -z, positive = turned left (about +y).
    float head_yaw(const glm::mat4& head)
    {
        const glm::vec3 f = glm::cross(glm::vec3{ 0.0f, 1.0f, 0.0f }, level_right(head));
        return std::atan2(-f.x, -f.z);
    }

    void follow_body(const glm::mat4& head)
    {
        const float hy = head_yaw(head);
        if (!g_body_init) {
            g_body_init = true;
            g_body_yaw  = hy;
            return;
        }
        const float d = std::remainder(hy - g_body_yaw.load(), 6.2831853f);
        if (std::abs(d) > kBodyLag) g_body_yaw = std::remainder(g_body_yaw.load() + d - std::copysign(kBodyLag, d), 6.2831853f);
    }

    glm::vec3 anchor_pos(Anchor a, const Input& in)
    {
        const float weapon_side = in.mirror ? -1.0f : 1.0f;
        switch (a) {
        case Anchor::WeaponShoulder: return shoulder(in.head, weapon_side);
        case Anchor::OtherShoulder: return shoulder(in.head, -weapon_side);
        case Anchor::Sling: {
            const float     m    = in.mirror ? -1.0f : 1.0f;
            const glm::mat3 body = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f }) };
            return glm::vec3{ sling_pose(in.head, in.mirror)[3] } + body * glm::vec3{ m * g_sl_zone[0].load(), -g_sl_zone[1].load(), -g_sl_zone[2].load() };
        }
        case Anchor::Chest: return glm::vec3{ throw_pose(in.head, in.mirror)[3] };
        }
        return glm::vec3{ in.head[3] };
    }

    void hist_add(const clock::time_point t, const glm::vec3& p)
    {
        // A pose read again before the runtime gave a new one (several pad polls per frame) is not a sample.
        if (g_hist_n > 0) {
            const Sample& last = g_hist[(g_hist_at + kHist - 1) % kHist];
            if (last.p == p || t - last.t < std::chrono::milliseconds(3)) return;
        }
        g_hist[g_hist_at] = { t, p };
        g_hist_at         = (g_hist_at + 1) % kHist;
        g_hist_n          = std::min(g_hist_n + 1, kHist);
    }

    // The hand's velocity at the release (tracking space, m/s), from its path over the last 160 ms: the fastest
    // stretch, fitted over the samples around it (least squares, ~4 samples). People slow the hand down before the grip
    // opens and the grip opens late, so the last samples alone give a weak, low throw; the peak keeps the swing's speed
    // and direction (the release-window idea of Meta's Interaction SDK RANSACVelocity and Ultraleap's SlidingWindowThrow).
    glm::vec3 release_velocity(const clock::time_point now, float& peak, int& used)
    {
        Sample s[kHist];
        int    n = 0;
        for (int i = 0; i < g_hist_n; ++i) {
            const Sample& x = g_hist[(g_hist_at + kHist - g_hist_n + i) % kHist];
            if (now - x.t <= std::chrono::milliseconds(160)) s[n++] = x;
        }
        peak = 0.0f;
        used = n;
        if (n < 3) return glm::vec3{ 0.0f };
        auto sec = [&](int i) { return std::chrono::duration<float>(s[i].t - s[0].t).count(); };
        int best = 1;
        for (int i = 1; i < n; ++i) {
            const float dt = sec(i) - sec(i - 1);
            if (dt <= 1e-4f) continue;
            const float v = glm::length(s[i].p - s[i - 1].p) / dt;
            if (v > peak) { peak = v; best = i; }
        }
        const int a = std::max(0, best - 2), b = std::min(n - 1, best + 1);
        float     tm = 0.0f;
        glm::vec3 pm{ 0.0f };
        for (int i = a; i <= b; ++i) { tm += sec(i); pm += s[i].p; }
        const float k = float(b - a + 1);
        tm /= k;
        pm /= k;
        float     stt = 0.0f;
        glm::vec3 stp{ 0.0f };
        for (int i = a; i <= b; ++i) { stt += (sec(i) - tm) * (sec(i) - tm); stp += (sec(i) - tm) * (s[i].p - pm); }
        used = b - a + 1;
        return stt > 1e-8f ? stp / stt : glm::vec3{ 0.0f };
    }

    void set_phase(ThrowPhase p, const clock::time_point now)
    {
        if (int(p) == g_th_phase.load()) return;
        static const char* const kName[]{ "idle", "held", "aim", "throw", "follow" };
        if (g_th_log.load()) spdlog::info("[Throw] {} -> {}", kName[g_th_phase.load()], kName[int(p)]);
        g_th_phase = int(p);
        g_th_since = now;
    }
}

glm::mat4 throw_pose(const glm::mat4& head, bool mirror)
{
    const float     m    = mirror ? 1.0f : -1.0f;   // the other hand's side: the left (the right when left-handed)
    const glm::mat4 body = glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f });
    return glm::translate(glm::mat4{ 1.0f }, glm::vec3{ head[3] }) * body *
           glm::translate(glm::mat4{ 1.0f }, glm::vec3{ m * g_th_pos[0].load(), -g_th_pos[1].load(), -g_th_pos[2].load() });
}

glm::mat4 sling_pose(const glm::mat4& head, bool mirror)
{
    const float     m    = mirror ? -1.0f : 1.0f;
    const glm::mat4 body = glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f });
    glm::mat4       p    = glm::translate(glm::mat4{ 1.0f }, glm::vec3{ head[3] }) * body *
                    glm::translate(glm::mat4{ 1.0f }, glm::vec3{ m * g_sl_pos[0].load(), -g_sl_pos[1].load(), -g_sl_pos[2].load() });
    p = p * glm::rotate(glm::mat4{ 1.0f }, glm::radians(m * g_sl_rot[0].load()), glm::vec3{ 0, 1, 0 }) *
        glm::rotate(glm::mat4{ 1.0f }, glm::radians(g_sl_rot[1].load()), glm::vec3{ 1, 0, 0 }) *
        glm::rotate(glm::mat4{ 1.0f }, glm::radians(m * g_sl_rot[2].load()), glm::vec3{ 0, 0, -1 });
    return p;
}

Output update(const Input& in_game)
{
    init();
    Output     out{};
    const auto now = clock::now();
    Input      in  = in_game;
    if (in.head_ok && g_tt_req.exchange(false)) {
        g_tt_step = 1;
        g_tt_t0   = now;
        g_tt_p0   = glm::vec3{ throw_pose(in.head, in.mirror)[3] };
        // Head frame (right, up, back) -> tracking space.
        g_tt_vw   = glm::mat3{ in.head } * glm::vec3{ g_tt_v[0].load(), g_tt_v[1].load(), g_tt_v[2].load() };
        spdlog::info("[Throw] test: grab at the chest, then a swing of ({:.2f}, {:.2f}, {:.2f}) m/s (head: right, up, back) for {} ms", g_tt_v[0].load(), g_tt_v[1].load(),
                     g_tt_v[2].load(), g_tt_ms.load());
    }
    if (g_tt_step > 0) {
        // throwtest: the other hand driven along a scripted path (grab, hold_ms held, the swing, let go). A stall of the
        // game (no poll for 60 ms, e.g. a capture) pauses the path, so the swing is sampled as a hand would be.
        static clock::time_point last_poll{};
        if (g_tt_step == 1 && now - last_poll > std::chrono::milliseconds(60) && last_poll > g_tt_t0) g_tt_t0 += now - last_poll;
        last_poll      = now;
        const float t  = std::chrono::duration<float>(now - g_tt_t0).count();
        const float ts = std::max(0.0f, t - g_tt_hold.load() / 1000.0f), te = g_tt_ms.load() / 1000.0f;
        auto&       h  = in.hand[kOtherHand];
        h.tracked      = true;
        h.pos          = g_tt_p0 + g_tt_vw * std::min(ts, te);
        h.grip         = ts < te;
        if (ts >= te) g_tt_step = 0;
    }
    if (in.head_ok && g_t2_req.exchange(false)) {
        g_t2_on = true;
        g_t2_t0 = now;
        g_t2_p0 = glm::vec3{ throw_pose(in.head, in.mirror)[3] };
        spdlog::info("[Throw] test v2: taken at the chest, held {} ms, then both grips for {} ms, then the right grip let go", g_t2_hold.load(), g_t2_arm.load());
    }
    if (g_t2_on) {
        // throwtest2 (throwv2): the holding hand taken to the chest and its grip pressed; after hold ms the other grip too (the trajectory);
        // after arm ms more the weapon hand's grip let go (the throw), 300 ms later everything let go.
        const int   hh = def_hand(kThrow);
        const float t  = std::chrono::duration<float>(now - g_t2_t0).count() * 1000.0f;
        const float a = float(g_t2_hold.load()), b = a + float(g_t2_arm.load());
        auto&       h  = in.hand[hh];
        if (t < 120.0f) {
            h.tracked = true;
            h.pos     = g_t2_p0;
        }
        h.grip = (hh == kOtherHand) ? t < b + 300.0f : t < b;
        in.hand[1 - hh].grip = (hh == kOtherHand) ? (t >= a && t < b) : (t >= a && t < b + 300.0f);
        if (t >= b + 400.0f) g_t2_on = false;
    }
    if (in.head_ok) follow_body(in.head);
    for (int h = 0; h < 2; ++h) {
        g_hand_pt_ok[h] = in.hand[h].tracked;
        for (int k = 0; k < 3; ++k) g_hand_pt[h][k] = in.hand[h].pos[k];
    }
    bool pressed[2]{};
    for (int h = 0; h < 2; ++h) {
        pressed[h]    = in.hand[h].grip && !g_grip_was[h];
        g_grip_was[h] = in.hand[h].grip;
    }
    // Per zone: its distance, whether it can act now, the entry buzz; releasing the grip ends a press.
    bool  active[kCount]{};
    float dist[kCount]{};
    for (int i = 0; i < kCount; ++i) {
        const Def&  d  = kDefs[i];
        State&      s  = g_state[i];
        const auto& hs = in.hand[def_hand(i)];
        dist[i]        = (in.head_ok && hs.tracked) ? glm::length(hs.pos - anchor_pos(d.anchor, in)) : -1.0f;
        s.dist         = dist[i];
        if (!hs.grip) s.owned = false;
        active[i]         = s.on.load() && in.allowed && (!d.needs_weapon || in.weapon) && dist[i] >= 0.0f;
        // Zone entry: a light tick on that controller when the hand comes in with its grip free (a hand already
        // holding something passes through quietly). Leaving needs 15 % more distance, so the edge does not chatter.
        const bool was_in = s.inside;
        s.inside          = active[i] && dist[i] < zone_radius(i) * (was_in ? 1.15f : 1.0f);
        if (s.inside && !was_in && !hs.grip && g_zone_buzz.load()) buzz(out, def_hand(i), kEnterAmp, kEnterSec);
        s.shown_avail  = active[i];
        s.shown_inside = s.inside;
    }
    // A grip press goes to the zone it is deepest in (distance over zone size), if any.
    for (int h = 0; h < 2; ++h) {
        if (!pressed[h]) continue;
        int   best = -1;
        float best_k = 1.0f;
        for (int i = 0; i < kCount; ++i) {
            if (def_hand(i) != h || dist[i] < 0.0f) continue;
            const float r = zone_radius(i);
            if (!active[i]) {
                if (dist[i] < r) spdlog::info("[Gesture] {}: grip pressed in the zone ({:.2f} m) but {}", kDefs[i].name, dist[i], g_state[i].on.load() ? "not available now" : "switched off");
                continue;
            }
            if (dist[i] / r < best_k) { best_k = dist[i] / r; best = i; }
            else if (dist[i] < 2.0f * r) {
                // Which way it missed, in the body's frame (for moving the zone): right, up, forward of the zone centre.
                const glm::mat3 body = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f }) };
                const glm::vec3 o    = glm::transpose(body) * (in.hand[h].pos - anchor_pos(kDefs[i].anchor, in));
                spdlog::info("[Gesture] {}: grip pressed {:.2f} m from the zone centre, outside the zone ({:.2f} m): hand {:+.0f} cm right, {:+.0f} cm up, {:+.0f} cm forward of it",
                             kDefs[i].name, dist[i], r, o.x * 100.0f, o.y * 100.0f, -o.z * 100.0f);
            }
        }
        if (best < 0) continue;
        const Def& d = kDefs[best];
        State&     s = g_state[best];
        s.owned      = true;
        buzz(out, h, kFireAmp, kFireSec);
        if (d.kind == Kind::Tap) {
            s.tap_until = now + std::chrono::milliseconds(d.tap_ms);
            spdlog::info("[Gesture] {}: grip pressed {:.2f} m from the zone centre (zone {:.2f} m): pad buttons 0x{:04x} tapped", d.name, dist[best], s.radius.load(), d.buttons);
        } else {
            spdlog::info("[Gesture] {}: grip pressed {:.2f} m from the zone centre (zone {:.2f} m): held until the grip is released", d.name, dist[best], zone_radius(best));
        }
    }
    for (int i = 0; i < kCount; ++i) {
        const Def& d = kDefs[i];
        State&     s = g_state[i];
        if (s.owned && d.kind == Kind::Tap) out.grip_taken[def_hand(i)] = true;
        if (now < s.tap_until) out.buttons |= d.buttons;
        s.shown_held = s.owned;
    }
    // Sling: the weapon hangs on it unless the sling's grip press holds it. With nothing in hand on the sling, the weapon
    // hand's grip does nothing else (no aiming down sights, no "holding the gun" steady view).
    const auto& sd    = kDefs[kSling];
    const bool  slung = g_state[kSling].on.load() && in.allowed && in.weapon && !g_state[kSling].owned;
    if (slung != g_slung.load()) spdlog::info("[Gesture] sling: the weapon {}", slung ? "hangs on the sling" : "is in the hand");
    g_slung   = slung;
    out.slung = slung;
    if (slung) out.grip_taken[sd.hand] = true;

    // Throwable: held while its grip press lasts; let go with a swing, the game camera is turned to the swing's direction
    // (Stereo's aim loop, throw_aim()) and the game's throw button (pad RB) goes once it is there (or after throwwait).
    const auto& td = kDefs[kThrow];
    const auto& th = in.hand[def_hand(kThrow)];
    if (th.tracked) hist_add(now, th.pos);
    bool       hold_rb = g_th_hold.load();
    ThrowPhase ph      = ThrowPhase(g_th_phase.load());
    const bool v2      = g_v2.load();
    if (!v2) {
        g_v2_held = g_v2_arc = g_v2_rb = false;
        g_v2_armed = false;
        g_th_launch = false;
    }
    if (v2) {
        // throwv2: the press in the zone only puts the throwable in the hand (no RB). Both grips held: the trajectory, along the other
        // controller; RB held from then (throwv2rb arm). The weapon hand's grip let go while armed (or, with throwv2hand right, at all)
        // throws; with throwv2hand left the left grip let go before that puts it back.
        const int  hh    = def_hand(kThrow);
        const bool left  = hh == kOtherHand;
        const bool wgrip = in.hand[kWeaponHand].grip, ogrip = in.hand[kOtherHand].grip;
        // Taken at the press only (a left grip still held after the throw takes nothing more).
        static bool owned_was = false;
        const bool  took      = g_state[kThrow].owned && !owned_was;
        owned_was             = g_state[kThrow].owned;
        if (took && !g_v2_held.load() && ph == ThrowPhase::Idle && g_peek_step == 0) {
            // throwrock (Homard: "make the rock being selected"): the game drew no throwable at the last peek (none selected, none left):
            // this one is a rock (the game's own rock throw, D-pad down; endless).
            g_rock_now = g_rock.load() && g_peek_failed;
            if (g_rock_now) spdlog::info("[Throw] v2: no throwable to draw at the last peek: a rock (D-pad down) this time");
            g_v2_held  = true;
            g_v2_armed = g_v2_was_armed = false;
            g_v2_rb    = false;
            set_phase(ThrowPhase::Held, now);
            ph = ThrowPhase::Held;
            spdlog::info("[Throw] v2: taken in the {} hand (no RB: no raise animation)", left ? "left" : "right");
        }
        bool release = false, cancel = false;
        if (g_v2_held.load()) {
            out.grip_taken[kWeaponHand] = out.grip_taken[kOtherHand] = true;   // both grips belong to the throw meanwhile
            const bool armed = wgrip && ogrip;
            if (armed != g_v2_armed && g_th_log.load()) spdlog::info("[Throw] v2: {}", armed ? "both grips held: trajectory shown" : "trajectory hidden");
            if (left) {
                if (g_v2_was_armed && !wgrip) release = true;   // the right grip let go after both were held (the left one may be let go by then)
                else if (!ogrip && !g_v2_was_armed) cancel = true;   // the left grip let go first: back on the chest
            } else if (!wgrip) {
                // The right grip let go: thrown only if the trajectory was shown (both grips) since the grab; else back on the chest (headset
                // 25 Oct: a grab by mistake let go threw it where the head looked).
                if (g_v2_was_armed) release = true;
                else cancel = true;
            }
            g_v2_armed = armed;
            if (armed) g_v2_was_armed = true;
            // once RB is held it stays until the throw (letting it go throws). The rock (throwrock): D-pad down, tapped at the throw.
            if (armed && g_v2_rb_arm.load() && !g_rock_now) g_v2_rb = true;
            if (!in.allowed) cancel = !g_v2_rb.load(), release = g_v2_rb.load();
        }
        g_v2_arc = g_v2_held.load() && g_v2_armed && in.other_fwd_ok;
        // throwpreaim: while the trajectory shows, the game's aim already follows it, so letting go throws at once.
        if (g_v2_arc.load() && g_preaim.load()) {
            std::lock_guard _{ g_th_mtx };
            g_th_dir    = in.other_fwd;
            g_th_launch = true;
        }
        if (cancel) {
            spdlog::info("[Throw] v2: let go before the throw: back on the chest");
            g_v2_held = false;
            set_phase(ThrowPhase::Idle, now);
            ph = ThrowPhase::Idle;
        } else if (release) {
            // Along the trajectory when it was shown (the other controller's aim); else the swing's direction, or the head's.
            float           peak = 0.0f;
            int             used = 0;
            const glm::vec3 v    = release_velocity(now, peak, used);
            const float     sp   = glm::length(v);
            glm::vec3       dir{ 0.0f, 0.0f, -1.0f };
            const bool      along = in.other_fwd_ok && (left || g_v2_was_armed);
            if (along) dir = in.other_fwd;
            else if (sp >= g_th_min.load()) dir = v / sp;
            else if (in.head_ok) dir = -glm::normalize(glm::vec3{ in.head[2] });
            {
                std::lock_guard _{ g_th_mtx };
                g_th_dir   = dir;
                g_th_speed = sp;
            }
            g_th_launch = along;
            const glm::vec3 dh = glm::transpose(glm::mat3{ in.head }) * dir;
            spdlog::info("[Throw] v2: thrown {} (head frame right {:+.2f} up {:+.2f} back {:+.2f}, {:.0f} deg up), RB {}", along ? "along the trajectory" : sp >= g_th_min.load() ? "the swing's way" : "where the head looks",
                         dh.x, dh.y, dh.z, glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f))), g_v2_rb.load() ? "held since the trajectory showed" : "tapped");
            g_v2_held  = false;
            g_v2_armed = false;
            g_v2_arc   = false;
            set_phase(ThrowPhase::Aim, now);
            ph = ThrowPhase::Aim;
        }
        hold_rb = g_v2_rb.load();
        if (ph == ThrowPhase::Idle) g_v2_rb = false;

        // throwpeek: RB held a moment with nothing recorded (or after the weapon wheel closed) so the game draws the selected throwable
        // and the render side records it; the cancel buttons are tapped while RB is still held, then RB is let go.
        // Standing still only (headset 25 Oct: peeks while walking forward saw nothing in time, RB was let go without the cancel, and the
        // game threw a knife each time).
        const bool calm = in.allowed && !in.wheel && !in.moving;
        if (calm && !g_allowed_was) g_allowed_since = now;
        g_allowed_was = calm;
        // The peek's own LB opens and closes the wheel: not a new selection (the peek ran again and again in the simulator).
        if (g_wheel_was && !in.wheel && g_peek_step == 0 && now > g_peek_quiet) g_peek_again = true, g_peek_failed = false, g_peek_fails = 0;
        g_wheel_was = in.wheel;
        const int cancel_btn = g_peek_cancel.load();
        if (g_peek_step == 0 && g_peek_on.load() && cancel_btn != 0 && ph == ThrowPhase::Idle && calm && !wgrip && !ogrip &&
            now - g_allowed_since > std::chrono::seconds(2) && now > g_peek_quiet && ((!in.recorded && !g_peek_failed) || g_peek_again.load())) {
            g_peek_again = false;
            g_peek_seen  = false;
            g_peek_step  = 1;
            g_peek_t     = now;
            spdlog::info("[Throw] peek: RB held so the game draws the selected throwable ({})", in.recorded ? "the weapon wheel closed" : "nothing recorded yet");
        }
        else if (g_peek_step == 0 && g_peek_on.load() && ((!in.recorded && !g_peek_failed) || g_peek_again.load())) {
            static clock::time_point said{};
            if (now - said > std::chrono::seconds(3)) {
                said = now;
                spdlog::info("[Throw] peek waits: phase {}, on foot {}, wheel {}, grips {}/{}, {:.1f} s calm, cancel 0x{:04x}", int(ph), in.allowed, in.wheel, wgrip, ogrip,
                             calm ? std::chrono::duration<float>(now - g_allowed_since).count() : 0.0f, cancel_btn);
            }
        }
        if (g_peek_step > 0) {
            const auto ps = now - g_peek_t;
            if (g_peek_step == 1) {
                out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                // Headset 25 Oct: other throwables than the knife (0.34 s) took longer than 0.7 s; at 12:35 a peek saw nothing in 1.3 s and the knife
                // was thrown anyway: waited up to 2.5 s now, and a peek that saw nothing is cancelled twice.
                if (g_peek_seen.load() || ps > std::chrono::milliseconds(2500)) {
                    // Simulator 25 Oct: RB pressed while the game puts the weapon back after a throw draws nothing either: one more try
                    // 2.5 s later before calling it "none" (the rock then).
                    if (g_peek_seen.load()) {
                        g_peek_fails  = 0;
                        g_peek_failed = false;
                    } else if (++g_peek_fails < 2 && g_rock.load()) {   // tried again only for throwrock (each peek is a risk)
                        g_peek_again = true;
                        spdlog::info("[Throw] peek: the game drew no throwable in 1.3 s: trying again");
                    } else {
                        g_peek_failed = true;
                        spdlog::info("[Throw] peek: the game drew no throwable in 1.3 s twice (none selected or none left): no new peek until the weapon wheel closes");
                    }
                    if (!g_peek_seen.load()) g_peek_failed = true;
                    g_peek_twice = !g_peek_seen.load();
                    g_peek_step  = 2;   // always cancelled (X) before RB goes: a throwable drawn late would otherwise be thrown
                    g_peek_t    = now;
                }
            } else if (g_peek_step == 2) {
                out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER | uint16_t(cancel_btn);
                if (ps > std::chrono::milliseconds(g_peek_cancel_ms.load()) && g_peek_twice) {
                    g_peek_twice = false;
                    g_peek_step  = 4;   // RB alone a moment, then X with RB once more
                    g_peek_t     = now;
                } else if (ps > std::chrono::milliseconds(g_peek_cancel_ms.load())) {
                    g_peek_step = 3;
                    g_peek_t    = now;
                    spdlog::info("[Throw] peek: cancel 0x{:04x} tapped with RB held; RB let go", cancel_btn);
                }
            } else if (g_peek_step == 4) {
                out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
                if (ps > std::chrono::milliseconds(120)) {
                    g_peek_step = 2;
                    g_peek_t    = now;
                }
            } else if (ps > std::chrono::milliseconds(800)) {
                g_peek_step  = 0;
                g_peek_quiet = now + std::chrono::milliseconds(g_peek_fails > 0 ? 2500 : 1500);
            }
            out.no_fire = true;
        }
        g_peeking = g_peek_step == 1 || g_peek_step == 2 || g_peek_step == 4;
    } else if (g_state[kThrow].owned) {
        out.grip_taken[td.hand] = true;
        if (ph != ThrowPhase::Held) {
            set_phase(ThrowPhase::Held, now);
            ph = ThrowPhase::Held;
        }
        if (!in.allowed) g_state[kThrow].owned = false;   // a vehicle, a menu or the flat screen: dropped back on the chest
    } else if (ph == ThrowPhase::Held) {
        float     peak = 0.0f;
        int       used = 0;
        const glm::vec3 v = release_velocity(now, peak, used);
        const float     sp = glm::length(v);
        const glm::vec3 vh = glm::transpose(glm::mat3{ in.head }) * v;   // head frame, for the log
        if (in.allowed && sp >= g_th_min.load()) {
            {
                std::lock_guard _{ g_th_mtx };
                g_th_dir   = v / sp;
                g_th_speed = sp;
            }
            spdlog::info("[Throw] let go at {:.2f} m/s (peak {:.2f}, {} samples fitted): head frame right {:+.2f} up {:+.2f} back {:+.2f}, {:.0f} deg up", sp, peak, used,
                         vh.x, vh.y, vh.z, glm::degrees(std::asin(std::clamp(v.y / sp, -1.0f, 1.0f))));
            set_phase(ThrowPhase::Aim, now);
            ph = ThrowPhase::Aim;
        } else if (hold_rb && in.head_ok) {
            // The game holds its throw ready while RB is held and throws when RB is let go; no pad input
            // is known to cancel it (a simulator test on 16 Oct could not tell), so a release without a swing throws where
            // the head looks.
            const glm::vec3 f = -glm::normalize(glm::vec3{ in.head[2] });
            {
                std::lock_guard _{ g_th_mtx };
                g_th_dir   = f;
                g_th_speed = sp;
            }
            spdlog::info("[Throw] let go at {:.2f} m/s (below throwmin {:.2f}{}): thrown where the head looks", sp, g_th_min.load(), in.allowed ? "" : ", not on foot");
            set_phase(ThrowPhase::Aim, now);
            ph = ThrowPhase::Aim;
        } else {
            spdlog::info("[Throw] let go at {:.2f} m/s (below throwmin {:.2f}{}): back on the chest", sp, g_th_min.load(), in.allowed ? "" : ", or not on foot");
            set_phase(ThrowPhase::Idle, now);
            ph = ThrowPhase::Idle;
        }
    }
    const auto since = now - g_th_since;   // in the phase entered (this poll's release starts Aim at 0)
    const bool rock    = v2 && g_rock_now;
    const uint16_t tap_btn = rock ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_RIGHT_SHOULDER;
    switch (ph) {
    case ThrowPhase::Held:
        if (hold_rb) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        break;
    case ThrowPhase::Aim:
        if (hold_rb) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (in.throw_aimed || since > std::chrono::milliseconds(v2 ? std::max(g_th_wait.load(), 700) : g_th_wait.load())) {   // throwv2: the trajectory can be far off the look
            if (g_th_log.load()) spdlog::info("[Throw] RB {} after {} ms: {}", hold_rb ? "released" : "tapped", std::chrono::duration_cast<std::chrono::milliseconds>(since).count(),
                                              in.throw_aimed ? "the camera is on the throw's direction" : "the camera did not get there in time (throwwait)");
            set_phase(ThrowPhase::Throw, now);
            if (!hold_rb) out.buttons |= tap_btn;
        }
        break;
    case ThrowPhase::Throw:
        // A rock's D-pad down kept short: held ~350 ms (a stalled frame in the simulator) the game threw no rock; 120 ms did.
        if (!hold_rb && since < std::chrono::milliseconds(rock ? std::min(g_th_tap.load(), 100) : g_th_tap.load())) out.buttons |= tap_btn;
        else set_phase(ThrowPhase::Follow, now);
        break;
    case ThrowPhase::Follow:
        if (since > std::chrono::milliseconds(rock ? std::max(g_th_keep.load(), 1000) : g_th_keep.load())) {   // a rock leaves ~0.63 s after its button
            set_phase(ThrowPhase::Idle, now);
            if (v2 && g_rock.load()) {   // only for throwrock: is there another one (else the next is a rock)
                g_peek_again = true;
                g_peek_quiet = now + std::chrono::milliseconds(2500);   // RB pressed right after a throw draws nothing (simulator 25 Oct: 1.5 s was short)
            }
        }
        break;
    default: break;
    }
    out.no_fire = out.no_fire || ThrowPhase(g_th_phase.load()) != ThrowPhase::Idle;
    if (now < g_padtap_until) out.buttons |= uint16_t(g_padtap_buttons.load());   // padtap (test)
    return out;
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    init();
    const bool on = arg == "on" || arg == "1" || arg == "true";
    persist       = false;
    if (cmd == "gesturegrip") { g_test_grip = on; return true; }
    if (cmd == "gesturebuzz") { g_zone_buzz = on; persist = true; return true; }
    if (cmd == "gesturezones") { g_zone_show = on; return true; }
    if (cmd == "gestureshoulder") {
        float v[3]{ fv, g_sh_down.load(), g_sh_back.load() };
        rest >> v[1] >> v[2];
        set_shoulder_place(v);
        spdlog::info("[Gesture] shoulder from the eyes looking ahead (hangs from the neck, turns with the body): side {:.2f} down {:.2f} back {:.2f} m", g_sh_side.load(), g_sh_down.load(), g_sh_back.load());
        persist = true;
        return true;
    }
    if (cmd == "slingzone") {
        float v[3]{ fv, 0.0f, 0.0f };
        rest >> v[1] >> v[2];
        set_sling_zone(v);
        persist = true;
        return true;
    }
    if (cmd == "throwpos") {
        float v[3]{ fv, g_th_pos[1].load(), g_th_pos[2].load() };
        rest >> v[1] >> v[2];
        set_throw_place(v);
        persist = true;
        return true;
    }
    if (cmd == "throwmin") { g_th_min = std::clamp(fv, 0.1f, 5.0f); persist = true; return true; }
    if (cmd == "throwgate") { g_th_gate = std::clamp(fv, 0.2f, 20.0f); persist = true; return true; }
    if (cmd == "throwwait") { g_th_wait = std::clamp(int(fv), 0, 2000); persist = true; return true; }
    if (cmd == "throwkeep") { g_th_keep = std::clamp(int(fv), 0, 3000); persist = true; return true; }
    if (cmd == "throwtap") { g_th_tap = std::clamp(int(fv), 20, 1000); persist = true; return true; }
    if (cmd == "throwhold") { g_th_hold = on; persist = true; return true; }
    if (cmd == "throwlog") { g_th_log = on; return true; }
    if (cmd == "throwv2") { g_v2 = on; persist = true; spdlog::info("[Throw] throwv2 {}", on ? "on" : "off"); return true; }
    if (cmd == "throwv2hand") { g_v2_right = arg == "right"; persist = true; return true; }
    if (cmd == "throwv2rb") { g_v2_rb_arm = arg != "tap"; persist = true; return true; }
    if (cmd == "throwpeek") { g_peek_on = on; persist = true; return true; }
    if (cmd == "throwpreaim") { g_preaim = on; persist = true; return true; }
    if (cmd == "throwv2zone") { g_v2_zone = std::clamp(fv, 0.05f, 0.5f); persist = true; return true; }
    if (cmd == "throwrock") { g_rock = on; persist = true; return true; }
    if (cmd == "throwpeekcancel") { g_peek_cancel = int(std::strtol(arg.c_str(), nullptr, 16)); persist = true; return true; }
    if (cmd == "throwpeekms") { g_peek_cancel_ms = std::clamp(int(fv), 20, 1000); persist = true; return true; }
    if (cmd == "throwpeeknow") { g_peek_again = true; return true; }
    if (cmd == "throwtest2") {
        int arm = 1500;
        rest >> arm;
        g_t2_hold = std::clamp(arg.empty() ? 800 : int(fv), 0, 20000);
        g_t2_arm  = std::clamp(arm, 50, 20000);
        g_t2_req  = true;
        return true;
    }
    if (cmd == "padtap") {
        int ms = 120;
        rest >> ms;
        g_padtap_buttons = int(std::strtol(arg.c_str(), nullptr, 16));
        g_padtap_until   = clock::now() + std::chrono::milliseconds(std::clamp(ms, 10, 5000));
        spdlog::info("[Gesture] padtap: pad buttons 0x{:04x} for {} ms", g_padtap_buttons.load(), ms);
        return true;
    }
    if (cmd == "throwtest") {
        float y = 0.0f, z = 0.0f;
        int   ms = 150, hold = 600;
        rest >> y >> z >> ms >> hold;
        g_tt_v[0] = fv;
        g_tt_v[1] = y;
        g_tt_v[2] = z;
        g_tt_ms   = std::clamp(ms, 20, 2000);
        g_tt_hold = std::clamp(hold, 0, 20000);
        g_tt_req  = true;
        return true;
    }
    if (cmd == "slingpos" || cmd == "slingrot") {
        float v[3]{ fv, 0.0f, 0.0f };
        rest >> v[1] >> v[2];
        float in[3]{}, out[3]{};
        get_sling_place(in, out);
        if (cmd == "slingpos") set_sling_place(v, out);
        else set_sling_place(in, v);
        persist = true;
        return true;
    }
    for (int i = 0; i < kCount; ++i) {
        const std::string n = kDefs[i].name;
        if (cmd == n) { g_state[i].on = on; persist = true; return true; }
        if (cmd == n + "size") { g_state[i].radius = std::clamp(fv, 0.05f, 0.5f); persist = true; return true; }
    }
    return false;
}

bool test_grip() { return g_test_grip.load(); }
bool test_input() { return g_test_grip.load() || g_tt_req.load() || g_tt_step > 0 || g_t2_req.load() || g_t2_on || clock::now() < g_padtap_until; }
bool zone_buzz() { return g_zone_buzz.load(); }
void set_zone_buzz(bool on) { g_zone_buzz = on; }
bool zone_show() { return g_zone_show.load(); }
void set_zone_show(bool on) { g_zone_show = on; }

int zone_views(const glm::mat4& head, bool mirror, ZoneView* out, int max)
{
    init();
    Input in{};
    in.head    = head;
    in.head_ok = true;
    in.mirror  = mirror;
    int n = 0;
    for (int i = 0; i < kCount && n < max; ++i) {
        const State& s = g_state[i];
        ZoneView&    z = out[n++];
        z.centre       = anchor_pos(kDefs[i].anchor, in);
        z.radius       = zone_radius(i);
        z.hand         = def_hand(i);
        z.on           = s.on.load();
        z.available    = s.shown_avail.load();
        z.inside       = s.shown_inside.load();
        z.held         = s.shown_held.load();
    }
    return n;
}

bool hand_point(int hand, glm::vec3& pos)
{
    if (hand < 0 || hand > 1 || !g_hand_pt_ok[hand].load()) return false;
    pos = glm::vec3{ g_hand_pt[hand][0].load(), g_hand_pt[hand][1].load(), g_hand_pt[hand][2].load() };
    return true;
}
bool slung() { return g_slung.load(); }
bool sling_on() { init(); return g_state[kSling].on.load(); }

void get_sling_place(float pos[3], float rot[3])
{
    for (int i = 0; i < 3; ++i) {
        pos[i] = g_sl_pos[i].load();
        rot[i] = g_sl_rot[i].load();
    }
}

void set_sling_place(const float pos[3], const float rot[3])
{
    g_sl_pos[0] = std::clamp(pos[0], -0.4f, 0.4f);
    g_sl_pos[1] = std::clamp(pos[1], 0.0f, 0.9f);
    g_sl_pos[2] = std::clamp(pos[2], -0.4f, 0.6f);
    for (int i = 0; i < 3; ++i) g_sl_rot[i] = std::clamp(rot[i], -180.0f, 180.0f);
}

void get_sling_zone(float off[3]) { for (int i = 0; i < 3; ++i) off[i] = g_sl_zone[i].load(); }
void set_sling_zone(const float off[3]) { for (int i = 0; i < 3; ++i) g_sl_zone[i] = std::clamp(off[i], -0.3f, 0.3f); }

void reset_sling_place() { set_sling_place(kSlPosDefault, kSlRotDefault); }

void reset_sling_zone()
{
    const float zero[3]{};
    set_sling_zone(zero);
}

void reset_sling()
{
    reset_sling_place();
    reset_sling_zone();
}

void get_shoulder_place(float pos[3])
{
    pos[0] = g_sh_side.load();
    pos[1] = g_sh_down.load();
    pos[2] = g_sh_back.load();
}

void set_shoulder_place(const float pos[3])
{
    g_sh_side = std::clamp(pos[0], 0.0f, 0.5f);
    g_sh_down = std::clamp(pos[1], -0.2f, 0.6f);
    g_sh_back = std::clamp(pos[2], -0.3f, 0.4f);
}

void reset_shoulder() { set_shoulder_place(kShDefault); }

bool       throw_on() { init(); return g_state[kThrow].on.load(); }
ThrowPhase throw_phase() { return ThrowPhase(g_th_phase.load()); }
float      throw_gate_deg() { return g_th_gate.load(); }

bool throw_aim(glm::vec3& dir, float& speed)
{
    const auto p = ThrowPhase(g_th_phase.load());
    const bool pre = p == ThrowPhase::Held && g_v2.load() && g_v2_arc.load() && g_preaim.load();
    if (p != ThrowPhase::Aim && p != ThrowPhase::Throw && p != ThrowPhase::Follow && !pre) return false;
    std::lock_guard _{ g_th_mtx };
    dir   = g_th_dir;
    speed = g_th_speed;
    return true;
}

bool throw_v2() { return g_v2.load(); }
bool throw_v2_right() { return g_v2_right.load(); }
bool throw_v2_rb_arm() { return g_v2_rb_arm.load(); }
bool throw_peek_on() { return g_peek_on.load(); }
int  throw_hold_hand() { return def_hand(kThrow); }
bool throw_arc_shown() { return g_v2.load() && g_v2_arc.load(); }
bool throw_launch_aimed() { return g_v2.load() && g_th_launch.load() && throw_phase() != ThrowPhase::Idle; }
bool throw_preaim() { return g_preaim.load(); }
bool throw_rock() { return g_rock.load(); }
bool throw_is_rock() { return g_v2.load() && g_rock_now && throw_phase() != ThrowPhase::Idle; }
bool throw_rb_held() { return g_v2.load() && g_v2_rb.load(); }
bool throw_peeking() { return g_peeking.load(); }
void throw_peek_seen() { g_peek_seen = true; }
void throw_peek_again() { g_peek_again = true; }

void get_throw_place(float pos[3]) { for (int i = 0; i < 3; ++i) pos[i] = g_th_pos[i].load(); }

void set_throw_place(const float pos[3])
{
    g_th_pos[0] = std::clamp(pos[0], -0.4f, 0.4f);
    g_th_pos[1] = std::clamp(pos[1], 0.0f, 0.9f);
    g_th_pos[2] = std::clamp(pos[2], -0.3f, 0.5f);
}

void reset_throw() { set_throw_place(kThPosDefault); }

int         count() { return kCount; }
const char* label(int i) { return kDefs[i].label; }
const char* hint(int i) { return kDefs[i].hint; }
const char* name(int i) { return kDefs[i].name; }
bool        enabled(int i) { init(); return g_state[i].on.load(); }
void        set_enabled(int i, bool on) { init(); g_state[i].on = on; }
float       radius(int i) { init(); return g_state[i].radius.load(); }
void        set_radius(int i, float m) { init(); g_state[i].radius = std::clamp(m, 0.05f, 0.5f); }
float       default_radius(int i) { return kDefs[i].radius; }
float       last_distance(int i) { return g_state[i].dist.load(); }

}
