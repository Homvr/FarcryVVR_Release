#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "utility/Thread.hpp"

#include "WindowsMessageHook.hpp"
#include <SafetyHook.hpp>

using namespace std;

static WindowsMessageHook* g_windows_message_hook{ nullptr };
std::recursive_mutex g_proc_mutex{};

LRESULT WINAPI window_proc(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    std::lock_guard _{ g_proc_mutex };

    if (g_windows_message_hook == nullptr) {
        return 0;
    }

    // Call our onMessage callback.
    auto& on_message = g_windows_message_hook->on_message;

    if (on_message) {
        // If it returns false we don't call the original window procedure.
        if (!on_message(wnd, message, w_param, l_param)) {
            return DefWindowProc(wnd, message, w_param, l_param);
        }
    }

    if(message == RE_WM_DEVICECHANGE) {
        message = WM_DEVICECHANGE;
    }

    // Call the original message procedure.
    return CallWindowProc(g_windows_message_hook->get_original(), wnd, message, w_param, l_param);
}

WindowsMessageHook::WindowsMessageHook(HWND wnd)
    : m_wnd{ wnd },
    m_original_proc{ nullptr }
{
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Initializing WindowsMessageHook");

    utility::ThreadSuspender suspender{};

    g_windows_message_hook = this;

    // Save the original window procedure.
    m_original_proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);

    // Set it to our "hook" procedure.
    SetWindowLongPtr(m_wnd, GWLP_WNDPROC, (LONG_PTR)&window_proc);
#ifndef DISABLE_WINDOW_SIZE_FIXES
    auto get_window_rect_fn = GetProcAddress(GetModuleHandleA("user32.dll"), "GetWindowRect");
    m_get_window_rect_hook = safetyhook::create_inline((void*)get_window_rect_fn, onGetWindowRect);
    if (!m_get_window_rect_hook) {
        spdlog::error("Failed to hook GetWindowRect");
    }

    auto get_client_rect_fn = GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect");
    m_get_client_rect_hook = safetyhook::create_inline((void*)get_client_rect_fn, onGetClientRect);
    if (!m_get_client_rect_hook) {
        spdlog::error("Failed to hook GetClientRect");
    }

    auto adjust_window_rect_fn = GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRect");
    m_adjust_client_rect_hook = safetyhook::create_inline((void*)adjust_window_rect_fn, onAdjustWindowRect);
    if (!m_adjust_client_rect_hook) {
        spdlog::error("Failed to hook AdjustWindowRect");
    }

    auto screen_to_client_fn = GetProcAddress(GetModuleHandleA("user32.dll"), "ScreenToClient");
    m_screen_to_client_hook = safetyhook::create_inline((void*)screen_to_client_fn, onScreenToClient);
    if (!m_screen_to_client_hook) {
        spdlog::error("Failed to hook ScreenToClient");
    }

    auto clip_cursor_fn = GetProcAddress(GetModuleHandleA("user32.dll"), "ClipCursor");
    m_clip_cursor_hook = safetyhook::create_inline((void*)clip_cursor_fn, onClipCursor);
    if (!m_clip_cursor_hook) {
        spdlog::error("Failed to hook ClipCursor");
    }
#endif
    spdlog::info("Hooked Windows message handler");
}

WindowsMessageHook::~WindowsMessageHook() {
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Destroying WindowsMessageHook");

    utility::ThreadSuspender suspender{};

    remove();
    g_windows_message_hook = nullptr;
}

bool WindowsMessageHook::remove() {
    // Don't attempt to restore invalid original window procedures.
    if (m_original_proc == nullptr || m_wnd == nullptr) {
        return true;
    }

    // Restore the original window procedure.
    auto current_proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);

    // lets not try to restore the original window procedure if it's not ours.
    if (current_proc == &window_proc) {
        SetWindowLongPtr(m_wnd, GWLP_WNDPROC, (LONG_PTR)m_original_proc);
    }

    // Invalidate this message hook.
    m_wnd = nullptr;
    m_original_proc = nullptr;

    return true;
}

