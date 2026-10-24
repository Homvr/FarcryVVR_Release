#include "FarCry5Tobii.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

#include <spdlog/spdlog.h>

// The game's per-frame use (FC_m64+0xE089A60, manager in rcx): GetUpdateRate(mgr) -> float step; Update() -> bool
// (false: "no tracker"); GetNewGazePoints(&ptr, &count, 0); GetNewHeadPoses(&ptr, &count); UpdateInteraction(gaze,
// n, head, n, step); GetInfiniteScreenAngles() -> two floats in rax (yaw, pitch); stored at mgr+0x6C (yaw, sign
// flipped) and +0x70, both times the fade weight at +0x38 (moves toward +0x3C at +0x40 per second).
// Slots are found by name in FC_m64.dll's delay-import table (pipeline review R8), not by a fixed offset.
namespace fc5::tobii
{
    namespace
    {
        enum Fn { kStart, kUpdate, kGaze, kHead, kRate, kAimFilter, kInteraction, kAngles, kFnCount };
        const char* kFnNames[kFnCount]{ "Start", "Update", "GetNewGazePoints", "GetNewHeadPoses", "GetUpdateRate", "UpdateAimAtGazeFilter", "UpdateInteraction",
                                        "GetInfiniteScreenAngles" };

        std::atomic<bool>     g_installed{ false };
        std::atomic<uint64_t> g_angles{ 0 };   // yaw | pitch << 32 (one value: the two cannot tear)
        std::atomic<float>    g_step{ 1.0f / 72.0f };
        std::atomic<uint8_t*> g_mgr{ nullptr };
        std::atomic<uint64_t> g_calls[kFnCount]{};
        void**                g_slot[kFnCount]{};
        void**                g_all_slots = nullptr;
        // The offset each game update stored (+0x6C, +0x70 after update k) and what was handed to it, by update number.
        struct Applied { std::atomic<uint64_t> k{ ~0ull }; std::atomic<float> y{ 0 }, p{ 0 }, hy{ 0 }, hp{ 0 }; };
        Applied               g_ring[32];
        std::atomic<uint64_t> g_seq{ 0 };   // game Tobii updates started
        std::atomic<void (*)()> g_game_tick{ nullptr };
        int                   g_all_n     = 0;
        std::string           g_all_names[32];

        float __fastcall s_update_rate(void* mgr)
        {
            g_calls[kRate]++;
            if (auto* fn = g_game_tick.load()) fn();
            if (mgr) {
                g_mgr = static_cast<uint8_t*>(mgr);
                // The previous update's stored offset, filed under its number before this update replaces it.
                const uint64_t k = g_seq.load();
                if (k > 0) {
                    float y = 0, p = 0;
                    std::memcpy(&y, static_cast<uint8_t*>(mgr) + 0x6C, 4);
                    std::memcpy(&p, static_cast<uint8_t*>(mgr) + 0x70, 4);
                    auto& r = g_ring[(k - 1) & 31];
                    r.y = y;
                    r.p = p;
                    r.k = k - 1;
                }
                auto& n = g_ring[k & 31];
                n.k     = ~0ull;
                g_seq   = k + 1;
            }
            return g_step.load();
        }
        bool __fastcall s_update() { g_calls[kUpdate]++; return true; }
        bool __fastcall s_start(void*, void*, void*, void*) { g_calls[kStart]++; return true; }
        void __fastcall s_gaze(void** pts, int* n, void*)
        {
            g_calls[kGaze]++;
            if (pts) *pts = nullptr;
            if (n) *n = 0;
        }
        void __fastcall s_head(void** poses, int* n)
        {
            g_calls[kHead]++;
            if (poses) *poses = nullptr;
            if (n) *n = 0;
        }
        void __fastcall s_aim_filter(void*, void*, void*, void*) { g_calls[kAimFilter]++; }
        void __fastcall s_interaction(void*, int, void*, int) { g_calls[kInteraction]++; }
        uint64_t __fastcall s_angles()
        {
            g_calls[kAngles]++;
            const uint64_t a = g_angles.load();
            if (const uint64_t k = g_seq.load(); k > 0) {
                float          y = 0, p = 0;
                const uint32_t lo = uint32_t(a), hi = uint32_t(a >> 32);
                std::memcpy(&y, &lo, 4);
                std::memcpy(&p, &hi, 4);
                g_ring[(k - 1) & 31].hy = y;
                g_ring[(k - 1) & 31].hp = p;
            }
            return a;
        }
        void* const kStubs[kFnCount]{ reinterpret_cast<void*>(&s_start), reinterpret_cast<void*>(&s_update), reinterpret_cast<void*>(&s_gaze),
                                      reinterpret_cast<void*>(&s_head), reinterpret_cast<void*>(&s_update_rate), reinterpret_cast<void*>(&s_aim_filter),
                                      reinterpret_cast<void*>(&s_interaction), reinterpret_cast<void*>(&s_angles) };

        std::string module_of(void* p)
        {
            HMODULE m = nullptr;
            char    n[MAX_PATH]{};
            if (p && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(p), &m) && m) {
                GetModuleFileNameA(m, n, MAX_PATH);
                const char* s = std::strrchr(n, '\\');
                return fmt::format("{}+{:#x}", s ? s + 1 : n, uintptr_t(p) - uintptr_t(m));
            }
            return fmt::format("{}", p);
        }

