// throwable (Homard 16 Oct: "make the selected throwable show up on the left chest, left grip attached to the hand"):
// the game's own throwable model in the left hand and on the chest.
//
// The game draws its throwable (a rigid mesh of the weapon pass, e.g. the grenade: 2502 indices, weapon material
// #A7E51697, object transform at VS b1, previous at b3; simulator 16 Oct) only while the throw button (pad RB) is held,
// and hides the gun meanwhile. The throwable gesture (FarCry5Gesture) holds RB from the grab to the throw, so:
//  - in the hand: every weapon-pass draw that was not drawn in the last frame before the grab is the throwable; its
//    object transform is moved from the game's left hand to the left controller, the same move the left glove gets
//    (throwhand);
//  - on the chest: the throwable's draw (shaders, mesh, textures, material constants) is recorded at the grab and drawn
//    again at the chest from then on, between throws (throwchest). The game never draws it there, so the chest shows the
//    throwable of the last grab: nothing before the first grab of a session, and the old one for a moment after the
//    selection changes (until the next grab).
#include "FarCry5Stereo.h"
#include "FarCry5Record.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <istream>
#include <set>
#include <tuple>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

namespace
{
    using Key = std::tuple<void*, UINT, UINT, INT>;   // index buffer, start, count, base

    constexpr int kSrv = 16, kSmp = 16, kPsCb = 8, kVb = 4, kMaxRec = 6;   // a throwable of up to 6 parts (headset 25 Oct: 4)

    // One recorded draw of the throwable.
    struct Rec
    {
        Key                       key{};
        UINT                      count{ 0 }, start{ 0 };
        INT                       base{ 0 };
        ID3D11InputLayout*        il{ nullptr };
        D3D11_PRIMITIVE_TOPOLOGY  topo{ D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST };
        ID3D11Buffer*             vb[kVb]{};
        UINT                      stride[kVb]{}, off[kVb]{};
        ID3D11Buffer*             ib{ nullptr };
        DXGI_FORMAT               ib_fmt{ DXGI_FORMAT_R16_UINT };
        UINT                      ib_off{ 0 };
        ID3D11VertexShader*       vs{ nullptr };
        ID3D11PixelShader*        ps{ nullptr };
        ID3D11Buffer*             vs_cb2{ nullptr };   // CSceneGeometry, copied
        float                     obj[32]{};           // CSceneTransform (b1) as recorded: 3x4 rows + the rest
        ID3D11ShaderResourceView* srv[kSrv]{};
        ID3D11SamplerState*       smp[kSmp]{};
        ID3D11Buffer*             ps_cb[kPsCb]{};      // copies; nullptr where the camera buffer was bound
        bool                      ps_cb_view[kPsCb]{}; // the camera (VS b0) was bound there: the current one is used
        ID3D11RasterizerState*    rs{ nullptr };
        ID3D11DepthStencilState*  dss{ nullptr };
        UINT                      ds_ref{ 0 };
        ID3D11BlendState*         bs{ nullptr };
        float                     bf[4]{};
        UINT                      mask{ 0xffffffff };
        glm::mat3                 rq{ 1.0f };          // the object's axes in the game camera's (q) axes, as held
        glm::vec3                 tq{ 0.0f };          // its origin there
        glm::vec3                 Fq{ 0.0f };          // the game's left hand there when it was recorded (throwv2: the copy in the hand)
        // 28 Oct (simulator, Homard's save: the molotov 8244 and the C4 16110 + 192 not seen in the hand, the knife was): a skinned part
        // (BLENDINDICES in its layout) takes its bone palette from VS b3 and b4, where a rigid one has its previous transform (b3): copied.
        bool                      skinned{ false };
        ID3D11Buffer*             bones[2]{};

        void release()
        {
            auto rel = [](auto*& p) { if (p) p->Release(); p = nullptr; };
            rel(il); rel(ib); rel(vs); rel(ps); rel(vs_cb2); rel(rs); rel(dss); rel(bs);
            for (auto*& p : vb) rel(p);
            for (auto*& p : srv) rel(p);
            for (auto*& p : smp) rel(p);
            for (auto*& p : ps_cb) rel(p);
            for (auto*& p : bones) rel(p);
        }
    };

    template <class T> T* addref(T* p) { if (p) p->AddRef(); return p; }

    glm::mat4 from_rows(const float* f)
    {
        glm::mat4 m{ 1.0f };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) m[c][r] = f[r * 4 + c];
        return m;
    }
    void to_rows(const glm::mat4& m, float* f)
    {
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) f[r * 4 + c] = m[c][r];
    }
    glm::mat3 orthonormal(const glm::mat3& m)
    {
        glm::vec3 x = glm::normalize(m[0]);
        glm::vec3 y = m[1] - x * glm::dot(x, m[1]);
        y           = glm::length(y) > 1e-6f ? glm::normalize(y) : glm::vec3{ 0, 1, 0 };
        return glm::mat3{ x, y, glm::cross(x, y) };
    }
}

struct FarCry5Stereo::ThrowableState
{
    std::atomic<bool>  hand{ true };    // throwhand: the held throwable follows the left controller
    std::atomic<bool>  chest{ false };   // throwchest: the recorded throwable drawn on the chest between throws (off: pouch mode, the default since 28 Oct)
    std::atomic<float> rot[3]{ 0.0f, 90.0f, 0.0f };   // throwrot: its turn on the chest (yaw, pitch, roll, deg)
    std::atomic<float> hand_rot[3]{ 0.0f, 90.0f, 0.0f };   // throwhandrot: the copy's turn in the hand (throwv2), controller axes (90 deg: the knife along the controller, not hanging down)
    // throwhandrot2: the same for every throwable but the knife (2502 indices). Homard 29 Oct: "throwable were upside down in my hand" (the
    // molotov and the grenade): the copy's axes are the controller's turned by the pitch, and +90 puts a model's up (z) down; -90 stands it up.
    std::atomic<float> hand_rot2[3]{ 0.0f, -90.0f, 0.0f };
    std::atomic<float> hand_off[3]{ 0.0f, 0.0f, 0.0f };   // throwhandpos: its origin from the controller (m, controller axes)
    std::atomic<int>   log{ 2 };        // throwdrawlog N: the next N throwable draws logged
    std::chrono::steady_clock::time_point probe_until{};   // throwprobe S (test)
    std::set<Key>                         probe_seen;
    struct HudTex { UINT w{ 0 }, h{ 0 }; int fmt{ 0 }, slot{ 0 }; uint64_t n{ 0 }; };
    std::chrono::steady_clock::time_point hp_until{};   // hudtexprobe S (test)
    std::map<void*, HudTex>               hp;
    std::chrono::steady_clock::time_point ap_until{};   // throwallprobe S (test)
    int                                   ap_frame{ -1 }, ap_rb_frames{ 0 };
    std::set<std::tuple<UINT, UINT, INT>> ap_base, ap_seen;

    // Weapon-pass draws by frame: the last idle frame's set is "the gun"; while a throw is under way anything else is
    // the throwable.
    int           set_frame{ -1 };
    // By shape (count, start, base), not by index buffer: a weapon put back after a throw comes with new buffers (simulator 25 Oct: the gun's
    // parts looked new and spoiled the frames where the thrown throwable showed).
    using Shape = std::tuple<UINT, UINT, INT>;
    std::set<Shape> cur, idle;
    bool          idle_phase{ true };

    glm::vec3 F{ 0.0f };   // the game's left hand (gloves' left-hand bones' centre), q axes (last gloves draw)
    int       F_frame{ -100 };

    std::vector<Rec> recs;
    std::vector<Rec> pend;              // this frame's recording, committed at the next frame unless it was a junk frame
    int              pend_frame{ -1 };
    bool             pend_peek{ false };
    int              rec_frame{ -1 };
    int              junk_frame{ -1 };   // a frame with a big new mesh (the gun coming back): nothing recorded or moved in it
    bool             grab_recorded{ false };
    std::atomic<bool> any{ false };   // recs not empty (read by the pad thread: throwable_recorded)
    uint64_t         moved{ 0 }, chest_draws{ 0 };

    ID3D11Buffer* standin[2]{};      // object transform (b1) and previous (b3) for the held throwable
    ID3D11Buffer* orig[3]{};         // VS b1..b3 as the game bound them
    ID3D11Buffer* chest_cb[kMaxRec]{};   // object transform per recorded draw, on the chest

    // The pouch (throwpouch): a small bag at the chest point, built in the gloves' vertex format from two of their own
    // vertices (texture places, defaults) and drawn with the gloves' shaders, textures and lighting, hanging on a bone
    // palette of its own.
    std::atomic<bool>  pouch{ true };   // on by default since 28 Oct (pouch mode, headset-confirmed; it was off from 18 Oct: "its just bad")
    std::atomic<float> pouch_up{ 0.03f };   // throwpouchup: the throwable's origin above the pouch's centre (m); a knife (origin at one end) then stands ~20 cm out of the top
    // The pouch's mesh per host draw: 0 the hands' draw, 1 the first-person clothing draw (throwpouchmat cloth). Each is built in its
    // host's own vertex format, from its host's own vertices.
    struct PouchMesh
    {
        ID3D11Buffer*      vb{ nullptr };
        ID3D11Buffer*      ib{ nullptr };
        UINT               n{ 0 };
        UINT               stride{ 0 };
        ID3D11InputLayout* il{ nullptr };     // the layout it was built for (rebuilt if the host's changes)
        ID3D11Buffer*      host{ nullptr };   // cloth: the clothing mesh's vertex buffer it was built from (compared only)
        int                host_frame{ -1 };  // cloth: the last frame that mesh was drawn
        bool               srv_logged{ false };
        void release()
        {
            if (vb) vb->Release();
            if (ib) ib->Release();
            vb = ib = nullptr;
            il     = nullptr;
            host   = nullptr;
        }
    };
    PouchMesh          pm[2];
    std::atomic<bool>  pouch_rebuild{ false };   // set by the pouch commands: both meshes rebuilt at the next draw (on the render thread)
    ID3D11Buffer*      pouch_bones{ nullptr };
    bool               pouch_failed{ false };
    // throwpouchmat (30 Oct, Homard 09:25 "improve the quality of the pouch model ? Any way to make it reactive to light in game ?"): with
    // his bare arms the pouch took the skin's material (#D31033ED, its makeup and tattoo textures). cloth (default): it is drawn in the
    // first-person clothing draw (a Character_Clothing material, his shirt and sleeves #2892C62B), hidden with the arms, so it takes
    // their cloth texture. The game lights it either way (a G-buffer draw, lit by the deferred lighting with the world).
    std::atomic<int>   pouch_mat{ 1 };          // 0 hands, 1 cloth
    int                cloth_seen{ -1 };        // the last frame a usable clothing draw was seen
    int                cloth_wait{ -1 };        // the first frame the hands' draw waited for one (cloth)
    bool               cloth_fallback_logged{ false }, cloth_logged{ false };
    std::map<ID3D11PixelShader*, bool> cloth_ps;          // pixel shader -> a Character_Clothing material
    std::set<ID3D11VertexShader*>      cloth_vs_bad;      // clothing vertex shaders without the hands' constant slots (logged once)
    // 28 Oct (Homard: the pouch "did not show this time"; his 13:48 log: "stride 36"): the gloves are several draws, not all in the
    // 40-byte skinned format; the pouch was given up for the session at the first one. Now a draw in another format is passed over
    // (logged once per layout) and the pouch is built at, and drawn with, the first draw in the right one, once per frame.
    std::set<ID3D11InputLayout*> pouch_skip[2];
    int                pouch_frame{ -1 };
    uint64_t           copy_draws{ 0 };
    uint64_t           pouch_draws{ 0 };
    std::atomic<float> pouch_back{ 0.04f };   // throwpouchback: the pouch moved this far towards the body from the throwable's chest point (m)
    std::atomic<float> pouch_tex[2]{ 0.25f, 0.75f };   // throwpouchtex A B: where in the gloves' vertex buffer the body's and the flap's texture places are taken (0..1)
    std::atomic<bool>  pouch_v2{ true };     // throwpouchv2: the 29 Oct model (soft bag, lid and flap, band, stud, tab); off: the 27 Oct shape
    std::atomic<float> pouch_uv{ 0.04f };    // throwpouchuv: how much of the hands' surface (m) each pouch surface's texture spans; 0: one texel per part (as before)
    // throwv2rb grab: the copy goes in the hand only once this hold's own recording is in (the game's selected throwable, not the last one).
    bool               hand_ok{ false };
    // After a hold ends (put back or thrown; Homard 29 Oct: "when cancelling throwable, the throwable stay in hand"): the game keeps drawing
    // its throwable a while as it puts it away; the recorded parts stay hidden until a frame goes by without them (at most 10 s).
    bool               active_was{ false }, linger{ false };
    int                linger_last{ 0 };
    std::chrono::steady_clock::time_point linger_t0{};
    int                big_logs{ 0 }, big_frame{ -1 };
    bool               was_held{ false };
};

void FarCry5Stereo::throwable_note_support(const glm::mat4&, const glm::vec3& F)
{
    if (!m_th) m_th = new ThrowableState{};
    m_th->F       = F;
    m_th->F_frame = m_frame.load();
}

