#include "FarCry5Gesture.h"

#include <windows.h>
#include <Xinput.h>
#include <glm/gtc/matrix_transform.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <istream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fc5::gesture
{
namespace
{
    using clock = std::chrono::steady_clock;

    // Where a zone sits on the body. The shoulders hang from the neck and turn with the body's heading (see shoulder()), the sling
    // from the head and the body's heading (see sling_pose()).
    enum class Anchor { WeaponShoulder, OtherShoulder, Sling, Chest, Hip };

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
        { "shoulderswitch", "Right shoulder: weapon slot (right hand + grip)",
          "Reach to your right shoulder (as if taking a gun from your back) and press the grip: the weapon of the slot chosen below (default slot 2: the game's weapon slot key 2) is in your hand (pad Y, the last weapon, with shoulderwheel off). "
          "That grip press does not aim or hold the gun. Left-handed: the left hand at the left shoulder.",
          Kind::Tap, kWeaponHand, Anchor::WeaponShoulder, XINPUT_GAMEPAD_Y, 120, 0.24f, true, false },   // 0.20 until 27 Oct (4 of his presses missed by 0.21-0.23 m)
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
        { "sidearm", "Sidearm on the right hip: grab it with the right grip (experimental, needs the sling)",
          "Your sidearm (a handgun) hangs on your right hip and your main weapon on the sling, both always shown. Hold the right grip on the hip: "
          "the sidearm is in your hand (the game switches to it if needed, the sling keeps showing your main weapon); let go: back on the hip. "
          "Grab the sling: the main weapon is in your hand and the sidearm stays on the hip. The hip takes the weapon wheel's rightmost slot (the sidearm), "
          "the sling the game's last weapon (pad Y): the main weapon you held before. Off (default): the sling as before.",
          Kind::Hold, kWeaponHand, Anchor::Hip, 0, 0,  0.16f, false, true },
        { "shoulderleft", "Left shoulder: weapon slot (right hand + grip)",
          "Reach across to your left shoulder with the right hand and press the grip: the weapon of the slot chosen below (default slot 1: the game's weapon slot key 1) is in your hand. "
          "That grip press does not aim or hold the gun. Left-handed: the left hand at the right shoulder.",
          Kind::Tap, kWeaponHand, Anchor::OtherShoulder, XINPUT_GAMEPAD_Y, 120, 0.24f, true, false },   // Homard 30 Oct: "make left shoulder weapon slot 1 and right shoulder weapon slot 2"
    };
    constexpr int kCount = int(sizeof(kDefs) / sizeof(kDefs[0]));
    constexpr int kShoulder = 0;
    constexpr int kSling = 1;
    constexpr int kThrow = 2;
    constexpr int kSide  = 3;
    constexpr int kShoulderL = 4;

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
    // gesturemisswalk (27 Oct, Homard's 21:44 log: a left grip pressed 0.42 m from the throwable zone became the D-pad modifier, walking sent
    // D-pad up and opened the binoculars): a press just outside a zone (within twice its radius) leaves the left stick walking, not
    // the D-pad, until that grip is let go. The right stick's D-pad stays.
    std::atomic<bool>  g_miss_walk{ true };
    bool               g_miss_press[2]{};
    std::atomic<bool>  g_zone_show{ false };  // gesturezones (debug): the zones drawn in both eyes
    std::atomic<float> g_hand_pt[2][3]{};     // each hand's tested point at the last poll (tracking space), for gesturezones
    std::atomic<bool>  g_hand_pt_ok[2]{};

    // Sling: where the weapon's grip hangs from the head (body frame: right, down, forward, metres) and how the
    // weapon is turned there (degrees: barrel to the left, barrel down, roll). Defaults: grip by the right hip,
    // the barrel across the belly towards the left, pointing down (a two-point sling's carry).
    constexpr float    kSlPosDefault[3]{ 0.09f, 0.50f, 0.05f }, kSlRotDefault[3]{ 70.0f, -30.0f, 0.0f };
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

    // Sidearm: where its grip hangs from the head (body frame: right, down, forward, metres) and how it is turned (as the sling's:
    // degrees barrel to the left, barrel down, roll). Default: on the right hip, 26 cm right, 63 cm below and 12 cm behind the eyes (Homard 27 Oct: "closer to
    // the body, and the sidearm really on the right, not as much in front"; his hip grabs went 8 cm higher and 12 cm further back than the old
    // 0.20 0.72 -0.02), the barrel down in the holster (80 deg) with the grip to the back.
    constexpr float    kHpPosDefault[3]{ 0.26f, 0.63f, -0.12f }, kHpRotDefault[3]{ 0.0f, -80.0f, 0.0f };
    std::atomic<float> g_hp_pos[3]{ kHpPosDefault[0], kHpPosDefault[1], kHpPosDefault[2] };
    std::atomic<float> g_hp_rot[3]{ kHpRotDefault[0], kHpRotDefault[1], kHpRotDefault[2] };
    // The sidearm zone's centre from its grip (body frame: right, down, forward, m), as slingzone. Default 3 cm left, 9 cm higher, 12 cm further
    // back: where Homard's hand went for it on 27 Oct 21:09 (hip presses missed by -3 right, +9 up, -12 forward on average; zone 0.13 m).
    constexpr float    kHpZoneDefault[3]{ -0.03f, -0.09f, -0.12f };
    std::atomic<float> g_hp_zone[3]{ kHpZoneDefault[0], kHpZoneDefault[1], kHpZoneDefault[2] };
    std::atomic<bool>  g_cur_side{ false };   // the game's weapon at the last poll is a sidearm
    std::atomic<int>   g_sw_want{ -1 };       // a pending switch: 0 to the main weapon, 1 to the sidearm
    uint32_t           g_sw_from = 0, g_sw_new = 0;   // the weapon id when it began; a new id seen since (and when)
    clock::time_point  g_sw_t{}, g_sw_new_t{}, g_sw_tap_until{};
    // sidearmkey: 0 pad Y (last weapon), else a keyboard scan code. Default key 1 (0x02): the weapon wheel's sidearm slot (Homard 26 Oct: "sidearms
    // are a specific slot in the weapon wheel"; simulator, car-1021: key 1 selects the revolver, 2 the rifle, 3 a second handgun kept in a weapon slot).
    // Homard 27 Oct: "the sidearm (rightmost slot on the weapon wheel, slot 4)"; in his save keyboard key 1 gave a scoped rifle and key 4 another
    // rifle, so the number keys are not the wheel's slots: -1 (default, sidearmkey wheel) picks the slot through the wheel itself.
    // The wheel's rightmost slot by default (-1): a keyboard key (sidearmpick 4: his 12:22 log, key 4 gave his pistol) flips the game to keyboard
    // prompts (log review 27 Oct 22:00), so the wheel is used and its picture hidden while the mod picks (wheel_pick_now).
    std::atomic<int>   g_side_key{ -1 };
    clock::time_point  g_side_allowed_since{};
    // sidearmwheelms: how long LB and the right stick are held for the wheel pick (150 since 28 Oct, as shoulderwheelms: Homard 20:39 "picking up
    // the holstered weapon after using the sling weapon blocked it for a long time ... Can you make it instantaneous"; his hip switches took
    // 1.1-1.6 s with 500, the shoulder's picks 0.60-0.69 s with 150 instead of 0.95-1.02 with 500).
    // Back to 500 on 29 Oct 13:08: the shoulder thread found 150 ms picks often read as an LB tap or ignored in the headset (Homard's 13:44 log; the
    // simulator's steady 72 fps did not show it). The usual sling-then-hip draw goes with pad Y (sidearmy) and does not wait on it.
    std::atomic<int>   g_wheel_ms{ 500 };
    clock::time_point  g_wheel_until{};
    std::atomic<bool>  g_wheel_hide{ false };
    int                g_wheel_picks = 0;      // wheel picks made for the pending switch (a second one if the first gave nothing: Homard's 13:46:08 pick did nothing)
    // sidearmy on|off (saved, default on): a hip grab switches with pad Y (the game's last weapon, as the sling) when that is the sidearm:
    // the weapon held before the one in hand (each held 0.6 s; not a throwable) was it. A Y that brings another weapon is followed by the wheel pick.
    std::atomic<bool>  g_side_y{ true };
    bool               g_sw_by_y = false;      // the pending hip switch went with pad Y
    std::atomic<bool>  g_sw_grab{ false };     // the pending switch is a grab's (not the look after a load: Homard 28 Oct 22:10, its switch back drew his pistol's copy in his empty hand)
    uint32_t           g_held_cur = 0, g_held_prev = 0, g_held_cand = 0;   // the game's weapons held, newest two (stable ids)
    clock::time_point  g_held_cand_t{};

    // shoulderwheel (see the header): the shoulder switch picks the next of its weapon wheel spots instead of pad Y (Homard 28 Oct 14:11:
    // "shoulder switch should never select the sidearm, it should switch between slot 1, 2 and 3"). Spots: degrees from the top,
    // clockwise; his wheel (simulator 28 Oct): left the bat (no slot number), upper-left the AR-C (-32, slot 1), top (slot 2) and upper-right (slot 3) locked on his main save, right the pistol (slot 4, the sidearm).
    std::atomic<bool>  g_sh_wheel{ true };
    std::atomic<int>   g_sh_wheel_ms{ 500 };   // shoulderwheelms: LB and the right stick held for the pick (150 ms picked well in the simulator but not in the headset, 29 Oct 13:44: read as LB taps, the game's quick switch to the last weapon)
    std::mutex         g_sh_mtx;                                         // g_sh_spots (commands)
    std::vector<float> g_sh_spots{ -90.0f, -32.0f, 0.0f, 30.0f };   // his slots 1-4 from the left of the wheel (Homard 30 Oct 11:42: "the ARC (1st left) and the mp5 (2nd left center)"); the rightmost, the sidearm, never
    struct ShPick
    {
        bool              on{ false };
        int               idx{ -1 }, first{ -1 }, tries{ 0 };
        bool              side_again{ false };   // the sidearm came once and the spot was picked again
        bool              via_y{ false };        // pad Y (the game's last weapon) instead of the wheel
        bool              key{ false };          // shoulderkeys: the slot's keyboard key instead of the wheel
        uint32_t          from{ 0 }, got{ 0 };
        clock::time_point t{}, got_t{}, y_until{};
    };
    ShPick                              g_shp;
    std::unordered_map<uint32_t, int>   g_sh_slot_of;     // weapon id -> spot (learned from picks)
    std::unordered_map<uint32_t, int>   g_sh_weak;        // weapon id -> slot: a pick there gave nothing while it was held (probably its own)
    std::vector<clock::time_point>      g_sh_empty_t;     // per slot: when a pick there gave nothing while another slot's weapon was held
    std::map<int, int>                  g_sh_miss;        // per slot: picks in a row there that gave nothing (2: taken as locked or empty)
    bool                                g_wheel_by_sh = false;   // the wheel pick under way is the shoulder's (its direction), not the sidearm's (right)
    float                               g_sh_x = 0.0f, g_sh_y = 1.0f;
    std::atomic<uint32_t>               g_sh_target{ 0 };  // the weapon known at the spot being picked (shoulderinstant's guess)
    std::atomic<uint32_t>               g_sh_drops{ 0 };   // shoulder picks that gave no weapon (shoulder_pick_drops)
    std::atomic<int>                    g_sh_test_pick{ 0 };   // shoulderpick N (test)
    std::atomic<int>                    g_sh_slot_for[2]{ 1, 2 };   // shoulderleftslot, shoulderrightslot: the wheel slot (1-3) each shoulder takes
    std::atomic<bool>                   g_sh_keys{ true };   // shoulderkeys on|off (saved)
    std::atomic<bool>                   g_ls_on{ true };       // loadslot on|off (saved)
    bool                                g_ls_pending = true;   // a load (or the start) not yet followed by loadslot's check
    clock::time_point                   g_ls_on_t{}, g_ls_since{};   // last poll on foot with motion controls; playable since
    bool                                g_ls_fired = false;    // loadslot's pick under way
    std::atomic<int>                    g_ls_pub{ 0 };         // load_slot_state()
    std::atomic<bool>  g_side_peek{ true };   // sidearmpeek on|off (saved): look at the sidearm slot once after a load
    int                g_side_peek_step = 0;   // 0 idle, 1 switched to the sidearm (waiting for its recording), 2 switched back
    bool               g_side_peeked = false, g_side_calm_was = false;
    std::atomic<int>   g_side_peek_pub{ 0 };   // side_peek_state(): 2 looking, 1 looked since the load, 0 not yet
    uint32_t           g_side_peek_from = 0;
    clock::time_point  g_side_peek_t{}, g_side_calm_since{}, g_side_allowed_t{};
    std::atomic<uint32_t> g_side_landed{ 0 };   // the weapon a hip grab's switch landed on (take_side_landed: learned as the sidearm)

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
    std::atomic<uint32_t> g_switch_count{ 0 };   // weapon_switch_count(): weapon-switch gestures fired, plus note_weapon_switch() calls
    std::atomic<bool>     g_switch_grab{ false };   // set_switch_grab(): a switch gesture's grip press holds the weapon (sling on)
    std::atomic<bool>  g_tt_req{ false };
    std::atomic<float> g_tt_v[3]{};
    std::atomic<int>   g_tt_ms{ 150 }, g_tt_hold{ 600 };
    int                g_tt_step = 0;   // 0 off, 1 grab, 2 swing, 3 released
    clock::time_point  g_tt_t0{};
    glm::vec3          g_tt_p0{ 0.0f }, g_tt_vw{ 0.0f };

    // throwv2 (see the header): the hold, the trajectory, RB held for the wind-up, the peek.
    // throwv2 with pouch mode on by default (Homard 28 Oct 15:04, after the headset check: "yes" to making it the default).
    std::atomic<bool>  g_v2{ true }, g_v2_right{ false }, g_v2_rb_arm{ true }, g_preaim{ true }, g_rock{ false };   // throwrock off by default: a throwable the game draws only when thrown looks like "none left" to the peek
    std::atomic<bool>  g_rock_now{ false };   // this throwable is a rock (throwrock)
    std::atomic<bool>  g_v2_held{ false }, g_v2_arc{ false }, g_v2_rb{ false }, g_th_launch{ false };
    bool               g_v2_armed = false, g_v2_was_armed = false;
    // Simulator 25 Oct: with RB held the game shows "Cancel attack (X)"; X, LB and Y each kept the knife (no object flew), A, B, the sticks did not.
    std::atomic<bool>  g_peek_on{ false }, g_peek_again{ false }, g_peek_seen{ false }, g_peeking{ false };
    std::atomic<int>   g_peek_cancel{ 0x4000 }, g_peek_cancel_ms{ 150 };
    // throwv2rb grab (the pouch, Homard 27 Oct "let's go back to the pouch idea"): RB held from the grab, so the game draws the selected
    // throwable and the mod copies it into the hand. Put back with RB held: the game's cancel (X) with RB, RB alone, X again, then RB let go.
    std::atomic<bool>  g_v2_rb_grab{ true };
    int                g_v2_cxl_step = 0;   // 1: RB + cancel, 2: RB alone, 3: RB + cancel, 4: nothing (a moment before anything else)
    clock::time_point  g_v2_cxl_t{};
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
        case Anchor::Hip: {
            const float     m    = in.mirror ? -1.0f : 1.0f;
            const glm::mat3 body = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f }) };
            return glm::vec3{ hip_pose(in.head, in.mirror)[3] } + body * glm::vec3{ m * g_hp_zone[0].load(), -g_hp_zone[1].load(), -g_hp_zone[2].load() };
        }
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

    // shoulderwheel: the spots (a copy, the commands may change them), the next one after spot i that a pick may give (not one that gave
    // nothing in the last 20 minutes or since the player last opened the wheel: locked or empty), 
    // and a pick of spot i (LB held with the right stick toward it, wheelms).
    std::vector<float> sh_spots()
    {
        std::lock_guard _{ g_sh_mtx };
        return g_sh_spots;
    }
    void sh_pick(int i, float deg, clock::time_point now)
    {
        g_sh_x        = std::sin(glm::radians(deg));
        g_sh_y        = std::cos(glm::radians(deg));
        g_wheel_by_sh = true;
        g_wheel_until = now + std::chrono::milliseconds(std::clamp(g_sh_wheel_ms.load(), 80, 1500));
        g_peek_quiet  = std::max(g_peek_quiet, g_wheel_until + std::chrono::milliseconds(1500));   // not a new throwable selection (throwpeek)
        g_shp.idx     = i;
        g_shp.t       = now;
        g_shp.got     = 0;
        uint32_t known = 0;
        for (const auto& [w, s] : g_sh_slot_of) if (s == i) known = w;
        g_sh_target = known;
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

glm::mat4 hip_pose(const glm::mat4& head, bool mirror)
{
    const float     m    = mirror ? -1.0f : 1.0f;
    const glm::mat4 body = glm::rotate(glm::mat4{ 1.0f }, g_body_yaw.load(), glm::vec3{ 0.0f, 1.0f, 0.0f });
    glm::mat4       p    = glm::translate(glm::mat4{ 1.0f }, glm::vec3{ head[3] }) * body *
                    glm::translate(glm::mat4{ 1.0f }, glm::vec3{ m * g_hp_pos[0].load(), -g_hp_pos[1].load(), -g_hp_pos[2].load() });
    p = p * glm::rotate(glm::mat4{ 1.0f }, glm::radians(m * g_hp_rot[0].load()), glm::vec3{ 0, 1, 0 }) *
        glm::rotate(glm::mat4{ 1.0f }, glm::radians(g_hp_rot[1].load()), glm::vec3{ 1, 0, 0 }) *
        glm::rotate(glm::mat4{ 1.0f }, glm::radians(m * g_hp_rot[2].load()), glm::vec3{ 0, 0, -1 });
    return p;
}

glm::mat4 rest_pose(const glm::mat4& head, bool mirror) { return (side_on() && g_cur_side.load()) ? hip_pose(head, mirror) : sling_pose(head, mirror); }

// The sidearm switched on mid-game (Homard 28 Oct: "not from the start because it was off, so maybe when activating sidearm, you should do some
// procedure"): the slot is looked at at once (no wait for 5 s playable), and that look is redone even if one ran before.
void side_switched_on()
{
    g_side_peeked        = false;
    g_side_peek_step     = 0;
    g_side_allowed_since = clock::now() - std::chrono::seconds(10);
    spdlog::info("[Gesture] sidearm switched on: the sidearm slot will be looked at once a weapon is out (no grab held)");
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
        const bool put_back = g_t2_arm.load() == 0;
        h.grip = put_back ? t < a : (hh == kOtherHand) ? t < b + 300.0f : t < b;
        in.hand[1 - hh].grip = put_back ? false : (hh == kOtherHand) ? (t >= a && t < b) : (t >= a && t < b + 300.0f);
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
        if (!in.hand[h].grip) g_miss_press[h] = false;
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
        active[i]         = s.on.load() && in.allowed && (!d.needs_weapon || in.weapon) && dist[i] >= 0.0f && (i != kSide || g_state[kSling].on.load());
        // Zone entry: a light tick on that controller when the hand comes in with its grip free (a hand already
        // holding something passes through quietly). Leaving needs 15 % more distance, so the edge does not chatter.
        const bool was_in = s.inside;
        s.inside          = active[i] && dist[i] < zone_radius(i) * (was_in ? 1.15f : 1.0f);
        if (s.inside && !was_in && !hs.grip && g_zone_buzz.load()) buzz(out, def_hand(i), kEnterAmp, kEnterSec);
        s.shown_avail  = active[i];
        s.shown_inside = s.inside;
    }
    // shoulderkeys (default on; Homard 30 Oct 12:43: "I want the first main slot weapon (ARC, or "1" keyboard keybinds short) and second main
    // slot weapon (MP5, or key "2" on keyboard in keybinds)"; the wheel spots gave his pistol and melee weapon): slot N is the game's weapon
    // slot key N, tapped once (scan codes 0x02-0x05 for 1-4). Off: the weapon wheel's spot (shoulderslots).
    auto press_slot = [&](int i) {
        if (g_sh_keys.load()) {
            out.key_scan  = uint16_t(0x02 + std::clamp(i, 0, 3));
            g_shp.key     = true;
            g_shp.idx     = i;
            g_shp.t       = now;
            g_shp.got     = 0;
            uint32_t known = 0;
            for (const auto& [w, s] : g_sh_slot_of) if (s == i) known = w;
            g_sh_target = known;
        } else {
            const auto spots = sh_spots();
            g_shp.key        = false;
            if (!spots.empty()) sh_pick(i, spots[std::clamp(i, 0, (int)spots.size() - 1)], now);
        }
    };
    // shoulderwheel: a shoulder's slot i taken (unless the weapon held is known to be that slot's); what says so in the log first.
    auto start_slot = [&](int i, const std::string& why) {
        const auto spots = sh_spots();
        const int  n     = g_sh_keys.load() ? 4 : (int)spots.size();
        if (n == 0) return;
        i               = std::clamp(i, 0, n - 1);
        const auto it   = g_sh_slot_of.find(in.sig);
        const auto wk   = g_sh_weak.find(in.sig);
        if (!in.cur_side && ((it != g_sh_slot_of.end() && it->second == i) || (it == g_sh_slot_of.end() && wk != g_sh_weak.end() && wk->second == i))) {
            spdlog::info("[Gesture] {}: weapon {} is slot {}'s, already in hand: no switch", why, in.sig, i + 1);
            return;
        }
        // A slot that gave nothing (locked or empty: on his main save, 30 Oct, only slot 1 holds a weapon) does nothing at once, with no
        // weapon hidden for a second (Homard 11:35: "too slow"); looked at again after 5 min or once he opens the wheel himself.
        if ((int)g_sh_empty_t.size() != n) g_sh_empty_t.assign(n, clock::time_point{});
        if (g_sh_empty_t[i] != clock::time_point{} && now - g_sh_empty_t[i] < std::chrono::minutes(5)) {
            spdlog::info("[Gesture] {}: wheel slot {} gave no weapon before (locked or empty): no switch", why, i + 1);
            return;
        }
        g_switch_count++;   // shoulderdraw (FarCry5WeaponSwitch.cpp): the switch window
        g_shp.on         = true;
        g_shp.from       = in.sig;
        g_shp.first      = i;
        g_shp.tries      = 1;
        g_shp.side_again = false;
        // The slot's weapon is the game's last weapon (the one held before this one): pad Y brings it in about 0.5 s, the wheel (held 500 ms
        // to be read) in about 0.9 s (Homard 11:35: "the gun appeared after too many seconds in the hand").
        const auto pv = g_sh_slot_of.find(g_held_prev);
        g_shp.via_y   = g_held_prev != 0 && g_held_cur == in.sig && pv != g_sh_slot_of.end() && pv->second == i;
        if (g_shp.via_y) {
            g_shp.idx     = i;
            g_shp.t       = now;
            g_shp.got     = 0;
            g_shp.y_until = now + std::chrono::milliseconds(120);
            g_sh_target   = g_held_prev;
            spdlog::info("[Gesture] {}: slot {}'s weapon {} is the game's last weapon: pad Y (the weapon held: {})", why, i + 1, g_held_prev, in.sig);
            return;
        }
        press_slot(i);
        spdlog::info("[Gesture] {}: weapon slot {} ({}; the weapon held: {}{})", why, i + 1,
                     g_shp.key ? fmt::format("keyboard key {}", i + 1) : fmt::format("the weapon wheel, {:.0f} deg", spots[std::clamp(i, 0, std::max(0, (int)spots.size() - 1))]), in.sig,
                     g_sh_target.load() ? fmt::format("; slot {} gave weapon {} before", i + 1, g_sh_target.load()) : std::string{});
    };
    // loadslot on|off (saved, default on; Homard 30 Oct 13:52: "I had no weapon visible on the sling when loading in, make sure that there is
    // one"): his 14:30 log loaded with the bare hands out (4110), so nothing hung on the sling. Once after a load (or a menu of over 5 s), when
    // it has been playable 2.5 s, no switch or sidearm look is under way, and the game holds neither the main weapon nor the sidearm: the
    // left shoulder's slot (his first main weapon) is taken, as its grip would, and hangs on the sling. Given up 30 s after the load.
    if (!in.allowed) {
        if (g_ls_on_t != clock::time_point{} && now - g_ls_on_t > std::chrono::seconds(5)) g_ls_pending = true;
        g_ls_since = {};
    } else {
        g_ls_on_t = now;
        if (g_ls_since == clock::time_point{}) g_ls_since = now;
        const bool look_done = !side_on() || !g_side_peek.load() || (g_side_peeked && g_side_peek_step == 0);
        if (g_ls_pending && now - g_ls_since > std::chrono::seconds(30)) g_ls_pending = false;
        if (g_ls_pending && g_ls_on.load() && g_sh_wheel.load() && g_state[kSling].on.load() && look_done && g_sw_want.load() < 0 && !g_shp.on &&
            now - g_ls_since > std::chrono::milliseconds(2500) && !in.cur_side && (in.main_sig == 0 || in.sig != in.main_sig) &&
            ThrowPhase(g_th_phase.load()) == ThrowPhase::Idle && !g_v2_held.load() && !g_v2_rb.load()) {
            g_ls_pending = false;
            start_slot(g_sh_slot_for[0].load() - 1, fmt::format("loadslot (after the load the game holds weapon {}, not the main weapon {})", in.sig, in.main_sig));
            g_ls_fired = true;
        }
    }
    // load_slot_state (the load popup): 1 loadslot will take the slot, 2 its pick under way, 0 nothing to do.
    if (!g_shp.on) g_ls_fired = false;
    g_ls_pub = !in.allowed ? 0 : g_ls_fired ? 2
             : (g_ls_pending && g_ls_on.load() && g_sh_wheel.load() && g_state[kSling].on.load() && !in.cur_side && (in.main_sig == 0 || in.sig != in.main_sig)) ? 1 : 0;
    // shoulderpick N (test): slot N picked as its shoulder's grip would (the simulator's still controller reaches one shoulder only).
    if (const int k = g_sh_test_pick.exchange(0); k > 0 && in.allowed) start_slot(k - 1, fmt::format("shoulderpick {}", k));
    // A grip press goes to the zone it is deepest in (distance over zone size), if any.
    int took_weapon_hand = -1;   // the zone the weapon hand's press went to this poll (sidearm)
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
                if (g_miss_walk.load() && !g_miss_press[h]) {
                    g_miss_press[h] = true;
                    spdlog::info("[Gesture] {}: the press just outside its zone: the left stick keeps walking (no D-pad) until the grip is let go (gesturemisswalk)", kDefs[i].name);
                }
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
        if (h == kWeaponHand) took_weapon_hand = best;
        buzz(out, h, kFireAmp, kFireSec);
        if (d.kind == Kind::Tap) {
            const auto spots = sh_spots();
            if ((best == kShoulder || best == kShoulderL) && g_sh_wheel.load() && !spots.empty()) {
                // shoulderwheel (Homard 30 Oct: "make left shoulder weapon slot 1 and right shoulder weapon slot 2"): that shoulder's slot.
                start_slot(g_sh_slot_for[best == kShoulderL ? 0 : 1].load() - 1, fmt::format("{}: grip pressed {:.2f} m from the zone centre (zone {:.2f} m)", d.name, dist[best], s.radius.load()));
            } else {
                g_switch_count++;   // shoulderdraw (FarCry5WeaponSwitch.cpp): every tap gesture switches weapon
                s.tap_until = now + std::chrono::milliseconds(d.tap_ms);
                spdlog::info("[Gesture] {}: grip pressed {:.2f} m from the zone centre (zone {:.2f} m): pad buttons 0x{:04x} tapped", d.name, dist[best], s.radius.load(), d.buttons);
            }
            // shoulderdraw with the sling on: this grip press takes the new weapon in the hand, as a grab at the sling does (held
            // while the grip lasts, back on the sling when it is let go), and the grip keeps its usual action with the gun.
            if (g_switch_grab.load() && h == def_hand(kSling) && g_state[kSling].on.load()) {
                s.owned                = false;
                g_state[kSling].owned = true;
                spdlog::info("[Gesture] {}: the new weapon held in the hand while the grip lasts (shoulderdraw)", d.name);
            }
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
    // Sidearm: a grab of the sling or the hip that wants the weapon the game does not hold switches once: the hip picks the weapon
    // wheel's sidearm slot (sidearmkey wheel: LB held with the right stick to the right, the wheel's rightmost slot, then LB let go;
    // or a keyboard key), the sling the game's last weapon (pad Y). The switch is pending until the game has shown another weapon id
    // (150 ms, 0.6 s for an id never recorded: the switch shows one for 0.1-0.3 s) or 2.5 s have gone.
    const bool side   = side_on();
    g_cur_side        = in.cur_side;
    auto start_switch = [&](int want, const char* why, bool grab = true) {
        g_sw_want = want;
        g_sw_grab = grab;   // a grab (the copy goes in the hand), else the look after a load (the hand stays empty)
        g_sw_from = in.sig;
        g_sw_new  = 0;
        g_sw_t    = now;
        const int key = g_side_key.load();
        std::string how;
        g_sw_by_y = false;
        if (want == 1 && g_side_y.load() && in.side_sig != 0 && g_held_prev == in.side_sig && g_held_cur == in.sig) {
            g_sw_by_y      = true;
            g_wheel_picks  = 1;   // nothing in 1.3 s: the wheel pick
            g_sw_tap_until = now + std::chrono::milliseconds(120);
            how            = fmt::format("pad Y (the game's last weapon is the sidearm, {})", in.side_sig);
        } else if (want == 1 && key < 0) {
            g_wheel_until = now + std::chrono::milliseconds(std::clamp(g_wheel_ms.load(), 80, 1500));
            g_wheel_by_sh = false;   // the sidearm slot: the wheel's rightmost
            g_wheel_picks = 1;
            g_peek_quiet  = std::max(g_peek_quiet, g_wheel_until + std::chrono::milliseconds(1500));   // not a new throwable selection (throwpeek)
            how           = fmt::format("the weapon wheel's rightmost slot (LB and the right stick to the right, {} ms)", g_wheel_ms.load());
        } else if (want == 1 && key > 0) {
            out.key_scan  = uint16_t(key);
            g_wheel_picks = 1;
            how          = fmt::format("keyboard key 0x{:02x}", key);
        } else {
            g_sw_tap_until = now + std::chrono::milliseconds(120);
            how            = "pad Y (last weapon)";
        }
        note_weapon_switch();   // shoulderdraw's switch window (FarCry5WeaponSwitch.cpp): no holster or draw animation shown
        spdlog::info("[Gesture] sidearm: {} while the game holds weapon {} ({}): {} to switch", why, in.sig, in.cur_side ? "a sidearm" : "a main weapon", how);
    };
    if (side && in.allowed && in.weapon && (took_weapon_hand == kSling || took_weapon_hand == kSide) && g_sw_want.load() < 0) {
        const int want = took_weapon_hand == kSide ? 1 : 0;
        if (g_side_peek_step != 0) {
            g_side_peek_step = 0;
            spdlog::info("[Gesture] sidearm: look at the sidearm slot stopped (a grab)");
        }
        if (want != (in.cur_side ? 1 : 0)) start_switch(want, want ? "the hip grabbed" : "the sling grabbed");
    }
    // sidearmpeek (Homard 27 Oct: "the sidearm was not visible when loading"): the mod can only draw a weapon it has seen the game draw.
    // Once the game has been playable 0.5 s, with no grab held, with a recorded main weapon in hand and no sidearm recorded, it picks the sidearm
    // slot (as the hip), lets the mod record what the game shows there (held still 1 s), then switches back with the last weapon. The
    // game's weapon is hidden while each switch is pending (copies drawn), and its sidearm hangs on the hip meanwhile.
    {
        // A grip only counts when it holds something (a sling, hip or throwable grab): Homard's 28 Oct 21:16 session stood still 40 s with the
        // rifle slung and nothing in hand (21:18:28-21:19:08) and the look never ran; a grip pressed outside every zone holds nothing.
        const bool holding = g_state[kSling].owned || g_state[kSide].owned || g_state[kThrow].owned;
        // Walking is fine (Homard 29 Oct 09:39: "you can't do the look as soon as the game finish loading?"): only a grab, the wheel or a throw holds it.
        const bool calm = side && in.allowed && in.weapon && !in.wheel && !holding && ThrowPhase(g_th_phase.load()) == ThrowPhase::Idle;
        // Why the look waits, every 15 s once the game has been playable 20 s with no sidearm recorded.
        static clock::time_point said_t{};
        if (side && g_side_peek.load() && in.allowed && g_side_peek_step == 0 && !g_side_peeked && !in.side_recorded && now - g_side_allowed_since > std::chrono::seconds(20) &&
            now - said_t > std::chrono::seconds(15)) {
            said_t = now;
            spdlog::info("[Gesture] sidearm: the look at the sidearm slot waits: weapon {} (drawn {}, recorded {}, a sidearm {}), moving {}, wheel {}, holding {}, grips {}/{}, "
                         "throw {}, switch {}, calm for {:.1f} s",
                         in.sig, in.weapon, in.sig_known, in.cur_side, in.moving, in.wheel, holding, in.hand[kWeaponHand].grip, in.hand[kOtherHand].grip,
                         g_th_phase.load() != int(ThrowPhase::Idle) || g_v2_rb.load(), g_sw_want.load(), calm ? std::chrono::duration<float>(now - g_side_calm_since).count() : 0.0f);
        }
        if (calm && !g_side_calm_was) g_side_calm_since = now;
        g_side_calm_was = calm;
        if (!in.allowed) {
            if (now - g_side_allowed_t > std::chrono::seconds(5)) g_side_peeked = false;   // a load, a long menu: look again
        } else {
            if (now - g_side_allowed_t > std::chrono::milliseconds(500)) g_side_allowed_since = now;   // playable again after a gap
            g_side_allowed_t = now;
        }
        if (g_side_peek.load() && g_side_peek_step == 0 && !g_side_peeked && calm && !in.side_recorded && in.sig_known && !in.cur_side &&
            g_sw_want.load() < 0 && now - g_side_calm_since > std::chrono::milliseconds(300) && now - g_side_allowed_since > std::chrono::milliseconds(1500) && !g_v2_rb.load()) {   // 1.5 s playable, 0.3 s calm (29 Oct 09:39; 0.5 s: his 11:12 pick 1.1 s after the load was ignored by the game; was 2 s and 1 s, before that 5 s and 2 s) (Homard 29 Oct 09:20 "still no gun in holster for a long time on loading": 5 s + 2 s made his look start 8.8 s after the load); a look the load cuts is made again (his 27 Oct 21:08 log)
            g_side_peeked    = true;
            g_side_peek_step = 1;
            g_side_peek_t    = now;
            g_side_peek_from = in.sig;
            start_switch(1, "look at the sidearm slot (nothing recorded there yet)", false);
        } else if (g_side_peek_step == 1 && g_sw_want.load() < 0) {
            // Landed (or dropped): wait for the recording, then back.
            if (in.side_recorded || now - g_side_peek_t > std::chrono::seconds(6) || !in.allowed) {
                g_side_peek_step = 2;
                if (in.allowed && in.sig != g_side_peek_from && in.weapon) start_switch(0, in.side_recorded ? "the sidearm recorded: back" : "the sidearm not recorded in 6 s: back", false);
                else g_side_peek_step = 0;
            }
        } else if (g_side_peek_step == 2 && g_sw_want.load() < 0) {
            g_side_peek_step = 0;
            spdlog::info("[Gesture] sidearm: look at the sidearm slot done (sidearm {}recorded, now weapon {})", in.side_recorded ? "" : "not ", in.sig);
        }
        g_side_peek_pub = !side || !g_side_peek.load() ? -1 : g_side_peek_step != 0 ? 2 : g_side_peeked ? 1 : 0;   // side_peek_state (the load popup)
    }
    if (now < g_sw_tap_until) out.buttons |= XINPUT_GAMEPAD_Y;
    // shoulderwheel: the pick under way. Landed once the game shows another weapon id for 150 ms (0.6 s for one never recorded: the switch
    // shows a part for 0.1-0.3 s), which is then known to be that slot's. Nothing 0.7 s after the wheel let go: the slot is locked or empty
    // (the weapon held is another slot's), or the weapon held is probably that slot's own; no switch, the switch window ends at once.
    // Pad Y that brought nothing in 1 s, or the sidearm: the wheel instead.
    if (g_shp.on) {
        const auto spots = sh_spots();
        const int  n     = (int)spots.size();
        if (in.sig != 0 && in.sig != g_shp.from && in.sig != g_shp.got) {
            g_shp.got   = in.sig;
            g_shp.got_t = now;
        }
        const bool landed = g_shp.got != 0 && in.sig == g_shp.got && now - g_shp.got_t > std::chrono::milliseconds(in.sig_known ? 150 : 600);
        auto to_wheel = [&](const char* why) {
            g_shp.via_y = false;
            g_switch_count++;   // shoulderdraw: the switch window again
            g_shp.from = in.sig;
            press_slot(g_shp.idx);
            spdlog::info("[Gesture] shoulderswitch: {}: slot {} taken ({})", why, g_shp.idx + 1, g_shp.key ? "its keyboard key" : "the wheel");
        };
        if (landed && in.cur_side && !g_shp.side_again) {
            // The sidearm came (Homard's 29 Oct 13:44 log: picks held 150 ms read as an LB tap, the game's quick switch to its last weapon,
            // which was the pistol): never a slot 1-3 weapon, never learned for the slot; the slot picked through the wheel once more.
            g_shp.side_again = true;
            to_wheel(fmt::format("the sidearm {} came (read as the game's last weapon)", in.sig).c_str());
        } else if (landed) {
            if (!in.cur_side && !g_shp.via_y) {
                g_sh_slot_of[in.sig] = g_shp.idx;
                g_sh_weak.erase(in.sig);
                if (g_shp.idx < (int)g_sh_empty_t.size()) g_sh_empty_t[g_shp.idx] = clock::time_point{};
                g_sh_miss[g_shp.idx] = 0;
            }
            spdlog::info("[Gesture] shoulderswitch: weapon {} -> {} at wheel slot {} in {} ms ({}){}", g_shp.from, in.sig, g_shp.idx + 1,
                         std::chrono::duration_cast<std::chrono::milliseconds>(now - g_shp.t).count(), g_shp.via_y ? "pad Y" : "the wheel",
                         in.cur_side ? " (the sidearm again: not learned for the slot)" : "");
            g_shp.on = false;
        } else if (!in.allowed || n == 0 || now - g_shp.t > std::chrono::milliseconds(4000)) {
            spdlog::info("[Gesture] shoulderswitch: pick dropped ({})", !in.allowed ? "not on foot with motion controls" : "no other weapon in 4 s");
            g_shp.on = false;
        } else if (g_shp.via_y && g_shp.got == 0 && now > g_shp.y_until + std::chrono::milliseconds(900)) {
            to_wheel("pad Y brought nothing in 0.9 s");
        } else if (!g_shp.via_y && g_shp.got == 0 && now > (g_shp.key ? g_shp.t + std::chrono::milliseconds(900) : g_wheel_until + std::chrono::milliseconds(700))) {
            const int i = g_shp.idx;
            if ((int)g_sh_empty_t.size() != n) g_sh_empty_t.assign(n, clock::time_point{});
            const auto it    = g_sh_slot_of.find(g_shp.from);
            const auto wk    = g_sh_weak.find(g_shp.from);
            const int  other = it != g_sh_slot_of.end() ? it->second : wk != g_sh_weak.end() ? wk->second : -1;
            const bool own   = g_shp.from != 0 && other < 0 && !in.cur_side;
            if (own) g_sh_weak[g_shp.from] = i;                     // probably its own slot (a pick the game did not read gives nothing too)
            else if (i < n && other != i && ++g_sh_miss[i] >= 2) g_sh_empty_t[i] = now;   // another slot's weapon held, twice: locked or empty
                                                                                         // (once may be a pick the game read with the mouse: Homard 11:42, the wheel in keyboard mode)
            spdlog::info("[Gesture] shoulderswitch: wheel slot {} gave no other weapon ({}): no switch", i + 1,
                         own ? fmt::format("weapon {} in hand is probably that slot's", g_shp.from) : std::string{ "locked or empty" });
            g_shp.on = false;
            g_sh_drops++;   // shoulderdraw: the switch window ends now (the weapon held shows again)
        }
        if (g_shp.on) out.no_fire = true;
        if (g_shp.on && g_shp.via_y && now < g_shp.y_until) out.buttons |= XINPUT_GAMEPAD_Y;
    }
    // The player's own wheel (not a pick of ours): the loadout may have changed, the slots that gave nothing are looked at again.
    if (in.wheel && now > g_wheel_until + std::chrono::milliseconds(500) && !g_shp.on) {
        for (auto& e : g_sh_empty_t) e = clock::time_point{};
        g_sh_weak.clear();
    }
    g_wheel_hide = now < g_wheel_until + std::chrono::milliseconds(200);   // wheel_pick_now (the render thread)
    if (now < g_wheel_until) {
        out.wheel   = true;   // ModConfig: LB held, the right stick toward the spot (the sidearm's: right)
        out.wheel_x = g_wheel_by_sh ? g_sh_x : 1.0f;
        out.wheel_y = g_wheel_by_sh ? g_sh_y : 0.0f;
    }
    // sidearmy: the game's weapons held (an id shown 0.6 s, not while a throwable is out), for its last weapon (pad Y).
    if (in.sig != g_held_cand) {
        g_held_cand   = in.sig;
        g_held_cand_t = now;
    }
    if (g_held_cand != 0 && g_held_cand != g_held_cur && in.weapon && now - g_held_cand_t > std::chrono::milliseconds(600) &&
        ThrowPhase(g_th_phase.load()) == ThrowPhase::Idle && !g_v2_held.load() && !g_v2_rb.load()) {
        g_held_prev = g_held_cur;
        g_held_cur  = g_held_cand;
    }
    if (g_sw_want.load() >= 0) {
        if (in.sig != 0 && in.sig != g_sw_from && in.sig != g_sw_new) {
            g_sw_new   = in.sig;
            g_sw_new_t = now;
        }
        // A slot pick (the wheel or the slot's key) that gave no other weapon in 1.3 s: picked once more, the switch's time restarted.
        // Not with no sidearm known (the wheel's pick): the game kept its weapon, so that is the slot's (Homard 28 Oct 22:10, loaded with his pistol
        // out: the look's second pick switched to his rifle 57819, which was then taken for the sidearm, his pistol hung on the sling).
        // Not a weapon outside the saved sidearm list (Homard 29 Oct 11:12: the look 1.1 s after a load, the game ignored the pick, his rifle 57819 was
        // taken for the sidearm): then the pick is made again.
        const bool kept_once = g_sw_want.load() == 1 && g_side_key.load() < 0 && !g_sw_by_y && !in.side_known && !in.side_unlisted && g_wheel_picks == 1 && g_sw_new == 0 &&
                               now - g_sw_t > std::chrono::milliseconds(1300) && in.allowed && in.sig != 0 && in.sig == g_sw_from;
        if (kept_once) {
            g_side_landed = in.sig;
            g_sw_want     = -1;
            spdlog::info("[Gesture] sidearm: the game kept weapon {} at the sidearm slot (the pick gave no other in 1.3 s): that is the sidearm", in.sig);
        } else if (g_sw_want.load() == 1 && g_side_key.load() != 0 && g_wheel_picks == 1 && g_sw_new == 0 && now - g_sw_t > std::chrono::milliseconds(1300) && in.allowed) {
            g_wheel_picks = 2;
            g_sw_t        = now;
            if (g_side_key.load() > 0) out.key_scan = uint16_t(g_side_key.load());
            else {
                g_wheel_until = now + std::chrono::milliseconds(std::clamp(g_wheel_ms.load(), 80, 1500));
                g_wheel_by_sh = false;   // the sidearm slot: the wheel's rightmost
                g_peek_quiet  = std::max(g_peek_quiet, g_wheel_until + std::chrono::milliseconds(1500));
            }
            spdlog::info("[Gesture] sidearm: the slot pick gave no other weapon in 1.3 s: picked again");
        }
        // With shoulderdraw the new weapon stays hidden until its held pose is known: the copy stays in the hand until then.
        bool landed = g_sw_new != 0 && in.sig == g_sw_new && now - g_sw_new_t > std::chrono::milliseconds(in.sig_known ? 150 : 600) && !in.hiding;   // an unknown id: 0.6 s (the switch shows one for 0.1-0.3 s)
        // sidearmy: pad Y brought another weapon than the sidearm (the last weapon guessed wrong): the wheel's sidearm slot from there.
        if (landed && g_sw_want.load() == 1 && g_sw_by_y && in.sig != in.side_sig) {
            spdlog::info("[Gesture] sidearm: pad Y brought weapon {}, not the sidearm {}: the weapon wheel's rightmost slot now", in.sig, in.side_sig);
            g_sw_by_y     = false;
            g_sw_from     = in.sig;
            g_sw_new      = 0;
            g_sw_t        = now;
            g_wheel_picks = 2;
            g_wheel_until = now + std::chrono::milliseconds(std::clamp(g_wheel_ms.load(), 80, 1500));
            g_wheel_by_sh = false;   // the sidearm slot: the wheel's rightmost
            g_peek_quiet  = std::max(g_peek_quiet, g_wheel_until + std::chrono::milliseconds(1500));
            landed        = false;
        }
        if (landed || now - g_sw_t > std::chrono::milliseconds(2500) || !in.allowed) {
            if (landed && g_sw_want.load() == 1) g_side_landed = in.sig;
            if (landed)
                spdlog::info("[Gesture] sidearm: switched from weapon {} to {} in {} ms ({}{})", g_sw_from, in.sig, std::chrono::duration_cast<std::chrono::milliseconds>(now - g_sw_t).count(),
                             in.cur_side ? "a sidearm" : "a main weapon",
                             (in.cur_side ? 1 : 0) == g_sw_want.load() ? "" : g_sw_want.load() ? ": not known as a sidearm yet (learned once held still; if it is not your sidearm, the game's last weapon was another one)"
                                                                                                : ": the game's last weapon was a sidearm too");
            else if (g_sw_want.load() == 1 && g_side_key.load() != 0 && g_wheel_picks == 2 && in.allowed && in.sig != 0 && in.sig == g_sw_from && !in.side_known && !in.side_unlisted) {   // only with no sidearm known: a pick that did nothing is not proof (Homard 27 Oct 13:46: his rifle taken for the sidearm)
                // The wheel's rightmost slot picked and the game kept its weapon: that slot holds it (simulator 27 Oct, Homard's save loads with
                // its sidearm in hand, which hung on the sling until then).
                g_side_landed = in.sig;
                spdlog::info("[Gesture] sidearm: the game kept weapon {} at the sidearm slot (picked twice): that is the sidearm", in.sig);
            } else spdlog::info("[Gesture] sidearm: no other weapon came in {} ms (still weapon {}): switch dropped", std::chrono::duration_cast<std::chrono::milliseconds>(now - g_sw_t).count(), in.sig);
            if (!landed && !in.allowed && g_side_peek_step == 1) g_side_peeked = false;   // the look after a load cut by the load itself: again later (his 21:08 log)
            g_sw_want = -1;
        }
        out.no_fire = true;
    }
    // In the hand: a sling or hip grab held, once no switch is pending (the game holds the weapon it switched to, whichever it is).
    const bool  held  = g_state[kSling].owned || (side && g_state[kSide].owned);
    const bool  slung = g_state[kSling].on.load() && in.allowed && in.weapon && !(held && g_sw_want.load() < 0);
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
        if (took && !g_v2_held.load() && ph == ThrowPhase::Idle && g_peek_step == 0 && g_v2_cxl_step == 0) {
            // throwrock (Homard: "make the rock being selected"): the game drew no throwable at the last peek (none selected, none left):
            // this one is a rock (the game's own rock throw, D-pad down; endless).
            g_rock_now = g_rock.load() && g_peek_failed;
            if (g_rock_now) spdlog::info("[Throw] v2: no throwable to draw at the last peek: a rock (D-pad down) this time");
            g_v2_held  = true;
            g_v2_armed = g_v2_was_armed = false;
            g_v2_rb    = g_v2_rb_grab.load() && !g_rock_now;
            set_phase(ThrowPhase::Held, now);
            ph = ThrowPhase::Held;
            spdlog::info("[Throw] v2: taken in the {} hand ({})", left ? "left" : "right", g_v2_rb.load() ? "RB held from the grab: the game draws the selected throwable" : "no RB: no raise animation");
        }
        bool release = false, cancel = false;
        if (g_v2_held.load() && g_v2_cxl_step == 0) {
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
            // Not on foot any more: throwv2rb grab puts it back (through the game's cancel); arm and tap throw it if RB is held (as before).
            if (!in.allowed) cancel = !g_v2_rb_grab.load() ? !g_v2_rb.load() : true, release = g_v2_rb_grab.load() ? false : g_v2_rb.load();
        }
        g_v2_arc = g_v2_held.load() && g_v2_armed && in.other_fwd_ok;
        // throwpreaim: while the trajectory shows, the game's aim already follows it, so letting go throws at once.
        if (g_v2_arc.load() && g_preaim.load()) {
            std::lock_guard _{ g_th_mtx };
            g_th_dir    = in.other_fwd;
            g_th_launch = true;
        }
        if (cancel) {
            spdlog::info("[Throw] v2: let go before the throw: back on the chest{}", g_v2_rb.load() ? fmt::format(" (RB held: cancel 0x{:04x} with RB, twice, then RB let go)", g_peek_cancel.load()) : "");
            if (g_v2_rb.load()) {
                g_v2_cxl_step = 1;
                g_v2_cxl_t    = now;
            }
            g_v2_rb   = false;
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
        if (g_v2_cxl_step > 0) {
            const auto cs = now - g_v2_cxl_t;
            const auto ms = std::chrono::milliseconds(g_v2_cxl_step == 2 ? 120 : g_v2_cxl_step == 4 ? 600 : g_peek_cancel_ms.load());
            if (g_v2_cxl_step < 4) out.buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER | (g_v2_cxl_step == 2 ? 0 : uint16_t(g_peek_cancel.load()));
            if (cs > ms) {
                g_v2_cxl_t = now;
                if (++g_v2_cxl_step > 4) g_v2_cxl_step = 0;
            }
            out.no_fire = true;
            out.grip_taken[kWeaponHand] = out.grip_taken[kOtherHand] = true;
        }

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
    for (int h = 0; h < 2; ++h) out.miss_press[h] = g_miss_press[h];
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
    if (cmd == "gesturemisswalk") { g_miss_walk = on; persist = true; return true; }
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
    if (cmd == "throwv2rb") { g_v2_rb_arm = arg != "tap"; g_v2_rb_grab = arg == "grab"; persist = true; return true; }
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
        g_t2_arm  = arm <= 0 ? 0 : std::clamp(arm, 50, 20000);   // 0: no trajectory, the holding grip let go after hold ms (a put-back)
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
    if (cmd == "sidearmpos" || cmd == "sidearmrot") {
        float v[3]{ fv, 0.0f, 0.0f };
        rest >> v[1] >> v[2];
        // A saved line holding the old default place takes the new one (27 Oct: the F11 defaults were saved as they were).
        if (cmd == "sidearmpos" && std::abs(v[0] - 0.20f) < 0.001f && std::abs(v[1] - 0.72f) < 0.001f && std::abs(v[2] + 0.02f) < 0.001f) {
            for (int i = 0; i < 3; ++i) v[i] = kHpPosDefault[i];
            spdlog::info("[Gesture] sidearm: the old default place (0.20 0.72 -0.02) taken as the new one ({:.2f} {:.2f} {:.2f})", v[0], v[1], v[2]);
        }
        float in[3]{}, out[3]{};
        get_hip_place(in, out);
        if (cmd == "sidearmpos") set_hip_place(v, out);
        else set_hip_place(in, v);
        persist = true;
        return true;
    }
    if (cmd == "sidearmkey") {
        // Replaced by sidearmpick (27 Oct): saved lines from the number-key tries (keys 1 and 4 gave rifles in Homard's save) are ignored.
        spdlog::info("[Gesture] sidearm: 'sidearmkey {}' ignored (replaced by sidearmpick; the wheel's rightmost slot by default)", arg);
        return true;
    }
    if (cmd == "sidearmpick") {
        // wheel (default): the weapon wheel's rightmost slot (LB held, the right stick to the right, LB let go). pad: the game's last weapon
        // (pad Y). 1-9: that keyboard number key (scan codes 0x02-0x0a). Used for the switch to the sidearm; the switch back is the last weapon.
        const int k = (arg.size() == 1 && arg[0] >= '1' && arg[0] <= '9') ? 0x02 + (arg[0] - '1') : arg == "pad" ? 0 : arg == "wheel" ? -1 : -1;
        g_side_key  = k;
        persist     = true;
        spdlog::info("[Gesture] sidearm: the switch to the sidearm {}", k > 0 ? fmt::format("taps keyboard key {} (scan code 0x{:02x})", arg, k)
                                                                         : k == 0 ? std::string{ "taps pad Y (last weapon)" } : std::string{ "picks the weapon wheel's rightmost slot" });
        return true;
    }
    if (cmd == "sidearmzone") {
        float v[3]{ fv, 0.0f, 0.0f };
        rest >> v[1] >> v[2];
        for (int i = 0; i < 3; ++i) g_hp_zone[i] = std::clamp(v[i], -0.3f, 0.3f);
        persist = true;
        return true;
    }
    if (cmd == "sidearmwheelms") { g_wheel_ms = std::clamp(int(fv), 80, 1500); persist = true; return true; }
    if (cmd == "sidearmy") { g_side_y = on; persist = true; return true; }
    if (cmd == "shoulderwheelms") { g_sh_wheel_ms = std::clamp(int(fv), 80, 1500); persist = true; return true; }
    if (cmd == "shoulderpick") { g_sh_test_pick = std::clamp(int(fv), 1, 8); return true; }   // test: slot N picked as its shoulder's grip would
    if (cmd == "shoulderkeys") { g_sh_keys = on; persist = true; return true; }   // the shoulders press the weapon slot keys 1-4 (off: the weapon wheel)
    if (cmd == "loadslot") { g_ls_on = on; persist = true; return true; }   // after a load with no main weapon out, the left shoulder's slot taken
    if (cmd == "shoulderleftslot" || cmd == "shoulderrightslot") {   // the wheel slot (1-3) that shoulder takes (Homard 30 Oct: "they don't have separated slots in the F11 menu")
        g_sh_slot_for[cmd == "shoulderrightslot" ? 1 : 0] = std::clamp(int(fv), 1, 4);
        persist = true;
        return true;
    }
    if (cmd == "shoulderwheel") {
        g_sh_wheel = on;
        persist    = true;
        spdlog::info("[Gesture] shoulderwheel {}: the shoulder switch {}", on ? "on" : "off", on ? "picks the next of its weapon wheel spots (shoulderslots), never the sidearm's" : "taps pad Y (the game's last weapon)");
        return true;
    }
    if (cmd == "shoulderslots") {
        // The wheel spots the shoulder switch goes round, in order: left, upleft, up, upright, right (the sidearm's), or degrees from the top, clockwise.
        std::vector<float> v;
        std::string        tok = arg;
        do {
            if (tok == "left") v.push_back(-90.0f);
            else if (tok == "upleft") v.push_back(-32.0f);
            else if (tok == "up") v.push_back(0.0f);
            else if (tok == "upright") v.push_back(30.0f);
            else if (tok == "right") v.push_back(90.0f);
            else if (!tok.empty() && (std::isdigit((unsigned char)tok[0]) || tok[0] == '-')) v.push_back(std::strtof(tok.c_str(), nullptr));
        } while (rest >> tok);
        if (v.empty()) return true;
        {
            std::lock_guard _{ g_sh_mtx };
            g_sh_spots = v;
        }
        persist = true;
        std::string s;
        for (float a : v) s += fmt::format(" {:.0f}", a);
        spdlog::info("[Gesture] shoulderslots: the shoulder switch goes round {} wheel spots (deg from the top, clockwise):{}", v.size(), s);
        return true;
    }
    if (cmd == "sidearmpeek") { g_side_peek = on; persist = true; return true; }
    if (cmd == "slingpos" || cmd == "slingrot") {
        float v[3]{ fv, 0.0f, 0.0f };
        rest >> v[1] >> v[2];
        if (cmd == "slingpos" && std::abs(v[0] - 0.09f) < 0.001f && std::abs(v[1] - 0.50f) < 0.001f && std::abs(v[2] - 0.10f) < 0.001f) {
            for (int i = 0; i < 3; ++i) v[i] = kSlPosDefault[i];   // the old default place saved: the new one (27 Oct, closer to the body)
            spdlog::info("[Gesture] sling: the old default place (0.09 0.50 0.10) taken as the new one ({:.2f} {:.2f} {:.2f})", v[0], v[1], v[2]);
        }
        float in[3]{}, out[3]{};
        get_sling_place(in, out);
        if (cmd == "slingpos") set_sling_place(v, out);
        else set_sling_place(in, v);
        persist = true;
        return true;
    }
    if (cmd == "sidearmsize" && std::abs(fv - 0.13f) < 0.001f) {   // the old default saved: the new one (27 Oct, his presses missed by 0.14-0.17 m)
        g_state[kSide].radius = kDefs[kSide].radius;
        persist               = true;
        return true;
    }
    for (int i = 0; i < kCount; ++i) {
        const std::string n = kDefs[i].name;
        if (cmd == n) { g_state[i].on = on; persist = true; if (i == kSide && on) side_switched_on(); return true; }
        // shoulderswitchsize: the old default saved takes the new one (Homard 27 Oct 21:41-21:48: 4 presses 0.21-0.23 m from the centre, all retried).
        if (cmd == "shoulderswitchsize" && std::abs(fv - 0.20f) < 0.001f) fv = 0.24f;
        if (cmd == n + "size") { g_state[i].radius = std::clamp(fv, 0.05f, 0.5f); persist = true; return true; }
    }
    return false;
}

bool test_grip() { return g_test_grip.load(); }
uint32_t weapon_switch_count() { return g_switch_count.load(); }
void     note_weapon_switch() { g_switch_count++; }
uint32_t shoulder_pick_target() { return g_shp.on ? g_sh_target.load() : 0u; }
uint32_t shoulder_pick_drops() { return g_sh_drops.load(); }
bool     shoulder_wheel_on() { return g_sh_wheel.load(); }
int      shoulder_slot(int right) { return g_sh_slot_for[right ? 1 : 0].load(); }
void     set_shoulder_slot(int right, int slot) { g_sh_slot_for[right ? 1 : 0] = std::clamp(slot, 1, 4); }
void     set_switch_grab(bool on) { g_switch_grab = on; }
bool test_input() { return g_test_grip.load() || g_tt_req.load() || g_tt_step > 0 || g_t2_req.load() || g_t2_on || clock::now() < g_padtap_until; }
bool miss_walk() { return g_miss_walk.load(); }
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

bool side_on() { init(); return g_state[kSide].on.load() && g_state[kSling].on.load(); }
bool cur_side() { return g_cur_side.load(); }
int  switch_want() { return side_on() ? g_sw_want.load() : -1; }
bool switch_grabbed() { return g_sw_grab.load(); }
int  side_key() { return g_side_key.load(); }
bool side_peek_on() { return g_side_peek.load(); }
int  side_peek_state() { return g_side_peek_pub.load(); }
int  load_slot_state() { return g_ls_pub.load(); }
bool side_y_on() { return g_side_y.load(); }
bool wheel_pick_now() { return g_wheel_hide.load(); }
void side_peek_again() { g_side_peeked = false; }
uint32_t take_side_landed() { return g_side_landed.exchange(0); }

void get_hip_place(float pos[3], float rot[3])
{
    for (int i = 0; i < 3; ++i) {
        pos[i] = g_hp_pos[i].load();
        rot[i] = g_hp_rot[i].load();
    }
}

void set_hip_place(const float pos[3], const float rot[3])
{
    g_hp_pos[0] = std::clamp(pos[0], -0.5f, 0.5f);
    g_hp_pos[1] = std::clamp(pos[1], 0.0f, 1.2f);
    g_hp_pos[2] = std::clamp(pos[2], -0.4f, 0.6f);
    for (int i = 0; i < 3; ++i) g_hp_rot[i] = std::clamp(rot[i], -180.0f, 180.0f);
}

void reset_hip_place() { set_hip_place(kHpPosDefault, kHpRotDefault); }

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
bool throw_v2_rb_grab() { return g_v2_rb_grab.load(); }
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
void        set_enabled(int i, bool on) { init(); g_state[i].on = on; if (i == kSide && on) side_switched_on(); }
float       radius(int i) { init(); return g_state[i].radius.load(); }
void        set_radius(int i, float m) { init(); g_state[i].radius = std::clamp(m, 0.05f, 0.5f); }
float       default_radius(int i) { return kDefs[i].radius; }
float       last_distance(int i) { return g_state[i].dist.load(); }

}
