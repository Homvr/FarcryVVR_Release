// poseprobe [frames] (FC2 VR item 1 route B, 23 Oct; the coordinator's static reading of FC_m64): the skeleton pass is lazy, per bone.
// FC_m64+0x7253310 (via the jmp at +0x7dfd10, 62 callers) is the bone getter: rcx = pose, edx = bone; it returns
// [pose+0x10] + bone x 32 (model quat 16 B, then position 12 B), resolving the bone first (+0x70aa130, parents first) when its flag
// byte ([pose+0x18] + bone) says so. The skin builder (+0x84d9380) reads the model transforms through it.
// The probe hooks the getter's entry for N frames (each call: pose, bone, caller, thread, one sequence number shared with the
// palette writer's calls, fc5::skinorder), then logs [PoseProbe]:
//  - every pose read: calls, bones, threads, the main callers;
//  - which pose the weapon-pass arms come from: the pose whose bones cover the arms meshes' bone remaps (the palette writer's
//    rdx, 40 / 41 entries on the rifle);
//  - that pose per bone: parent (int16 at [[pose] + bone x 48 + 0x24]), children, model position, any name the 48 B skeleton
//    entry points to, the first 48 B of 8 entries, and the distance to the game's drawn weapon hand through each weapon-pass
//    skinned draw's object matrix;
//  - the order of the reads of the nearest bone (the right hand) within a frame, by caller and thread, against the palette writer.
#include "FarCry5PoseProbe.h"

#include <safetyhook.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <bitset>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fc5::skinorder
{
    uint64_t next();
    bool     calls(int frame, uint64_t& first, uint64_t& last, uint32_t& n, unsigned long& tid);
}

namespace fc5::poseprobe
{
    namespace
    {
        // pose: the getter's rcx (the skin builder's is a context on its job thread's stack, reused for every character); model: [pose+0x10]
        // read at the call (the instance's model transforms: what identifies a character's pose); skel: [pose].
        struct Call { uintptr_t pose; uintptr_t ret; uint64_t seq; uint32_t bone; int frame; DWORD tid; uintptr_t model; uintptr_t skel; };
        struct TBuf { DWORD tid{ 0 }; std::vector<Call> v; };
        struct Skin { uintptr_t rcx{ 0 }; int frame{ 0 }; uint64_t seq{ 0 }; std::vector<uint16_t> remap; };
        struct Draw { unsigned count{ 0 }; int frame{ 0 }; float obj[12]{}; };

        safetyhook::MidHook            g_getter{};
        std::atomic<bool>              g_armed{ false };
        std::atomic<int>               g_state{ 0 };   // 0 idle, 1 recording, 2 report due
        int                            g_end_frame{ 0 }, g_report_frame{ 0 };
        const std::atomic<int>*        g_frame_src{ nullptr };
        std::mutex                     g_mtx;
        std::vector<TBuf*>             g_bufs;
        thread_local TBuf*             t_buf = nullptr;
        std::vector<Skin>              g_skins;
        std::vector<Draw>              g_draws;
        uintptr_t                      g_fc{ 0 };
        // Verification (state 3): for a few frames after the recording, each arms mesh's skin matrices (palette writer) and each
        // candidate pose's model quats for the same bones. A bone's turn from one update to the next is the same in both (the
        // inverse bind cancels): the player's pose is the candidate whose turns match.
        struct VSample { int frame{ 0 }; std::vector<uint16_t> remap; std::vector<float> skin; std::vector<std::vector<float>> cq; };
        std::vector<Call>              g_all;
        std::vector<uintptr_t>         g_cand_models;
        std::vector<VSample>           g_vs;
        int                            g_verify_end{ 0 };