bool FarCry5Stereo::throwable_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, bool record_only)
{
    if (!m_th) m_th = new ThrowableState{};
    auto&      t     = *m_th;
    const int  frame = m_frame.load();
    const auto phase = fc5::gesture::throw_phase();
    const bool rb    = m_rb_down.load();   // any throw (left grip + B too) shows the game's throwable: recorded for the chest
    // Also while the game's throw animation lets it go (Follow). throwv2: in the hand before RB the game draws no throwable (the mod's
    // copy is drawn instead: throwable_hand), so only RB or the throw's own phases count.
    const bool held  = fc5::gesture::throw_v2() ? (rb || phase == fc5::gesture::ThrowPhase::Throw || phase == fc5::gesture::ThrowPhase::Follow)
                                                : phase != fc5::gesture::ThrowPhase::Idle;
    const bool peek  = fc5::gesture::throw_peeking();
    if (frame != t.set_frame) {
        const bool active = phase != fc5::gesture::ThrowPhase::Idle || rb;
        if (t.active_was && !active && fc5::gesture::throw_v2() && !t.recs.empty()) {
            t.linger      = true;
            t.linger_t0   = std::chrono::steady_clock::now();
            t.linger_last = frame;
        }
        if (active) t.linger = false;
        t.active_was = active;
        if (t.idle_phase) t.idle = std::move(t.cur);
        t.cur.clear();
        t.set_frame  = frame;
        t.idle_phase = phase == fc5::gesture::ThrowPhase::Idle && !rb;
        if (t.idle_phase) t.grab_recorded = false;
        // The last frame's recording, kept only if no big new mesh came in that frame (the gun coming back after a throw: headset 25 Oct,
        // a multi-part throwable recorded and thrown away again every frame while the gun came back): the chest keeps the last good one.
        if (!t.pend.empty()) {
            if (t.pend_frame != t.junk_frame) {
                for (auto& r : t.recs) r.release();
                t.recs          = std::move(t.pend);
                t.any           = true;
                t.grab_recorded = true;
                t.hand_ok       = phase == fc5::gesture::ThrowPhase::Held || phase == fc5::gesture::ThrowPhase::Aim;
                std::string parts;
                for (const auto& r : t.recs) parts += fmt::format(" {}{}", r.count, r.skinned ? " (skinned)" : "");
                spdlog::info("[Throwable] recorded for the chest: {} draws (indices:{}), {:.2f} m from the game camera{}", t.recs.size(), parts, glm::length(t.recs.front().tq),
                             t.pend_peek ? " (peek)" : "");
                if (t.pend_peek) fc5::gesture::throw_peek_seen();
            } else {
                for (auto& r : t.pend) r.release();
                spdlog::info("[Throwable] a recording dropped: a big new mesh came in the same frame (the gun coming back)");
            }
            t.pend.clear();
        }
    }
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   fmt{};
    UINT          ib_off = 0;
    ctx->IAGetIndexBuffer(&ib, &fmt, &ib_off);
    const Key key{ ib, start, count, base };
    if (t.linger) {
        if (frame - t.linger_last > 20 || std::chrono::steady_clock::now() - t.linger_t0 > std::chrono::seconds(10)) {
            t.linger = false;
            spdlog::info("[Throwable] the game's throwable put away ({:.1f} s after the hold ended)", std::chrono::duration<float>(std::chrono::steady_clock::now() - t.linger_t0).count());
        } else if (std::any_of(t.recs.begin(), t.recs.end(), [&](const Rec& r) { return r.key == key; })) {
            t.linger_last = frame;
            if (ib) ib->Release();
            return true;   // not drawn
        }
    }
    const ThrowableState::Shape shape{ count, start, base };
    t.cur.insert(shape);
    // throwprobe S (test): for S seconds every weapon-pass draw that the last idle frame did not have is logged once, whatever
    // the pad does (which draws a rock throw, a D-pad throw or a cancelled throw makes).
    if (std::chrono::steady_clock::now() < t.probe_until && !t.idle.empty() && !t.idle.contains(shape) && t.probe_seen.insert(key).second)
        spdlog::info("[ThrowProbe] frame {}: new weapon-pass draw {} indices from {} base {} (index buffer {}), RB {}, phase {}", frame, count, start, base, (void*)ib, rb, int(phase));
    if (!(held || rb) || t.idle.empty() || t.idle.contains(shape) || !fc5::gesture::throw_on()) {
        if (ib) ib->Release();
        return false;
    }
    // A big new mesh is not a throwable (simulator 25 Oct: with none left, RB brought back the gun's parts, 11040 and 16110 indices, which
    // were recorded and drawn in the hand): that frame records nothing (knife 2502, rock 582).
    // Headset 27 Oct (throwv2rb grab, Homard: "the molotov and dynamite were idle on the right side"): with RB held from the grab a big
    // new mesh came with them while still in the hand (no gun coming back then), and the frame's early return left the game's own
    // throwable drawn where the game holds it. Now a junk frame only records nothing (the game's throwable is still hidden), and while
    // the game holds the throw ready from the grab (Held, Aim) a big mesh does not make the frame junk.
    // Headset 27 Oct 21:10 (Homard: "still not follow the hand, tested with molotov and C4"): that big mesh is the throwable itself, the
    // molotov's main part (11040 indices from 0 in the index buffer of its 4122-index part): held from the grab it is recorded and hidden
    // like any part (the 25 Oct "gun's parts, 11040 and 16110" were most likely the molotov too).
    const bool ready = fc5::gesture::throw_v2_rb_grab() && (phase == fc5::gesture::ThrowPhase::Held || phase == fc5::gesture::ThrowPhase::Aim);
    if (count > 8000) {
        if (t.big_logs < 40 && t.big_frame != frame && (!ready || t.big_frame < frame - 1)) {
            t.big_logs++;
            spdlog::info("[Throwable] a big new mesh in the weapon pass: {} indices from {}, phase {}{}", count, start, int(phase), ready ? " (held from the grab: a part of the throwable)" : " (that frame records nothing)");
        }
        t.big_frame = frame;
        if (!ready) {
            t.junk_frame = frame;
            if (ib) ib->Release();
            return false;
        }
    }
    const bool junk = t.junk_frame == frame;
    if (count <= 8000) m_arc_sig = count;   // throwarc: the throwable in hand (its measured launch, per throwable)
    // The throwable. A rigid mesh: object transform at b1 (128 B), previous at b3.
    ID3D11Buffer* cb[4]{};
    ctx->VSGetConstantBuffers(0, 4, cb);
    float obj[32]{}, prev[32]{};
    bool  have = false;
    {
        std::lock_guard _{ m_obj_mtx };
        auto it1 = cb[1] ? m_obj_consts.find(cb[1]) : m_obj_consts.end();
        auto it3 = cb[3] ? m_obj_consts.find(cb[3]) : m_obj_consts.end();
        if (it1 != m_obj_consts.end() && it1->second.size == 128) {
            std::memcpy(obj, it1->second.f, sizeof(obj));
            std::memcpy(prev, (it3 != m_obj_consts.end() && it3->second.size == 128) ? it3->second.f : it1->second.f, sizeof(prev));
            have = true;
        }
    }
    const auto& hm    = m_hand_map;
    const bool  fresh = have && m_mo.valid && m_mo.other_valid && frame - hm.frame <= 1;
    if (t.log.load() > 0) {
        t.log--;
        spdlog::info("[Throwable] the game's throwable drawn: {} indices from {} (index buffer {}), object at ({:.2f}, {:.2f}, {:.2f}){}", count, start, (void*)ib, obj[3], obj[7], obj[11],
                     have ? "" : ", no rigid transform at b1: left as drawn");
    }
    // Record it for the chest, once per press (the draws of the first frame it shows, committed at the next frame if that frame was
    // clean). Simulator 25 Oct, Homard's save: his other throwable is not drawn at all while RB is held, only in the throw itself (Throw,
    // Follow; in the headset its first frame there had 3 parts), so those count too; a frame where the gun comes back is not clean.
    const bool rec_ok = held && !junk;
    if (fresh && rec_ok && !t.grab_recorded && (t.pend.empty() || t.pend_frame == frame)) {
        if (t.pend.empty()) {
            t.pend_frame = frame;
            t.pend_peek  = peek;
        }
        const bool dup = std::any_of(t.pend.begin(), t.pend.end(), [&](const Rec& r) { return r.key == key; });
        if ((int)t.pend.size() < kMaxRec && !dup) {
            Rec r{};
            r.key   = key;
            r.count = count;
            r.start = start;
            r.base  = base;
            ctx->IAGetInputLayout(&r.il);
            ctx->IAGetPrimitiveTopology(&r.topo);
            ctx->IAGetVertexBuffers(0, kVb, r.vb, r.stride, r.off);
            r.ib     = addref(ib);
            r.ib_fmt = fmt;
            r.ib_off = ib_off;
            ctx->VSGetShader(&r.vs, nullptr, nullptr);
            ctx->PSGetShader(&r.ps, nullptr, nullptr);
            ctx->PSGetShaderResources(0, kSrv, r.srv);
            ctx->PSGetSamplers(0, kSmp, r.smp);
            ctx->RSGetState(&r.rs);
            ctx->OMGetDepthStencilState(&r.dss, &r.ds_ref);
            ctx->OMGetBlendState(&r.bs, r.bf, &r.mask);
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            auto copy_cb = [&](ID3D11Buffer* src) -> ID3D11Buffer* {
                if (!src || !dev) return nullptr;
                D3D11_BUFFER_DESC d{};
                src->GetDesc(&d);
                D3D11_BUFFER_DESC n{};
                n.ByteWidth = d.ByteWidth;
                n.Usage     = D3D11_USAGE_DEFAULT;
                n.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                ID3D11Buffer* dst = nullptr;
                if (SUCCEEDED(dev->CreateBuffer(&n, nullptr, &dst)) && dst) ctx->CopyResource(dst, src);
                return dst;
            };
            r.vs_cb2 = copy_cb(cb[2]);
            {
                std::lock_guard _{ m_ps_mtx };
                if (auto it = m_layouts.find(r.il); it != m_layouts.end())
                    r.skinned = std::any_of(it->second.begin(), it->second.end(), [](const LayoutElem& e) { return _stricmp(e.semantic.c_str(), "BLENDINDICES") == 0; });
            }
            if (r.skinned) {
                ID3D11Buffer* bc[2]{};
                ctx->VSGetConstantBuffers(3, 2, bc);
                for (int i = 0; i < 2; ++i) {
                    r.bones[i] = copy_cb(bc[i]);
                    if (bc[i]) bc[i]->Release();
                }
            }
            ID3D11Buffer* pcb[kPsCb]{};
            ctx->PSGetConstantBuffers(0, kPsCb, pcb);
            for (int i = 0; i < kPsCb; ++i) {
                r.ps_cb_view[i] = pcb[i] && pcb[i] == cb[0];
                if (pcb[i] && !r.ps_cb_view[i]) r.ps_cb[i] = copy_cb(pcb[i]);
                if (pcb[i]) pcb[i]->Release();
            }
            if (dev) dev->Release();
            std::memcpy(r.obj, obj, sizeof(obj));
            const glm::mat4 O = from_rows(obj);
            r.rq              = hm.rot * glm::mat3{ O };
            r.tq              = hm.rot * (glm::vec3{ O[3] } - hm.cam);
            r.Fq              = t.F;
            t.pend.push_back(r);
        }
    }
    // In the hand: the move that puts the game's left hand on the left controller (as the gloves get it, FarCry5Stereo
    // weapon_draw_impl), from this frame's placement and the last gloves draw's hand centre.
    const bool hide = peek || fc5::gesture::throw_v2();
    if (!record_only && held && have && (hide || (fresh && t.hand.load() && frame - t.F_frame <= 2))) {
        // throwv2 with throwv2hand right: the right hand (its own controller, also with the weapon on the sling).
        glm::vec3 hp = m_mo.other_pos_world;
        glm::mat3 hr = m_mo.other_rot_world;
        throwable_hold_pose(hp, hr);
        const glm::mat3 rT = glm::transpose(hm.rot);
        const glm::mat3 WT = glm::transpose(hm.w_rot);
        const glm::mat3 LR = hr * hm.corr;
        const glm::mat3 G  = rT * WT * LR * hm.rot;
        const glm::vec3 c  = hm.cam + rT * (hm.grip + WT * (hp - hm.w_pos)) - rT * WT * LR * t.F;
        glm::mat4       A{ G };
        A[3] = glm::vec4{ c - G * hm.cam, 1.0f };
        float o1[32], o3[32];
        std::memcpy(o1, obj, sizeof(o1));
        to_rows(A * from_rows(obj), o1);
        if (peek || fc5::gesture::throw_v2()) {
            // throwpeek: the game's throwable is only looked at (recorded), not shown: collapsed to a point in front of the camera. throwv2: the
            // mod's copy is in the hand instead (the game's raise and throw animation do not move it).
            glm::mat4 Z{ 0.0f };
            Z[3] = glm::vec4{ hm.cam, 1.0f };
            to_rows(Z, o1);
        }
        std::memcpy(o3, o1, sizeof(o3));   // no motion of its own (as the moved gloves: DLSS takes it with the view)
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        for (auto*& s : t.standin) {
            if (s || !dev) continue;
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = 128;
            d.Usage     = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&d, nullptr, &s);
        }
        if (dev) dev->Release();
        if (t.standin[0] && t.standin[1] && !m_th_cb_swapped) {
            ctx->VSGetConstantBuffers(1, 3, t.orig);
            call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.standin[0]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o1), 0u, 0u);
            call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.standin[1]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o3), 0u, 0u);
            ID3D11Buffer* b[3]{ t.standin[0], t.orig[1], t.standin[1] };
            call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(b));
            m_th_cb_swapped   = true;
            m_hand_cb_swapped = true;   // the draw hooks restore through hands_cb_restore
            if (t.moved++ % 300 == 0)
                spdlog::info("[Throwable] in the left hand: moved from the game's left hand ({:.2f}, {:.2f}, {:.2f}) (q) to the left controller ({:.2f}, {:.2f}, {:.2f})", t.F.x, t.F.y,
                             t.F.z, m_mo.other_pos_world.x, m_mo.other_pos_world.y, m_mo.other_pos_world.z);
        }
    }
    for (auto* b : cb) if (b) b->Release();
    if (ib) ib->Release();
    // throwv2: a new draw while the throwable is held that has no rigid transform to collapse (a skinned throwable) is not drawn at all.
    return !record_only && !have && fc5::gesture::throw_v2();
}

