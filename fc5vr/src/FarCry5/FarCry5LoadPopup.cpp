// loadpopup (Homard 30 Oct: when a save loads, a small "VR mod loading" popup in the headset while the controllers are blocked, counting down
// until the mod's load-time work is done; a launcher option skips it, shown by default).
//
// Back in play somewhere else (loadrecenter's test: a save loaded, a respawn, fast travel, or the first play) the popup hangs where the F11 menu
// would (the framework's UI layer, no pointer) and the player's controller input is not sent to the game (ModConfig.cpp: buttons, triggers,
// sticks and grips; the mod's own presses for the look at the sidearm slot and the camera's steering still go). It closes once, at least
// 2 s after the start:
//  - the weapon in hand has been recorded (held still 1 s; nothing to do in bare hands or off foot),
//  - the sidearm has been recorded or the look at the sidearm slot is done (sidearmpeek; nothing to do with it or the sidearm off),
//  - no weapon-switch scan is running or due (switchfast: the scan a load asks for reads GB of memory and slows the game while it runs),
//  - loadslot has nothing left to do (the main weapon taken out after the look when the game holds neither it nor the sidearm, so it hangs on the sling),
// or after 30 s whatever is left (logged). The flat menu screen coming back (a cinematic, another loading screen) drops it, and the next return to
// play starts it again wherever it is (Homard's 30 Oct 15:43 start: 2.2 s of a camera at z 0 pitched -90 deg, then the loading screen again, then play
// 3 m from there, so no new 'somewhere else' and no popup).
// The countdown is an estimate: 2.5 s for the weapon, 7 s for the sidearm, 9 s for loadslot (2 s once its pick is under way), the latest scan's time
// (10 s before the first) for the scan.
#include "FarCry5LoadPopup.h"
#include "FarCry5Gesture.h"
#include "FarCry5SwitchSpeed.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <istream>

#include <imgui.h>
#include <spdlog/spdlog.h>

namespace fc5::loadpopup
{
namespace
{
    using clock = std::chrono::steady_clock;

    constexpr float kMinS = 2.0f, kMaxS = 30.0f;
    enum : int { kWeapon = 1, kSide = 2, kScan = 4, kSlot = 8, kAll = 15 };

    std::atomic<bool>  g_on{ true };
    std::atomic<bool>  g_start_req{ false };
    std::atomic<bool>  g_stop_req{ false };
    std::atomic<bool>  g_again{ false };     // dropped before it was done: shown again at the next return to play
    std::atomic<bool>  g_shown{ false };
    std::atomic<int>   g_done{ 0 };          // kWeapon | kSide | kScan
    std::atomic<float> g_eta{ 0.0f };        // seconds left (estimate)
    std::atomic<float> g_elapsed{ 0.0f };

    // Render thread only.
    clock::time_point g_t0{}, g_armed_t{};
    bool              g_scan_seen = false;   // a scan ran since the start
    float             g_done_at[4]{};        // when each step was done (s since the start; logged)
    int               g_done_was = 0;

    const char* why_left(int done)
    {
        return (done & kScan) == 0 ? "the weapon switch scan" : (done & kSlot) == 0 ? "the main weapon on the sling" : (done & kSide) == 0 ? "the sidearm" : "the weapon in hand";
    }
}

bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    (void)fv;
    (void)rest;
    if (cmd != "loadpopup") return false;
    g_on    = !(arg == "off" || arg == "0" || arg == "false");
    persist = true;
    if (!g_on.load()) {
        g_stop_req = true;
        g_again    = false;
    }
    spdlog::info("[LoadPopup] loadpopup {}: {}", g_on.load() ? "on" : "off",
                 g_on.load() ? "\"VR mod loading\" after a load, the controllers held back until the mod's load-time work is done" : "no popup after a load");
    return true;
}

void start(const char* why)
{
    if (!g_on.load()) return;
    spdlog::info("[LoadPopup] {}: \"VR mod loading\" shown, controller input held back", why);
    g_again     = false;
    g_start_req = true;
}

void resume()
{
    if (g_again.load()) start("back in play after it was dropped");
}

void stop(const char* why)
{
    if (!g_shown.load()) return;
    spdlog::info("[LoadPopup] dropped ({}) after {:.1f} s: shown again at the next return to play", why, g_elapsed.load());
    g_again    = true;
    g_stop_req = true;
}

