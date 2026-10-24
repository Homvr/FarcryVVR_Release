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

    constexpr int kSrv = 16, kSmp = 16, kPsCb = 8, kVb = 4, kMaxRec = 4;

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

        void release()
        {
            auto rel = [](auto*& p) { if (p) p->Release(); p = nullptr; };
            rel(il); rel(ib); rel(vs); rel(ps); rel(vs_cb2); rel(rs); rel(dss); rel(bs);
            for (auto*& p : vb) rel(p);
            for (auto*& p : srv) rel(p);
            for (auto*& p : smp) rel(p);
            for (auto*& p : ps_cb) rel(p);
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
    std::atomic<bool>  chest{ true };   // throwchest: the recorded throwable drawn on the chest between throws
    std::atomic<float> rot[3]{ 0.0f, 90.0f, 0.0f };   // throwrot: its turn on the chest (yaw, pitch, roll, deg)
    std::atomic<int>   log{ 2 };        // throwdrawlog N: the next N throwable draws logged

    // Weapon-pass draws by frame: the last idle frame's set is "the gun"; while a throw is under way anything else is
    // the throwable.
    int           set_frame{ -1 };
    std::set<Key> cur, idle;
    bool          idle_phase{ true };

    glm::vec3 F{ 0.0f };   // the game's left hand (gloves' left-hand bones' centre), q axes (last gloves draw)
    int       F_frame{ -100 };

    std::vector<Rec> recs;
    int              rec_frame{ -1 };
    bool             grab_recorded{ false };
    uint64_t         moved{ 0 }, chest_draws{ 0 };

    ID3D11Buffer* standin[2]{};      // object transform (b1) and previous (b3) for the held throwable
    ID3D11Buffer* orig[3]{};         // VS b1..b3 as the game bound them
    ID3D11Buffer* chest_cb[kMaxRec]{};   // object transform per recorded draw, on the chest

    // The pouch (throwpouch): a small bag at the chest point, built in the gloves' vertex format from two of their own
    // vertices (texture places, defaults) and drawn with the gloves' shaders, textures and lighting, hanging on a bone
    // palette of its own.
    std::atomic<bool>  pouch{ false };   // off by default (Homard 18 Oct 21:13: "turn it off by default for the moment its just bad")
    std::atomic<float> pouch_up{ 0.03f };   // throwpouchup: the throwable's origin above the pouch's centre (m); a knife (origin at one end) then stands ~20 cm out of the top
    ID3D11Buffer*      pouch_vb{ nullptr };
    ID3D11Buffer*      pouch_ib{ nullptr };
    ID3D11Buffer*      pouch_bones{ nullptr };
    UINT               pouch_n{ 0 };
    UINT               pouch_stride{ 0 };
    ID3D11InputLayout* pouch_il{ nullptr };   // the layout it was built for (rebuilt if the gloves' changes)
    bool               pouch_failed{ false };
    uint64_t           pouch_draws{ 0 };
};

void FarCry5Stereo::throwable_note_support(const glm::mat4&, const glm::vec3& F)
{
    if (!m_th) m_th = new ThrowableState{};
    m_th->F       = F;
    m_th->F_frame = m_frame.load();
}