void FarCry5Stereo::throwable_restore(ID3D11DeviceContext* ctx)
{
    m_th_cb_swapped = false;
    if (!m_th) return;
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 3u, static_cast<ID3D11Buffer* const*>(m_th->orig));
    for (auto*& p : m_th->orig) {
        if (p) p->Release();
        p = nullptr;
    }
}

// On the chest: the recorded draws, drawn just before the gloves (the gloves draw's targets and camera are bound), at the
// throwable's place on the chest, turned by throwrot in the body's frame. Every binding changed is put back.
void FarCry5Stereo::throwable_chest(ID3D11DeviceContext* ctx)
{
    if (!m_th) m_th = new ThrowableState{};
    auto& t = *m_th;
    throwable_pouch(ctx);
    // throwv2: in the hand before RB (the game draws nothing there then), the recorded copy goes to the holding hand instead.
    const auto ph      = fc5::gesture::throw_phase();
    const bool hold    = ph == fc5::gesture::ThrowPhase::Held || ph == fc5::gesture::ThrowPhase::Aim;
    if (hold && !t.was_held) t.hand_ok = false;   // a new grab: with throwv2rb grab, nothing in the hand until the game has drawn this one
    t.was_held = hold;
    // (The game's own throwable is hidden throughout with throwv2: throwable_draw.) Until RB is let go at the throw (Held, Aim).
    const bool in_hand = fc5::gesture::throw_v2() && hold && (!fc5::gesture::throw_v2_rb_grab() || t.hand_ok);
    if (t.recs.empty() || !fc5::gesture::throw_on() || (!in_hand && (!t.chest.load() || ph != fc5::gesture::ThrowPhase::Idle))) return;
    const auto& hm = m_hand_map;
    if (!m_mo.valid || !m_mo.chest_valid || m_frame.load() - hm.frame > 1 || in_vehicle() || flat_screen_on() || m_cine_on) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    for (auto*& s : t.chest_cb) {
        if (s) continue;
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 128;
        d.Usage     = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &s);
    }
    dev->Release();

    // Save what is changed.
    ID3D11InputLayout*        il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  topo{};
    ID3D11Buffer*             vb[kVb]{};
    UINT                      stride[kVb]{}, off[kVb]{};
    ID3D11Buffer*             ib = nullptr;
    DXGI_FORMAT               ib_fmt{};
    UINT                      ib_off = 0;
    ID3D11VertexShader*       vs = nullptr;
    ID3D11ClassInstance*      vs_ci[256]{};
    UINT                      vs_nci = 256;
    ID3D11PixelShader*        ps = nullptr;
    ID3D11ClassInstance*      ps_ci[256]{};
    UINT                      ps_nci = 256;
    ID3D11Buffer*             vcb[5]{};
    ID3D11Buffer*             pcb[kPsCb]{};
    ID3D11ShaderResourceView* srv[kSrv]{};
    ID3D11SamplerState*       smp[kSmp]{};
    ID3D11RasterizerState*    rs = nullptr;
    ID3D11DepthStencilState*  dss = nullptr;
    UINT                      ds_ref = 0;
    ID3D11BlendState*         bs = nullptr;
    float                     bf[4]{};
    UINT                      mask = 0;
    ctx->IAGetInputLayout(&il);
    ctx->IAGetPrimitiveTopology(&topo);
    ctx->IAGetVertexBuffers(0, kVb, vb, stride, off);
    ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
    ctx->VSGetShader(&vs, vs_ci, &vs_nci);
    ctx->PSGetShader(&ps, ps_ci, &ps_nci);
    ctx->VSGetConstantBuffers(0, 5, vcb);
    ctx->PSGetConstantBuffers(0, kPsCb, pcb);
    ctx->PSGetShaderResources(0, kSrv, srv);
    ctx->PSGetSamplers(0, kSmp, smp);
    ctx->RSGetState(&rs);
    ctx->OMGetDepthStencilState(&dss, &ds_ref);
    ctx->OMGetBlendState(&bs, bf, &mask);

    // The chest's frame: the body's heading (throw_pose), turned by throwrot.
    const glm::mat3 Rc = m_mo.chest_rot_world * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(t.rot[0].load()), glm::vec3{ 0, 1, 0 }) *
                                                           glm::rotate(glm::mat4{ 1.0f }, glm::radians(t.rot[1].load()), glm::vec3{ 1, 0, 0 }) *
                                                           glm::rotate(glm::mat4{ 1.0f }, glm::radians(t.rot[2].load()), glm::vec3{ 0, 0, -1 }) };
    const glm::mat3 rT = glm::transpose(hm.rot);
    const glm::mat3 WT = glm::transpose(hm.w_rot);
    // With the pouch shown, the throwable sticks out of its top (throwpouchup m above the pouch's centre).
    const glm::vec3 chest_at = m_mo.chest_pos_world + (t.pouch.load() ? m_mo.chest_rot_world * glm::vec3{ 0.0f, t.pouch_up.load(), 0.0f } : glm::vec3{ 0.0f });
    // The first draw's origin goes to the chest point; the others keep their place relative to it.
    if (m_census_frame.load() == m_frame.load()) spdlog::info("[Census] throwable: {} recorded draws drawn on the chest before weapon draw #{} (the gloves), frame {}", t.recs.size(), m_weapon_draws_this_frame, m_frame.load());
    const glm::vec3 F0 = t.recs.front().tq;
    const glm::mat3 R0 = orthonormal(t.recs.front().rq);
    int             k  = 0;
    // In the hand (throwv2): the same move onto the holding controller, turned by throwhandrot (its axes: x right, y up, -z where it
    // points). (The game's own move from its left hand put the copy ~0.5 m off in the simulator: its hand point was not of that frame.)
    glm::vec3 hp = m_mo.other_pos_world;
    glm::mat3 hr = m_mo.other_rot_world;
    throwable_hold_pose(hp, hr);
    const auto&     hrot = t.recs.front().count == 2502 ? t.hand_rot : t.hand_rot2;   // the knife, or any other throwable
    const glm::mat3 Rh = hr * glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(hrot[0].load()), glm::vec3{ 0, 1, 0 }) *
                                         glm::rotate(glm::mat4{ 1.0f }, glm::radians(hrot[1].load()), glm::vec3{ 1, 0, 0 }) *
                                         glm::rotate(glm::mat4{ 1.0f }, glm::radians(hrot[2].load()), glm::vec3{ 0, 0, -1 }) };
    const glm::vec3 place = in_hand ? hp + hr * glm::vec3{ t.hand_off[0].load(), t.hand_off[1].load(), t.hand_off[2].load() } : chest_at;
    for (auto& r : t.recs) {
        if (!t.chest_cb[k]) break;
        // q -> world now (this frame's game camera), then the move that puts q point F0 with axes R0 at the chest with Rc.
        glm::mat4 Oq{ glm::mat3{ rT * r.rq } };
        Oq[3]              = glm::vec4{ hm.cam + rT * r.tq, 1.0f };
        const glm::mat3 LR = (in_hand ? Rh : Rc) * glm::transpose(R0);
        const glm::mat3 G  = rT * WT * LR * hm.rot;
        const glm::vec3 c  = hm.cam + rT * (hm.grip + WT * (place - hm.w_pos)) - rT * WT * LR * F0;
        glm::mat4       A{ G };
        A[3] = glm::vec4{ c - G * hm.cam, 1.0f };
        float o[32];
        std::memcpy(o, r.obj, sizeof(o));
        to_rows(A * Oq, o);
        call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.chest_cb[k]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o), 0u, 0u);

        ctx->IASetInputLayout(r.il);
        ctx->IASetPrimitiveTopology(r.topo);
        ctx->IASetVertexBuffers(0, kVb, r.vb, r.stride, r.off);
        ctx->IASetIndexBuffer(r.ib, r.ib_fmt, r.ib_off);
        ctx->VSSetShader(r.vs, nullptr, 0);
        call_orig<void>(m_ps_set_hook, 9, ctx, r.ps, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
        ID3D11Buffer* v[5]{ vcb[0], t.chest_cb[k], r.vs_cb2, r.skinned ? r.bones[0] : t.chest_cb[k], r.skinned ? r.bones[1] : vcb[4] };
        call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 5u, static_cast<ID3D11Buffer* const*>(v));
        ID3D11Buffer* p[kPsCb]{};
        for (int i = 0; i < kPsCb; ++i) p[i] = r.ps_cb_view[i] ? vcb[0] : r.ps_cb[i];
        ctx->PSSetConstantBuffers(0, kPsCb, p);
        ctx->PSSetShaderResources(0, kSrv, r.srv);
        call_orig<void>(m_ps_samplers_hook, 10, ctx, 0u, UINT(kSmp), static_cast<ID3D11SamplerState* const*>(r.smp));
        ctx->RSSetState(r.rs);
        ctx->OMSetDepthStencilState(r.dss, r.ds_ref);
        ctx->OMSetBlendState(r.bs, r.bf, r.mask);
        call_orig<void>(m_draw_idx_hook, 12, ctx, r.count, r.start, r.base);
        fp_eye_again(ctx, r.count, r.start, r.base);   // afwcopies: the held eye's weapon pass too (AFW)
        ++k;
    }
    if (in_hand ? t.copy_draws++ % 90 == 0 : t.chest_draws++ % 600 == 0)
        spdlog::info("[Throwable] {}: {} recorded draws at ({:.2f}, {:.2f}, {:.2f}) world", in_hand ? "the copy in the hand" : "on the chest", k, place.x, place.y, place.z);

    // Put everything back.
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(topo);
    ctx->IASetVertexBuffers(0, kVb, vb, stride, off);
    ctx->IASetIndexBuffer(ib, ib_fmt, ib_off);
    ctx->VSSetShader(vs, vs_ci, vs_nci);
    call_orig<void>(m_ps_set_hook, 9, ctx, ps, static_cast<ID3D11ClassInstance* const*>(ps_ci), ps_nci);
    call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 5u, static_cast<ID3D11Buffer* const*>(vcb));
    ctx->PSSetConstantBuffers(0, kPsCb, pcb);
    ctx->PSSetShaderResources(0, kSrv, srv);
    call_orig<void>(m_ps_samplers_hook, 10, ctx, 0u, UINT(kSmp), static_cast<ID3D11SamplerState* const*>(smp));
    ctx->RSSetState(rs);
    ctx->OMSetDepthStencilState(dss, ds_ref);
    ctx->OMSetBlendState(bs, bf, mask);
    auto rel = [](auto* x) { if (x) x->Release(); };
    rel(il); rel(ib); rel(vs); rel(ps); rel(rs); rel(dss); rel(bs);
    for (UINT i = 0; i < vs_nci && i < 256; ++i) rel(vs_ci[i]);
    for (UINT i = 0; i < ps_nci && i < 256; ++i) rel(ps_ci[i]);
    for (auto* x : vb) rel(x);
    for (auto* x : vcb) rel(x);
    for (auto* x : pcb) rel(x);
    for (auto* x : srv) rel(x);
    for (auto* x : smp) rel(x);
}

