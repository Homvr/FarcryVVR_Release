// Walk direction (Homard 22 Oct: "going forward follow the headset rather than the weapon (currently it follows the headset with
// no weapon, and the weapon when weapon is held)"). Ported from the Motion V2 branch (research/motion-v2, FarCry5MotionV2.cpp and
// MotionV2/src/Locomotion.cpp; that branch was dropped on 22 Oct, these three switches were never headset-tested there).
//
// The left stick goes to the game as pushed, and the game walks along its aim. Unarmed (or slung) the aim loop keeps the aim on
// the head, so walking followed the head; with a weapon in hand fireready keeps the aim on the barrel (firesplit turns only the
// drawn camera back to the head), so walking followed the gun.
//  - walkbasis game|head|hand (default head): the left stick is turned by (basis yaw - the game's walk yaw) before it goes to the
//    pad, so forward is where the head looks (or where the off controller points), whatever the gun does. The length is kept, so
//    the walk / run thresholds stay. On foot only.
//  - The game walks along its aim (the camera minus the Extended View offset it carries), and its walk yaw includes the mouse counts
//    in flight (walk_set_rot). Were walkref aim|camera and walklead on|off, removed 24 Oct (Homard: only the defaults).
//  - sprinthead (always, was a switch until 24 Oct): the game also sprints along its aim. While sprinting, fireready lets the aim follow the head
//    (the gun is lowered while sprinting anyway); a shot reaching the game, aiming down sights or the stick eased under 70 % ends
//    the sprint and the aim goes back to the barrel. The sprint is gunstill's (the left stick pressed since the player was last
//    slower than 1 m/s), so it needs gunstill (on by default).
#include "FarCry5Stereo.h"

#include <glm/gtc/matrix_access.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace
{
    float       wrap(float a) { return std::remainder(a, glm::two_pi<float>()); }
    float       yaw_of(const glm::vec3& f) { return std::atan2(-f.x, f.y); }   // as FarCry5Stereo.cpp: 0 = +y, counter-clockwise
    const char* basis_name(int b) { return b == 1 ? "head" : b == 2 ? "hand" : "game"; }
    // A direction more than ~14 deg off vertical has a meaningful heading (the head looking at the feet, a hand hanging down has none).
    bool        has_heading(const glm::vec3& f)
    {
        const float len = glm::length(f);
        return len > 1e-6f && std::sqrt(f.x * f.x + f.y * f.y) / len > 0.25f;
    }
}

bool FarCry5Stereo::walk_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    if (cmd == "walkbasis") {   // walkbasis game|head|hand (saved)
        m_walk_basis = arg == "head" ? 1 : arg == "hand" ? 2 : 0;
        persist      = true;
        return true;
    }
    if (cmd == "walklog") {   // walklog N (test): the player's camera, the yaws and the stick, for the simulator checks
        m_walk_log = arg.empty() ? 600 : std::max(0, std::atoi(arg.c_str()));
        return true;
    }
    if (cmd == "walklogevery") { m_walk_log_every = std::max(1, std::atoi(arg.c_str())); return true; }   // walklogevery K (test): one walklog line per K presents (6)
    if (cmd == "walkstatus") {
        spdlog::info("[Walk] walkbasis {}: turn now {:.1f} deg, {}; sprinting {}; left stick {:.2f}", basis_name(m_walk_basis.load()), glm::degrees(m_walk_rot.load()),
                     m_walk_rot_ok.load() ? "applied" : "not applied now", m_sh_sprinting, m_lstick_mag.load());
        return true;
    }
    return false;
}