void FarCry5Stereo::throwable_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    if (!m_th) m_th = new ThrowableState{};
    auto&      t     = *m_th;
    const int  frame = m_frame.load();
    const auto phase = fc5::gesture::throw_phase();
    const bool held  = phase != fc5::gesture::ThrowPhase::Idle;   // also while the game's throw animation lets it go (Follow)
    const bool rb    = m_rb_down.load();   // any throw (left grip + B too) shows the game's throwable: recorded for the chest
    if (frame != t.set_frame) {
        if (t.idle_phase) t.idle = std::move(t.cur);
        t.cur.clear();
        t.set_frame  = frame;
        t.idle_phase = phase == fc5::gesture::ThrowPhase::Idle && !rb;
        if (t.idle_phase) t.grab_recorded = false;
    }
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   fmt{};
    UINT          ib_off = 0;
    ctx->IAGetIndexBuffer(&ib, &fmt, &ib_off);
    const Key key{ ib, start, count, base };
    t.cur.insert(key);
    if (!(held || rb) || t.idle.empty() || t.idle.contains(key) || !fc5::gesture::throw_on()) {
        if (ib) ib->Release();
        return;
    }
    m_arc_sig = count;   // throwarc: the throwable in hand (its measured launch, per throwable)
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
    // Record it for the chest, once per grab (the draws of the first frame it shows).
    if (fresh && (!t.grab_recorded || t.rec_frame == frame)) {
        if (!t.grab_recorded) {
            for (auto& r : t.recs) r.release();
            t.recs.clear();
            t.grab_recorded = true;
            t.rec_frame     = frame;
        }
        const bool dup = std::any_of(t.recs.begin(), t.recs.end(), [&](const Rec& r) { return r.key == key; });
        if ((int)t.recs.size() < kMaxRec && !dup) {
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
            t.recs.push_back(r);
            spdlog::info("[Throwable] recorded for the chest: {} indices from {}, {} vertex buffers, {} textures, object {:.2f} m from the game camera", count, start,
                         std::count_if(std::begin(r.vb), std::end(r.vb), [](auto* p) { return p != nullptr; }),
                         std::count_if(std::begin(r.srv), std::end(r.srv), [](auto* p) { return p != nullptr; }), glm::length(r.tq));
        }
    }
    // In the hand: the move that puts the game's left hand on the left controller (as the gloves get it, FarCry5Stereo
    // weapon_draw_impl), from this frame's placement and the last gloves draw's hand centre.
    if (held && fresh && t.hand.load() && frame - t.F_frame <= 2) {
        const glm::mat3 rT = glm::transpose(hm.rot);
        const glm::mat3 WT = glm::transpose(hm.w_rot);
        const glm::mat3 LR = m_mo.other_rot_world * hm.corr;
        const glm::mat3 G  = rT * WT * LR * hm.rot;
        const glm::vec3 c  = hm.cam + rT * (hm.grip + WT * (m_mo.other_pos_world - hm.w_pos)) - rT * WT * LR * t.F;
        glm::mat4       A{ G };
        A[3] = glm::vec4{ c - G * hm.cam, 1.0f };
        float o1[32], o3[32];
        std::memcpy(o1, obj, sizeof(o1));
        to_rows(A * from_rows(obj), o1);
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
    if (t.recs.empty() || !t.chest.load() || !fc5::gesture::throw_on() || fc5::gesture::throw_phase() != fc5::gesture::ThrowPhase::Idle) return;
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
    ID3D11Buffer*             vcb[4]{};
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
    ctx->VSGetConstantBuffers(0, 4, vcb);
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
    for (auto& r : t.recs) {
        if (!t.chest_cb[k]) break;
        // q -> world now (this frame's game camera), then the move that puts q point F0 with axes R0 at the chest with Rc.
        glm::mat4 Oq{ glm::mat3{ rT * r.rq } };
        Oq[3]              = glm::vec4{ hm.cam + rT * r.tq, 1.0f };
        const glm::mat3 LR = Rc * glm::transpose(R0);
        const glm::mat3 G  = rT * WT * LR * hm.rot;
        const glm::vec3 c  = hm.cam + rT * (hm.grip + WT * (chest_at - hm.w_pos)) - rT * WT * LR * F0;
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
        ID3D11Buffer* v[4]{ vcb[0], t.chest_cb[k], r.vs_cb2, t.chest_cb[k] };
        call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 4u, static_cast<ID3D11Buffer* const*>(v));
        ID3D11Buffer* p[kPsCb]{};
        for (int i = 0; i < kPsCb; ++i) p[i] = r.ps_cb_view[i] ? vcb[0] : r.ps_cb[i];
        ctx->PSSetConstantBuffers(0, kPsCb, p);
        ctx->PSSetShaderResources(0, kSrv, r.srv);
        call_orig<void>(m_ps_samplers_hook, 10, ctx, 0u, UINT(kSmp), static_cast<ID3D11SamplerState* const*>(r.smp));
        ctx->RSSetState(r.rs);
        ctx->OMSetDepthStencilState(r.dss, r.ds_ref);
        ctx->OMSetBlendState(r.bs, r.bf, r.mask);
        call_orig<void>(m_draw_idx_hook, 12, ctx, r.count, r.start, r.base);
        ++k;
    }
    if (t.chest_draws++ % 600 == 0)
        spdlog::info("[Throwable] on the chest: {} recorded draws at ({:.2f}, {:.2f}, {:.2f}) world", k, m_mo.chest_pos_world.x, m_mo.chest_pos_world.y, m_mo.chest_pos_world.z);

    // Put everything back.
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(topo);
    ctx->IASetVertexBuffers(0, kVb, vb, stride, off);
    ctx->IASetIndexBuffer(ib, ib_fmt, ib_off);
    ctx->VSSetShader(vs, vs_ci, vs_nci);
    call_orig<void>(m_ps_set_hook, 9, ctx, ps, static_cast<ID3D11ClassInstance* const*>(ps_ci), ps_nci);
    call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 4u, static_cast<ID3D11Buffer* const*>(vcb));
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
    // BlendIndices0 at 20, TexCoord6/7 at 24/28, Normal0 and Fog0 (tangent) R10G10B10A2 at 32/36): boxes in metres, x right,
    // y up, z back (towards the body), every vertex on bone 0. Faces are drawn both ways round (the gloves' culling is the
    // game's). body: tmpl_a's texture place; flap and press stud: tmpl_b's.
    void build_pouch(const uint8_t* tmpl_a, const uint8_t* tmpl_b, float scale, std::vector<uint8_t>& vb, std::vector<uint16_t>& ib)
    {
        struct Box { glm::vec3 c, h; const uint8_t* t; };
        const Box boxes[]{
            { { 0.0f, 0.0f, 0.0f }, { 0.055f, 0.065f, 0.025f }, tmpl_a },        // the bag
            { { 0.0f, 0.040f, -0.008f }, { 0.059f, 0.031f, 0.033f }, tmpl_b },   // the flap over its top front (1.6 cm proud)
            { { 0.0f, 0.020f, -0.044f }, { 0.009f, 0.009f, 0.004f }, tmpl_b },   // the press stud
        };
        const glm::vec3 axes[3]{ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        for (const auto& b : boxes) {
            for (int a = 0; a < 3; ++a) {
                for (float s : { -1.0f, 1.0f }) {
                    const glm::vec3 n = axes[a] * s, u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
                    const uint16_t  first = uint16_t(vb.size() / 40);
                    for (int k = 0; k < 4; ++k) {
                        const float     su = (k == 1 || k == 2) ? 1.0f : -1.0f, sv = k >= 2 ? 1.0f : -1.0f;
                        const glm::vec3 p  = b.c + b.h * (n + u * su + v * sv);
                        uint8_t         vtx[40];
                        std::memcpy(vtx, b.t, 40);
                        const int16_t pi[3]{ int16_t(std::lround(p.x / scale)), int16_t(std::lround(p.y / scale)), int16_t(std::lround(p.z / scale)) };
                        std::memcpy(vtx, pi, 6);
                        const uint8_t w1[4]{ 255, 255, 255, 255 }, zero[4]{};
                        std::memcpy(vtx + 16, zero, 4);
                        vtx[16 + 2] = w1[0];   // B8G8R8A8: x is the third byte
                        std::memcpy(vtx + 20, zero, 4);   // bone 0
                        std::memcpy(vtx + 24, zero, 4);
                        std::memcpy(vtx + 28, zero, 4);
                        uint32_t nt = 0, tt = 0;
                        std::memcpy(&nt, b.t + 32, 4);
                        std::memcpy(&tt, b.t + 36, 4);
                        nt = pack_dir(n, nt);
                        tt = pack_dir(u, tt);
                        std::memcpy(vtx + 32, &nt, 4);
                        std::memcpy(vtx + 36, &tt, 4);
                        vb.insert(vb.end(), vtx, vtx + 40);
                    }
                    for (int i : { 0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2 }) ib.push_back(uint16_t(first + i));
                }
            }
        }
    }
}