namespace
{
    bool read_buf(ID3D11DeviceContext* ctx, ID3D11Buffer* b, std::vector<uint8_t>& out)
    {
        D3D11_BUFFER_DESC d{};
        b->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        ID3D11Buffer* st = nullptr;
        if (dev) { dev->CreateBuffer(&d, nullptr, &st); dev->Release(); }
        if (!st) return false;
        ctx->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m{};
        const bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
            ctx->Unmap(st, 0);
        }
        st->Release();
        return ok;
    }

    // A unit vector in R10G10B10A2_UNORM (v * 0.5 + 0.5), keeping the template's two top bits.
    uint32_t pack_dir(const glm::vec3& v, uint32_t tmpl)
    {
        auto q = [](float f) { return uint32_t(std::clamp(std::lround((f * 0.5f + 0.5f) * 1023.0f), 0L, 1023L)); };
        return q(v.x) | (q(v.y) << 10) | (q(v.z) << 20) | (tmpl & 0xC0000000u);
    }

    // The pouch in the gloves' 40-byte vertex (Position0 R16G16B16A16_SINT at 0, TexCoord0/1 at 8/12, BlendWeight0 at 16,
    // BlendIndices0 at 20, TexCoord6/7 at 24/28, Normal0 and Fog0 (tangent) R10G10B10A2 at 32/36), in metres, x right, y up,
    // z back (towards the body), every vertex on bone 0. Faces are drawn both ways round (the gloves' culling is the game's).
    // 27 Oct (the 18 Oct box "just bad", Homard): a bag with cut corners (an eight-sided section), a lid wider than the bag, a
    // tab down its front with a press stud; tilted 15 deg with its top forward so the lid shows from above. Bag: tmpl_a's
    // texture place; lid, tab and stud: tmpl_b's.
    // Where the pouch writes in the hands' vertex (offsets; -1: not in this format). 40 bytes with gloves; 36 with bare hands (28 Oct,
    // Homard's skin without gloves: TexCoord1 missing, the rest 4 bytes earlier).
    struct PouchFmt { int stride = 40, bw = 16, bi = 20, tc6 = 24, tc7 = 28, nrm = 32, tan = 36; };
    void build_pouch(const uint8_t* tmpl_a, const uint8_t* tmpl_b, float scale, const PouchFmt& f, std::vector<uint8_t>& vb, std::vector<uint16_t>& ib)
    {
        const glm::mat3 tilt = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-15.0f), glm::vec3{ 1, 0, 0 }) };   // top towards -z (forward)
        auto put = [&](const glm::vec3& p0, const glm::vec3& n0, const glm::vec3& u0, const uint8_t* tm) {
            const glm::vec3 p = tilt * p0, n = tilt * n0, u = tilt * u0;
            uint8_t vtx[64];
            std::memcpy(vtx, tm, f.stride);
            const int16_t pi[3]{ int16_t(std::lround(p.x / scale)), int16_t(std::lround(p.y / scale)), int16_t(std::lround(p.z / scale)) };
            std::memcpy(vtx, pi, 6);
            for (int o : { f.bw, f.bi, f.tc6, f.tc7 }) if (o >= 0) std::memset(vtx + o, 0, 4);   // blend weights, indices (bone 0), TexCoord6/7
            if (f.bw >= 0) vtx[f.bw + 2] = 255;   // B8G8R8A8: x is the third byte
            for (int k = 0; k < 2; ++k) {
                const int o = k ? f.tan : f.nrm;
                if (o < 0) continue;
                uint32_t d = 0;
                std::memcpy(&d, tm + o, 4);
                d = pack_dir(k ? u : n, d);
                std::memcpy(vtx + o, &d, 4);
            }
            vb.insert(vb.end(), vtx, vtx + f.stride);
        };
        auto tri = [&](uint16_t a, uint16_t b, uint16_t c) {
            for (uint16_t i : { a, b, c, a, c, b }) ib.push_back(i);
        };
        // A prism: an eight-sided section (half width w, half depth d, corners cut by c) from y0 to y1 at z offset z0.
        auto prism = [&](float w, float d, float c, float y0, float y1, float z0, const uint8_t* tm) {
            const glm::vec2 s[8]{ { w - c, -d }, { w, -d + c }, { w, d - c }, { w - c, d }, { -w + c, d }, { -w, d - c }, { -w, -d + c }, { -w + c, -d } };
            for (int i = 0; i < 8; ++i) {
                const glm::vec2 a = s[i], b = s[(i + 1) % 8], e = glm::normalize(b - a);
                const glm::vec3 n{ e.y, 0.0f, -e.x };   // outward (the section runs counter-clockwise seen from +y with z down the page)
                const uint16_t  v0 = uint16_t(vb.size() / f.stride);
                put({ a.x, y0, a.y + z0 }, n, { e.x, 0.0f, e.y }, tm);
                put({ b.x, y0, b.y + z0 }, n, { e.x, 0.0f, e.y }, tm);
                put({ b.x, y1, b.y + z0 }, n, { e.x, 0.0f, e.y }, tm);
                put({ a.x, y1, a.y + z0 }, n, { e.x, 0.0f, e.y }, tm);
                tri(v0, uint16_t(v0 + 1), uint16_t(v0 + 2));
                tri(v0, uint16_t(v0 + 2), uint16_t(v0 + 3));
            }
            for (float y : { y0, y1 }) {
                const uint16_t v0 = uint16_t(vb.size() / f.stride);
                for (const auto& p : s) put({ p.x, y, p.y + z0 }, { 0.0f, y == y1 ? 1.0f : -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, tm);
                for (int i = 1; i < 7; ++i) tri(v0, uint16_t(v0 + i), uint16_t(v0 + i + 1));
            }
        };
        auto box = [&](const glm::vec3& c, const glm::vec3& h, const uint8_t* tm) { prism(h.x, h.z, 0.3f * std::min(h.x, h.z), c.y - h.y, c.y + h.y, c.z, tm); };
        prism(0.060f, 0.026f, 0.012f, -0.060f, 0.034f, 0.0f, tmpl_a);        // the bag (12 x 9.4 x 5.2 cm)
        prism(0.064f, 0.030f, 0.014f, 0.030f, 0.046f, -0.001f, tmpl_b);     // the lid, 4 mm wider all round
        box({ 0.0f, 0.016f, -0.0315f }, { 0.017f, 0.022f, 0.0025f }, tmpl_b);   // the tab down the front
        box({ 0.0f, 0.002f, -0.0350f }, { 0.006f, 0.006f, 0.0015f }, tmpl_b);   // the press stud
    }

    // throwpouchv2 (29 Oct, Homard "improve the pouch model"): the 27 Oct pouch read as a dark block, flat-shaded and one
    // texel of the hands' texture over every face. This one is a soft bag: a rounded-rectangle section (corners 13 mm,
    // rounded bottom edge) whose front and sides bulge a little, smooth normals; a domed lid over the top with a rounded rim;
    // a flap down the front with rounded corners and 3 mm of edge; a press stud on the flap, a pull tab under it, and a
    // webbing band round the lower bag. Same size and place as before (11 x 9.2 x 5.2 cm, tilted 15 deg, top forward).
    // With throwpouchuv each surface also spreads over a patch of the hands' texture (PouchUV) instead of a single texel,
    // so their weave and normal map show. Bag, bottom, stud and tab: tmpl_a's texture place; lid, flap and band: tmpl_b's.
    //
    // TexCoord0 in the hands' vertex: 0 none (left as the template's, as before), 1 R16G16_FLOAT, 2 a 16-bit signed pair
    // (SNORM or SINT), 3 a 16-bit unsigned pair (UNORM or UINT). c: the window's centre per template, h: its half size,
    // both in the format's own units (the integer value for 2 and 3).
    struct PouchUV { int fmt = 0; float c[2][2]{}; float h[2]{}; };

    float half_to_f(uint16_t v)
    {
        const uint32_t s = uint32_t(v & 0x8000u) << 16, e = (v >> 10) & 0x1Fu, m = v & 0x3FFu;
        uint32_t b;
        if (e == 0) {
            if (m == 0) b = s;
            else {
                int ee = -1;
                uint32_t mm = m;
                do { ++ee; mm <<= 1; } while (!(mm & 0x400u));
                b = s | (uint32_t(127 - 15 - ee) << 23) | ((mm & 0x3FFu) << 13);
            }
        } else if (e == 31) b = s | 0x7F800000u | (m << 13);
        else b = s | ((e + 112u) << 23) | (m << 13);
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }

    uint16_t f_to_half(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        const uint16_t s = uint16_t((b >> 16) & 0x8000u);
        const int      e = int((b >> 23) & 0xFFu) - 127 + 15;
        const uint32_t m = b & 0x7FFFFFu;
        if (e <= 0) return s;   // a texture coordinate never needs the subnormals
        if (e >= 31) return uint16_t(s | 0x7C00u);
        uint32_t h = (uint32_t(e) << 10) | (m >> 13);
        if ((m & 0x1FFFu) > 0x1000u || ((m & 0x1FFFu) == 0x1000u && (h & 1u))) ++h;   // round to nearest even
        return uint16_t(s | h);
    }

    void uv_get(const uint8_t* v, int fmt, float out[2])
    {
        uint16_t r[2];
        std::memcpy(r, v, 4);
        for (int k = 0; k < 2; ++k) out[k] = fmt == 1 ? half_to_f(r[k]) : fmt == 2 ? float(int16_t(r[k])) : float(r[k]);
    }

    void uv_set(uint8_t* v, int fmt, const float in[2])
    {
        uint16_t r[2];
        for (int k = 0; k < 2; ++k) {
            if (fmt == 1) r[k] = f_to_half(in[k]);
            else if (fmt == 2) r[k] = uint16_t(int16_t(std::clamp(std::lround(in[k]), -32768L, 32767L)));
            else r[k] = uint16_t(std::clamp(std::lround(in[k]), 0L, 65535L));
        }
        std::memcpy(v, r, 4);
    }

    // The texture windows: how many TexCoord0 units one metre of the hands' surface spans (the median over consecutive
    // vertices 2-30 mm apart, which are mostly neighbours on the mesh), then for each template a square window spanning
    // size_m of hand surface round its texture place, kept inside the texture places of the hand vertices round it (within
    // 4 cm and on the same texture island), so it does not run into another island or the texture's empty space.
    // false (and a log line): one texel per part, as before.
    bool pouch_uv_window(const uint8_t* v, size_t nv, int stride, float scale, int uv_off, int fmt, const uint8_t* const tmpl[2], float size_m, PouchUV& out, bool log = true)
    {
        auto pos = [&](const uint8_t* p) {
            int16_t q[3];
            std::memcpy(q, p, 6);
            return glm::vec3{ q[0], q[1], q[2] } * scale;
        };
        auto uv = [&](const uint8_t* p) {
            float t[2];
            uv_get(p + uv_off, fmt, t);
            return glm::vec2{ t[0], t[1] };
        };
        std::vector<float> r;
        for (size_t i = 0; i + 1 < nv; ++i) {
            const uint8_t* a  = v + i * stride;
            const uint8_t* b  = a + stride;
            const float    dp = glm::length(pos(b) - pos(a)), du = glm::length(uv(b) - uv(a));
            if (dp > 0.002f && dp < 0.03f && du > 0.0f && std::isfinite(du)) r.push_back(du / dp);
        }
        if (r.size() < 50) {
            if (log) spdlog::info("[Throwable] pouch texture: {} usable vertex pairs, too few: one texel per part", r.size());
            return false;
        }
        std::nth_element(r.begin(), r.begin() + r.size() / 2, r.end());
        const float dens = r[r.size() / 2];   // units per metre
        out.fmt          = fmt;
        for (int m = 0; m < 2; ++m) {
            const glm::vec3 p0 = pos(tmpl[m]);
            const glm::vec2 c0 = uv(tmpl[m]);
            glm::vec2       lo = c0, hi = c0;
            int             near_n = 0;
            for (size_t i = 0; i < nv; ++i) {
                const uint8_t*  p = v + i * stride;
                const glm::vec2 q = uv(p);
                if (glm::length(pos(p) - p0) < 0.04f && glm::length(q - c0) < dens * 0.06f) {
                    lo = glm::min(lo, q);
                    hi = glm::max(hi, q);
                    ++near_n;
                }
            }
            float h = std::min({ dens * size_m * 0.5f, (hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f });
            if (near_n < 4 || !(h > 0.0f)) h = 0.0f;
            const glm::vec2 c = h > 0.0f ? glm::clamp(c0, lo + h, hi - h) : c0;
            out.c[m][0] = c.x;
            out.c[m][1] = c.y;
            out.h[m]    = h;
            if (log) spdlog::info("[Throwable] pouch texture {}: {:.4g} units/m ({} pairs), window {:.4g} units ({:.1f} cm of hand) at ({:.4g}, {:.4g}), from {} hand vertices round ({:.4g}, {:.4g})",
                         m ? "flap" : "body", dens, r.size(), 2.0f * h, 200.0f * h / dens, c.x, c.y, near_n, c0.x, c0.y);
        }
        return true;
    }

    void build_pouch2(const uint8_t* tmpl_a, const uint8_t* tmpl_b, float scale, const PouchFmt& f, int uv_off, const PouchUV& uvw, std::vector<uint8_t>& vb, std::vector<uint16_t>& ib)
    {
        constexpr float pi = 3.14159265f, hp = 0.5f * pi;
        const glm::mat3 tilt = glm::mat3{ glm::rotate(glm::mat4{ 1.0f }, glm::radians(-15.0f), glm::vec3{ 1, 0, 0 }) };   // top towards -z (forward)
        const uint8_t*  tm[2]{ tmpl_a, tmpl_b };
        float           hand[2]{ 1.0f, 1.0f };   // each template's tangent handedness (the tangent's top bits)
        if (f.tan >= 0)
            for (int m = 0; m < 2; ++m) {
                uint32_t d = 0;
                std::memcpy(&d, tm[m] + f.tan, 4);
                hand[m] = (d >> 30) >= 2 ? 1.0f : -1.0f;
            }
        auto put = [&](const glm::vec3& p0, const glm::vec3& n0, const glm::vec3& u0, int m, const glm::vec2& w) {
            const glm::vec3 p = tilt * p0, n = tilt * n0, u = tilt * u0;
            uint8_t vtx[64];
            std::memcpy(vtx, tm[m], f.stride);
            const int16_t pi16[3]{ int16_t(std::lround(p.x / scale)), int16_t(std::lround(p.y / scale)), int16_t(std::lround(p.z / scale)) };
            std::memcpy(vtx, pi16, 6);
            for (int o : { f.bw, f.bi, f.tc6, f.tc7 }) if (o >= 0) std::memset(vtx + o, 0, 4);   // blend weights, indices (bone 0), TexCoord6/7
            if (f.bw >= 0) vtx[f.bw + 2] = 255;   // B8G8R8A8: x is the third byte
            for (int k = 0; k < 2; ++k) {
                const int o = k ? f.tan : f.nrm;
                if (o < 0) continue;
                uint32_t d = 0;
                std::memcpy(&d, tm[m] + o, 4);
                d = pack_dir(k ? u : n, d);
                std::memcpy(vtx + o, &d, 4);
            }
            if (uvw.fmt && uv_off >= 0 && uvw.h[m] > 0.0f) {
                const float t[2]{ uvw.c[m][0] + std::clamp(w.x, -1.0f, 1.0f) * uvw.h[m], uvw.c[m][1] + std::clamp(w.y, -1.0f, 1.0f) * uvw.h[m] };
                uv_set(vtx + uv_off, uvw.fmt, t);
            }
            vb.insert(vb.end(), vtx, vtx + f.stride);
        };
        // A surface P(s, t) sampled at ss x ts, both ways round. Normals from the surface, turned to face ref(s, t) (ref itself
        // where the surface folds to a point); the tangent along the texture's u. M(s, t): metric coordinates on the surface,
        // its longer side spanning the texture window; v flipped when the surface's handedness is not the template's.
        auto grid = [&](const std::vector<float>& ss, const std::vector<float>& ts, auto&& P, auto&& M, auto&& ref, int m) {
            glm::vec2 lo{ 1e9f }, hi{ -1e9f };
            for (float s : ss) for (float t : ts) { lo = glm::min(lo, M(s, t)); hi = glm::max(hi, M(s, t)); }
            const glm::vec2 mid  = 0.5f * (lo + hi);
            const float     half = std::max({ 0.5f * (hi.x - lo.x), 0.5f * (hi.y - lo.y), 1e-4f });
            const float     e    = 1e-4f;
            struct Fr { glm::vec3 n, u; float hs; };
            auto frame = [&](float s, float t) {
                const glm::vec3 ps = (P(s + e, t) - P(s - e, t)) / (2 * e), pt = (P(s, t + e) - P(s, t - e)) / (2 * e);
                const glm::vec2 ms = (M(s + e, t) - M(s - e, t)) / (2 * e), mt = (M(s, t + e) - M(s, t - e)) / (2 * e);
                const glm::vec3 r  = ref(s, t);
                glm::vec3       n  = glm::cross(ps, pt);
                if (glm::length(n) < 1e-8f * std::max(1e-6f, glm::length(ps) * glm::length(pt)) || glm::length(n) < 1e-12f) n = r;
                n = glm::normalize(n);
                if (glm::dot(n, r) < 0.0f) n = -n;
                // The surface's directions of texture u and v: [ps pt] = [T B] [ms mt].
                const float det = ms.x * mt.y - mt.x * ms.y;
                glm::vec3   T = ps, B = pt;
                if (std::fabs(det) > 1e-8f) { T = (ps * mt.y - pt * ms.y) / det; B = (pt * ms.x - ps * mt.x) / det; }
                T = T - n * glm::dot(n, T);
                if (glm::length(T) < 1e-8f) T = glm::cross(n, std::fabs(n.y) < 0.9f ? glm::vec3{ 0, 1, 0 } : glm::vec3{ 1, 0, 0 });
                T = glm::normalize(T);
                return Fr{ n, T, glm::dot(glm::cross(n, T), B) >= 0.0f ? 1.0f : -1.0f };
            };
            const float flip = frame(ss[ss.size() / 2], ts[ts.size() / 2]).hs == hand[m] ? 1.0f : -1.0f;
            const uint16_t v0 = uint16_t(vb.size() / f.stride);
            for (float t : ts)
                for (float s : ss) {
                    const Fr  fr = frame(s, t);
                    glm::vec2 w  = (M(s, t) - mid) / half;
                    w.y *= flip;
                    put(P(s, t), fr.n, fr.u, m, w);
                }
            const size_t ns = ss.size();
            for (size_t j = 0; j + 1 < ts.size(); ++j)
                for (size_t i = 0; i + 1 < ns; ++i) {
                    const uint16_t a = uint16_t(v0 + j * ns + i), b = uint16_t(a + 1), c = uint16_t(a + ns), d = uint16_t(c + 1);
                    for (uint16_t k : { a, b, d, a, d, c, a, d, b, a, c, d }) ib.push_back(k);
                }
        };
        auto steps = [](float a, float b, int n) {
            std::vector<float> v;
            for (int i = 0; i <= n; ++i) v.push_back(a + (b - a) * float(i) / float(n));
            return v;
        };
        auto join = [](std::vector<float> a, const std::vector<float>& b) {
            a.insert(a.end(), b.begin() + (a.empty() ? 0 : 1), b.end());
            return a;
        };

        // The section: a rounded rectangle, half width W, half depth D, corners R; s is the arc length round it from the front's
        // middle towards +x, off pushes it out (negative: in; past the corners' radius the corners stay square).
        const float W = 0.055f, D = 0.026f, R = 0.013f, Yb = -0.060f, Yt = 0.032f, Rb = 0.010f, H = Yt - Yb;
        const float sa = W - R, sb = D - R, q = hp * R, L = 4 * sa + 4 * sb + 4 * q;
        auto ring = [=](float s, float off, glm::vec2& n) {
            s = std::fmod(s, L);
            if (s < 0.0f) s += L;
            const float r = std::max(R + off, 0.0f);
            glm::vec2   p;
            if (s < sa) { n = { 0, -1 }; p = { s, -D - off }; }
            else if ((s -= sa) < q) { n = { std::sin(s / R), -std::cos(s / R) }; p = glm::vec2{ sa, -sb } + r * n; }
            else if ((s -= q) < 2 * sb) { n = { 1, 0 }; p = { W + off, -sb + s }; }
            else if ((s -= 2 * sb) < q) { n = { std::cos(s / R), std::sin(s / R) }; p = glm::vec2{ sa, sb } + r * n; }
            else if ((s -= q) < 2 * sa) { n = { 0, 1 }; p = { sa - s, D + off }; }
            else if ((s -= 2 * sa) < q) { n = { -std::sin(s / R), std::cos(s / R) }; p = glm::vec2{ -sa, sb } + r * n; }
            else if ((s -= q) < 2 * sb) { n = { -1, 0 }; p = { -W - off, sb - s }; }
            else if ((s -= 2 * sb) < q) { n = { -std::cos(s / R), -std::sin(s / R) }; p = glm::vec2{ -sa, -sb } + r * n; }
            else { s -= q; n = { 0, -1 }; p = { -sa + s, -D - off }; }
            return glm::clamp(p, -glm::vec2{ W + off, D + off }, glm::vec2{ W + off, D + off });
        };
        // The bag's wall: s round, v up from the bottom's flat (v = 0) over the rounded bottom edge (La) and up the side; above
        // the top it goes on straight up (for the flap). The front and back bulge 4 mm at the middle, the sides 1.5 mm.
        const float La = hp * Rb;
        auto vy = [=](float y) { return La + (y - Yb - Rb); };
        auto body = [=](float s, float v, float extra) {
            float inset = 0.0f, y;
            if (v < La) {
                const float al = hp - v / Rb;
                inset = Rb * (1.0f - std::cos(al));
                y     = Yb + Rb - Rb * std::sin(al);
            } else y = Yb + Rb + (v - La);
            glm::vec2       n;
            const glm::vec2 p  = ring(s, -inset, n);
            const float     tt = std::clamp((y - Yb) / H, 0.0f, 1.0f), fy = 4.0f * tt * (1.0f - tt);
            const float     bu = fy * (0.004f * n.y * n.y * std::max(0.0f, 1.0f - (p.x / W) * (p.x / W)) + 0.0015f * n.x * n.x * std::max(0.0f, 1.0f - (p.y / D) * (p.y / D)));
            const glm::vec2 o  = p + n * (bu + extra);
            return glm::vec3{ o.x, y, o.y };
        };
        const glm::vec3 mid_c{ 0.0f, Yb + 0.5f * H, 0.0f };
        auto out_ref = [&](auto&& P) { return [&, P](float s, float t) { return P(s, t) - mid_c; }; };

        // Four panels round the bag with their seams at the corners' middles (a texture seam where a sewn one would be).
        const float m0 = sa + 0.5f * q, m1 = m0 + 0.5f * q + 2 * sb + 0.5f * q, m2 = m1 + q + 2 * sa, m3 = m2 + q + 2 * sb;
        const float pan[5]{ m3 - L, m0, m1, m2, m3 };
        auto panel_s = [&](float s0, float s1) {
            const float a = s0 + 0.5f * q, b = s1 - 0.5f * q;
            return join(join(steps(s0, a, 3), steps(a, b, std::max(2, int(std::lround((b - a) / 0.012f))))), steps(b, s1, 3));
        };
        std::vector<float> all_s;
        for (int i = 0; i < 4; ++i) all_s = join(all_s, panel_s(pan[i], pan[i + 1]));
        const float Vt = vy(Yt);

        // The bag: wall panels and the bottom (its flat closes by insetting the section to a line).
        for (int i = 0; i < 4; ++i) {
            auto P = [&](float s, float v) { return body(s, v, 0.0f); };
            grid(panel_s(pan[i], pan[i + 1]), join(steps(0.0f, La, 3), steps(La, Vt, 6)), P, [](float s, float v) { return glm::vec2{ s, v }; }, out_ref(P), 0);
        }
        {
            auto P = [&](float s, float k) { glm::vec2 n; const glm::vec2 p = ring(s, -Rb - k, n); return glm::vec3{ p.x, Yb, p.y }; };
            grid(all_s, steps(0.0f, D - Rb, 4), P, [&](float s, float k) { const glm::vec3 p = P(s, k); return glm::vec2{ p.x, p.z }; },
                 [](float, float) { return glm::vec3{ 0, -1, 0 }; }, 0);
        }
        // The webbing band round the lower bag (2.4 cm, 2 mm proud) with its two edges.
        {
            const float y0 = Yb + 0.014f, y1 = Yb + 0.038f, tb = 0.002f;
            for (int i = 0; i < 4; ++i) {
                auto P = [&](float s, float y) { return body(s, vy(y), tb); };
                grid(panel_s(pan[i], pan[i + 1]), steps(y0, y1, 2), P, [](float s, float y) { return glm::vec2{ s, y }; }, out_ref(P), 1);
            }
            for (float y : { y0, y1 }) {
                auto P = [&, y](float s, float k) { return body(s, vy(y), k); };
                grid(all_s, steps(0.0f, tb, 1), P, [](float s, float k) { return glm::vec2{ s, k }; },
                     [y, y1](float, float) { return glm::vec3{ 0, y == y1 ? 1.0f : -1.0f, 0 }; }, 1);
            }
        }
        // The lid: hangs 4 mm over the bag's top edge, 3.5 mm out from it, 3 mm thick above the top, a 2.5 mm round on its rim
        // and a 2 mm dome (the rim's section scaled in to the middle). Rim profile stations (out, height), then the dome.
        const float lo_ = 0.0035f, lip = 0.004f, T = 0.003f, rr = 0.0025f, dome = 0.002f;
        {
            std::vector<glm::vec2> st{ { lo_, Yt - lip }, { lo_, Yt + T - rr } };
            for (int k = 1; k <= 3; ++k) {
                const float b = hp * float(k) / 3.0f;
                st.push_back({ lo_ - rr + rr * std::cos(b), Yt + T - rr + rr * std::sin(b) });
            }
            auto P = [&, st](float s, float t) {
                glm::vec2 n;
                if (t > 4.0f) {
                    const float k = std::max(0.0f, 1.0f - (t - 4.0f) / 6.0f);
                    const glm::vec2 p = ring(s, lo_ - rr, n) * k;
                    return glm::vec3{ p.x, Yt + T + dome * (1.0f - k * k), p.y };
                }
                const int       i = std::clamp(int(std::floor(t)), 0, 3);
                const glm::vec2 a = glm::mix(st[i], st[i + 1], std::clamp(t - float(i), -0.5f, 1.5f));
                const glm::vec2 p = ring(s, a.x, n);
                return glm::vec3{ p.x, a.y, p.y };
            };
            grid(all_s, steps(0.0f, 10.0f, 10), P, [&](float s, float t) { const glm::vec3 p = P(s, t); return glm::vec2{ p.x, p.z }; },
                 [&](float s, float t) { return P(s, t) - glm::vec3{ 0.0f, Yt - 0.03f, 0.0f }; }, 1);
        }
        // The flap: from on the lid over its front rim and down the bag's front (4.5 cm, rounded corners 12 mm, 1.5 mm off the
        // bag, 3 mm thick), across the front's flat part and a little round the corners.
        const float e = 0.001f, g = 0.0015f, Lf = 0.045f, rc = 0.012f, Sf = sa + 0.004f;
        auto yend = [=](float s) {
            const float x = std::fabs(s) - (Sf - rc);
            return x <= 0.0f ? Yt - Lf : Yt - Lf + rc - std::sqrt(std::max(0.0f, rc * rc - x * x));
        };
        {
            const float ytop = Yt + T - rr, k0 = 1.0f - 0.013f / (D + lo_ - rr);
            // Stations 0-4: over the lid (on it, then round its rim); 4 + 0..1: down the front to the edge.
            auto P = [&](float s, float t) {
                if (t >= 4.0f) {
                    const float y = ytop + std::clamp((t - 4.0f) / 6.0f, -0.1f, 1.1f) * (yend(s) - ytop);
                    return body(s, vy(y), g + T);
                }
                glm::vec2 n;
                if (t <= 1.0f) {   // on the lid's dome, 13 mm in from its rim at the front
                    const float     k = glm::mix(k0, 1.0f, t);
                    const glm::vec2 p = ring(s, lo_ - rr, n) * k;
                    return glm::vec3{ p.x, Yt + T + dome * (1.0f - k * k) + e, p.y };
                }
                const float     b = hp * (1.0f - (t - 1.0f) / 3.0f);
                const glm::vec2 p = ring(s, lo_ - rr + (rr + e) * std::cos(b), n);
                return glm::vec3{ p.x, ytop + (rr + e) * std::sin(b), p.y };
            };
            // Texture v: the length down the flap (height on the front, round the rim, then in across the top).
            auto M = [&](float s, float t) {
                const float arc = ytop + (rr + e) * hp;
                if (t >= 4.0f) return glm::vec2{ s, P(s, t).y };
                if (t >= 1.0f) return glm::vec2{ s, ytop + (rr + e) * hp * (1.0f - (t - 1.0f) / 3.0f) };
                return glm::vec2{ s, arc + (1.0f - t) * 0.013f };
            };
            const auto ss = join(join(steps(-Sf, -Sf + rc, 4), steps(-Sf + rc, Sf - rc, 8)), steps(Sf - rc, Sf, 4));
            grid(ss, steps(0.0f, 10.0f, 10), P, M, out_ref(P), 1);
            // Its edge: along the bottom, and up the two sides to the lid's lip.
            auto Pe = [&](float s, float k) { return body(s, vy(yend(s)), k); };
            const glm::vec3 above = body(0.0f, vy(Yt - Lf + rc + 0.01f), 0.0f);
            grid(ss, steps(g, g + T, 1), Pe, [](float s, float k) { return glm::vec2{ s, k }; }, [&](float s, float k) { return Pe(s, k) - above; }, 1);
            for (float sx : { -Sf, Sf }) {
                auto Ps = [&, sx](float y, float k) { return body(sx, vy(y), k); };
                grid(steps(yend(sx), Yt - lip, 4), steps(g, g + T, 1), Ps, [](float y, float k) { return glm::vec2{ y, k }; },
                     [&, sx](float y, float k) { return Ps(y, k) - body(0.0f, vy(y), k); }, 1);
            }
        }
        // The press stud on the flap (12 mm across, 2 mm high, a 1 mm dome).
        {
            const float     ys = Yt - Lf + 0.012f, rs = 0.006f, hs = 0.002f;
            const glm::vec3 b0 = body(0.0f, vy(ys), g + T);
            const glm::vec3 nn = glm::normalize(glm::cross(body(0.0f, vy(ys) + 1e-3f, 0.0f) - body(0.0f, vy(ys) - 1e-3f, 0.0f), glm::vec3{ 1, 0, 0 }));
            const glm::vec3 X{ 1, 0, 0 }, Y = glm::normalize(glm::cross(nn, X));
            const glm::vec3 nf = glm::dot(nn, glm::vec3{ 0, 0, -1 }) < 0.0f ? -nn : nn;   // out of the front
            auto Pside = [&](float th, float h) { return b0 + nf * h + rs * (std::cos(th) * X + std::sin(th) * Y); };
            grid(steps(0.0f, 2 * pi, 16), steps(0.0f, hs, 1), Pside, [&](float th, float h) { return glm::vec2{ th * rs, h }; },
                 [&](float th, float h) { return Pside(th, h) - (b0 + nf * h); }, 0);
            auto Ptop = [&](float th, float k) { return b0 + nf * (hs + 0.001f * (1.0f - k * k)) + k * rs * (std::cos(th) * X + std::sin(th) * Y); };
            grid(steps(0.0f, 2 * pi, 16), steps(1.0f, 0.0f, 3), Ptop, [&](float th, float k) { return glm::vec2{ k * std::cos(th), k * std::sin(th) } * rs; },
                 [&](float, float) { return nf; }, 0);
        }
        // The pull tab: 16 mm wide, from under the flap's edge to 16 mm below it, a round end, 3 mm thick, on the bag.
        {
            const float wt = 0.008f, y0 = Yt - Lf + 0.004f, y1 = Yt - Lf - 0.016f, to = 0.0035f, ti = 0.0005f;
            auto hw = [=](float y) { const float d = y - (y1 + wt); return d >= 0.0f ? wt : std::sqrt(std::max(0.0f, wt * wt - d * d)); };
            auto P  = [&](float u, float t) { const float y = glm::mix(y0, y1, t); return body(u * hw(y), vy(y), to); };
            const glm::vec3 c = body(0.0f, vy(0.5f * (y0 + y1)), 0.0f);
            grid(steps(-1.0f, 1.0f, 4), steps(0.0f, 1.0f, 8), P, [&](float u, float t) { const float y = glm::mix(y0, y1, t); return glm::vec2{ u * hw(y), y }; }, out_ref(P), 0);
            for (float u : { -1.0f, 1.0f }) {
                auto Pe = [&, u](float t, float k) { const float y = glm::mix(y0, y1, t); return body(u * hw(y), vy(y), k); };
                grid(steps(0.0f, 1.0f, 8), steps(ti, to, 1), Pe, [&](float t, float k) { return glm::vec2{ glm::mix(y0, y1, t), k }; },
                     [&](float t, float k) { const glm::vec3 p = Pe(t, k); return glm::vec3{ p.x - c.x, p.y - c.y, 0.0f }; }, 0);
            }
        }
    }
}