        bool readable(const void* p, size_t n)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
            return uintptr_t(p) + n <= uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
        }

        // The delay-import address table of Tobii.GameIntegration.dll in the loaded FC_m64.dll, slots by name.
        bool find_slots(uint8_t* base)
        {
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (!readable(dos, sizeof *dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (!readable(nt, sizeof *nt) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
            const auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            if (dd.VirtualAddress == 0) return false;
            for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dd.VirtualAddress); readable(d, sizeof *d) && d->DllNameRVA; ++d) {
                const char* dll = reinterpret_cast<const char*>(base + d->DllNameRVA);
                if (_stricmp(dll, "Tobii.GameIntegration.dll") != 0) continue;
                auto** iat  = reinterpret_cast<void**>(base + d->ImportAddressTableRVA);
                auto*  ints = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->ImportNameTableRVA);
                g_all_slots = iat;
                g_all_n     = 0;
                for (int i = 0; i < 32 && readable(&ints[i], 8) && ints[i].u1.AddressOfData; ++i) {
                    std::string name = IMAGE_SNAP_BY_ORDINAL64(ints[i].u1.Ordinal) ? fmt::format("#{}", IMAGE_ORDINAL64(ints[i].u1.Ordinal))
                                                                                   : std::string{ reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + ints[i].u1.AddressOfData)->Name };
                    for (int f = 0; f < kFnCount; ++f)
                        if (name == kFnNames[f]) g_slot[f] = &iat[i];
                    g_all_names[i] = std::move(name);
                    g_all_n        = i + 1;
                }
                for (int f = 0; f < kFnCount; ++f)
                    if (!g_slot[f]) { spdlog::info("[Tobii] {} not imported by this build", kFnNames[f]); return false; }
                return true;
            }
            return false;
        }
    }

    bool install()
    {
        if (g_installed.load()) return true;
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        if (!base || !find_slots(base)) { spdlog::info("[Tobii] Tobii slots not found: head look stays off"); return false; }
        // The slots left to the real DLL (settings, window) still load it on first use: it must be there.
        if (!LoadLibraryA("Tobii.GameIntegration.dll")) { spdlog::info("[Tobii] Tobii.GameIntegration.dll missing: head look stays off"); return false; }
        DWORD old = 0;
        for (int f = 0; f < kFnCount; ++f) {
            if (!VirtualProtect(g_slot[f], sizeof(void*), PAGE_READWRITE, &old)) return false;
            *g_slot[f] = kStubs[f];
            VirtualProtect(g_slot[f], sizeof(void*), old, &old);
        }
        g_installed = true;
        spdlog::info("[Tobii] per-frame slots now fed by the mod ({} Tobii imports found by name)", g_all_n);
        return true;
    }

    bool installed() { return g_installed.load(); }
    void set_game_tick(void (*fn)()) { g_game_tick = fn; }
    void set_angles(float yaw, float pitch)
    {
        uint32_t a = 0, b = 0;
        std::memcpy(&a, &yaw, 4);
        std::memcpy(&b, &pitch, 4);
        g_angles = uint64_t(a) | (uint64_t(b) << 32);
    }
    void set_step(float seconds) { if (seconds > 0.001f && seconds < 0.1f) g_step = seconds; }
    uint8_t* manager()
    {
        uint8_t* m = g_mgr.load();
        return readable(m, 0x80) ? m : nullptr;
    }
    uint64_t updates() { return g_calls[kUpdate].load(); }
    uint64_t seq() { return g_seq.load(); }
    bool applied(uint64_t k, float& yaw, float& pitch)
    {
        const auto& r = g_ring[k & 31];
        if (r.k.load() != k) return false;
        yaw   = r.y.load();
        pitch = r.p.load();
        return r.k.load() == k && std::isfinite(yaw) && std::isfinite(pitch);
    }
    bool handed(uint64_t k, float& yaw, float& pitch)
    {
        const auto& r = g_ring[k & 31];
        yaw   = r.hy.load();
        pitch = r.hp.load();
        return true;
    }

    bool read_float(int off, float& v)
    {
        uint8_t* m = manager();
        if (!m || off < 0 || off > 0x7C) return false;
        std::memcpy(&v, m + off, 4);
        return std::isfinite(v);
    }

    void status()
    {
        for (int i = 0; i < g_all_n; ++i) {
            uint64_t calls = 0;
            for (int f = 0; f < kFnCount; ++f) if (g_slot[f] == &g_all_slots[i]) calls = g_calls[f].load();
            spdlog::info("[Tobii] slot {:2} {:<40} -> {} (calls {})", i, g_all_names[i], module_of(g_all_slots[i]), calls);
        }
        if (auto* m = manager()) {
            std::string b, f;
            for (int o = 0x10; o < 0x24; ++o) b += fmt::format("+{:x}={} ", o, m[o]);
            for (int o = 0x34; o < 0x78; o += 4) { float v; std::memcpy(&v, m + o, 4); f += fmt::format("+{:x}={:.4g} ", o, v); }
            spdlog::info("[Tobii] manager {} bytes {}", (void*)m, b);
            spdlog::info("[Tobii] manager floats {}", f);
        } else {
            spdlog::info("[Tobii] manager not seen (the game has not run its per-frame Tobii update since install)");
        }
        const uint64_t a = g_angles.load();
        float          y = 0, p = 0;
        uint32_t       lo = uint32_t(a), hi = uint32_t(a >> 32);
        std::memcpy(&y, &lo, 4);
        std::memcpy(&p, &hi, 4);
        spdlog::info("[Tobii] offset handed to the game: yaw {:.3f}, pitch {:.3f}", y, p);
    }

    void poke_byte(int off, uint8_t v)
    {
        if (auto* m = manager(); m && off >= 0 && off < 0x80) m[off] = v;
    }
    void poke_float(int off, float v)
    {
        if (auto* m = manager(); m && off >= 0 && off <= 0x7C) std::memcpy(m + off, &v, 4);
    }
}
