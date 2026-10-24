#include <chrono>
#include <spdlog/spdlog.h>
#include <utility/Scan.hpp>
#include <utility/String.hpp>

#include <SafetyHook.hpp>

#include "Framework.hpp"
#include "Mods.hpp"
#include "XInputHook.hpp"

XInputHook* g_hook{ nullptr };

XInputHook::XInputHook()
{
    g_hook = this;
    spdlog::info("[XInputHook] Entry");

    static auto find_dll = [](const std::string& name) -> HMODULE {
        const auto start_time = std::chrono::system_clock::now();
        auto       found_dll  = GetModuleHandleA(name.c_str());

        while (found_dll == nullptr) {
            if (found_dll = GetModuleHandleA(name.c_str()); found_dll != nullptr) {
                // Load it from the system directory instead, it might be hooked
                /*wchar_t system_dir[MAX_PATH]{};
                if (GetSystemDirectoryW(system_dir, MAX_PATH) != 0) {
                    const auto new_dir =
                 * (std::wstring{system_dir} + L"\\" + utility::widen(name));

                    spdlog::info("[XInputHook] Loading {} from {}", name, utility::narrow(new_dir));

                 * const auto new_dll = LoadLibraryW(new_dir.c_str());

                    if (new_dll != nullptr) {
                        found_dll =
                 * LoadLibraryW(new_dir.c_str());
                    }
                }*/

                break;
            }

            const auto elapsed_time = std::chrono::system_clock::now() - start_time;

            if (elapsed_time > std::chrono::seconds(120)) {
                spdlog::error("[XInputHook] Failed to find {} after 120 seconds", name);
                return nullptr;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return found_dll;
    };

    std::function<uintptr_t(uint8_t*)> recursive_resolve_jmp;

    recursive_resolve_jmp = [&](uint8_t* instr) -> uintptr_t {
        try {
            const auto decoded = utility::decode_one(instr);

            if (decoded) {
                const auto mnem = std::string_view{ decoded->Mnemonic };

                if (mnem.starts_with("JMP")) {
                    const auto target = utility::resolve_displacement((uintptr_t)instr);

                    if (target.has_value()) {
                        if (instr[0] == 0xFF && instr[1] == 0x25) {
                            const auto real_target = *(uintptr_t*)*target;

                            if (real_target == 0) {
                                return (uintptr_t)instr;
                            }

                            return recursive_resolve_jmp((uint8_t*)real_target);
                        }

                        return recursive_resolve_jmp((uint8_t*)target.value());
                    }
                }
            }
        }
        catch (...) {
            SPDLOG_ERROR("[XInputHook] recursive_resolve_jmp exception");
        }

        return (uintptr_t)instr;
    };

    auto perform_hooks_1_4 = [&]() {
        const auto xinput_1_4_dll = find_dll("xinput1_4.dll");

        if (xinput_1_4_dll != nullptr) {
            std::scoped_lock _{ g_framework->get_hook_monitor_mutex() };

            const auto get_state_fn        = (void*)GetProcAddress(xinput_1_4_dll, "XInputGetState");
            const auto set_state_fn        = (void*)GetProcAddress(xinput_1_4_dll, "XInputSetState");
            const auto get_capabilities_fn = (void*)GetProcAddress(xinput_1_4_dll, "XInputGetCapabilities");

            if (get_state_fn != nullptr) {
                m_xinput_1_4_get_state_hook = safetyhook::create_inline(get_state_fn, get_state_hook_1_4);

                if (!m_xinput_1_4_get_state_hook) {
                    spdlog::error("Failed to hook XInputGetState (1_4), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)get_state_fn);

                    if (jmp_addr != (uintptr_t)get_state_fn) {
                        m_xinput_1_4_get_state_hook = safetyhook::create_inline((void*)jmp_addr, get_state_hook_1_4);

                        if (!m_xinput_1_4_get_state_hook) {
                            spdlog::error("Failed to hook XInputGetState (1_4) (jmp)");
                        }
                    }
                    else {
                        spdlog::error("Cannot try jmp hook for XInputGetState (1_4) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            }
            else {
                spdlog::error("[XInputHook] Failed to find XInputGetState");
            }

            if (set_state_fn != nullptr) {
                m_xinput_1_4_set_state_hook = safetyhook::create_inline(set_state_fn, set_state_hook_1_4);

                if (!m_xinput_1_4_set_state_hook) {
                    spdlog::error("Failed to hook XInputSetState (1_4), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)set_state_fn);

                    if (jmp_addr != (uintptr_t)set_state_fn) {
                        m_xinput_1_4_set_state_hook = safetyhook::create_inline((void*)jmp_addr, set_state_hook_1_4);

                        if (!m_xinput_1_4_set_state_hook) {
                            spdlog::error("Failed to hook XInputSetState (1_4) (jmp)");
                        }
                    }
                    else {
                        spdlog::error("Cannot try jmp hook for XInputSetState (1_4) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            }
            else {
                spdlog::error("[XInputHook] Failed to find XInputSetState");
            }

            if (get_capabilities_fn != nullptr) {
                m_xinput_1_4_get_capabilities = safetyhook::create_inline(get_capabilities_fn, get_capabilities_1_4);
                if (!m_xinput_1_4_get_capabilities) {
                    spdlog::error("Failed to hook XInputGetCapabilities (1_4), trying jmp");

                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)get_capabilities_fn);

                    if (jmp_addr != (uintptr_t)get_capabilities_fn) {
                        m_xinput_1_4_get_capabilities = safetyhook::create_inline((void*)jmp_addr, get_capabilities_1_4);
                        if (!m_xinput_1_4_get_capabilities) {
                            spdlog::error("Failed to hook XInputGetCapabilities (1_4) (jmp)");
                        }
                    }
                    else {
                        spdlog::error("Cannot try jmp hook for XInputGetCapabilities (1_4) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            }
            else {
                spdlog::error("[XInputHook] Failed to find XInputGetCapabilities");
            }
        }

        spdlog::info("[XInputHook] Done (1_4)");
    };

    auto perform_hooks_1_3 = [&]() {
        // Far Cry 5 loads XINPUT9_1_0.dll (well after start-up); same exports as 1_3. Poll for either for two minutes.
        HMODULE xinput_1_3_dll = nullptr;
        {
            const auto start = std::chrono::steady_clock::now();
            while (xinput_1_3_dll == nullptr && std::chrono::steady_clock::now() - start < std::chrono::seconds(120)) {
                xinput_1_3_dll = GetModuleHandleA("xinput1_3.dll");
                if (xinput_1_3_dll == nullptr) xinput_1_3_dll = GetModuleHandleA("xinput9_1_0.dll");
                if (xinput_1_3_dll == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            char name[MAX_PATH]{};
            if (xinput_1_3_dll) GetModuleFileNameA(xinput_1_3_dll, name, MAX_PATH);
            spdlog::info("[XInputHook] legacy XInput module: {}", xinput_1_3_dll ? name : "none after 120 s");
        }

        if (xinput_1_3_dll != nullptr) {
            std::scoped_lock _{ g_framework->get_hook_monitor_mutex() };

            const auto get_state_fn        = (void*)GetProcAddress(xinput_1_3_dll, "XInputGetState");
            const auto set_state_fn        = (void*)GetProcAddress(xinput_1_3_dll, "XInputSetState");
            const auto get_capabilities_fn = (void*)GetProcAddress(xinput_1_3_dll, "XInputGetCapabilities");

            if (get_state_fn != nullptr) {
                m_xinput_1_3_get_state_hook = safetyhook::create_inline(get_state_fn, get_state_hook_1_3);

                if (!m_xinput_1_3_get_state_hook) {
                    spdlog::error("Failed to hook XInputGetState (1_3), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)get_state_fn);

                    if (jmp_addr != (uintptr_t)get_state_fn) {
                        m_xinput_1_3_get_state_hook = safetyhook::create_inline((void*)jmp_addr, get_state_hook_1_3);

                        if (!m_xinput_1_3_get_state_hook) {
                            spdlog::error("Failed to hook XInputGetState (1_3) (jmp)");
                        }
                    }
                    else {
                        spdlog::error("Cannot try jmp hook for XInputGetState (1_3) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            }
            else {
                spdlog::error("[XInputHook] Failed to find XInputGetState");
            }

            if (set_state_fn != nullptr) {
                m_xinput_1_3_set_state_hook = safetyhook::create_inline(set_state_fn, set_state_hook_1_3);

                if (!m_xinput_1_3_set_state_hook) {
                    spdlog::error("Failed to hook XInputSetState (1_3), trying jmp");

                    // Check if there is a jmp instruction at the start of the function and try to hook that instead
                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)set_state_fn);

                    if (jmp_addr != (uintptr_t)set_state_fn) {
                        m_xinput_1_3_set_state_hook = safetyhook::create_inline((void*)jmp_addr, set_state_hook_1_3);

                        if (!m_xinput_1_3_set_state_hook) {
                            spdlog::error("Failed to hook XInputSetState (1_3) (jmp)");
                        }
                    }
                    else {
                        spdlog::error("Cannot try jmp hook for XInputSetState (1_3) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            }
            else {
                spdlog::error("[XInputHook] Failed to find XInputSetState");
            }

            if (get_capabilities_fn != nullptr) {
                m_xinput_1_3_get_capabilities = safetyhook::create_inline(get_capabilities_fn, get_capabilities_1_3);
                if (!m_xinput_1_3_get_capabilities) {
                    spdlog::error("Failed to hook XInputGetCapabilities (1_3), trying jmp");

                    const auto jmp_addr = recursive_resolve_jmp((uint8_t*)get_capabilities_fn);

                    if (jmp_addr != (uintptr_t)get_capabilities_fn) {
                        m_xinput_1_3_get_capabilities = safetyhook::create_inline((void*)jmp_addr, get_capabilities_1_3);
                        if (!m_xinput_1_3_get_capabilities) {
                            spdlog::error("Failed to hook XInputGetCapabilities (1_3) (jmp)");
                        }
                    }
                    else {
                        spdlog::error("Cannot try jmp hook for XInputGetCapabilities (1_3) (jmp) (recursive_resolve_jmp failed)");
                    }
                }
            }
            else {
                spdlog::error("[XInputHook] Failed to find XInputGetCapabilities");
            }
        }

        spdlog::info("[XInputHook] Done (1_3)");
    };

    // We use a thread because this may possibly take a while to search for the DLLs
    // and it shouldn't cause any issues anyway
    spdlog::info("[XInputHook] Starting hook thread");
    m_hook_thread_1_4 = std::make_unique<std::jthread>(perform_hooks_1_4);
    m_hook_thread_1_3 = std::make_unique<std::jthread>(perform_hooks_1_3);
    spdlog::info("[XInputHook] Hook thread started");
}

uint32_t XInputHook::get_capabilities_1_3(uint32_t dwUserIndex, uint32_t dwFlags, XINPUT_CAPABILITIES* pCapabilities)
{
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_3_get_capabilities.call<uint32_t>(dwUserIndex, dwFlags, pCapabilities);
    }

    auto ret = g_hook->m_xinput_1_3_get_capabilities.call<uint32_t>(dwUserIndex, dwFlags, pCapabilities);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_get_capabilities(&ret, dwUserIndex, dwFlags, pCapabilities);
    }

    return ret;
}

uint32_t XInputHook::get_capabilities_1_4(uint32_t dwUserIndex, uint32_t dwFlags, XINPUT_CAPABILITIES* pCapabilities)
{
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_4_get_capabilities.call<uint32_t>(dwUserIndex, dwFlags, pCapabilities);
    }

    auto ret = g_hook->m_xinput_1_4_get_capabilities.call<uint32_t>(dwUserIndex, dwFlags, pCapabilities);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_get_capabilities(&ret, dwUserIndex, dwFlags, pCapabilities);
    }

    return ret;
}

uint32_t XInputHook::get_state_hook_1_4(uint32_t user_index, XINPUT_STATE* state)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::info("[XInputHook] the game polls XInputGetState through the 1_4 hook (pad {})", user_index);
    }
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_4_get_state_hook.call<uint32_t>(user_index, state);
    }

    auto ret = g_hook->m_xinput_1_4_get_state_hook.call<uint32_t>(user_index, state);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_get_state(&ret, user_index, state);
    }

    return ret;
}

uint32_t XInputHook::set_state_hook_1_4(uint32_t user_index, XINPUT_VIBRATION* vibration)
{
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_4_set_state_hook.call<uint32_t>(user_index, vibration);
    }

    auto ret = g_hook->m_xinput_1_4_set_state_hook.call<uint32_t>(user_index, vibration);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_set_state(&ret, user_index, vibration);
    }

    return ret;
}

uint32_t XInputHook::get_state_hook_1_3(uint32_t user_index, XINPUT_STATE* state)
{
    static bool logged = false;
    if (!logged) {
        logged = true;
        spdlog::info("[XInputHook] the game polls XInputGetState through the 1_3 hook (pad {})", user_index);
    }
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_3_get_state_hook.call<uint32_t>(user_index, state);
    }

    auto ret = g_hook->m_xinput_1_3_get_state_hook.call<uint32_t>(user_index, state);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_get_state(&ret, user_index, state);
    }

    return ret;
}

uint32_t XInputHook::set_state_hook_1_3(uint32_t user_index, XINPUT_VIBRATION* vibration)
{
    if (!g_framework->is_ready()) {
        return g_hook->m_xinput_1_3_set_state_hook.call<uint32_t>(user_index, vibration);
    }

    auto ret = g_hook->m_xinput_1_3_set_state_hook.call<uint32_t>(user_index, vibration);

    const auto& mods = g_framework->get_mods()->get_mods();

    for (auto& mod : mods) {
        mod->on_xinput_set_state(&ret, user_index, vibration);
    }

    return ret;
}
