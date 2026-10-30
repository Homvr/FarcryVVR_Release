// sidearm (Homard 26 Oct: "the sidearm model should always be visible on the right hip (like with the sling) ... when holding no weapon in
// hand, the main weapon in sling and the sidearm should be visible in their respective places. When you grip the sidearm it is in your
// hand, but sling should stay visible. When you grip the sling, the main weapon get in your hand, and the sidearm is visible in your right hip").
//
// The game draws only the weapon in its hands. So the game keeps holding the weapon the hand last grabbed (FarCry5Gesture: the sidearm
// gesture switches with the game's last weapon) and hangs it where it is carried when the hand lets go (rest_pose: the sling or the hip);
// the other weapon is drawn by the mod from a recording of its weapon-pass draws:
//  - recording: once per hold, a weapon held still for 1 s (not firing, reloading, throwing, switching, walking fast) has every kept
//    weapon-pass draw of one frame recorded (shaders, mesh, textures, material constants, a skinned part's bone palette) with its object
//    transform in the gun's steadied axes (q' = Cr q + Ct, q = the game camera's axes: the pose gunstill holds it in) and its grip point
//    and calibration, so it can be placed like the game's own weapon on any controller-like pose;
//  - drawn just before the gloves (the weapon pass's targets and camera bound), where the throwable's chest copy is drawn: the main weapon
//    on the sling while the game holds the sidearm, the sidearm on the hip while it holds the main weapon, and the wanted one in the hand
//    while a switch is pending (the game's weapon is then still where it is carried).
// Which weapon is a sidearm: set by hand (sidearmweapons), else measured: a handgun's support hand is beside or behind the weapon hand
// (gun_still_handgun); melee weapons never.
#include "FarCry5Stereo.h"
#include "FarCry5Gesture.h"
#include "FarCry5Record.h"
#include "ModPath.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <istream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
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
    using clock = std::chrono::steady_clock;
    using Shape = std::tuple<UINT, UINT, INT, const void*>;   // count, start, base, pixel shader (a part drawn twice with two materials is two parts)

    constexpr int kSrv = 16, kSmp = 16, kPsCb = 8, kVb = 4, kVsSrv = 8, kMaxParts = 32, kMaxWeapons = 8, kPool = 3 * kMaxParts;
    constexpr UINT kPaletteBytes = 12288;   // a skinned draw's bone palette at VS b3 (previous at b4); a rigid draw has its previous transform there

    // One recorded weapon-pass draw.
    struct Part
    {
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
        ID3D11Buffer*             vs_cb2{ nullptr };        // CSceneGeometry, copied
        ID3D11Buffer*             pal[2]{};                 // a skinned part's palettes (b3, b4), copied
        ID3D11ShaderResourceView* vs_srv[kVsSrv]{};
        float                     obj[32]{};                // CSceneTransform (b1) as recorded
        ID3D11ShaderResourceView* srv[kSrv]{};
        ID3D11SamplerState*       smp[kSmp]{};
        ID3D11Buffer*             ps_cb[kPsCb]{};           // copies; nullptr where the camera buffer was bound
        bool                      ps_cb_view[kPsCb]{};      // the camera (VS b0) was bound there: the current one is used
        ID3D11RasterizerState*    rs{ nullptr };
        ID3D11DepthStencilState*  dss{ nullptr };
        UINT                      ds_ref{ 0 };
        ID3D11BlendState*         bs{ nullptr };
        float                     bf[4]{};
        UINT                      mask{ 0xffffffff };
        glm::mat3                 rq{ 1.0f };               // the part's axes in the gun's steadied axes (q')
        glm::vec3                 tq{ 0.0f };               // its origin there

        void release()
        {
            auto rel = [](auto*& p) { if (p) p->Release(); p = nullptr; };
            rel(il); rel(ib); rel(vs); rel(ps); rel(vs_cb2); rel(rs); rel(dss); rel(bs);
            for (auto*& p : vb) rel(p);
            for (auto*& p : pal) rel(p);
            for (auto*& p : vs_srv) rel(p);
            for (auto*& p : srv) rel(p);
            for (auto*& p : smp) rel(p);
            for (auto*& p : ps_cb) rel(p);
        }
    };

    struct Weapon
    {
        uint32_t          sig{ 0 };
        std::vector<Part> parts;
        glm::vec3         grip0{ 0.0f };   // its grip point in q' (put on the controller when held)
        glm::mat3         corr{ 1.0f };    // its calibration turn (weaponcal)
        glm::vec3         hand{ 0.0f };    // the game's right hand centre from the grip, q' (holding it), for the copy in an empty hand
        bool              hand_ok{ false };
        // holsterglove: the game's weapon-hand bones holding it (per glove mesh: bone, its skinning in q'), for the copy in the hand.
        std::map<const void*, std::vector<std::pair<int, glm::mat4>>> glove;
        clock::time_point glove_when{};   // when the hand was last taken (again every 10 s while it is held in the hand)
        clock::time_point when{};
        void release() { for (auto& p : parts) p.release(); parts.clear(); }
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
}

struct FarCry5Stereo::HolsterState
{
    std::atomic<bool> copies{ true };     // weaponcopies on|off (saved): the recorded copies drawn (with the sidearm gesture on)
    std::atomic<bool> autoside{ true };   // sidearmauto on|off (saved): a measured handgun is a sidearm
    std::atomic<int>  log{ 6 };           // holsterlog N: the next N copy draws logged
    std::mutex        mtx;                // force_on / force_off (commands and the F11 menu)
    std::set<uint32_t> force_on, force_off;   // sidearmweapons +sig -sig (saved)

    std::vector<Weapon> weapons;   // by sig, at most kMaxWeapons (the oldest dropped)
    uint32_t            main_sig{ 0 }, side_sig{ 0 };   // the last main weapon and sidearm held still in the game's hands
    std::vector<std::pair<uint32_t, clock::time_point>> held_at;   // each weapon's last hold (main_sig / side_sig: the latest of each kind)
    std::atomic<uint32_t> slot_sig{ 0 };   // the weapon the sidearm slot gave this session (not saved: learned at each load, sidearmpeek) (slot modes: the only sidearm)
    bool                  side_by_hip{ false };   // the game holds the sidearm because the hip (or the look after a load) picked it, not the player's wheel
    bool                  cur_side_was{ false };
    clock::time_point     throw_quiet_until{};   // a throwable out until then (and 1.5 s after): the weapon id is not a weapon's
    std::atomic<bool>     throw_out{ false };     // a throwable out this frame: the game draws no weapon, the copies stand in (holster_show)
    clock::time_point     wheel_t{};   // the weapon wheel last seen open
    uint32_t              switch_count_seen{ 0 };   // fc5::gesture::weapon_switch_count() last seen, and when it changed
    clock::time_point     switch_count_t{};

    // The current hold: the frame's weapon id, since when; recorded once per hold.
    uint32_t          hold_sig{ 0 };
    clock::time_point hold_since{};
    bool              hold_recorded{ false };
    int               next_try{ 0 };   // frame: a failed recording waits a little

    // The frame being drawn: its weapon draws' shapes (and the last frame's), the recording under way.
    bool              rec_next{ false };   // this frame's draws are recorded
    int               rec_frame{ -1 };
    std::set<Shape>   cur, last;
    std::vector<Part> pend;
    bool              pend_full{ false };
    glm::vec3         pend_grip0{ 0.0f };
    glm::mat3         pend_corr{ 1.0f };
    glm::vec3         pend_hand{ 0.0f };
    bool              pend_hand_ok{ false };
    std::map<const void*, std::vector<std::pair<int, glm::mat4>>> pend_glove;   // the recorded frame's weapon hand (holster_glove_note)
    uint32_t          glove_said{ 0 };   // the last weapon whose recorded hand was drawn (log)
    // The hand alone, taken on the frame rec_frame while the weapon is held in the hand (one hand, held still, gunstill holding it): a recording
    // is made once per hold and mostly on the sling or hip since 29 Oct, so the hand was missing (Homard 29 Oct 11:39: the rifle beside an open hand).
    bool              glove_next{ false };
    uint32_t          glove_sig{ 0 };
    // The hands kept across sessions (bin\fc5vr_holsterhands.txt), per weapon and glove mesh (its count, start, base: the buffer pointer is the
    // session's), so the first grab after a load has one too (Homard 29 Oct 15:01: "I still had hand in the wrong position when picking up from
    // sling": his first sling grab, 15:59:20, came before the rifle had been held in one hand that session).
    using MeshKey = std::tuple<UINT, UINT, INT>;
    std::map<std::pair<uint32_t, MeshKey>, std::vector<std::pair<int, glm::mat4>>> saved_hands;
    std::map<const void*, MeshKey> pend_glove_key;   // the recorded frame's meshes
    bool              hands_loaded{ false }, hands_dirty{ false };
    clock::time_point hands_saved_t{};
    int               fails{ 0 };
    uint32_t          learn_sig{ 0 };   // a hip grab's switch landed (on this id): the weapon next held still within 5 s is the sidearm
    clock::time_point learn_t{};

    ID3D11Buffer* pool[kPool]{};   // object transforms of the copies drawn in a frame

    // holstermodel: the holster on the hip, molded on the sidearm's recorded mesh, drawn with its material (HolsterModel).
    std::atomic<bool> model_on{ true };
    std::atomic<float> model_gap{ 0.004f }, model_wall{ 0.003f }, model_mouth{ -0.025f };   // holstergap, holsterwall (m), holstermouth (m along the barrel from the grip)
    uint32_t          model_sig{ 0 };   // built for this sidearm (0: not built)
    int               model_part{ -1 }; // the part whose material and vertex format it uses
    bool              model_failed{ false };
    ID3D11Buffer*     model_vb{ nullptr };
    ID3D11Buffer*     model_ib{ nullptr };
    ID3D11Buffer*     model_obj{ nullptr };   // its object transform (128 bytes)
    ID3D11Buffer*     model_pal{ nullptr };   // an identity bone palette, for a skinned material
    UINT              model_n{ 0 }, model_stride{ 0 };
    bool              model_said{ false };
    uint64_t      shown{ 0 };
    int           shown_frame{ -1 };
    std::string   last_state;      // for the log: what was shown where

    Weapon* find(uint32_t sig)
    {
        for (auto& w : weapons) if (w.sig == sig && !w.parts.empty()) return &w;
        return nullptr;
    }
};

// shoulderdraw's phase 1 (FarCry5WeaponSwitch.cpp), or a sling or hip grab's switch pending (holster_hides_game): the game's weapon-pass
// draws are hidden, so both weapons are drawn as copies.
bool FarCry5Stereo::holster_switch_hiding() const { return weapon_switch_hiding() || holster_hides_game(); }

// While a sidearm switch is pending the game puts one weapon away and draws the other where the outgoing one is carried (simulator 26 Oct:
// the incoming rifle's draw on the sling beside its copy in the hand): hidden, with the copies on.
bool FarCry5Stereo::holster_hides_game() const
{
    return m_hol && m_hol->copies.load() && fc5::gesture::switch_want() >= 0 && m_motion.load() && !in_vehicle() && !flat_screen_on() && !m_cine_on;
}

