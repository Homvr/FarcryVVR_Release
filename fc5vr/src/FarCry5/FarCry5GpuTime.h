#pragma once

// GPU time per stage of a stereo pair (pipeline review P1): a disjoint query and begin/end timestamps per
// stage, read back a few pairs later without waiting (D3D11_ASYNC_GETDATA_DONOTFLUSH). Mean and 99th
// percentile per stage logged every 5 s. Off by default (cmd gputime, F11 Debug > Pipeline improvements).

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace fc5::gputime
{
    enum Stage : int { kPair = 0, kGameCall, kExtraCall, kDlssPrep, kDlssEval, kCompose, kAim, kScopeCall, kCount };

    void set_enabled(bool on);
    bool enabled();
    // Around one stereo pair (both game calls), on the render thread. id (hitchlog): the pair's times can be asked for by it
    // once read (the last 32 pairs are kept).
    void begin_pair(ID3D11DeviceContext* ctx, uint64_t id = 0);
    bool pair_result(uint64_t id, double ms[kCount], bool seen[kCount]);
    void end_pair(ID3D11DeviceContext* ctx);
    // A stage inside the pair; stages may repeat (once per eye): their times add up per pair.
    void begin(ID3D11DeviceContext* ctx, Stage s);
    void end(ID3D11DeviceContext* ctx, Stage s);
    // Collects finished pairs; logs every 5 s. Returns the last summary line (for the menu).
    void poll(ID3D11DeviceContext* ctx);
    std::string summary();
}
