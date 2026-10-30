// shoulderdraw (Homard 26 Oct: "when gripping at the shoulder gesture, the weapon should appear in the hand as if grabbed from
// the shoulder"; F11 Experimental > Weapon switch (test), default off, saved).
//
// The shoulder gesture taps pad Y and the game switches weapon with its own animations: the old weapon is lowered out of view
// (holster, about 0.5 s in Homard's 25 Oct logs), the weapon id passes through a part for 0.1-0.2 s, then the new weapon is
// raised (draw). gunstill let go of the gun for both (the gun far from its held pose, and the new weapon held only once it
// came within 0.6 m of it), so both animations showed on the controller.
// With shoulderdraw a weapon-switch gesture opens a window:
//  - Phase 1 (hiding), from the grip: every weapon-pass draw but the gloves is hidden (its pose is still measured, and the
//    weapon id still counted), and the right glove is an empty hand on its controller (emptyhandstill's bones, emptyhandalign's
//    palm). It ends when the frame's weapon id is a new one whose held pose gunstill knows (fc5vr_gunposes.txt).
//  - Phase 2 (holding), shoulderdrawms from the first frame of the new weapon: gunstill holds it at its held pose at once and
//    fully, however far the game's draw animation has it, and no reload is read from that animation.
//  - Ends early: no new weapon id within shoulderdrawwait (the game did not switch: only one weapon, a menu...), the new weapon's
//    pose not known 1.2 s after the id changed (a weapon never held standing: the end of the game's draw shows, gunstill learns
//    it), 3 s in all, a vehicle, a menu, motion controls or gunstill off. It does not open with the weapon left on the sling.
//  - With the sling on (Homard's setup), the switch grip also takes the weapon off the sling while it lasts (set_switch_grab):
//    without it the game switched the weapon on the sling (the shoulder grip let go of the gun).
//  - With the switch off the window still opens (watched and logged only), for comparing.
//  - shoulderinstant (default on; Homard 27 Oct: "it is a bit too slow. Can you make it instant"): the game takes 0.6 s (1.2 s from the
//    sling) to bring the new weapon, an empty hand meanwhile. Now the weapon the switch should bring is guessed at the grip (what a switch
//    from the weapon held brought last time, else the latest other weapon held) and its recorded copy (FarCry5Holster.cpp, made once a
//    weapon is held still for a second) is drawn in the hand from the next frame; the game's own weapon replaces it, held at the same pose,
//    when it is out (phase 2). A wrong guess shows until then and is learned; it cannot fire before the game's weapon is out.
// Simulator 26 Oct (testruns/2026-10-26_shoulderdraw): the new weapon drawn 0.57-0.58 s after the grip (1.08-1.23 s for the first
// switch after the gun had hung on the sling), held from its first shown frame, 0.0 cm / 0.0 deg from its held pose.
// The holster thread (FarCry5Holster.cpp) draws its recorded copies during phase 1 (weapon_switch_hiding()).
#include "FarCry5Stereo.h"
#include "FarCry5Gesture.h"
#include "FarCry5SwitchSpeed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <istream>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace
{
    using clock = std::chrono::steady_clock;
    float secs(clock::time_point a, clock::time_point b) { return std::chrono::duration<float>(b - a).count(); }
    // shoulderdrawsnap a b (test): in the next window, a pair of eye pictures (snappair) a ms after the grip and b ms after the new
    // weapon shows (-1: none).
    std::atomic<int> g_snap_a{ -1 }, g_snap_b{ -1 };
}

