#include "FarCry5CamScan.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <spdlog/spdlog.h>
#include <bddisasm.h>

#include <algorithm>
#include <atomic>
#include <tuple>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fc5::camscan
{
    namespace
    {
        struct Cand
        {
            float*   p;
            uint32_t hits;
            bool     image;
        };
        std::vector<Cand>   g_cands;
        std::vector<float*> g_live;
        uint32_t            g_frames = 0;
        std::vector<std::pair<uintptr_t, uintptr_t>> g_excl;   // [begin, end) skipped by scan

        bool excluded(const void* p)
        {
            const auto a = reinterpret_cast<uintptr_t>(p);
            for (const auto& [b, e] : g_excl) if (a >= b && a < e) return true;
            return false;
        }

        bool readable_rw(DWORD protect)
        {
            if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
            const DWORD p = protect & 0xff;
            return p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY;
        }

        std::string module_of(const void* p)
        {
            HMODULE m = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(p), &m) || !m) return "heap";
            char name[MAX_PATH]{};
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* b = strrchr(name, '\\');
            return fmt::format("{}+0x{:x}", b ? b + 1 : name, reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(m));
        }

        bool read16(const float* p, float out[16])
        {
            __try {
                for (int k = 0; k < 16; ++k) out[k] = p[k];
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool matches(const float* p, const float pos[3])
        {
            __try {
                return p[0] == pos[0] && p[1] == pos[1] && p[2] == pos[2];
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        size_t scan_region(uint8_t* base, size_t size, const float pos[3], bool image)
        {
            size_t found = 0;
            __try {
                const uint32_t* w  = reinterpret_cast<const uint32_t*>(base);
                const size_t    n  = size / 4;
                uint32_t        k0 = 0, k1 = 0, k2 = 0;
                std::memcpy(&k0, &pos[0], 4);
                std::memcpy(&k1, &pos[1], 4);
                std::memcpy(&k2, &pos[2], 4);
                for (size_t i = 0; i + 2 < n; ++i) {
                    if (w[i] == k0 && w[i + 1] == k1 && w[i + 2] == k2) {
                        if (excluded(base + i * 4)) continue;
                        if (g_cands.size() < 200000) g_cands.push_back({ reinterpret_cast<float*>(base + i * 4), 0, image });
                        ++found;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            return found;
        }

        // Hardware write breakpoints (debug registers DR0..DR3, every thread of the process).
        struct Hit
        {
            uint32_t slot;
            DWORD    tid;
            uint64_t rip;
            uint64_t ret[8];
            bool operator<(const Hit& o) const { return std::tie(slot, rip, ret[0], ret[1]) < std::tie(o.slot, o.rip, o.ret[0], o.ret[1]); }
        };
        std::atomic<bool>       g_bp_on{ false };
        std::atomic<int>        g_bp_budget{ 0 };
        void*                   g_bp_addr[4]{};
        PVOID                   g_veh = nullptr;
        bool                    g_rw  = false;   // watch reads too (condition 11) instead of writes only
        std::mutex              g_hits_mtx;
        std::map<Hit, uint32_t> g_hits;
        DWORD                   g_render_tid = 0;
        // Window filter (research): only accesses by the thread inside a marked code window are recorded.
        std::atomic<bool>       g_window_only{ false };
        std::atomic<DWORD>      g_window_tid{ 0 };

        bool read_u64(const uint64_t* p, uint64_t& v)
        {
            __try {
                v = *p;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep)
        {
            if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_bp_on.load()) return EXCEPTION_CONTINUE_SEARCH;
            const DWORD64 dr6 = ep->ContextRecord->Dr6;
            if ((dr6 & 0xf) == 0) return EXCEPTION_CONTINUE_SEARCH;
            if (g_window_only.load() && GetCurrentThreadId() != g_window_tid.load()) {
                ep->ContextRecord->Dr6 = 0;   // outside the window: not recorded, no budget used
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            Hit h{};
            for (uint32_t s = 0; s < 4; ++s) if (dr6 & (1ull << s)) h.slot = s;
            h.tid = GetCurrentThreadId();
            h.rip = ep->ContextRecord->Rip;
            // The caller chain: the first return addresses on the stack that point into a module's code.
            const uint64_t* sp = reinterpret_cast<const uint64_t*>(ep->ContextRecord->Rsp);
            int             n  = 0;
            for (int k = 0; k < 160 && n < 8; ++k) {
                uint64_t v = 0;
                if (!read_u64(sp + k, v)) break;
                HMODULE m = nullptr;
                if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(v), &m) && m) {
                    h.ret[n++] = v;
                }
            }
            {
                std::lock_guard _{ g_hits_mtx };
                g_hits[h]++;
            }
            ep->ContextRecord->Dr6 = 0;
            if (--g_bp_budget <= 0) {
                // Enough samples: this thread stops watching (the others when cleared).
                ep->ContextRecord->Dr7 = 0;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        void apply_to_threads(bool on)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return;
            THREADENTRY32 te{ sizeof(te) };
            const DWORD   pid = GetCurrentProcessId(), self = GetCurrentThreadId();
            int           done = 0;
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!th) continue;
                const bool other = te.th32ThreadID != self;
                if (other) SuspendThread(th);
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &c)) {
                    c.Dr7 = 0;
                    if (on) {
                        c.Dr0 = reinterpret_cast<DWORD64>(g_bp_addr[0]);
                        c.Dr1 = reinterpret_cast<DWORD64>(g_bp_addr[1]);
                        c.Dr2 = reinterpret_cast<DWORD64>(g_bp_addr[2]);
                        c.Dr3 = reinterpret_cast<DWORD64>(g_bp_addr[3]);
                        for (int s = 0; s < 4; ++s) {
                            if (!g_bp_addr[s]) continue;
                            // Local enable, condition 01 = write, length 11 = 4 bytes.
                            c.Dr7 |= (1ull << (s * 2)) | ((g_rw ? 0b11ull : 0b01ull) << (16 + s * 4)) | (0b11ull << (18 + s * 4));
                        }
                    }
                    c.Dr6 = 0;
                    if (SetThreadContext(th, &c)) ++done;
                }
                if (other) ResumeThread(th);
                CloseHandle(th);
            }
            CloseHandle(snap);
            spdlog::info("[CamScan] write watch {} on {} threads", on ? "set" : "cleared", done);
        }
    }

    size_t scan(const float pos[3])
    {
        g_cands.clear();
        g_frames = 0;
        if (pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) return 0;
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* a = reinterpret_cast<uint8_t*>(0x10000);
        size_t   bytes = 0, regions = 0, heap = 0, image = 0;
        while (VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if (mbi.State == MEM_COMMIT && readable_rw(mbi.Protect) && mbi.RegionSize < (size_t(1) << 32)) {
                const bool img = mbi.Type == MEM_IMAGE;
                const size_t f = scan_region(static_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize, pos, img);
                (img ? image : heap) += f;
                bytes += mbi.RegionSize;
                ++regions;
            }
            a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (reinterpret_cast<uintptr_t>(a) >= 0x7FFFFFFF0000ull) break;
        }
        spdlog::info("[CamScan] ({:.3f}, {:.3f}, {:.3f}): {} copies ({} in heaps, {} in module data) in {} regions, {} MB", pos[0], pos[1], pos[2], g_cands.size(), heap, image,
                     regions, bytes >> 20);
        return g_cands.size();
    }

    void track(const float pos[3])
    {
        if (g_cands.empty()) return;
        ++g_frames;
        for (auto& c : g_cands) {
            if (matches(c.p, pos)) ++c.hits;
        }
    }

    void report(float min_share)
    {
        g_live.clear();
        std::ofstream out{ "D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/camscan.txt" };
        int logged = 0;
        for (const auto& c : g_cands) {
            // Every candidate's count, also below the share (the first 50).
            if (logged < 50) {
                const bool kept = g_frames != 0 && c.hits >= uint32_t(min_share * g_frames);
                spdlog::info("[CamScan] candidate {} {} hits {}/{}{}", static_cast<void*>(c.p), module_of(c.p), c.hits, g_frames, kept ? fmt::format(" (listed as {})", g_live.size()) : std::string{ " (below the share)" });
                ++logged;
            }
            if (g_frames == 0 || c.hits < uint32_t(min_share * g_frames)) continue;
            float       v[16]{};
            std::string around = read16(c.p - 8, v) ? std::string{} : std::string{ "(unreadable)" };
            if (around.empty()) for (float f : v) around += fmt::format("{:.6g} ", f);
            out << fmt::format("{} {} {} hits {}/{} | {}\n", g_live.size(), static_cast<void*>(c.p), module_of(c.p), c.hits, g_frames, around);
            g_live.push_back(c.p);
        }
        spdlog::info("[CamScan] {} of {} copies followed the camera in at least {:.0f}% of {} frames (list: probes/camscan.txt)", g_live.size(), g_cands.size(),
                     min_share * 100.0f, g_frames);
    }

    size_t candidates() { return g_cands.size(); }

    void exclude(const void* p, size_t bytes)
    {
        const auto a = reinterpret_cast<uintptr_t>(p);
        g_excl.emplace_back(a, a + bytes);
    }
    void clear_excludes() { g_excl.clear(); }

    void watch(const std::vector<int>& live_indices, DWORD render_tid, bool reads)
    {
        g_rw = reads;
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, &on_exception);
        g_render_tid = render_tid;
        {
            std::lock_guard _{ g_hits_mtx };
            g_hits.clear();
        }
        for (int s = 0; s < 4; ++s) g_bp_addr[s] = nullptr;
        int s = 0;
        for (int i : live_indices) {
            if (s < 4 && i >= 0 && i < (int)g_live.size()) g_bp_addr[s++] = g_live[i];
        }
        g_bp_budget = reads ? 60000 : 4000;
        g_bp_on     = true;
        apply_to_threads(true);
    }

    void watch_addresses(const std::vector<void*>& addrs, DWORD render_tid, bool reads)
    {
        g_live.clear();
        std::vector<int> idx;
        for (void* a : addrs) { idx.push_back((int)g_live.size()); g_live.push_back(static_cast<float*>(a)); }
        watch(idx, render_tid, reads);
    }

    void set_window_only(bool on) { g_window_only = on; }
    void set_window_thread(unsigned long tid) { g_window_tid = tid; }

    void unwatch()
    {
        apply_to_threads(false);
        g_bp_on = false;
        std::lock_guard _{ g_hits_mtx };
        std::vector<std::pair<uint32_t, Hit>> v;
        for (const auto& [h, n] : g_hits) v.emplace_back(n, h);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        int shown = 0;
        for (const auto& [n, h] : v) {
            if (shown++ >= 60) break;
            std::string chain;
            for (uint64_t r : h.ret) if (r) chain += " <- " + module_of(reinterpret_cast<void*>(r));
            spdlog::info("[CamScan] watch {} ({}) {} {}x by {} thread {} at {}{}", h.slot, g_bp_addr[h.slot], g_rw ? "accessed" : "written", n, h.tid == g_render_tid ? "render" : "other",
                         h.tid, module_of(reinterpret_cast<void*>(h.rip)), chain);
        }
        spdlog::info("[CamScan] watch done: {} distinct writers", v.size());
    }
}

