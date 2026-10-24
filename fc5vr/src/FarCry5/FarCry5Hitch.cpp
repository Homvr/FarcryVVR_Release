#include "FarCry5Hitch.h"
#include "FarCry5GpuTime.h"

#include <D3D11Hook.hpp>
#include <Mods.hpp>
#include <mods/vr/runtimes/OpenXR.hpp>
#include <spdlog/spdlog.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace fc5::hitch
{
    namespace
    {
        using clock = std::chrono::steady_clock;

        // Mods by their place in the list (ModConfig.cpp: VRConfig, FarCry5Stereo, VR).
        constexpr int kModFc5 = 1, kModVr = 2;

        enum Counter : int {
            cOnPresent = 0, cPresentFc5, cPresentVr, cDxgi, cPostPresent, cPostFc5, cPostVr, cXrWait, cXrBegin, cXrEnd, cEyeCopies,
            cBlocks, cSyncReads = cBlocks + kBlockCount, cCreates, cCount
        };
        struct Counters
        {
            int64_t v[cCount]{};
            Counters operator-(const Counters& o) const
            {
                Counters r;
                for (int i = 0; i < cCount; ++i) r.v[i] = v[i] - o.v[i];
                return r;
            }
        };

        Counters snap()
        {
            Counters c;
            c.v[cOnPresent]   = D3D11Hook::s_fc5_on_present_us.load(std::memory_order_relaxed);
            c.v[cPresentFc5]  = Mods::s_fc5_present_us[kModFc5].load(std::memory_order_relaxed);
            c.v[cPresentVr]   = Mods::s_fc5_present_us[kModVr].load(std::memory_order_relaxed);
            c.v[cDxgi]        = D3D11Hook::s_fc5_dxgi_us.load(std::memory_order_relaxed);
            c.v[cPostPresent] = D3D11Hook::s_fc5_post_present_us.load(std::memory_order_relaxed);
            c.v[cPostFc5]     = Mods::s_fc5_post_present_us[kModFc5].load(std::memory_order_relaxed);
            c.v[cPostVr]      = Mods::s_fc5_post_present_us[kModVr].load(std::memory_order_relaxed);
            c.v[cXrWait]      = runtimes::OpenXR::s_fc5_wait_us.load(std::memory_order_relaxed);
            c.v[cXrBegin]     = runtimes::OpenXR::s_fc5_begin_us.load(std::memory_order_relaxed);
            c.v[cXrEnd]       = runtimes::OpenXR::s_fc5_end_us.load(std::memory_order_relaxed);
            c.v[cEyeCopies]   = runtimes::OpenXR::s_fc5_copy_us.load(std::memory_order_relaxed);
            for (int b = 0; b < kBlockCount; ++b) c.v[cBlocks + b] = g_block_us[b].load(std::memory_order_relaxed);
            c.v[cSyncReads] = g_sync_reads.load(std::memory_order_relaxed);
            c.v[cCreates]   = g_creates.load(std::memory_order_relaxed);
            return c;
        }

        double ms(int64_t us) { return double(us) / 1000.0; }
        double ms(clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

        // One call's parts: the present hook (mods' on_present with the mod's and VR's share, the swap chain's Present, mods'
        // on_post_present), the runtime calls inside those, the mod's blocks, the blocking read-backs.
        std::string parts(const Counters& c)
        {
            const char* kBlockNames[kBlockCount]{ "hud", "compose", "overlays", "aim", "paircheck", "cmds", "stats", "reads", "draw hooks", "state hooks", "map hooks", "creates here", "creates elsewhere", "(map driver", "scan", "objects", "viewport", "lights)" };
            std::string s = fmt::format("on_present {:.2f} (mod {:.2f}, VR {:.2f}), Present {:.2f}, post {:.2f} (mod {:.2f}, VR {:.2f}); xrWait {:.2f}, xrBegin {:.2f}, xrEnd {:.2f}, copies {:.2f};",
                                        ms(c.v[cOnPresent]), ms(c.v[cPresentFc5]), ms(c.v[cPresentVr]), ms(c.v[cDxgi]), ms(c.v[cPostPresent]), ms(c.v[cPostFc5]),
                                        ms(c.v[cPostVr]), ms(c.v[cXrWait]), ms(c.v[cXrBegin]), ms(c.v[cXrEnd]), ms(c.v[cEyeCopies]));
            for (int b = 0; b < kBlockCount; ++b) s += fmt::format(" {} {:.2f}", kBlockNames[b], ms(c.v[cBlocks + b]));
            s += fmt::format(" ({} read-backs, {} shaders/layouts created)", c.v[cSyncReads], c.v[cCreates]);
            return s;
        }

        struct Pair
        {
            uint64_t   id = 0;
            clock::time_point start{}, t_game0{}, t_game1{}, t_extra0{}, t_extra1{};
            Counters   c_start, c_game0, c_game1, c_extra0, c_extra1;
            bool       game = false, extra = false;
            State      st;
            double     speed = 0.0, view_turn = 0.0, head_turn = 0.0;   // m/s, deg/s, deg/s over the previous pair
            // Filled once the pair is over.
            double     pair_ms = 0.0;
            Counters   c_total;
            int        age = 0;
        };

        std::atomic<int>   g_count{ 0 };   // lines per 5 s window, 0 off
        std::atomic<float> g_threshold{ 35.0f };
        std::atomic<int>   g_csv_left{ 0 };   // hitchcsv N: one compact line for each of the next N pairs
        Pair               g_cur;
        bool               g_have_cur = false;
        Pair               g_pending[8];
        int                g_pending_n = 0;
        // The 5 s summary.
        clock::time_point g_window = clock::now();
        int               g_lines = 0, g_pairs = 0, g_slow = 0, g_unlogged = 0;
        double            g_longest = 0.0;
        Counters          g_slow_sum;     // over the slow pairs of the window
        double            g_slow_game = 0.0, g_slow_extra = 0.0, g_slow_outside = 0.0;

        float angle_deg(const float a[3], const float b[3])
        {
            const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (la < 1e-6f || lb < 1e-6f) return 0.0f;
            const float d = std::clamp((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb), -1.0f, 1.0f);
            return std::acos(d) * 57.2957795f;
        }
        float quat_angle_deg(const float a[4], const float b[4])
        {
            const float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
            if (d < 1e-6f) return 0.0f;
            return 2.0f * std::acos(std::min(1.0f, d)) * 57.2957795f;
        }

        void print(const Pair& p, const double* gpu, const bool* seen)
        {
            const double game  = p.game ? ms(p.t_game1 - p.t_game0) : 0.0;
            const double extra = p.extra ? ms(p.t_extra1 - p.t_extra0) : 0.0;
            const Counters cg  = p.game ? p.c_game1 - p.c_game0 : Counters{};
            const Counters ce  = p.extra ? p.c_extra1 - p.c_extra0 : Counters{};
            const Counters co  = p.c_total - cg - ce;
            std::string g;
            if (gpu) {
                const char* kNames[gputime::kCount]{ "pair", "L", "R", "DLSS inputs", "DLSS eval", "compose", "aim", "scope" };
                for (int s = 0; s < gputime::kCount; ++s)
                    if (seen[s]) g += fmt::format("{}{} {:.2f}", g.empty() ? "" : ", ", kNames[s], gpu[s]);
            } else {
                g = "not read";
            }
            const auto& st = p.st;
            spdlog::info("[Hitch] pair {}: {:.1f} ms (game call {:.2f}, extra call {:.2f}{}, outside the calls {:.2f}) | GPU {} | {}{}{}, {:.1f} m/s, view turn {:.0f} deg/s, head turn {:.0f} deg/s, {}{}"
                         " | game call: {} | extra call: {} | outside: {}",
                         p.id, p.pair_ms, game, extra, p.extra ? "" : " (single)", p.pair_ms - game - extra, g, st.menu ? "menu" : (st.vehicle ? "vehicle" : "on foot"),
                         st.cinematic ? ", cinematic" : "", st.scope_call ? ", scope pass" : "", p.speed, p.view_turn, p.head_turn,
                         st.weapon ? "weapon drawn" : "no weapon", st.weapon ? fmt::format(" (sig {})", st.weapon_sig) : std::string{}, parts(cg), parts(ce), parts(co));
        }

        void flush_pending(bool all)
        {
            int keep = 0;
            for (int i = 0; i < g_pending_n; ++i) {
                Pair& p = g_pending[i];
                p.age++;
                double gpu[gputime::kCount]{};
                bool   seen[gputime::kCount]{};
                const bool have = gputime::pair_result(p.id, gpu, seen);
                if (have || all || p.age > 12) print(p, have ? gpu : nullptr, seen);
                else g_pending[keep++] = p;
            }
            g_pending_n = keep;
        }

        void window(clock::time_point now)
        {
            if (now - g_window < std::chrono::seconds(5)) return;
            g_window = now;
            if (g_count.load() > 0 && g_pairs > 0) {
                const int n = std::max(1, g_slow);
                Counters  m = g_slow_sum;
                for (auto& v : m.v) v /= n;
                spdlog::info("[Hitch] last 5 s: {} pairs, {} over {:.0f} ms ({} not logged), longest {:.1f} ms; slow pairs, mean: game call {:.2f}, extra call {:.2f}, outside {:.2f}; per pair: {}",
                             g_pairs, g_slow, g_threshold.load(), g_unlogged, g_longest, g_slow_game / n, g_slow_extra / n, g_slow_outside / n, g_slow ? parts(m) : std::string{ "-" });
            }
            g_lines = g_pairs = g_slow = g_unlogged = 0;
            g_longest = g_slow_game = g_slow_extra = g_slow_outside = 0.0;
            g_slow_sum = Counters{};
        }
    }

    Block create_block()
    {
        g_creates++;
        return GetCurrentThreadId() == g_render_tid.load(std::memory_order_relaxed) ? kCreateRender : kCreateOther;
    }

    void pair_start(uint64_t id, const State& s)
    {
        const auto now = clock::now();
        g_render_tid = GetCurrentThreadId();
        const auto c   = snap();
        if (g_pending_n > 0) flush_pending(false);
        if (g_have_cur && (g_count.load() > 0 || g_csv_left.load() > 0)) {
            Pair& p   = g_cur;
            p.pair_ms = ms(now - p.start);
            p.c_total = c - p.c_start;
            if (g_csv_left.load() > 0) {
                // pair game extra outside | per call: draw state map (driver scan objects viewport lights) on_present post xrWait xrEnd | creates | speed view head
                const Counters cg = p.game ? p.c_game1 - p.c_game0 : Counters{};
                const Counters ce = p.extra ? p.c_extra1 - p.c_extra0 : Counters{};
                const double   game = p.game ? ms(p.t_game1 - p.t_game0) : 0.0, extra = p.extra ? ms(p.t_extra1 - p.t_extra0) : 0.0;
                auto calls = [](const Counters& k) {
                    return fmt::format("{:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f} {:.2f}", ms(k.v[cBlocks + kDrawHooks]), ms(k.v[cBlocks + kStateHooks]), ms(k.v[cBlocks + kMapHooks]),
                                       ms(k.v[cBlocks + kMapOrig]), ms(k.v[cBlocks + kScanMatrices]), ms(k.v[cBlocks + kObjConsts]), ms(k.v[cBlocks + kViewportConsts]), ms(k.v[cBlocks + kFixLights]),
                                       ms(k.v[cOnPresent]), ms(k.v[cPostPresent]), ms(k.v[cXrWait]), ms(k.v[cXrEnd]));
                };
                spdlog::info("[HitchCsv] {} {:.2f} {:.2f} {:.2f} {:.2f} | {} | {} | {} | {:.2f} {:.0f} {:.0f}", p.id, p.pair_ms, game, extra, p.pair_ms - game - extra, calls(cg), calls(ce),
                             p.c_total.v[cCreates], p.speed, p.view_turn, p.head_turn);
                if (--g_csv_left == 0) g_active = g_count.load() > 0;
            }
            if (g_count.load() > 0 && p.pair_ms < 10000.0) {
                g_pairs++;
                g_longest = std::max(g_longest, p.pair_ms);
                if (p.pair_ms > g_threshold.load()) {
                    g_slow++;
                    const double game  = p.game ? ms(p.t_game1 - p.t_game0) : 0.0;
                    const double extra = p.extra ? ms(p.t_extra1 - p.t_extra0) : 0.0;
                    g_slow_game += game;
                    g_slow_extra += extra;
                    g_slow_outside += p.pair_ms - game - extra;
                    const Counters all = p.c_total;
                    for (int i = 0; i < cCount; ++i) g_slow_sum.v[i] += all.v[i];
                    if (g_lines < g_count.load() && g_pending_n < 8) { g_lines++; g_pending[g_pending_n++] = p; }
                    else g_unlogged++;
                }
            }
        }
        window(now);
        Pair n;
        n.id      = id;
        n.start   = now;
        n.c_start = c;
        n.st      = s;
        if (g_have_cur) {
            const double dt = std::max(1e-3, std::chrono::duration<double>(now - g_cur.start).count());
            const float  d[3]{ s.cam[0] - g_cur.st.cam[0], s.cam[1] - g_cur.st.cam[1], s.cam[2] - g_cur.st.cam[2] };
            n.speed     = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / dt;
            n.view_turn = angle_deg(s.fwd, g_cur.st.fwd) / dt;
            n.head_turn = quat_angle_deg(s.head_q, g_cur.st.head_q) / dt;
        }
        g_cur      = n;
        g_have_cur = true;
    }

    void game_call_begin() { g_cur.t_game0 = clock::now(); g_cur.c_game0 = snap(); }
    void game_call_end() { g_cur.t_game1 = clock::now(); g_cur.c_game1 = snap(); g_cur.game = true; }
    void extra_call_begin() { g_cur.t_extra0 = clock::now(); g_cur.c_extra0 = snap(); }
    void extra_call_end() { g_cur.t_extra1 = clock::now(); g_cur.c_extra1 = snap(); g_cur.extra = true; }
    void mark_scope_call() { g_cur.st.scope_call = true; }

    void set_count(int n)
    {
        if (n <= 0 && g_pending_n > 0) flush_pending(true);
        g_count = std::max(0, n);
        g_active = g_count.load() > 0 || g_csv_left.load() > 0;
        g_have_cur = false;   // the next pair starts the count (not one spanning the time hitchlog was off)
        g_window = clock::now();
        g_lines = g_pairs = g_slow = g_unlogged = 0;
        g_longest = g_slow_game = g_slow_extra = g_slow_outside = 0.0;
        g_slow_sum = Counters{};
        spdlog::info("[Hitch] hitchlog {}: {}", g_count.load(), g_count.load() > 0 ? fmt::format("up to {} lines per 5 s for pairs over {:.0f} ms, and a 5 s summary", g_count.load(), g_threshold.load()) : std::string{ "off" });
    }
    void set_csv(int n)
    {
        g_csv_left = std::max(0, n);
        g_active   = g_count.load() > 0 || n > 0;
        g_have_cur = false;
        spdlog::info("[Hitch] hitchcsv {}: pair game extra outside | game call: draw state map (driver scan objects viewport lights) on_present post xrWait xrEnd | extra call: the same | creates | speed view head", n);
    }
    bool  active() { return g_active.load(); }
    void  set_threshold(float m) { g_threshold = std::clamp(m, 15.0f, 500.0f); }
    int   count() { return g_count.load(); }
    float threshold() { return g_threshold.load(); }
}
