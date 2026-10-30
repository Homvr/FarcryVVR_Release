#pragma once

// The game's own SetPlayerFOV, called by the mod for the local player (see FarCry5Lua.cpp). No script text is run.

#include <cstdint>
#include <string>

namespace fc5::lua
{
    // From the frame hook: finds the game's code once; in_play gates the call.
    void frame_tick(bool in_play);
    // SetPlayerFOV(angle deg, blend s, override) for the local player, made at the next game camera update on the game's
    // script thread.
    void queue_fov(float angle, float blend, bool flag);
    std::string status();
    // meleedamage (FarCry5Lua.cpp): on: the game's entity lookup watched (ids recorded); queue_damage: at the next game update the
    // recorded entity nearest (x, y, z) (a body's root, world) is found and, with send, given amount of damage from the player.
    bool        damage_watch(bool on);
    void        queue_damage(float x, float y, float z, float amount, bool send);
    std::string damage_status();
    void        damage_self(float amount);                    // meleedamageself (test): the damage stim sent to the player itself
    void        probe_entities(float x, float y, float z);   // meleedamageprobe (test): the recorded entities nearest a point, logged
    // vehscript (FarCry5Lua.cpp): on: the game's GetPawnVehicle asked for the local player every 150 ms on its script thread.
    // vehicle_state: -1 unknown (off, failed, or no answer in the last second), 0 on foot, 1 in a vehicle; driver -1 unknown, 0, 1;
    // type (in a vehicle): 0 wheeled, 1 boat, 2 paraglider, 3 motorcycle, 4 helicopter, 5 plane, 6 wingsuit, else -1.
    void        vehicle_query(bool on);
    int         vehicle_state(int& driver, int& type);
    std::string vehicle_status();
    // vehdrivegun (log only): the game's seat, its driver's seat and the vehicle entity from the last answer (-1, -1, 0 when on foot or unknown).
    void        vehicle_seat(int& seat, int& dseat, uint64_t& id);
    // downview: on: the game's IsPawnDown asked for the player in the same 150 ms script-thread poll (with or without vehscript).
    // pawn_down: -1 unknown (off, failed, no answer in the last second), 0, 1. down_failed: a fault or unexpected code (off for the session).
    void        down_query(bool on);
    int         pawn_down();
    bool        down_failed();
    // GetPawnStance for the player while vehscript asks (same poll): 0 standing, 1 crouched, 4 downed, 18 seated in a vehicle; -1 unknown.
    int         pawn_stance();
    // chutemin: IsInAir for the player while vehscript asks (same poll): 1 a jump or free fall, 0 on the ground or under an open chute, -1 unknown.
    int         pawn_in_air();
    // gamestate (log only, off by default, saved; F11 Comfort > Cinematics and vehicles): the player's state from the game's own script functions (vehicle and its type,
    // seat, riding, stance, QTE, down, dead, in air, in water, FOV, near FOV) every 100 ms on the script thread, and the pause byte
    // every frame; a [GameState] line on each change, the pause line with the mod's own menu/cinematic/vehicle state beside it.
    void        gamestate_query(bool on);
    void        gamestate_frame(bool mod_menu, bool mod_cine, bool mod_vehicle);
    std::string gamestate_status();
}