// Weapons are recorded for the sidearm's copies, and for shoulderinstant's (FarCry5WeaponSwitch.cpp) with the sidearm off.
bool FarCry5Stereo::holster_records() const { return fc5::gesture::side_on() || (m_sh_draw.load() && m_sh_instant.load()); }

bool FarCry5Stereo::holster_has_copy(uint32_t sig) { return m_hol && sig != 0 && m_hol->find(sig) != nullptr; }

bool FarCry5Stereo::holster_is_side(uint32_t sig)
{
    if (sig == 0) return false;
    if (!m_hol) m_hol = new HolsterState{};
    // The sidearm slot (sidearmpick wheel or a key; Homard 27 Oct: "the sidearm (rightmost slot on the weapon wheel, slot 4) ... should never go
    // into the sling"): exactly one weapon, the one that slot gave this session (learned after each load, not saved: a wrong one saved in Homard's 27 Oct session blocked the right one). Nothing else ever hangs on the hip.
    if (fc5::gesture::side_key() != 0) return sig == m_hol->slot_sig.load();
    {
        std::lock_guard _{ m_hol->mtx };
        if (m_hol->force_on.contains(sig)) return true;
        if (m_hol->force_off.contains(sig)) return false;
    }
    if (!m_hol->autoside.load() || melee_weapon(sig)) return false;
    return gun_still_handgun(sig) == 1;
}

// The copy drawn in the hand this frame: a sling or hip grab's switch pending (the game's weapon still carried), else shoulderinstant's
// guess (FarCry5WeaponSwitch.cpp). Its weapon, the pose it is held at, and whether the right hand is otherwise empty (slung).
bool FarCry5Stereo::holster_hand_pose(uint32_t& sig, glm::vec3& pos, glm::mat3& rot, bool& empty_hand)
{
    if (!m_hol) return false;
    auto&          t      = *m_hol;
    const uint32_t sh_sig = weapon_switch_guess();
    const bool     side   = fc5::gesture::side_on();
    if (!t.copies.load() || (!side && sh_sig == 0) || t.weapons.empty()) return false;
    if (!m_mo.valid || (side && !m_mo.carry_valid) || m_frame.load() - m_hand_map.frame > 1 || in_vehicle() || flat_screen_on() || m_cine_on) return false;
    const int      want     = fc5::gesture::switch_want();
    const uint32_t hand_sig = want == 1 ? t.side_sig : want == 0 ? t.main_sig : 0;
    if (hand_sig != 0) {
        if (!m_mo.slung || !t.find(hand_sig) || !fc5::gesture::switch_grabbed()) return false;   // the look after a load: nothing in the hand
        sig        = hand_sig;
        pos        = m_mo.hand_pos_world;
        rot        = m_mo.hand_rot_world;
        empty_hand = true;
        return true;
    }
    // The shoulder's copy where the game's weapon would be drawn: the controller (on the sling: the weapon hand's own controller).
    if (sh_sig == 0 || !t.find(sh_sig) || !m_mo.ctrl_valid) return false;
    sig        = sh_sig;
    pos        = m_mo.slung ? m_mo.hand_pos_world : m_mo.ctrl_pos_world;
    rot        = m_mo.slung ? m_mo.hand_rot_world : m_mo.ctrl_rot_world;
    empty_hand = m_mo.slung;
    return true;
}

// holsterglove (Homard 28 Oct: "hands is still wrong for a few seconds when taking a weapon from sling then holster or holster then sling"; his
// headset picture: the pistol beside an open right hand). While a copy is in the hand the game's right hand holds nothing (the switch: its
// weapon carried, or hidden), so the glove was an empty hand on the controller beside the copy. The hand as the game held that weapon is
// recorded with it (the gloves draw's weapon-hand bones of the recorded frame, in the same steadied axes) and drawn with the copy instead.
void FarCry5Stereo::holster_glove_note(const void* mesh, UINT count, UINT start, INT base, const glm::mat4& Om, const std::vector<float>& bones,
                                       const std::vector<glm::vec4>& cent, int wside)
{
    if (!m_hol || !mesh) return;
    auto& t = *m_hol;
    if (t.rec_frame == m_frame.load() && (t.rec_next || t.glove_next)) t.pend_glove_key[mesh] = HolsterState::MeshKey{ count, start, base };
    if (!(t.rec_next || t.glove_next) || t.rec_frame != m_frame.load() || m_unarmed || holster_switch_hiding() || t.pend_glove.contains(mesh)) return;
    const auto& hm = m_hand_map;
    // Game world -> q' (q = rot (p - cam), q' = Cr q + Ct), as the weapon's parts.
    glm::mat4 Q{ hm.cr * hm.rot };
    Q[3]               = glm::vec4{ hm.ct - hm.cr * (hm.rot * hm.cam), 1.0f };
    const glm::mat4 QO = Q * Om;
    auto&           v  = t.pend_glove[mesh];
    // The bones the weapon hand's move takes (weapon_draw_impl's move_weapon_hand): that side's, and bone 0 (the wrist and cuff).
    for (int bi = 0; bi < 256 && bi < (int)cent.size() && (bi + 1) * 12 <= (int)bones.size(); ++bi) {
        const glm::vec4& gc = cent[bi];
        if (gc.x <= 0.0f || (bi != 0 && (gc.y > 0.0f ? 0 : 1) != wside)) continue;
        v.emplace_back(bi, QO * from_rows(bones.data() + bi * 12));
    }
}

// holsterglove: this frame's recorded hands kept for later sessions (fc5vr_holsterhands.txt).
void FarCry5Stereo::holster_keep_hands(uint32_t sig)
{
    auto& t = *m_hol;
    for (const auto& [mesh, v] : t.pend_glove)
        if (const auto k = t.pend_glove_key.find(mesh); k != t.pend_glove_key.end() && !v.empty()) {
            t.saved_hands[{ sig, k->second }] = v;
            t.hands_dirty                      = true;
        }
}

bool FarCry5Stereo::holster_has_saved_hand(uint32_t sig) const
{
    if (!m_hol) return false;
    const auto it = m_hol->saved_hands.lower_bound({ sig, HolsterState::MeshKey{ 0, 0, INT_MIN } });
    return it != m_hol->saved_hands.end() && it->first.first == sig;
}

bool FarCry5Stereo::holster_glove(const void* mesh, UINT count, UINT start, INT base, const glm::mat4& Om, std::vector<float>& bones)
{
    if (!m_hol || !mesh || !m_hol_glove.load()) return false;
    uint32_t  sig = 0;
    glm::vec3 pos{ 0.0f };
    glm::mat3 rot{ 1.0f };
    bool      empty = false;
    if (!holster_hand_pose(sig, pos, rot, empty)) return false;
    const Weapon* w = m_hol->find(sig);
    if (!w) return false;
    const std::vector<std::pair<int, glm::mat4>>* hand = nullptr;
    if (const auto it = w->glove.find(mesh); it != w->glove.end() && !it->second.empty()) hand = &it->second;
    else if (const auto s = m_hol->saved_hands.find({ sig, HolsterState::MeshKey{ count, start, base } }); s != m_hol->saved_hands.end() && !s->second.empty()) hand = &s->second;   // an earlier session's
    if (!hand) return false;
    // q' -> game world as the copy's parts are placed (holster_show), then into this draw's object space.
    const auto&     hm = m_hand_map;
    const glm::mat3 rT = glm::transpose(hm.rot);
    const glm::mat3 WT = glm::transpose(hm.w_rot);
    const glm::mat3 PR = rot * w->corr;
    glm::mat4       A{ rT * WT * PR };
    A[3]              = glm::vec4{ hm.cam + rT * (hm.grip + WT * (pos - hm.w_pos - PR * w->grip0)), 1.0f };
    const glm::mat4 X = glm::inverse(Om) * A;
    for (const auto& [bi, B] : *hand)
        if ((bi + 1) * 12 <= (int)bones.size()) to_rows(X * B, bones.data() + bi * 12);
    if (m_hol->glove_said != sig) {
        m_hol->glove_said = sig;
        spdlog::info("[Holster] holsterglove: the right hand drawn holding weapon {}'s copy as it held that weapon", sig);
    }
    return true;
}

void FarCry5Stereo::holster_draw(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base, UINT instances)
{
    if (!m_hol) m_hol = new HolsterState{};
    auto&     t     = *m_hol;
    const int frame = m_frame.load();
    if (!holster_records()) return;
    const Shape shape{ count, start, base, static_cast<const void*>(m_cur_ps) };
    // The same part drawn twice in a frame (both eyes' passes in one frame, a second pass): one part, recorded once (his 27 Oct save: 5694 and 24417
    // were never recorded, "3 of 2 draws").
    const bool  again = t.cur.contains(shape);
    t.cur.insert(shape);
    if (again) return;
    const auto& hm = m_hand_map;
    if (!t.rec_next || t.rec_frame != frame || frame - hm.frame > 1 || instances > 1) return;   // (same-frame stereo: the right eye's camera carries its own parity)
    if ((int)t.pend.size() >= kMaxParts) {
        t.pend_full = true;
        return;
    }
    ID3D11Buffer* cb[5]{};
    ctx->VSGetConstantBuffers(0, 5, cb);
    float obj[32]{};
    bool  have = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (auto it = cb[1] ? m_obj_consts.find(cb[1]) : m_obj_consts.end(); it != m_obj_consts.end() && it->second.size == 128) {
            std::memcpy(obj, it->second.f, sizeof(obj));
            have = true;
        }
    }
    if (!have) {
        // A draw without a rigid object transform cannot be placed: the frame is not recorded.
        t.pend_full = true;
        for (auto* b : cb) if (b) b->Release();
        return;
    }
    Part p{};
    p.count = count;
    p.start = start;
    p.base  = base;
    ctx->IAGetInputLayout(&p.il);
    ctx->IAGetPrimitiveTopology(&p.topo);
    ctx->IAGetVertexBuffers(0, kVb, p.vb, p.stride, p.off);
    ctx->IAGetIndexBuffer(&p.ib, &p.ib_fmt, &p.ib_off);
    ctx->VSGetShader(&p.vs, nullptr, nullptr);
    ctx->PSGetShader(&p.ps, nullptr, nullptr);
    ctx->VSGetShaderResources(0, kVsSrv, p.vs_srv);
    ctx->PSGetShaderResources(0, kSrv, p.srv);
    ctx->PSGetSamplers(0, kSmp, p.smp);
    ctx->RSGetState(&p.rs);
    ctx->OMGetDepthStencilState(&p.dss, &p.ds_ref);
    ctx->OMGetBlendState(&p.bs, p.bf, &p.mask);
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
    p.vs_cb2 = copy_cb(cb[2]);
    if (cb[3]) {
        D3D11_BUFFER_DESC d{};
        cb[3]->GetDesc(&d);
        if (d.ByteWidth == kPaletteBytes) {
            p.pal[0] = copy_cb(cb[3]);
            p.pal[1] = cb[4] ? copy_cb(cb[4]) : addref(p.pal[0]);
        }
    }
    ID3D11Buffer* pcb[kPsCb]{};
    ctx->PSGetConstantBuffers(0, kPsCb, pcb);
    for (int i = 0; i < kPsCb; ++i) {
        p.ps_cb_view[i] = pcb[i] && pcb[i] == cb[0];
        if (pcb[i] && !p.ps_cb_view[i]) p.ps_cb[i] = copy_cb(pcb[i]);
        if (pcb[i]) pcb[i]->Release();
    }
    if (dev) dev->Release();
    for (auto* b : cb) if (b) b->Release();
    std::memcpy(p.obj, obj, sizeof(obj));
    // Into the gun's steadied axes: q = rot (p - cam), q' = Cr q + Ct.
    const glm::mat4 O = from_rows(obj);
    p.rq              = hm.cr * hm.rot * glm::mat3{ O };
    p.tq              = hm.cr * (hm.rot * (glm::vec3{ O[3] } - hm.cam)) + hm.ct;
    if (t.pend.empty()) {
        t.pend_grip0 = hm.grip0;
        t.pend_corr  = hm.corr;
        t.pend_hand_ok = !m_mo.slung && !m_unarmed && frame - m_wpn_hand_frame <= 3;
        t.pend_hand    = hm.cr * m_wpn_hand_cam + hm.ct - hm.grip0;
    }
    t.pend.push_back(p);
}

