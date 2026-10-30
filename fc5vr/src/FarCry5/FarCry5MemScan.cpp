// memscan (test): a value scan of the game's own memory, to find where the game keeps the selected throwable (Homard 26 Oct: "go into
// the game memory and find out which throwables are which models"). The scan runs on its own thread over the committed, writable
// private memory, read with ReadProcessMemory (a page freed meanwhile is skipped, never a fault).
//   memscan u32|u16|u8|f32 V   new scan: every aligned value equal to V (u32 / f32: 4-byte aligned, u16: 2, u8: 1)
//   memscan next V             keep those equal to V now
//   memscan changed|unchanged  keep those that changed / did not change since the last step
//   memscan list [N]           log the first N (64) with their value now and the bytes around them
//   memscan near ADDR [BYTES]  log BYTES (256) around an address as dwords
//   memscan watch ADDR         log the dword at ADDR whenever it changes (one address, 10 times a second); watch off
//   memscan snapa / snapb      the whole writable memory (private and the modules' data) snapa: kept in memory (a byte per dword); snapb: the dwords still equal marked
//   memscan diff               new hit list: the dwords still from snapa to snapb (idle) that differ now.
//                              Each hit keeps its value per state: state 1 = now (state 0 is not a value)
//   memscan snap               the values now kept as the next state;  memscan same K / differ K: keep the hits whose value now
//                              equals / differs from state K's
//   memscan f32group A B C [BYTES]  floats A with B and C within BYTES; f32near ADDR [BYTES]; f32set ADDR V (write)
#include "FarCry5MemScan.h"