// The pouch: drawn just before its host draw with everything that draw has bound (shaders, textures, camera, the object's
// constants) except the mesh and the bone palette, so it gets the host's material and the game's lighting. host 0: the hands'
// draw (gloves or bare hands); host 1: a first-person clothing draw (throwpouchmat cloth), which is hidden with the arms, with
// count/start/base its own (its mesh's vertices are the templates). With cloth, the hands' draw is the fallback when no usable
// clothing draw has been seen for 60 frames.
void FarCry5Stereo::throwable_pouch(ID3D11DeviceContext* ctx, int host, UINT count, UINT start, INT base)
{
    auto& t = *m_th;
    if (t.pouch_rebuild.exchange(false)) {
        for (auto& m : t.pm) { m.release(); m.srv_logged = false; }
        t.cloth_seen = t.cloth_wait = -1;
        t.cloth_fallback_logged = t.cloth_logged = false;
    }
    if (!t.pouch.load() || t.pouch_failed || !fc5::gesture::throw_on()) return;
    const auto& hm = m_hand_map;
    if (!m_mo.valid || !m_mo.chest_valid || m_frame.load() - hm.frame > 1 || in_vehicle() || flat_screen_on() || m_cine_on) return;
    const int  frame = m_frame.load();
    const bool cloth = t.pouch_mat.load() == 1;
    if (host == 1 && !cloth) return;
    // Drawn already this frame (the hands' draw as the fallback): a clothing draw is still built and counted as seen, not drawn.
    const bool drawn = t.pouch_frame == frame;
    if (host == 0 && drawn) return;
    if (host == 0 && cloth) {
        // Wait for a clothing draw (they come after the hands' in the frame): only after 60 frames without one is the pouch drawn here.
        if (t.cloth_wait < 0) t.cloth_wait = frame;
        if (frame - (t.cloth_seen >= 0 ? t.cloth_seen : t.cloth_wait) <= 60) return;
        if (!t.cloth_fallback_logged) {
            t.cloth_fallback_logged = true;
            spdlog::info("[Throwable] pouch: no usable first-person clothing draw for 60 frames: drawn in the hands' draw (throwpouchmat cloth)");
        }
    }
    auto& pm = t.pm[host];
    ID3D11InputLayout* il = nullptr;
    ctx->IAGetInputLayout(&il);
    if (il) il->Release();
    if (!il || t.pouch_skip[host].contains(il)) return;
    ID3D11Buffer* vb0 = nullptr;
    UINT          stride = 0, voff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb0, &stride, &voff);
    if (vb0) vb0->Release();   // compared only
    if (host == 1 && pm.vb && (il != pm.il || vb0 != pm.host)) {
        // Another clothing mesh: the pouch stays with the one it was built from while that is drawn (60 frames).
        if (frame - pm.host_frame <= 60) return;
        pm.release();
        pm.srv_logged = false;
    }
    if (host == 0 && pm.vb && il != pm.il) return;
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (vs) vs->Release();
    if (host == 1 && !pm.vb) {
        // The clothing draw's vertex shader must read the object, the geometry and the bones where the hands' does (b1, b2, b3/b4).
        if (t.cloth_vs_bad.contains(vs)) return;
        std::string vinfo;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_vs_info.find(vs); it != m_vs_info.end()) vinfo = it->second;
        }
        if (vinfo.find("CSceneTransformParameterProvider@b1 ") == std::string::npos || vinfo.find("CSceneGeometry@b2 ") == std::string::npos ||
            vinfo.find("CBlendMatricesCurrentParameterProvider@b3 ") == std::string::npos || vinfo.find("CBlendMatricesPreviousParameterProvider@b4 ") == std::string::npos) {
            t.cloth_vs_bad.insert(vs);
            spdlog::info("[Throwable] pouch: a clothing draw's vertex shader {} reads its constants elsewhere, passed over: {}", (void*)vs, vinfo.substr(0, 400));
            return;
        }
    }
    ID3D11Buffer* cb[3]{};
    ctx->VSGetConstantBuffers(0, 3, cb);
    float obj[12]{}, scale = 0.0f;
    bool  have_obj = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (cb[1]) if (auto it = m_obj_consts.find(cb[1]); it != m_obj_consts.end()) { std::memcpy(obj, it->second.f, sizeof(obj)); have_obj = true; }
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end() && it->second.f[5] > 1e-6f && it->second.f[5] < 1e-3f) scale = it->second.f[5];
    }
    for (auto* b : cb) if (b) b->Release();
    if (!have_obj || scale <= 0.0f) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    if (pm.il != il || !pm.vb) {
        // Built from the host's own vertex buffer and layout (once per layout; cloth: once per clothing mesh).
        pm.release();
        pm.il = il;
        std::vector<LayoutElem> layout;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
        }
        auto has = [&](const char* sem, UINT idx, DXGI_FORMAT f, UINT off) {
            return std::any_of(layout.begin(), layout.end(), [&](const LayoutElem& e) { return _stricmp(e.semantic.c_str(), sem) == 0 && e.index == idx && e.format == f && e.offset == off && e.slot == 0; });
        };
        ctx->IAGetVertexBuffers(0, 1, &vb0, &stride, &voff);
        // The pouch's offsets from the layout: the position (R16G16B16A16_SINT at 0), the blend weight and index (bone 0), the normal
        // (R10G10B10A2) needed; TexCoord6/7 (zeroed) and Fog0 (the tangent) where present.
        auto off = [&](const char* sem, UINT idx, DXGI_FORMAT f) {
            for (const auto& e : layout) if (_stricmp(e.semantic.c_str(), sem) == 0 && e.index == idx && (f == DXGI_FORMAT_UNKNOWN || e.format == f) && e.slot == 0) return int(e.offset);   // UNKNOWN: any format
            return -1;
        };
        PouchFmt pf;
        pf.stride = int(stride);
        pf.bw     = off("BLENDWEIGHT", 0, DXGI_FORMAT_B8G8R8A8_UNORM);
        pf.bi     = off("BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT);
        pf.tc6    = off("TEXCOORD", 6, DXGI_FORMAT_UNKNOWN);
        pf.tc7    = off("TEXCOORD", 7, DXGI_FORMAT_UNKNOWN);
        pf.nrm    = off("NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM);
        pf.tan    = off("FOG", 0, DXGI_FORMAT_R10G10B10A2_UNORM);
        // throwpouchv2's texture spread: TexCoord0 in a 16-bit pair format it can read and write (4 bytes, inside the vertex).
        const int uv_off = off("TEXCOORD", 0, DXGI_FORMAT_UNKNOWN);
        int       uv_fmt = 0;
        for (const auto& e : layout)
            if (_stricmp(e.semantic.c_str(), "TEXCOORD") == 0 && e.index == 0 && e.slot == 0) {
                if (e.format == DXGI_FORMAT_R16G16_FLOAT) uv_fmt = 1;
                else if (e.format == DXGI_FORMAT_R16G16_SNORM || e.format == DXGI_FORMAT_R16G16_SINT) uv_fmt = 2;
                else if (e.format == DXGI_FORMAT_R16G16_UNORM || e.format == DXGI_FORMAT_R16G16_UINT) uv_fmt = 3;
                if (uv_fmt == 0) spdlog::info("[Throwable] pouch texture: TexCoord0 format {} not read: one texel per part", int(e.format));
                break;
            }
        if (uv_off < 0 || uv_off + 4 > int(stride)) uv_fmt = 0;
        const bool fmt_ok = stride >= 28 && stride <= 64 && has("POSITION", 0, DXGI_FORMAT_R16G16B16A16_SINT, 0) && pf.bw >= 0 && pf.bi >= 0 && pf.nrm >= 0;
        std::vector<uint8_t> data;
        if (!fmt_ok || !vb0 || !read_buf(ctx, vb0, data) || data.size() < size_t(voff) + 80) {
            std::string els;
            for (const auto& e : layout) els += fmt::format(" {}{}@{}:{}", e.semantic, e.index, e.offset, int(e.format));
            spdlog::info("[Throwable] pouch: a {} draw in a vertex format it cannot build from (stride {}:{}) passed over", host ? "clothing" : "hands", stride, els);
            t.pouch_skip[host].insert(il);
            pm.il = nullptr;
            if (vb0) vb0->Release();
            dev->Release();
            return;
        }
        pm.host = vb0;
        if (vb0) vb0->Release();
        size_t nv = (data.size() - voff) / stride, v_lo = 0;
        if (host == 1 && count > 0) {
            // A clothing buffer can hold more than this mesh: the templates and the texture density come from this draw's own vertices.
            ID3D11Buffer* ib = nullptr;
            DXGI_FORMAT   ib_fmt{};
            UINT          ib_off = 0;
            ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
            std::vector<uint8_t> idx;
            if (ib && read_buf(ctx, ib, idx)) {
                const size_t w = ib_fmt == DXGI_FORMAT_R32_UINT ? 4 : 2;
                size_t lo = SIZE_MAX, hi = 0;
                for (size_t i = 0; i < count; ++i) {
                    const size_t at = ib_off + (size_t(start) + i) * w;
                    if (at + w > idx.size()) break;
                    uint32_t v = 0;
                    std::memcpy(&v, idx.data() + at, w);
                    const int64_t vi = int64_t(v) + base;
                    if (vi < 0 || size_t(vi) >= nv) continue;
                    lo = std::min(lo, size_t(vi));
                    hi = std::max(hi, size_t(vi));
                }
                if (lo <= hi && hi - lo + 1 >= 64) { v_lo = lo; nv = hi - lo + 1; }
            }
            if (ib) ib->Release();
        }
        const uint8_t* vdata = data.data() + voff + v_lo * stride;
        // Two of the host's vertices as templates: one a quarter of the way into the buffer, one at three quarters.
        const size_t ia = std::min(nv - 1, size_t(nv * t.pouch_tex[0].load()));
        size_t       jb = std::min(nv - 1, size_t(nv * t.pouch_tex[1].load()));
        const uint8_t* ta = vdata + ia * stride;
        const uint8_t* tb = vdata + jb * stride;
        std::vector<uint8_t>  vbd;
        std::vector<uint16_t> ibd;
        PouchUV uvw;
        const bool spread = t.pouch_v2.load() && uv_fmt && t.pouch_uv.load() > 0.0f;
        if (spread) {
            const uint8_t* const tm[2]{ ta, tb };
            if (!pouch_uv_window(vdata, nv, int(stride), scale, uv_off, uv_fmt, tm, t.pouch_uv.load(), uvw, false)) uvw = PouchUV{};
        }
        if (uv_fmt) {
            // Two-tone (30 Oct; the 10:27 gloves run had the body and the flap on the same texture place): the flap's template is stepped
            // through the buffer until its texture place is at least twice the larger window from the body's (with one texel per part:
            // not the same texel).
            auto uv = [&](const uint8_t* p) { float q[2]; uv_get(p + uv_off, uv_fmt, q); return glm::vec2{ q[0], q[1] }; };
            const float need = uvw.fmt ? 2.0f * std::max(2.0f * uvw.h[0], 2.0f * uvw.h[1]) : uv_fmt == 1 ? 1e-3f : 1.0f;
            const float d0   = glm::length(uv(ta) - uv(tb));
            if (d0 < need) {
                const size_t step = std::max<size_t>(1, nv / 64);
                size_t       j = jb, k = 0;
                float        d = d0;
                for (k = 1; k <= 64; ++k) {
                    j = (j + step) % nv;
                    if (j == ia) continue;
                    d = glm::length(uv(ta) - uv(vdata + j * stride));
                    if (d >= need) break;
                }
                if (d >= need) {
                    spdlog::info("[Throwable] pouch texture: the flap's template moved from vertex {} to {} of {} ({} steps): its texture place was {:.4g} units from the body's, now {:.4g} (needs {:.4g})",
                                 jb, j, nv, k, d0, d, need);
                    jb = j;
                    tb = vdata + jb * stride;
                } else
                    spdlog::info("[Throwable] pouch texture: the flap's texture place is {:.4g} units from the body's (needs {:.4g}); no vertex farther found, kept", d0, need);
            }
        }
        if (spread) {
            const uint8_t* const tm[2]{ ta, tb };
            if (!pouch_uv_window(vdata, nv, int(stride), scale, uv_off, uv_fmt, tm, t.pouch_uv.load(), uvw)) uvw = PouchUV{};
        }
        if (t.pouch_v2.load()) build_pouch2(ta, tb, scale, pf, uv_off, uvw, vbd, ibd);
        else build_pouch(ta, tb, scale, pf, vbd, ibd);
        D3D11_BUFFER_DESC     d{};
        D3D11_SUBRESOURCE_DATA s{};
        d.ByteWidth = UINT(vbd.size());
        d.Usage     = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        s.pSysMem   = vbd.data();
        dev->CreateBuffer(&d, &s, &pm.vb);
        d.ByteWidth = UINT(ibd.size() * 2);
        d.BindFlags = D3D11_BIND_INDEX_BUFFER;
        s.pSysMem   = ibd.data();
        dev->CreateBuffer(&d, &s, &pm.ib);
        pm.n      = UINT(ibd.size());
        pm.stride = stride;
        std::string psn;
        {
            ID3D11PixelShader* ps = nullptr;
            ctx->PSGetShader(&ps, nullptr, nullptr);
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_ps_names.find(ps); it != m_ps_names.end()) psn = it->second.substr(0, 9);
            if (ps) ps->Release();
        }
        spdlog::info("[Throwable] pouch built from the {} vertex buffer ({} bytes a vertex; pixel shader {}, {} of its vertices from {}): {} vertices, {} indices, position scale {:.3g} m",
                     host ? "clothing draw's" : "hands'", stride, psn, nv, v_lo, vbd.size() / stride, ibd.size(), scale);
    }
    if (!t.pouch_bones) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 12288;
        d.Usage     = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &t.pouch_bones);
    }
    dev->Release();
    if (!pm.vb || !pm.ib || !t.pouch_bones) return;
    if (host == 1) {
        t.cloth_seen  = frame;
        pm.host_frame = frame;
        if (drawn) return;
        if (!t.cloth_logged) spdlog::info("[Throwable] pouch: drawn in the first-person clothing draw ({} indices; throwpouchmat cloth)", count);
        t.cloth_logged = true;
    }
    if (!pm.srv_logged) {
        // Which texture is where (for a texture of the mod's own later): every pixel-shader texture bound at the host draw, once.
        pm.srv_logged = true;
        ID3D11ShaderResourceView* srv[kSrv]{};
        ctx->PSGetShaderResources(0, kSrv, srv);
        std::string list;
        for (int i = 0; i < kSrv; ++i) {
            if (!srv[i]) continue;
            ID3D11Resource* r = nullptr;
            srv[i]->GetResource(&r);
            D3D11_RESOURCE_DIMENSION dim{};
            if (r) r->GetType(&dim);
            if (r && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC td{};
                static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
                list += fmt::format(" t{} {}x{} format {} mips {} array {};", i, td.Width, td.Height, int(td.Format), td.MipLevels, td.ArraySize);
            } else
                list += fmt::format(" t{} dimension {};", i, int(dim));
            if (r) r->Release();
            srv[i]->Release();
        }
        spdlog::info("[Throwable] pouch: textures at the {} draw:{}", host ? "clothing" : "hands'", list.empty() ? " none" : list);
    }

    // Placement: pouch axes (x right, y up, z back) taken as the game camera's axes, then the move that puts that point
    // with those axes at the chest point with the body's axes (as the throwable on the chest). The host's shader draws
    // object * bone * vertex, so bone 0 = inverse(object) * that placement, object being the host draw's own (VS b1): the
    // pouch lands in the same place whichever draw it is drawn in.
    const glm::mat3 rT = glm::transpose(hm.rot);
    const glm::mat3 WT = glm::transpose(hm.w_rot);
    const glm::mat3 LR = m_mo.chest_rot_world;
    const glm::mat3 G  = rT * WT * LR * hm.rot;
    // throwpouchback (Homard 27 Oct "a bit closer to the body"): the pouch only, moved towards the body (the grab zone stays).
    const glm::vec3 at = m_mo.chest_pos_world + LR * glm::vec3{ 0.0f, 0.0f, t.pouch_back.load() };
    const glm::vec3 c  = hm.cam + rT * (hm.grip + WT * (at - hm.w_pos));
    glm::mat4       A{ G };
    A[3] = glm::vec4{ c - G * hm.cam, 1.0f };
    glm::mat4 Oq{ rT };
    Oq[3]             = glm::vec4{ hm.cam, 1.0f };
    const glm::mat4 B = glm::inverse(from_rows(obj)) * A * Oq;
    std::vector<float> bones(3072);
    for (int i = 0; i < 256; ++i) to_rows(B, bones.data() + i * 12);
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.pouch_bones), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(bones.data()), 0u, 0u);

    ID3D11Buffer* vb = nullptr;
    UINT          vstride = 0, off = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ib_fmt{};
    UINT          ib_off = 0;
    ID3D11Buffer* bcb[2]{};
    ctx->IAGetVertexBuffers(0, 1, &vb, &vstride, &off);
    ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
    ctx->VSGetConstantBuffers(3, 2, bcb);
    const UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &pm.vb, &pm.stride, &zero);
    ctx->IASetIndexBuffer(pm.ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11Buffer* pb[2]{ t.pouch_bones, t.pouch_bones };
    call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(pb));
    call_orig<void>(m_draw_idx_hook, 12, ctx, pm.n, 0u, 0);
    fp_eye_again(ctx, pm.n, 0u, 0);   // afwcopies: the held eye's weapon pass too (AFW)
    t.pouch_frame = frame;
    ctx->IASetVertexBuffers(0, 1, &vb, &vstride, &off);
    ctx->IASetIndexBuffer(ib, ib_fmt, ib_off);
    call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(bcb));
    if (vb) vb->Release();
    if (ib) ib->Release();
    for (auto* b : bcb) if (b) b->Release();
    if (t.pouch_draws++ % 600 == 0)
        spdlog::info("[Throwable] pouch drawn at ({:.2f}, {:.2f}, {:.2f}) world ({} draw)", m_mo.chest_pos_world.x, m_mo.chest_pos_world.y, m_mo.chest_pos_world.z,
                     host ? "clothing" : "hands'");
}

