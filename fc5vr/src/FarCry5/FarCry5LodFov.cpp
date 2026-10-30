// lodfov (Homard 28 Oct 2026, route A of research\2026-10-28_fov_lod_popin.md): far detail as at the flat FOV.
// The game picks LOD with LodScale and KillLodScale times its zoom factor (FOV / 75 deg)^0.75 (simulator, 28 Oct): x1.42 at the
// mod's 120 deg camera, so objects switch to coarser models and disappear sooner than on the flat screen. The scales live in
// engine\settings\DefaultRenderConfig.xml (common.dat, LZ4 block); the parsed values were not found in memory. So when the game
// reads that entry from common.dat, the read gets a changed copy instead: the Geometry presets' LodScale and KillLodScale times
// the scale (on = 0.70: ultrahigh 0.5 -> 0.35), comments blanked to make room, compressed to exactly the original size (the
// archive's index is not touched, nothing on disk changes). Off: no hook, the game reads its own bytes.
#include "FarCry5LodFov.h"

#include "../ModPath.h"

#include <windows.h>
#include <winternl.h>

#include <safetyhook.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fc5::lodfov
{
namespace
{
    constexpr uint64_t kHash  = 0x9d542a475c8ccadfull;   // engine\settings\DefaultRenderConfig.xml
    constexpr float    kOn    = 0.70f;                   // 1 / 1.4226: LOD as at the game's own 75 deg
    inline bool        NT_OK(NTSTATUS s) { return s >= 0; }

    std::atomic<float> g_scale{ 0.0f };                  // the setting (next start)
    float              g_active = 0.0f;                  // what this run's config was built with
    std::string        g_why    = "off";
    std::wstring       g_dat;                            // full path of the archive holding the entry
    uint64_t           g_off = 0;                        // the entry's compressed bytes in it
    uint32_t           g_csize = 0, g_usize = 0;
    std::vector<uint8_t> g_block;                        // the changed entry, g_csize bytes
    std::atomic<int>   g_reads{ 0 }, g_patched{ 0 };
    SafetyHookInline   g_read_hook{};

    // --- LZ4 block -------------------------------------------------------------------------------------------------------------
    bool lz4_decode(const uint8_t* s, size_t n, std::vector<uint8_t>& d, size_t usize)
    {
        d.assign(usize, 0);
        size_t ip = 0, op = 0;
        while (ip < n) {
            const int tok = s[ip++];
            size_t    lit = tok >> 4;
            if (lit == 15) { uint8_t x; do { if (ip >= n) return false; x = s[ip++]; lit += x; } while (x == 255); }
            if (ip + lit > n || op + lit > usize) return false;
            std::memcpy(&d[op], s + ip, lit);
            ip += lit; op += lit;
            if (ip >= n) break;
            if (ip + 2 > n) return false;
            const size_t off = s[ip] | (s[ip + 1] << 8);
            ip += 2;
            size_t ml = tok & 15;
            if (ml == 15) { uint8_t x; do { if (ip >= n) return false; x = s[ip++]; ml += x; } while (x == 255); }
            ml += 4;
            if (off == 0 || off > op || op + ml > usize) return false;
            for (size_t k = 0; k < ml; ++k, ++op) d[op] = d[op - off];
        }
        return op == usize && ip == n;
    }

    struct Seq { size_t lit_start, lit_len, off, match_len; };   // match_len 0: the last sequence (literals only)

    size_t ext_bytes(size_t v) { return v < 15 ? 0 : (v - 15) / 255 + 1; }
    size_t encoded_size(const std::vector<Seq>& q)
    {
        size_t s = 0;
        for (const auto& x : q) {
            s += 1 + ext_bytes(x.lit_len) + x.lit_len;
            if (x.match_len) s += 2 + ext_bytes(x.match_len - 4);
        }
        return s;
    }
    // Greedy LZ4 (4-byte hash, 64 KB window); the last 5 bytes are literals, no match starts in the last 12.
    std::vector<Seq> lz4_parse(const std::vector<uint8_t>& t)
    {
        std::vector<Seq> q;
        const size_t     n = t.size();
        std::vector<int64_t> head(1 << 16, -1);
        auto h4 = [&](size_t i) { uint32_t v; std::memcpy(&v, &t[i], 4); return (v * 2654435761u) >> 16; };
        size_t anchor = 0, i = 0;
        while (n >= 13 && i + 12 < n) {
            const uint32_t h = h4(i);
            const int64_t  c = head[h];
            head[h]          = int64_t(i);
            if (c >= 0 && i - size_t(c) <= 65535 && std::memcmp(&t[c], &t[i], 4) == 0) {
                size_t len = 4;
                while (i + len + 5 < n && t[c + len] == t[i + len]) ++len;
                q.push_back({ anchor, i - anchor, i - size_t(c), len });
                for (size_t k = i + 1; k < i + len && k + 4 <= n; ++k) head[h4(k)] = int64_t(k);
                i += len;
                anchor = i;
            } else {
                ++i;
            }
        }
        q.push_back({ anchor, n - anchor, 0, 0 });
        return q;
    }
    // Grows the block by exactly one byte: a match of 5-18 bytes gives its last byte to the next sequence's literals (under 14).
    bool grow_one(std::vector<Seq>& q)
    {
        for (size_t i = 0; i + 1 < q.size(); ++i) {
            auto& a = q[i];
            auto& b = q[i + 1];
            if (a.match_len < 5 || a.match_len - 4 > 14 || b.lit_len >= 14) continue;
            a.match_len--;
            b.lit_start--;
            b.lit_len++;
            return true;
        }
        return false;
    }
    std::vector<uint8_t> lz4_write(const std::vector<Seq>& q, const std::vector<uint8_t>& t)
    {
        std::vector<uint8_t> o;
        auto put_len = [&](size_t v) { if (v < 15) return; v -= 15; while (v >= 255) { o.push_back(255); v -= 255; } o.push_back(uint8_t(v)); };
        for (const auto& x : q) {
            const size_t ml = x.match_len ? x.match_len - 4 : 0;
            o.push_back(uint8_t((std::min<size_t>(x.lit_len, 15) << 4) | std::min<size_t>(ml, 15)));
            put_len(x.lit_len);
            o.insert(o.end(), t.begin() + x.lit_start, t.begin() + x.lit_start + x.lit_len);
            if (!x.match_len) break;
            o.push_back(uint8_t(x.off & 0xff));
            o.push_back(uint8_t(x.off >> 8));
            put_len(ml);
        }
        return o;
    }

    // --- the archive entry ----------------------------------------------------------------------------------------------------
    std::vector<uint8_t> read_file(const std::wstring& p, uint64_t off = 0, size_t n = 0)
    {
        std::ifstream f{ p, std::ios::binary };
        if (!f) return {};
        if (!n) { f.seekg(0, std::ios::end); n = size_t(f.tellg()); }
        std::vector<uint8_t> b(n);
        f.seekg(std::streamoff(off));
        f.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
        if (size_t(f.gcount()) != n) return {};
        return b;
    }
    bool find_entry(const std::wstring& fat)
    {
        const auto b = read_file(fat);
        if (b.size() < 0x18) return false;
        uint32_t n;
        std::memcpy(&n, &b[0x14], 4);
        for (uint32_t i = 0; i < n && 0x18 + size_t(i + 1) * 20 <= b.size(); ++i) {
            uint32_t w[5];
            std::memcpy(w, &b[0x18 + size_t(i) * 20], 20);
            if ((uint64_t(w[1]) << 32 | w[0]) != kHash) continue;
            if ((w[2] & 3) != 2) { g_why = "the config entry is not LZ4 (scheme " + std::to_string(w[2] & 3) + ")"; return false; }
            g_usize = w[2] >> 2;
            g_off   = uint64_t(w[3]) << 3 | (w[4] >> 29);
            g_csize = w[4] & 0x1fffffff;
            return true;
        }
        return false;
    }

    std::string fmt_scale(float v)
    {
        std::string s = fmt::format("{:.3f}", v);
        while (s.size() > 3 && s.back() == '0') s.pop_back();
        return s;
    }

    // The Geometry presets' LodScale and KillLodScale times s (a preset without them, "high", gets s: the game's default is 1).
    // Returns the changes made, empty when the text is not as expected.
    std::string change_text(std::string& t, float s)
    {
        const size_t g0 = t.find("<Geometry>"), g1 = t.find("</Geometry>");
        if (g0 == std::string::npos || g1 == std::string::npos || g1 < g0) return {};
        std::string geo = t.substr(g0, g1 - g0), log;
        // Each <quality ...> opening tag of the section.
        std::string out;
        size_t      pos = 0;
        while (true) {
            const size_t q = geo.find("<quality", pos);
            if (q == std::string::npos) break;
            const size_t e = geo.find('>', q);
            if (e == std::string::npos) return {};
            out += geo.substr(pos, q - pos);
            std::string tag = geo.substr(q, e - q);   // without '>' (and a closing '/')
            const bool  self_close = !tag.empty() && tag.back() == '/';
            if (self_close) tag.pop_back();
            std::smatch m;
            std::string id = std::regex_search(tag, m, std::regex{ R"re(id="([^"]*)")re" }) ? m[1].str() : "?";
            for (const char* key : { "KillLodScale", "LodScale" }) {
                const std::regex r{ std::string{ "(\\s)" } + key + "=\"([0-9.]+)\"" };
                if (std::regex_search(tag, m, r)) {
                    const float v  = std::strtof(m[2].str().c_str(), nullptr);
                    const auto  nv = fmt_scale(v * s);
                    tag            = m.prefix().str() + m[1].str() + key + "=\"" + nv + "\"" + m.suffix().str();
                    log += fmt::format(" {} {} {} -> {};", id, key, m[2].str(), nv);
                } else {
                    while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t' || tag.back() == '\r' || tag.back() == '\n')) tag.pop_back();
                    tag += std::string{ " " } + key + "=\"" + fmt_scale(s) + "\" ";
                    log += fmt::format(" {} {} (1) -> {};", id, key, fmt_scale(s));
                }
            }
            out += tag + (self_close ? "/>" : ">");
            pos = e + 1;
        }
        out += geo.substr(pos);
        t = t.substr(0, g0) + out + t.substr(g1);
        return log;
    }

    // Comment text replaced by spaces (they compress to almost nothing); then spaces added or removed in the longest comment so
    // the text keeps its size.
    bool fit_text(std::string& t, size_t want)
    {
        size_t best = 0, best_len = 0;
        for (size_t p = 0; (p = t.find("<!--", p)) != std::string::npos;) {
            const size_t e = t.find("-->", p + 4);
            if (e == std::string::npos) return false;
            for (size_t k = p + 4; k < e; ++k) if (t[k] != '\n' && t[k] != '\r') t[k] = ' ';
            if (e - p > best_len) { best = p + 4; best_len = e - p - 4; }
            p = e + 3;
        }
        if (t.size() > want) {
            const size_t d = t.size() - want;
            if (best_len < d + 2) return false;
            t.erase(best + 1, d);
        } else if (t.size() < want) {
            t.insert(best + 1, want - t.size(), ' ');
        }
        return t.size() == want;
    }

    bool build(float s)
    {
        const std::wstring dir = std::filesystem::path{ fc5::exe_dir() }.parent_path().parent_path().wstring() + L"\\data_final\\pc\\";
        // patch.dat would override common.dat: the entry is looked for there first.
        for (const wchar_t* a : { L"patch", L"common" }) {
            if (find_entry(dir + a + L".fat")) { g_dat = dir + a + L".dat"; break; }
        }
        if (g_dat.empty()) { if (g_why == "off") g_why = "the config entry was not found in patch.fat or common.fat"; return false; }
        const auto comp = read_file(g_dat, g_off, g_csize);
        std::vector<uint8_t> plain;
        if (comp.size() != g_csize || !lz4_decode(comp.data(), comp.size(), plain, g_usize)) { g_why = "the config entry did not decompress"; return false; }
        std::string t{ plain.begin(), plain.end() };
        const std::string changes = change_text(t, s);
        if (changes.empty()) { g_why = "the config's Geometry section is not as expected"; return false; }
        if (!fit_text(t, g_usize)) { g_why = "no room in the config's comments"; return false; }
        std::vector<uint8_t> nt{ t.begin(), t.end() };
        auto q = lz4_parse(nt);
        size_t sz = encoded_size(q);
        if (sz > g_csize) { g_why = fmt::format("the changed config compresses to {} bytes, more than the {} available", sz, g_csize); return false; }
        while (sz < g_csize && grow_one(q)) sz++;
        if (sz != g_csize) { g_why = fmt::format("could not size the changed config to {} bytes ({})", g_csize, sz); return false; }
        g_block = lz4_write(q, nt);
        std::vector<uint8_t> check;
        if (g_block.size() != g_csize || !lz4_decode(g_block.data(), g_block.size(), check, g_usize) || check != nt) { g_why = "the changed config did not check out"; g_block.clear(); return false; }
        g_why = fmt::format("x{} ({}):{}", fmt_scale(s), std::filesystem::path{ g_dat }.filename().string(), changes);
        return true;
    }

    // --- the read --------------------------------------------------------------------------------------------------------------
    using NtReadFile_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);

    bool is_our_file(HANDLE h)
    {
        wchar_t p[MAX_PATH * 2]{};
        const DWORD n = GetFinalPathNameByHandleW(h, p, DWORD(std::size(p)), FILE_NAME_NORMALIZED);
        if (!n || n >= std::size(p)) return false;
        const std::wstring a{ p }, b = std::filesystem::path{ g_dat }.filename().wstring();
        return a.size() >= b.size() && _wcsicmp(a.c_str() + a.size() - b.size(), b.c_str()) == 0;
    }
    void patch(uint8_t* buf, uint64_t off, uint64_t len)
    {
        const uint64_t a = std::max(off, g_off), b = std::min(off + len, g_off + g_csize);
        if (a >= b) return;
        std::memcpy(buf + (a - off), g_block.data() + (a - g_off), size_t(b - a));
        const int n = ++g_patched;
        spdlog::info("[LodFov] the game read its render config ({} bytes at {} of {} in a {}-byte read): the changed copy given ({} so far); {}", b - a, a,
                     std::filesystem::path{ g_dat }.filename().string(), len, n, g_why);
    }

    NTSTATUS NTAPI hk_read(HANDLE h, HANDLE ev, PVOID apc, PVOID apc_ctx, PIO_STATUS_BLOCK iosb, PVOID buf, ULONG len, PLARGE_INTEGER at, PULONG key)
    {
        // The read's place: the given offset, else the file's own position.
        uint64_t off = 0;
        bool     known = false;
        if (at && at->QuadPart >= 0) { off = uint64_t(at->QuadPart); known = true; }
        else {
            LARGE_INTEGER zero{}, cur{};
            if (SetFilePointerEx(h, zero, &cur, FILE_CURRENT)) { off = uint64_t(cur.QuadPart); known = true; }
        }
        const bool hit = known && off < g_off + g_csize && off + len > g_off && is_our_file(h);
        const NTSTATUS st = g_read_hook.call<NTSTATUS>(h, ev, apc, apc_ctx, iosb, buf, len, at, key);
        if (!hit) return st;
        const int n = ++g_reads;
        if (st == STATUS_PENDING) {
            // Asynchronous: the bytes land later; the changed copy goes in as soon as the read is done (the IO status changes).
            spdlog::info("[LodFov] read {} of the render config is asynchronous ({} bytes at {}): waiting for it", n, len, off);
            for (int i = 0; i < 2000000 && iosb->Status == STATUS_PENDING; ++i) YieldProcessor();
            if (iosb->Status == STATUS_PENDING) { spdlog::warn("[LodFov] the read did not finish: the game's own config is used"); return st; }
        }
        if (NT_OK(st) || st == STATUS_PENDING) patch(static_cast<uint8_t*>(buf), off, len);
        else spdlog::info("[LodFov] read {} of the render config failed ({:#x})", n, uint32_t(st));
        return st;
    }

    float parse(const std::string& arg)
    {
        if (arg == "on") return kOn;
        if (arg == "off" || arg.empty()) return 0.0f;
        const float v = std::strtof(arg.c_str(), nullptr);
        return (v > 0.05f && v < 1.5f) ? v : 0.0f;
    }
}

void install_early()
{
    // The saved line (the settings are applied later than the game reads its config).
    std::ifstream f{ fc5::mod_file("fc5vr_settings.txt") };
    for (std::string line; std::getline(f, line);) {
        std::istringstream ls{ line };
        std::string        cmd, arg;
        ls >> cmd >> arg;
        if (cmd == "lodfov") g_scale = parse(arg);
    }
    const float s = g_scale.load();
    if (s <= 0.0f || std::abs(s - 1.0f) < 1e-3f) { spdlog::info("[LodFov] lodfov off: the game's own render config"); return; }
    if (!build(s)) { spdlog::warn("[LodFov] lodfov {}: not used, {}", fmt_scale(s), g_why); return; }
    auto* p = GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtReadFile");
    if (!p) { spdlog::warn("[LodFov] no NtReadFile: not used"); return; }
    g_read_hook = safetyhook::create_inline(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&hk_read));
    if (!g_read_hook) { spdlog::warn("[LodFov] the read hook failed: not used"); return; }
    g_active = s;
    spdlog::info("[LodFov] lodfov {}: changed render config ready ({} bytes at {} in {}, {} bytes of text): {}", fmt_scale(s), g_csize, g_off,
                 std::filesystem::path{ g_dat }.filename().string(), g_usize, g_why);
}

void set(const std::string& arg)
{
    g_scale = parse(arg);
    spdlog::info("[LodFov] lodfov {} saved: used from the next game start (this run: {})", g_scale.load() > 0.0f ? fmt_scale(g_scale.load()) : "off", describe());
}

float scale() { return g_scale.load(); }

std::string describe()
{
    if (g_active <= 0.0f) return "off";
    return fmt::format("x{}, given to the game {} times", fmt_scale(g_active), g_patched.load());
}
}
