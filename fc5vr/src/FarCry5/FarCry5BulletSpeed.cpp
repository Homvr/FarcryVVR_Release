// Bullet speed research (Homard 18 Oct: "add a setting for increase velocity next to bullet tracer option in F11").
// FC5 bullets are projectiles (~280-310 m/s, with drop) created by protected code. A flying bullet's velocity is a float3
// of that size along the shot: velscan finds such vectors in writable memory right after a shot (the x12 rifle: 305.00 m/s
// exactly, simulator 18 Oct), velnext shows how they change, velwatch puts write watches on them (camscan's). fltscan finds
// a float value (the speed's source) and fltnext narrows it; fltwatchr puts read watches on candidates.
// Every scan runs on its own thread: a 13 GB scan on the render thread (60 s) made the game lose its device and close.
#include "FarCry5BulletSpeed.h"
#include "FarCry5CamScan.h"

#include <Windows.h>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fc5::bullet
{
    namespace
    {
        struct Cand
        {
            float* p;
            float  v[3];
        };
        std::mutex          g_mtx;
        std::vector<Cand>   g_cands;   // velocity candidates
        std::vector<float*> g_flts;    // float value candidates
        std::atomic<bool>   g_busy{ false };

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

        template <class F> size_t each_region(F&& f)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            uint8_t* a = reinterpret_cast<uint8_t*>(0x10000);
            size_t   bytes = 0;
            while (VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                if (mbi.State == MEM_COMMIT && readable_rw(mbi.Protect) && mbi.RegionSize < (size_t(1) << 32)) {
                    f(static_cast<uint8_t*>(mbi.BaseAddress), mbi.RegionSize);
                    bytes += mbi.RegionSize;
                }
                a = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
                if (reinterpret_cast<uintptr_t>(a) >= 0x7FFFFFFF0000ull) break;
            }
            return bytes;
        }

        void vel_region(std::vector<Cand>& out, uint8_t* base, size_t size, const float dir[3], float lo2, float hi2, float hi, float mindot)
        {
            __try {
                const float* f = reinterpret_cast<const float*>(base);
                const size_t n = size / 4;
                for (size_t i = 0; i + 2 < n; ++i) {
                    const float x = f[i];
                    if (!(std::fabs(x) <= hi)) continue;   // also drops NaN
                    const float y = f[i + 1], z = f[i + 2];
                    const float m2 = x * x + y * y + z * z;
                    if (!(m2 >= lo2 && m2 <= hi2)) continue;
                    if ((x * dir[0] + y * dir[1] + z * dir[2]) / std::sqrt(m2) < mindot) continue;
                    if (out.size() < 20000) out.push_back({ const_cast<float*>(f + i), { x, y, z } });
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        void flt_region(std::vector<float*>& out, uint8_t* base, size_t size, uint32_t bits)
        {
            __try {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(base);
                for (size_t i = 0, n = size / 4; i < n; ++i)
                    if (w[i] == bits && out.size() < 400000) out.push_back(reinterpret_cast<float*>(const_cast<uint32_t*>(w + i)));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        bool read3(const float* p, float out[3])
        {
            __try {
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
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

        std::string around16(const float* p)
        {
            float v[16]{};
            if (!read16(p, v)) return "(unreadable)";
            std::string s;
            for (float f : v) s += fmt::format("{:.6g} ", f);
            return s;
        }

        bool start(const char* what)
        {
            if (g_busy.exchange(true)) {
                spdlog::info("[BulletSpeed] {}: a scan is still running", what);
                return false;
            }
            return true;
        }
    }

    void scan(const float dir_in[3], float lo, float hi, float mindot)
    {
        if (!start("velscan")) return;
        const float dir[3]{ dir_in[0], dir_in[1], dir_in[2] };
        std::thread([=] {
            const DWORD       t0 = GetTickCount();
            std::vector<Cand> found;
            const size_t      bytes = each_region([&](uint8_t* b, size_t s) { vel_region(found, b, s, dir, lo * lo, hi * hi, hi, mindot); });
            std::ofstream     out{ "D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/velscan.txt" };
            for (size_t i = 0; i < found.size(); ++i) {
                const auto& c = found[i];
                const float m = std::sqrt(c.v[0] * c.v[0] + c.v[1] * c.v[1] + c.v[2] * c.v[2]);
                out << fmt::format("{} {} {} |v| {:.3f} cos {:.6f} ({:.4f} {:.4f} {:.4f}) | from -16 bytes: {}\n", i, static_cast<void*>(c.p), module_of(c.p), m,
                                   (c.v[0] * dir[0] + c.v[1] * dir[1] + c.v[2] * dir[2]) / m, c.v[0], c.v[1], c.v[2], around16(c.p - 4));
            }
            {
                std::lock_guard _{ g_mtx };
                g_cands.swap(found);
            }
            spdlog::info("[BulletSpeed] velscan along ({:.4f}, {:.4f}, {:.4f}), |v| {:.0f}-{:.0f}, cos >= {:.4f}: {} candidates in {} MB, {} ms (background; list: probes/velscan.txt)",
                         dir[0], dir[1], dir[2], lo, hi, mindot, g_cands.size(), bytes >> 20, GetTickCount() - t0);
            g_busy = false;
        }).detach();
    }

    void next()
    {
        std::lock_guard _{ g_mtx };
        int changed = 0;
        for (size_t i = 0; i < g_cands.size(); ++i) {
            auto& c = g_cands[i];
            float v[3]{};
            if (!read3(c.p, v)) continue;
            if (v[0] == c.v[0] && v[1] == c.v[1] && v[2] == c.v[2]) continue;
            ++changed;
            if (changed <= 60)
                spdlog::info("[BulletSpeed]   {} {} {}: ({:.3f} {:.3f} {:.3f}) -> ({:.3f} {:.3f} {:.3f}), |v| {:.2f}", i, static_cast<void*>(c.p), module_of(c.p), c.v[0], c.v[1], c.v[2],
                             v[0], v[1], v[2], std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]));
            c.v[0] = v[0]; c.v[1] = v[1]; c.v[2] = v[2];
        }
        spdlog::info("[BulletSpeed] velnext: {} of {} candidates changed", changed, g_cands.size());
    }

    void watch(const std::vector<int>& idx, unsigned long render_tid)
    {
        std::vector<void*> addrs;
        {
            std::lock_guard _{ g_mtx };
            for (int i : idx) if (i >= 0 && i < (int)g_cands.size()) addrs.push_back(g_cands[i].p);
        }
        fc5::camscan::watch_addresses(addrs, render_tid, false);
        spdlog::info("[BulletSpeed] write watch on {} velocity candidate(s); camunwatch lists the writers", addrs.size());
    }

    void flt_scan(float value)
    {
        if (!start("fltscan")) return;
        std::thread([=] {
            const DWORD         t0 = GetTickCount();
            uint32_t            bits = 0;
            std::memcpy(&bits, &value, 4);
            std::vector<float*> found;
            const size_t        bytes = each_region([&](uint8_t* b, size_t s) { flt_region(found, b, s, bits); });
            size_t              img = 0;
            for (float* p : found) if (module_of(p) != "heap") ++img;
            {
                std::lock_guard _{ g_mtx };
                g_flts.swap(found);
            }
            spdlog::info("[BulletSpeed] fltscan {:.4f}: {} addresses ({} in module data) in {} MB, {} ms (background)", value, g_flts.size(), img, bytes >> 20, GetTickCount() - t0);
            g_busy = false;
        }).detach();
    }

    void flt_next(float value, bool list)
    {
        std::lock_guard _{ g_mtx };
        std::vector<float*> keep;
        for (float* p : g_flts) {
            float v[3]{};
            if (read3(p, v) && v[0] == value) keep.push_back(p);
        }
        g_flts.swap(keep);
        spdlog::info("[BulletSpeed] fltnext {:.4f}: {} addresses", value, g_flts.size());
        if (!list) return;
        std::ofstream out{ "D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/fltscan.txt" };
        for (size_t i = 0; i < g_flts.size(); ++i) out << fmt::format("{} {} {} | from -32 bytes: {}\n", i, static_cast<void*>(g_flts[i]), module_of(g_flts[i]), around16(g_flts[i] - 8));
        spdlog::info("[BulletSpeed] list: probes/fltscan.txt");
    }

    void flt_watch(const std::vector<int>& idx, unsigned long render_tid, bool reads)
    {
        std::vector<void*> addrs;
        {
            std::lock_guard _{ g_mtx };
            for (int i : idx) if (i >= 0 && i < (int)g_flts.size()) addrs.push_back(g_flts[i]);
        }
        fc5::camscan::watch_addresses(addrs, render_tid, reads);
        spdlog::info("[BulletSpeed] {} watch on {} value candidate(s); camunwatch lists the code", reads ? "read/write" : "write", addrs.size());
    }

    bool busy() { return g_busy.load(); }
}

// bulletspeed (F11 Weapon, next to the bullet tracers): the speed of the bullets of the weapons fired, times k.
// FC_m64+0x1e10d90 (plain code, game thread, once per shot; simulator 18 Oct): [r14+0xc0] is the weapon's projectile record,
// its speed at +0x514 (305.0 for the x12 rifle: what a flying bullet's velocity measured), times [[r14+0x50]+0x7dc] (1.0).
// The protected code that moves the bullet reads the record afterwards (27 reads of +0x514 per shot), so the record itself is
// scaled: on the first shot of each record its speed is kept and set to speed x k; changing k rewrites every known record.
// Records under 150 m/s (rockets, ~30 m/s) are left alone. The records are shared: anyone firing the same kind of weapon
// (enemies too) shoots as fast; records are only taken from the player's shots.
namespace fc5::bullet
{
    namespace
    {
        safetyhook::MidHook                g_speed_hook{};
        std::atomic<float>                 g_scale{ 1.0f };
        std::mutex                         g_rec_mtx;
        std::unordered_map<uint8_t*, float> g_recs;   // record -> the game's speed
        std::atomic<uint64_t>              g_shots{ 0 };
        std::atomic<uint64_t>              g_player_fire_ms{ 0 };   // GetTickCount64 of the player's last trigger (the mod's fire gate)
        std::atomic<uint64_t>              g_press_ms{ 0 };         // shotbuzz: GetTickCount64 of the trigger press (first frame down after 500 ms up)
        // [Shot] timing (Homard 21 Oct "two guns that have a high delay when shooting"): the mod's last release, waiting for the
        // game's next shot code call.
        std::atomic<bool>                  g_rel_armed{ false };
        std::atomic<int64_t>               g_rel_t{ 0 };   // steady_clock ns of the release
        std::atomic<uint64_t>              g_rel_press_ms{ 0 }, g_rel_weapon{ 0 };
        // shotbuzz: the callback, the press whose first shot named the player's weapon, that weapon (r14).
        std::atomic<void (*)()>            g_shot_cb{ nullptr };
        std::atomic<uint64_t>              g_buzz_press{ 0 }, g_buzz_weapon{ 0 };

        bool read_f(const float* p, float& v)
        {
            __try { v = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool read_p(const uint64_t* p, uint64_t& v)
        {
            __try { v = *p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool write_f(float* p, float v)
        {
            __try { *p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void speed_probe(safetyhook::Context& c)
        {
            const uint64_t n = ++g_shots;
            if (auto cb = g_shot_cb.load(); cb && GetTickCount64() - g_player_fire_ms.load() <= 500) {
                // shotbuzz: the first shot code call of a press is the player's weapon; later calls count only from that weapon.
                const uint64_t press = g_press_ms.load();
                if (g_buzz_press.exchange(press) != press) g_buzz_weapon = c.r14;
                if (c.r14 == g_buzz_weapon.load()) cb();
            }
            if (g_rel_armed.exchange(false)) {
                const int64_t ms = (std::chrono::steady_clock::now().time_since_epoch().count() - g_rel_t.load()) / 1000000;
                if (ms < 1500)
                    spdlog::info("[Shot] weapon {}: the game's shot {} ms after the mod let the trigger through ({} ms after the press; {} ms in all)",
                                 g_rel_weapon.load(), ms, g_rel_press_ms.load(), ms + int64_t(g_rel_press_ms.load()));
                else
                    spdlog::info("[Shot] weapon {}: no game shot within 1.5 s of the mod letting the trigger through", g_rel_weapon.load());
            }
            uint64_t rec = 0;
            const bool got = read_p(reinterpret_cast<const uint64_t*>(c.r14 + 0xc0), rec);
            if (n <= 12) {
                float v = 0.0f;
                if (got && rec >= 0x10000) read_f(reinterpret_cast<const float*>(rec + 0x514), v);
                spdlog::info("[BulletSpeed] shot code call {}: weapon {:x} record {:x} speed {:.2f}", n, c.r14, rec, v);
            }
            if (!got || rec < 0x10000) return;
            float* sp = reinterpret_cast<float*>(rec + 0x514);
            const float k = g_scale.load();
            std::lock_guard _{ g_rec_mtx };
            auto it = g_recs.find(reinterpret_cast<uint8_t*>(rec));
            if (it == g_recs.end()) {
                // Only the player's weapons are taken (a shot code call while the player's trigger is down); an enemy shooting the same
                // kind of weapon shares its record and so its speed.
                if (GetTickCount64() - g_player_fire_ms.load() > 500) return;
                float v = 0.0f;
                if (!read_f(sp, v) || !(v >= 150.0f && v <= 2000.0f)) return;   // not a bullet (rockets ~30 m/s), or unreadable
                it = g_recs.emplace(reinterpret_cast<uint8_t*>(rec), v).first;
                spdlog::info("[BulletSpeed] weapon record {:x}: bullet speed {:.1f} m/s, set to {:.1f} (x{:.2f})", rec, v, v * k, k);
            }
            write_f(sp, it->second * k);
        }
    }

    namespace
    {
        bool code_matches(const uint8_t* at)
        {
            static const uint8_t kAt[7]{ 0x49, 0x8B, 0x86, 0xC0, 0x00, 0x00, 0x00 };   // mov rax, [r14+0C0h]; then movaps xmm7, xmm0
            __try { return std::memcmp(at, kAt, 7) == 0 && at[7] == 0x0F && at[8] == 0x28; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    void install_speed_hook(uint8_t* base)
    {
        if (!code_matches(base + 0x1e10d90)) { spdlog::info("[BulletSpeed] shot code not found (other game build): no bullet speed setting"); return; }
        g_speed_hook = safetyhook::create_mid(base + 0x1e10d90, speed_probe);
        spdlog::info("[BulletSpeed] bullet speed hook {}", g_speed_hook ? "installed" : "failed");
    }

    void set_speed_scale(float k)
    {
        k = std::clamp(k, 1.0f, 4.0f);
        g_scale = k;
        std::lock_guard _{ g_rec_mtx };
        for (auto& [rec, v] : g_recs) write_f(reinterpret_cast<float*>(rec + 0x514), v * k);
    }

    float speed_scale() { return g_scale.load(); }
    void  note_player_fire()
    {
        const uint64_t now = GetTickCount64();
        if (now - g_player_fire_ms.load() > 500) g_press_ms = now;   // shotbuzz: a new press
        g_player_fire_ms = now;
    }
    void  note_shot_release(uint64_t press_ms, uint64_t weapon)
    {
        if (g_rel_armed.load() && std::chrono::steady_clock::now().time_since_epoch().count() - g_rel_t.load() > 1500000000ll)
            spdlog::info("[Shot] weapon {}: no game shot within 1.5 s of the mod letting the trigger through", g_rel_weapon.load());
        g_rel_press_ms = press_ms;
        g_rel_weapon   = weapon;
        g_rel_t        = std::chrono::steady_clock::now().time_since_epoch().count();
        g_rel_armed    = true;
    }
    bool  speed_available() { return (bool)g_speed_hook; }
    void  set_player_shot_callback(void (*cb)()) { g_shot_cb = cb; }

    void speed_status()
    {
        std::lock_guard _{ g_rec_mtx };
        spdlog::info("[BulletSpeed] status: x{:.2f}, hook {}, {} shots seen, {} weapon records", g_scale.load(), (bool)g_speed_hook ? "on" : "off", g_shots.load(), g_recs.size());
        for (const auto& [rec, v] : g_recs) {
            float now = 0.0f;
            read_f(reinterpret_cast<const float*>(rec + 0x514), now);
            spdlog::info("[BulletSpeed]   record {}: game {:.1f} m/s, now {:.1f}", static_cast<void*>(rec), v, now);
        }
    }
}

// FC2 VR item 2 (Homard 19:14 "You can go for idea 2 too"; research\fc2_items_1_2_feasibility_2026-10-23.md): the shot's origin and
// direction, written by the weapon's protected call and read by the projectile spawn (+0x1e10cf0) and the shot event, rewritten
// between the two. The spawn's spread (+0x1d6ba30) turns each pellet around the direction given, so it is kept.
namespace fc5::bullet
{
    namespace
    {
        safetyhook::MidHook   g_shot_hook{};
        std::atomic<int>      g_probe_left{ 0 };
        std::atomic<uint64_t> g_player_weapon{ 0 };   // the weapon (rsi) of the last player shot
        std::mutex            g_view_mtx;
        struct View { float cam[3]{}, fwd[3]{ 0, 1, 0 }, aim[3]{ 0, 1, 0 }, muzzle[3]{}; bool aim_ok{ false }, wall{ false }; uint64_t ms{ 0 }; };
        View                  g_view{};

        bool read3(uintptr_t p, float* v) { __try { std::memcpy(v, reinterpret_cast<const void*>(p), 12); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        float len3(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
        float angle_deg(const float* a, const float* b)
        {
            const float la = len3(a), lb = len3(b);
            if (la < 1e-6f || lb < 1e-6f) return -1.0f;
            const float d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
            return std::acos(std::clamp(d, -1.0f, 1.0f)) * 57.29578f;
        }

        void shot_call(safetyhook::Context& c)
        {
            const int probe = g_probe_left.load();
            if (probe <= 0) return;
            const uintptr_t po = c.rbp - 0x58, pd = c.rbp - 0x68;
            float           o[3]{}, d[3]{};
            if (!read3(po, o) || !read3(pd, d)) return;
            View v{};
            {
                std::lock_guard _{ g_view_mtx };
                v = g_view;
            }
            const uint64_t now     = GetTickCount64();
            const bool     window  = now - g_player_fire_ms.load() <= 500;
            const float    dc[3]{ o[0] - v.cam[0], o[1] - v.cam[1], o[2] - v.cam[2] };
            const float    dist    = len3(dc);
            const bool     at_cam  = dist < 0.5f;
            const bool     same    = c.rsi != 0 && c.rsi == g_player_weapon.load();
            const bool     player  = window && (at_cam || same);
            if (player && at_cam) g_player_weapon = c.rsi;
            {
                g_probe_left--;
                spdlog::info("[ShotProbe] weapon (rsi) {:x} arg (r14) {:x} | origin ({:.3f}, {:.3f}, {:.3f}), {:.3f} m from the game camera | direction ({:.4f}, {:.4f}, {:.4f}) "
                             "length {:.4f}, {:.2f} deg from the game camera's forward, {:.2f} deg from the barrel{} | trigger window {} ({} ms) -> {}",
                             c.rsi, c.r14, o[0], o[1], o[2], dist, d[0], d[1], d[2], len3(d), angle_deg(d, v.fwd), angle_deg(d, v.aim), v.aim_ok ? "" : " (no barrel)",
                             window ? "open" : "shut", now - g_player_fire_ms.load(), player ? "the player's" : "not the player's");
            }
        }

        bool shot_code_matches(const uint8_t* at)
        {
            static const uint8_t k[12]{ 0x48, 0x8D, 0x44, 0x24, 0x70, 0x49, 0x8B, 0xD6, 0x4C, 0x8D, 0x4D, 0x98 };   // lea rax,[rsp+70h] / mov rdx,r14 / lea r9,[rbp-68h]
            __try { return std::memcmp(at, k, sizeof(k)) == 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
    }

    bool install_shot_hook(uint8_t* base)
    {
        if (g_shot_hook) return true;
        if (!base || !shot_code_matches(base + 0x1e375fa)) {
            spdlog::info("[ShotProbe] the fire function's code differs at FC_m64+0x1e375fa (other game build): no shot hook");
            return false;
        }
        g_shot_hook = safetyhook::create_mid(base + 0x1e375fa, shot_call);
        spdlog::info("[ShotProbe] shot hook at FC_m64+0x1e375fa {}", g_shot_hook ? "installed" : "failed");
        return (bool)g_shot_hook;
    }
    void shot_probe(int n) { g_probe_left = std::max(0, n); }
    void set_view(const float cam[3], const float fwd[3], const float aim[3], const float muzzle[3], bool aim_valid, bool muzzle_at_wall)
    {
        std::lock_guard _{ g_view_mtx };
        std::memcpy(g_view.cam, cam, 12);
        std::memcpy(g_view.fwd, fwd, 12);
        std::memcpy(g_view.aim, aim, 12);
        std::memcpy(g_view.muzzle, muzzle, 12);
        g_view.aim_ok = aim_valid;
        g_view.wall   = muzzle_at_wall;
        g_view.ms     = GetTickCount64();
    }
}
