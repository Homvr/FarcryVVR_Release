#pragma once

#include <windows.h>
#include <Xinput.h>

#include <atomic>
#include <cstdint>
#include <string>

// Headset-test probes: what each stage of the controller -> game chain did, logged once a second
// ([Probe]) and in full on the snapshot key (F6). Counters reset at each report.
namespace fc5::probe
{
    // One XInput state built for the game: by the virtual pad's update (from_pad) or returned by a
    // hooked XInputGetState the game called (blocked = passed through untouched).
    void note_xinput(bool from_pad, bool blocked, uint32_t index, uint32_t ret_in, uint32_t ret_out, const XINPUT_GAMEPAD* g);
    void note_vigem(int error, const XINPUT_GAMEPAD& g);
    std::string xinput_line();
    std::string front_window();
}