// throwpouchmat cloth: a first-person draw the arm hider hides (or a non-hand mesh of the gloves' shader). One with a
// Character_Clothing material (not skin, hair or eyes) is a host for the pouch.
void FarCry5Stereo::throwable_pouch_cloth(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!m_th || m_th->pouch_mat.load() != 1 || !m_th->pouch.load() || count == 0) return;
    auto& t = *m_th;
    if (t.cloth_seen == m_frame.load()) return;   // this frame's clothing draw has been used
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps) ps->Release();
    if (!ps) return;
    auto it = t.cloth_ps.find(ps);
    if (it == t.cloth_ps.end()) {
        std::string n;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto jt = m_ps_names.find(ps); jt != m_ps_names.end()) n = jt->second;
        }
        it = t.cloth_ps.emplace(ps, n.find("Character_Clothing") != std::string::npos).first;
        if (it->second) spdlog::info("[Throwable] pouch: clothing material for the pouch: pixel shader {} ({})", (void*)ps, n.substr(0, 9));
    }
    if (it->second) throwable_pouch(ctx, 1, count, start, base);
}

// F11 Gestures > Throwable and pouch (experimental): throwv2 and its choices (each through its command, so it is saved).
void FarCry5Stereo::draw_throw_v2_menu(const std::function<void(const char*, std::string)>&, const std::function<void(const char*)>& hint,
                                       const std::function<void(const char*)>& section)
{
    section("Take, aim with the trajectory, let go to throw");
    bool v = fc5::gesture::throw_v2();
    if (ImGui::Checkbox("Throwables: take, aim with the trajectory, throw by letting go of the right grip##throwv2", &v)) m_ui_cmds += v ? "throwv2 on\n" : "throwv2 off\n";
    // The pouch (Homard 27 Oct): four switches set together, each saved as itself.
    bool pz = m_th && m_th->pouch.load() && !m_th->chest.load() && fc5::gesture::throw_v2_rb_grab();
    if (ImGui::Checkbox("Pouch on the chest, the throwable only in the hand##throwpouchmode", &pz))
        m_ui_cmds += pz ? "throwpouch on\nthrowchest off\nthrowv2rb grab\nthrowpeek off\n" : "throwpouch off\nthrowchest on\nthrowv2rb arm\nthrowpeek on\n";
    hint("On (default): the left chest shows a pouch instead of a throwable. Taking from it holds the game's throw button at once, so the game draws "
         "whichever throwable is selected and that one appears in your hand (a moment after the grab). Putting it back cancels the game's "
         "throw (its X with the throw button held). No automatic peek. Off: the chest shows the last throwable seen (throwpouch, throwchest, "
         "throwv2rb arm, throwpeek).");
    if (m_th && ImGui::Button("Turn the throwable in the hand upside down (all but the knife)##throwhandrot2")) {
        float p = m_th->hand_rot2[1].load() + 180.0f;
        if (p > 180.0f) p -= 360.0f;
        m_ui_cmds += fmt::format("throwhandrot2 {:.0f} {:.0f} {:.0f}\n", m_th->hand_rot2[0].load(), p, m_th->hand_rot2[2].load());
    }
    hint("If a grenade, a molotov or another throwable stands the wrong way up in your hand, this flips it (saved; throwhandrot2, default 0 -90 0). "
         "The knife has its own turn (throwhandrot).");
    if (m_th && m_th->pouch.load()) {
        bool p2 = m_th->pouch_v2.load();
        if (ImGui::Checkbox("New pouch model (soft bag, lid and flap, strap, press stud)##throwpouchv2", &p2)) m_ui_cmds += p2 ? "throwpouchv2 on\n" : "throwpouchv2 off\n";
        hint("On (default): a rounded fabric pouch with a domed lid, a flap down its front with a press stud and a pull tab, and a strap round its "
             "lower part, smooth-shaded. Off: the 27 Oct pouch (an eight-sided box with a lid and a tab). Same size and place either way.");
        if (p2) {
            float uvcm = m_th->pouch_uv.load() * 100.0f;
            ImGui::SliderFloat("Pouch texture detail##throwpouchuv", &uvcm, 0.0f, 10.0f, "%.1f cm of glove");
            if (ImGui::IsItemDeactivatedAfterEdit()) m_ui_cmds += fmt::format("throwpouchuv {:.3f}\n", uvcm / 100.0f);
            hint("How much of your gloves' (or hands') texture each side of the pouch shows (default 4 cm): their weave and seams then show on the "
                 "pouch. 0: one plain colour per part, as before. Lower it if the pouch shows odd patches (a seam, a logo, skin).");
        }
        int mat = m_th->pouch_mat.load();
        ImGui::TextUnformatted("Pouch material");
        bool mc = ImGui::RadioButton("Your clothes (cloth)##throwpouchmat", &mat, 1);
        ImGui::SameLine();
        mc |= ImGui::RadioButton("Your hands (gloves or skin)##throwpouchmat", &mat, 0);
        if (mc) m_ui_cmds += mat ? "throwpouchmat cloth\n" : "throwpouchmat hands\n";
        hint("Clothes (default): the pouch is drawn with your shirt and sleeves' cloth material, so with bare arms it is fabric, not skin; if your "
             "outfit has none, it takes your hands' as before. Hands: your gloves' or your skin's. The game lights it either way, like your hands.");
    }
    hint("Taking the throwable at the left chest only puts it in your hand (the game's raise animation does not play). Hold both grips: a "
         "dotted trajectory shows where it will go, following where your left controller points. Let go of the right grip: it is thrown along "
         "the trajectory. Letting go of the left grip first puts it back on the chest. Off: the swing-to-throw gesture as before.");
    if (!v) return;
    int hand = fc5::gesture::throw_v2_right() ? 1 : 0;
    ImGui::TextUnformatted("Hand that takes it");
    bool ch = ImGui::RadioButton("Left##throwv2hand", &hand, 0);
    ImGui::SameLine();
    ch |= ImGui::RadioButton("Right##throwv2hand", &hand, 1);
    if (ch) m_ui_cmds += hand ? "throwv2hand right\n" : "throwv2hand left\n";
    hint("Left (default): the left hand holds it and aims; the right grip shows the trajectory and letting it go throws. Right: the right hand "
         "takes it from the left chest; hold the left grip too (the trajectory) and let go of the right grip to throw; let go without the "
         "trajectory and it goes back on the chest. The zone is smaller with this switch (throwv2zone, 12 cm).");
    bool arm = fc5::gesture::throw_v2_rb_arm();
    if (ImGui::Checkbox("Wind up while the trajectory shows##throwv2rb", &arm)) m_ui_cmds += arm ? "throwv2rb arm\n" : "throwv2rb tap\n";
    hint("On (default): the game's throw button is held from the moment the trajectory shows, so the throw leaves sooner when you let go. Off: "
         "it is only pressed when you let go (the game's whole throw plays then).");
    bool as = m_arc_stop.load();
    if (ImGui::Checkbox("Trajectory stops where it lands##throwarcstop", &as)) m_ui_cmds += as ? "throwarcstop on\n" : "throwarcstop off\n";
    hint("On (default): the dotted trajectory ends where it meets the ground or a wall, with an orange ring there. Off: it goes on through "
         "the ground.");
    bool pa = fc5::gesture::throw_preaim();
    if (ImGui::Checkbox("Aim while the trajectory shows##throwpreaim", &pa)) m_ui_cmds += pa ? "throwpreaim on\n" : "throwpreaim off\n";
    hint("On (default): while the trajectory shows, the game already aims along it, so letting go throws at once. Off: the game turns to it "
         "after you let go (up to 0.7 s later).");
    bool ts = m_throw_split.load();
    if (ImGui::Checkbox("Keep the view's culling on the head while aiming a throw##throwsplit", &ts)) m_ui_cmds += ts ? "throwsplit on\n" : "throwsplit off\n";
    hint("On (default): the game's aim turns to the trajectory the way it turns to the barrel when you fire, and the picture the game draws stays "
         "where you look (nothing vanishes at the edges). Off: the game's whole camera turns to the throw.");
    bool rk = fc5::gesture::throw_rock();
    if (ImGui::Checkbox("A rock when there is no throwable##throwrock", &rk)) m_ui_cmds += rk ? "throwrock on\n" : "throwrock off\n";
    hint("Off by default: some throwables are only drawn once thrown, so they look like none left. On: when the game has no throwable to show "
         "(none selected or none left), the chest gesture throws a rock (the game's own "
         "rock throw, endless) and the chest shows the rock once one has been thrown.");
    bool pk = fc5::gesture::throw_peek_on();
    if (ImGui::Checkbox("Show the selected throwable on the chest from the start##throwpeek", &pk)) m_ui_cmds += pk ? "throwpeek on\n" : "throwpeek off\n";
    hint("The mod can only show a throwable it has seen the game draw. On: when nothing is known yet and after the weapon wheel closes, the throw "
         "button is pressed for a moment and the throw cancelled, so the chest shows the selected throwable (a short hide of the gun).");
}

