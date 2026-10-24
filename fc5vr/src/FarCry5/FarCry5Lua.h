#pragma once

// The game's own SetPlayerFOV, called by the mod for the local player (see FarCry5Lua.cpp). No script text is run.

#include <string>

namespace fc5::lua
{
    // From the frame hook: finds the game's code once; in_play gates the call.
    void frame_tick(bool in_play);
    // SetPlayerFOV(angle deg, blend s, override) for the local player, made at the next game camera update on the game's
    // script thread.
    void queue_fov(float angle, float blend, bool flag);
    std::string status();
}
