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
};

struct Output
{
    uint16_t buttons{ 0 };            // XINPUT_GAMEPAD_* bits to add this poll
    bool     grip_taken[2]{};         // that hand's grip belongs to a gesture: no usual grip action
    float    buzz[2]{};               // haptic pulse on that hand this poll: amplitude (0 = none) ...
    float    buzz_s[2]{};             // ... and length (s): light on entering a zone, firmer when a gesture fires
    bool     slung{ false };          // the weapon hangs on the sling (not in the hand): it must not fire
    bool     no_fire{ false };        // a throw is under way: the right trigger must not fire
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
bool test_input();   // a gesture test drives the controllers (gesturegrip, throwtest): counts as controller use
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
