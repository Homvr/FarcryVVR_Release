// The game's own SetPlayerFOV, called by the mod (Homard 19 Oct: widen the parachute's 65 deg view through the game's
// script API instead of changing memory). The native behind the Lua function SetPlayerFOV (FC_m64+0x2FBEA10, reached
// from its Lua binding +0x2F61460): (player id*, angle deg, blend s, override flag, byte). Simulator 19 Oct 23:50, on foot:
// (1.745, 0.5, true) moved the view 120 -> 1.7 deg in 0.5 s, (120, 0.5, true) back; with the flag false nothing changed.
//
// Safety: the game's code is not thread-safe. The call is made inside the game's own camera update (the Tobii slot),
// and only when that thread is the one the game runs its scripts on: the game's own CScriptSystem::ExecuteString calls
// are watched by an inline hook that only records the thread and passes the call on (CryHook5's route, code at
// FC_m64+0xdfa424 verified byte for byte). No script text is ever run by the mod.
#include "FarCry5Lua.h"
#include "FarCry5Tobii.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

namespace fc5::lua
{
    namespace
    {
        using ExecFn    = bool(__fastcall*)(void* self, const char* code, bool raw);
        using FovFn     = void(__fastcall*)(uint64_t* id, float angle, float blend, bool flag, uint8_t extra);
        using LocalIdFn = uint64_t*(__fastcall*)(uint64_t* out);   // GetLocalPlayerId's helper (+0x2FA1E50)

        uint8_t*                   g_base     = nullptr;
        ExecFn                     g_exec     = nullptr;
        FovFn                      g_fov      = nullptr;
        LocalIdFn                  g_local_id = nullptr;
        safetyhook::InlineHook     g_hook{};
        bool                       g_tried    = false;
        bool                       g_ready    = false;
        std::atomic<unsigned long> g_game_tid{ 0 };    // the thread the game runs its scripts on
        std::atomic<uint64_t>      g_game_calls{ 0 };
        std::atomic<unsigned long> g_tick_tid{ 0 };    // the game's camera update thread
        std::atomic<bool>          g_in_play{ false };
        std::atomic<bool>          g_fov_pending{ false };
        std::atomic<float>         g_fov_a{ 0.0f }, g_fov_b{ 0.0f };
        std::atomic<bool>          g_fov_flag{ false };
        std::atomic<uint64_t>      g_fov_calls{ 0 };

        uint8_t* rel_target(uint8_t* at)   // E8 / E9 rel32
        {
            return at + 5 + *reinterpret_cast<int32_t*>(at + 1);
        }

        bool __fastcall hk_exec(void* self, const char* code, bool raw)
        {
            g_game_tid = GetCurrentThreadId();
            g_game_calls++;
            return g_hook.call<bool>(self, code, raw);
        }

        // Inside the game's camera update (the Tobii slot), on the game's own thread.
        void game_tick()
        {
            g_tick_tid = GetCurrentThreadId();
            if (!g_in_play.load() || !g_fov_pending.load()) return;
            const unsigned long script_tid = g_game_tid.load();
            if (script_tid == 0 || script_tid != GetCurrentThreadId()) {
                static int told = 0;
                if (told++ % 600 == 0)
                    spdlog::info("[PlayerFov] waiting: the game update runs on thread {}, its scripts on thread {}; not called", GetCurrentThreadId(), script_tid);
                return;
            }
            g_fov_pending = false;
            if (!g_fov || !g_local_id) return;
            uint64_t  tmp = 0;
            uint64_t* idp = g_local_id(&tmp);
            uint64_t  id  = idp ? *idp : 0;
            if (id == 0 || id == ~0ull) return;
            g_fov(&id, g_fov_a.load(), g_fov_b.load(), g_fov_flag.load(), 0);
            g_fov_calls++;
            spdlog::info("[PlayerFov] SetPlayerFOV({:.1f} deg, {:.2f} s, override {}) for the local player", g_fov_a.load(), g_fov_b.load(), g_fov_flag.load());
        }

        bool resolve()
        {
            g_base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
            if (!g_base) return false;
            // CryHook5's pattern "89 70 30 48 8B 0D", found once, at +0xdfa424 in this build; ExecuteString is the jmp 0x1D further.
            static const uint8_t kSig[]{ 0x89, 0x70, 0x30, 0x48, 0x8B, 0x0D };
            uint8_t* loc = g_base + 0xdfa424;
            if (std::memcmp(loc, kSig, sizeof(kSig)) != 0) {
                spdlog::error("[PlayerFov] unexpected code at FC_m64.dll+0xdfa424: not used");
                return false;
            }
            uint8_t* c = loc + 3 + 0x1D;
            if (*c != 0xE8 && *c != 0xE9) {
                spdlog::error("[PlayerFov] no call to ExecuteString at FC_m64.dll+{:#x}: not used", c - g_base);
                return false;
            }
            g_exec = reinterpret_cast<ExecFn>(rel_target(c));
            static const uint8_t kFovSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70 };
            if (std::memcmp(g_base + 0x2FBEA10, kFovSig, sizeof(kFovSig)) != 0 || g_base[0x2FA1E50] == 0xCC) {
                spdlog::error("[PlayerFov] unexpected code at SetPlayerFOV's native (FC_m64.dll+0x2fbea10): not used");
                return false;
            }
            g_fov      = reinterpret_cast<FovFn>(g_base + 0x2FBEA10);
            g_local_id = reinterpret_cast<LocalIdFn>(g_base + 0x2FA1E50);
            g_hook     = safetyhook::create_inline(reinterpret_cast<void*>(g_exec), reinterpret_cast<void*>(&hk_exec));
            if (!g_hook) {
                spdlog::error("[PlayerFov] could not watch the game's script thread: not used");
                return false;
            }
            fc5::tobii::set_game_tick(&game_tick);
            spdlog::info("[PlayerFov] ready: ExecuteString at FC_m64.dll+{:#x} watched for the script thread; SetPlayerFOV at +0x2fbea10", reinterpret_cast<uint8_t*>(g_exec) - g_base);
            return true;
        }
    }

    void frame_tick(bool in_play)
    {
        if (!g_tried) {
            g_tried = true;
            g_ready = resolve();
        }
        g_in_play = in_play;
    }

    void queue_fov(float angle, float blend, bool flag)
    {
        if (!g_ready) return;
        g_fov_a       = angle;
        g_fov_b       = blend;
        g_fov_flag    = flag;
        g_fov_pending = true;
    }

    std::string status()
    {
        return fmt::format("{}; game script thread {} ({} script calls seen), game update thread {}, SetPlayerFOV calls {}",
                           g_ready ? "ready" : (g_tried ? "not usable" : "not looked for yet"), g_game_tid.load(), g_game_calls.load(), g_tick_tid.load(), g_fov_calls.load());
    }
}
