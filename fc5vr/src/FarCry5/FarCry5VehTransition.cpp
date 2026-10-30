// Getting in and out of vehicles, comfort (Homard 23 Oct, after the 12:04 headset run: "the camera movements were very
// unpleasant in a lot of moments, especially when transitioning between vehicles"). His log: each entry turned the view
// ~40 deg/s for 2 s, each exit added a seat-change settle and recentre 0.6 s before the real one, and bailing out of the
// plane turned the view ~180 deg in half a second. With vehentryfollow the view's heading was held on the game camera's
// during the animation (m_body_yaw_pending = camera - (body + head)): turning the head did not turn the view for 1-3 s.
//  - vehfollowhead on|off (default on, saved; F11 Comfort > Cinematics and vehicles): the body frame turns by the animation
//    camera's own turn since the settle started; the head still turns the view on top. Getting out, the exit ends with the
//    centre moved under the head (no turn of the view: on foot the aim loop brings the game camera to the head).
//  - vehfollowsnap on|off (default on, saved): that turn in snaps of the snap-turn angle (15-45 deg, at most one per 0.25 s),
//    like a snap turn, instead of a smooth artificial turn; the eyes stay within that angle of the camera the game culls with.
//  - vehseatguard on|off (default on, saved): a seat-change settle is not finished while no vehicle is under the camera
//    (update_vehicle); getting out has one settle, not two.
//  - vehseathold on|off (default on, saved; F11 Comfort > Cinematics and vehicles; Homard 28 Oct 22:17: "I was in a boat and could
//    not accelerate"). His 22:15 log: in the boat the mod went from "in a vehicle" to "on foot again" every ~4 s for a minute. The
//    boat's model is 3 m from the driver's camera, outside vehicle_probe's window (0.8 m aside, 0.3-2.2 m below), so only the hands'
//    pass told in from out; the game drew the sidearm (9282, 24417) ~10 ms after each settle and put it away 3 s later (4110), and
//    each time the mod went on foot: the right trigger held for the aim and cut by the sling, so no throttle. Now, when a settle
//    ends, the seat is learnt on the vehicle's model (the Vehicle_ draw within 8 m, the camera in its axes); while the camera stays
//    within vehseatholdm (default 0.6 m, saved) of that seat the player is in that vehicle, and one who settled driving stays driving
//    with a weapon out (the game's vehicle fire is LB; the trigger stays the throttle). A seat change or getting out moves the camera
//    off the seat and goes as before. Off: as before 28 Oct.
//  - vehtriggers left|right (default left, saved; F11 Layout; Homard 29 Oct 10:19: "right grip and right trigger are used for shooting
//    but also for accelerating in some vehicles"; 10:39: "right grip also need to be free because players uses it to grip the weapon in
//    hand ... make left grip brakes instead of right grip"). The game's driver fire is LB (the sidearm drawn and fired, or the vehicle's
//    guns), which the mod put on the left trigger, while the right trigger, the gun hand's, stayed the throttle and the right grip the
//    brake: in his 10:12 drive he pressed B (the game drew the pistol 24417 for 3 s), never the left trigger, and the right trigger kept
//    accelerating. left: driving (any vehicle, planes and helicopters too: RT throttle up / ascend, LT throttle down / descend, LB guns),
//    the left hand drives and the right hand shoots: right trigger LB, left trigger RT, left grip LT, right grip nothing. The left grip
//    stays the modifier for the right stick (D-pad: radio, companions, throwables), right B (RB: throw, rockets) and the menu button
//    (map); a grip press used for one of those stops braking until let go, and the left stick keeps steering while the grip brakes.
//    Passengers and on foot are unchanged. A trigger or left grip held when the layout changes (getting in or out, a seat change) does
//    nothing until it is let go. right: as before 29 Oct.
//  - vehdrivegun on|off (default on, saved; F11 Comfort > Cinematics and vehicles; Homard 30 Oct 09:11 "I was not able to accelerate on the
//    quad"). His right trigger during the quad's entry went to the game as LB, the game drew the driver's sidearm, and the weapon view
//    coming back was taken for a passenger seat (the entry settled "riding", vehseathold learnt it so, the left trigger was LB for 6 s).
//    A weapon view that shows while driving within 1.5 s of that LB (or while held, or after one sent during this vehicle's entry) is the
//    driver's sidearm: latched until the vehicle is left (update_vehicle). Before that rule, the game's own answer (vehscript, Homard's 09:27
//    log read by the engine natives thread: GetPawnVehicleSeat counts from 1 and reads 0 during the entry animation, the driver's seat lookup
//    counts from 0): its driver drives with a weapon out, its passenger rides; seat 0, vehscript off or silent leave it to the LB latch.
//    The game's seat, driver's seat and entity are logged on each vehtriggers change, "now driving / riding" and each change of the rule
//    that decides (game_seat_text). Off: as before.
// Getting in still ends with one cut to the vehicle's front and a recentre (as before); the log line "[Vehicle] settle cut"
// gives its size.
#include "FarCry5Stereo.h"
#include "FarCry5Lua.h"