void frame(const Steps& s)
{
    const auto now = clock::now();
    if (g_stop_req.exchange(false)) {
        g_shown     = false;
        g_start_req = false;
    }
    if (g_start_req.exchange(false)) {
        g_t0        = now;
        g_armed_t   = now;
        g_scan_seen = false;
        g_done_was  = 0;
        g_done      = 0;
        g_elapsed   = 0.0f;
        g_shown     = true;
    }
    if (!g_shown.load()) return;
    const float t = std::chrono::duration<float>(now - g_t0).count();
    if (s.armed) g_armed_t = now;
    const bool bare = !s.armed && now - g_armed_t > std::chrono::milliseconds(1500);   // nothing to record in bare hands
    const int  peek = fc5::gesture::side_peek_state();
    float      scan_s = 0.0f, took = 0.0f;
    const int  scan = fc5::switchspeed::scan_state(scan_s, took);
    if (scan == 2) g_scan_seen = true;
    int done = 0;
    if (!s.allowed || bare || s.weapon_recorded) done |= kWeapon;
    if (!s.allowed || bare || peek < 0 || peek == 1 || s.side_recorded) done |= kSide;
    if (scan == 0 || (scan == 3 && g_scan_seen)) done |= kScan;   // none found yet: one scan since the load is enough
    const int slot = fc5::gesture::load_slot_state();
    if (!s.allowed || slot == 0) done |= kSlot;
    for (int i = 0; i < 4; i++)
        if ((done & (1 << i)) && !(g_done_was & (1 << i))) g_done_at[i] = t;
    g_done_was = done;
    g_done     = done;
    g_elapsed  = t;

    float eta = kMinS - t;
    if (!(done & kWeapon)) eta = std::max(eta, 2.5f - t);
    if (!(done & kSide)) eta = std::max(eta, 7.0f - t);
    if (!(done & kSlot)) eta = std::max(eta, slot == 2 ? 2.0f : 9.0f - t);
    if (!(done & kScan)) {
        const float each = took > 0.5f ? took : 10.0f;
        eta = std::max(eta, scan == 2 ? each - scan_s : scan_s + each);
    }
    if (done != kAll) eta = std::max(eta, 1.0f);
    g_eta = std::clamp(eta, 0.0f, kMaxS - t);

    if ((done == kAll && t >= kMinS) || t >= kMaxS) {
        g_shown = false;
        if (done == kAll)
            spdlog::info("[LoadPopup] done after {:.1f} s (weapon {:.1f} s, sidearm {:.1f} s, weapon switch scan {:.1f} s, main weapon on the sling {:.1f} s): controller input sent again",
                         t, g_done_at[0], g_done_at[1], g_done_at[2], g_done_at[3]);
        else
            spdlog::info("[LoadPopup] closed after {:.0f} s with {} not done (weapon {}, sidearm {}: look {}, weapon switch scan state {}, loadslot {}): controller input sent again",
                         t, why_left(done), (done & kWeapon) ? "done" : "not recorded", (done & kSide) ? "done" : "not recorded", peek, scan, slot);
    }
}

bool blocking() { return g_shown.load(); }

bool draw()
{
    if (!g_shown.load()) return false;
    static const ImVec4 kAccent{ 0.93f, 0.46f, 0.13f, 1.00f };
    static const ImVec4 kText{ 0.95f, 0.92f, 0.86f, 1.00f };
    static const ImVec4 kHint{ 0.66f, 0.62f, 0.55f, 1.00f };
    static const ImVec4 kDone{ 0.45f, 0.80f, 0.40f, 1.00f };
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2{ disp.x * 0.5f, disp.y * 0.5f }, ImGuiCond_Always, ImVec2{ 0.5f, 0.5f });
    ImGui::PushFont(nullptr, 26.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{ 0.075f, 0.068f, 0.060f, 0.94f });
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4{ 0.55f, 0.27f, 0.09f, 1.00f });
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{ 0.16f, 0.14f, 0.12f, 1.00f });
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 28.0f, 22.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::Begin("##fc5vr_loadpopup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize);
    const int   done = g_done.load();
    const float eta = g_eta.load(), t = g_elapsed.load();
    ImGui::TextColored(kAccent, "VR mod loading");
    ImGui::Text("Controls back in %d s", std::max(1, int(std::ceil(eta))));
    ImGui::ProgressBar(std::clamp(t / std::max(0.1f, t + eta), 0.0f, 1.0f), ImVec2{ 440.0f, 10.0f }, "");
    const char* labels[4]{ "Weapon in hand", "Sidearm on the hip", "Faster weapon switch", "Main weapon on the sling" };
    for (int i = 0; i < 4; i++) {
        const bool d = (done & (1 << i)) != 0;
        ImGui::TextColored(d ? kDone : kHint, d ? "done" : "....");
        ImGui::SameLine(96.0f);
        ImGui::TextColored(d ? kText : kHint, "%s", labels[i]);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopFont();
    return true;
}

void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint)
{
    bool v = g_on.load();
    if (ImGui::Checkbox("\"VR mod loading\" popup after a load##loadpopup", &v)) cmds += v ? "loadpopup on\n" : "loadpopup off\n";
    hint("On (default): after a save loads (also a respawn or fast travel), a small popup counts down while the mod records your weapon and "
         "sidearm and looks for the weapons in memory (faster weapon switch); the controllers do nothing until it closes (30 s at most). "
         "Off: play starts at once, with a few seconds of hitches and no sidearm copy until then. The launcher's \"Skip the loading popup\" sets it too.");
}
}