// Once per present, before the aim loop (motion_post_present): the walk turn, and sprinthead's sprint.
void FarCry5Stereo::walk_present()
{
    const int basis = m_walk_basis.load();
    bool      ok    = false;
    if (basis != 0 && m_mo.valid && m_mo.frame == m_frame.load() && m_player_views_last_frame != 0 && !m_menu_screen_on && !m_in_vehicle.load() && !m_cine_on) {
        // While a basis points straight up or down it has no heading: its last heading is kept relative to the body, so it turns with
        // snap and smooth turns (a world yaw held from before would point elsewhere after one). Each basis keeps its own; the off hand
        // uses its held heading while it is not tracked, and the head's until it has given one.
        if (basis != m_walk_rel_basis) {
            m_walk_rel_basis = basis;
            m_walk_rel_ok[1] = m_walk_rel_ok[2] = false;
        }
        if (has_heading(m_mo.head_world)) {
            m_walk_rel[1]    = wrap(yaw_of(m_mo.head_world) - m_body_yaw);
            m_walk_rel_ok[1] = true;
        }
        if (basis == 2 && m_mo.other_valid) {
            const glm::vec3 hf = m_mo.other_rot_world * glm::vec3{ 0.0f, 0.0f, -1.0f };
            if (has_heading(hf)) {
                m_walk_rel[2]    = wrap(yaw_of(hf) - m_body_yaw);
                m_walk_rel_ok[2] = true;
            }
        }
        const int src = (basis == 2 && m_walk_rel_ok[2]) ? 2 : 1;
        if (m_walk_rel_ok[src]) {   // none yet (pointing down since walkbasis was picked): the game's own walk direction
            m_walk_basis_yaw = m_walk_rel[src] + m_body_yaw;
            ok               = true;
        }
    }
    m_walk_basis_ok = ok;
    if (ok) walk_set_rot();
    if (ok != m_walk_rot_ok.exchange(ok) && basis != 0)
        spdlog::info("[Walk] walkbasis {}: {}", basis_name(basis),
                     ok ? fmt::format("the left stick walks along the {} (now {:.1f} deg from the game's walk direction)", basis == 1 ? "head" : "off hand", glm::degrees(m_walk_rot.load()))
                        : std::string{ "not applied (menu, vehicle, cinematic, no player camera, or no heading yet)" });
    {
        // For the simulator checks: each time the turn goes past 20 deg (the gun or the off hand well to the side) or back under 10.
        const float a    = std::abs(glm::degrees(m_walk_rot.load()));
        const bool  wide = ok && (m_walk_wide ? a > 10.0f : a > 20.0f);
        const auto  now  = std::chrono::steady_clock::now();
        if (wide != m_walk_wide && now - m_walk_wide_t > std::chrono::milliseconds(500)) {
            m_walk_wide   = wide;
            m_walk_wide_t = now;
            spdlog::info("[Walk] turn {:+.1f} deg ({}), left stick {:.2f}", glm::degrees(m_walk_rot.load()), wide ? "the basis well off the game's walk direction" : "back near it",
                         m_lstick_mag.load());
        }
    }
    sprint_update();
    if (m_walk_log.load() > 0 && m_mo.valid) {
        // walklog: yaws in deg (0 = +y, counter-clockwise), the game camera in m; the script measures where the camera goes.
        const int n = m_walk_log.fetch_sub(1);
        if (n % m_walk_log_every.load() == 0) {
            const glm::vec3 cam_fwd = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
            const glm::vec3 gun     = m_mo.ctrl_rot_world * glm::vec3{ 0.0f, 0.0f, -1.0f };
            spdlog::info("[Walk] log frame {} cam {:.4f} {:.4f} {:.4f} | yaw head {:.1f} gun {:.1f} game {:.1f} ev {:.1f} body {:.1f} | turn {:.1f} {} | stick in {:.2f} {:.2f} out {:.2f} {:.2f} | "
                         "sprint {} ready {} unarmed {} vehicle {}",
                         m_frame.load(), m_mo.cam.x, m_mo.cam.y, m_mo.cam.z, glm::degrees(yaw_of(m_mo.head_world)), glm::degrees(yaw_of(gun)), glm::degrees(yaw_of(cam_fwd)),
                         m_mo.ev_ok ? glm::degrees(m_mo.ev_oy) : 0.0f, glm::degrees(m_body_yaw), glm::degrees(m_walk_rot.load()), m_walk_rot_ok.load() ? "on" : "off",
                         m_walk_stick[0].load(), m_walk_stick[1].load(), m_walk_stick[2].load(), m_walk_stick[3].load(), m_sh_sprinting, m_fire_ready_was, m_unarmed,
                         m_in_vehicle.load());
        }
    }
}

// The turn: basis yaw - the game's walk yaw at the update that reads the stick. The camera read here is a few game updates old,
// and the mouse counts sent since (the aim loop following the gun or the head) land in the same update as the next stick read:
// against the camera alone, turning the gun while walking bent the walk for a moment the other way, then it came back (Homard
// 23 Oct 06:53: "a small change in direction that was then immediately corrected"). So the counts in flight are added, as
// firemeasure does for the aim. Called once per present, and again right after the aim loop's send (walk_after_send).
void FarCry5Stereo::walk_set_rot()
{
    if (!m_walk_basis_ok) return;
    const glm::vec3 cam_fwd  = -glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) };
    float           game_yaw = yaw_of(cam_fwd);
    if (m_mo.ev_ok) game_yaw -= m_mo.ev_oy;   // the aim: the camera minus the Extended View offset it carries
    {
        int fx = 0, fy = 0;   // the mouse counts in flight
        inflight(m_mo.ev_seq, fx, fy);
        game_yaw += glm::radians(-float(fx) * m_deg_per_count);
    }
    m_walk_rot = wrap(m_walk_basis_yaw - game_yaw);
}