        bool read_u64(uintptr_t p, uint64_t& v) { __try { v = *reinterpret_cast<const uint64_t*>(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        bool read_bytes(uintptr_t p, void* out, size_t n) { __try { std::memcpy(out, reinterpret_cast<const void*>(p), n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

        void getter_probe(safetyhook::Context& c)
        {
            if (!g_armed.load(std::memory_order_relaxed)) return;
            TBuf* b = t_buf;
            if (!b) {
                b      = new TBuf{};
                b->tid = GetCurrentThreadId();
                b->v.reserve(1 << 16);
                std::lock_guard _{ g_mtx };
                g_bufs.push_back(b);
                t_buf = b;
            }
            if (b->v.size() >= 600000) return;
            uint64_t ret = 0, model = 0, skel = 0;
            read_u64(c.rsp, ret);
            read_u64(c.rcx + 0x10, model);
            read_u64(c.rcx, skel);
            b->v.push_back({ c.rcx, uintptr_t(ret), fc5::skinorder::next(), uint32_t(c.rdx), g_frame_src ? g_frame_src->load() : 0, b->tid, uintptr_t(model), uintptr_t(skel) });
        }

        std::string off(uintptr_t p) { return (g_fc && p > g_fc && p < g_fc + 0x10000000) ? fmt::format("+{:x}", p - g_fc) : fmt::format("{:x}", p); }

        // A printable name the 8-byte value p points to, if any.
        std::string name_at(uint64_t p)
        {
            if (p < 0x10000 || p > 0x7fffffffffffull) return {};
            char s[48]{};
            if (!read_bytes(uintptr_t(p), s, sizeof(s) - 1)) return {};
            int n = 0;
            while (n < 47 && s[n] >= 0x20 && s[n] < 0x7f) ++n;
            return (n >= 3 && s[n] == 0) ? std::string(s, n) : std::string{};
        }

        // After the recording: every thread's calls in one list, and the candidate poses (by model storage) for each arms mesh.
        void prepare()
        {
            {
                std::lock_guard _{ g_mtx };
                g_all.clear();
                for (auto* b : g_bufs) {
                    g_all.insert(g_all.end(), b->v.begin(), b->v.end());
                    b->v.clear();
                }
            }
            std::sort(g_all.begin(), g_all.end(), [](const Call& a, const Call& b) { return a.seq < b.seq; });
            std::unordered_map<uintptr_t, std::bitset<1024>> bones;
            for (const auto& c : g_all) if (c.bone < 1024) bones[c.model ? c.model : c.pose].set(c.bone);
            std::set<std::vector<uint16_t>> remaps;
            for (const auto& s : g_skins) if (s.remap.size() >= 30 && s.remap.size() <= 60) remaps.insert(s.remap);
            std::vector<std::pair<float, uintptr_t>> cov;
            for (const auto& [k, bs] : bones) {
                float best = 0.0f;
                for (const auto& r : remaps) {
                    size_t hit = 0;
                    for (uint16_t b : r) if (b < 1024 && bs.test(b)) hit++;
                    best = std::max(best, float(hit) / float(r.size()));
                }
                if (best >= 0.75f) cov.push_back({ best, k });
            }
            std::sort(cov.rbegin(), cov.rend());
            g_cand_models.clear();
            for (size_t i = 0; i < cov.size() && i < 16; ++i) g_cand_models.push_back(cov[i].second);
            g_vs.clear();
            spdlog::info("[PoseProbe] {} calls merged; {} candidate poses for {} arms meshes: verifying their bones' turns against the skin matrices", g_all.size(),
                         g_cand_models.size(), remaps.size());
        }

        // Rotation angle (deg) between two 3x3 rotations given as 9 floats (rows normalised; layout-agnostic).
        float rot_angle(const float* a, const float* b)
        {
            float tr = 0.0f;
            for (int r = 0; r < 3; ++r) {
                const float na = std::sqrt(a[r * 3] * a[r * 3] + a[r * 3 + 1] * a[r * 3 + 1] + a[r * 3 + 2] * a[r * 3 + 2]);
                const float nb = std::sqrt(b[r * 3] * b[r * 3] + b[r * 3 + 1] * b[r * 3 + 1] + b[r * 3 + 2] * b[r * 3 + 2]);
                if (na < 1e-6f || nb < 1e-6f) return -1.0f;
                for (int c = 0; c < 3; ++c) tr += a[r * 3 + c] * b[r * 3 + c] / (na * nb);
            }
            return std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.29578f;
        }
        float quat_angle(const float* a, const float* b)
        {
            const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]);
            if (la < 1e-6f || lb < 1e-6f) return -1.0f;
            const float d = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) / (la * lb);
            return 2.0f * std::acos(std::min(1.0f, d)) * 57.29578f;
        }

        void report(unsigned long render_tid, glm::vec3 hand, bool hand_ok, const glm::vec3& grip, bool grip_ok)
        {
            std::vector<Call>&      all = g_all;
            std::map<DWORD, size_t> per_thread;
            for (const auto& c : all) per_thread[c.tid]++;
            std::string th;
            for (const auto& [tid, n] : per_thread) th += fmt::format(" {}{}:{}", tid, tid == render_tid ? "(render)" : "", n);
            spdlog::info("[PoseProbe] {} getter calls over frames {}-{}; by thread{}", all.size(), all.empty() ? 0 : all.front().frame, all.empty() ? 0 : all.back().frame, th);
            if (all.empty()) return;

            struct PoseAgg { uint64_t calls{ 0 }; std::bitset<1024> bones; int max_bone{ -1 }; std::set<DWORD> tids; std::map<uintptr_t, uint64_t> callers; uintptr_t skel{ 0 }; std::set<uintptr_t> ctx; };
            std::unordered_map<uintptr_t, PoseAgg> poses;
            // By the model transforms' storage (one per character instance), not the getter's rcx (a reused context).
            auto key_of = [](const Call& c) { return c.model ? c.model : c.pose; };
            for (const auto& c : all) {
                auto& p = poses[key_of(c)];
                p.calls++;
                p.tids.insert(c.tid);
                p.skel = c.skel;
                if (p.ctx.size() < 8) p.ctx.insert(c.pose);
                if (c.bone < 1024) p.bones.set(c.bone);
                p.max_bone = std::max(p.max_bone, int(c.bone));
                p.callers[c.ret]++;
            }
            std::vector<std::pair<uintptr_t, const PoseAgg*>> order;
            for (const auto& [k, v] : poses) order.push_back({ k, &v });
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second->calls > b.second->calls; });
            spdlog::info("[PoseProbe] {} poses read (by model storage [ctx+0x10]); the 12 most read:", poses.size());
            for (size_t i = 0; i < order.size() && i < 12; ++i) {
                const auto& [k, p] = order[i];
                std::vector<std::pair<uint64_t, uintptr_t>> cs;
                for (const auto& [r, n] : p->callers) cs.push_back({ n, r });
                std::sort(cs.rbegin(), cs.rend());
                std::string cl;
                for (size_t j = 0; j < cs.size() && j < 4; ++j) cl += fmt::format(" {} x{}", off(cs[j].second), cs[j].first);
                std::string tl;
                for (DWORD id : p->tids) tl += fmt::format(" {}{}", id, id == render_tid ? "(render)" : "");
                spdlog::info("[PoseProbe]   model {:x} (skeleton {:x}, {} contexts): {} calls, {} bones (max {}), threads{}; callers{}", k, p->skel, p->ctx.size(), p->calls, p->bones.count(), p->max_bone, tl, cl);
            }

            // The arms: the weapon-pass skinned meshes' remaps (30-60 entries), each matched to the pose covering most of it.
            std::vector<const Skin*> arms;
            {
                std::set<std::vector<uint16_t>> seen;
                for (const auto& s : g_skins) {
                    if (s.remap.size() < 30 || s.remap.size() > 60 || !seen.insert(s.remap).second) continue;
                    arms.push_back(&s);
                }
            }
            // Candidates: every pose covering 75 % or more of an arms mesh's remap (the getter reads only the bones someone asks for,
            // so coverage alone does not decide: a whole body covers a small rig's indices too).
            std::vector<std::pair<float, uintptr_t>> cands;
            for (const auto* s : arms) {
                uint16_t mx = 0;
                for (uint16_t b : s->remap) mx = std::max(mx, b);
                std::vector<std::pair<float, uintptr_t>> cov;
                for (const auto& [k, p] : poses) {
                    size_t hit = 0;
                    for (uint16_t b : s->remap) if (b < 1024 && p.bones.test(b)) hit++;
                    cov.push_back({ float(hit) / float(s->remap.size()), k });
                }
                std::sort(cov.rbegin(), cov.rend());
                std::string top;
                for (size_t i = 0; i < cov.size() && i < 6; ++i) top += fmt::format(" {:x} {:.0f} % (max bone {})", cov[i].second, 100.0f * cov[i].first, poses[cov[i].second].max_bone);
                spdlog::info("[PoseProbe] mesh with {} remap entries (skin matrices {:x}, bones up to {}): covered by{}", s->remap.size(), s->rcx, mx, top);
                for (const auto& c : cov) {
                    if (c.first < 0.75f) break;
                    if (std::none_of(cands.begin(), cands.end(), [&](const auto& x) { return x.second == c.second; })) cands.push_back(c);
                }
            }
            std::sort(cands.begin(), cands.end(), [&](const auto& a, const auto& b) { return poses[a.second].max_bone < poses[b.second].max_bone; });   // small rigs first
            // Verification: per arms mesh and candidate, the mean difference between the bones' turns (deg) in the skin matrices and in
            // the candidate's model quats, the candidate taken 1 update earlier, the same update, or 1 later; the best match goes first.
            uintptr_t verified = 0;
            float     verified_err = 1e9f;
            {
                std::map<size_t, std::vector<const VSample*>> by_mesh;
                for (const auto& s : g_vs) by_mesh[s.remap.size()].push_back(&s);
                for (auto& [n, vs] : by_mesh) {
                    std::sort(vs.begin(), vs.end(), [](const VSample* a, const VSample* b) { return a->frame < b->frame; });
                    // one sample per frame
                    std::vector<const VSample*> u;
                    for (const auto* s : vs) if (u.empty() || u.back()->frame != s->frame) u.push_back(s);
                    float moving = 0.0f;
                    int   moving_n = 0;
                    std::vector<std::string> lines;
                    for (size_t ci = 0; ci < g_cand_models.size(); ++ci) {
                        float best = 1e9f;
                        int   best_lag = 0;
                        for (int lag = -1; lag <= 1; ++lag) {
                            double sum = 0.0;
                            int    cnt = 0;
                            for (size_t k = 1; k + 1 < u.size(); ++k) {
                                const size_t kc = k + lag;
                                if (kc < 1 || kc >= u.size()) continue;
                                const VSample* s1 = u[k - 1];
                                const VSample* s2 = u[k];
                                const VSample* c1 = u[kc - 1];
                                const VSample* c2 = u[kc];
                                if (ci >= c1->cq.size() || ci >= c2->cq.size() || s1->remap != s2->remap || c1->remap != s1->remap || c2->remap != s1->remap) continue;
                                for (size_t b = 0; b < s1->remap.size() && b < 64; ++b) {
                                    const float as = rot_angle(&s1->skin[b * 9], &s2->skin[b * 9]);
                                    const float ac = quat_angle(&c1->cq[ci][b * 4], &c2->cq[ci][b * 4]);
                                    if (as < 0.0f || ac < 0.0f || as < 0.05f) continue;
                                    sum += std::abs(as - ac);
                                    cnt++;
                                    if (lag == 0 && ci == 0) { moving += as; moving_n++; }
                                }
                            }
                            const float e = cnt ? float(sum / cnt) : 1e9f;
                            if (e < best) { best = e; best_lag = lag; }
                        }
                        lines.push_back(fmt::format(" {:x} {:.3f} deg (lag {})", g_cand_models[ci], best, best_lag));
                        if (best < verified_err) { verified_err = best; verified = g_cand_models[ci]; }
                    }
                    std::string all_l;
                    for (const auto& l : lines) all_l += l;
                    spdlog::info("[PoseProbe] verify, mesh with {} remap entries: {} samples, the bones turned {:.2f} deg per update on average; candidates' mean turn error:{}", n,
                                 u.size(), moving_n ? moving / moving_n : 0.0f, all_l);
                }
            }
            if (verified) {
                spdlog::info("[PoseProbe] the player's arms pose (verified): model transforms {:x}, mean turn error {:.3f} deg", verified, verified_err);
                cands.erase(std::remove_if(cands.begin(), cands.end(), [&](const auto& c) { return c.second == verified; }), cands.end());
                cands.insert(cands.begin(), { 1.0f, verified });
            }
            if (cands.size() > 6) cands.resize(6);
            if (cands.empty()) {
                spdlog::info("[PoseProbe] no pose covers an arms mesh's bones (meshes seen: {}): nothing more", arms.size());
                return;
            }
            if (!hand_ok && grip_ok) {
                hand    = grip;
                hand_ok = true;
                spdlog::info("[PoseProbe] no glove hand measured: the weapon's grip point (gunstill's, game camera) used as the hand");
            }
            const int last = all.back().frame;
            for (const auto& [cv, chosen] : cands) {
                const uint64_t skel = poses[chosen].skel, model = chosen;
                const int      nb   = std::min(poses[chosen].max_bone + 1, 1024);
                const bool     dump = nb <= 200 || chosen == verified;
                std::string    ctxs;
                for (auto p : poses[chosen].ctx) ctxs += fmt::format(" {:x}", p);
                std::vector<int>       parent(nb, -2);
                std::vector<glm::vec3> pos(nb, glm::vec3{ 0.0f });
                std::vector<int>       kids(nb, 0);
                for (int b = 0; b < nb; ++b) {
                    int16_t pa = -2;
                    if (read_bytes(uintptr_t(skel + b * 48 + 0x24), &pa, 2)) parent[b] = pa;
                    float m[8]{};
                    if (read_bytes(uintptr_t(model + b * 32), m, sizeof(m))) pos[b] = glm::vec3{ m[4], m[5], m[6] };
                }
                for (int b = 0; b < nb; ++b) if (parent[b] >= 0 && parent[b] < nb) kids[parent[b]]++;
                // The drawn hand through each recorded skinned draw's object matrix: the nearest bone.
                int         near_b = -1;
                float       near_d = 1e9f;
                std::string near_draw;
                if (hand_ok) {
                    std::set<unsigned> done;
                    for (auto it = g_draws.rbegin(); it != g_draws.rend(); ++it) {
                        // Only a root's object matrix (a heading only: its third row (0, 0, 1)), the arms' and gloves'; not the gun's.
                        if (std::abs(it->obj[8]) > 0.02f || std::abs(it->obj[9]) > 0.02f || std::abs(it->obj[10] - 1.0f) > 0.02f || std::abs(it->obj[2]) > 0.02f || std::abs(it->obj[6]) > 0.02f) continue;
                        if (!done.insert(it->count).second) continue;
                        glm::mat4 Om{ 1.0f };
                        for (int r = 0; r < 3; ++r) for (int col = 0; col < 4; ++col) Om[col][r] = it->obj[r * 4 + col];
                        for (int b = 0; b < nb; ++b) {
                            const float d = glm::length(glm::vec3{ Om * glm::vec4{ pos[b], 1.0f } } - hand);
                            if (d < near_d) { near_d = d; near_b = b; near_draw = fmt::format("draw of {} indices", it->count); }
                        }
                    }
                }
                spdlog::info("[PoseProbe] candidate: model transforms {:x}, skeleton table {:x}, {} bones (0-{}), covers {:.0f} %, contexts{}; the drawn hand ({:.3f}, {:.3f}, {:.3f}) "
                             "nearest bone {} at {:.1f} cm (through the {})",
                             model, skel, poses[chosen].bones.count(), nb - 1, 100.0f * cv, ctxs, hand.x, hand.y, hand.z, near_b, 100.0f * near_d, near_draw);
                if (dump) {
                    for (int b = 0; b < nb; ++b) {
                        uint64_t e[6]{};
                        read_bytes(uintptr_t(skel + b * 48), e, sizeof(e));
                        std::string names;
                        for (int k = 0; k < 6; ++k) if (auto s = name_at(e[k]); !s.empty()) names += fmt::format(" [+{:x}] '{}'", k * 8, s);
                        std::string raw;
                        if (b < 8) for (int k = 0; k < 6; ++k) raw += fmt::format(" {:016x}", e[k]);
                        spdlog::info("[PoseProbe]   {:x} bone {:3} parent {:4} children {} model ({: .3f}, {: .3f}, {: .3f}){}{}{}", model, b, parent[b], kids[b], pos[b].x, pos[b].y,
                                     pos[b].z, b == near_b ? " <- nearest the drawn hand" : "", names, raw.empty() ? "" : " |" + raw);
                    }
                }
                // The order of the reads of the nearest bone (and its parent) within the last two whole frames, by caller and thread.
                if (near_b < 0) continue;
                for (int f = last - 2; f <= last - 1; ++f) {
                    uint64_t      w0 = 0, w1 = 0;
                    uint32_t      wn = 0;
                    unsigned long wt = 0;
                    const bool    w = fc5::skinorder::calls(f, w0, w1, wn, wt);
                    std::string   seq;
                    int           groups = 0;
                    uintptr_t     g_ret = 0;
                    DWORD         g_tid = 0;
                    uint64_t      g_first = 0, g_n = 0;
                    auto flush = [&] {
                        if (g_n && groups < 24) seq += fmt::format(" | {} on {}{} x{} from seq {}", off(g_ret), g_tid, g_tid == render_tid ? "(render)" : "", g_n, g_first);
                        if (g_n) groups++;
                        g_n = 0;
                    };
                    for (const auto& c : all) {
                        if (c.frame != f || key_of(c) != chosen || (int(c.bone) != near_b && int(c.bone) != parent[near_b])) continue;
                        if (c.ret != g_ret || c.tid != g_tid) { flush(); g_ret = c.ret; g_tid = c.tid; g_first = c.seq; }
                        g_n++;
                    }
                    flush();
                    spdlog::info("[PoseProbe]   {:x} frame {}: reads of bone {} and its parent {}{} ; palette writer calls this frame {} (seq {}-{})", model, f, near_b, parent[near_b], seq,
                                 w ? wn : 0, w0, w1);
                }
            }
            spdlog::info("[PoseProbe] report done");
        }
    }
    bool start(int frames, const std::atomic<int>* frame)
    {
        g_frame_src = frame;
        auto* base  = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        g_fc        = reinterpret_cast<uintptr_t>(base);
        if (!base) return false;
        if (!g_getter) {
            static const uint8_t kEntry[16]{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57 };
            if (std::memcmp(base + 0x7253310, kEntry, sizeof(kEntry)) != 0) {
                std::string got;
                for (int k = 0; k < 16; ++k) got += fmt::format(" {:02x}", base[0x7253310 + k]);
                spdlog::info("[PoseProbe] the bone getter's entry bytes differ at FC_m64+0x7253310:{}", got);
                return false;
            }
            if (auto h = safetyhook::MidHook::create(base + 0x7253310, getter_probe); h) g_getter = std::move(*h);
            else {
                spdlog::info("[PoseProbe] mid hook at FC_m64+0x7253310 failed (error type {})", (int)h.error().type);
                return false;
            }
        }
        {
            std::lock_guard _{ g_mtx };
            for (auto* b : g_bufs) b->v.clear();
            g_skins.clear();
            g_draws.clear();
        }
        const int f = frame ? frame->load() : 0;
        g_end_frame = f + std::max(2, frames);
        g_state     = 1;
        g_armed     = true;
        spdlog::info("[PoseProbe] bone getter hooked at FC_m64+0x7253310; recording frames {}-{}", f, g_end_frame);
        return true;
    }