void FarCry5Stereo::holster_frame_end()
{
    if (!m_hol) m_hol = new HolsterState{};
    auto&      t   = *m_hol;
    const int  frame = m_frame.load();
    const auto now = clock::now();
    const uint32_t sig = m_weapon_sig_frame;   // this frame's weapon id (0: none drawn)
    // While a throwable is out (RB held, a throw under way, its peek) the game's weapon id is the throwable's (his 21:10 log: 11040, 2502, 16110 taken
    // for the main weapon, the sling emptied): the kinds and the weapons held are not updated then.
    const bool throwing = m_rb_down.load() || fc5::gesture::throw_phase() != fc5::gesture::ThrowPhase::Idle || fc5::gesture::throw_peeking() || fc5::gesture::throw_rb_held();
    if (throwing) t.throw_quiet_until = now + std::chrono::milliseconds(1500);
    t.throw_out = throwing;
    const bool throw_quiet = now < t.throw_quiet_until;
    if (!throw_quiet) m_hol_cur_side = holster_is_side(m_weapon_sig.load());
    m_hol_hiding       = weapon_switch_hiding();   // shoulderdraw only (the gesture waits for its new weapon's pose; not our own pending hide)
    m_hol_known        = m_hol->find(m_weapon_sig.load()) != nullptr;   // a weapon recorded (held still) before: the switch lands on it at once
    m_hol_side_rec     = t.slot_sig.load() != 0 && t.find(t.slot_sig.load()) != nullptr;   // sidearmpeek: nothing to look at
    // A saved sidearm measured since as no handgun (Homard's 27 Oct session saved his rifle 57819): forgotten, the slot is looked at again.
    if (const uint32_t ss = t.slot_sig.load(); ss != 0 && (gun_still_handgun(ss) == 0 || melee_weapon(ss))) {
        t.slot_sig = 0;
        fc5::gesture::side_peek_again();
        {
            std::lock_guard _{ t.mtx };
            if (t.force_on.erase(ss) > 0) {   // nor a hint any more
                std::string line = "sidearmweapons";
                for (uint32_t s : t.force_on) line += fmt::format(" +{}", s);
                for (uint32_t s : t.force_off) line += fmt::format(" -{}", s);
                if (t.force_on.empty() && t.force_off.empty()) line += " none";
                m_ui_cmds += line + "\n";
            }
        }
        spdlog::info("[Holster] weapon {} was taken for the sidearm but is {}: forgotten, the sidearm slot is looked at again", ss, melee_weapon(ss) ? "a melee weapon" : "measured as no handgun");
    }
    m_hol_side_known   = t.slot_sig.load() != 0;
    m_hol_side_sig     = t.slot_sig.load();
    {
        std::lock_guard _{ t.mtx };
        m_hol_side_unlisted = !t.force_on.empty() && !t.force_on.contains(m_weapon_sig.load());
    }
    // The sidearm taken by the player's own wheel (no pick of ours pending): the sling shows no weapon then (Homard 27 Oct).
    // A shoulder switch (a gesture of ours, the last 2.5 s) bringing the sidearm keeps the main weapon on the sling (edge run 27 Oct 23:17: it emptied it).
    if (const uint32_t n = fc5::gesture::weapon_switch_count(); n != t.switch_count_seen) {
        t.switch_count_seen = n;
        t.switch_count_t    = now;
    }
    const bool by_gesture = fc5::gesture::switch_want() == 1 || now - t.switch_count_t < std::chrono::milliseconds(2500);
    // Only the player's own wheel pick empties the sling (Homard 27 Oct): a switch the game makes by itself (his 29 Oct 12:20-12:24 log: rifle -> pistol
    // three times with no gesture and no wheel) keeps the main weapon on it.
    if (m_wheel_open.load()) t.wheel_t = now;
    const bool by_wheel = t.wheel_t != clock::time_point{} && now - t.wheel_t < std::chrono::seconds(3);
    if (m_hol_cur_side.load() && !t.cur_side_was) t.side_by_hip = by_gesture || !by_wheel;
    t.cur_side_was = m_hol_cur_side.load();
    if (!holster_records()) {
        t.rec_next = false;
        for (auto& p : t.pend) p.release();
        t.pend.clear();
        t.cur.clear();
        t.last.clear();
        t.pend_glove.clear();
        return;
    }
    if (sig != t.hold_sig) {
        t.hold_sig      = sig;
        t.hold_since    = now;
        t.hold_recorded = false;
        t.fails         = 0;
    }
    // What a hip grab switched to is the sidearm (a one-handed gun shows no support hand to measure), unless measured as not a handgun.
    // Learned once the weapon is held still in the hand (an id the game shows for a moment during its switch is not a weapon).
    if (const uint32_t l = fc5::gesture::take_side_landed(); l != 0) {
        if (fc5::gesture::side_key() != 0) {
            // The slot's pick landed: what the game shows now is that slot's weapon, the sidearm (the 0.6 s an unknown id must last keeps the
            // switch's passing ids out), at once so it never hangs on the sling.
            // Not a weapon measured as no handgun, nor a melee weapon (the slot holds a handgun): such a result is a pick gone wrong.
            if (t.slot_sig.load() != l && (gun_still_handgun(l) == 0 || melee_weapon(l))) {
                spdlog::info("[Holster] weapon {} came from the sidearm pick but is {}: not taken for the sidearm (still {})", l, melee_weapon(l) ? "a melee weapon" : "measured as no handgun",
                             t.slot_sig.load());
            } else if (const uint32_t ss = t.slot_sig.load(); ss != 0 && ss != l && [&] { std::lock_guard _{ t.mtx }; return t.force_on.contains(ss) && !t.force_on.contains(l); }()) {
                // A pick gone wrong (read as an LB tap, the last weapon): a listed sidearm is not replaced by an unlisted weapon.
                spdlog::info("[Holster] weapon {} came from the sidearm pick but is not in the saved sidearm list: the sidearm stays {}", l, ss);
            } else if (t.slot_sig.load() != l) {
                t.slot_sig = l;
                spdlog::info("[Holster] weapon {}: the sidearm (what the sidearm slot gave)", l);
            }
            t.side_by_hip = true;
        } else {
            t.learn_sig = l;
            t.learn_t   = now;
        }
    }
    const bool held_still = sig != 0 && !m_unarmed && now - t.hold_since > std::chrono::seconds(1);
    // The slot modes: the saved sidearm list (sidearmweapons, his handguns of earlier sessions) as a hint. One of them held still in the game's
    // hands with no sidearm known yet is the sidearm, without a look at the slot (Homard 28 Oct 22:10: loaded with his pistol 24417 out, the
    // look picked the slot it was already on and the game brought his rifle, taken for the sidearm). A sidearm learned this session and measured
    // as a handgun is added to the list (saved).
    if (fc5::gesture::side_key() != 0 && !throw_quiet) {
        std::string line;
        {
            std::lock_guard _{ t.mtx };
            if (t.slot_sig.load() == 0 && held_still && t.force_on.contains(sig) && !melee_weapon(sig) && gun_still_handgun(sig) != 0) {
                t.slot_sig = sig;
                spdlog::info("[Holster] weapon {}: the sidearm (in the saved sidearm list, held with no sidearm known yet)", sig);
            }
            // A sidearm outside the list (not measured as a handgun) while the game holds a listed one: the listed one (his 29 Oct 11:12 session:
            // the rifle taken at the load, then his pistol 24417 held as the "main weapon").
            else if (const uint32_t ss = t.slot_sig.load(); ss != 0 && ss != sig && held_still && t.force_on.contains(sig) && !t.force_on.contains(ss) &&
                     gun_still_handgun(ss) != 1 && !melee_weapon(sig) && gun_still_handgun(sig) != 0) {
                t.slot_sig = sig;
                spdlog::info("[Holster] weapon {}: the sidearm (in the saved sidearm list), not {} (not in it)", sig, ss);
            }
            if (const uint32_t ss = t.slot_sig.load(); ss != 0 && gun_still_handgun(ss) == 1 && !t.force_on.contains(ss)) {
                t.force_on.insert(ss);
                t.force_off.erase(ss);
                line = "sidearmweapons";
                for (uint32_t s : t.force_on) line += fmt::format(" +{}", s);
                for (uint32_t s : t.force_off) line += fmt::format(" -{}", s);
                spdlog::info("[Holster] weapon {}: the sidearm, measured as a handgun: added to the saved sidearm list", ss);
            }
        }
        if (!line.empty()) m_ui_cmds += line + "\n";
    }
    if (t.learn_sig != 0 && now - t.learn_t > std::chrono::seconds(5)) t.learn_sig = 0;
    if (const uint32_t got = (held_still && t.learn_sig != 0) ? sig : 0u; got != 0) {
        t.learn_sig       = 0;
        bool        set   = false;
        const bool  maybe = (fc5::gesture::side_key() != 0 || gun_still_handgun(got) != 0) && !melee_weapon(got);   // the slot key: whatever its slot holds
        std::string line  = "sidearmweapons";
        {
            std::lock_guard _{ t.mtx };
            if (maybe && !t.force_on.contains(got) && !t.force_off.contains(got)) {
                t.force_on.insert(got);
                set = true;
            }
            for (uint32_t s : t.force_on) line += fmt::format(" +{}", s);
            for (uint32_t s : t.force_off) line += fmt::format(" -{}", s);
        }
        if (set) {
            m_ui_cmds += line + "\n";   // saved, as the F11 buttons do
            spdlog::info("[Holster] weapon {} taken from the hip: the sidearm from now on (F11 Gestures > Sidearm: \"Not a sidearm\" undoes it)", got);
        }
    }
    // The right hand on the weapon in hand (for its copy in an empty hand during a switch), kept up to date while it is held still.
    if (held_still && !throw_quiet && !m_mo.slung && !m_unarmed && frame - m_wpn_hand_frame <= 3 && frame - m_hand_map.frame <= 1) {
        if (Weapon* w = t.find(sig)) {
            const glm::vec3 h = m_hand_map.cr * m_wpn_hand_cam + m_hand_map.ct - m_hand_map.grip0;
            if (glm::length(h) < 0.25f) {
                w->hand    = w->hand_ok ? glm::mix(w->hand, h, 0.1f) : h;
                w->hand_ok = true;
            }
        }
    }
    if (held_still && !throw_quiet) {
        auto it = std::find_if(t.held_at.begin(), t.held_at.end(), [&](const auto& h) { return h.first == sig; });
        if (it == t.held_at.end()) t.held_at.emplace_back(sig, now);
        else it->second = now;
    }
    // The latest held of each kind (a weapon set as a sidearm since leaves the main weapon to the one held before it).
    {
        uint32_t          m = 0, s = 0;
        clock::time_point tm{}, ts{};
        for (const auto& [w, at] : t.held_at) {
            const bool side = holster_is_side(w);
            // A main weapon is one gunstill holds (its held pose known) or a melee weapon: not an item the game shows for a moment (Homard 29 Oct
            // 12:21: 4110, after pad X, became the main weapon and the sling showed it instead of his rifle).
            glm::mat4 hp{ 1.0f };
            if (!side && !melee_weapon(w) && !gun_still_held(w, hp)) continue;
            if (side && (s == 0 || at > ts)) { s = w; ts = at; }
            if (!side && (m == 0 || at > tm)) { m = w; tm = at; }
        }
        if (m != t.main_sig || s != t.side_sig) spdlog::info("[Holster] main weapon {}, sidearm {}", m, s);
        t.main_sig = m;
        m_hol_main = m;   // loadslot (FarCry5Gesture.cpp)
        t.side_sig = s;
    }

    // This frame's recording: kept if it holds the weapon's own mesh and every draw of a frame like the last one.
    if (t.rec_frame == frame && t.rec_next) {
        const bool has_mesh = std::any_of(t.pend.begin(), t.pend.end(), [&](const Part& p) { return p.count == sig_mesh(sig); });
        const bool ok       = sig != 0 && sig == t.hold_sig && has_mesh && !t.pend_full && t.cur == t.last && t.pend.size() == t.cur.size();
        if (ok) {
            Weapon* w = nullptr;
            for (auto& x : t.weapons) if (x.sig == sig) w = &x;
            if (!w) {
                if ((int)t.weapons.size() >= kMaxWeapons) {
                    // The oldest, unless it is the one shown for the other place.
                    auto old = std::min_element(t.weapons.begin(), t.weapons.end(), [&](const Weapon& a, const Weapon& b) {
                        const bool ka = a.sig == t.main_sig || a.sig == t.side_sig, kb = b.sig == t.main_sig || b.sig == t.side_sig;
                        return ka != kb ? kb : a.when < b.when;
                    });
                    old->release();
                    t.weapons.erase(old);
                }
                t.weapons.push_back(Weapon{});
                w = &t.weapons.back();
            }
            w->release();
            w->sig   = sig;
            w->parts = std::move(t.pend);
            w->grip0 = t.pend_grip0;
            w->corr  = t.pend_corr;
            if (t.pend_hand_ok && glm::length(t.pend_hand) < 0.25f) { w->hand = t.pend_hand; w->hand_ok = true; }   // (a re-record while slung keeps the last one)
            w->when  = now;
            t.hold_recorded = true;
            // The gun put at gunstill's held pose (the parts kept on it): a recording made while the game's own animation still moved the gun
            // (a raise after a switch, a sway) is drawn as the gun is held, not as it was caught (Homard 27 Oct: "the wrong position").
            glm::mat4 held{ 1.0f }, Dn{ 1.0f };
            float     fix_cm = -1.0f, fix_deg = 0.0f;
            if (gun_still_held(sig, held)) {
                const auto mesh = std::find_if(w->parts.begin(), w->parts.end(), [&](const Part& p) { return p.count == sig_mesh(sig); });
                if (mesh != w->parts.end()) {
                    glm::mat4 M{ mesh->rq };
                    M[3]            = glm::vec4{ mesh->tq, 1.0f };
                    const glm::mat4 D = held * glm::inverse(M);
                    Dn                = D;
                    fix_cm            = 100.0f * glm::length(glm::vec3{ D[3] } + glm::mat3{ D } * mesh->tq - mesh->tq);
                    fix_deg           = glm::degrees(std::acos(std::clamp((glm::mat3{ D }[0][0] + glm::mat3{ D }[1][1] + glm::mat3{ D }[2][2] - 1.0f) * 0.5f, -1.0f, 1.0f)));
                    for (auto& p : w->parts) {
                        glm::mat4 X{ p.rq };
                        X[3] = glm::vec4{ p.tq, 1.0f };
                        X    = D * X;
                        p.rq = glm::mat3{ X };
                        p.tq = glm::vec3{ X[3] };
                    }
                }
            }
            // holsterglove: the hand on it moved with it (a re-record without the hand, two-handed or slung, keeps the last one).
            if (!t.pend_glove.empty()) {
                for (auto& [mesh, v] : t.pend_glove) for (auto& [bi, B] : v) B = Dn * B;
                holster_keep_hands(sig);
                w->glove      = std::move(t.pend_glove);
                w->glove_when = now;
            }
            int skinned = 0;
            for (const auto& p : w->parts) skinned += p.pal[0] ? 1 : 0;
            spdlog::info("[Holster] weapon {} recorded ({}): {} draws ({} skinned), grip ({:.2f}, {:.2f}, {:.2f}), {}", sig, holster_is_side(sig) ? "a sidearm" : "a main weapon",
                         w->parts.size(), skinned, w->grip0.x, w->grip0.y, w->grip0.z,
                         fix_cm < 0.0f ? std::string{ "as caught (no held pose known)" } : fmt::format("put at its held pose ({:.1f} cm, {:.1f} deg from where it was caught)", fix_cm, fix_deg));
        } else {
            if (++t.fails % 10 == 1)
                spdlog::info("[Holster] weapon {}: a recording dropped (its mesh {}, {} of {} draws{}, same draws as the last frame {}); trying again", sig, has_mesh ? "seen" : "missing",
                             t.pend.size(), t.cur.size(), t.pend_full ? ", some not recordable" : "", t.cur == t.last);
            t.next_try = frame + 15;
        }
        for (auto& p : t.pend) p.release();
        t.pend.clear();
    }
    t.pend_full = false;
    // holsterglove: the hand alone (no recording this frame), in the steadied axes where gunstill holds the weapon at its held pose.
    if (t.glove_next && t.rec_frame == frame && !t.pend_glove.empty() && sig == t.glove_sig) {
        if (Weapon* w = t.find(sig)) {
            if (w->glove.empty()) spdlog::info("[Holster] weapon {}: the hand holding it recorded (holsterglove)", sig);
            holster_keep_hands(sig);
            w->glove      = std::move(t.pend_glove);
            w->glove_when = now;
        }
    }
    t.glove_next = false;
    t.pend_glove.clear();
    t.pend_glove_key.clear();
    // The kept hands: read once, written at most every 10 s when changed.
    if (!t.hands_loaded) {
        t.hands_loaded = true;
        std::ifstream in{ fc5::mod_file("fc5vr_holsterhands.txt") };
        std::string   line;
        int           n = 0;
        while (std::getline(in, line)) {
            std::istringstream ls{ line };
            uint32_t           sig = 0;
            UINT               c = 0, st = 0;
            INT                b = 0;
            int                bi = -1;
            glm::mat4          m{ 1.0f };
            if (!(ls >> sig >> c >> st >> b >> bi)) continue;
            bool ok = true;
            for (int col = 0; col < 4 && ok; ++col) for (int r = 0; r < 4 && ok; ++r) ok = static_cast<bool>(ls >> m[col][r]);
            if (!ok || bi < 0 || bi >= 256) continue;
            t.saved_hands[{ sig, HolsterState::MeshKey{ c, st, b } }].emplace_back(bi, m);
            ++n;
        }
        if (n) spdlog::info("[Holster] holsterglove: {} hands ({} bones) read from fc5vr_holsterhands.txt", t.saved_hands.size(), n);
    }
    if (t.hands_dirty && now - t.hands_saved_t > std::chrono::seconds(10)) {
        t.hands_dirty   = false;
        t.hands_saved_t = now;
        std::ofstream out{ fc5::mod_file("fc5vr_holsterhands.txt"), std::ios::trunc };
        for (const auto& [k, v] : t.saved_hands)
            for (const auto& [bi, m] : v) {
                out << k.first << ' ' << std::get<0>(k.second) << ' ' << std::get<1>(k.second) << ' ' << std::get<2>(k.second) << ' ' << bi;
                for (int col = 0; col < 4; ++col) for (int r = 0; r < 4; ++r) out << ' ' << m[col][r];
                out << '\n';
            }
    }
    t.last      = std::move(t.cur);
    t.cur.clear();

    // The next frame is recorded when the weapon has been held still for a second and nothing moves it but the hand.
    bool  sprint = false;
    float since  = 1e6f;
    const float speed = gun_still_speed(sprint, since);
    t.rec_next = t.copies.load() && held_still && !t.hold_recorded && frame >= t.next_try && m_motion.load() && m_mo.valid && m_mo.ctrl_valid && !in_vehicle() &&
                 !flat_screen_on() && !m_cine_on && fc5::gesture::throw_phase() == fc5::gesture::ThrowPhase::Idle && !m_rb_down.load() && !fc5::gesture::throw_peeking() &&
                 !gun_still_busy(false) && fc5::gesture::switch_want() < 0 && !holster_switch_hiding() && !sprint &&
                 ((speed < 0.3f && since > 0.5f) || m_mo.slung);   // carried (sling, hip), walking too: put at the held pose anyway (29 Oct: the look after a load while walking)
    {
        Weapon*   gw = t.find(sig);
        glm::mat4 hp{ 1.0f };
        t.glove_next = !t.rec_next && gw && t.copies.load() && held_still && !m_unarmed && !throw_quiet && !gun_still_busy(false) &&
                       fc5::gesture::switch_want() < 0 && !holster_switch_hiding() && !sprint && speed < 1.0f && m_mo.valid && m_mo.ctrl_valid &&
                       !in_vehicle() && gun_still_held(sig, hp) &&
                       (gw->glove.empty() || now - gw->glove_when > std::chrono::seconds(10));
        t.glove_sig  = sig;
    }
    t.rec_frame = frame + 1;
}

