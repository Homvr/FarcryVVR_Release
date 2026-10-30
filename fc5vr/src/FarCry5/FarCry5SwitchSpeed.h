#pragma once
// switchfast: the game's own weapon draw and holster animations made shorter (FarCry5SwitchSpeed.cpp).
#include <functional>
#include <iosfwd>
#include <string>

namespace fc5::switchspeed
{
    // switchfast on|off, switchdraw S, switchholster S, switchrescan S (saved); switchscan, switchlist N (tests).
    bool command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist);
    // Once per frame on the render thread: starts the first scan, keeps the written values and rescans when due (no memory work here).
    void frame();
    // A weapon switch's time from the gesture to the new weapon drawn (FarCry5WeaponSwitch.cpp): a slow one asks for a rescan.
    void note_switch(float secs);
    // The load popup (FarCry5LoadPopup.cpp): 2 = a scan running (secs since it started), 1 = one due after a load, a slow switch or
    // a request (secs until it may start), 3 = one due only because no weapon was found yet, 0 = none; took = the latest scan's time.
    int scan_state(float& secs, float& took);
    // F11 Experimental > Weapon switch (test): its rows; commands go into cmds.
    void draw_menu(std::string& cmds, const std::function<void(const char*)>& hint);
}