bool WindowsMessageHook::is_hook_intact() {
    if (!m_wnd) {
        return false;
    }

    return GetWindowLongPtr(m_wnd, GWLP_WNDPROC) == (LONG_PTR)&window_proc;
}

BOOL WindowsMessageHook::onGetClientRect(HWND hWnd, LPRECT lpRect)
{
    auto on_get_client_rect = g_windows_message_hook->on_get_client_rect;

    auto result =  g_windows_message_hook->m_get_client_rect_hook.call<BOOL>(hWnd, lpRect);
    if(on_get_client_rect) {
        on_get_client_rect(&result, hWnd, lpRect);
    }
    return result;
}

BOOL WindowsMessageHook::onGetWindowRect(HWND hWnd, LPRECT lpRect)
{
    auto result = g_windows_message_hook->m_get_window_rect_hook.call<BOOL>(hWnd, lpRect);
    auto on_get_window_rect = g_windows_message_hook->on_get_window_rect;
    if(on_get_window_rect) {
        on_get_window_rect(&result, hWnd, lpRect);
    }
    return result;
}

BOOL WindowsMessageHook::onAdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu)
{
    auto on_adjust_window_rect = g_windows_message_hook->on_adjust_window_rect;
    auto result = g_windows_message_hook->m_adjust_client_rect_hook.call<BOOL>(lpRect, dwStyle, bMenu);
    if(on_adjust_window_rect) {
        on_adjust_window_rect(&result, g_windows_message_hook->m_wnd, lpRect, bMenu);
    }
    return result;
}

BOOL WindowsMessageHook::onScreenToClient(HWND hWnd, LPPOINT lpPoint)
{
    auto on_screen_to_client = g_windows_message_hook->on_screen_to_client;

    auto result = g_windows_message_hook->m_screen_to_client_hook.call<BOOL>(hWnd, lpPoint);

    if (on_screen_to_client) {
        on_screen_to_client(&result, hWnd, lpPoint);
    }

    return result;
}

BOOL WindowsMessageHook::onClipCursor(LPRECT lpRect)
{
    auto on_clip_cursor = g_windows_message_hook->on_clip_cursor;
    if (on_clip_cursor) {
        on_clip_cursor(&lpRect);
    }
    return g_windows_message_hook->m_clip_cursor_hook.call<BOOL>(lpRect);
}

BOOL WindowsMessageHook::GetClientRectOriginal(HWND hWnd, LPRECT lpRect)
{
    if(!g_windows_message_hook || !g_windows_message_hook->m_get_client_rect_hook) {
        return ::GetClientRect(hWnd, lpRect);
    }
    return g_windows_message_hook->m_get_client_rect_hook.call<BOOL>(hWnd, lpRect);
}

BOOL WindowsMessageHook::GetWindowRectOriginal(HWND hWnd, LPRECT lpRect)
{
    if(!g_windows_message_hook || !g_windows_message_hook->m_get_window_rect_hook) {
        return ::GetWindowRect(hWnd, lpRect);
    }
    return g_windows_message_hook->m_get_window_rect_hook.call<BOOL>(hWnd, lpRect);
}

BOOL WindowsMessageHook::ScreenToClientOriginal(HWND hWnd, LPPOINT lpPoint)
{
    if (!g_windows_message_hook || !g_windows_message_hook->m_screen_to_client_hook) {
        return ::ScreenToClient(hWnd, lpPoint);
    }
    return g_windows_message_hook->m_screen_to_client_hook.call<BOOL>(hWnd, lpPoint);
}

BOOL WindowsMessageHook::ClipCursorOriginal(RECT* lpRect)
{
    if (!g_windows_message_hook || !g_windows_message_hook->m_clip_cursor_hook) {
        return ::ClipCursor(lpRect);
    }
    return g_windows_message_hook->m_clip_cursor_hook.call<BOOL>(lpRect);
}