namespace
{
    // Copies of FarCry5Throwable.cpp's helpers (the pouch is built the same way).
    bool read_gpu(ID3D11DeviceContext* ctx, ID3D11Buffer* b, std::vector<uint8_t>& out)
    {
        if (!b) return false;
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
    uint32_t pack_unit(const glm::vec3& v, uint32_t tmpl)
    {
        auto q = [](float f) { return uint32_t(std::clamp(std::lround((f * 0.5f + 0.5f) * 1023.0f), 0L, 1023L)); };
        return q(v.x) | (q(v.y) << 10) | (q(v.z) << 20) | (tmpl & 0xC0000000u);
    }
    glm::vec3 unpack_unit(uint32_t d) { return glm::vec3{ float(d & 1023u), float((d >> 10) & 1023u), float((d >> 20) & 1023u) } / 1023.0f * 2.0f - 1.0f; }

    // Where the holster writes in the sidearm's vertex (offsets; -1: not in its format).
    struct ModelFmt { int stride = 0, nrm = -1, tan = -1, bw = -1, bi = -1; };

    // The holster in the pose frame (x right, y up, z back, the origin the grip point; the barrel along -z): a shell molded on the gun.
    // env: per 5 mm slice from the muzzle (z0, slice i at z0 + 5 mm i), the gun's x/y extent there (x0, x1, y0, y1).
    // Ahead of the mouth the shell follows the gun gap metres off it, wall thick; a rounded cap past the muzzle; the mouth flares
    // 2 mm; a paddle (the belt side, -x) from 3 cm into the holster to 5 cm above the mouth.
    void build_holster_mesh(const std::vector<glm::vec4>& env, float z0, float zm, float gap, float wall, const uint8_t* tm, float scale, const ModelFmt& f,
                            std::vector<uint8_t>& vb, std::vector<uint16_t>& ib)
    {
        constexpr float pi = 3.14159265f, hp = 0.5f * pi, dz = 0.005f;
        auto put = [&](const glm::vec3& p, const glm::vec3& n, const glm::vec3& u) {
            uint8_t vtx[128];
            std::memcpy(vtx, tm, f.stride);
            const int16_t q[3]{ int16_t(std::clamp(std::lround(p.x / scale), -32768L, 32767L)), int16_t(std::clamp(std::lround(p.y / scale), -32768L, 32767L)),
                                int16_t(std::clamp(std::lround(p.z / scale), -32768L, 32767L)) };
            std::memcpy(vtx, q, 6);
            if (f.bw >= 0) { std::memset(vtx + f.bw, 0, 4); vtx[f.bw + 2] = 255; }   // B8G8R8A8: x is the third byte (all on bone 0)
            if (f.bi >= 0) std::memset(vtx + f.bi, 0, 4);
            for (int k = 0; k < 2; ++k) {
                const int o = k ? f.tan : f.nrm;
                if (o < 0) continue;
                uint32_t d = 0;
                std::memcpy(&d, tm + o, 4);
                d = pack_unit(k ? u : n, d);
                std::memcpy(vtx + o, &d, 4);
            }
            vb.insert(vb.end(), vtx, vtx + f.stride);
        };
        // A surface P(s, t) at ss x ts, both ways round; normals turned to face ref(s, t).
        auto grid = [&](const std::vector<float>& ss, const std::vector<float>& ts, auto&& P, auto&& ref) {
            const float e  = 1e-4f;
            const uint16_t v0 = uint16_t(vb.size() / f.stride);
            for (float t : ts)
                for (float s : ss) {
                    const glm::vec3 ps = (P(s + e, t) - P(s - e, t)) / (2 * e), pt = (P(s, t + e) - P(s, t - e)) / (2 * e);
                    glm::vec3       n  = glm::cross(ps, pt);
                    const glm::vec3 r  = ref(s, t);
                    if (glm::length(n) < 1e-10f) n = r;
                    n = glm::normalize(n);
                    if (glm::dot(n, r) < 0.0f) n = -n;
                    glm::vec3 u = ps - n * glm::dot(n, ps);
                    if (glm::length(u) < 1e-8f) u = glm::cross(n, std::fabs(n.y) < 0.9f ? glm::vec3{ 0, 1, 0 } : glm::vec3{ 1, 0, 0 });
                    put(P(s, t), n, glm::normalize(u));
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
        // The gun's extent at z (between slices), grown by off; a rounded rectangle round it.
        const int nsl = int(env.size());
        auto ext  = [&](float z, float off) {
            const float fi = std::clamp((z - z0) / dz, 0.0f, float(nsl - 1));
            const int   i  = std::min(int(fi), nsl - 2);
            const glm::vec4 b = glm::mix(env[i], env[i + 1], fi - float(i));
            return glm::vec4{ b.x - off, b.y + off, b.z - off, b.w + off };
        };
        // u in [0, 1) round the section (counter-clockwise seen from the back), the point and its outward normal (x, y).
        auto ring = [&](const glm::vec4& b, float u, glm::vec2& n) {
            const float hx = 0.5f * (b.y - b.x), hy = 0.5f * (b.w - b.z), r = std::max(0.002f, std::min({ 0.010f, hx * 0.9f, hy * 0.9f }));
            const glm::vec2 c{ 0.5f * (b.x + b.y), 0.5f * (b.z + b.w) };
            const float sx = hx - r, sy = hy - r, q = hp * r, L = 4 * sx + 4 * sy + 4 * q;
            float       s  = std::fmod(u, 1.0f) * L;
            if (s < 0.0f) s += L;
            glm::vec2 p;
            if (s < sy) { n = { 1, 0 }; p = { hx, s }; }
            else if ((s -= sy) < q) { const float a = s / r; n = { std::cos(a), std::sin(a) }; p = glm::vec2{ sx, sy } + r * n; }
            else if ((s -= q) < 2 * sx) { n = { 0, 1 }; p = { sx - s, hy }; }
            else if ((s -= 2 * sx) < q) { const float a = hp + s / r; n = { std::cos(a), std::sin(a) }; p = glm::vec2{ -sx, sy } + r * n; }
            else if ((s -= q) < 2 * sy) { n = { -1, 0 }; p = { -hx, sy - s }; }
            else if ((s -= 2 * sy) < q) { const float a = pi + s / r; n = { std::cos(a), std::sin(a) }; p = glm::vec2{ -sx, -sy } + r * n; }
            else if ((s -= q) < 2 * sx) { n = { 0, -1 }; p = { -sx + s, -hy }; }
            else if ((s -= 2 * sx) < q) { const float a = 1.5f * pi + s / r; n = { std::cos(a), std::sin(a) }; p = glm::vec2{ sx, -sy } + r * n; }
            else { s -= q; n = { 1, 0 }; p = { hx, -sy + s }; }
            return c + p;
        };
        const auto us = steps(0.0f, 1.0f, 64);
        const float zt = z0 - gap;   // the shell's tip, past the muzzle
        // The shell: outer surface from the tip to the mouth (flared 2 mm over its last 6 mm), then the rim over the wall to the inside.
        auto shell = [&](float u, float z) {
            const float flare = std::clamp((z - (zm - 0.006f)) / 0.006f, 0.0f, 1.0f);
            glm::vec2   n;
            const glm::vec2 p = ring(ext(z, gap + wall + 0.002f * flare * flare), u, n);
            return glm::vec3{ p.x, p.y, z };
        };
        auto axis = [&](float z) { const glm::vec4 b = ext(z, 0.0f); return glm::vec3{ 0.5f * (b.x + b.y), 0.5f * (b.z + b.w), z }; };
        std::vector<float> zs = steps(zt, zm, std::max(4, int(std::lround((zm - zt) / dz))));
        grid(us, zs, shell, [&](float u, float z) { return shell(u, z) - axis(z); });
        // The rim at the mouth: from the outside over the wall's thickness to the inside.
        auto rim = [&](float u, float k) {
            glm::vec2       n;
            const glm::vec2 p = ring(ext(zm, gap + 0.002f + wall * (1.0f - k)), u, n);
            return glm::vec3{ p.x, p.y, zm + 0.0015f * std::sin(pi * k) };
        };
        grid(us, steps(0.0f, 1.0f, 3), rim, [&](float, float) { return glm::vec3{ 0, 0, 1 }; });
        // The inside, a little way down (the gun fills the rest).
        auto inner = [&](float u, float z) {
            glm::vec2       n;
            const glm::vec2 p = ring(ext(z, gap + 0.002f * std::clamp((z - (zm - 0.006f)) / 0.006f, 0.0f, 1.0f)), u, n);
            return glm::vec3{ p.x, p.y, z };
        };
        grid(us, steps(zm - 0.03f, zm, 6), inner, [&](float u, float z) { return axis(z) - inner(u, z); });
        // The rounded cap past the muzzle: the tip's section closing over 1 cm.
        {
            const glm::vec4 bt = ext(z0, gap + wall);
            const glm::vec3 ct = axis(z0);
            auto cap = [&](float u, float a) {
                glm::vec2       n;
                const glm::vec2 p = ring(bt, u, n);
                const float     k = std::cos(a);
                return glm::vec3{ ct.x + (p.x - ct.x) * k, ct.y + (p.y - ct.y) * k, zt - 0.010f * std::sin(a) };
            };
            grid(us, steps(0.0f, hp, 6), cap, [&](float u, float a) { return cap(u, a) - glm::vec3{ ct.x, ct.y, zt + 0.02f }; });
        }
        // The paddle on the belt side (-x): a rounded plate 5 cm high, from 3 cm into the holster to 5 cm above the mouth, 4 mm thick.
        {
            const glm::vec4 bm = ext(zm - 0.03f, gap + wall);
            const float     x  = bm.x - 0.001f, th = 0.004f, yc = 0.5f * (bm.z + bm.w), hh = 0.025f, za = zm - 0.03f, zb = zm + 0.05f, rr = 0.010f;
            auto outline = [&](float u, glm::vec2& n) {   // the plate's outline in (z, y)
                return ring(glm::vec4{ za, zb, yc - hh, yc + hh }, u, n);
            };
            for (float side : { 0.0f, 1.0f }) {
                auto face = [&, side](float u, float k) {
                    glm::vec2       n;
                    const glm::vec2 p = outline(u, n);
                    const glm::vec2 c{ 0.5f * (za + zb), yc };
                    const glm::vec2 q = c + (p - c) * k;
                    return glm::vec3{ x - th * side, q.y, q.x };
                };
                grid(us, steps(0.0f, 1.0f, 3), face, [side](float, float) { return glm::vec3{ side > 0.5f ? -1.0f : 1.0f, 0, 0 }; });
            }
            auto edge = [&](float u, float k) {
                glm::vec2       n;
                const glm::vec2 p = outline(u, n);
                return glm::vec3{ x - th * k, p.y, p.x };
            };
            grid(us, steps(0.0f, 1.0f, 1), edge, [&](float u, float) { glm::vec2 n; outline(u, n); return glm::vec3{ 0.0f, n.y, n.x }; });
            (void)rr;
        }
    }
}

// holstermodel (Homard 30 Oct: "Can you make a good looking holster model ?"): a holster on the right hip, molded on the sidearm. The
// sidearm's recorded draws give its shape (each part's vertices, in the pose frame the copy is placed in: x right, y up, the barrel -z,
// the origin its grip point); the shell follows it 4 mm off from the muzzle to just ahead of the hand, 3 mm thick, with a rounded tip,
// a flared mouth and a paddle on the belt side. Drawn with the sidearm's own material (shaders, textures, the texel of its slide's top):
// a dark polymer holster the game lights. It stays on the hip whether the gun is in it (the copy, or the game's own on the hip) or in the hand.
bool FarCry5Stereo::holster_model_build(ID3D11DeviceContext* ctx, uint32_t sig)
{
    auto& t = *m_hol;
    const Weapon* w = t.find(sig);
    if (!w) return false;
    std::vector<glm::vec3> pts;   // the gun's points in the pose frame
    // The material: the part with the most indices (the gun's body; not a sight's glass, drawn see-through: the first build took the
    // optic on his pistol 24417 and the holster did not show), at its highest point facing up.
    int main_part = -1;
    for (int pi = 0; pi < (int)w->parts.size(); ++pi)
        if (main_part < 0 || w->parts[pi].count > w->parts[main_part].count) main_part = pi;
    int                    tmpl_part = -1;
    size_t                 tmpl_vtx  = 0;
    float                  tmpl_y    = -1e9f;
    std::vector<uint8_t>   tmpl_vb;
    ModelFmt               tf;
    float                  tmpl_scale = 0.0f;
    for (int pi = 0; pi < (int)w->parts.size(); ++pi) {
        const Part& r = w->parts[pi];
        std::vector<LayoutElem> layout;
        {
            std::lock_guard _{ m_ps_mtx };
            if (auto it = m_layouts.find(r.il); it != m_layouts.end()) layout = it->second;
        }
        auto off = [&](const char* sem, UINT idx, DXGI_FORMAT fm) {
            for (const auto& e : layout) if (_stricmp(e.semantic.c_str(), sem) == 0 && e.index == idx && (fm == DXGI_FORMAT_UNKNOWN || e.format == fm) && e.slot == 0) return int(e.offset);
            return -1;
        };
        const bool pos_ok = off("POSITION", 0, DXGI_FORMAT_R16G16B16A16_SINT) == 0;
        if (!pos_ok || r.topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST || !r.vb[0] || !r.ib || r.stride[0] < 8 || r.stride[0] > 128) {
            std::string els;
            for (const auto& e : layout) els += fmt::format(" {}{}@{}:{}", e.semantic, e.index, e.offset, int(e.format));
            spdlog::info("[Holster] holstermodel: weapon {} part {} ({} indices) not readable (stride {}:{})", sig, pi, r.count, r.stride[0], els);
            continue;
        }
        std::vector<uint8_t> vbd, ibd, cb2, pal;
        if (!read_gpu(ctx, r.vb[0], vbd) || !read_gpu(ctx, r.ib, ibd) || !read_gpu(ctx, r.vs_cb2, cb2) || cb2.size() < 24) continue;
        float scale = 0.0f;
        std::memcpy(&scale, cb2.data() + 20, 4);
        if (!(scale > 1e-7f && scale < 1e-2f)) continue;
        if (r.pal[0]) read_gpu(ctx, r.pal[0], pal);
        const int bi_off = off("BLENDINDICES", 0, DXGI_FORMAT_UNKNOWN), n_off = off("NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM);
        const bool i32 = r.ib_fmt == DXGI_FORMAT_R32_UINT;
        glm::mat4  O{ r.rq };
        O[3] = glm::vec4{ r.tq - w->grip0, 1.0f };
        for (UINT k = 0; k < r.count; ++k) {
            const size_t io = size_t(r.ib_off) + size_t(r.start + k) * (i32 ? 4 : 2);
            if (io + (i32 ? 4 : 2) > ibd.size()) break;
            uint32_t idx = 0;
            std::memcpy(&idx, ibd.data() + io, i32 ? 4 : 2);
            const size_t vo = size_t(r.off[0]) + (size_t(int64_t(idx) + r.base)) * r.stride[0];
            if (vo + r.stride[0] > vbd.size()) continue;
            const uint8_t* v = vbd.data() + vo;
            int16_t q[3];
            std::memcpy(q, v, 6);
            glm::vec4 p{ float(q[0]) * scale, float(q[1]) * scale, float(q[2]) * scale, 1.0f };
            if (!pal.empty() && bi_off >= 0) {
                const int b = v[bi_off];
                if ((b + 1) * 48 <= (int)pal.size()) p = glm::vec4{ glm::vec3{ from_rows(reinterpret_cast<const float*>(pal.data() + b * 48)) * p }, 1.0f };
            }
            const glm::vec3 g = glm::vec3{ O * p };
            pts.push_back(g);
            // The material's texel: the top of the slide (the highest point whose normal faces up).
            if (n_off >= 0) {
                uint32_t nd = 0;
                std::memcpy(&nd, v + n_off, 4);
                const glm::vec3 n = glm::normalize(r.rq * unpack_unit(nd));
                if (pi == main_part && n.y > 0.8f && g.y > tmpl_y) { tmpl_y = g.y; tmpl_part = pi; tmpl_vtx = vo; }
            }
        }
        if (tmpl_part == pi) {
            tmpl_vb    = vbd;
            tmpl_scale = scale;
            tf.stride  = int(r.stride[0]);
            tf.nrm     = n_off;
            tf.tan     = off("FOG", 0, DXGI_FORMAT_R10G10B10A2_UNORM);
            tf.bw      = off("BLENDWEIGHT", 0, DXGI_FORMAT_B8G8R8A8_UNORM);
            tf.bi      = off("BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT);
        }
    }
    if (pts.size() < 100 || tmpl_part < 0) {
        spdlog::info("[Holster] holstermodel: weapon {}: {} points, {} for its material: no holster", sig, pts.size(), tmpl_part < 0 ? "no part" : "a part");
        return false;
    }
    // The gun's extent per 5 mm slice from its muzzle to the mouth, each slice the widest over 1.5 cm round it (no ridges), the trigger
    // guard's depth kept back to the mouth.
    float zmin = 1e9f;
    for (const auto& p : pts) zmin = std::min(zmin, p.z);
    const float zm = t.model_mouth.load();
    if (zmin > zm - 0.03f) {
        spdlog::info("[Holster] holstermodel: weapon {}: its muzzle {:.0f} mm from the grip, too short for a holster", sig, -zmin * 1000.0f);
        return false;
    }
    const int               nsl = int(std::ceil((zm - zmin) / 0.005f)) + 1;
    std::vector<glm::vec4>  raw(nsl, glm::vec4{ 1e9f, -1e9f, 1e9f, -1e9f });
    for (const auto& p : pts) {
        const int i = int(std::floor((p.z - zmin) / 0.005f + 0.5f));
        if (i < 0 || i >= nsl) continue;
        raw[i] = glm::vec4{ std::min(raw[i].x, p.x), std::max(raw[i].y, p.x), std::min(raw[i].z, p.y), std::max(raw[i].w, p.y) };
    }
    std::vector<glm::vec4> env(nsl);
    for (int i = 0; i < nsl; ++i) {
        glm::vec4 b{ 1e9f, -1e9f, 1e9f, -1e9f };
        for (int j = std::max(0, i - 3); j <= std::min(nsl - 1, i + 3); ++j)
            if (raw[j].y > raw[j].x) b = glm::vec4{ std::min(b.x, raw[j].x), std::max(b.y, raw[j].y), std::min(b.z, raw[j].z), std::max(b.w, raw[j].w) };
        env[i] = b;
    }
    for (int i = 1; i < nsl; ++i) if (!(env[i].y > env[i].x)) env[i] = env[i - 1];
    for (int i = nsl - 2; i >= 0; --i) if (!(env[i].y > env[i].x)) env[i] = env[i + 1];
    if (!(env[0].y > env[0].x)) return false;
    // Not down over the grip and magazine near the mouth (the first build was 104 mm tall there): no lower than the trigger guard, the
    // lowest point 2.5-6 cm ahead of the mouth.
    float guard = 1e9f;
    for (int i = 0; i < nsl; ++i) {
        const float z = zmin + 0.005f * float(i);
        if (z >= zm - 0.06f && z <= zm - 0.025f) guard = std::min(guard, env[i].z);
    }
    if (guard < 1e8f)
        for (auto& b : env) b.z = std::max(b.z, guard);
    // Nor up over a sight on the slide (his 24417's red dot made it bulky, 30 Oct simulator): no higher than the slide's top at the
    // muzzle end (its first 3 cm), so a sight stands out of it as from an optics-cut holster.
    float slide = -1e9f;
    for (int i = 0; i < nsl && zmin + 0.005f * float(i) < zmin + 0.03f; ++i) slide = std::max(slide, env[i].w);
    if (slide > -1e8f)
        for (auto& b : env) b.w = std::min(b.w, std::max(slide, b.z + 0.01f));
    std::vector<uint8_t>  vbd;
    std::vector<uint16_t> ibd;
    build_holster_mesh(env, zmin, zm, t.model_gap.load(), t.model_wall.load(), tmpl_vb.data() + tmpl_vtx, tmpl_scale, tf, vbd, ibd);
    if (vbd.empty() || vbd.size() / tf.stride > 65000) return false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    auto rel = [](ID3D11Buffer*& b) { if (b) b->Release(); b = nullptr; };
    rel(t.model_vb);
    rel(t.model_ib);
    D3D11_BUFFER_DESC      d{};
    D3D11_SUBRESOURCE_DATA s{};
    d.ByteWidth = UINT(vbd.size());
    d.Usage     = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    s.pSysMem   = vbd.data();
    dev->CreateBuffer(&d, &s, &t.model_vb);
    d.ByteWidth = UINT(ibd.size() * 2);
    d.BindFlags = D3D11_BIND_INDEX_BUFFER;
    s.pSysMem   = ibd.data();
    dev->CreateBuffer(&d, &s, &t.model_ib);
    if (!t.model_obj) {
        D3D11_BUFFER_DESC c{};
        c.ByteWidth = 128;
        c.Usage     = D3D11_USAGE_DEFAULT;
        c.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&c, nullptr, &t.model_obj);
    }
    if (!t.model_pal) {
        std::vector<float> id(kPaletteBytes / 4, 0.0f);
        for (int b = 0; b < int(kPaletteBytes / 48); ++b) { id[b * 12 + 0] = 1.0f; id[b * 12 + 5] = 1.0f; id[b * 12 + 10] = 1.0f; }
        D3D11_BUFFER_DESC c{};
        c.ByteWidth = kPaletteBytes;
        c.Usage     = D3D11_USAGE_IMMUTABLE;
        c.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cs{};
        cs.pSysMem = id.data();
        dev->CreateBuffer(&c, &cs, &t.model_pal);
    }
    dev->Release();
    if (!t.model_vb || !t.model_ib || !t.model_obj || !t.model_pal) return false;
    t.model_n      = UINT(ibd.size());
    t.model_stride = UINT(tf.stride);
    t.model_part   = tmpl_part;
    const glm::vec4& m = env[nsl - 1];
    spdlog::info("[Holster] holstermodel: weapon {}: holster built from {} points ({} vertices, {} indices): muzzle {:.0f} mm ahead of the grip, mouth {:.0f} mm, "
                 "at the mouth {:.0f} x {:.0f} mm; material of part {} (the slide's top, {:.0f} mm above the grip)",
                 sig, pts.size(), vbd.size() / tf.stride, ibd.size(), -zmin * 1000.0f, -zm * 1000.0f, (m.y - m.x) * 1000.0f, (m.w - m.z) * 1000.0f, tmpl_part, tmpl_y * 1000.0f);
    return true;
}

// The holster drawn on the hip pose with the sidearm's material (the bindings are put back by holster_show).
void FarCry5Stereo::holster_model_draw(ID3D11DeviceContext* ctx, ID3D11Buffer* const* vcb)
{
    auto& t = *m_hol;
    if (!t.model_on.load() || !fc5::gesture::side_on() || !m_mo.carry_valid) return;
    const uint32_t sig = t.slot_sig.load() ? t.slot_sig.load() : t.side_sig;
    const Weapon*  w   = sig ? t.find(sig) : nullptr;
    if (!w) return;
    if (t.model_sig != sig) {
        if (t.model_failed && t.model_sig == sig) return;
        t.model_sig    = sig;
        t.model_failed = !holster_model_build(ctx, sig);
    }
    if (t.model_failed || t.model_part < 0 || t.model_part >= (int)w->parts.size() || !t.model_vb) return;
    const Part&     r  = w->parts[t.model_part];
    const auto&     hm = m_hand_map;
    const glm::mat3 rT = glm::transpose(hm.rot);
    const glm::mat3 WT = glm::transpose(hm.w_rot);
    const glm::mat3 PR = m_mo.hip_rot_world * w->corr;
    // The pose frame's point v at hip + PR v: an object at the grip with the pose's axes (as a copy's part with rq = I, tq = grip0).
    glm::mat4 Ow{ rT * WT * PR };
    Ow[3] = glm::vec4{ hm.cam + rT * (hm.grip + WT * (m_mo.hip_pos_world - hm.w_pos)), 1.0f };
    float o[32];
    std::memcpy(o, r.obj, sizeof(o));
    to_rows(Ow, o);
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.model_obj), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o), 0u, 0u);
    ID3D11Buffer* const vcb0 = vcb[0], * const vcb4 = vcb[4];   // the gloves draw's camera and previous-transform buffers
    const UINT zero = 0;
    ctx->IASetInputLayout(r.il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &t.model_vb, &t.model_stride, &zero);
    ctx->IASetIndexBuffer(t.model_ib, DXGI_FORMAT_R16_UINT, 0);
    ctx->VSSetShader(r.vs, nullptr, 0);
    call_orig<void>(m_ps_set_hook, 9, ctx, r.ps, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
    ID3D11Buffer* v[5]{ vcb0, t.model_obj, r.vs_cb2, r.pal[0] ? t.model_pal : t.model_obj, r.pal[0] ? t.model_pal : vcb4 };
    call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 5u, static_cast<ID3D11Buffer* const*>(v));
    ctx->VSSetShaderResources(0, kVsSrv, r.vs_srv);
    ID3D11Buffer* p[kPsCb]{};
    for (int j = 0; j < kPsCb; ++j) p[j] = r.ps_cb_view[j] ? vcb0 : r.ps_cb[j];
    ctx->PSSetConstantBuffers(0, kPsCb, p);
    ctx->PSSetShaderResources(0, kSrv, r.srv);
    call_orig<void>(m_ps_samplers_hook, 10, ctx, 0u, UINT(kSmp), static_cast<ID3D11SamplerState* const*>(r.smp));
    ctx->RSSetState(r.rs);
    ctx->OMSetDepthStencilState(r.dss, r.ds_ref);
    ctx->OMSetBlendState(r.bs, r.bf, r.mask);
    call_orig<void>(m_draw_idx_hook, 12, ctx, t.model_n, 0u, 0);
    fp_eye_again(ctx, t.model_n, 0u, 0);   // afwcopies: the held eye's weapon pass too (AFW)
    if (!t.model_said) {
        t.model_said = true;
        spdlog::info("[Holster] holstermodel: drawn on the hip ({:.2f}, {:.2f}, {:.2f})", m_mo.hip_pos_world.x, m_mo.hip_pos_world.y, m_mo.hip_pos_world.z);
    }
}

// At the gloves draw, before it: the recorded weapons where they are carried (and the wanted one in the hand during a switch). Every
// binding changed is put back.
void FarCry5Stereo::holster_show(ID3D11DeviceContext* ctx)
{
    if (!m_hol) m_hol = new HolsterState{};
    auto& t = *m_hol;
    // shoulderinstant (FarCry5WeaponSwitch.cpp): a shoulder switch's guessed weapon in the hand from the grip, with or without the sidearm.
    const uint32_t sh_sig = weapon_switch_guess();
    const bool     side   = fc5::gesture::side_on();
    if (!t.copies.load() || (!side && sh_sig == 0) || t.weapons.empty()) return;
    const auto& hm = m_hand_map;
    if (!m_mo.valid || (side && !m_mo.carry_valid) || m_frame.load() - hm.frame > 1 || in_vehicle() || flat_screen_on() || m_cine_on) return;

    // Which copies: the game's weapon is drawn where it is carried or in the hand; the other one in its own place.
    const int  want     = fc5::gesture::switch_want();
    const bool hiding   = holster_switch_hiding();
    const bool cur_side = m_hol_cur_side.load();
    const uint32_t hand_sig = want == 1 ? t.side_sig : want == 0 ? t.main_sig : 0;
    // empty_hand: held by the empty right hand (slung); hand: the copy in the hand (holster_hand_pose).
    struct Show { const Weapon* w; glm::vec3 pos; glm::mat3 rot; const char* where; bool empty_hand{ false }; bool hand{ false }; };
    Show show[3]{};
    int  ns = 0;
    uint32_t in_hand = fc5::gesture::switch_grabbed() ? hand_sig : 0;   // the look after a load: both copies where they are carried
    {
        uint32_t  hs = 0;
        glm::vec3 hp{ 0.0f };
        glm::mat3 hr{ 1.0f };
        bool      he = false;
        if (holster_hand_pose(hs, hp, hr, he)) {
            show[ns++] = { t.find(hs), hp, hr, hand_sig != 0 ? "in the hand" : "in the hand (shoulder)", he, true };
            if (hand_sig == 0) in_hand = hs;
        }
    }
    // A throwable out (Homard 29 Oct: "when picking up a throwable, the sling weapon disappear"): the game puts its weapon away, so both
    // copies stand where they are carried.
    const bool thr = t.throw_out.load();
    if (side) {
        if (const Weapon* w = t.main_sig ? t.find(t.main_sig) : nullptr; w && t.main_sig != in_hand && ((cur_side && t.side_by_hip) || hiding || thr))   // the sidearm from the player's wheel: an empty sling
            show[ns++] = { w, m_mo.sling_pos_world, m_mo.sling_rot_world, "on the sling" };
        if (const Weapon* w = t.side_sig ? t.find(t.side_sig) : nullptr; w && t.side_sig != in_hand && (!cur_side || hiding || thr))
            show[ns++] = { w, m_mo.hip_pos_world, m_mo.hip_rot_world, "on the hip" };
    }
    {
        std::string state;
        for (int i = 0; i < ns; ++i) state += fmt::format("{}{} {}", i ? ", " : "", show[i].w->sig, show[i].where);
        if (state != t.last_state) {
            spdlog::info("[Holster] copies: {} (the game holds weapon {}, {}{})", state.empty() ? std::string{ "none" } : state, m_weapon_sig.load(), cur_side ? "a sidearm" : "a main weapon",
                         want >= 0 ? fmt::format(", switching to the {}", want ? "sidearm" : "main weapon") : std::string{});
            t.last_state = state;
        }
    }
    const bool model = t.model_on.load() && side && m_mo.carry_valid && (t.slot_sig.load() || t.side_sig);   // holstermodel: the holster on the hip
    if (ns == 0 && !model) return;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    for (auto*& s : t.pool) {
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
    ID3D11ShaderResourceView* vsrv[kVsSrv]{};
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
    ctx->VSGetShaderResources(0, kVsSrv, vsrv);
    ctx->PSGetShaderResources(0, kSrv, srv);
    ctx->PSGetSamplers(0, kSmp, smp);
    ctx->RSGetState(&rs);
    ctx->OMGetDepthStencilState(&dss, &ds_ref);
    ctx->OMGetBlendState(&bs, bf, &mask);

    // A copy on a controller-like pose P (x right, y up, -z the barrel, origin the grip), placed as the game's own weapon is on the
    // controller: world' = P_pos + P_rot corr (q' - grip0). This pass draws game-world point p at world' = w_pos + w_rot (rot (p - cam) - grip),
    // so the part's object transform is p = cam + rot^T (grip + w_rot^T (world' - w_pos)).
    const glm::mat3 rT = glm::transpose(hm.rot);
    const glm::mat3 WT = glm::transpose(hm.w_rot);
    int             k  = 0;
    for (int i = 0; i < ns; ++i) {
        const Weapon&   w  = *show[i].w;
        const glm::mat3 PR = show[i].rot * w.corr;
        // In the empty right hand (its centre on the controller): the grip put where that hand holds this weapon (Homard 28 Oct: "the hands kept
        // having a wrong position" between the hip and the sling; simulator: the copy sat beside the empty glove).
        // holsterglove: with its recorded hand drawn on it, the copy is placed as the weapon is held (its grip on the controller).
        const bool      gl = show[i].hand && m_hol_glove.load() && (!w.glove.empty() || holster_has_saved_hand(w.sig));
        const glm::vec3 g0 = w.grip0 + ((show[i].empty_hand && !gl && w.hand_ok && m_hol_copy_hand.load()) ? w.hand : glm::vec3{ 0.0f });
        for (const auto& r : w.parts) {
            if (k >= kPool || !t.pool[k]) break;
            glm::mat4 Ow{ rT * WT * PR * r.rq };
            Ow[3] = glm::vec4{ hm.cam + rT * (hm.grip + WT * (show[i].pos - hm.w_pos + PR * (r.tq - g0))), 1.0f };
            float o[32];
            std::memcpy(o, r.obj, sizeof(o));
            to_rows(Ow, o);
            call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(t.pool[k]), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(o), 0u, 0u);

            ctx->IASetInputLayout(r.il);
            ctx->IASetPrimitiveTopology(r.topo);
            ctx->IASetVertexBuffers(0, kVb, r.vb, r.stride, r.off);
            ctx->IASetIndexBuffer(r.ib, r.ib_fmt, r.ib_off);
            ctx->VSSetShader(r.vs, nullptr, 0);
            call_orig<void>(m_ps_set_hook, 9, ctx, r.ps, static_cast<ID3D11ClassInstance* const*>(nullptr), 0u);
            // No motion of its own (previous = current transform, a skinned part's previous palette = its palette as recorded).
            ID3D11Buffer* v[5]{ vcb[0], t.pool[k], r.vs_cb2, r.pal[0] ? r.pal[0] : t.pool[k], r.pal[0] ? r.pal[1] : vcb[4] };
            call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 5u, static_cast<ID3D11Buffer* const*>(v));
            ctx->VSSetShaderResources(0, kVsSrv, r.vs_srv);
            ID3D11Buffer* p[kPsCb]{};
            for (int j = 0; j < kPsCb; ++j) p[j] = r.ps_cb_view[j] ? vcb[0] : r.ps_cb[j];
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
    }
    if (model) holster_model_draw(ctx, vcb);
    if (t.log.load() > 0 && t.shown_frame != m_frame.load()) {
        t.log--;
        std::string what;
        for (int i = 0; i < ns; ++i)
            what += fmt::format("{}weapon {} {} at ({:.2f}, {:.2f}, {:.2f})", i ? ", " : "", show[i].w->sig, show[i].where, show[i].pos.x, show[i].pos.y, show[i].pos.z);
        spdlog::info("[Holster] drawn before the gloves, frame {}: {} ({} draws)", m_frame.load(), what, k);
    }
    t.shown_frame = m_frame.load();
    t.shown++;

    // Put everything back.
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(topo);
    ctx->IASetVertexBuffers(0, kVb, vb, stride, off);
    ctx->IASetIndexBuffer(ib, ib_fmt, ib_off);
    ctx->VSSetShader(vs, vs_ci, vs_nci);
    call_orig<void>(m_ps_set_hook, 9, ctx, ps, static_cast<ID3D11ClassInstance* const*>(ps_ci), ps_nci);
    call_orig<void>(m_vs_cb_hook, 7, ctx, 0u, 5u, static_cast<ID3D11Buffer* const*>(vcb));
    ctx->PSSetConstantBuffers(0, kPsCb, pcb);
    ctx->VSSetShaderResources(0, kVsSrv, vsrv);
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
    for (auto* x : vsrv) rel(x);
    for (auto* x : srv) rel(x);
    for (auto* x : smp) rel(x);
}

bool FarCry5Stereo::holster_command(const std::string& cmd, const std::string& arg, float fv, std::istream& rest, bool& persist)
{
    if (!m_hol) m_hol = new HolsterState{};
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "weaponcopies") { m_hol->copies = on; persist = true; return true; }
    if (cmd == "sidearmsig") { spdlog::info("[Holster] 'sidearmsig {}' ignored (the sidearm is learned at each load)", arg); return true; }   // old saved lines
    if (cmd == "sidearmnow") { m_hol->slot_sig = uint32_t(std::strtoul(arg.c_str(), nullptr, 10)); return true; }   // test / F11: this weapon is the sidearm (this session)
    if (cmd == "sidearmauto") { m_hol->autoside = on; persist = true; return true; }
    if (cmd == "holsterhand") { m_hol_copy_hand = on; return true; }   // test: A/B of the copy in the empty hand
    if (cmd == "holsterglove") { m_hol_glove = on; return true; }      // test: A/B of the recorded hand on the copy in the hand
    if (cmd == "holstermodel") { m_hol->model_on = on; persist = true; return true; }
    if (cmd == "holstergap" || cmd == "holsterwall" || cmd == "holstermouth") {   // the holster's fit (m): gap off the gun, wall, mouth along the barrel from the grip
        (cmd == "holstergap" ? m_hol->model_gap : cmd == "holsterwall" ? m_hol->model_wall : m_hol->model_mouth) =
            cmd == "holstermouth" ? std::clamp(fv, -0.15f, 0.05f) : std::clamp(fv, 0.0f, 0.02f);
        m_hol->model_sig = 0;   // built again at the next draw
        persist = true;
        return true;
    }
    if (cmd == "holsterlog") { m_hol->log = arg.empty() ? 6 : std::clamp(int(fv), 0, 1000); return true; }
    if (cmd == "holsterclear") {
        // Test: every recording dropped (each weapon is recorded again at its next hold).
        for (auto& w : m_hol->weapons) w.release();
        m_hol->weapons.clear();
        m_hol->hold_recorded = false;
        spdlog::info("[Holster] recordings dropped");
        return true;
    }
    if (cmd == "sidearmweapons") {
        // The whole list each time (the saved line is the last one): +sig a sidearm, -sig not one; "none" clears it.
        std::set<uint32_t> yes, no;
        std::string        tok = arg;
        do {
            if (tok.size() > 1 && (tok[0] == '+' || tok[0] == '-')) {
                const uint32_t s = uint32_t(std::strtoul(tok.c_str() + 1, nullptr, 10));
                if (s != 0) (tok[0] == '+' ? yes : no).insert(s);
            }
        } while (rest >> tok);
        {
            std::lock_guard _{ m_hol->mtx };
            m_hol->force_on  = std::move(yes);
            m_hol->force_off = std::move(no);
        }
        persist = true;
        return true;
    }
    return false;
}

