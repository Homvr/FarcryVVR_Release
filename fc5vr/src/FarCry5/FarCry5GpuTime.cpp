#include "FarCry5GpuTime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include <spdlog/spdlog.h>

namespace fc5::gputime
{
    namespace
    {
        constexpr int kSets   = 4;    // pairs in flight
        constexpr int kStamps = 48;   // timestamps per pair
        const char*   kNames[kCount]{ "pair", "game call (left)", "extra call (right)", "DLSS inputs", "DLSS evaluation", "compose", "aim march", "scope call" };

        struct Set
        {
            ID3D11Query* disjoint = nullptr;
            ID3D11Query* ts[kStamps]{};
            int          stage[kStamps]{};
            bool         is_end[kStamps]{};
            int          n       = 0;
            uint64_t     id      = 0;
            bool         open    = false;   // between begin_pair and end_pair
            bool         pending = false;   // ended, not yet read
        };
        Set                 g_sets[kSets];
        int                 g_cur = -1;     // set being recorded, -1 = none
        int                 g_next = 0;
        std::atomic<bool>   g_on{ true };   // on by default (2026-10-12 crash thread): a few timestamps per pair, read without waiting; shows which stage a stall is in
        bool                g_ready = false;
        std::vector<double> g_ms[kCount];   // per pair, this 5 s window
        uint64_t            g_skipped = 0, g_disjoint = 0;
        std::string         g_summary;
        std::mutex          g_sum_mtx;
        auto                g_last_log = std::chrono::steady_clock::now();
        // The last pairs read, by id (hitchlog).
        struct Result
        {
            uint64_t id = 0;
            double   ms[kCount]{};
            bool     seen[kCount]{};
        };
        Result g_results[32];
        int    g_result_next = 0;

        bool create(ID3D11DeviceContext* ctx)
        {
            if (g_ready) return true;
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            if (!dev) return false;
            bool ok = true;
            for (auto& s : g_sets) {
                D3D11_QUERY_DESC d{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
                ok = ok && SUCCEEDED(dev->CreateQuery(&d, &s.disjoint));
                d.Query = D3D11_QUERY_TIMESTAMP;
                for (auto& q : s.ts) ok = ok && SUCCEEDED(dev->CreateQuery(&d, &q));
            }
            dev->Release();
            g_ready = ok;
            spdlog::info("[GpuTime] timestamp queries {}", ok ? "created" : "FAILED");
            return ok;
        }

        void stamp(ID3D11DeviceContext* ctx, int stage, bool is_end)
        {
            if (g_cur < 0) return;
            Set& s = g_sets[g_cur];
            if (s.n >= kStamps) return;
            s.stage[s.n]  = stage;
            s.is_end[s.n] = is_end;
            ctx->End(s.ts[s.n]);
            s.n++;
        }

        // A finished pair: per stage, the sum of its begin..end spans.
        bool read(ID3D11DeviceContext* ctx, Set& s)
        {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            if (ctx->GetData(s.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
            uint64_t t[kStamps]{};
            for (int i = 0; i < s.n; ++i)
                if (ctx->GetData(s.ts[i], &t[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
            s.pending = false;
            if (dj.Disjoint || dj.Frequency == 0) { g_disjoint++; return true; }
            double sum[kCount]{};
            bool   seen[kCount]{};
            uint64_t open_at[kCount]{};
            bool     is_open[kCount]{};
            for (int i = 0; i < s.n; ++i) {
                const int st = s.stage[i];
                if (!s.is_end[i]) { open_at[st] = t[i]; is_open[st] = true; }
                else if (is_open[st] && t[i] >= open_at[st]) {
                    sum[st] += double(t[i] - open_at[st]) * 1000.0 / double(dj.Frequency);
                    seen[st]    = true;
                    is_open[st] = false;
                }
            }
            for (int st = 0; st < kCount; ++st) if (seen[st]) g_ms[st].push_back(sum[st]);
            if (s.id != 0) {
                Result& r = g_results[g_result_next];
                g_result_next = (g_result_next + 1) % 32;
                r.id = s.id;
                for (int st = 0; st < kCount; ++st) { r.ms[st] = sum[st]; r.seen[st] = seen[st]; }
            }
            return true;
        }
    }

    void set_enabled(bool on)
    {
        if (g_on.exchange(on) != on) spdlog::info("[GpuTime] GPU timing per stage {}", on ? "on" : "off");
    }
    bool enabled() { return g_on.load(); }

    bool pair_result(uint64_t id, double ms[kCount], bool seen[kCount])
    {
        for (const auto& r : g_results) {
            if (r.id != id || id == 0) continue;
            for (int st = 0; st < kCount; ++st) { ms[st] = r.ms[st]; seen[st] = r.seen[st]; }
            return true;
        }
        return false;
    }

    void begin_pair(ID3D11DeviceContext* ctx, uint64_t id)
    {
        g_cur = -1;
        if (!g_on.load() || !ctx || !create(ctx)) return;
        Set& s = g_sets[g_next];
        if (s.pending && !read(ctx, s)) { g_skipped++; return; }   // the GPU is more than kSets pairs behind
        g_next = (g_next + 1) % kSets;
        s.n    = 0;
        s.id   = id;
        s.open = true;
        g_cur  = int(&s - g_sets);
        ctx->Begin(s.disjoint);
        stamp(ctx, kPair, false);
    }

    void end_pair(ID3D11DeviceContext* ctx)
    {
        if (g_cur < 0 || !ctx) return;
        Set& s = g_sets[g_cur];
        stamp(ctx, kPair, true);
        ctx->End(s.disjoint);
        s.open    = false;
        s.pending = true;
        g_cur     = -1;
    }

    void begin(ID3D11DeviceContext* ctx, Stage st) { if (g_cur >= 0 && ctx) stamp(ctx, st, false); }
    void end(ID3D11DeviceContext* ctx, Stage st) { if (g_cur >= 0 && ctx) stamp(ctx, st, true); }

    void poll(ID3D11DeviceContext* ctx)
    {
        if (!g_ready || !ctx) return;
        for (auto& s : g_sets)
            if (s.pending) read(ctx, s);
        const auto now = std::chrono::steady_clock::now();
        if (now - g_last_log < std::chrono::seconds(5)) return;
        g_last_log = now;
        if (!g_on.load() && g_ms[kPair].empty()) return;
        std::string line;
        for (int st = 0; st < kCount; ++st) {
            auto& v = g_ms[st];
            if (v.empty()) continue;
            double m = 0.0;
            for (double x : v) m += x;
            m /= double(v.size());
            std::sort(v.begin(), v.end());
            const double p99 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.99))];
            line += fmt::format("{} {:.2f}/{:.2f}; ", kNames[st], m, p99);
        }
        const size_t pairs = g_ms[kPair].size();
        for (auto& v : g_ms) v.clear();
        line = fmt::format("GPU ms per pair, mean/p99 over {} pairs: {}(skipped {}, disjoint {})", pairs, line, std::exchange(g_skipped, 0), std::exchange(g_disjoint, 0));
        spdlog::info("[GpuTime] {}", line);
        std::lock_guard _{ g_sum_mtx };
        g_summary = line;
    }

    std::string summary()
    {
        std::lock_guard _{ g_sum_mtx };
        return g_summary;
    }
}