// throwallprobe S (test): which draw, in any pass, shows the selected throwable while RB is held. For S seconds every indexed draw's
// shape is kept while RB is up (the baseline); while RB is held (from its 6th frame) each shape not in the baseline is logged once with
// its pass (the weapon camera or not), target size and pixel shader. (Only the knife is drawn in the weapon pass, simulator 25 Oct.)
void FarCry5Stereo::throw_all_probe(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, UINT inst)
{
    if (!m_th || std::chrono::steady_clock::now() >= m_th->ap_until) return;
    auto&     t     = *m_th;
    const int frame = m_frame.load();
    const bool rb   = m_rb_down.load();
    if (frame != t.ap_frame) {
        t.ap_frame     = frame;
        t.ap_rb_frames = rb ? t.ap_rb_frames + 1 : 0;
    }
    const ThrowableState::Shape s{ count, start, base };
    if (!rb) {
        t.ap_base.insert(s);
        return;
    }
    if (t.ap_rb_frames < 6 || t.ap_base.contains(s) || !t.ap_seen.insert(s).second || t.ap_seen.size() > 60) return;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    std::string name;
    {
        std::lock_guard _{ m_ps_mtx };
        if (auto it = m_ps_names.find(ps); it != m_ps_names.end()) name = it->second.substr(0, 120);
    }
    if (ps) ps->Release();
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    UINT w = 0, h = 0;
    if (rtv) {
        ID3D11Resource* r = nullptr;
        rtv->GetResource(&r);
        if (ID3D11Texture2D* tex = nullptr; r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC d{};
            tex->GetDesc(&d);
            w = d.Width;
            h = d.Height;
            tex->Release();
        }
        if (r) r->Release();
        rtv->Release();
    }
    const bool wpn = m_vs_cb0 && is_weapon_cb(m_vs_cb0);
    spdlog::info("[ThrowAll] frame {} (RB frame {}): new draw {} indices from {} base {} x{} | weapon pass {} | target {}x{} | ps {} {}", frame, t.ap_rb_frames, count, start, base, inst, wpn,
                 w, h, (void*)ps, name);
}