#include <windows.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <climits>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fc5::memscan
{
namespace
{
    enum class Type { U8, U16, U32, F32, U64 };
    struct Hit { uintptr_t a; uint64_t v; };

    std::mutex        g_mtx;
    std::vector<Hit>  g_hits;
    Type              g_type = Type::U32;
    std::atomic<bool> g_busy{ false };
    std::atomic<uintptr_t> g_watch{ 0 };
    std::atomic<bool> g_watch_thread{ false };
    constexpr size_t  kMaxHits = 30'000'000;
    // Per hit, its value in each state (diff, snap), in step with g_hits; cleared by a new scan.
    constexpr int     kStates = 12;
    std::vector<std::array<uint32_t, kStates>> g_hist;
    int               g_nstates = 0;

    int  width(Type t) { return t == Type::U8 ? 1 : t == Type::U16 ? 2 : t == Type::U64 ? 8 : 4; }
    uint64_t read_v(const uint8_t* p, Type t)
    {
        uint64_t v = 0;
        std::memcpy(&v, p, width(t));
        return v;
    }
    uint64_t encode(Type t, double val)
    {
        if (t == Type::F32) { float f = float(val); uint32_t u; std::memcpy(&u, &f, 4); return u; }
        return uint64_t(int64_t(val));
    }
    bool readable(const MEMORY_BASIC_INFORMATION& m)
    {
        if (m.State != MEM_COMMIT || (m.Type != MEM_PRIVATE && m.Type != MEM_IMAGE)) return false;
        if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        return (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }
    bool read(uintptr_t a, void* out, size_t n)
    {
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
    }

    // want: an exact value; with lo < hi a range (u64 pointer scans: every qword pointing into [lo, hi]).
    void scan_new(Type t, uint64_t want, uint64_t lo = 0, uint64_t hi = 0)
    {
        std::vector<Hit> hits;
        std::vector<uint8_t> buf(1 << 20);
        const int w = width(t);
        size_t regions = 0, bytes = 0;
        bool   capped  = false;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            if (readable(m)) {
                regions++;
                for (uintptr_t c = base; c < end && !capped; c += buf.size()) {
                    const size_t n = std::min<size_t>(buf.size(), end - c);
                    if (!read(c, buf.data(), n)) continue;
                    bytes += n;
                    for (size_t o = 0; o + w <= n; o += w) {
                        const uint64_t v = read_v(buf.data() + o, t);
                        if (lo < hi ? (v >= lo && v <= hi) : v == want) {
                            hits.push_back({ c + o, v });
                            if (hits.size() >= kMaxHits) { capped = true; break; }
                        }
                    }
                }
            }
            a = end;
        }
        std::lock_guard _{ g_mtx };
        g_hits = std::move(hits);
        g_type = t;
        g_hist.clear();
        g_nstates = 0;
        spdlog::info("[MemScan] new scan: {} regions, {:.0f} MB read, {} hits{}", regions, bytes / 1048576.0, g_hits.size(), capped ? " (capped: scan for a rarer value)" : "");
    }

    // mode 0: equal to want, 1: changed, 2: unchanged, 3: same as state want, 4: differs from state want, 5: kept as a new state
    void scan_filter(int mode, uint64_t want)
    {
        std::lock_guard _{ g_mtx };
        const int        w = width(g_type);
        std::vector<Hit> keep;
        std::vector<std::array<uint32_t, kStates>> keep_h;
        const bool hist = g_hist.size() == g_hits.size() && g_nstates > 0;
        for (size_t i = 0; i < g_hits.size(); ++i) {
            const auto& h = g_hits[i];
            uint8_t b[8]{};
            if (!read(h.a, b, w)) continue;
            const uint64_t v = read_v(b, g_type);
            bool ok = (mode == 0 && v == want) || (mode == 1 && v != h.v) || (mode == 2 && v == h.v) || mode == 5;
            if (mode == 3 || mode == 4) ok = hist && want < uint64_t(g_nstates) && ((mode == 3) == (uint32_t(v) == g_hist[i][want]));
            if (!ok) continue;
            keep.push_back({ h.a, v });
            if (hist) {
                keep_h.push_back(g_hist[i]);
                if (mode == 5 && g_nstates < kStates) keep_h.back()[g_nstates] = uint32_t(v);
            }
        }
        static const char* names[] = { "next", "changed", "unchanged", "same", "differ", "snap" };
        spdlog::info("[MemScan] {}{}: {} -> {} hits", names[mode], mode == 3 || mode == 4 ? fmt::format(" {}", want) : "", g_hits.size(), keep.size());
        g_hits = std::move(keep);
        g_hist = std::move(keep_h);
        if (mode == 5) {
            if (!hist) spdlog::info("[MemScan] snap: only after a diff");
            else if (g_nstates < kStates) spdlog::info("[MemScan] state {} kept", g_nstates++);
            else spdlog::info("[MemScan] no room for another state ({})", kStates);
        }
    }

    // A snapshot of the whole writable memory kept in memory, no file (Homard 27 Oct: nothing on C:; D: wrote at 60-150 MB/s): one
    // byte per dword (the dword's four bytes xored: any change of one byte changes it). snapa takes it; snapb marks the dwords
    // still equal to it (1 bit each); diff keeps the still ones that differ now. Chunks of up to 1 MB, snapa's list reused.
    struct Chunk { uintptr_t a; uint32_t n; bool ok; uint64_t bit; };
    std::vector<Chunk>    g_chunks;
    std::vector<uint8_t>  g_snap;
    std::vector<uint64_t> g_still;
    inline uint8_t fold(uint32_t v) { return uint8_t(v ^ v >> 8 ^ v >> 16 ^ v >> 24); }
    // f(chunk, snapshot bytes, live dwords) over every chunk readable at each pass
    template <class F> void snap_pass(F&& f)
    {
        std::vector<uint8_t> L(1 << 20);
        for (auto& c : g_chunks) {
            if (!c.ok || !read(c.a, L.data(), c.n)) { c.ok = false; continue; }
            f(c, g_snap.data() + c.bit, reinterpret_cast<const uint32_t*>(L.data()));
        }
    }
    void snap_a()
    {
        const DWORD t0 = GetTickCount();
        g_chunks.clear();
        g_snap  = {};
        g_still = {};
        uint64_t bit = 0;
        for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
            const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
            if (readable(m))
                for (uintptr_t c = base; c < end; c += 1 << 20) {
                    const uint32_t n = uint32_t(std::min<uintptr_t>(1 << 20, end - c));
                    g_chunks.push_back({ c, n, true, bit });
                    bit += n / 4;
                }
            a = end;
        }
        // (allocated after the list: the snapshot's own memory is not in it)
        try { g_snap.assign(size_t(bit), 0); } catch (...) { g_chunks.clear(); spdlog::info("[MemScan] snapa: no memory for {} MB", bit >> 20); return; }
        uint64_t good = 0;
        snap_pass([&](const Chunk& c, uint8_t* s, const uint32_t* l) {
            for (uint32_t i = 0; i < c.n / 4; ++i) s[i] = fold(l[i]);
            good += c.n;
        });
        spdlog::info("[MemScan] snapshot a: {} chunks, {:.0f} MB looked at, {:.0f} MB kept, {:.1f} s", g_chunks.size(), good / 1048576.0, bit / 1048576.0, (GetTickCount() - t0) / 1000.0);
    }
    void snap_b()
    {
        const DWORD t0 = GetTickCount();
        if (g_snap.empty()) { spdlog::info("[MemScan] snapb: snapa first"); return; }
        g_still.assign(size_t((g_snap.size() + 63) / 64), 0);
        uint64_t still = 0;
        snap_pass([&](const Chunk& c, uint8_t* s, const uint32_t* l) {
            for (uint32_t i = 0; i < c.n / 4; ++i)
                if (s[i] == fold(l[i])) { const uint64_t b = c.bit + i; g_still[b >> 6] |= 1ull << (b & 63); still++; }
        });
        spdlog::info("[MemScan] snapshot b: {} dwords still since a, {:.1f} s", still, (GetTickCount() - t0) / 1000.0);
    }
    void snap_diff()
    {
        const DWORD t0 = GetTickCount();
        if (g_still.empty()) { spdlog::info("[MemScan] diff: snapa and snapb needed"); return; }
        std::vector<Hit> hits;
        std::vector<std::array<uint32_t, kStates>> hist;
        bool capped = false;
        snap_pass([&](const Chunk& c, uint8_t* s, const uint32_t* l) {
            for (uint32_t i = 0; i < c.n / 4 && !capped; ++i) {
                const uint64_t b = c.bit + i;
                if (!(g_still[b >> 6] >> (b & 63) & 1) || fold(l[i]) == s[i]) continue;
                hits.push_back({ c.a + 4 * i, l[i] });
                std::array<uint32_t, kStates> st{};
                st[1] = l[i];
                hist.push_back(st);
                capped = hits.size() >= 10'000'000;
            }
        });
        g_snap  = {};
        g_still = {};
        std::lock_guard _{ g_mtx };
        g_hits    = std::move(hits);
        g_hist    = std::move(hist);
        g_type    = Type::U32;
        g_nstates = 2;
        spdlog::info("[MemScan] diff: {} hits (still from a to b, changed now){}, {:.1f} s; state 1 kept (state 0, the snapshot's, is not a value)", g_hits.size(), capped ? " (capped)" : "", (GetTickCount() - t0) / 1000.0);
    }

    std::string module_of(uintptr_t a)
    {
        HMODULE mod = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(a), &mod) || !mod) return {};
        char name[MAX_PATH]{};
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* s = std::strrchr(name, '\\');
        return fmt::format("{}+{:#x}", s ? s + 1 : name, a - reinterpret_cast<uintptr_t>(mod));
    }

    void log_near(uintptr_t a, size_t bytes)
    {
        const uintptr_t s = (a - bytes / 2) & ~uintptr_t(15);
        for (uintptr_t r = s; r < s + bytes; r += 16) {
            uint32_t d[4]{};
            if (!read(r, d, 16)) continue;
            spdlog::info("[MemScan]   {:#x}: {:08x} {:08x} {:08x} {:08x}{}", r, d[0], d[1], d[2], d[3], (a >= r && a < r + 16) ? "  <" : "");
        }
    }

    template <class F> void run(F&& f)
    {
        if (g_busy.exchange(true)) { spdlog::info("[MemScan] busy: the last step is still running"); return; }
        std::thread([f = std::forward<F>(f)]() mutable { f(); g_busy = false; }).detach();
    }
}