    bool armed() { return g_state.load() == 1 || g_state.load() == 3; }

    void note_skin(uintptr_t rcx, uintptr_t rdx)
    {
        uint64_t list = 0, cnt = 0;
        if (!read_u64(rdx, list) || !read_u64(rdx + 8, cnt)) return;
        const uint32_t n = uint32_t(cnt >> 32) & 0x7fffffff;
        if (!list || n == 0 || n > 512) return;
        if (g_state.load() == 3) {
            // Verification sample: an arms mesh's skin rotations and each candidate's model quats for its bones.
            if (n < 30 || n > 60) return;
            VSample v{};
            v.frame = g_frame_src ? g_frame_src->load() : 0;
            v.remap.resize(n);
            if (!read_bytes(uintptr_t(list), v.remap.data(), n * 2)) return;
            v.skin.resize(size_t(n) * 9);
            for (uint32_t i = 0; i < n; ++i) {
                float m[16]{};
                read_bytes(rcx + uintptr_t(v.remap[i]) * 64, m, sizeof(m));
                for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) v.skin[i * 9 + r * 3 + c] = m[r * 4 + c];
            }
            v.cq.resize(g_cand_models.size());
            for (size_t ci = 0; ci < g_cand_models.size(); ++ci) {
                v.cq[ci].resize(size_t(n) * 4);
                for (uint32_t i = 0; i < n; ++i) read_bytes(g_cand_models[ci] + uintptr_t(v.remap[i]) * 32, &v.cq[ci][i * 4], 16);
            }
            std::lock_guard _{ g_mtx };
            if (g_vs.size() < 400) g_vs.push_back(std::move(v));
            return;
        }
        Skin s{};
        s.rcx   = rcx;
        s.frame = g_frame_src ? g_frame_src->load() : 0;
        s.seq   = fc5::skinorder::next();
        s.remap.resize(n);
        if (!read_bytes(uintptr_t(list), s.remap.data(), n * 2)) return;
        std::lock_guard _{ g_mtx };
        if (g_skins.size() < 4000) g_skins.push_back(std::move(s));
    }

