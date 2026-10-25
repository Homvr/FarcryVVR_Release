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

// skelhand (below): its work in the getter's hook.
namespace fc5::skelhand
{
    void on_getter(safetyhook::Context& c);
    int  mode();
}

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
            if (fc5::skelhand::mode() > 0) fc5::skelhand::on_getter(c);
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
    float rot_angle_deg(const float* a, const float* b) { return rot_angle(a, b); }
    float quat_angle_deg(const float* a, const float* b) { return quat_angle(a, b); }

    bool install_getter(const std::atomic<int>* frame)
    {
        if (frame) g_frame_src = frame;
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        g_fc       = reinterpret_cast<uintptr_t>(base);
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
        return true;
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

// skelhand 0|1|2 (FC2 VR item 1 route B, 24 Oct): the player's weapon hand put on the controller in the game's own skeleton, so the
// gun (attached through the hand's subtree), its muzzle flash and tracers follow. In the getter's hook (job threads): on the first read
// of a bone under B (mode 1: the hand, bone 194; mode 2: the shoulder, 191) in an update (B still marked unresolved), the hand is
// resolved through the getter itself, B's model transform is replaced by D x B (D: the render thread's model-space move, the mod's
// controller placement without gunstill taken through the arms' root matrix), and B's descendants are marked unresolved, so the
// fingers, the helper bones 320-324 and the gun attachment resolve again from the moved hand. The player's pose is the model storage
// [ctx+0x10] read by the callers +725a23c / +725a2ec (seen on the player only), checked every 2 s against the arms' skin matrices
// (the bones' turns between updates must match). The quaternion layout is taken from a child's position (self-test).
namespace fc5::skelhand
{
    namespace
    {
        std::atomic<int>       g_mode{ 0 };
        std::atomic<uintptr_t> g_player_model{ 0 }, g_player_skel{ 0 };
        std::atomic<bool>      g_player_ok{ false };
        std::atomic<int>       g_conv{ -1 };          // bit 0 wxyz layout, bit 1 transposed rotation, bit 2 local position scaled; -1 unknown, -2 no fit
        std::atomic<uint32_t>  g_dirty_mask{ 0x08 };  // the flag bit the resolve clears (simulator 24 Oct: 0b -> 03 for the hand, 0a -> 02 below it); re-learned at the first move
        std::mutex             g_t_mtx;
        struct Target { bool ok{ false }; int frame{ -100 }; glm::mat4 D{ 1.0f }; };
        Target                 g_target{};
        std::mutex             g_tree_mtx;
        uintptr_t              g_tree_skel{ 0 };
        std::vector<int>       g_parent;
        std::vector<uint8_t>   g_sub[3];   // descendants of 194, of 191, of the left shoulder (skelhandarms)
        int                    g_chain[2][4]{ { 191, 192, 193, 194 }, { -1, -1, -1, 344 } };   // shoulder, elbow, wrist, hand: right, left (from the left hand 344 up)
        std::atomic<bool>      g_arms{ false };   // skelhandarms: two-bone IK of both arms onto the moved hands (mode 3)
        std::atomic<uint64_t>  g_ik_applied[2]{}, g_ik_clamped[2]{}, g_ik_reached[2]{};
        std::atomic<int>       g_ik_log{ 12 };
        thread_local bool      t_in = false;
        std::atomic<uint64_t>  g_applied{ 0 }, g_no_target{ 0 }, g_not_player{ 0 };
        std::atomic<int>       g_delta_log{ 100 }, g_order_log{ 50 }, g_flag_log{ 40 };
        int                    g_chk_bad{ 0 };
        std::atomic<int>       g_apply_frame{ -100 };
        std::atomic<uint64_t>  g_apply_seq{ 0 };
        uintptr_t              g_fcb{ 0 };
        const std::atomic<int>* g_fsrc{ nullptr };
        // the identity check (render thread)
        std::vector<uint16_t>  g_chk_remap;
        std::vector<float>     g_chk_skin, g_chk_q;
        int                    g_chk_frame{ -100 };
        double                 g_chk_err{ 0.0 };
        int                    g_chk_n{ 0 };
        std::chrono::steady_clock::time_point g_chk_t{};

        bool rd(uintptr_t p, void* out, size_t n) { __try { std::memcpy(out, reinterpret_cast<const void*>(p), n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        bool wr(uintptr_t p, const void* in, size_t n) { __try { std::memcpy(reinterpret_cast<void*>(p), in, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        using Getter = void*(__fastcall*)(uintptr_t, uint32_t);
        void resolve(uintptr_t ctx, uint32_t bone)
        {
            __try { reinterpret_cast<Getter>(g_fcb + 0x7253310)(ctx, bone); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        glm::mat4 decode(const float* m, int conv)
        {
            glm::quat q = (conv & 1) ? glm::quat{ m[0], m[1], m[2], m[3] } : glm::quat{ m[3], m[0], m[1], m[2] };   // glm::quat{w, x, y, z}
            glm::mat3 R = glm::mat3_cast(glm::normalize(q));
            if (conv & 2) R = glm::transpose(R);
            glm::mat4 M{ R };
            M[3] = glm::vec4{ m[4], m[5], m[6], 1.0f };
            return M;
        }
        void encode(const glm::mat4& M, int conv, float* m)
        {
            glm::mat3 R{ M };
            if (conv & 2) R = glm::transpose(R);
            glm::quat       q   = glm::normalize(glm::quat_cast(R));
            const glm::quat old = (conv & 1) ? glm::quat{ m[0], m[1], m[2], m[3] } : glm::quat{ m[3], m[0], m[1], m[2] };
            if (glm::dot(q, old) < 0.0f) q = -q;
            if (conv & 1) { m[0] = q.w; m[1] = q.x; m[2] = q.y; m[3] = q.z; }
            else { m[0] = q.x; m[1] = q.y; m[2] = q.z; m[3] = q.w; }
            m[4] = M[3].x; m[5] = M[3].y; m[6] = M[3].z;
        }

        bool tree(uintptr_t skel)
        {
            std::lock_guard _{ g_tree_mtx };
            if (g_tree_skel == skel && g_parent.size() > 194) return true;
            g_parent.clear();
            for (int b = 0; b < 1024; ++b) {
                int16_t pa = -2;
                if (!rd(skel + uintptr_t(b) * 48 + 0x24, &pa, 2) || pa < -1 || (b > 0 && pa >= b) || (b == 0 && pa != -1)) break;
                g_parent.push_back(pa);
            }
            const size_t n = g_parent.size();
            // The left arm from its hand up (shoulder - elbow - wrist - hand, as the right arm's 191 - 192 - 193 - 194).
            g_chain[1][2] = n > 344 ? g_parent[344] : -1;
            g_chain[1][1] = g_chain[1][2] >= 0 ? g_parent[g_chain[1][2]] : -1;
            g_chain[1][0] = g_chain[1][1] >= 0 ? g_parent[g_chain[1][1]] : -1;
            const int lsh = g_chain[1][0] > 0 ? g_chain[1][0] : 100000;
            for (int k = 0; k < 3; ++k) {
                const int root = k == 0 ? 194 : (k == 1 ? 191 : lsh);
                g_sub[k].assign(n, 0);
                for (size_t b = 0; b < n; ++b) g_sub[k][b] = (int(b) == root || (g_parent[b] >= 0 && g_sub[k][g_parent[b]])) ? 1 : 0;
            }
            g_tree_skel = skel;
            size_t c194 = 0, c191 = 0;
            for (size_t b = 0; b < n; ++b) { c194 += g_sub[0][b]; c191 += g_sub[1][b]; }
            spdlog::info("[SkelHand] skeleton {:x}: {} bones; under the hand (194) {}, under the shoulder (191) {}; 194's parent {}, 191's parent {}", skel, n, c194, c191,
                         n > 194 ? g_parent[194] : -9, n > 191 ? g_parent[191] : -9);
            spdlog::info("[SkelHand] arms: right {} - {} - {} - {} (parents {} {} {}), left {} - {} - {} - 344", g_chain[0][0], g_chain[0][1], g_chain[0][2], g_chain[0][3], n > 192 ? g_parent[192] : -9,
                         n > 193 ? g_parent[193] : -9, n > 194 ? g_parent[194] : -9, g_chain[1][0], g_chain[1][1], g_chain[1][2]);
            return n > 194;
        }

        // The quaternion convention from a child of the hand: its position = the hand's position + the hand's rotation x its local position.
        void self_test(uintptr_t ctx, uintptr_t model)
        {
            int child = -1;
            {
                std::lock_guard _{ g_tree_mtx };
                for (size_t b = 195; b < g_parent.size(); ++b) if (g_parent[b] == 194) { child = int(b); break; }
            }
            if (child < 0) { g_conv = -2; spdlog::info("[SkelHand] self-test: no child of bone 194: off"); return; }
            t_in = true;
            resolve(ctx, uint32_t(child));
            t_in = false;
            uint64_t local = 0;
            float    mh[8]{}, mc[8]{}, lc[8]{}, scale = 1.0f;
            rd(ctx + 8, &local, 8);
            rd(ctx + 0x24, &scale, 4);
            if (!rd(model + 194 * 32, mh, 32) || !rd(model + uintptr_t(child) * 32, mc, 32) || !local || !rd(uintptr_t(local) + uintptr_t(child) * 32, lc, 32)) {
                g_conv = -2;
                spdlog::info("[SkelHand] self-test: transforms unreadable: off");
                return;
            }
            int   best = -2;
            float be   = 1e9f;
            std::string all;
            for (int conv = 0; conv < 8; ++conv) {
                const glm::mat4 H = decode(mh, conv);
                const glm::vec3 lp = glm::vec3{ lc[4], lc[5], lc[6] } * ((conv & 4) ? scale : 1.0f);
                const float     e  = glm::length(glm::vec3{ H * glm::vec4{ lp, 1.0f } } - glm::vec3{ mc[4], mc[5], mc[6] });
                all += fmt::format(" {}:{:.2f}", conv, 100.0f * e);
                if (e < be) { be = e; best = conv; }
            }
            g_conv = be < 0.005f ? best : -2;
            spdlog::info("[SkelHand] self-test with bone {} (local ({:.3f}, {:.3f}, {:.3f}), scale {:.3f}): error per convention, cm{} -> {}", child, lc[4], lc[5], lc[6], scale, all,
                         be < 0.005f ? fmt::format("convention {} ({} layout, {} rotation, local position {})", best, (best & 1) ? "w x y z" : "x y z w", (best & 2) ? "transposed" : "as is",
                                                   (best & 4) ? "scaled" : "unscaled")
                                     : std::string{ "none fits within 5 mm: skelhand off" });
        }
    }

    // skelhandarms (mode 3): on the first read of a bone under a shoulder in an update, both hands go where the gun's move takes them
    // (the game's hands sit on the game's gun: the same move keeps the right one on the grip and the left one on the fore-end), and the arm
    // follows by a two-bone IK: shoulder S fixed, the elbow on the circle |E' - S| = a, |T - E'| = b, bent toward the game's own elbow (the
    // pole). The upper arm turns by the shortest arc from E - S to E' - S, the forearm (with the wrist) by the arc from the turned forearm
    // to T - E'. The shoulder's other descendants (twist helpers, fingers) are marked unresolved and follow.
    void arms_on_read(uintptr_t ctx, uintptr_t model, uint32_t bone, int frame)
    {
        int side = -1;
        {
            std::lock_guard _{ g_tree_mtx };
            if (bone < g_sub[1].size() && g_sub[1][bone]) side = 0;
            else if (bone < g_sub[2].size() && g_sub[2][bone]) side = 1;
        }
        if (side < 0) return;
        const int S = g_chain[side][0], E = g_chain[side][1], W = g_chain[side][2], H = g_chain[side][3];
        if (S <= 0 || E <= 0 || W <= 0) return;
        uint64_t flags = 0;
        if (!rd(ctx + 0x18, &flags, 8) || !flags) return;
        uint8_t fs = 0;
        rd(uintptr_t(flags) + S, &fs, 1);
        if (!(fs & g_dirty_mask.load())) return;   // this arm already done this update
        Target tg;
        {
            std::lock_guard _{ g_t_mtx };
            tg = g_target;
        }
        if (!tg.ok || frame - tg.frame > 4) { g_no_target++; return; }
        t_in = true;
        resolve(ctx, uint32_t(H));   // the hand and the arm above it resolved by the game
        t_in = false;
        if (g_conv.load() == -1) self_test(ctx, model);
        const int conv = g_conv.load();
        if (conv < 0) return;
        float     m[4][8]{};
        const int ids[4]{ S, E, W, H };
        for (int i = 0; i < 4; ++i) if (!rd(model + uintptr_t(ids[i]) * 32, m[i], 32)) return;
        glm::mat4 M[4];
        for (int i = 0; i < 4; ++i) M[i] = decode(m[i], conv);
        const glm::vec3 s0{ M[0][3] }, e{ M[1][3] }, h{ M[3][3] };
        const glm::mat4 H2 = tg.D * M[3];
        glm::vec3       T{ H2[3] };
        const float     a = glm::length(e - s0), b = glm::length(h - e);
        float           d = glm::length(T - s0);
        if (a < 0.05f || b < 0.05f || d < 1e-3f) return;
        const glm::vec3 u = (T - s0) / d;
        // Out of reach (the controller holds the gun further out than the game: the left hand on the fore-end 0.70 m from its shoulder
        // against 0.58 m of arm, simulator 24 Oct): the shoulder itself goes up to 15 cm toward the hand (the arm reaching forward).
        const float     shift = std::clamp(d - (a + b - 0.01f), 0.0f, 0.15f);
        const glm::vec3 s     = s0 + u * shift;
        d -= shift;
        if (shift > 0.0f) g_ik_reached[side]++;
        const float     dc      = std::clamp(d, std::abs(a - b) + 0.01f, a + b - 0.01f);
        const bool      clamped = dc != d;
        if (clamped) { g_ik_clamped[side]++; T = s + u * dc; d = dc; }
        const float x  = (a * a - b * b + d * d) / (2.0f * d);
        const float hh = std::sqrt(std::max(0.0f, a * a - x * x));
        glm::vec3   p  = (e - s0) - glm::dot(e - s0, u) * u;   // the game's elbow as the pole
        if (glm::length(p) < 1e-4f) p = glm::cross(u, glm::vec3{ 0.0f, 0.0f, 1.0f });
        if (glm::length(p) < 1e-4f) return;
        p = glm::normalize(p);
        const glm::vec3 e2  = s + x * u + hh * p;
        auto            arc = [](glm::vec3 f, glm::vec3 t) {   // the shortest turn taking direction f onto t
            f = glm::normalize(f);
            t = glm::normalize(t);
            const float     c  = glm::dot(f, t);
            const glm::vec3 ax = glm::cross(f, t);
            if (c < -0.9999f) return glm::angleAxis(glm::pi<float>(), glm::normalize(glm::abs(f.x) < 0.9f ? glm::cross(f, glm::vec3{ 1, 0, 0 }) : glm::cross(f, glm::vec3{ 0, 1, 0 })));
            return glm::normalize(glm::quat{ 1.0f + c, ax.x, ax.y, ax.z });
        };
        const glm::quat r1  = arc(e - s0, e2 - s);
        const glm::quat r2  = arc(r1 * (h - e), T - e2);
        const glm::mat3 R1  = glm::mat3_cast(r1), R21 = glm::mat3_cast(r2 * r1);
        glm::mat4       N[4];
        N[0]    = glm::mat4{ R1 * glm::mat3{ M[0] } };
        N[0][3] = glm::vec4{ s, 1.0f };
        N[1]    = glm::mat4{ R21 * glm::mat3{ M[1] } };
        N[1][3] = glm::vec4{ e2, 1.0f };
        N[2]    = glm::mat4{ R21 * glm::mat3{ M[2] } };
        N[2][3] = glm::vec4{ e2 + R21 * (glm::vec3{ M[2][3] } - e), 1.0f };
        N[3]    = H2;
        N[3][3] = glm::vec4{ T, 1.0f };
        for (int i = 0; i < 4; ++i) {
            float out[8];
            std::memcpy(out, m[i], sizeof(out));
            encode(N[i], conv, out);
            wr(model + uintptr_t(ids[i]) * 32, out, 28);
        }
        {
            std::lock_guard _{ g_tree_mtx };
            const uint8_t mk  = uint8_t(g_dirty_mask.load());
            const auto&   sub = g_sub[side + 1];
            for (size_t dd = size_t(S) + 1; dd < sub.size(); ++dd) {
                if (!sub[dd] || int(dd) == E || int(dd) == W || int(dd) == H) continue;
                uint8_t f = 0;
                if (rd(uintptr_t(flags) + dd, &f, 1)) { f |= mk; wr(uintptr_t(flags) + dd, &f, 1); }
            }
        }
        g_ik_applied[side]++;
        g_applied++;
        if (g_ik_log.load() > 0) {
            g_ik_log--;
            auto deg = [](const glm::quat& q) { return glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(q.w)))); };
            spdlog::info("[SkelHand] frame {}: {} arm IK: hand moved {:.1f} cm; upper arm {:.2f} m, forearm {:.2f} m, shoulder to hand {:.2f} m (shoulder moved {:.1f} cm){}; elbow moved {:.1f} cm, upper arm turned {:.1f} deg, "
                         "forearm {:.1f} deg",
                         frame, side ? "left" : "right", 100.0f * glm::length(T - h), a, b, glm::length(glm::vec3{ H2[3] } - s0), 100.0f * shift, clamped ? " (out of reach: clamped)" : "",
                         100.0f * glm::length(e2 - e), deg(r1), deg(r2 * r1));
        }
    }

    void on_getter(safetyhook::Context& c)
    {
        if (t_in) return;
        const int mode = g_mode.load(std::memory_order_relaxed);
        if (mode == 0 || !g_fcb) return;
        uint64_t ret = 0, model = 0, skel = 0;
        rd(c.rsp, &ret, 8);
        rd(c.rcx + 0x10, &model, 8);
        rd(c.rcx, &skel, 8);
        const uint64_t off   = ret - g_fcb;
        const int      frame = g_fsrc ? g_fsrc->load() : 0;
        if ((off == 0x725a23c || off == 0x725a2ec) && model) {
            if (g_player_model.load() != uintptr_t(model)) {
                g_player_model = uintptr_t(model);
                g_player_skel  = uintptr_t(skel);
                g_player_ok    = true;
                spdlog::info("[SkelHand] the player's pose (read by +{:x}): model transforms {:x}, skeleton {:x}", off, model, skel);
            }
            if (off == 0x725a23c && g_order_log.load() > 0) {
                g_order_log--;
                spdlog::info("[SkelHand] order: +725a23c reads bone {} in frame {} (seq {}); the hand was last moved in frame {} (seq {})", uint32_t(c.rdx), frame, fc5::skinorder::next(),
                             g_apply_frame.load(), g_apply_seq.load());
            }
        }
        if (!g_player_ok.load() || uintptr_t(model) != g_player_model.load()) return;
        if (g_conv.load() == -2 || !tree(uintptr_t(skel))) return;
        if (mode == 3 && g_arms.load()) { arms_on_read(c.rcx, uintptr_t(model), uint32_t(c.rdx), frame); return; }
        const int      k    = mode == 2 ? 1 : 0;   // mode 3 moves the hand as mode 1 (and the gun at its writer)
        const uint32_t B    = mode == 2 ? 191 : 194;
        const uint32_t bone = uint32_t(c.rdx);
        uint64_t       flags = 0;
        {
            std::lock_guard _{ g_tree_mtx };
            if (bone >= g_sub[k].size() || !g_sub[k][bone]) return;
        }
        if (!rd(c.rcx + 0x18, &flags, 8) || !flags) return;
        uint8_t fb = 0;
        rd(uintptr_t(flags) + B, &fb, 1);
        if (g_flag_log.load() > 0) {
            // The flags as they are at a read under B (what marks a bone unresolved?): B's, the bone's own, and bytes around B.
            g_flag_log--;
            uint8_t own = 0, around[16]{};
            rd(uintptr_t(flags) + bone, &own, 1);
            rd(uintptr_t(flags) + (B & ~7u), around, 16);
            std::string a;
            for (uint8_t x : around) a += fmt::format(" {:02x}", x);
            uint64_t q0 = 0;
            rd(uintptr_t(flags) + (B / 8), &q0, 1);
            spdlog::info("[SkelHand] flags at a read of bone {} (frame {}, by +{:x}): B {} = {:02x}, the bone's own {:02x}; bytes from {}:{}; as a bit array, B's byte {:02x} (bit {})", bone, frame, off, B, fb,
                         own, B & ~7u, a, uint8_t(q0), B % 8);
        }
        if (!(fb & g_dirty_mask.load())) return;   // B already resolved this update (moved, or nothing to do)
        Target tg;
        {
            std::lock_guard _{ g_t_mtx };
            tg = g_target;
        }
        if (!tg.ok || frame - tg.frame > 4) { g_no_target++; return; }
        uint8_t f194_before = 0, f194_after = 0;
        rd(uintptr_t(flags) + 194, &f194_before, 1);
        t_in = true;
        resolve(c.rcx, 194);   // the hand and its ancestors (191-193) resolved by the game
        t_in = false;
        rd(uintptr_t(flags) + 194, &f194_after, 1);
        if (g_applied.load() == 0) {
            const uint32_t cleared = uint32_t(f194_before & ~f194_after);
            if (cleared) g_dirty_mask = cleared;
            spdlog::info("[SkelHand] first move: bone 194's flags {:02x} before the resolve, {:02x} after: unresolved mask {:02x}", f194_before, f194_after, g_dirty_mask.load());
        }
        if (g_conv.load() == -1) self_test(c.rcx, uintptr_t(model));
        const int conv = g_conv.load();
        if (conv < 0) return;
        float mb[8]{}, mh[8]{};
        if (!rd(uintptr_t(model) + B * 32, mb, 32) || !rd(uintptr_t(model) + 194 * 32, mh, 32)) return;
        const glm::mat4 MB  = decode(mb, conv);
        const glm::mat4 MB2 = tg.D * MB;
        float           out[8];
        std::memcpy(out, mb, sizeof(out));
        encode(MB2, conv, out);
        wr(uintptr_t(model) + B * 32, out, 28);
        // B's descendants resolve again from it (B itself stays resolved: no second move this update).
        {
            std::lock_guard _{ g_tree_mtx };
            const uint8_t m = uint8_t(g_dirty_mask.load());
            for (size_t d = B + 1; d < g_sub[k].size(); ++d) {
                if (!g_sub[k][d]) continue;
                uint8_t f = 0;
                if (rd(uintptr_t(flags) + d, &f, 1)) { f |= m; wr(uintptr_t(flags) + d, &f, 1); }
            }
        }
        g_applied++;
        g_apply_frame = frame;
        g_apply_seq   = fc5::skinorder::next();
        if (g_delta_log.load() > 0) {
            g_delta_log--;
            const glm::mat4 H  = decode(mh, conv);
            const glm::mat4 H2 = tg.D * H;
            const glm::mat3 dR = glm::mat3{ H2 } * glm::transpose(glm::mat3{ H });
            const float     ang = glm::degrees(std::acos(std::clamp((dR[0][0] + dR[1][1] + dR[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
            spdlog::info("[SkelHand] frame {} (target of frame {}): bone {} moved; the hand {:.1f} cm / {:.1f} deg from the game's (model ({:.3f}, {:.3f}, {:.3f}) -> ({:.3f}, {:.3f}, {:.3f})), "
                         "read for bone {} by +{:x}",
                         frame, tg.frame, B, 100.0f * glm::length(glm::vec3{ H2[3] } - glm::vec3{ H[3] }), ang, H[3].x, H[3].y, H[3].z, H2[3].x, H2[3].y, H2[3].z, bone, off);
        }
    }

    // skelhand 3: the gun itself, where the game sets an entity's world transform (FC_m64+0x6a10660, reached by the jmp at +0x4db3b0 from
    // +0xc85c1f on the animation job threads): it stores a 4x4 column-major matrix at [rbx+0x10] and the scale at [rbx+0x50]. Hooked
    // right after the matrix stores (+0x6a10785, mov ecx,[rsi] / mov [rbx+0x50],ecx). The gun's entity is learned once per weapon by
    // matching the incoming matrix with the drawn gun's (the render thread's last gun draw); its matrix is then replaced by Mw0 x it.
    safetyhook::MidHook g_gun_hook{};
    std::mutex          g_gun_mtx;
    struct GunRef { int frame{ -100 }; glm::vec3 t{ 0.0f }, c0{ 0.0f }; };
    GunRef              g_gun_ref{};
    std::atomic<uintptr_t> g_gun_obj{ 0 };
    std::atomic<uintptr_t> g_gun_objs[8]{};   // every entity matched while learning (the game alternates 4 for a rifle; a pistol: 6 matched, 24 Oct)
    std::atomic<int>    g_gun_learn_until{ 0 };
    std::atomic<bool>   g_gun_learn{ true };
    std::atomic<uint64_t> g_gun_moves{ 0 }, g_gun_calls{ 0 };
    struct WTarget { bool ok{ false }; int frame{ -100 }; glm::mat4 Mw{ 1.0f }; };
    WTarget             g_wtarget{};
    std::atomic<int>    g_gun_log{ 20 };
    // The gun's parts (scope, silencer, clip ...): other entities the writer sets whose game position lies within 0.6 m of the gun body's
    // latest game position get the same move, so they stay with the body. Logged per frame (the first 30 frames).
    std::mutex          g_part_mtx;
    glm::vec3           g_body_game{ 0.0f };
    int                 g_body_frame{ -100 };
    struct PartFrame { int frame{ -100 }; int body{ 0 }, parts{ 0 }, left{ 0 }; float max_d{ 0.0f }; };
    PartFrame           g_pf{};
    std::atomic<int>    g_part_log{ 30 };
    std::atomic<uint64_t> g_part_moves{ 0 };
    std::atomic<int>    g_used_frame{ -100 };
    glm::mat4           g_body_M{ 1.0f };   // the gun body's last game matrix
    struct FarPart { glm::vec3 off{ 0.0f }; int frame{ -100 }; int stable{ 0 }; bool said{ false }; };
    std::unordered_map<uintptr_t, FarPart> g_far;
    // skelhandstill: the gun body's last matrices as moved and as the game set them (column-major 4x4), so the render side can give
    // gunstill the game's own pose of the gun it draws (the drawn one is already moved).
    struct RawGun { bool ok{ false }; int frame{ -100 }; float moved[16]{}, raw[16]{}; };   // frame: the render frame whose placement it got
    RawGun              g_raw[16]{};
    unsigned            g_raw_i{ 0 };

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

    void gun_probe(safetyhook::Context& c)
    {
        if (g_tp_on.load(std::memory_order_relaxed)) {
            float p3[3]{};
            if (rd(c.rbx + 0x10 + 48, p3, sizeof(p3))) throw_track(c.rbx, glm::vec3{ p3[0], p3[1], p3[2] });
        }
        if (g_mode.load(std::memory_order_relaxed) != 3) return;
        g_gun_calls++;
        float m[16]{};
        if (!rd(c.rbx + 0x10, m, sizeof(m))) return;
        const glm::vec3 t{ m[12], m[13], m[14] }, c0{ m[0], m[1], m[2] };
        if (g_gun_learn.load()) {
            GunRef ref;
            {
                std::lock_guard _{ g_gun_mtx };
                ref = g_gun_ref;
            }
            const int fl = g_fsrc ? g_fsrc->load() : 0;
            if (g_gun_obj.load() && fl > g_gun_learn_until.load()) { g_gun_learn = false; return; }
            bool known = false;
            for (auto& o : g_gun_objs) known = known || o.load() == c.rbx;
            if (!known && ref.frame >= 0 && glm::length(t - ref.t) < 0.02f && glm::length(c0 - ref.c0) < 0.01f) {
                for (auto& o : g_gun_objs) { uintptr_t z = 0; if (o.compare_exchange_strong(z, c.rbx)) break; }
                if (!g_gun_obj.load()) g_gun_learn_until = fl + 90;   // learn the others for 90 frames
                g_gun_obj   = c.rbx;
                spdlog::info("[SkelHand] the gun's entity {:x}: its world matrix matches the drawn gun ({:.3f}, {:.3f}, {:.3f})", c.rbx, t.x, t.y, t.z);
            }
            return;
        }
        bool mine = false;
        for (auto& o : g_gun_objs) mine = mine || (o.load() == c.rbx && c.rbx != 0);
        WTarget wt;
        {
            std::lock_guard _{ g_gun_mtx };
            wt = g_wtarget;
        }
        const int frame = g_fsrc ? g_fsrc->load() : 0;
        if (!wt.ok || frame - wt.frame > 4) return;
        // Per-frame tally (logged when the frame changes).
        {
            std::lock_guard _{ g_part_mtx };
            if (g_pf.frame != frame) {
                if (g_pf.frame >= 0 && g_part_log.load() > 0 && (g_pf.body || g_pf.parts)) {
                    g_part_log--;
                    spdlog::info("[SkelHand] frame {}: gun body entities moved {}, parts moved {} (furthest {:.2f} m from the body), near but left (0.6-1.5 m) {}", g_pf.frame, g_pf.body,
                                 g_pf.parts, g_pf.max_d, g_pf.left);
                }
                g_pf = PartFrame{};
                g_pf.frame = frame;
            }
            if (mine) {
                g_body_game  = t;
                g_body_frame = frame;
                for (int col = 0; col < 4; ++col) for (int r = 0; r < 4; ++r) g_body_M[col][r] = m[col * 4 + r];
                g_pf.body++;
            } else {
                const float d = (frame - g_body_frame <= 4) ? glm::length(t - g_body_game) : 1e9f;
                // 0.6-1.2 m (a rifle's muzzle end: the silencer of 42000 sits ~0.7 m from the body's origin, simulator 24 Oct): a part only
                // once it has kept its place in the body's own axes (5 mm) for 5 updates running, so things on the player's body are left.
                bool far_part = false;
                if (d > 0.6f && d < 1.2f) {
                    const glm::vec3 off{ glm::inverse(g_body_M) * glm::vec4{ t, 1.0f } };
                    auto&           st = g_far[c.rbx];
                    st.stable          = (st.frame >= 0 && frame - st.frame <= 4 && glm::length(off - st.off) < 0.005f) ? st.stable + 1 : 0;
                    st.off             = off;
                    st.frame           = frame;
                    far_part           = st.stable >= 5;
                    if (far_part && !st.said) {
                        st.said = true;
                        spdlog::info("[SkelHand] entity {:x} {:.2f} m from the gun body, fixed in its axes ({:.2f}, {:.2f}, {:.2f}): moved with the gun as a part", c.rbx, d, off.x, off.y, off.z);
                    }
                    if (g_far.size() > 256) g_far.clear();
                }
                if (d > 0.6f && !far_part) {
                    if (d < 1.5f) g_pf.left++;
                    return;
                }
                g_pf.parts++;
                g_pf.max_d = std::max(g_pf.max_d, d);
                g_part_moves++;
            }
        }
        glm::mat4 M{ 1.0f };
        for (int col = 0; col < 4; ++col) for (int r = 0; r < 4; ++r) M[col][r] = m[col * 4 + r];
        const glm::mat4 M2 = wt.Mw * M;
        float out[16];
        for (int col = 0; col < 4; ++col) for (int r = 0; r < 4; ++r) out[col * 4 + r] = M2[col][r];
        wr(c.rbx + 0x10, out, sizeof(out));
        g_gun_moves++;
        if (mine) {
            g_used_frame = wt.frame;   // skelhandlate: the render frame whose placement this update got
            std::lock_guard _{ g_part_mtx };
            auto& r = g_raw[g_raw_i++ % std::size(g_raw)];
            std::memcpy(r.moved, out, sizeof(out));
            std::memcpy(r.raw, m, sizeof(m));
            r.ok    = true;
            r.frame = wt.frame;
        }
        if (g_gun_log.load() > 0) {
            g_gun_log--;
            spdlog::info("[SkelHand] frame {}: the gun's world matrix moved {:.1f} cm ({:.3f}, {:.3f}, {:.3f}) -> ({:.3f}, {:.3f}, {:.3f})", frame, 100.0f * glm::length(glm::vec3{ M2[3] } - t), t.x, t.y,
                         t.z, M2[3].x, M2[3].y, M2[3].z);
        }
    }
    // The bone resolve (FC_m64+0x70aa130: rcx = the pose context, edx = the bone, as the getter's) gets the same check: some code resolves
    // bones directly (the job behind the gun's world matrix, writer +0x6a1076d, simulator 24 Oct), not through the getter.
    safetyhook::MidHook g_resolve_hook{};
    void resolve_probe(safetyhook::Context& c) { if (g_mode.load(std::memory_order_relaxed) > 0) on_getter(c); }

    bool install_writer_hook()
    {
        static std::mutex mx;
        std::lock_guard   _{ mx };
        if (g_gun_hook) return true;
        auto*                base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
        static const uint8_t kAt[5]{ 0x8B, 0x0E, 0x89, 0x4B, 0x50 };
        if (base && std::memcmp(base + 0x6a10785, kAt, sizeof(kAt)) == 0) {
            g_gun_hook = safetyhook::create_mid(base + 0x6a10785, gun_probe);
            spdlog::info("[SkelHand] gun world-matrix hook at FC_m64+0x6a10785 {}", g_gun_hook ? "installed" : "failed");
        } else {
            spdlog::info("[SkelHand] the code differs at FC_m64+0x6a10785: no gun hook");
        }
        return bool(g_gun_hook);
    }

    bool set_mode(int m, const std::atomic<int>* frame)
    {
        g_fsrc = frame;
        if (m > 0) {
            if (!fc5::poseprobe::install_getter(frame)) return false;
            if (!g_resolve_hook) {
                auto*                base = reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll"));
                static const uint8_t kEntry[15]{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x60 };
                if (base && std::memcmp(base + 0x70aa130, kEntry, sizeof(kEntry)) == 0) {
                    g_resolve_hook = safetyhook::create_mid(base + 0x70aa130, resolve_probe);
                    spdlog::info("[SkelHand] bone resolve hook at FC_m64+0x70aa130 {}", g_resolve_hook ? "installed" : "failed");
                } else {
                    spdlog::info("[SkelHand] the bone resolve's entry bytes differ at FC_m64+0x70aa130: getter path only");
                }
            }
            install_writer_hook();
            g_fcb = reinterpret_cast<uintptr_t>(GetModuleHandleA("FC_m64.dll"));
            g_delta_log = 100;
            g_order_log = 50;
            g_flag_log  = 40;
        }
        g_mode      = std::clamp(m, 0, 3);
        g_gun_learn = true;
        g_gun_obj   = 0;
        for (auto& o : g_gun_objs) o = 0;
        g_gun_log   = 20;
        g_part_log  = 30;
        return true;
    }
    int  mode() { return g_mode.load(std::memory_order_relaxed); }
    void set_target(int frame, const glm::mat4& D, bool ok)
    {
        std::lock_guard _{ g_t_mtx };
        g_target.ok    = ok;
        g_target.frame = frame;
        g_target.D     = D;
    }
    bool player_ok() { return g_player_ok.load() && g_conv.load() >= 0; }
    std::string status()
    {
        return fmt::format("mode {}, player pose {:x} ({}), convention {}, moves {}, skipped with no target {}{}", g_mode.load(), g_player_model.load(), g_player_ok.load() ? "checked" : "not checked",
                           g_conv.load(), g_applied.load(), g_no_target.load(),
                           g_arms.load() ? fmt::format(", arms IK right {} (shoulder moved {}, out of reach {}), left {} ({}, {})", g_ik_applied[0].load(), g_ik_reached[0].load(), g_ik_clamped[0].load(), g_ik_applied[1].load(), g_ik_reached[1].load(), g_ik_clamped[1].load()) : std::string{});
    }

    // The identity check (render thread, the palette writer): an arms mesh's skin rotations and the player's model quats for its bones,
    // against the last update's. Every 2 s: a mean turn mismatch over 0.25 deg (with the bones moving) drops the pose until it is read
    // again by +725a23c / +725a2ec.
    void note_skin(uintptr_t rcx, uintptr_t rdx)
    {
        const uintptr_t model = g_player_model.load();
        if (!model || g_mode.load() == 0) return;
        uint64_t list = 0, cnt = 0;
        if (!rd(rdx, &list, 8) || !rd(rdx + 8, &cnt, 8)) return;
        const uint32_t n = uint32_t(cnt >> 32) & 0x7fffffff;
        if (!list || n < 30 || n > 60) return;
        std::vector<uint16_t> remap(n);
        if (!rd(uintptr_t(list), remap.data(), n * 2)) return;
        std::vector<float> skin(size_t(n) * 9), q(size_t(n) * 4);
        for (uint32_t i = 0; i < n; ++i) {
            float m[16]{};
            rd(rcx + uintptr_t(remap[i]) * 64, m, sizeof(m));
            for (int r = 0; r < 3; ++r) for (int cc = 0; cc < 3; ++cc) skin[i * 9 + r * 3 + cc] = m[r * 4 + cc];
            rd(model + uintptr_t(remap[i]) * 32, &q[i * 4], 16);
        }
        const int frame = g_fsrc ? g_fsrc->load() : 0;
        if (remap == g_chk_remap && frame != g_chk_frame) {
            for (uint32_t i = 0; i < n; ++i) {
                const float as = fc5::poseprobe::rot_angle_deg(&g_chk_skin[i * 9], &skin[i * 9]);
                const float aq = fc5::poseprobe::quat_angle_deg(&g_chk_q[i * 4], &q[i * 4]);
                if (as < 0.05f || aq < 0.0f) continue;
                g_chk_err += std::abs(as - aq);
                g_chk_n++;
            }
        }
        if (remap == g_chk_remap || g_chk_remap.empty()) {
            g_chk_remap = remap;
            g_chk_skin  = std::move(skin);
            g_chk_q     = std::move(q);
            g_chk_frame = frame;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - g_chk_t > std::chrono::seconds(2)) {
            g_chk_t = now;
            if (g_chk_n >= 20) {
                // Fast arm motion shows a few tenths to ~3 deg here (the next update's animation can be under way while the render thread
                // reads): only a mismatch over 3 deg in two checks running drops the pose; anything else is logged.
                const double e = g_chk_err / g_chk_n;
                g_chk_bad      = e > 3.0 ? g_chk_bad + 1 : 0;
                if (g_chk_bad >= 2) {
                    g_player_ok    = false;
                    g_player_model = 0;
                    g_chk_bad      = 0;
                    spdlog::info("[SkelHand] identity check: the player's pose {:x} no longer matches the arms' skin ({:.2f} deg mean turn error over {} bone turns, twice): dropped", model, e, g_chk_n);
                } else if (e > 0.25) {
                    spdlog::info("[SkelHand] identity check: {:.2f} deg mean turn error over {} bone turns (kept)", e, g_chk_n);
                }
            }
            g_chk_err = 0.0;
            g_chk_n   = 0;
        }
    }
}

namespace fc5::skelhand
{
    void set_world_target(int frame, const glm::mat4& Mw, bool ok)
    {
        std::lock_guard _{ g_gun_mtx };
        g_wtarget.ok    = ok;
        g_wtarget.frame = frame;
        g_wtarget.Mw    = Mw;
    }
    void note_gun(int frame, const glm::mat4& Om)   // the drawn gun (row-major object matrix as a glm mat4)
    {
        std::lock_guard _{ g_gun_mtx };
        g_gun_ref.frame = frame;
        g_gun_ref.t     = glm::vec3{ Om[3] };
        g_gun_ref.c0    = glm::vec3{ Om[0] };
    }
    void set_arms(bool on) { g_arms = on; g_ik_log = 12; }
    bool arms() { return g_arms.load(); }
    int  used_target_frame() { return g_used_frame.load(); }
    void relearn_gun() { g_gun_obj = 0; for (auto& o : g_gun_objs) o = 0; g_gun_learn = true; g_part_log = 10; g_gun_log = 4; }   // the new weapon's parts logged too
    std::atomic<int> g_drawn_frame{ -100 };
    int  drawn_target_frame() { return g_drawn_frame.load(); }
    bool raw_gun(const float* d, float* raw12)
    {
        std::lock_guard _{ g_part_mtx };
        for (unsigned k = 0; k < std::size(g_raw); ++k) {
            const auto& r = g_raw[(g_raw_i - 1 - k) % std::size(g_raw)];   // newest first
            if (!r.ok) continue;
            bool same = true;
            for (int row = 0; row < 3 && same; ++row)
                for (int col = 0; col < 4 && same; ++col) same = std::abs(d[row * 4 + col] - r.moved[col * 4 + row]) < (col == 3 ? 2e-3f : 1e-4f);   // world positions ~3000 m: float steps 0.25 mm
            if (!same) continue;
            for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) raw12[row * 4 + col] = r.raw[col * 4 + row];
            g_drawn_frame = r.frame;
            return true;
        }
        return false;
    }
    void throw_probe_arm(const glm::vec3& hand)
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
    ThrowFit throw_fit(const glm::vec3& head_dir)
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
    std::string gun_status() { return fmt::format("gun entity {:x}, writer calls {}, gun moves {} (parts {})", g_gun_obj.load(), g_gun_calls.load(), g_gun_moves.load(), g_part_moves.load()); }
}