bool command(const std::string& cmd, const std::string& arg, std::istream& rest)
{
    if (cmd != "memscan") return false;
    std::string v;
    rest >> v;
    const double val = v.empty() ? 0.0 : std::atof(v.c_str());
    if (arg == "u32" || arg == "u16" || arg == "u8" || arg == "f32") {
        const Type t = arg == "u8" ? Type::U8 : arg == "u16" ? Type::U16 : arg == "f32" ? Type::F32 : Type::U32;
        const uint64_t want = encode(t, val);
        spdlog::info("[MemScan] new {} scan for {}", arg, v);
        run([t, want] { scan_new(t, want); });
    } else if (arg == "str") {
        // memscan str TEXT: where an ASCII text is (logged, up to 256 places; the hit list is not changed)
        std::string text = v, more;
        while (rest >> more) text += " " + more;
        run([text] {
            std::vector<uint8_t> buf(1 << 20);
            int found = 0;
            for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && found < 256;) {
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
                const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
                if (readable(m)) {
                    for (uintptr_t c = base; c < end && found < 256; c += buf.size() - text.size()) {
                        const size_t n = std::min<size_t>(buf.size(), end - c);
                        if (n < text.size() || !read(c, buf.data(), n)) continue;
                        for (size_t o = 0; o + text.size() <= n && found < 256; ++o)
                            if (std::memcmp(buf.data() + o, text.data(), text.size()) == 0) {
                                // The game's name strings (the inventory's): two lengths and a count before the text.
                                uint32_t hdr[3]{};
                                const bool named = read(c + o - 12, hdr, 12) && hdr[0] == text.size() && hdr[1] == text.size();
                                spdlog::info("[MemScan] '{}' at {:#x}{}", text, c + o, named ? fmt::format(" (name header at {:#x})", c + o - 12) : "");
                                found++;
                            }
                        if (n < buf.size()) break;
                    }
                }
                a = end;
            }
            spdlog::info("[MemScan] '{}': {} places", text, found);
        });
    } else if (arg == "entry") {
        // memscan entry IDHEX: the inventory entries of an item id: the id (u32) followed by a pointer to a name header (two equal
        // lengths, then the text); logged with the name.
        const uint32_t id = uint32_t(std::strtoul(v.c_str(), nullptr, 16));
        run([id] {
            std::vector<uint8_t> buf(1 << 20);
            int found = 0;
            for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && found < 64;) {
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
                const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
                if (readable(m)) {
                    for (uintptr_t c = base; c < end && found < 64; c += buf.size()) {
                        const size_t n = std::min<size_t>(buf.size(), end - c);
                        if (!read(c, buf.data(), n)) continue;
                        for (size_t o = 0; o + 12 <= n && found < 64; o += 4) {
                            uint32_t v32;
                            std::memcpy(&v32, buf.data() + o, 4);
                            if (v32 != id) continue;
                            uint64_t p;
                            std::memcpy(&p, buf.data() + o + 4, 8);
                            uint32_t hdr[3]{};
                            char     name[64]{};
                            if (p < 0x10000 || !read(p, hdr, 12) || hdr[0] == 0 || hdr[0] > 60 || hdr[0] != hdr[1] || !read(p + 12, name, hdr[0])) continue;
                            spdlog::info("[MemScan] entry of {:#x} at {:#x}: name '{}' (header {:#x})", id, c + o, std::string(name, hdr[0]), p);
                            found++;
                        }
                    }
                }
                a = end;
            }
            spdlog::info("[MemScan] entries of {:#x}: {}", id, found);
        });
    } else if (arg == "range") {
        // memscan range u32|u64 LO HI (hex): every aligned value within [LO, HI]
        std::string lo_s, hi_s;
        rest >> lo_s >> hi_s;
        const Type     t  = v == "u64" ? Type::U64 : Type::U32;
        const uint64_t lo = std::strtoull(lo_s.c_str(), nullptr, 16), hi = std::strtoull(hi_s.c_str(), nullptr, 16);
        spdlog::info("[MemScan] new {} range scan {:#x}..{:#x}", v, lo, hi);
        run([t, lo, hi] { scan_new(t, 0, lo, hi); });
    } else if (arg == "ptrnear") {
        // memscan ptrnear ADDR RANGE: every qword pointing within RANGE (hex, default 0x400) bytes of ADDR (hex)
        const uint64_t a = std::strtoull(v.c_str(), nullptr, 16);
        std::string r;
        rest >> r;
        const uint64_t range = r.empty() ? 0x400 : std::strtoull(r.c_str(), nullptr, 16);
        spdlog::info("[MemScan] new pointer scan into {:#x} +- {:#x}", a, range);
        run([a, range] { scan_new(Type::U64, 0, a - range, a + range); });
    } else if (arg == "f32group") {
        // memscan f32group A B C [BYTES]: new hit list: each float A with a float B and a float C within BYTES (64) after or before it
        // (lodfov: the game's geometry config, KillLodScale 0.5 / MinZoomFactor 0.1 / LodScale 0.5 close together); logged with their offsets.
        std::string b_s, c_s, r_s;
        rest >> b_s >> c_s >> r_s;
        // Compared as bits: the build's fast floating point takes a NaN in memory as equal to anything.
        const uint32_t fa = uint32_t(encode(Type::F32, val)), fb = uint32_t(encode(Type::F32, std::atof(b_s.c_str()))),
                       fc = uint32_t(encode(Type::F32, std::atof(c_s.c_str())));
        const int   range = r_s.empty() ? 64 : std::clamp(std::atoi(r_s.c_str()), 4, 1024);
        spdlog::info("[MemScan] new f32 group scan {:#x} {:#x} {:#x} within {} bytes", fa, fb, fc, range);
        run([fa, fb, fc, range] {
            std::vector<Hit> hits;
            std::vector<uint8_t> buf(1 << 20);
            const uintptr_t self_buf = reinterpret_cast<uintptr_t>(buf.data());
            const HMODULE   game     = GetModuleHandleA("FC_m64.dll");
            for (uintptr_t a = 0x10000; a < 0x7FFFFFFF0000ull && hits.size() < 4096;) {
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &m, sizeof(m))) break;
                const uintptr_t base = reinterpret_cast<uintptr_t>(m.BaseAddress), end = base + m.RegionSize;
                // The game's heap and FC_m64.dll only (other modules' constant tables, and this scan's own buffer, left out)
                const bool other_module = m.Type == MEM_IMAGE && m.AllocationBase != reinterpret_cast<PVOID>(game);
                if (readable(m) && !other_module && !(self_buf >= base && self_buf < end)) {
                    for (uintptr_t c = base; c < end && hits.size() < 4096; c += 1 << 20) {
                        const size_t n = std::min<size_t>(1 << 20, end - c);
                        if (!read(c, buf.data(), n)) continue;
                        const auto* f = reinterpret_cast<const uint32_t*>(buf.data());
                        const int   w = range / 4;
                        for (size_t i = 0; i < n / 4; ++i) {
                            if (f[i] != fa) continue;
                            int ob = INT_MIN, oc = INT_MIN;
                            for (int k = -w; k <= w; ++k) {
                                const int64_t j = int64_t(i) + k;
                                if (k == 0 || j < 0 || j >= int64_t(n / 4)) continue;
                                if (ob == INT_MIN && f[j] == fb) ob = k * 4;
                                else if (oc == INT_MIN && f[j] == fc) oc = k * 4;
                            }
                            if (ob == INT_MIN || oc == INT_MIN) continue;
                            hits.push_back({ c + 4 * i, 0 });
                            if (hits.size() <= 200) spdlog::info("[MemScan]   {:#x} {}: B at {:+d}, C at {:+d}", c + 4 * i, module_of(c + 4 * i), ob, oc);
                        }
                    }
                }
                a = end;
            }
            std::lock_guard _{ g_mtx };
            g_hits = std::move(hits);
            g_type = Type::F32;
            g_hist.clear();
            g_nstates = 0;
            spdlog::info("[MemScan] f32 group: {} places", g_hits.size());
        });
    } else if (arg == "f32set") {
        // memscan f32set ADDR V: write a float (test; lodfov's search). Logs the value before.
        const uintptr_t a = std::strtoull(v.c_str(), nullptr, 16);
        float nv = 0.0f, old = 0.0f;
        rest >> nv;
        SIZE_T done = 0;
        if (!read(a, &old, 4) || !WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), &nv, 4, &done) || done != 4)
            spdlog::info("[MemScan] f32set {:#x}: not writable", a);
        else
            spdlog::info("[MemScan] f32set {:#x}: {} -> {}", a, old, nv);
    } else if (arg == "f32near") {
        // memscan f32near ADDR [BYTES]: the floats around an address (BYTES 128 each side)
        const uintptr_t a = std::strtoull(v.c_str(), nullptr, 16);
        int n = 128;
        rest >> n;
        n = std::clamp(n, 16, 2048) & ~3;
        for (intptr_t o = -n; o < n; o += 16) {
            float f[4]{};
            if (!read(a + o, f, 16)) continue;
            uint32_t u[4];
            std::memcpy(u, f, 16);
            spdlog::info("[MemScan]   {:+5d} {:#x}: {:10.4f} {:10.4f} {:10.4f} {:10.4f}  ({:08x} {:08x} {:08x} {:08x})", int(o), a + o, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
        }
    } else if (arg == "next") {
        const uint64_t want = g_type == Type::U64 ? std::strtoull(v.c_str(), nullptr, 16) : encode(g_type, val);
        run([want] { scan_filter(0, want); });
    } else if (arg == "changed" || arg == "unchanged") {
        const int mode = arg == "changed" ? 1 : 2;
        run([mode] { scan_filter(mode, 0); });
    } else if (arg == "snapa" || arg == "snapb") {
        if (arg == "snapa") run([] { snap_a(); });
        else run([] { snap_b(); });
    } else if (arg == "diff") {
        run([] { snap_diff(); });
    } else if (arg == "snap") {
        run([] { scan_filter(5, 0); });
    } else if (arg == "same" || arg == "differ") {
        const int      mode = arg == "same" ? 3 : 4;
        const uint64_t k    = uint64_t(val);
        run([mode, k] { scan_filter(mode, k); });
    } else if (arg == "list") {
        const size_t n = v.empty() ? 64 : size_t(val);
        std::lock_guard _{ g_mtx };
        spdlog::info("[MemScan] {} hits, the first {}:", g_hits.size(), std::min(n, g_hits.size()));
        for (size_t i = 0; i < g_hits.size() && i < n; ++i) {
            uint8_t b[8]{};
            read(g_hits[i].a, b, width(g_type));
            const uint64_t now = read_v(b, g_type);
            std::string st;
            if (g_hist.size() == g_hits.size())
                for (int k = 0; k < g_nstates; ++k) st += fmt::format(" {:x}", g_hist[i][k]);
            spdlog::info("[MemScan]   {:#x} = {} ({:#x}) {}{}", g_hits[i].a, now, now, module_of(g_hits[i].a), st.empty() ? "" : " | states" + st);
        }
    } else if (arg == "near") {
        const uintptr_t a = std::strtoull(v.c_str(), nullptr, 16);
        size_t n = 256;
        rest >> n;
        spdlog::info("[MemScan] around {:#x}{}:", a, module_of(a).empty() ? "" : " (" + module_of(a) + ")");
        log_near(a, std::clamp<size_t>(n, 16, 4096));
    } else if (arg == "watch") {
        if (v == "off" || v.empty()) { g_watch = 0; return true; }
        g_watch = std::strtoull(v.c_str(), nullptr, 16);
        if (!g_watch_thread.exchange(true)) {
            std::thread([] {
                uint32_t last = 0;
                uintptr_t was = 0;
                for (;;) {
                    Sleep(100);
                    const uintptr_t a = g_watch.load();
                    if (!a) { was = 0; continue; }
                    uint32_t d = 0;
                    if (!read(a, &d, 4)) continue;
                    if (a != was || d != last) spdlog::info("[MemScan] watch {:#x} = {} ({:#x})", a, d, d);
                    last = d;
                    was  = a;
                }
            }).detach();
        }
    } else {
        spdlog::info("[MemScan] memscan u32|u16|u8|f32 V, next V, changed, unchanged, list [N], near ADDR [BYTES], watch ADDR|off, snapa, snapb, diff, snap, same K, differ K");
    }
    return true;
}
}