struct FarCry5Stereo::WeaponSwitchState
{
    uint32_t          seen{ 0 };       // weapon_switch_count() last read
    bool              seen_init{ false };
    int               phase{ 0 };      // 0 idle, 1 hiding, 2 holding
    uint32_t          old_sig{ 0 };    // the weapon in hand at the grip (0: none)
    uint32_t          target{ 0 };     // the new weapon (phase 2)
    bool              changed{ false };   // the frame's weapon id left the old one
    bool              side{ false };      // a sidearm gesture's switch (FarCry5Holster.cpp): the old weapon hangs where it is carried meanwhile
    clock::time_point t0{}, change_t{}, shown_t{};
    uint64_t          hidden{ 0 };     // weapon-pass draws hidden in this window
    int               frames{ 0 };     // frames in phase 1
    uint32_t          windows{ 0 };
    bool              act{ false };    // shoulderdraw on when the window opened (off: the window is only watched and logged, for comparing)
    // shoulderinstant: which weapon the switch brings, guessed at the grip so its recorded copy (FarCry5Holster.cpp) is in the hand at once.
    uint32_t          guess{ 0 };
    std::vector<uint32_t> recent;      // weapons held (a known held pose, 0.5 s on the same id), the latest first
    uint32_t          cand{ 0 };       // the id held now, not yet 0.5 s
    clock::time_point cand_t{};
    std::unordered_map<uint32_t, uint32_t> next;   // what a switch from that weapon brought last time
    uint32_t          guesses{ 0 }, right{ 0 };
    uint32_t          drops{ 0 };      // fc5::gesture::shoulder_pick_drops() when the window opened
};

bool FarCry5Stereo::weapon_switch_hiding() const { return m_wsw && m_wsw->act && m_wsw->phase == 1; }
uint32_t FarCry5Stereo::weapon_switch_target() const { return m_wsw && m_wsw->act && m_wsw->phase == 2 ? m_wsw->target : 0u; }
bool FarCry5Stereo::weapon_switch_holds(uint32_t sig) const { return m_wsw && m_wsw->act && m_wsw->phase == 2 && sig != 0 && sig == m_wsw->target; }
void FarCry5Stereo::weapon_switch_note_hidden() { if (m_wsw) m_wsw->hidden++; }
// gunstill: a weapon the switch in phase 1 brought (not the old one) with no held pose yet: its pose is taken once the game's draw settles.
bool FarCry5Stereo::weapon_switch_needs_pose(uint32_t sig) const
{
    return m_wsw && m_wsw->act && m_wsw->phase == 1 && m_wsw->changed && sig != 0 && sig != m_wsw->old_sig;
}
// shoulderinstant: in phase 1 of a shoulder switch (not a sidearm one, whose copy the holster picks itself), the weapon it is guessed to bring.
uint32_t FarCry5Stereo::weapon_switch_guess() const
{
    return m_wsw && m_wsw->act && m_wsw->phase == 1 && !m_wsw->side && m_sh_instant.load() ? m_wsw->guess : 0u;
}

