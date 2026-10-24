#include "FarCry5Probe.h"

#include <Framework.hpp>
#include <mods/VR.hpp>
#include <mods/vr/runtimes/OpenXR.hpp>
#include <spdlog/spdlog.h>

// The title of a window without sending it a message (GetWindowText sends WM_GETTEXT to windows of
// this process, which deadlocks when their thread waits on the render thread).
static void fc5_window_title(HWND w, char* out, int n)
{
    wchar_t wide[256]{};
    using Fn = int(WINAPI*)(HWND, LPWSTR, int);
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText"));
    out[0] = 0;
    if (fn && fn(w, wide, 256) > 0) WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, n, nullptr, nullptr);
}

namespace fc5::probe
{
    std::atomic<uint64_t> g_calls_pad{ 0 }, g_calls_hook{ 0 }, g_calls_hook_blocked{ 0 };
    std::atomic<uint32_t> g_last_index{ 0xFFFFFFFF }, g_last_ret_in{ 0 }, g_last_ret_out{ 0 };
    std::atomic<uint32_t> g_last_buttons{ 0 };
    std::atomic<int>      g_last_lx{ 0 }, g_last_ly{ 0 }, g_last_rx{ 0 }, g_last_ry{ 0 }, g_last_lt{ 0 }, g_last_rt{ 0 };
    std::atomic<int>      g_vigem_last_error{ 0 };
    std::atomic<uint32_t> g_vigem_buttons{ 0 };
    std::atomic<int>      g_vigem_lx{ 0 }, g_vigem_ly{ 0 }, g_vigem_rt{ 0 };

    void note_xinput(bool from_pad, bool blocked, uint32_t index, uint32_t ret_in, uint32_t ret_out, const XINPUT_GAMEPAD* g)
    {
        if (blocked) {
            g_calls_hook_blocked++;
            g_last_index   = index;
            g_last_ret_in  = ret_in;
            g_last_ret_out = ret_out;
            if (g) {
                g_last_buttons = g->wButtons;
                g_last_lx = g->sThumbLX; g_last_ly = g->sThumbLY; g_last_rx = g->sThumbRX; g_last_ry = g->sThumbRY;
                g_last_lt = g->bLeftTrigger; g_last_rt = g->bRightTrigger;
            }
            return;
        }
        (from_pad ? g_calls_pad : g_calls_hook)++;
        if (from_pad) return;
        g_last_index   = index;
        g_last_ret_in  = ret_in;
        g_last_ret_out = ret_out;
        if (g) {
            g_last_buttons = g->wButtons;
            g_last_lx = g->sThumbLX; g_last_ly = g->sThumbLY; g_last_rx = g->sThumbRX; g_last_ry = g->sThumbRY;
            g_last_lt = g->bLeftTrigger; g_last_rt = g->bRightTrigger;
        }
    }

    void note_vigem(int error, const XINPUT_GAMEPAD& g)
    {
        g_vigem_last_error = error;
        g_vigem_buttons    = g.wButtons;
        g_vigem_lx = g.sThumbLX; g_vigem_ly = g.sThumbLY; g_vigem_rt = g.bRightTrigger;
    }

    std::string xinput_line()
    {
        auto line = fmt::format("game XInput polls={} (passed through untouched={}) last pad={} ret {}->{} buttons={:04x} L=({},{}) R=({},{}) LT={} RT={} | virtual pad updates={} err={} buttons={:04x} L=({},{}) RT={}",
                                g_calls_hook.load(), g_calls_hook_blocked.load(), g_last_index.load(), g_last_ret_in.load(), g_last_ret_out.load(), g_last_buttons.load(),
                                g_last_lx.load(), g_last_ly.load(), g_last_rx.load(), g_last_ry.load(), g_last_lt.load(), g_last_rt.load(), g_calls_pad.load(),
                                g_vigem_last_error.load(), g_vigem_buttons.load(), g_vigem_lx.load(), g_vigem_ly.load(), g_vigem_rt.load());
        g_calls_hook = 0;
        g_calls_hook_blocked = 0;
        g_calls_pad = 0;
        return line;
    }

    std::string front_window()
    {
        char title[128]{};
        fc5_window_title(GetForegroundWindow(), title, sizeof(title));   // no message sent: the window's thread may be waiting on this one
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        CURSORINFO ci{ sizeof(ci) };
        GetCursorInfo(&ci);
        return fmt::format("front='{}' ({}) cursor={}", title, pid == GetCurrentProcessId() ? "game" : "other app", (ci.flags & CURSOR_SHOWING) ? "visible" : "hidden");
    }
}

// The framework's view of the controllers: detection, activity, poses and every action it maps.
std::string VR::fc5_probe_input()
{
    auto rt = get_runtime();
    if (!rt->loaded) {
        return "runtime not loaded";
    }
    const auto left  = get_left_stick_axis();
    const auto right = get_right_stick_axis();
    auto pose = [&](int idx) {
        const auto t = get_transform(idx);
        return fmt::format("({:.2f},{:.2f},{:.2f})", t[3][0], t[3][1], t[3][2]);
    };
    auto act = [&](vr::VRActionHandle_t h) {
        return (is_action_active(h, m_left_joystick) ? 1 : 0) + (is_action_active(h, m_right_joystick) ? 2 : 0);
    };
    std::string session = "?";
    if (rt->is_openxr()) {
        session = fmt::format("{}", (int)get_runtime<runtimes::OpenXR>()->session_state);
    }
    return fmt::format("session={} hmd={} controllers={} using={} spoofed={} lowestPad={} sinceInput={}s | headPos={} L={} R={} | Lstick=({:.2f},{:.2f}) Rstick=({:.2f},{:.2f}) "
                       "trigger={} grip={} A={} B={} stickClick={} system={} (1=left 2=right 3=both)",
                       session, is_hmd_active(), m_controllers.size(), is_using_controllers(), m_spoofed_gamepad_connection, m_lowest_xinput_user_index,
                       std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_last_controller_update).count(), pose(0),
                       pose(get_left_controller_index()), pose(get_right_controller_index()), left.x, left.y, right.x, right.y, act(m_action_trigger), act(m_action_grip),
                       act(m_action_a_button_right) | act(m_action_a_button_left), act(m_action_b_button_right) | act(m_action_b_button_left), act(m_action_joystick_click),
                       act(m_action_system_button));
}
