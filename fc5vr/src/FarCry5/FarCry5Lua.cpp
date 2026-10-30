// The game's own SetPlayerFOV, called by the mod (Homard 19 Oct: widen the parachute's 65 deg view through the game's
// script API instead of changing memory). The native behind the Lua function SetPlayerFOV (FC_m64+0x2FBEA10, reached
// from its Lua binding +0x2F61460): (player id*, angle deg, blend s, override flag, byte). Simulator 19 Oct 23:50, on foot:
// (1.745, 0.5, true) moved the view 120 -> 1.7 deg in 0.5 s, (120, 0.5, true) back; with the flag false nothing changed.
//
// Safety: the game's code is not thread-safe. The call is made inside the game's own camera update (the Tobii slot),
// and only when that thread is the one the game runs its scripts on: the game's own CScriptSystem::ExecuteString calls
// are watched by an inline hook that only records the thread and passes the call on (CryHook5's route, code at
// FC_m64+0xdfa424 verified byte for byte). No script text is ever run by the mod.
//
// meleedamage (26 Oct, Homard 12:44: "make the melee hitting instantaneous ... the damage is done immediatly with the gesture"):
// the native behind the Lua function SendPierceStimWithOrigin (+0x2FB6760: (target id*, origin id, damage); its binding +0x2F59B80
// reads (id, id, float)) sends the target a damage stim from the origin, its direction from origin to target. The NPC hit is found
// by position: the game's entity lookup by id (+0xB27D60: (manager [+0x4FAC1E0], handle* out, id), a jmp into protected code; a
// handle holds its entity at +0x10, a refcount at +8, the null handle is +0x4CB9E70) is watched only while meleedamage is on and
// records the ids it finds; at a hit the recorded ids' positions are read with the native behind GetEntityPosition (+0x2FA09C0:
// (id*, axis 0-2) -> float) on the game's script thread, and the one nearest the body's root is the target. All read statically
// from this build's FC_m64.dll (26 Oct), not run yet.
//
// vehscript (29 Oct, Homard 15:22: "go to game-script route"): whether the player is in a vehicle, from the game itself instead of
// the hands' pass. The native behind the Lua function GetPawnVehicle (+0x2FA2C30, from its binding +0x2F4A3A0 registered at
// +0x2F76E58): (pawn id*, vehicle id* out) -> out; the pawn from its id (+0x2F8E610, the entity lookup), its current vehicle
// (+0x1C72980, protected code), and that vehicle's entity id, or -1 when it has none. The seat: GetPawnVehicleSeat's native
// (+0x2FA2C80, binding +0x2F4A450): (pawn id*) -> seat index (0 without one). The driver's seat: what GetDriverSeatIndex's binding
// (+0x2F80E30) does inline: the vehicle from its id (+0x2F8E8C0), then its driver seat index (+0x1C73890 on the vehicle + 0x20).
// The type: GetPawnVehicleType's native (+0x2FA2CD0, binding +0x2F4A4F0): (pawn id*) -> 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle,
// 4 helicopter, 5 plane, 6 wingsuit, 7 none (the engine natives report of 30 Oct). All plain reads, made like SetPlayerFOV (the camera update on the script thread), under SEH: a fault turns vehscript off.
// Read statically from this build's FC_m64.dll (29 Oct), not run yet.
//
// gamestate (30 Oct, log only, research/2026-10-30_engine_natives.md): the player's state as the game's own script functions give it,
// read every 100 ms on the script thread like vehscript: GetPawnVehicleType (+0x2FA2CD0; 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle,
// 4 helicopter, 5 plane, 6 wingsuit, 7 none, from the VEHICLE_TYPE_* string order), GetPawnRidingEntity (+0x2FA2AE0), GetPawnStance
// (+0x2FA2B50), IsInQTE (+0x2FA8E80), IsPawnDown (+0x2FA9900), IsPawnDead (+0x2FA97D0), IsInAir (+0x2FA8D20), IsInWater (+0x2FA8F90),
// GetPlayerFOV (+0x2FA2F40), GetPlayerNearFOV (+0x2FA3670); IsGamePaused's byte (*(+0x4EB59F8) + 0x68, set by paused menu pages) read every
// frame; the Lua bindings that start, stop or skip cinematics or switch the camera, and the cinematic black bars (+0x1476B40, a jmp), logged
// as they are called. All read statically from this build's FC_m64.dll (30 Oct), not run yet.
#include "FarCry5Lua.h"
#include "FarCry5Tobii.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>
#include <string>
#include <utility>

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

        // meleedamage
        using LookupFn = void*(__fastcall*)(void* mgr, void** out, uint64_t id);
        using PosFn    = float(__fastcall*)(uint64_t* id, int axis);
        using StimFn   = void(__fastcall*)(uint64_t* target, uint64_t origin, float damage);
        using HealthFn = float(__fastcall*)(uint64_t* id);   // GetPawnHealth's native (+0x2FA2900)
        PosFn                  g_pos  = nullptr;
        HealthFn               g_health = nullptr;
        std::atomic<bool>      g_self_pending{ false };   // meleedamageself (test): the damage stim sent to the player
        std::atomic<float>     g_self_amount{ 0.0f };
        uint64_t               g_after_id   = 0;          // the last target, its health read again 30 updates later
        uint32_t               g_after_tick = 0;
        float                  g_after_h0   = 0.0f;
        StimFn                 g_stim = nullptr;
        safetyhook::InlineHook g_lookup_hook{};
        std::atomic<bool>      g_watch{ false };
        bool                   g_watch_tried = false, g_watch_ok = false;
        constexpr int          kIds = 2048;
        std::atomic<uint64_t>  g_ids[kIds]{};       // ids the lookup found an entity for (by hash)
        std::atomic<uint32_t>  g_id_tick[kIds]{};   // ... and the game update (g_tick_n) it was last seen in
        std::atomic<uint32_t>  g_tick_n{ 0 };       // game updates (game_tick)
        std::atomic<uint64_t>  g_lookups{ 0 };
        std::atomic<bool>      g_dmg_pending{ false };
        std::atomic<float>     g_dmg_x{ 0.0f }, g_dmg_y{ 0.0f }, g_dmg_z{ 0.0f }, g_dmg_amount{ 0.0f };
        std::atomic<bool>      g_dmg_send{ false };
        std::atomic<uint64_t>  g_dmg_sent{ 0 }, g_dmg_found{ 0 }, g_dmg_missed{ 0 };
        std::atomic<int64_t>   g_dmg_t{ 0 };
        std::atomic<bool>      g_probe_pending{ false };   // meleedamageprobe (test)
        std::atomic<float>     g_probe_x{ 0.0f }, g_probe_y{ 0.0f }, g_probe_z{ 0.0f };

        // vehscript
        using PawnVehFn    = uint64_t*(__fastcall*)(uint64_t* id, uint64_t* out);   // GetPawnVehicle's native (+0x2FA2C30)
        using PawnSeatFn   = int(__fastcall*)(uint64_t* id);                         // GetPawnVehicleSeat's native (+0x2FA2C80)
        using VehObjFn     = uint8_t*(__fastcall*)(uint64_t id);                     // the vehicle from its id (+0x2F8E8C0)
        using DriverSeatFn = int(__fastcall*)(uint8_t* seats);                       // its driver seat index (+0x1C73890, vehicle + 0x20)
        using PawnTypeFn   = int(__fastcall*)(uint64_t* id);                         // GetPawnVehicleType's native (+0x2FA2CD0)
        PawnVehFn             g_pawn_veh    = nullptr;
        PawnSeatFn            g_pawn_seat   = nullptr;
        VehObjFn              g_veh_obj     = nullptr;
        DriverSeatFn          g_driver_seat = nullptr;
        PawnTypeFn            g_pawn_type   = nullptr;
        std::atomic<bool>     g_veh_on{ false };
        bool                  g_veh_tried = false, g_veh_ok = false;
        std::atomic<bool>     g_veh_failed{ false };
        std::atomic<int>      g_veh_state{ -1 }, g_veh_driver{ -1 }, g_veh_seat{ -1 }, g_veh_dseat{ -1 }, g_veh_type{ -1 };
        std::atomic<uint64_t> g_veh_id{ 0 }, g_veh_queries{ 0 };
        std::atomic<int64_t>  g_veh_t{ 0 };     // the last answer (now_ms)
        int64_t               g_veh_ask_t = 0;  // the last query (script thread only)
        int                   g_veh_said  = 0;

        // gamestate (log only)
        using PawnIntFn   = int(__fastcall*)(uint64_t* id);     // GetPawnVehicleType (+0x2FA2CD0), GetPawnStance (+0x2FA2B50)
        using PawnBoolFn  = bool(__fastcall*)(uint64_t* id);    // IsInQTE, IsPawnDown, IsPawnDead, IsInAir, IsInWater
        using PawnFloatFn = float(__fastcall*)(uint64_t* id);   // GetPlayerFOV (+0x2FA2F40), GetPlayerNearFOV (+0x2FA3670)
        struct GameState
        {
            uint64_t veh = ~0ull, ride = ~0ull;
            int      vtype = -1, seat = -1, dseat = -1, stance = -1;
            bool     qte = false, down = false, dead = false, air = false, water = false;
            float    fov = 0.0f, nearfov = 0.0f;
            int64_t  cam_vt = -1;        // the active camera's vtable (FC_m64 rva), -1 none, -2 the read faulted
        };
        PawnIntFn             g_gs_vtype = nullptr, g_gs_stance = nullptr;
        PawnBoolFn            g_gs_qte = nullptr, g_gs_down = nullptr, g_gs_dead = nullptr, g_gs_air = nullptr, g_gs_water = nullptr;
        PawnFloatFn           g_gs_fov = nullptr, g_gs_nearfov = nullptr;
        PawnVehFn             g_gs_ride  = nullptr;   // GetPawnRidingEntity (+0x2FA2AE0), GetPawnVehicle's shape
        uint8_t**             g_gs_pause = nullptr;   // IsGamePaused (+0x2FA8580): *(*(+0x4EB59F8) + 0x68)
        std::atomic<bool>     g_gs_on{ false };
        bool                  g_gs_tried = false, g_gs_ok = false;
        std::atomic<bool>     g_gs_failed{ false };
        GameState             g_gs_last{};            // script thread only
        bool                  g_gs_have = false;
        int64_t               g_gs_ask_t = 0;
        std::atomic<uint64_t> g_gs_reads{ 0 }, g_gs_changes{ 0 };
        int                   g_gs_pause_last = -1;   // render thread only

        // downview: IsPawnDown (+0x2FA9900) for the player, read in vehscript's 150 ms poll on the script thread (its own switch,
        // with or without vehscript and gamestate); a fault turns it off for the session.
        PawnBoolFn            g_down_fn = nullptr;
        std::atomic<bool>     g_down_on{ false };
        bool                  g_down_tried = false, g_down_ok = false;
        std::atomic<bool>     g_down_failed{ false };
        std::atomic<int>      g_down_state{ -1 };
        std::atomic<int64_t>  g_down_t{ 0 };     // the last answer (now_ms)
        // GetPawnStance (+0x2FA2B50) in the same poll while vehscript asks: 0 standing, 1 crouched, 4 downed, 18 seated in a vehicle (09:27 log).
        PawnIntFn             g_stance_fn = nullptr;
        std::atomic<int>      g_stance{ -1 };
        std::atomic<int64_t>  g_stance_t{ 0 };
        // chutemin: IsInAir (+0x2FA8D20) in the same poll while vehscript asks: true from leaving the ground to landing or the chute opening.
        PawnBoolFn            g_air_fn = nullptr;
        std::atomic<int>      g_air{ -1 };
        std::atomic<int64_t>  g_air_t{ 0 };

        // gamestate: the game's scripts starting, stopping or skipping cinematics, taking the camera, and the cinematic black bars,
        // logged as they are called (their Lua bindings, int(lua_State*), watched while gamestate is on; nothing changed).
        struct ScriptWatch { uint32_t rva; const char* name; };
        constexpr ScriptWatch kWatch[]{
            { 0x2F89B90, "StartCinematicScene" }, { 0x2F89DD0, "StartCinematicScene_v2" },
            { 0x2F65090, "StartCinematicMetaSequence" }, { 0x31B8D60, "StartCinematicMetaSequence (sequence manager)" },
            { 0x2F65430, "StartCinematicSequenceWithMetaId" }, { 0x31B9130, "StartCinematicSequenceWithMetaId (sequence manager)" },
            { 0x2F652D0, "StartCinematicSequenceFromBeginningWithMetaId" }, { 0x31B8FC0, "StartCinematicSequenceFromBeginningWithMetaId (sequence manager)" },
            { 0x2F65130, "StartCinematicSequenceAtSyncPointWithMetaId" }, { 0x31B8E10, "StartCinematicSequenceAtSyncPointWithMetaId (sequence manager)" },
            { 0x2F667F0, "StopCinematicSequence" }, { 0x31B9620, "StopCinematicSequence (sequence manager)" },
            { 0x2F66700, "StopCinematicMetaSequence" }, { 0x31B92A0, "StopCinematicMetaSequence (sequence manager)" },
            { 0x2F637E0, "SkipCinematicSequence" }, { 0x31B8C30, "SkipCinematicSequence (sequence manager)" },
            { 0x2F67620, "SwitchCamera" }, { 0x2F5B440, "SetCloseUpCamera" },
        };
        constexpr int          kWatchN = int(sizeof(kWatch) / sizeof(kWatch[0]));
        safetyhook::InlineHook g_watch_hooks[kWatchN]{};
        std::atomic<uint64_t>  g_watch_calls{ 0 };
        safetyhook::InlineHook g_bars_hook{};
        std::atomic<int>       g_bars{ -1 };   // the last DisplayCinematicBlackBars (+0x1476B40, bool), -1 none yet

        template <int N> int __fastcall hk_script(void* L)
        {
            if (g_gs_on.load(std::memory_order_relaxed)) {
                g_watch_calls++;
                spdlog::info("[GameState] script calls {}", kWatch[N].name);
            }
            return g_watch_hooks[N].call<int>(L);
        }

        template <int... I> void watch_install(std::integer_sequence<int, I...>, uint8_t* base, int& ok)
        {
            ((g_watch_hooks[I] = safetyhook::create_inline(reinterpret_cast<void*>(base + kWatch[I].rva), reinterpret_cast<void*>(&hk_script<I>)), ok += bool(g_watch_hooks[I])), ...);
        }

        void __fastcall hk_bars(uint8_t on)
        {
            if (g_gs_on.load(std::memory_order_relaxed)) {
                spdlog::info("[GameState] cinematic black bars {} (were {})", on ? "on" : "off", g_bars.load());
                g_bars = on ? 1 : 0;
            }
            g_bars_hook.call<void>(on);
        }

        // gamestate: the camera the game projects HUD points with, found as Map3DPosInto2DHud's native (+0x2FAB890) does: the camera list
        // at +0x4FB3110 (count in the high dword of +0x10, first entry *[+8]), its view index (+0xE69C50), the view service (the function
        // pointer at +0x4CCDA60), the service's camera for that index (vtable +0x198), all game code. Only the camera's vtable is kept: one
        // per camera kind (player, map, photo mode, death, a bone during an animation, cinematic, vehicle...), named once the run shows them.
        bool                  g_gs_cam_ok = true;
        int64_t gs_cam_read(uint8_t* base)
        {
            __try {
                uint8_t* list = *reinterpret_cast<uint8_t**>(base + 0x4FB3110);
                if (!list || ((*reinterpret_cast<uint64_t*>(list + 0x10) >> 32) & 0x7FFFFFFF) == 0) return -1;
                void* first = **reinterpret_cast<void***>(list + 8);
                if (!first) return -1;
                const uint8_t idx = reinterpret_cast<uint8_t(__fastcall*)(void*)>(base + 0xE69C50)(first);
                void*  tmp[2]{};
                void** svcp = reinterpret_cast<void**(__fastcall*)(void**)>(*reinterpret_cast<void**>(base + 0x4CCDA60))(tmp);
                void*  svc  = svcp ? *svcp : nullptr;
                if (!svc) return -1;
                void* cam = (*reinterpret_cast<void* (__fastcall***)(void*, uint32_t)>(svc))[0x198 / 8](svc, idx);
                if (!cam) return -1;
                return int64_t(*reinterpret_cast<uint8_t**>(cam) - base);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        const char* vehicle_type_name(int t)
        {
            // The VEHICLE_TYPE_* strings in FC_m64.dll, in their stored order (7 = none, GetVehicleType's default).
            static const char* kNames[]{ "wheeled", "boat", "paraglider", "motorcycle", "helicopter", "plane", "wingsuit", "none" };
            return t >= 0 && t < 8 ? kNames[t] : t == -1 ? "-" : "?";
        }

        // gamestate: the reads, under SEH (no C++ objects here): false on a fault.
        bool gs_read(uint64_t me, GameState& s)
        {
            __try {
                uint64_t id = me, out = ~0ull;
                if (g_pawn_veh) {
                    g_pawn_veh(&id, &out);
                    s.veh = out;
                    if (out != ~0ull && out != 0) {
                        id     = me;
                        s.seat = g_pawn_seat(&id);
                        if (uint8_t* v = g_veh_obj(out)) s.dseat = g_driver_seat(v + 0x20);
                    }
                }
                id = me; out = ~0ull;
                g_gs_ride(&id, &out);
                s.ride = out;
                id = me; s.vtype   = g_gs_vtype(&id);
                id = me; s.stance  = g_gs_stance(&id);
                id = me; s.qte     = g_gs_qte(&id);
                id = me; s.down    = g_gs_down(&id);
                id = me; s.dead    = g_gs_dead(&id);
                id = me; s.air     = g_gs_air(&id);
                id = me; s.water   = g_gs_water(&id);
                id = me; s.fov     = g_gs_fov(&id);
                id = me; s.nearfov = g_gs_nearfov(&id);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        int gs_pause_read()
        {
            __try {
                const uint8_t* g = *g_gs_pause;
                return g ? g[0x68] : -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        std::string gs_text(const GameState& s)
        {
            const bool in = s.veh != ~0ull && s.veh != 0;
            return fmt::format("vehicle {} (type {} {}, seat {}, driver's seat {}), riding {}, stance {}, QTE {}, down {}, dead {}, in air {}, in water {}, FOV {:.2f}, near FOV {:.2f}, camera {}",
                               in ? fmt::format("{:x}", s.veh) : std::string{ "none" }, s.vtype, vehicle_type_name(s.vtype), s.seat, s.dseat,
                               s.ride != ~0ull && s.ride != 0 ? fmt::format("{:x}", s.ride) : std::string{ "none" }, s.stance, s.qte, s.down, s.dead, s.air, s.water, s.fov,
                               s.nearfov, s.cam_vt >= 0 ? fmt::format("vtable +{:#x}", s.cam_vt) : s.cam_vt == -1 ? std::string{ "none" } : std::string{ "fault" });
        }

        // On the game's script thread, every 100 ms: the player's state as the game's own script functions give it; a line on each change.
        void gs_tick()
        {
            g_gs_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            GameState s{};
            if (!gs_read(me, s)) {
                g_gs_on     = false;
                g_gs_failed = true;
                spdlog::error("[GameState] a fault reading the player's state ({:x}): gamestate turned off", me);
                return;
            }
            if (g_gs_cam_ok) {
                s.cam_vt = gs_cam_read(g_base);
                if (s.cam_vt == -2) {
                    g_gs_cam_ok = false;
                    spdlog::error("[GameState] a fault finding the active camera: the camera no longer read (the rest goes on)");
                }
            }
            g_gs_reads++;
            const GameState& p = g_gs_last;
            std::string what;
            auto add = [&](bool changed, const char* name) { if (changed) what += what.empty() ? name : fmt::format(", {}", name); };
            add(!g_gs_have, "first read");
            add(g_gs_have && (s.veh != p.veh || s.seat != p.seat || s.dseat != p.dseat), "vehicle");
            add(g_gs_have && s.vtype != p.vtype, "vehicle type");
            add(g_gs_have && s.ride != p.ride, "riding");
            add(g_gs_have && s.stance != p.stance, "stance");
            add(g_gs_have && s.qte != p.qte, "QTE");
            add(g_gs_have && s.down != p.down, "down");
            add(g_gs_have && s.dead != p.dead, "dead");
            add(g_gs_have && s.air != p.air, "in air");
            add(g_gs_have && s.water != p.water, "in water");
            add(g_gs_have && (std::abs(s.fov - p.fov) > 0.05f || std::abs(s.nearfov - p.nearfov) > 0.05f), "FOV");
            add(g_gs_have && s.cam_vt != p.cam_vt, "camera");
            if (!what.empty()) {
                g_gs_changes++;
                spdlog::info("[GameState] {}: {}", what, gs_text(s));
            }
            g_gs_last = s;
            g_gs_have = true;
        }

        // vehscript: the reads, under SEH (no C++ objects here): false on a fault.
        bool veh_read(uint64_t me, uint64_t& veh, int& seat, int& dseat, int& type)
        {
            __try {
                uint64_t id = me, out = ~0ull;
                g_pawn_veh(&id, &out);
                veh   = out;
                seat  = -1;
                dseat = -1;
                type  = -1;
                if (out != ~0ull && out != 0) {
                    id   = me;
                    seat = g_pawn_seat(&id);
                    id   = me;
                    type = g_pawn_type(&id);
                    if (uint8_t* v = g_veh_obj(out)) dseat = g_driver_seat(v + 0x20);
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // GetPawnStance, under SEH: -2 a fault.
        int stance_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_stance_fn(&id);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // chutemin: IsInAir, under SEH: 1, 0, -2 a fault.
        int air_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_air_fn(&id) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // downview: IsPawnDown, under SEH: 1 down, 0 not, -2 a fault.
        int down_read(uint64_t me)
        {
            __try {
                uint64_t id = me;
                return g_down_fn(&id) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -2;
            }
        }

        // On the game's script thread: GetPawnVehicle for the local player; every change logged (downview: IsPawnDown too).
        void veh_tick()
        {
            g_veh_ask_t = now_ms();
            if (!g_local_id) return;
            uint64_t       tmp = 0;
            uint64_t*      idp = g_local_id(&tmp);
            const uint64_t me  = idp ? *idp : 0;
            if (me == 0 || me == ~0ull) return;
            if (g_down_on.load()) {
                const int d = down_read(me);
                if (d == -2) {
                    g_down_on     = false;
                    g_down_failed = true;
                    g_down_state  = -1;
                    spdlog::error("[Down] downview: a fault reading IsPawnDown for the player {:x}: downview off for this session", me);
                } else {
                    g_down_state = d;
                    g_down_t     = now_ms();
                }
            }
            if (!g_veh_on.load()) return;
            if (g_stance_fn) {
                const int s = stance_read(me);
                if (s == -2) {
                    g_stance_fn = nullptr;
                    g_stance    = -1;
                    spdlog::error("[Vehicle] vehscript: a fault reading GetPawnStance for the player {:x}: the stance no longer read", me);
                } else {
                    g_stance   = s;
                    g_stance_t = now_ms();
                }
            }
            if (g_air_fn) {
                const int a = air_read(me);
                if (a == -2) {
                    g_air_fn = nullptr;
                    g_air    = -1;
                    spdlog::error("[Vehicle] vehscript: a fault reading IsInAir for the player {:x}: in the air no longer read (chutemin falls back to its timing rule)", me);
                } else {
                    g_air   = a;
                    g_air_t = now_ms();
                }
            }
            uint64_t veh = ~0ull;
            int      seat = -1, dseat = -1, type = -1;
            if (!veh_read(me, veh, seat, dseat, type)) {
                g_veh_on     = false;
                g_veh_failed = true;
                g_veh_state  = -1;
                spdlog::error("[Vehicle] vehscript: a fault reading GetPawnVehicle for the player {:x}: turned off (the mod's own rules decide)", me);
                return;
            }
            const int st  = (veh != ~0ull && veh != 0) ? 1 : 0;
            // GetPawnVehicleSeat counts from 1 (0 during the ~2 s entry animation), the driver's seat lookup from 0 (research\2026-10-30_engine_natives.md,
            // Homard's 09:27 log: quad driver 1, heli pilot 1, heli passenger 3; the old seat == dseat called the driver a passenger). Seat 0: no answer (-1).
            const int drv = st ? (seat >= 1 ? ((seat - 1 == dseat) ? 1 : 0) : -1) : 0;
            g_veh_queries++;
            g_veh_t = now_ms();
            // vehdrivegun (30 Oct): a change of the seat or the driver's seat alone is logged too (both are under question).
            if (st != g_veh_state.load() || (st && (drv != g_veh_driver.load() || veh != g_veh_id.load() || type != g_veh_type.load() || seat != g_veh_seat.load() ||
                                                    dseat != g_veh_dseat.load())) || g_veh_said < 3) {
                g_veh_said++;
                static const char* kTypes[]{ "wheeled", "boat", "paraglider", "motorcycle", "helicopter", "plane", "wingsuit", "none" };
                if (st) spdlog::info("[Vehicle] vehscript: the game says in a vehicle (entity {:x}, type {} {}, seat {}, driver's seat {}: {})", veh, type,
                                     type >= 0 && type <= 7 ? kTypes[type] : "?", seat, dseat, drv == 1 ? "driver" : drv == 0 ? "passenger" : "getting in (seat 0)");
                else spdlog::info("[Vehicle] vehscript: the game says on foot");
            }
            g_veh_id     = st ? veh : 0;
            g_veh_seat   = seat;
            g_veh_dseat  = dseat;
            g_veh_type   = type;
            g_veh_driver = drv;
            g_veh_state  = st;
        }

        void* __fastcall hk_lookup(void* mgr, void** out, uint64_t id)
        {
            void* r = g_lookup_hook.call<void*>(mgr, out, id);
            if (g_watch.load(std::memory_order_relaxed) && out && id != ~0ull && id != 0) {
                const uint8_t* h = static_cast<const uint8_t*>(*out);
                if (h && h != g_base + 0x4CB9E70 && *reinterpret_cast<void* const*>(h + 0x10)) {
                    const size_t k = size_t(((id ^ (id >> 29)) * 0x9E3779B97F4A7C15ull) >> 53) & (kIds - 1);
                    g_ids[k].store(id, std::memory_order_relaxed);
                    g_id_tick[k].store(g_tick_n.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
                g_lookups.fetch_add(1, std::memory_order_relaxed);
            }
            return r;
        }

        // meleedamageprobe (test): the 12 recorded entities nearest a point, with their positions from it.
        void probe_tick()
        {
            g_probe_pending = false;
            if (!g_pos) return;
            const float    px = g_probe_x.load(), py = g_probe_y.load(), pz = g_probe_z.load();
            const uint32_t now = g_tick_n.load();
            struct Near { float d; uint64_t id; float x, y, z; };
            std::vector<Near> v;
            for (int k = 0; k < kIds; ++k) {
                uint64_t id = g_ids[k].load(std::memory_order_relaxed);
                if (id == 0 || now - g_id_tick[k].load(std::memory_order_relaxed) > 120) continue;
                const float x = g_pos(&id, 0) - px, y = g_pos(&id, 1) - py, z = g_pos(&id, 2) - pz;
                v.push_back(Near{ std::sqrt(x * x + y * y + z * z), id, x, y, z });
            }
            std::sort(v.begin(), v.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
            std::string list;
            for (size_t i = 0; i < v.size() && i < 12; ++i) list += fmt::format(" {:x} ({:+.2f}, {:+.2f}, {:+.2f})", v[i].id, v[i].x, v[i].y, v[i].z);
            spdlog::info("[Melee] meleedamageprobe: {} entities recorded in the last 120 updates ({} lookups seen); nearest to ({:.2f}, {:.2f}, {:.2f}):{}", v.size(), g_lookups.load(), px, py,
                         pz, list);
        }

        // On the game's script thread: the recorded entity nearest the body's root (across the ground within 0.7 m, its root within
        // 1.2 m up or down), not the player; then the damage stim from the player to it.
        void damage_tick()
        {
            g_dmg_pending = false;
            if (!g_pos || !g_stim || !g_local_id) return;
            uint64_t       tmp    = 0;
            uint64_t*      idp    = g_local_id(&tmp);
            const uint64_t player = idp ? *idp : 0;
            const float    bx = g_dmg_x.load(), by = g_dmg_y.load(), bz = g_dmg_z.load();
            const uint32_t now = g_tick_n.load();
            uint64_t       best   = 0;
            float          best_d = 1e9f, bpx = 0.0f, bpy = 0.0f, bpz = 0.0f;
            int            looked = 0;
            std::string    near_list;
            for (int k = 0; k < kIds; ++k) {
                uint64_t id = g_ids[k].load(std::memory_order_relaxed);
                if (id == 0 || id == player || now - g_id_tick[k].load(std::memory_order_relaxed) > 120) continue;
                looked++;
                const float x = g_pos(&id, 0), y = g_pos(&id, 1), z = g_pos(&id, 2);
                const float dh = std::sqrt((x - bx) * (x - bx) + (y - by) * (y - by)), dz = z - bz;
                if (dh < 3.0f && near_list.size() < 600) near_list += fmt::format(" {:x} ({:+.2f}, {:+.2f}, {:+.2f}) {:.2f} m", id, x - bx, y - by, dz, dh);
                if (dh < 0.7f && std::abs(dz) < 1.2f && dh < best_d) { best = id; best_d = dh; bpx = x; bpy = y; bpz = z; }
            }
            const int64_t ms = now_ms() - g_dmg_t.load();
            if (!best) {
                g_dmg_missed++;
                spdlog::info("[Melee] meleedamage: no entity at the body's root ({:.2f}, {:.2f}, {:.2f}) among {} recorded ({} ms after the hit); within 3 m:{}", bx, by, bz, looked, ms,
                             near_list.empty() ? std::string{ " none" } : near_list);
                return;
            }
            g_dmg_found++;
            if (!g_dmg_send.load()) {
                spdlog::info("[Melee] meleedamage (find only): entity {:x} at ({:.2f}, {:.2f}, {:.2f}), {:.2f} m from the body's root, {} recorded, {} ms after the hit;{}", best, bpx, bpy, bpz,
                             best_d, looked, ms, near_list);
                return;
            }
            uint64_t    target = best;
            const float h0     = g_health ? g_health(&target) : -1.0f;
            g_stim(&target, player, g_dmg_amount.load());
            const float h1 = g_health ? g_health(&target) : -1.0f;
            g_dmg_sent++;
            g_after_id   = best;
            g_after_tick = g_tick_n.load();
            g_after_h0   = h0;
            spdlog::info("[Melee] meleedamage: {:.0f} damage sent to entity {:x} from the player {:x} ({:.2f} m from the body's root, {} ms after the hit); its health {:.1f} -> {:.1f}",
                         g_dmg_amount.load(), best, player, best_d, ms, h0, h1);
        }

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
            g_tick_n.fetch_add(1, std::memory_order_relaxed);
            if (g_after_id && g_in_play.load() && g_tick_n.load() - g_after_tick >= 30 && g_game_tid.load() == GetCurrentThreadId()) {
                uint64_t id = g_after_id;
                g_after_id  = 0;
                spdlog::info("[Melee] meleedamage: entity {:x}'s health 30 updates later {:.1f} (before the stim {:.1f})", id, g_health ? g_health(&id) : -1.0f, g_after_h0);
            }
            const bool veh_due = (g_veh_on.load() || g_down_on.load()) && now_ms() - g_veh_ask_t >= 150;   // vehscript, downview
            const bool gs_due  = g_gs_on.load() && now_ms() - g_gs_ask_t >= 100;     // gamestate
            if (!g_in_play.load() || (!g_fov_pending.load() && !g_dmg_pending.load() && !g_probe_pending.load() && !g_self_pending.load() && !veh_due && !gs_due)) return;
            const unsigned long script_tid = g_game_tid.load();
            if (script_tid == 0 || script_tid != GetCurrentThreadId()) {
                static int told = 0;
                if (told++ % 600 == 0)
                    spdlog::info("[PlayerFov] waiting: the game update runs on thread {}, its scripts on thread {}; not called", GetCurrentThreadId(), script_tid);
                return;
            }
            if (veh_due) veh_tick();                   // vehscript
            if (gs_due) gs_tick();                     // gamestate
            if (g_dmg_pending.load()) damage_tick();   // meleedamage
            if (g_probe_pending.load()) probe_tick();
            if (g_self_pending.exchange(false) && g_stim && g_local_id) {
                uint64_t  tmp = 0;
                uint64_t* idp = g_local_id(&tmp);
                uint64_t  me  = idp ? *idp : 0;
                if (me != 0 && me != ~0ull) {
                    const float h0 = g_health ? g_health(&me) : -1.0f;
                    g_stim(&me, me, g_self_amount.load());
                    const float h1 = g_health ? g_health(&me) : -1.0f;
                    g_after_id   = me;
                    g_after_tick = g_tick_n.load();
                    g_after_h0   = h0;
                    spdlog::info("[Melee] meleedamageself: {:.0f} damage sent to the player {:x} from itself; health {:.1f} -> {:.1f}", g_self_amount.load(), me, h0, h1);
                }
            }
            if (!g_fov_pending.load()) return;
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

    bool damage_watch(bool on)
    {
        if (!on) {
            g_watch = false;
            return true;
        }
        if (!g_ready) return false;
        if (!g_watch_tried) {
            g_watch_tried = true;
            // The lookup is a jmp into protected code; the two natives start as read on 26 Oct.
            static const uint8_t kPosSig[]{ 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x01 };   // push rbx; sub rsp,40h; mov r8,[rcx]
            static const uint8_t kStimSig[]{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56 };   // mov rax,rsp; mov [rax+10h],rdx; push rbp; push rsi
            const bool lookup_ok = g_base[0xB27D60] == 0xE9;
            const bool pos_ok    = g_base[0x2FA09C0] == 0x40 && std::memcmp(g_base + 0x2FA09C1, kPosSig, sizeof(kPosSig)) == 0;
            const bool stim_ok   = std::memcmp(g_base + 0x2FB6760, kStimSig, sizeof(kStimSig)) == 0;
            if (!lookup_ok || !pos_ok || !stim_ok) {
                spdlog::error("[Melee] meleedamage: unexpected code (entity lookup {}, position {}, damage stim {}): not used", lookup_ok, pos_ok, stim_ok);
                return false;
            }
            g_pos         = reinterpret_cast<PosFn>(g_base + 0x2FA09C0);
            static const uint8_t kHealthSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60 };   // GetPawnHealth's native
            g_health      = std::memcmp(g_base + 0x2FA2900, kHealthSig, sizeof(kHealthSig)) == 0 ? reinterpret_cast<HealthFn>(g_base + 0x2FA2900) : nullptr;
            g_stim        = reinterpret_cast<StimFn>(g_base + 0x2FB6760);
            g_lookup_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0xB27D60), reinterpret_cast<void*>(&hk_lookup));
            g_watch_ok    = static_cast<bool>(g_lookup_hook);
            spdlog::info("[Melee] meleedamage: {}", g_watch_ok ? "the game's entity lookup watched (FC_m64.dll+0xb27d60)" : "the entity lookup could not be hooked: not used");
        }
        g_watch = g_watch_ok;
        return g_watch_ok;
    }

    void queue_damage(float x, float y, float z, float amount, bool send)
    {
        if (!g_watch.load()) return;
        g_dmg_x       = x;
        g_dmg_y       = y;
        g_dmg_z       = z;
        g_dmg_amount  = amount;
        g_dmg_send    = send;
        g_dmg_t       = now_ms();
        g_dmg_pending = true;
    }

    void damage_self(float amount)
    {
        if (!g_watch.load()) return;
        g_self_amount  = amount;
        g_self_pending = true;
    }

    void probe_entities(float x, float y, float z)
    {
        if (!g_watch.load()) return;
        g_probe_x       = x;
        g_probe_y       = y;
        g_probe_z       = z;
        g_probe_pending = true;
    }

    std::string damage_status()
    {
        return fmt::format("lookup watched {} ({} lookups seen), {} hits with an entity found, {} sent, {} without one", g_watch.load(), g_lookups.load(), g_dmg_found.load(),
                           g_dmg_sent.load(), g_dmg_missed.load());
    }

    void vehicle_query(bool on)
    {
        if (!on || !g_ready || g_veh_failed.load()) {
            g_veh_on = false;
            return;
        }
        if (!g_veh_tried) {
            g_veh_tried = true;
            static const uint8_t kVehSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xDA, 0xE8 };   // GetPawnVehicle's native
            static const uint8_t kSeatSig[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 };               // GetPawnVehicleSeat's native
            static const uint8_t kTypeSig[]{ 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x09, 0xE8 };                            // GetPawnVehicleType's native
            static const uint8_t kObjSig[]{ 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC1, 0x48, 0x8D, 0x54, 0x24, 0x58 };   // the vehicle from its id
            const bool veh_ok  = std::memcmp(g_base + 0x2FA2C30, kVehSig, sizeof(kVehSig)) == 0 && rel_target(g_base + 0x2FA2C3C) == g_base + 0x2F8E610;
            const bool seat_ok = std::memcmp(g_base + 0x2FA2C80, kSeatSig, sizeof(kSeatSig)) == 0;
            const bool obj_ok  = std::memcmp(g_base + 0x2F8E8C0, kObjSig, sizeof(kObjSig)) == 0 && g_base[0x1C73890] == 0xE9;
            const bool type_ok = std::memcmp(g_base + 0x2FA2CD0, kTypeSig, sizeof(kTypeSig)) == 0;
            g_veh_ok = veh_ok && seat_ok && obj_ok && type_ok;
            if (g_veh_ok) {
                g_pawn_veh    = reinterpret_cast<PawnVehFn>(g_base + 0x2FA2C30);
                g_pawn_seat   = reinterpret_cast<PawnSeatFn>(g_base + 0x2FA2C80);
                g_veh_obj     = reinterpret_cast<VehObjFn>(g_base + 0x2F8E8C0);
                g_driver_seat = reinterpret_cast<DriverSeatFn>(g_base + 0x1C73890);
                g_pawn_type   = reinterpret_cast<PawnTypeFn>(g_base + 0x2FA2CD0);
                static const uint8_t kStanceSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C };   // GetPawnStance's native
                if (std::memcmp(g_base + 0x2FA2B50, kStanceSig, sizeof(kStanceSig)) == 0) g_stance_fn = reinterpret_cast<PawnIntFn>(g_base + 0x2FA2B50);
                else spdlog::error("[Vehicle] vehscript: unexpected code at GetPawnStance (FC_m64.dll+0x2fa2b50): the stance not read");
                static const uint8_t kAirSig[]{ 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60 };   // IsInAir's native
                if (std::memcmp(g_base + 0x2FA8D20, kAirSig, sizeof(kAirSig)) == 0) g_air_fn = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8D20);
                else spdlog::error("[Vehicle] vehscript: unexpected code at IsInAir (FC_m64.dll+0x2fa8d20): in the air not read");
                spdlog::info("[Vehicle] vehscript: GetPawnVehicle (FC_m64.dll+0x2fa2c30) asked every 150 ms on the game's script thread");
            } else {
                spdlog::error("[Vehicle] vehscript: unexpected code (GetPawnVehicle {}, its seat {}, the driver's seat {}, its type {}): not used", veh_ok, seat_ok, obj_ok, type_ok);
            }
        }
        g_veh_on = g_veh_ok;
    }

    int vehicle_state(int& driver, int& type)
    {
        driver = -1;
        type   = -1;
        if (!g_veh_on.load() || now_ms() - g_veh_t.load() > 1000) return -1;
        const int st = g_veh_state.load();
        if (st >= 0) driver = g_veh_driver.load();
        if (st == 1) type = g_veh_type.load();
        return st;
    }

    void vehicle_seat(int& seat, int& dseat, uint64_t& id)
    {
        const bool ok = g_veh_on.load() && now_ms() - g_veh_t.load() <= 1000 && g_veh_state.load() == 1;
        seat  = ok ? g_veh_seat.load() : -1;
        dseat = ok ? g_veh_dseat.load() : -1;
        id    = ok ? g_veh_id.load() : 0;
    }

    void down_query(bool on)
    {
        if (!on || !g_ready || g_down_failed.load()) {
            g_down_on    = false;
            g_down_state = -1;
            return;
        }
        if (!g_down_tried) {
            g_down_tried = true;
            static const uint8_t kDownSig[]{ 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54 };   // IsPawnDown's native
            g_down_ok = std::memcmp(g_base + 0x2FA9900, kDownSig, sizeof(kDownSig)) == 0;
            if (g_down_ok) {
                g_down_fn = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA9900);
                spdlog::info("[Down] downview: IsPawnDown (FC_m64.dll+0x2fa9900) asked every 150 ms on the game's script thread");
            } else {
                spdlog::error("[Down] downview: unexpected code at IsPawnDown (FC_m64.dll+0x2fa9900): not used (the view as before when knocked down)");
            }
        }
        g_down_on = g_down_ok;
    }

    int pawn_down()
    {
        if (!g_down_on.load() || now_ms() - g_down_t.load() > 1000) return -1;
        return g_down_state.load();
    }

    int pawn_stance()
    {
        if (!g_veh_on.load() || !g_stance_fn || now_ms() - g_stance_t.load() > 1000) return -1;
        return g_stance.load();
    }

    int pawn_in_air()
    {
        if (!g_veh_on.load() || !g_air_fn || now_ms() - g_air_t.load() > 1000) return -1;
        return g_air.load();
    }

    bool down_failed() { return g_down_failed.load() || (g_down_tried && !g_down_ok); }

    std::string vehicle_status()
    {
        return fmt::format("vehscript {}{}: {} answers, the game says {} (entity {:x}, type {}, seat {}, driver's seat {}), last answer {} ms ago", g_veh_on.load() ? "asking" : "not asking",
                           g_veh_failed.load() ? " (failed)" : "", g_veh_queries.load(),
                           g_veh_state.load() < 0 ? "nothing yet" : g_veh_state.load() ? (g_veh_driver.load() == 1 ? "in a vehicle, driving" : g_veh_driver.load() == 0 ? "in a vehicle, riding" : "in a vehicle, getting in") : "on foot",
                           g_veh_id.load(), g_veh_type.load(), g_veh_seat.load(), g_veh_dseat.load(), g_veh_t.load() ? now_ms() - g_veh_t.load() : -1);
    }

    void gamestate_query(bool on)
    {
        if (!on || !g_ready || g_gs_failed.load()) {
            g_gs_on = false;
            return;
        }
        if (!g_gs_tried) {
            g_gs_tried = true;
            if (!g_veh_tried) {   // GetPawnVehicle and the seats: vehscript's checks and pointers, its own switch left as it is
                const bool was = g_veh_on.load();
                vehicle_query(true);
                g_veh_on = was;
            }
            // The natives' first bytes as read on 30 Oct (research\2026-10-30_engine_natives.md).
            struct Sig { uint32_t rva; std::initializer_list<uint8_t> b; const char* name; };
            const Sig sigs[]{
                { 0x2FA2CD0, { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x09, 0xE8 }, "GetPawnVehicleType" },
                { 0x2FA2B50, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C }, "GetPawnStance" },
                { 0x2FA8E80, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 }, "IsInQTE" },
                { 0x2FA9900, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54 }, "IsPawnDown" },
                { 0x2FA97D0, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x58 }, "IsPawnDead" },
                { 0x2FA8D20, { 0x40, 0x53, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x60 }, "IsInAir" },
                { 0x2FA8F90, { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0xE8 }, "IsInWater" },
                { 0x2FA2F40, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x48 }, "GetPlayerFOV" },
                { 0x2FA3670, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x01, 0x48, 0x8D, 0x54, 0x24, 0x48 }, "GetPlayerNearFOV" },
                { 0x2FA2AE0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x09, 0x48, 0x8B, 0xDA }, "GetPawnRidingEntity" },
                { 0x2FA8580, { 0x48, 0x8B, 0x05, 0x71, 0xD4, 0xF0, 0x01, 0x0F, 0xB6, 0x40, 0x68, 0xC3 }, "IsGamePaused" },
            };
            std::string bad;
            for (const auto& s : sigs)
                if (std::memcmp(g_base + s.rva, s.b.begin(), s.b.size()) != 0) bad += fmt::format(" {}", s.name);
            g_gs_ok = bad.empty() && g_veh_ok;
            if (g_gs_ok) {
                g_gs_vtype   = reinterpret_cast<PawnIntFn>(g_base + 0x2FA2CD0);
                g_gs_stance  = reinterpret_cast<PawnIntFn>(g_base + 0x2FA2B50);
                g_gs_qte     = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8E80);
                g_gs_down    = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA9900);
                g_gs_dead    = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA97D0);
                g_gs_air     = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8D20);
                g_gs_water   = reinterpret_cast<PawnBoolFn>(g_base + 0x2FA8F90);
                g_gs_fov     = reinterpret_cast<PawnFloatFn>(g_base + 0x2FA2F40);
                g_gs_nearfov = reinterpret_cast<PawnFloatFn>(g_base + 0x2FA3670);
                g_gs_ride    = reinterpret_cast<PawnVehFn>(g_base + 0x2FA2AE0);
                g_gs_pause   = reinterpret_cast<uint8_t**>(g_base + 0x4EB59F8);   // IsGamePaused's global (mov rax,[rip+0x1F0D471])
                spdlog::info("[GameState] gamestate: the player's state from the game's script functions every 100 ms on its script thread, the pause byte every frame");
            } else {
                spdlog::error("[GameState] gamestate: unexpected code ({}{}): not used", bad.empty() ? "" : "changed:", bad.empty() ? " GetPawnVehicle" : bad);
            }
        }
        if (g_gs_ok && !g_watch_hooks[0] && !g_bars_hook) {
            int hooked = 0;
            watch_install(std::make_integer_sequence<int, kWatchN>{}, g_base, hooked);
            if (g_base[0x1476B40] == 0xE9) g_bars_hook = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1476B40), reinterpret_cast<void*>(&hk_bars));
            spdlog::info("[GameState] gamestate: {} of {} cinematic and camera script functions watched, black bars {}", hooked, kWatchN, g_bars_hook ? "watched" : "not watched");
        }
        g_gs_on = g_gs_ok;
    }

    void gamestate_frame(bool mod_menu, bool mod_cine, bool mod_vehicle)
    {
        if (!g_gs_on.load() || !g_gs_pause) return;
        const int p = gs_pause_read();
        if (p == g_gs_pause_last) return;
        if (p == -2) {
            g_gs_on     = false;
            g_gs_failed = true;
            spdlog::error("[GameState] a fault reading the pause byte: gamestate turned off");
            return;
        }
        spdlog::info("[GameState] pause byte {} -> {} (the mod: menu screen {}, cinematic {}, vehicle {})", g_gs_pause_last, p, mod_menu, mod_cine, mod_vehicle);
        g_gs_pause_last = p;
    }

    std::string gamestate_status()
    {
        return fmt::format("gamestate {}{}: {} reads, {} changes logged, pause byte {}, black bars {}, {} cinematic/camera script calls; last: {}", g_gs_on.load() ? "on" : "off",
                           g_gs_failed.load() ? " (failed)" : "", g_gs_reads.load(), g_gs_changes.load(), g_gs_pause_last, g_bars.load(), g_watch_calls.load(), g_gs_have ? gs_text(g_gs_last) : std::string{ "none" });
    }

    std::string status()
    {
        return fmt::format("{}; game script thread {} ({} script calls seen), game update thread {}, SetPlayerFOV calls {}",
                           g_ready ? "ready" : (g_tried ? "not usable" : "not looked for yet"), g_game_tid.load(), g_game_calls.load(), g_tick_tid.load(), g_fov_calls.load());
    }
}