namespace fc5::camscan
{
    namespace
    {
        std::vector<int32_t*> g_ints;

        void int_region(uint8_t* base, size_t size, int32_t v)
        {
            __try {
                const int32_t* w = reinterpret_cast<const int32_t*>(base);
                for (size_t i = 0, n = size / 4; i < n; ++i) {
                    if (w[i] == v && g_ints.size() < 4000000) g_ints.push_back(const_cast<int32_t*>(w + i));
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        bool read_i32(const int32_t* p, int32_t& v)
        {
            __try {
                v = *p;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool read_code(const uint8_t* p, uint8_t* out, size_t n)
        {
            __try {
                for (size_t i = 0; i < n; ++i) out[i] = p[i];
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
    }

    void int_scan(int32_t v)
    {
        g_ints.clear();
        MEMORY_BASIC_INFORMATION mbi{};
        uint8_t* a = reinterpret_cast<uint8_t*>(0x10000);
        while (VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            if (mbi.State == MEM_COMMIT && readable_rw(mbi.Protect) && mbi.RegionSize < (size_t(1) << 32)) int_region(static_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize, v);
            a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (reinterpret_cast<uintptr_t>(a) >= 0x7FFFFFFF0000ull) break;
        }
        spdlog::info("[IntScan] value {}: {} addresses", v, g_ints.size());
    }

    void int_next(int32_t v)
    {
        std::vector<int32_t*> keep;
        for (auto* p : g_ints) {
            int32_t x = 0;
            if (read_i32(p, x) && x == v) keep.push_back(p);
        }
        g_ints.swap(keep);
        spdlog::info("[IntScan] still {}: {} addresses", v, g_ints.size());
        for (size_t i = 0; i < g_ints.size() && i < 40; ++i) spdlog::info("[IntScan]   {} {} {}", i, static_cast<void*>(g_ints[i]), module_of(g_ints[i]));
    }

    void int_watch(const std::vector<int>& indices, DWORD render_tid)
    {
        std::vector<void*> addrs;
        for (int i : indices) if (i >= 0 && i < (int)g_ints.size()) addrs.push_back(g_ints[i]);
        watch_addresses(addrs, render_tid, false);
    }

    void int_set(int index, int32_t v)
    {
        if (index < 0 || index >= (int)g_ints.size()) return;
        int32_t old = 0;
        if (!read_i32(g_ints[index], old)) return;
        DWORD prot = 0;
        if (VirtualProtect(g_ints[index], 4, PAGE_READWRITE, &prot)) {
            *g_ints[index] = v;
            VirtualProtect(g_ints[index], 4, prot, &prot);
        }
        spdlog::info("[IntScan] {} {}: {} -> {}", index, static_cast<void*>(g_ints[index]), old, v);
    }

    void disasm(uint8_t* at, int count)
    {
        uint8_t* p = at;
        for (int i = 0; i < count; ++i) {
            uint8_t code[16]{};
            if (!read_code(p, code, sizeof(code))) { spdlog::info("[Disasm] {} unreadable", module_of(p)); return; }
            INSTRUX ix{};
            if (!ND_SUCCESS(NdDecodeEx(&ix, code, sizeof(code), ND_CODE_64, ND_DATA_64))) { spdlog::info("[Disasm] {} (bad: {:02x})", module_of(p), code[0]); ++p; continue; }
            char text[ND_MIN_BUF_SIZE]{};
            NdToText(&ix, reinterpret_cast<ND_UINT64>(p), sizeof(text), text);
            std::string target;
            if (ix.Category == ND_CAT_CALL || ix.Category == ND_CAT_UNCOND_BR || ix.Category == ND_CAT_COND_BR) {
                if (ix.Operands[0].Type == ND_OP_OFFS) target = " -> " + module_of(p + ix.Length + ix.Operands[0].Info.RelativeOffset.Rel);
            }
            spdlog::info("[Disasm] {:<24} {}{}", module_of(p), text, target);
            p += ix.Length;
        }
    }
}