// The pouch: drawn just before the gloves with everything the gloves draw has bound (shaders, textures, camera, the
// object's constants) except the mesh and the bone palette, so it gets the gloves' material and the game's lighting.
void FarCry5Stereo::throwable_pouch(ID3D11DeviceContext* ctx)
{
    auto& t = *m_th;
    if (!t.pouch.load() || t.pouch_failed || !fc5::gesture::throw_on()) return;
    const auto& hm = m_hand_map;
    if (!m_mo.valid || !m_mo.chest_valid || m_frame.load() - hm.frame > 1 || in_vehicle() || flat_screen_on() || m_cine_on) return;
    ID3D11InputLayout* il = nullptr;
    ctx->IAGetInputLayout(&il);
    if (il) il->Release();
    if (!il) return;
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
    if (t.pouch_il != il || !t.pouch_vb) {
        // Built from the gloves' own vertex buffer and layout (once per layout).
        t.pouch_il = il;
        if (t.pouch_vb) { t.pouch_vb->Release(); t.pouch_vb = nullptr; }
        if (t.pouch_ib) { t.pouch_ib->Release(); t.pouch_ib = nullptr; }
        std::vector<LayoutElem> layout;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_layouts.find(il); it != m_layouts.end()) layout = it->second;
        }
        auto has = [&](const char* sem, UINT idx, DXGI_FORMAT f, UINT off) {
            return std::any_of(layout.begin(), layout.end(), [&](const LayoutElem& e) { return _stricmp(e.semantic.c_str(), sem) == 0 && e.index == idx && e.format == f && e.offset == off && e.slot == 0; });
        };
        ID3D11Buffer* vb0 = nullptr;
        UINT          stride = 0, voff = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb0, &stride, &voff);
        const bool fmt_ok = stride == 40 && has("POSITION", 0, DXGI_FORMAT_R16G16B16A16_SINT, 0) && has("BLENDWEIGHT", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 16) &&
                            has("BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 20) && has("NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 32);
        std::vector<uint8_t> data;
        if (!fmt_ok || !vb0 || !read_buf(ctx, vb0, data) || data.size() < size_t(voff) + 80) {
            spdlog::info("[Throwable] pouch: the gloves' vertex format is not the expected one (stride {}): no pouch", stride);
            t.pouch_failed = true;
            if (vb0) vb0->Release();
            dev->Release();
            return;
        }
        if (vb0) vb0->Release();
        // Two of the gloves' vertices as templates: one a quarter of the way into the buffer, one at three quarters.
        const size_t nv = (data.size() - voff) / 40;
        const uint8_t* ta = data.data() + voff + (nv / 4) * 40;
        const uint8_t* tb = data.data() + voff + (3 * nv / 4) * 40;
        std::vector<uint8_t>  vbd;
        std::vector<uint16_t> ibd;
        build_pouch(ta, tb, scale, vbd, ibd);
        D3D11_BUFFER_DESC     d{};
        D3D11_SUBRESOURCE_DATA s{};
        d.ByteWidth = UINT(vbd.size());
        d.Usage     = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        s.pSysMem   = vbd.data();
        dev->CreateBuffer(&d, &s, &t.pouch_vb);
        d.ByteWidth = UINT(ibd.size() * 2);
        d.BindFlags = D3D11_BIND_INDEX_BUFFER;
        s.pSysMem   = ibd.data();
        dev->CreateBuffer(&d, &s, &t.pouch_ib);
        t.pouch_n      = UINT(ibd.size());
        t.pouch_stride = 40;
        spdlog::info("[Throwable] pouch built from the gloves' vertex buffer: {} vertices, {} indices, position scale {:.3g} m", vbd.size() / 40, ibd.size(), scale);
    }
    if (!t.pouch_bones) {
        D3D11_BUFFER_DESC d{};
        d.ByteWidth = 12288;
        d.Usage     = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &t.pouch_bones);
    }
    dev->Release();
    if (!t.pouch_vb || !t.pouch_ib || !t.pouch_bones) return;

    // Placement: pouch axes (x right, y up, z back) taken as the game camera's axes, then the move that puts that point
    // with those axes at the chest point with the body's axes (as the throwable on the chest). The gloves' shader draws
    // object * bone * vertex, so bone 0 = inverse(object) * that placement.
    const glm::mat3 rT = glm::transpose(hm.rot);
    const glm::mat3 WT = glm::transpose(hm.w_rot);
    const glm::mat3 LR = m_mo.chest_rot_world;
    const glm::mat3 G  = rT * WT * LR * hm.rot;
    const glm::vec3 c  = hm.cam + rT * (hm.grip + WT * (m_mo.chest_pos_world - hm.w_pos));
    glm::mat4       A{ G };
    A[3] = glm::vec4{ c - G * hm.cam, 1.0f };
    glm::mat4 Oq{ rT };
    Oq[3]             = glm::vec4{ hm.cam, 1.0f };
    const glm::mat4 B = glm::inverse(from_rows(obj)) * A * Oq;
    std::vector<float> bones(3072);
    for (int i = 0; i < 256; ++i) to_rows(B, bones.data() + i * 12);
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.pouch_bones), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(bones.data()), 0u, 0u);

    ID3D11Buffer* vb = nullptr;
    UINT          stride = 0, off = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT   ib_fmt{};
    UINT          ib_off = 0;
    ID3D11Buffer* bcb[2]{};
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
    ctx->IAGetIndexBuffer(&ib, &ib_fmt, &ib_off);
    ctx->VSGetConstantBuffers(3, 2, bcb);
    const UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &t.pouch_vb, &t.pouch_stride, &zero);
    ctx->IASetIndexBuffer(t.pouch_ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11Buffer* pb[2]{ t.pouch_bones, t.pouch_bones };
    call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(pb));
    call_orig<void>(m_draw_idx_hook, 12, ctx, t.pouch_n, 0u, 0);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    ctx->IASetIndexBuffer(ib, ib_fmt, ib_off);
    call_orig<void>(m_vs_cb_hook, 7, ctx, 3u, 2u, static_cast<ID3D11Buffer* const*>(bcb));
    if (vb) vb->Release();
    if (ib) ib->Release();
    for (auto* b : bcb) if (b) b->Release();
    if (t.pouch_draws++ % 600 == 0)
        spdlog::info("[Throwable] pouch drawn at ({:.2f}, {:.2f}, {:.2f}) world", m_mo.chest_pos_world.x, m_mo.chest_pos_world.y, m_mo.chest_pos_world.z);
}

bool FarCry5Stereo::throwable_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    if (!m_th) m_th = new ThrowableState{};
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "throwhand") { m_th->hand = on; persist = true; return true; }
    if (cmd == "throwchest") { m_th->chest = on; persist = true; return true; }
    if (cmd == "throwpouch") { m_th->pouch = on; persist = true; return true; }
    if (cmd == "throwpouchup") { m_th->pouch_up = std::clamp(fv, -0.3f, 0.3f); persist = true; return true; }
    if (cmd == "throwdrawlog") { m_th->log = arg.empty() ? 4 : (int)fv; return true; }
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
