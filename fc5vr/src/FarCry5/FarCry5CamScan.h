#pragma once

// Research probe: where does the game keep its camera position in CPU memory? (The shot origin for
// firing from the barrel.) scan() finds every exact copy of a position in writable memory; track()
// then keeps the copies that follow the camera as it moves.

#include <cstdint>
#include <vector>

namespace fc5::camscan
{
    // Full scan for the 3 floats (exact) in committed read-write memory. Returns the number found.
    size_t scan(const float pos[3]);
    // Ranges the next scans skip (the mod's own copies of what is scanned for, e.g. its kept skinning palettes).
    void exclude(const void* p, size_t bytes);
    void clear_excludes();
    // Called each frame with the current camera position while tracking: counts which candidates
    // still hold it.
    void track(const float pos[3]);
    // Logs the candidates that matched in at least min_share of the tracked frames, with the 64 bytes
    // around each one.
    void report(float min_share);
    size_t candidates();
    // Hardware write watches (DR0-3, all threads) on up to 4 of the live copies (indices in the
    // report list); unwatch() logs which code wrote them, from which thread.
    void watch(const std::vector<int>& live_indices, unsigned long render_tid, bool reads = false);
    // The same on addresses (e.g. module base + offset).
    void watch_addresses(const std::vector<void*>& addrs, unsigned long render_tid, bool reads);
    void unwatch();
    // Research: record only accesses made by the thread inside a marked window (set_window_thread(tid) at the
    // window's start, 0 at its end).
    void set_window_only(bool on);
    void set_window_thread(unsigned long tid);
    // Research: an int32 value in memory (e.g. the ammo count): scan, narrow on the next value, watch writes.
    void int_scan(int32_t v);
    void int_next(int32_t v);
    void int_set(int index, int32_t v);
    void int_watch(const std::vector<int>& indices, unsigned long render_tid);
    // Disassembly of count instructions at an address (bddisasm), to the log.
    void disasm(uint8_t* at, int count);
}
