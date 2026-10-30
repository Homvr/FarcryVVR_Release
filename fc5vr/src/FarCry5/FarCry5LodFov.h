#pragma once
// lodfov (FarCry5LodFov.cpp): far detail as at the flat FOV, through the game's render config read from its archive.
#include <string>

namespace fc5::lodfov
{
    // Early (before the game reads its render config): reads the lodfov line of fc5vr_settings.txt, builds the changed
    // config and hooks the file reads. Off: nothing is hooked.
    void install_early();
    // lodfov off|on|<scale>: saved; used at the next game start.
    void set(const std::string& arg);
    float       scale();          // 0 = off
    std::string describe();       // for the log and the F11 menu
}