// hudtexprobe S (test): for S seconds, the textures (pixel shader slot 0..3) of every draw made while the HUD is drawn into the mod's HUD
// panel; at the end one line per texture (size, format, draws). Two runs with different throwables selected show which texture is
// the selected throwable's icon.
void FarCry5Stereo::hud_tex_probe(ID3D11DeviceContext* ctx)
{
    if (!m_th) return;
    auto&      t   = *m_th;
    const auto now = std::chrono::steady_clock::now();
    if (t.hp_until == std::chrono::steady_clock::time_point{}) return;
    if (now >= t.hp_until) {
        t.hp_until = {};
        spdlog::info("[HudTex] {} textures seen in HUD draws:", t.hp.size());
        for (const auto& [res, e] : t.hp) spdlog::info("[HudTex]   {} {}x{} format {} slot {}: {} draws", res, e.w, e.h, e.fmt, e.slot, e.n);
        return;
    }
    if (!m_hud_redirected) return;
    ID3D11ShaderResourceView* srv[4]{};
    ctx->PSGetShaderResources(0, 4, srv);
    for (int i = 0; i < 4; ++i) {
        if (!srv[i]) continue;
        ID3D11Resource* r = nullptr;
        srv[i]->GetResource(&r);
        if (r) {
            auto& e = t.hp[(void*)r];
            if (e.n++ == 0) {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    e.w = d.Width; e.h = d.Height; e.fmt = int(d.Format);
                    tex->Release();
                }
                e.slot = i;
            }
            r->Release();
        }
        srv[i]->Release();
    }
}

bool FarCry5Stereo::throwable_recorded() const { return m_th && m_th->any.load(); }

void FarCry5Stereo::throwable_hold_pose(glm::vec3& pos, glm::mat3& rot) const
{
    if (!fc5::gesture::throw_v2() || fc5::gesture::throw_hold_hand() != fc5::gesture::kWeaponHand) return;
    pos = m_mo.slung ? m_mo.hand_pos_world : m_mo.ctrl_pos_world;
    rot = m_mo.slung ? m_mo.hand_rot_world : m_mo.ctrl_rot_world;
}

bool FarCry5Stereo::throwable_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    if (!m_th) m_th = new ThrowableState{};
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "throwhand") { m_th->hand = on; persist = true; return true; }
    if (cmd == "throwchest") { m_th->chest = on; persist = true; return true; }
    if (cmd == "throwpouch") { m_th->pouch = on; persist = true; return true; }
    if (cmd == "throwpouchup") { m_th->pouch_up = std::clamp(fv, -0.3f, 0.3f); persist = true; return true; }
    if (cmd == "throwpouchback") { m_th->pouch_back = std::clamp(fv, -0.2f, 0.2f); persist = true; return true; }
    if (cmd == "throwpouchtex") {
        float b = m_th->pouch_tex[1].load();
        rest >> b;
        m_th->pouch_tex[0] = std::clamp(fv, 0.0f, 1.0f);
        m_th->pouch_tex[1] = std::clamp(b, 0.0f, 1.0f);
        m_th->pouch_rebuild = true;   // rebuilt at the next draw
        persist = true;
        return true;
    }
    if (cmd == "throwpouchv2" || cmd == "throwpouchuv") {
        if (cmd == "throwpouchv2") m_th->pouch_v2 = on;
        else m_th->pouch_uv = std::clamp(fv, 0.0f, 0.2f);
        m_th->pouch_rebuild = true;   // rebuilt at the next draw
        persist = true;
        return true;
    }
    if (cmd == "throwpouchmat") {
        if (arg != "hands" && arg != "cloth") return false;
        m_th->pouch_mat     = arg == "cloth" ? 1 : 0;
        m_th->pouch_rebuild = true;
        persist             = true;
        return true;
    }
    if (cmd == "throwdrawlog") { m_th->log = arg.empty() ? 4 : (int)fv; return true; }
    if (cmd == "hudtexprobe") {
        m_th->hp.clear();
        m_th->hp_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(std::clamp(arg.empty() ? 3.0f : fv, 0.1f, 60.0f) * 1000.0f));
        spdlog::info("[HudTex] HUD draw textures collected for {:.1f} s", arg.empty() ? 3.0f : fv);
        return true;
    }
    if (cmd == "throwallprobe") {
        m_th->ap_base.clear();
        m_th->ap_seen.clear();
        m_th->ap_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(std::clamp(arg.empty() ? 8.0f : fv, 0.0f, 60.0f) * 1000.0f));
        spdlog::info("[ThrowAll] every pass's new draws while RB is held logged for {:.1f} s", arg.empty() ? 8.0f : fv);
        return true;
    }
    if (cmd == "throwprobe") {
        m_th->probe_seen.clear();
        m_th->probe_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(std::clamp(arg.empty() ? 5.0f : fv, 0.0f, 60.0f) * 1000.0f));
        spdlog::info("[ThrowProbe] new weapon-pass draws logged for {:.1f} s", arg.empty() ? 5.0f : fv);
        return true;
    }
    if (cmd == "throwsplit") { m_throw_split = on; persist = true; return true; }
    if (cmd == "throwarcstop") { m_arc_stop = on; persist = true; return true; }
    if (cmd == "throwhandrot" || cmd == "throwhandrot2" || cmd == "throwhandpos") {
        float b = 0.0f, c = 0.0f;
        rest >> b >> c;
        auto& v = cmd == "throwhandrot" ? m_th->hand_rot : cmd == "throwhandrot2" ? m_th->hand_rot2 : m_th->hand_off;
        const float lim = cmd == "throwhandpos" ? 0.3f : 180.0f;
        v[0] = std::clamp(fv, -lim, lim);
        v[1] = std::clamp(b, -lim, lim);
        v[2] = std::clamp(c, -lim, lim);
        persist = true;
        return true;
    }
    if (cmd == "throwrot") {
        float p = m_th->rot[1].load(), r = m_th->rot[2].load();
        rest >> p >> r;
        m_th->rot[0] = std::clamp(fv, -180.0f, 180.0f);
        m_th->rot[1] = std::clamp(p, -180.0f, 180.0f);
        m_th->rot[2] = std::clamp(r, -180.0f, 180.0f);
        persist      = true;
        return true;
    }
    return false;
}
