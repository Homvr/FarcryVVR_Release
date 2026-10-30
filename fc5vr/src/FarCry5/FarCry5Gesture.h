#pragma once
// Body gestures: a controller's grip pressed inside a zone on the body. The body is estimated from the
// headset pose (a shoulder: a bit below, behind and to the side of the head; the sling: in front of the
// belly, turning with the body's heading). One table entry per gesture in FarCry5Gesture.cpp; a new gesture
// is a new entry (its kind, hand, body anchor and pad buttons), with its own on/off and zone size commands.
//
// Tap gestures (shoulderswitch): a press that starts inside the zone taps pad buttons once and is not the
// grip's usual action until released. Hold gestures (sling, throwable): a press that starts inside the zone holds the
// thing until the grip is released. The throwable (left chest, other hand): letting go while swinging throws it the
// way the hand was moving (see throw_aim()); letting go without a swing throws it where the head looks. A press goes
// to the zone the hand is deepest in. A free hand entering a zone gets a light buzz (gesturebuzz).
#include <cstdint>
#include <iosfwd>
#include <string>
#include <glm/glm.hpp>

namespace fc5::gesture {

// Hands by role: the weapon hand is the right controller (the left one when left-handed).
enum Hand : int { kWeaponHand = 0, kOtherHand = 1 };

struct HandState
{
    glm::vec3 pos{ 0.0f };   // tracking space, metres (y up)
    bool      tracked{ false };
    bool      grip{ false };
};

// The throw's phases (throwable gesture): Held = in the hand; Aim = let go with a swing, the game camera turns to the
// throw's direction; Throw = the game's throw button (pad RB) released / tapped; Follow = the camera kept on the throw's
// direction while the game's animation lets the throwable go.
enum class ThrowPhase : int { Idle = 0, Held, Aim, Throw, Follow };

struct Input
{
    glm::mat4 head{ 1.0f };   // headset pose in the same space (x right, y up, -z forward)
    bool      head_ok{ false };
    HandState hand[2];
    bool      mirror{ false };   // left-handed: the weapon hand's side is the left
    bool      allowed{ false };  // on foot with motion controls, no menu or flat screen
    bool      weapon{ false };   // the game shows a weapon in hand (not bare hands)
    bool      throw_aimed{ false };   // the game camera is on the throw's direction (the aim loop's verdict)
    glm::vec3 other_fwd{ 0.0f, 0.0f, -1.0f };   // throwv2: where the other hand's controller points (tracking space, unit)
    bool      other_fwd_ok{ false };
    bool      wheel{ false };          // the weapon wheel is open (throwv2's peek waits for it to close)
    bool      recorded{ false };       // throwv2: the mod has the selected throwable's model (its peek has nothing to do)
    bool      moving{ false };         // walking, running or sprinting (or within 1 s of it): no peek then
    uint32_t  sig{ 0 };                // sidearm: the game's weapon id now (m_weapon_sig)
    bool      cur_side{ false };       // sidearm: that weapon is a sidearm (FarCry5Holster.cpp)
    bool      hiding{ false };         // sidearm: shoulderdraw hides the game's weapon now (weapon_switch_hiding)
    bool      sig_known{ false };      // sidearm: that weapon has been recorded (a real weapon, not an id the switch animation shows)
    bool      side_recorded{ false };  // sidearm: the sidearm (the slot's weapon) has been recorded this session (sidearmpeek has nothing to do)
    bool      side_known{ false };     // sidearm: the sidearm slot's weapon is known (sidearmsig): a wheel pick that keeps the weapon in hand teaches nothing
    uint32_t  side_sig{ 0 };           // sidearm: that weapon (0: not known yet)
    uint32_t  main_sig{ 0 };           // sidearm: the main weapon last held still (0: none known; FarCry5Holster.cpp)
    bool      side_unlisted{ false };  // sidearm: the saved sidearm list holds weapons and the one in hand is not among them (no "kept" proof then)
};

struct Output
{
    uint16_t buttons{ 0 };            // XINPUT_GAMEPAD_* bits to add this poll
    bool     grip_taken[2]{};         // that hand's grip belongs to a gesture: no usual grip action
    bool     miss_press[2]{};         // gesturemisswalk: that grip was pressed just outside a zone (the left stick stays the stick)
    float    buzz[2]{};               // haptic pulse on that hand this poll: amplitude (0 = none) ...
    float    buzz_s[2]{};             // ... and length (s): light on entering a zone, firmer when a gesture fires
    bool     slung{ false };          // the weapon hangs on the sling (not in the hand): it must not fire
    bool     no_fire{ false };        // a throw is under way (or a sidearm switch): the right trigger must not fire
    uint16_t key_scan{ 0 };           // sidearm with sidearmkey N: a keyboard key (scan code) to tap once now
    bool     wheel{ false };           // sidearm (sidearmkey wheel): LB held and the right stick at (wheel_x, wheel_y) this poll: the wheel's slot pick
    float    wheel_x{ 0.0f }, wheel_y{ 0.0f };
};

// Called once per pad poll (the XInput hook). Keeps the press / pulse / sling state.
Output update(const Input& in);

// Commands (bin\fc5vr_cmd.txt / settings): "<name> on|off" and "<name>size <metres>" per gesture (shoulderswitch,
// sling, throwable), gesturebuzz on|off, gesturezones on|off (debug, not saved), slingpos right down forward (m), slingrot
// left down roll (deg), slingzone right down forward (m), gestureshoulder side down back (m), throwpos side down forward (m), throwmin m/s, throwgate deg, throwwait / throwkeep / throwtap ms, throwhold on|off; tests:
// gesturegrip on|off (presses the weapon hand's grip), throwtest vx vy vz [ms [hold_ms]],
// padtap hexbuttons ms, throwlog on|off. Returns true when the command was a gesture command; persist is set for the
// saved ones.
bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
bool test_grip();
bool test_input();
// shoulderdraw (FarCry5WeaponSwitch.cpp): how many weapon-switch gestures have fired (each tap gesture, and each
// note_weapon_switch() from a gesture that switches weapon some other way). A change starts the switch window.
uint32_t weapon_switch_count();
void     note_weapon_switch();
// shoulderwheel on|off (saved, default on; Homard 30 Oct: "make left shoulder weapon slot 1 and right shoulder weapon slot 2"): the weapon hand
// at the left shoulder (shoulderleft, new, on, saved) picks the weapon wheel's slot 1, at the right shoulder (shoulderswitch) slot 2, through the
// wheel instead of pad Y (shoulderslots, saved, default left upleft up upright: the wheel spots of his slots 1-4, from the left); never the sidearm. A slot
// whose weapon is in hand (known from an earlier pick) is not picked; a pick that gave nothing is made once more. shoulder_pick_target(): while
// a pick is under way, the weapon that slot gave before (0: not known); shoulder_pick_drops(): picks that gave no weapon (the switch window ends).
uint32_t shoulder_pick_target();
uint32_t shoulder_pick_drops();
bool     shoulder_wheel_on();
// shoulderleftslot / shoulderrightslot N (saved, default 1 / 2): the wheel slot each shoulder takes: 1-4 from the left of the weapon wheel (left, upper-left, top, upper-right; the rightmost is the sidearm).
// shoulderkeys on|off (saved, default on; Homard 30 Oct 12:43): the slot is taken with the game's weapon slot key N (keyboard 1-4) instead of the wheel.
int      shoulder_slot(int right);
void     set_shoulder_slot(int right, int slot);
// shoulderdraw on: with the sling on, a tap gesture's grip press also holds the weapon in the hand (the sling's hold) while
// the grip lasts, so the new weapon is in the hand, not on the sling.
void     set_switch_grab(bool on);   // a gesture test drives the controllers (gesturegrip, throwtest): counts as controller use
bool miss_walk();                 // gesturemisswalk on|off (saved): a press just outside a zone leaves the left stick walking
bool zone_buzz();                 // gesturebuzz on|off (saved): the light buzz on entering any gesture zone
void set_zone_buzz(bool on);

// gesturezones on|off (debug, not saved; F11 Gestures): the zones drawn in both eyes (FarCry5GestureZones.cpp). zone_views()
// gives each zone's centre (tracking space) for this head pose and its state at the last poll; hand_point() the point of that
// hand the zones test (tracking space, the last poll).
struct ZoneView
{
    glm::vec3 centre{ 0.0f };
    float     radius{ 0.0f };
    int       hand{ 0 };
    bool      on{ false };          // switched on
    bool      available{ false };   // it can act now (on foot, motion controls, a weapon in hand for the sling)
    bool      inside{ false };      // its hand is in the zone
    bool      held{ false };        // its grip press began in the zone and lasts (the sling: the weapon in hand)
};
bool zone_show();
void set_zone_show(bool on);
int  zone_views(const glm::mat4& head, bool mirror, ZoneView* out, int max);
bool hand_point(int hand, glm::vec3& pos);

// Sling. The weapon's pose on the sling, tracking space, from the head pose: x right, y up, -z along the barrel,
// origin = the weapon's grip (where the hand holds it). slung(): the last poll put the weapon on the sling.
glm::mat4 sling_pose(const glm::mat4& head, bool mirror);
bool      slung();
bool      sling_on();   // sling mode switched on (cmd sling)
void      get_sling_place(float pos[3], float rot[3]);   // right / down / forward (m), left / down / roll (deg)
void      set_sling_place(const float pos[3], const float rot[3]);
// slingzone right down forward (m, saved, default 0 0 0): the sling zone's centre from the weapon's grip (body frame), so the zone
// moves without the weapon. reset_sling_place(): slingpos and slingrot back to their defaults (FarCry5Defaults.h);
// reset_sling_zone(): slingzone 0 0 0; reset_sling(): both.
void      get_sling_zone(float off[3]);
void      set_sling_zone(const float off[3]);
void      reset_sling_place();
void      reset_sling_zone();
void      reset_sling();

// Throwable. Its place on the chest, tracking space, from the head pose: x right, y up, -z forward (the body's
// heading), origin = where the hand takes it.
glm::mat4  throw_pose(const glm::mat4& head, bool mirror);
bool       throw_on();
ThrowPhase throw_phase();
// While aiming, throwing and following: true, with the throw's direction (unit, tracking space) and the hand's release
// speed (m/s).
bool       throw_aim(glm::vec3& dir, float& speed);
float      throw_gate_deg();   // throwgate: how close (deg) the game camera must come to the throw's direction
void       get_throw_place(float pos[3]);   // to the other hand's side / down / forward (m)
void       set_throw_place(const float pos[3]);
void       reset_throw();   // throwpos back to its default

// throwv2 on|off (saved, default off; Homard 24 Oct 21:59, F11 Experimental > Throwables (test)): taking the throwable at the
// chest only puts it in the hand: no pad RB, so the game's raise animation does not play and the mod draws its recorded copy of
// the throwable in that hand. The trajectory shows while both grips are held, along where the other hand's (left) controller
// points; the throw goes when the weapon hand's (right) grip is let go, along it. throwv2hand left|right (saved, default left):
// the hand that takes it from the chest; left: a left grip let go before the right grip was pressed puts it back on the chest;
// right: the right grip holds it and letting it go throws along the trajectory if the left grip was held since, else puts it back.
// throwv2zone m (saved, default 0.12): the throwable's zone with throwv2 (a press meant for the sling took it at 0.22).
// throwv2rb arm|tap (saved, default arm): RB is held from the moment the trajectory shows (the game's wind-up plays then, the throw
// leaves sooner) or only tapped at the release. throwpeek on|off + throwpeekcancel <hex pad buttons>: see throw_peek().
bool throw_v2();
bool throw_v2_right();       // throwv2hand right
bool throw_v2_rb_arm();      // throwv2rb arm
bool throw_v2_rb_grab();     // throwv2rb grab: RB held from the grab (the pouch)
bool throw_peek_on();
bool throw_rock();          // throwrock on|off (saved, default off): with no throwable to draw at the last peek, the gesture throws a rock (D-pad down)
bool throw_is_rock();       // the throw under way is a rock
bool throw_preaim();         // throwpreaim on|off (saved, default on): the game's aim follows the trajectory while it shows
int  throw_hold_hand();     // the hand role holding the throwable in throwv2 (kOtherHand: the left; kWeaponHand: the right)
bool throw_arc_shown();      // throwv2: the trajectory is shown now
bool throw_launch_aimed();   // the throw under way goes along the trajectory: throw_aim's direction is the launch's, not the camera's
bool throw_rb_held();        // throwv2: the gesture holds RB now (the game draws its own throwable)
// throwpeek (saved, default on; only with throwv2): with nothing recorded yet (the start, or after the weapon wheel closes) the gesture holds RB for a moment so
// the game draws the selected throwable and the mod records it, then taps throwpeekcancel (pad buttons, hex, default 4000: X, the game's "Cancel attack") while RB is still held,
// and lets RB go. peeking(): RB is held for it now; peek_seen(): the render side recorded it (the cancel follows).
bool throw_peeking();
void throw_peek_seen();
void throw_peek_again();     // the selection may have changed (the wheel closed): peek again

// Sidearm (sidearm on|off, saved, default off, experimental; Homard 26 Oct: "the sidearm model should always be visible on the right
// hip (like with the sling)"; needs the sling on). The game holds the weapon the hand last grabbed: the main weapon (the sling's) or the
// sidearm (the hip's). Not in the hand, the game's weapon hangs where it is carried (rest_pose: the sling for a main weapon, the hip for
// a sidearm) and the mod draws its recorded copy of the other one in its own place (FarCry5Holster.cpp). Grabbing the one the game does
// not hold taps the game's "last weapon" (pad Y, or a keyboard key with sidearmkey N) once; until the game shows another weapon (or
// 2.5 s) the switch is pending: the game's weapon stays where it is carried, the copy of the wanted one is in the hand, and it does not
// fire. hip_pose: the sidearm's place, tracking space, from the head pose (x right, y up, -z along the barrel, origin = its grip);
// sidearmpos right down forward (m), sidearmrot left down roll (deg), as slingpos / slingrot.
glm::mat4 hip_pose(const glm::mat4& head, bool mirror);
glm::mat4 rest_pose(const glm::mat4& head, bool mirror);   // where the game's weapon hangs when not in the hand
bool      side_on();          // sidearm and sling both on
bool      cur_side();         // the game's weapon at the last poll is a sidearm
int       switch_want();      // a pending switch: 0 to the main weapon, 1 to the sidearm; -1 none
bool      switch_grabbed();   // that switch is a sling or hip grab's (the copy in the hand), not the look at the sidearm slot after a load
void      get_hip_place(float pos[3], float rot[3]);
void      set_hip_place(const float pos[3], const float rot[3]);
void      reset_hip_place();
int       side_key();         // sidearmkey: -1 the weapon wheel's rightmost slot (default), 0 pad Y, else a keyboard scan code
bool      side_peek_on();     // sidearmpeek on|off (saved, default on): look at the sidearm slot once after a load
int       side_peek_state();  // the load popup: 2 looking at the sidearm slot, 1 looked since the load, 0 not yet, -1 no look (sidearm or sidearmpeek off)
int       load_slot_state();  // the load popup: 1 loadslot will take the left shoulder's slot, 2 that pick is under way, 0 nothing to do
bool      side_y_on();        // sidearmy on|off (saved, default on): the hip switches with pad Y when the game's last weapon is the sidearm
bool      wheel_pick_now();   // the mod's wheel pick is under way (and 0.2 s after): the game's HUD (the wheel) is not drawn then
void      side_peek_again();  // the sidearm slot to be looked at again (a wrong sidearm forgotten)
uint32_t  take_side_landed();   // once: the weapon a hip grab switched to (0 none): the sidearm, unless measured otherwise

// The weapon-switch zone at the weapon hand's shoulder (gestureshoulder side down back, m, saved, default 0.18 0.25 0.13): from the
// eyes when looking straight ahead, to the side, below them, behind them. It hangs from the neck and turns with the body's heading
// (like the sling), not with the head's yaw.
void get_shoulder_place(float pos[3]);
void set_shoulder_place(const float pos[3]);
void reset_shoulder();

// For the F11 Controls tab.
int         count();
const char* label(int i);
const char* hint(int i);
const char* name(int i);
bool        enabled(int i);
void        set_enabled(int i, bool on);
float       radius(int i);
void        set_radius(int i, float m);
float       default_radius(int i);   // the zone size before any <name>size command (m)
float       last_distance(int i);   // the gesture's hand to the zone centre at the last poll (m), < 0 untracked

}