// F11 Gestures > Sidearm on the right hip: the recorded weapons, which is the sidearm, how the switch is made.
void FarCry5Stereo::draw_holster_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    if (!m_hol) m_hol = new HolsterState{};
    auto& t = *m_hol;
    bool  c = t.copies.load();
    if (ImGui::Checkbox("Show the weapon the game does not hold where it is carried##weaponcopies", &c)) {
        t.copies = c;
        keep("weaponcopies", c ? "weaponcopies on" : "weaponcopies off");
    }
    hint("On (default): the mod draws its own copy of your main weapon on the sling while you hold the sidearm, and of the sidearm on the hip "
         "while you hold the main weapon. A weapon is copied once you have held it still in your hand for a second (the first time after each "
         "start of the game): until then its place stays empty.");
    int key = fc5::gesture::side_key();
    int sel = key < 0 ? 0 : key == 0 ? 1 : key - 0x02 + 2;
    const char* const items[]{ "The weapon wheel's rightmost slot", "The game's last weapon (pad Y)", "Keyboard key 1", "Keyboard key 2", "Keyboard key 3",
                               "Keyboard key 4", "Keyboard key 5", "Keyboard key 6", "Keyboard key 7", "Keyboard key 8", "Keyboard key 9" };
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("The hip takes##sidearmpick", &sel, items, IM_ARRAYSIZE(items))) {
        const std::string v = sel == 0 ? "wheel" : sel == 1 ? "pad" : std::to_string(sel - 1);
        m_ui_cmds += "sidearmpick " + v + "\n";
    }
    hint("The weapon wheel's rightmost slot (default; Homard 27 Oct: the sidearm slot): the hip opens the wheel for half a second and picks that slot; "
         "the game's HUD is hidden meanwhile. A keyboard key (key 4 is that slot) also works but turns the game's button prompts to the keyboard's. Only the weapon that slot gives is the sidearm: nothing else ever hangs on the hip. If you pick the sidearm in the wheel yourself, it goes "
         "on the hip and the sling shows no weapon. The game's last weapon (pad Y) goes between the two weapons you last held instead (then a "
         "measured handgun counts as a sidearm). The switch back to the main weapon always uses the last weapon.");
    bool pk = fc5::gesture::side_peek_on();
    if (ImGui::Checkbox("Look at the sidearm slot after loading##sidearmpeek", &pk)) m_ui_cmds += pk ? "sidearmpeek on\n" : "sidearmpeek off\n";
    hint("On (default): the mod can only show a weapon it has seen the game draw. After a load, once your main weapon is out (no grab held), "
         "it picks the sidearm slot, records the sidearm (about a second, on your hip) and switches back, so the hip shows it from then on.");
    bool hm = m_hol->model_on.load();
    if (ImGui::Checkbox("Holster on the hip##holstermodel", &hm)) m_ui_cmds += hm ? "holstermodel on\n" : "holstermodel off\n";
    hint("On (default; Homard 30 Oct: \"a good looking holster model\"): a holster molded on your sidearm (from its own mesh), drawn with its material, "
         "on your hip whether the gun is in it or in your hand. Fit: holstergap (default 0.004 m off the gun), holsterwall (0.003 m), holstermouth "
         "(-0.025 m: where it ends along the barrel from your grip).");
    bool sy = fc5::gesture::side_y_on();
    if (ImGui::Checkbox("Hip grab: the game's last weapon when that is the sidearm (faster)##sidearmy", &sy)) m_ui_cmds += sy ? "sidearmy on\n" : "sidearmy off\n";
    hint("On (default; Homard 28 Oct: \"make it instantaneous\"): when the weapon you held before the one in hand was the sidearm (the usual hip, sling, hip), "
         "the hip switches with pad Y like the sling does, the game's quicker switch, instead of opening the weapon wheel. If Y brings another weapon, the "
         "wheel's sidearm slot is picked next. Off: always the wheel.");
    const uint32_t sig = m_weapon_sig.load();
    if (key != 0) {
        ImGui::TextDisabled("Sidearm: weapon %u%s; weapon in the game's hands now: %u", t.slot_sig.load(), t.slot_sig.load() == 0 ? " (none yet)" : "", sig);
        if (sig != 0 && ImGui::Button("The weapon in my hand is the sidearm##sideslot")) m_ui_cmds += fmt::format("sidearmnow {}\n", sig);
        for (const auto& w : t.weapons)
            ImGui::TextDisabled("Copied: weapon %u, %s, %d draws%s", w.sig, holster_is_side(w.sig) ? "sidearm" : "main weapon", int(w.parts.size()),
                                w.sig == t.main_sig ? " (on the sling)" : w.sig == t.side_sig ? " (on the hip)" : "");
        if (t.weapons.empty()) ImGui::TextDisabled("No weapon copied yet: hold each weapon still in your hand for a second.");
        return;
    }
    bool a = t.autoside.load();
    if (ImGui::Checkbox("Tell the sidearm by how the game holds it##sidearmauto", &a)) {
        t.autoside = a;
        keep("sidearmauto", a ? "sidearmauto on" : "sidearmauto off");
    }
    hint("On (default): a gun the game holds with the left hand beside or behind the right one (a handgun) is a sidearm; one with the left hand "
         "along the barrel is a main weapon. It is measured while you hold the gun still. The buttons below decide for the gun in your hand.");
    const int      meas = gun_still_handgun(sig);
    bool           f_on = false, f_off = false;
    std::string    list;
    {
        std::lock_guard _{ t.mtx };
        f_on  = t.force_on.contains(sig);
        f_off = t.force_off.contains(sig);
    }
    ImGui::TextDisabled("Weapon in the game's hands now: %u, %s (%s)", sig, holster_is_side(sig) ? "a sidearm" : "a main weapon",
                        f_on || f_off ? "set by hand" : meas < 0 ? "not measured yet" : meas ? "measured: a handgun" : "measured: not a handgun");
    auto set_list = [&](int mode) {   // 1 a sidearm, 0 not, -1 measured
        std::string line = "sidearmweapons";
        {
            std::lock_guard _{ t.mtx };
            t.force_on.erase(sig);
            t.force_off.erase(sig);
            if (mode == 1) t.force_on.insert(sig);
            if (mode == 0) t.force_off.insert(sig);
            for (uint32_t s : t.force_on) line += fmt::format(" +{}", s);
            for (uint32_t s : t.force_off) line += fmt::format(" -{}", s);
        }
        if (line == "sidearmweapons") line += " none";
        keep("sidearmweapons", line);
    };
    if (sig != 0) {
        if (ImGui::Button("This weapon is a sidearm##sideon")) set_list(1);
        ImGui::SameLine();
        if (ImGui::Button("Not a sidearm##sideoff")) set_list(0);
        ImGui::SameLine();
        if (ImGui::Button("As measured##sideauto")) set_list(-1);
    }
    for (const auto& w : t.weapons)
        ImGui::TextDisabled("Copied: weapon %u, %s, %d draws%s", w.sig, holster_is_side(w.sig) ? "sidearm" : "main weapon", int(w.parts.size()),
                            w.sig == t.main_sig ? " (on the sling)" : w.sig == t.side_sig ? " (on the hip)" : "");
    if (t.weapons.empty()) ImGui::TextDisabled("No weapon copied yet: hold each weapon still in your hand for a second.");
}
