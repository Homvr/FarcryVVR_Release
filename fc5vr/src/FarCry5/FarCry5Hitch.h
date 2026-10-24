#pragma once

// hitchlog N (test command, not saved; hitch thread 23 Oct): one "[Hitch]" line for each of the next N stereo pairs longer than
// hitchms (default 35 ms, start to start), giving the CPU time of the game call and of the extra call, and inside each the time in
// the present hook (each mod's on_present, the swap chain's Present, each mod's on_post_present), xrWaitFrame, xrBeginFrame,
// xrEndFrame, the eye copies, the mod's own present-side blocks and blocking read-backs, plus that pair's GPU times once they
// arrive and what the frame was (on foot / vehicle, moving, turning, weapon, menu). The counters are running totals read before
// and after each call: a few atomics per call, always on; the lines only while hitchlog counts down.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace fc5::hitch
{
    // The mod's own blocks (microsecond totals).
    // The hook groups (draws and dispatches, state setters and clears, Map / Unmap / UpdateSubresource, shader and layout creation
    // on the render thread and on other threads) are timed only while hitchlog is on (g_active), the original call included.
    enum Block : int { kHudWorld = 0, kCompose, kOverlays, kAimMarch, kPairCheck, kPollCommands, kStats5s, kSyncRead,
                       kDrawHooks, kStateHooks, kMapHooks, kCreateRender, kCreateOther,
                       kMapOrig, kScanMatrices, kObjConsts, kViewportConsts, kFixLights, kBlockCount };   // inside the Map / Unmap hooks
    inline std::atomic<int64_t>  g_block_us[kBlockCount]{};
    inline std::atomic<uint32_t> g_sync_reads{ 0 };   // blocking Map read-backs done (indirect args, buffer reads)
    inline std::atomic<uint32_t> g_creates{ 0 };      // shaders and input layouts created
    inline std::atomic<bool>     g_active{ false };
    inline std::atomic<uint32_t> g_render_tid{ 0 };

    struct Timer
    {
        Block                                 b;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit Timer(Block blk) : b(blk) {}
        ~Timer() { g_block_us[b] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
    };

    struct HookTimer
    {
        Block                                 b;
        bool                                  on;
        std::chrono::steady_clock::time_point t0{};
        explicit HookTimer(Block blk) : b(blk), on(g_active.load(std::memory_order_relaxed)) { if (on) t0 = std::chrono::steady_clock::now(); }
        ~HookTimer() { if (on) g_block_us[b] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
    };
    // Shader / layout creation: counted, and timed by the thread it runs on.
    Block create_block();

    // What the frame was, filled by the mod at the pair's start.
    struct State
    {
        bool     vehicle = false, menu = false, cinematic = false, weapon = false, scope_call = false;
        uint32_t weapon_sig = 0;
        float    cam[3]{};       // game camera position (m)
        float    fwd[3]{};       // game camera forward
        float    head_q[4]{};    // headset left-eye rotation x y z w
    };

    // One per stereo pair, from the frame hook: the pair's start (before the game call), around the game call, around the
    // extra call, and its end. id is the pair's GPU timing id.
    void pair_start(uint64_t id, const State& s);
    void game_call_begin();
    void game_call_end();
    void extra_call_begin();
    void extra_call_end();
    void mark_scope_call();   // the pair had the scope pass's third call

    void set_count(int n);          // hitchlog N
    void set_csv(int n);            // hitchcsv N
    bool active();
    void set_threshold(float ms);   // hitchms M
    int  count();
    float threshold();
}
