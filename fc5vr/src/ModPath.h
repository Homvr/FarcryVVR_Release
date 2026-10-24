#pragma once

#include <string>
#include <windows.h>

// The mod's own files (settings, launcher choice, commands, headset sizes) live next to FarCry5.exe, where the log goes
// and the launcher writes. The game's working folder is not always that folder (Homard 18 Oct: a tester's game outside
// Steam never read fc5vr_settings.txt or fc5vr_launcher.txt), so they are opened by full path.
namespace fc5 {
inline const std::string& exe_dir()
{
    static const std::string dir = [] {
        char        p[MAX_PATH]{};
        const DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
        std::string s(p, n);
        const auto  cut = s.find_last_of("\\/");
        return cut == std::string::npos ? std::string{} : s.substr(0, cut + 1);
    }();
    return dir;
}
inline std::string mod_file(const char* name) { return exe_dir() + name; }
}
