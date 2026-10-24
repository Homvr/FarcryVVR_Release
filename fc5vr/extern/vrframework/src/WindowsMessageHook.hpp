#pragma once

#include <functional>

#include <Windows.h>
#include <safetyhook/inline_hook.hpp>


#define RE_TOGGLE_CURSOR WM_APP + 1
#define RE_WM_DEVICECHANGE RE_TOGGLE_CURSOR + 1

// This type of hook replaces a windows message procedure so that it can intercept
// messages sent to the window.
class WindowsMessageHook {
public:
    std::function<bool(HWND, UINT, WPARAM, LPARAM)> on_message;
    std::function<void(int*,HWND, LPRECT)> on_get_client_rect;
    std::function<void(int*,HWND, LPRECT)> on_get_window_rect;
    std::function<void(int*,HWND, LPRECT, bool)> on_adjust_window_rect;
    std::function<void(int*, HWND, LPPOINT)> on_screen_to_client;
    std::function<bool(LPRECT*)> on_clip_cursor;

    WindowsMessageHook() = delete;
    WindowsMessageHook(const WindowsMessageHook& other) = delete;
    WindowsMessageHook(WindowsMessageHook&& other) = delete;
    WindowsMessageHook(HWND wnd);
    virtual ~WindowsMessageHook();

    // This gets called automatically by the destructor but you can call it
    // explicitly if you need to remove the message hook for some reason.
    bool remove();

    auto is_valid() const {
        return m_original_proc != nullptr;
    }

    auto get_original() const {
        return m_original_proc;
    }

    inline void window_toggle_cursor(bool show) {
        ::PostMessage(m_wnd, RE_TOGGLE_CURSOR, show, 1);
    }

    static BOOL GetClientRectOriginal(HWND hWnd,_Out_ LPRECT lpRect);

    static BOOL GetWindowRectOriginal(HWND hWnd,_Out_ LPRECT lpRect);
    static BOOL ScreenToClientOriginal(HWND hWnd, _Inout_ LPPOINT lpPoint);
    static BOOL ClipCursorOriginal(_In_opt_ RECT* lpRect);

    WindowsMessageHook& operator=(const WindowsMessageHook& other) = delete;
    WindowsMessageHook& operator=(const WindowsMessageHook&& other) = delete;

    bool is_hook_intact();

private:
    HWND m_wnd;
    WNDPROC m_original_proc;
    safetyhook::InlineHook m_get_window_rect_hook;
    safetyhook::InlineHook m_get_client_rect_hook;
    safetyhook::InlineHook m_adjust_client_rect_hook;
    safetyhook::InlineHook m_screen_to_client_hook;
    safetyhook::InlineHook m_clip_cursor_hook;

    static BOOL WINAPI onGetClientRect(_In_ HWND hWnd,_Out_ LPRECT lpRect);
    static BOOL WINAPI onGetWindowRect(_In_ HWND hWnd,_Out_ LPRECT lpRect);
    static BOOL WINAPI onAdjustWindowRect(_Inout_ LPRECT lpRect, DWORD dwStyle, BOOL bMenu);
    static BOOL WINAPI onScreenToClient(_In_ HWND hWnd, _Inout_ LPPOINT lpPoint);
    static BOOL WINAPI onClipCursor(_In_opt_ LPRECT lpRect);
};