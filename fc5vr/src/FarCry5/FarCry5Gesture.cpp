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
          "That grip press is not the left grip's usual modifier or two-handed hold.",
          Kind::Hold, kOtherHand, Anchor::Chest, 0, 0, 0.22f, true, false },
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
        const auto& hs = in.hand[d.hand];
        dist[i]        = (in.head_ok && hs.tracked) ? glm::length(hs.pos - anchor_pos(d.anchor, in)) : -1.0f;
        s.dist         = dist[i];
        if (!hs.grip) s.owned = false;
        active[i]         = s.on.load() && in.allowed && (!d.needs_weapon || in.weapon) && dist[i] >= 0.0f;
        // Zone entry: a light tick on that controller when the hand comes in with its grip free (a hand already
        // holding something passes through quietly). Leaving needs 15 % more distance, so the edge does not chatter.
        const bool was_in = s.inside;
        s.inside          = active[i] && dist[i] < s.radius.load() * (was_in ? 1.15f : 1.0f);
        if (s.inside && !was_in && !hs.grip && g_zone_buzz.load()) buzz(out, d.hand, kEnterAmp, kEnterSec);
        s.shown_avail  = active[i];
        s.shown_inside = s.inside;
    }
    // A grip press goes to the zone it is deepest in (distance over zone size), if any.
    for (int h = 0; h < 2; ++h) {
        if (!pressed[h]) continue;
        int   best = -1;
        float best_k = 1.0f;
        for (int i = 0; i < kCount; ++i) {
            if (kDefs[i].hand != h || dist[i] < 0.0f) continue;
            const float r = g_state[i].radius.load();
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
            spdlog::info("[Gesture] {}: grip pressed {:.2f} m from the zone centre (zone {:.2f} m): held until the grip is released", d.name, dist[best], s.radius.load());
        }
    }
    for (int i = 0; i < kCount; ++i) {
        const Def& d = kDefs[i];
        State&     s = g_state[i];
        if (s.owned && d.kind == Kind::Tap) out.grip_taken[d.hand] = true;
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
    const auto& th = in.hand[td.hand];
    if (th.tracked) hist_add(now, th.pos);
    const bool hold_rb = g_th_hold.load();
    ThrowPhase ph      = ThrowPhase(g_th_phase.load());
    if (g_state[kThrow].owned) {
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
    switch (ph) {
    case ThrowPhase::Held:
        if (hold_rb) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        break;
    case ThrowPhase::Aim:
        if (hold_rb) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        if (in.throw_aimed || since > std::chrono::milliseconds(g_th_wait.load())) {
            if (g_th_log.load()) spdlog::info("[Throw] RB {} after {} ms: {}", hold_rb ? "released" : "tapped", std::chrono::duration_cast<std::chrono::milliseconds>(since).count(),
                                              in.throw_aimed ? "the camera is on the throw's direction" : "the camera did not get there in time (throwwait)");
            set_phase(ThrowPhase::Throw, now);
            if (!hold_rb) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
        break;
    case ThrowPhase::Throw:
        if (!hold_rb && since < std::chrono::milliseconds(g_th_tap.load())) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        else set_phase(ThrowPhase::Follow, now);
        break;
    case ThrowPhase::Follow:
        if (since > std::chrono::milliseconds(g_th_keep.load())) set_phase(ThrowPhase::Idle, now);
        break;
    default: break;
    }
    out.no_fire = ThrowPhase(g_th_phase.load()) != ThrowPhase::Idle;
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
bool test_input() { return g_test_grip.load() || g_tt_req.load() || g_tt_step > 0 || clock::now() < g_padtap_until; }
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
        z.radius       = s.radius.load();
        z.hand         = kDefs[i].hand;
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
    if (p != ThrowPhase::Aim && p != ThrowPhase::Throw && p != ThrowPhase::Follow) return false;
    std::lock_guard _{ g_th_mtx };
    dir   = g_th_dir;
    speed = g_th_speed;
    return true;
}

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
