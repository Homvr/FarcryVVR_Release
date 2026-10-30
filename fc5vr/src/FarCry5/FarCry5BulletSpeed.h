#pragma once

// Bullet speed (FarCry5BulletSpeed.cpp): research tools for where the game keeps a flying bullet's velocity and its source.
// Scans run on their own thread (results logged when done).

#include <cstdint>
#include <vector>

namespace fc5::bullet
{
    // Every float3 in writable memory with a size between lo and hi (m/s) pointing along dir (cosine >= mindot).
    void scan(const float dir[3], float lo, float hi, float mindot);
    // The candidates whose value changed since the scan (or the last next()), logged.
    void next();
    // Write watches (camscan's DR0-3) on up to 4 velocity candidates; camunwatch logs the writers.
    void watch(const std::vector<int>& idx, unsigned long render_tid);
    // A float value: every address holding it exactly; next() keeps those still holding it (list: probes/fltscan.txt).
    void flt_scan(float value);
    void flt_next(float value, bool list);
    void flt_watch(const std::vector<int>& idx, unsigned long render_tid, bool reads);
    bool busy();

    // bulletspeed: the speed of fired bullets times k (1-4), set in each weapon's projectile record (FC_m64+0x1e10d90 hook).
    void  install_speed_hook(uint8_t* fc_m64_base);
    void  set_speed_scale(float k);
    float speed_scale();
    bool  speed_available();
    void  speed_status();
    void  note_player_fire();   // the mod's fire gate, each frame the player's trigger is down
    // The mod let the trigger through to the game (press_ms after the press): the next shot code call logs how long the game took.
    void  note_shot_release(uint64_t press_ms, uint64_t weapon);
    // shotbuzz (FarCry5Stereo::shot_buzz): called on the game thread at each of the player's shots, from the shot code above (the
    // player's trigger down within 500 ms, and the weapon that fired the press's first shot, so enemies' shots are left out).
    void  set_player_shot_callback(void (*cb)());

    // FC2 VR item 2 (23 Oct): the weapon's fire function (FC_m64+0x1e37110) right after its protected call [vtbl+0x278] fills the shot's
    // origin [rbp-0x58] and direction [rbp-0x68] (hook at +0x1e375fa, bytes checked); the projectile spawn and the shot event read them.
    //  shotprobe N: the next N calls logged ([ShotProbe]). bulletaim / bulletorigin (shots rewritten along the barrel) were removed with
    //  F11 Experimental > Research for alternatives (Homard 30 Oct).
    bool install_shot_hook(uint8_t* fc_m64_base);
    void shot_probe(int n);
    // Each present: the game camera (position, forward) and the barrel (direction, drawn muzzle), world space, z up.
    void set_view(const float cam[3], const float fwd[3], const float aim[3], const float muzzle[3], bool aim_valid, bool muzzle_at_wall);
}
