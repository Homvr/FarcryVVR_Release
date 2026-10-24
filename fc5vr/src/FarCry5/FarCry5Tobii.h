#pragma once

// The game's Tobii "Extended View": the drawn (and culled) camera turns by an offset while the aim stays
// where the mouse put it. The game reads the offset from Tobii.GameIntegration.dll through delay-import
// slots in FC_m64.dll; the mod writes its own functions into the per-frame slots so the offset can come
// from the headset instead of an eye tracker (culling report 2026-10-10, section 5.3 A1).

#include <cstdint>

namespace fc5::tobii
{
    // Writes the mod's functions into the per-frame Tobii slots (once). Returns false when FC_m64.dll
    // or a slot is not where expected.
    bool install();
    bool installed();
    // The offset handed to the game (radians; yaw positive = left, as the game's own yaw).
    void set_angles(float yaw, float pitch);
    // Logs the slots, how often the game called each stub, and the game's Tobii manager fields.
    void status();
    // The game's manager object (from the first argument of GetUpdateRate), or null before the first call.
    uint8_t* manager();
    // A manager float (checked: null manager or unreadable memory -> false).
    bool read_float(int off, float& v);
    // The time step reported to the game (seconds per update).
    void set_step(float seconds);
    // Calls of the game's per-frame Tobii update since install.
    uint64_t updates();
    // Game Tobii updates started so far (update k is number k, from 0; the one in progress is seq() - 1).
    uint64_t seq();
    // The offset update k stored (+0x6C yaw, +0x70 pitch, radians), known once update k + 1 has started; false when gone.
    bool applied(uint64_t k, float& yaw, float& pitch);
    // The offset handed to update k (set_angles), radians.
    bool handed(uint64_t k, float& yaw, float& pitch);
    // Writes a byte or float into the manager (discovery: enable flags, fade target).
    void poke_byte(int off, uint8_t v);
    void poke_float(int off, float v);
    // Called at the start of each game Tobii update, on the game's own thread (the game's camera update): fc5::lua uses it
    // to call the game's natives and scripts where the game itself would.
    void set_game_tick(void (*fn)());
}