void FarCry5Stereo::weapon_switch_frame(uint32_t frame_sig)
{
    if (!m_wsw) m_wsw = new WeaponSwitchState{};
    auto&          w   = *m_wsw;
    const auto     now = clock::now();
    const uint32_t n   = fc5::gesture::weapon_switch_count();
    if (!w.seen_init) {
        w.seen_init = true;
        w.seen      = n;
    }
    // The window opens with the switch off too (nothing hidden or held), so its [Switch] lines and pictures compare the two.
    const bool on = m_motion.load() && m_gun_still.load() && !m_in_vehicle.load() && !m_menu_screen_on;
    auto end = [&](const char* why) {
        spdlog::info("[Switch] shoulderdraw{}: window over after {:.2f} s ({}): weapon {} -> {}, {} frames hidden ({} weapon-pass draws), {}", w.act ? "" : " (off, watched only)", secs(w.t0, now), why, w.old_sig,
                     w.target != 0 ? w.target : frame_sig, w.frames, w.hidden,
                     w.phase == 2 ? fmt::format("new weapon held from {:.2f} s", secs(w.t0, w.shown_t)) : std::string{ "the new weapon was not held" });
        w.phase = 0;
    };
    // shoulderinstant: the weapons held, outside switches (an id the switch shows for a moment is no weapon held).
    {
        glm::mat4 held{ 1.0f };
        const uint32_t s = (w.phase == 0 && frame_sig != 0 && !m_unarmed && gun_still_held(frame_sig, held)) ? frame_sig : 0u;
        if (s != w.cand) {
            w.cand   = s;
            w.cand_t = now;
        } else if (s != 0 && secs(w.cand_t, now) > 0.5f && (w.recent.empty() || w.recent.front() != s)) {
            std::erase(w.recent, s);
            w.recent.insert(w.recent.begin(), s);
            if (w.recent.size() > 8) w.recent.pop_back();
        }
    }
    if (n != w.seen) {
        w.seen = n;
        // The weapon in the hand after the gesture (with the sling on, shoulderdraw makes the switch grip hold it: set_switch_grab).
        // sidearm: a sling or hip grab switches while the game's weapon still hangs where it is carried (slung).
        const bool side = fc5::gesture::switch_want() >= 0;
        if (on && (!fc5::gesture::slung() || side)) {
            if (w.phase != 0) end("another switch gesture");
            w.side    = side;
            w.phase   = 1;
            w.old_sig = m_unarmed ? 0u : m_weapon_sig.load();
            w.target  = 0;
            w.changed = false;
            w.t0      = now;
            w.hidden  = 0;
            w.frames  = 0;
            w.windows++;
            w.act     = m_sh_draw.load();
            w.drops   = fc5::gesture::shoulder_pick_drops();
            // The game's Y brings its previous weapon: what a switch from this weapon brought last time, else the latest other one held
            // (with no weapon out: the latest held).
            // shoulderwheel: the weapon the wheel spot being picked gave before, when known; never the latest held otherwise (it may be the
            // sidearm, which the shoulder never brings).
            w.guess = fc5::gesture::shoulder_pick_target();
            if (w.guess == 0) {
                if (auto it = w.next.find(w.old_sig); it != w.next.end()) w.guess = it->second;
                else if (side || !fc5::gesture::shoulder_wheel_on())
                    for (uint32_t r : w.recent)
                        if (r != w.old_sig) { w.guess = r; break; }
            }
            // Never the sidearm in the hand for a shoulder switch (Homard 29 Oct: "at some point it was picking up the sidearm but then remove it"),
            // nor the weapon being put away.
            if (!side && fc5::gesture::shoulder_wheel_on() && (w.guess == w.old_sig || holster_is_side(w.guess))) w.guess = 0;
            const bool copy = w.act && !side && m_sh_instant.load() && w.guess != 0 && holster_has_copy(w.guess);
            spdlog::info("[Switch] shoulderdraw: switch gesture: weapon {} {}{}", w.old_sig,
                         w.act ? "put away (hidden)" : "(shoulderdraw off: watched only)",
                         !w.act || side ? std::string{} : copy ? fmt::format(", weapon {}'s recorded copy in the hand at once (shoulderinstant; guessed)", w.guess)
                                                         : fmt::format(", the right hand empty until the new weapon is drawn ({})",
                                                                       !m_sh_instant.load() ? "shoulderinstant off" : w.guess == 0 ? "no weapon to guess yet" : fmt::format("weapon {} not recorded yet", w.guess)));
        }
    }
    if (w.phase == 0) return;
    // shoulderdrawsnap (test): the pictures at their times in this window.
    auto snap = [&](std::atomic<int>& ms, clock::time_point from, const char* what) {
        const int v = ms.load();
        if (v < 0 || secs(from, now) * 1000.0f < float(v)) return;
        ms         = -1;
        m_snap_pair = 2;
        spdlog::info("[Switch] shoulderdrawsnap: eye pictures {} ms after {} (present {})", v, what, m_presents);
    };
    snap(g_snap_a, w.t0, "the grip");
    if (w.phase == 2) snap(g_snap_b, w.shown_t, "the new weapon showed");
    if (!on) { end("motion or gunstill off, a vehicle or a menu"); return; }
    if (w.phase == 1) {
        w.frames++;
        const float t = secs(w.t0, now);
        if (frame_sig != w.old_sig && !w.changed) {
            w.changed  = true;
            w.change_t = now;
        }
        glm::mat4 held{ 1.0f };
        if (frame_sig != 0 && frame_sig != w.old_sig && gun_still_held(frame_sig, held)) {
            w.phase   = 2;
            w.target  = frame_sig;
            w.shown_t = now;
            spdlog::info("[Switch] shoulderdraw: weapon {} drawn {:.2f} s after the grip ({:.2f} s after the id changed){}", frame_sig, t,
                         secs(w.change_t, now), w.act ? fmt::format(": shown held in the hand for {} ms", m_sh_draw_ms.load()) : std::string{});
            fc5::switchspeed::note_switch(t);   // switchfast: a slow switch asks for a rescan
            // shoulderinstant: the guess checked, and learned for the next switch from that weapon.
            if (!w.side) {
                if (w.guess != 0) {
                    w.guesses++;
                    if (w.guess == frame_sig) w.right++;
                    spdlog::info("[Switch] shoulderinstant: guessed weapon {}, the game brought {}: {} ({} of {} right this session)", w.guess, frame_sig,
                                 w.guess == frame_sig ? "right" : "wrong, the copy swapped for it now", w.right, w.guesses);
                }
                w.next[w.old_sig] = frame_sig;
            }
            return;
        }
        if (!w.changed && fc5::gesture::shoulder_pick_drops() != w.drops) end("the wheel slot gave no other weapon: the weapon held is back");
        else if (!w.changed && t * 1000.0f > float(m_sh_draw_wait.load())) end("the game did not switch weapon");
        else if (w.changed && secs(w.change_t, now) > 1.2f) end("the new weapon's held pose is not known yet: the end of the game's draw shown");
        else if (t > 3.0f) end("no new weapon in 3 s");
        return;
    }
    if (secs(w.shown_t, now) * 1000.0f > float(m_sh_draw_ms.load())) end("held to the end");
}