#include <glm/gtc/constants.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

bool FarCry5Stereo::vehtrans_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "vehfollowhead") { m_veh_follow_head = on; persist = true; return true; }   // vehfollowhead on|off (saved)
    if (cmd == "vehfollowsnap") { m_veh_follow_snap = on; persist = true; return true; }   // vehfollowsnap on|off (saved)
    if (cmd == "vehseatguard") { m_veh_seat_guard = on; persist = true; return true; }     // vehseatguard on|off (saved)
    if (cmd == "exitpitch") { m_exit_pitch = on; persist = true; return true; }            // exitpitch on|off (saved)
    if (cmd == "vehseathold") { m_veh_seat_hold = on; persist = true; return true; }       // vehseathold on|off (saved)
    if (cmd == "vehdrivegun") { m_veh_drive_gun = on; persist = true; return true; }       // vehdrivegun on|off (saved)
    if (cmd == "vehtriggers") {                                                            // vehtriggers left|right (saved)
        if (arg != "left" && arg != "right") return false;
        m_veh_triggers = arg == "left";
        spdlog::info("[Vehicle] vehtriggers {}: driving, {}", arg, arg == "left" ? "the right trigger shoots, the left trigger accelerates, the left grip brakes" : "the right trigger accelerates, the right grip brakes, the left trigger shoots");
        persist = true;
        return true;
    }
    if (cmd == "vehseatholdm") {                                                           // vehseatholdm N (metres, saved)
        try {
            m_veh_seat_hold_m = std::clamp(std::stof(arg), 0.2f, 2.0f);
        } catch (...) {
            return false;
        }
        spdlog::info("[Vehicle] vehseatholdm {:.2f} m", m_veh_seat_hold_m.load());
        persist = true;
        return true;
    }
    return false;
}

// vehtriggers: from the pad hook (VR::on_xinput_get_state) when the driving layout goes on or off.
void FarCry5Stereo::note_veh_triggers(bool swapped, bool right_held, bool left_held)
{
    const int shots = m_vt_shots.exchange(0);
    if (swapped)
        spdlog::info("[Vehicle] vehtriggers: driving, the right trigger shoots (LB), the left trigger accelerates (RT), the left grip brakes (LT){} ({})",
                     right_held || left_held ? "; a trigger held now waits until let go" : "", game_seat_text());
    else
        spdlog::info("[Vehicle] vehtriggers: the triggers back to on foot / passenger (right fires, left LB); {} shots from the right trigger while driving{} ({})", shots,
                     right_held || left_held ? "; a trigger held now waits until let go" : "", game_seat_text());
}

// vehdrivegun (log only): the game's own seat values beside the mod's driving / riding (GetPawnVehicleSeat and the driver's seat lookup
// are under question: on the quad, 30 Oct 09:11, the seat read 1 from 40.56 to the exit while he drove).
std::string FarCry5Stereo::game_seat_text() const
{
    int      seat = -1, dseat = -1;
    uint64_t id   = 0;
    fc5::lua::vehicle_seat(seat, dseat, id);
    if (id == 0) return "the game: no vehicle answer";
    return fmt::format("the game: seat {}, driver's seat {}, entity {:x}", seat, dseat, id);
}

// A settle starts (getting in, a seat change, getting out): the follow takes its start at its first step.
void FarCry5Stereo::veh_follow_start()
{
    m_vf_init      = false;
    m_vf_turn      = 0.0f;
    m_vf_snaps     = 0;
    m_vf_seat_held = 0;
}

