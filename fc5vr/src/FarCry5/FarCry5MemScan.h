#pragma once
// memscan (test): a value scan of the game's memory (FarCry5MemScan.cpp).
#include <istream>
#include <string>

namespace fc5::memscan
{
    bool command(const std::string& cmd, const std::string& arg, std::istream& rest);
}