bool FarCry5Stereo::weapon_switch_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "shoulderdraw") {
        m_sh_draw = on;
        fc5::gesture::set_switch_grab(on);
        persist   = true;
        spdlog::info("[Switch] shoulderdraw {}: {}", on ? "on" : "off",
                     on ? "a weapon-switch gesture hides the old weapon and shows the new one held in the hand (no holster or draw animation)" : "the game's switch animations");
        return true;
    }
    if (cmd == "shoulderinstant") { m_sh_instant = on; persist = true; return true; }
    if (cmd == "shoulderdrawms") { m_sh_draw_ms = std::clamp(int(fv), 0, 5000); persist = true; return true; }
    if (cmd == "shoulderdrawwait") { m_sh_draw_wait = std::clamp(int(fv), 200, 3000); persist = true; return true; }
    if (cmd == "shoulderdrawsnap") {   // test: shoulderdrawsnap a b (ms after the grip, ms after the new weapon shows; -1 none)
        g_snap_a = arg.empty() ? 250 : int(fv);
        int b = 200;
        rest >> b;
        g_snap_b = b;
        return true;
    }
    if (cmd == "shoulderdrawtest") {   // test (simulator): as the shoulder gesture does, the window opened and pad Y tapped
        fc5::gesture::note_weapon_switch();
        m_ui_cmds += "padtap 8000 120\n";
        return true;
    }
    return false;
}

// F11 Gestures > Right shoulder: switch weapon (experimental): the weapon taken from the shoulder. The faster switch (switchfast)
// stays in Experimental > Weapon switch (test).
void FarCry5Stereo::draw_weapon_switch_menu(const std::function<void(const char*)>& hint, const std::function<void(const char*)>& section)
{
    section("Weapon taken from the shoulder");
    bool v = m_sh_draw.load();
    if (ImGui::Checkbox("Weapon taken from the shoulder: in the hand at once, no draw animation##shoulderdraw", &v)) m_ui_cmds += v ? "shoulderdraw on\n" : "shoulderdraw off\n";
    hint("Off by default. On: when you grip at your shoulder to switch weapon, the weapon you held disappears and the new one shows in your "
         "hand where you hold it, without the game's draw animation. With the sling on, the new weapon stays in your hand while you keep the "
         "grip pressed and goes on the sling when you let go. A weapon the mod has never seen you hold standing still shows the end of the game's "
         "animation the first time.");
    if (!v) return;
    bool in = m_sh_instant.load();
    if (ImGui::Checkbox("At once: the new weapon in the hand the moment you grip##shoulderinstant", &in)) m_ui_cmds += in ? "shoulderinstant on\n" : "shoulderinstant off\n";
    hint("On (default): the game needs about half a second to take the new weapon out; meanwhile the mod draws its own copy of it in your "
         "hand (a weapon you have held still for a second this session), and the game's weapon takes its place when it is out. It cannot fire "
         "before then. The copy is the weapon the switch brought last time from the one you held, else the one you held before. Off: your hand "
         "is empty until the game's weapon is out.");
}