// vehseathold: from vehicle_probe, each Vehicle_ draw of the world pass within 8 m of the camera while in a vehicle. Kept per frame:
// the draw that puts the camera nearest the learnt seat, or before it is learnt the largest (the body, not a door or a wheel).
void FarCry5Stereo::veh_seat_probe(int frame, const float* f, UINT count, float dist)
{
    const glm::vec3 x  = glm::normalize(glm::vec3{ f[0], f[4], f[8] });
    const glm::vec3 y0 = glm::vec3{ f[1], f[5], f[9] };
    const glm::vec3 y  = glm::normalize(y0 - glm::dot(y0, x) * x);
    glm::vec3       z  = glm::cross(x, y);
    if (glm::dot(z, glm::vec3{ f[2], f[6], f[10] }) < 0.0f) z = -z;
    const glm::mat3 rot = glm::transpose(glm::mat3{ x, y, z });   // world -> the model's axes
    const glm::vec3 cam = rot * (m_world_cam_pos - glm::vec3{ f[3], f[7], f[11] });
    if (!std::isfinite(cam.x + cam.y + cam.z)) return;
    auto& r = m_vsh_rec[frame & 3];
    if (r.frame == frame) {
        if (m_vsh_valid ? glm::length(cam - m_vsh_seat) >= glm::length(r.cam - m_vsh_seat) : count <= r.count) return;
    }
    r.frame = frame;
    r.count = count;
    r.cam   = cam;
    r.dist  = dist;
}

namespace
{
    // The latest frame's record (the model of a frame is known once its world pass is drawn).
    template <class R> const R* latest_rec(const R (&recs)[4], int frame)
    {
        for (int k = 0; k <= 3; ++k)
            if (recs[(frame - k) & 3].frame == frame - k) return &recs[(frame - k) & 3];
        return nullptr;
    }
}

// vehseathold: a settle ended (getting in, a seat change): the camera's place on the vehicle's model is the seat.
void FarCry5Stereo::veh_seat_learn(int frame)
{
    if (!m_veh_seat_hold.load()) return;
    const VehSeatRec* r = latest_rec(m_vsh_rec, frame);
    if (!r) {
        m_vsh_valid = false;
        spdlog::info("[Vehicle] vehseathold: no vehicle model within 8 m of the camera: no seat learnt (the hands' pass alone tells in from out)");
        return;
    }
    m_vsh_seat      = r->cam;
    m_vsh_valid     = true;
    m_vsh_near      = true;
    m_vsh_driving   = m_veh_driving.load();
    m_vsh_kept_said = false;
    spdlog::info("[Vehicle] vehseathold: the seat learnt on the vehicle's model ({} indices, {:.1f} m from the camera; the camera at ({:.2f}, {:.2f}, {:.2f}) in its axes), {}",
                 r->count, r->dist, r->cam.x, r->cam.y, r->cam.z, m_vsh_driving ? "driving" : "riding");
}

// vehseathold: true while the camera is within vehseatholdm of the learnt seat (update_vehicle, once per frame).
bool FarCry5Stereo::veh_seat_update(int frame)
{
    if (!m_veh_seat_hold.load() || !m_in_vehicle.load() || !m_vsh_valid) {
        m_vsh_near = false;
        return false;
    }
    const VehSeatRec* r    = latest_rec(m_vsh_rec, frame);
    const float       d    = r ? glm::length(r->cam - m_vsh_seat) : -1.0f;
    const bool        in_seat = r && d < m_veh_seat_hold_m.load();
    if (in_seat) m_vsh_off_max = std::max(m_vsh_off_max, d);
    if (in_seat != m_vsh_near) {
        if (r)
            spdlog::info("[Vehicle] vehseathold: the camera {} the seat ({:.2f} m from it)", in_seat ? "back in" : "off", d);
        else
            spdlog::info("[Vehicle] vehseathold: no vehicle model drawn near the camera");
    }
    m_vsh_near = in_seat;
    return in_seat;
}