void FarCry5Stereo::walk_after_send() { walk_set_rot(); }

// sprinthead: the game's sprint as far as the mod can tell. gunstill's sprint (the left stick pressed since the player was last
// slower than 1 m/s) stays set while the player keeps running after the game has ended its sprint, which it does on a shot,
// on aiming down sights, or when the stick is eased off. Each ends it here too (a shot is the trigger let through to the game: a
// press the aim loop still holds back has not reached it), until the next stick press. With the stick held pressed the press
// stamp is renewed on every read, as the game's sprint signal is held: the sprint
// counts again once the shot or the aim is over.
void FarCry5Stereo::sprint_update()
{
    const int64_t press = m_pad_sprint_press.load();
    bool          s     = m_gun_still.load() && player_sprinting() && press > m_sh_end;
    const char*   why   = nullptr;   // what ended it (for the log)
    if (s) why = m_fire_open ? "a shot" : m_lt_sent.load() ? "aiming down sights" : m_lstick_mag.load() < 0.7f ? "the stick eased off" : nullptr;
    if (why) {
        m_sh_end = press;   // ended: only a later press starts the next sprint
        s        = false;
    }
    if (s != m_sh_sprinting) {
        const bool in_hand = !m_mo.slung && !m_unarmed;   // slung or empty-handed the aim follows the head anyway
        spdlog::info("[Walk] sprinthead: {}", s ? (in_hand ? std::string{ "sprinting, the game's aim follows the head" } : std::string{ "sprinting (no weapon in hand)" })
                                                : fmt::format("sprint over ({}){}", why ? why : "slower than 1 m/s", in_hand ? ", the aim back on the barrel" : ""));
    }
    m_sh_sprinting = s;
}

// The left stick, after the D-pad handling in the XInput hook (game input thread).
void FarCry5Stereo::walk_left_stick(int16_t& lx, int16_t& ly)
{
    m_lstick_mag = std::min(1.0f, std::hypot(float(lx), float(ly)) / 32767.0f);   // sprinthead: the stick eased off ends a sprint
    m_walk_stick[0] = m_walk_stick[2] = float(lx) / 32767.0f;
    m_walk_stick[1] = m_walk_stick[3] = float(ly) / 32767.0f;
    if (!m_motion.load() || m_menu_screen_on || m_in_vehicle.load()) return;       // on foot with motion controls only
    if (m_walk_basis.load() == 0 || !m_walk_rot_ok.load() || (lx == 0 && ly == 0)) return;
    // The basis turned d to the left of the game's walk direction: its forward is (-sin d, cos d) and its right (cos d, sin d) in
    // the game's (x right, y forward) stick frame.
    const float d = m_walk_rot.load(), c = std::cos(d), n = std::sin(d);
    const float x = float(lx) / 32767.0f, y = float(ly) / 32767.0f;
    lx            = int16_t(std::clamp((x * c - y * n) * 32767.0f, -32767.0f, 32767.0f));
    ly            = int16_t(std::clamp((x * n + y * c) * 32767.0f, -32767.0f, 32767.0f));
    m_walk_stick[2] = float(lx) / 32767.0f;
    m_walk_stick[3] = float(ly) / 32767.0f;
}

// F11 Motion control tab: the Walking section.
void FarCry5Stereo::draw_walk_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                   const std::function<void(const char*)>& section)
{
    section("Walking");
    {
        int         wb = m_walk_basis.load();
        const char* kBasis[]{ "Where the game aims (as before 22 Oct)", "Where you look", "Where your off hand points" };
        if (ImGui::Combo("Walk direction", &wb, kBasis, 3)) {
            m_walk_basis = wb;
            keep("walkbasis", fmt::format("walkbasis {}", basis_name(wb)));
        }
        hint("Where pushing the left stick forward takes you. Where you look (default): the stick is turned so forward is where the headset faces, "
             "also with a weapon in hand; walking stays straight while the gun turns, and while sprinting the game's aim follows your head (a shot, "
             "aiming down sights or easing off the stick ends the sprint and the aim goes back to the gun). Where the game aims: the old way, forward "
             "followed the gun while a weapon was in hand.");
    }
}