    void note_draw(unsigned count, const float* obj12)
    {
        Draw d{};
        d.count = count;
        d.frame = g_frame_src ? g_frame_src->load() : 0;
        std::memcpy(d.obj, obj12, sizeof(d.obj));
        std::lock_guard _{ g_mtx };
        if (g_draws.size() < 2000) g_draws.push_back(d);
    }

    void present(int frame, unsigned long render_tid, const glm::vec3& hand_world, bool hand_ok, const glm::vec3& grip_world, bool grip_ok)
    {
        const int st = g_state.load();
        if (st == 1 && frame >= g_end_frame) {
            g_armed        = false;
            g_state        = 2;
            g_report_frame = frame + 3;   // calls in flight on the job threads finish first
        } else if (st == 2 && frame >= g_report_frame) {
            prepare();
            g_verify_end = frame + 16;
            g_state      = 3;
        } else if (st == 3 && frame >= g_verify_end) {
            g_state = 0;
            report(render_tid, hand_world, hand_ok, grip_world, grip_ok);
        }
    }
}

// throwarc's probe (Fc2Vr.cpp): after a throw's release, every entity the writer sets near the hand is followed for 2.5 s, and the launch
// of the one that flew furthest is fitted. It once shared this writer hook with skelhand (removed 30 Oct with the F11 Experimental
// section "Research for alternatives").
namespace fc5::throwprobe
{
    // The entity writer: where the game sets an entity's world transform (FC_m64+0x6a10660, reached by the jmp at +0x4db3b0 from
    // +0xc85c1f on the animation job threads): it stores a 4x4 column-major matrix at [rbx+0x10] and the scale at [rbx+0x50]. Hooked
    // right after the matrix stores (+0x6a10785, mov ecx,[rsi] / mov [rbx+0x50],ecx).
    safetyhook::MidHook g_writer_hook{};
    bool rd(uintptr_t p, void* out, size_t n) { __try { std::memcpy(out, reinterpret_cast<const void*>(p), n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

    // throwarc's probe (see the header): every entity set near the hand after a release, its positions (one per change) for 2.5 s.
    struct TrackPt { float t; glm::vec3 p; };
    std::mutex                                       g_tp_mtx;
    std::atomic<bool>                                g_tp_on{ false };
    std::chrono::steady_clock::time_point            g_tp_t0{};
    glm::vec3                                        g_tp_hand{ 0.0f };
    std::unordered_map<uintptr_t, std::vector<TrackPt>> g_tp;
    void throw_track(uintptr_t ent, const glm::vec3& p)
    {
        const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_tp_t0).count();
        if (t > 2.5f) return;
        std::lock_guard _{ g_tp_mtx };
        auto it = g_tp.find(ent);
        if (it == g_tp.end()) {
            if (t > 1.5f || glm::length(p - g_tp_hand) > 2.0f || g_tp.size() > 600) return;
            it = g_tp.emplace(ent, std::vector<TrackPt>{}).first;
        }
        auto& v = it->second;
        if (!v.empty() && glm::length(v.back().p - p) < 1e-5f) return;
        if (v.size() < 400) v.push_back({ t, p });
    }

    void writer_probe(safetyhook::Context& c)
    {
        if (!g_tp_on.load(std::memory_order_relaxed)) return;
        float p3[3]{};
        if (rd(c.rbx + 0x10 + 48, p3, sizeof(p3))) throw_track(c.rbx, glm::vec3{ p3[0], p3[1], p3[2] });
    }

    bool install_writer_hook()
    {
        static std::mutex mx;
        std::lock_guard   _{ mx };
        if (g_writer_hook) return true;
        auto*                base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        static const uint8_t kAt[5]{ 0x8B, 0x0E, 0x89, 0x4B, 0x50 };
        if (base && std::memcmp(base + 0x6a10785, kAt, sizeof(kAt)) == 0) {
            g_writer_hook = safetyhook::create_mid(base + 0x6a10785, writer_probe);
            spdlog::info("[ThrowArc] entity writer hook at FC_m64+0x6a10785 {}", g_writer_hook ? "installed" : "failed");
        } else {
            spdlog::info("[ThrowArc] the code differs at FC_m64+0x6a10785: no throw measure");
        }
        return bool(g_writer_hook);
    }

    void arm(const glm::vec3& hand)
    {
        if (!install_writer_hook()) return;
        std::lock_guard _{ g_tp_mtx };
        g_tp.clear();
        g_tp_hand = hand;
        g_tp_t0   = std::chrono::steady_clock::now();
        g_tp_on   = true;
    }
    // The object that flew furthest from the hand; its launch: the first sample moving over 3 m/s, then p(t) = p0 + v0 t + a t^2 / 2
    // fitted (least squares, per axis) over the next 0.5 s.
    ThrowFit fit(const glm::vec3& head_dir)
    {
        ThrowFit r;
        std::lock_guard _{ g_tp_mtx };
        g_tp_on   = false;
        r.tracked = int(g_tp.size());
        const std::vector<TrackPt>* best = nullptr;
        float                       bd   = 0.0f;
        struct Cand { float dist; float vmax; const std::vector<TrackPt>* v; uintptr_t e; };
        std::vector<Cand> cands;
        for (const auto& [e, v] : g_tp) {
            float furthest = 0.0f, vmax = 0.0f;   // (far is a Windows macro)
            for (size_t i = 0; i < v.size(); ++i) {
                furthest = std::max(furthest, glm::length(v[i].p - v.front().p));
                if (i > 0 && v[i].t - v[i - 1].t > 1e-3f) vmax = std::max(vmax, glm::length(v[i].p - v[i - 1].p) / (v[i].t - v[i - 1].t));
            }
            cands.push_back({ furthest, vmax, &v, e });
            if (v.size() < 6 || vmax > 100.0f) continue;   // a teleport (a pooled object put elsewhere), not a throw
            // Not the thrown object (Homard's headset, 24 Oct: two fits flew 146-175 deg from where he looked): it must start within
            // 1 m of the throwing hand and fly within 60 deg (sideways) of the head's direction.
            if (glm::length(v.front().p - g_tp_hand) > 1.0f) { r.rejected++; continue; }
            const glm::vec2 hh{ head_dir.x, head_dir.y }, mv{ v.back().p.x - v.front().p.x, v.back().p.y - v.front().p.y };
            if (glm::length(hh) > 1e-3f && glm::length(mv) > 1.0f && glm::dot(glm::normalize(hh), glm::normalize(mv)) < 0.5f) { r.rejected++; continue; }
            if (furthest > bd) { bd = furthest; best = &v; }
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist > b.dist; });
        for (size_t i = 0; i < cands.size() && i < 6; ++i) {
            const auto& v = *cands[i].v;
            r.path += fmt::format(" | {:x}: {} samples from {:.2f} s, moved {:.1f} m, fastest step {:.0f} m/s, first ({:.2f} {:.2f} {:.2f}) last ({:.2f} {:.2f} {:.2f}) from the hand", cands[i].e, v.size(),
                                  v.front().t, cands[i].dist, cands[i].vmax, v.front().p.x - g_tp_hand.x, v.front().p.y - g_tp_hand.y, v.front().p.z - g_tp_hand.z, v.back().p.x - g_tp_hand.x,
                                  v.back().p.y - g_tp_hand.y, v.back().p.z - g_tp_hand.z);
        }
        r.dist = bd;
        if (!best || bd < 3.0f) return r;
        r.path += " | fitted:";
        const auto& v  = *best;
        // The flight starts where the object leaves the hand for good: the first sample over 1 m from the hand (in the hand it swings at
        // up to 12 m/s with the throw animation, simulator 24 Oct), taken from the sample before it.
        size_t i0 = 0;
        for (size_t i = 1; i < v.size(); ++i)
            if (glm::length(v[i].p - g_tp_hand) > 1.0f) { i0 = i - 1; break; }
        // Over the next 0.5 s; 1 s when that holds fewer than 5 samples (the simulator at ~35 fps wrote a flying knife 3-4 times in 0.5 s).
        double S[5]{}, Bx[3][3]{};
        int    n   = 0;
        float  win = 0.5f;
        {
            int k = 0;
            for (size_t i = i0; i < v.size() && v[i].t - v[i0].t <= 0.5f; ++i) ++k;
            if (k < 5) win = 1.0f;
        }
        for (size_t i = i0; i < v.size() && v[i].t - v[i0].t <= win; ++i, ++n) {
            const double tt = v[i].t - v[i0].t, pw[3]{ 1.0, tt, tt * tt };
            for (int k = 0; k < 5; ++k) S[k] += std::pow(tt, k);
            for (int ax = 0; ax < 3; ++ax) for (int k = 0; k < 3; ++k) Bx[ax][k] += v[i].p[ax] * pw[k];
        }
        if (n < 5) return r;
        // Normal equations [S0 S1 S2; S1 S2 S3; S2 S3 S4] x = b, solved by Cramer's rule.
        auto det3 = [](double a, double b, double c, double d, double e, double f, double g, double h, double i) { return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g); };
        const double D = det3(S[0], S[1], S[2], S[1], S[2], S[3], S[2], S[3], S[4]);
        if (std::abs(D) < 1e-12) return r;
        double coef[3][3];
        for (int ax = 0; ax < 3; ++ax) {
            const double* b = Bx[ax];
            coef[ax][0]     = det3(b[0], S[1], S[2], b[1], S[2], S[3], b[2], S[3], S[4]) / D;
            coef[ax][1]     = det3(S[0], b[0], S[2], S[1], b[1], S[3], S[2], b[2], S[4]) / D;
            coef[ax][2]     = det3(S[0], S[1], b[0], S[1], S[2], b[1], S[2], S[3], b[2]) / D;
        }
        r.p0       = glm::vec3{ float(coef[0][0]), float(coef[1][0]), float(coef[2][0]) };
        r.v0       = glm::vec3{ float(coef[0][1]), float(coef[1][1]), float(coef[2][1]) };
        r.g        = float(-2.0 * coef[2][2]);
        r.t_launch = v[i0].t;
        r.samples  = n;
        r.ok       = true;
        for (size_t i = i0; i < v.size() && i < i0 + 12; ++i)
            r.path += fmt::format(" {:.3f}s ({:.2f} {:.2f} {:.2f})", v[i].t, v[i].p.x - g_tp_hand.x, v[i].p.y - g_tp_hand.y, v[i].p.z - g_tp_hand.z);
        return r;
    }
}