// One step of the view following the enter / exit animation (vehentryfollow), applied between pairs as m_body_yaw_pending.
void FarCry5Stereo::veh_follow_step(std::chrono::steady_clock::time_point now)
{
    if (!m_veh_follow_head.load()) {
        // As before 23 Oct: the view's heading held on the game camera's.
        m_body_yaw_pending = std::remainder(m_cine_game_yaw - (m_body_yaw + m_head_rel_yaw), glm::two_pi<float>());
        return;
    }
    if (!m_vf_init) {
        m_vf_init     = true;
        m_vf_body0    = m_body_yaw + m_body_yaw_pending;
        m_vf_cam_last = m_cine_game_yaw;
        m_vf_turn     = 0.0f;
        m_vf_snap_t   = now - std::chrono::seconds(1);
    }
    m_vf_turn += std::remainder(m_cine_game_yaw - m_vf_cam_last, glm::two_pi<float>());   // unwrapped: a turn past 180 deg is kept
    m_vf_cam_last = m_cine_game_yaw;
    const float d = std::remainder(m_vf_body0 + m_vf_turn - m_body_yaw, glm::two_pi<float>());
    if (!m_veh_follow_snap.load()) {
        m_body_yaw_pending = d;
        return;
    }
    const float step = glm::radians(std::clamp(m_snap_deg.load(), 15.0f, 45.0f));
    if (std::abs(d) >= step && now - m_vf_snap_t >= std::chrono::milliseconds(250)) {
        m_body_yaw_pending = d;   // the whole way to the animation's heading, in one cut
        m_vf_snap_t        = now;
        m_vf_snaps++;
        request_jump_reset();     // the view jumped: each eye's DLSS history starts over
    }
}

void FarCry5Stereo::draw_vehtrans_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    bool v = m_veh_follow_head.load();
    if (ImGui::Checkbox("Vehicles: getting in or out, the head still turns the view", &v)) {
        m_veh_follow_head = v;
        keep("vehfollowhead", v ? "vehfollowhead on" : "vehfollowhead off");
    }
    hint("On (default): during the enter / exit animation the view turns with the animation's camera and your head still turns it on top; getting out ends without a "
         "turn. Off: as before 23 Oct (the view held on the game's camera: turning the head did not turn the view during the animation, then a recentre).");
    v = m_veh_follow_snap.load();
    if (ImGui::Checkbox("Vehicles: the enter / exit animation's turn in snaps", &v)) {
        m_veh_follow_snap = v;
        keep("vehfollowsnap", v ? "vehfollowsnap on" : "vehfollowsnap off");
    }
    hint("On (default): the animation's turn comes in steps of your snap-turn angle, like a snap turn, instead of the world turning smoothly around you. Off: a smooth turn.");
    v = m_veh_seat_guard.load();
    if (ImGui::Checkbox("Vehicles: getting out, no extra recentre", &v)) {
        m_veh_seat_guard = v;
        keep("vehseatguard", v ? "vehseatguard on" : "vehseatguard off");
    }
    hint("Getting out first looks like a change of seat to the mod. On (default): that is not settled while no vehicle is under you, so getting out has one settle. "
         "Off: as before 23 Oct (a cut and a recentre 0.6 s before the exit's own).");
    v = m_exit_pitch.load();
    if (ImGui::Checkbox("Vehicles: getting out, the game draws where you look up or down", &v)) {
        m_exit_pitch = v;
        keep("exitpitch", v ? "exitpitch on" : "exitpitch off");
    }
    hint("On (default; Homard 29 Oct: getting out of a car while looking down, the lower part of the view was not drawn for a second): during the exit animation the "
         "game's head look tilts the camera the game draws with up or down with your head, as when getting in. Off: as before 29 Oct.");
    v = m_veh_seat_hold.load();
    if (ImGui::Checkbox("Vehicles: stay in the vehicle while in its seat (boats)", &v)) {
        m_veh_seat_hold = v;
        keep("vehseathold", v ? "vehseathold on" : "vehseathold off");
    }
    hint("On (default): the seat is learnt on the vehicle's model once you are in; while you sit in it you stay in the vehicle, and the driver stays the driver when "
         "the game shows a weapon (in a boat the game draws the sidearm every few seconds, and the mod took you for on foot: no throttle). "
         "Off: as before 28 Oct.");
}
