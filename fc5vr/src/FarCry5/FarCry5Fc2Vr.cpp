// FC2 VR motion (24 Oct, Homard 22:10 "progress on all fronts"): the render side of skelhand (FarCry5PoseProbe.cpp does the move in
// the game's skeleton), its measurement, and the F11 Experimental section "FC2VR motion (test)" with every FC2 VR switch.
//  - skelhand 0|1|2 (saved, default 0): 1 = the weapon hand (bone 194) on the controller in the game's skeleton, 2 = the same move
//    given to the whole arm from the shoulder (191). The move is the mod's controller placement without gunstill (the game's own pose
//    of the hand relative to its camera, put on the controller with the grip calibration), taken into the arms' model space through
//    their root's object matrix (the one the skin matrices use). While it is on, the weapon pass's camera is the eye's, gunstill and
//    gunanchor step aside, and the floating left hand works against the identity (the game already holds the gun at the controller).
//    Stereo only.
//  - skelhandstat on|off (test): every 300 frames, the drawn gun's pose against the controller (offset, spread, frame-to-frame jitter
//    per eye), with skelhand on or off; skelhandstatus: the job-thread side's state.
//  - throwarc on|off (saved, default off): while a throwable is held (the chest gesture, or the pad's RB held), a dotted arc in both
//    eyes from the left hand where the head looks: throwarcspeed m/s (15) launched throwarcup deg (10) above the head's direction,
//    gravity 9.81 m/s2, until 2.5 s or 30 m below the hand. The game's own throw speed per throwable is not known to the mod: the
//    arc is a guide, not the game's path; the scene does not stop it.
//  - meleeswing on|off (saved, default off; log only): a swing of the weapon hand faster than meleespeed m/s (2.5; relative to the game
//    camera, so walking does not count) with something within reach in front (the barrel ray meets the scene within 1.2 m) logs
//    "[Melee] ... would press melee". The game's melee is not pressed (the switch is a test of the detection: no attacks on NPCs).
#include "FarCry5Stereo.h"
#include "FarCry5BulletSpeed.h"
#include "FarCry5PoseProbe.h"
#include "FarCry5Gesture.h"
#include "FarCry5Aim.h"
#include "FarCry5Afw.h"

#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace fc5::skinorder
{
    bool install(const std::atomic<int>* frame);
}

bool FarCry5Stereo::skel_hand_active() const { return fc5::skelhand::mode() > 0 && m_motion.load() && !afw_mode(); }
// skelhandstill (24 Oct, after Homard's "I had hoped that the model would be a lot steadier in hand"): skelhand 3 gives the game's gun the
// whole placement with gunstill (the gun at its standing pose against the controller, the game's sway taken out except for reloads and
// weapon switches); gunstill measures the game's own matrix of the gun (the drawn one is already moved).
// skelhandarms: with skelhand 3, the game's arms are shown (not hidden by the floating weapon) and moved onto the hands by IK.
bool FarCry5Stereo::skel_hand_arms() const { return skel_hand_active() && fc5::skelhand::mode() == 3 && fc5::skelhand::arms(); }
bool FarCry5Stereo::skel_hand_still() const { return skel_hand_active() && fc5::skelhand::mode() == 3 && m_skh_still.load(); }

bool FarCry5Stereo::skel_hand_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    if (cmd == "skelhand") {
        const int m = std::clamp(std::atoi(arg.c_str()), 0, 3);
        if (m > 0 && !fc5::skinorder::install(&m_frame)) spdlog::info("[SkelHand] the palette writer could not be hooked: no identity check");
        if (!fc5::skelhand::set_mode(m, &m_frame)) spdlog::info("[SkelHand] the bone getter could not be hooked: skelhand stays off");
        persist = true;
        spdlog::info("[SkelHand] skelhand {}{}", fc5::skelhand::mode(), m > 0 && afw_mode() ? " (AFW: not applied, Stereo only)" : "");
        return true;
    }
    if (cmd == "skelhandlate") { m_skh_late = arg == "on" || arg == "1" || arg == "pos"; m_skh_late_pos = arg == "pos"; persist = true; return true; }   // pos: the move only, no turn (test)
    if (cmd == "skelhandarms") { fc5::skelhand::set_arms(arg == "on" || arg == "1"); persist = true; return true; }
    if (cmd == "skelhandstill") { m_skh_still = arg == "on" || arg == "1"; persist = true; return true; }
    if (cmd == "skelhandstat") { m_skh_stat = arg == "on" || arg == "1"; return true; }
    if (cmd == "throwarc") { m_throw_arc = arg == "on" || arg == "1"; persist = true; return true; }
    if (cmd == "meleeswing") { m_melee_swing = arg == "on" || arg == "1"; persist = true; return true; }
    if (cmd == "meleespeed") { m_melee_speed = std::clamp(float(std::atof(arg.c_str())), 0.5f, 10.0f); persist = true; return true; }    if (cmd == "throwarcspeed") { m_arc_speed = std::clamp(float(std::atof(arg.c_str())), 3.0f, 60.0f); persist = true; return true; }
    if (cmd == "throwarcsnap") { m_arc_snap = true; return true; }   // test: the next eye image with the arc drawn saved to probes\arc_N.bmp
    if (cmd == "throwarcup") { m_arc_up = std::clamp(float(std::atof(arg.c_str())), -30.0f, 60.0f); persist = true; return true; }
    if (cmd == "skelhandstatus") { spdlog::info("[SkelHand] status: {}", fc5::skelhand::status()); return true; }
    return false;
}

// The weapon camera upload (weapon view, motion controls): today's move (w_full, w_off, with gunstill) for the measurement, and
// skelhand's target: Mw0 (the placement without gunstill) in the arms' model space.
void FarCry5Stereo::skel_hand_camera(int frame, const glm::mat3& Wc, const glm::vec3& ctrl, const glm::vec3& grip, const glm::mat3& wrot, const glm::vec3& cam,
                                     const glm::mat3& w_full, const glm::vec3& w_off)
{
    auto affine = [&](const glm::mat3& A, const glm::vec3& off) {   // p -> A (p - cam) + off
        glm::mat4 M{ A };
        M[3] = glm::vec4{ off - A * cam, 1.0f };
        return M;
    };
    m_skh.frame  = frame;
    m_skh.Mw     = affine(w_full, w_off);
    m_skh.Wc     = Wc;
    m_skh.ctrl   = ctrl;
    m_skh.active = skel_hand_active();
    if (fc5::skelhand::mode() == 0) return;
    const glm::mat4 Mw0  = affine(Wc * wrot, ctrl - Wc * grip);
    const bool      root = frame - m_skh_root_frame <= 4;
    fc5::skelhand::set_target(frame, root ? glm::inverse(m_skh_root) * Mw0 * m_skh_root : glm::mat4{ 1.0f }, root && m_skh.active);
    // skelhand 3: the gun's world matrix; with skelhandstill the whole placement (gunstill's hold in it), the hand moved the same way.
    const glm::mat4 Mw3 = skel_hand_still() ? m_skh.Mw : Mw0;
    if (fc5::skelhand::mode() == 3) fc5::skelhand::set_target(frame, root ? glm::inverse(m_skh_root) * Mw3 * m_skh_root : glm::mat4{ 1.0f }, root && m_skh.active);
    fc5::skelhand::set_world_target(frame, Mw3, m_skh.active && fc5::skelhand::mode() == 3);
    // skelhandlate: the controller's poses by frame; the move since the one the drawn update was placed with.
    m_skh_ctrl[frame & 15] = SkhCtrl{ frame, Wc, ctrl };
    m_skh_late_ok          = false;
    if (m_skh_late.load() && m_skh.active && fc5::skelhand::mode() == 3) {
        // The drawn update's placement: as old as the last drawn gun's (matched at its draw, after this camera is set).
        const int   fu = frame - m_skh_lag;
        const auto& c0 = m_skh_ctrl[fu & 15];
        if (c0.frame == fu && frame - fu <= 8 && frame >= fu) {
            m_skh_late_R    = m_skh_late_pos ? glm::mat3{ 1.0f } : Wc * glm::transpose(c0.Wc);
            m_skh_late_from = c0.ctrl;
            m_skh_late_to   = ctrl;
            m_skh_late_ok   = true;
            m_skh_late_frame = frame;
            m_skh_late_n++;
            m_skh_late_cm = 100.0f * glm::length(ctrl - c0.ctrl);
        } else {
            m_skh_late_miss++;
        }
    }
}

// A draw about to be made (weapon_draw): in the weapon pass, the arms' root (a skinned draw whose object matrix is a heading only) and
// the gun mesh (the weapon's signature draw) for the measurement.
void FarCry5Stereo::skel_hand_draw(ID3D11DeviceContext* ctx, UINT count)
{
    if (!m_vs_cb0 || !is_weapon_cb(m_vs_cb0)) return;
    ID3D11Buffer* cb[3]{};
    ctx->VSGetConstantBuffers(1, 3, cb);
    ObjConsts o1{}, o3{};
    bool      h1 = false, h3 = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (cb[0]) if (auto it = m_obj_consts.find(cb[0]); it != m_obj_consts.end()) { o1 = it->second; h1 = true; }
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end()) { o3 = it->second; h3 = true; }
    }
    for (auto* b : cb) if (b) b->Release();
    if (!h1 || o1.size < 48) return;
    glm::mat4 Om{ 1.0f };
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) Om[c][r] = o1.f[r * 4 + c];
    const int frame = m_frame.load();
    const bool heading_only = std::abs(o1.f[8]) < 0.02f && std::abs(o1.f[9]) < 0.02f && std::abs(o1.f[10] - 1.0f) < 0.02f && std::abs(o1.f[2]) < 0.02f && std::abs(o1.f[6]) < 0.02f;
    if (h3 && o3.size == 12288 && heading_only) {
        m_skh_root       = Om;
        m_skh_root_frame = frame;
    }
    if (count != 0 && count == m_weapon_sig.load() && fc5::skelhand::mode() == 3) {
        if (count != m_skh_gun_sig) { m_skh_gun_sig = count; fc5::skelhand::relearn_gun(); }   // another weapon: its entity learned again
        fc5::skelhand::note_gun(frame, Om);
    }
    if (!m_skh_stat.load() || count == 0 || count != m_weapon_sig.load() || m_skh.frame != frame || m_skh_g_frame == frame) return;
    m_skh_g_frame = frame;
    // The gun as drawn: with skelhand the game draws it where its skeleton holds it (the pass's camera is the eye's); without, the
    // weapon camera moves it by today's placement.
    glm::mat4 late{ 1.0f };   // skelhandlate's turn of the weapon pass, as the eye sees the gun
    if (m_skh_late_ok && m_skh_late_frame == frame) {
        late    = glm::mat4{ m_skh_late_R };
        late[3] = glm::vec4{ m_skh_late_to - m_skh_late_R * m_skh_late_from, 1.0f };
    }
    const glm::mat4 drawn = m_skh.active ? late * Om : m_skh.Mw * Om;
    glm::mat4       W{ m_skh.Wc };
    W[3]                  = glm::vec4{ m_skh.ctrl, 1.0f };
    const glm::mat4 rel   = glm::inverse(W) * drawn;
    const glm::vec3 p     = glm::vec3{ rel[3] };
    glm::mat3       R{ rel };
    for (int k = 0; k < 3; ++k) R[k] = glm::normalize(R[k]);
    const glm::quat q = glm::normalize(glm::quat_cast(R));
    auto&           s = m_skh_st;
    const int       e = frame & 1;
    if (s.prev_frame[e] >= 0 && frame - s.prev_frame[e] <= 4) {
        const float dc = 100.0f * glm::length(p - s.prev_p[e]);
        const float dd = glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(q, s.prev_q[e])))));
        s.steps++;
        s.step_cm += dc;
        s.step_deg += dd;
        s.step_max_cm  = std::max(s.step_max_cm, dc);
        s.step_max_deg = std::max(s.step_max_deg, dd);
    }
    s.prev_p[e]     = p;
    s.prev_q[e]     = q;
    s.prev_frame[e] = frame;
    s.ps.push_back(p);
    s.qs.push_back(q);
}

void FarCry5Stereo::skel_hand_present()
{
    melee_frame();       // meleeswing (log only)
    throw_arc_result();  // throwarc: a throw measured
    auto& s = m_skh_st;
    if (s.ps.size() < 300) return;
    glm::vec3 mean{ 0.0f };
    for (const auto& p : s.ps) mean += p;
    mean /= float(s.ps.size());
    float sp = 0.0f, sp_max = 0.0f, rd = 0.0f, rd_max = 0.0f;
    for (size_t i = 0; i < s.ps.size(); ++i) {
        const float d = 100.0f * glm::length(s.ps[i] - mean);
        const float a = glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(glm::dot(s.qs[i], s.qs[0])))));
        sp += d;
        sp_max = std::max(sp_max, d);
        rd += a;
        rd_max = std::max(rd_max, a);
    }
    const float n = float(s.ps.size());
    spdlog::info("[SkelHand] skelhand {}: the drawn gun against the controller over {} frames: offset ({:.1f}, {:.1f}, {:.1f}) cm; spread {:.2f} cm (max {:.2f}), {:.2f} deg (max {:.2f}) "
                 "from the first; jitter per eye {:.2f} cm (max {:.2f}), {:.2f} deg (max {:.2f}) per frame | {}",
                 m_skh.active ? fc5::skelhand::mode() : 0, s.ps.size(), 100.0f * mean.x, 100.0f * mean.y, 100.0f * mean.z, sp / n, sp_max, rd / n, rd_max,
                 s.steps ? s.step_cm / s.steps : 0.0, s.step_max_cm, s.steps ? s.step_deg / s.steps : 0.0, s.step_max_deg, fc5::skelhand::status() + "; " + fc5::skelhand::gun_status() + fmt::format("; skelhandstill {}, gunstill fed the game's gun {} / not matched {}; skelhandlate {} (applied {}, no pose for the update {}, last {:.1f} cm, placement {}-{} frames old)", skel_hand_still() ? "on" : "off", m_skh_raw_hits.load(), m_skh_raw_miss.load(),
                                                                                                  m_skh_late.load() ? "on" : "off", m_skh_late_n, m_skh_late_miss, m_skh_late_cm, m_skh_lag_min, m_skh_lag_max));
    s = SkhStat{};
}

void FarCry5Stereo::draw_fc2vr_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                    const std::function<void(const char*)>& section)
{
    // Labelled without the other mod's name (Homard 24 Oct: "Remove all mention of FC2 in the menu").
    section("Research for alternatives (test)");
    ImGui::TextDisabled("Other ways of holding the gun and aiming, each off by default. Stereo only. Try one at a time.");
    {
        int  sh = fc5::skelhand::mode();
        ImGui::TextUnformatted("Hand in the game's skeleton");
        bool ch = ImGui::RadioButton("Off##skelhand", &sh, 0);
        ImGui::SameLine();
        ch |= ImGui::RadioButton("Hand##skelhand", &sh, 1);
        ImGui::SameLine();
        ch |= ImGui::RadioButton("Whole arm##skelhand", &sh, 2);
        ImGui::SameLine();
        ch |= ImGui::RadioButton("Gun and hand##skelhand", &sh, 3);
        if (ch) {
            fc5::skinorder::install(&m_frame);
            fc5::skelhand::set_mode(sh, &m_frame);
            keep("skelhand", fmt::format("skelhand {}", sh));
        }
        hint("The game itself holds the gun at your controller: your weapon hand is moved in the game's own skeleton, so the gun, its muzzle "
             "flash and its tracers come out of it. Hand: only the hand moves. Whole arm: the arm moves with it from the shoulder. Look for: the gun "
             "on the controller, steady, the same in both eyes; the muzzle flash and tracers at the gun. Off: as before.");
        bool v = m_skh_still.load();
        if (ImGui::Checkbox("Gun and hand: held still (reloads shown)##skelhandstill", &v)) {
            m_skh_still = v;
            keep("skelhandstill", v ? "skelhandstill on" : "skelhandstill off");
        }
        hint("With Gun and hand: the game's gun is held at its standing pose in your hand (as \"Gun moves only for reloads\"), its sway, bob and "
             "recoil taken out; reloads and weapon switches still play. Off: the game's own sway of the gun is kept.");
        bool l = m_skh_late.load();
        if (ImGui::Checkbox("Gun and hand: no trail when swinging (test)##skelhandlate", &l)) {
            m_skh_late = l;
            keep("skelhandlate", l ? "skelhandlate on" : "skelhandlate off");
        }
        hint("With Gun and hand: the game places the gun one update before it is drawn; this turns the gun's picture by how far your controller "
             "moved since, so the gun does not trail your hand in fast swings.");
        bool a = fc5::skelhand::arms();
        if (ImGui::Checkbox("Gun and hand: the game's arms shown, bent to the hands##skelhandarms", &a)) {
            fc5::skelhand::set_arms(a);
            keep("skelhandarms", a ? "skelhandarms on" : "skelhandarms off");
        }
        hint("With Gun and hand: the game's own arms are drawn (not hidden), both hands on the gun where the game holds them, the forearms and "
             "upper arms bent to reach them from the shoulders (the elbows bend the way the game's do).");
    }
    {
        bool v = m_throw_arc.load();
        if (ImGui::Checkbox("Throw arc##throwarc", &v)) {
            m_throw_arc = v;
            keep("throwarc", v ? "throwarc on" : "throwarc off");
        }
        hint("While you hold a throwable (from the chest, or with the throw button held), a dotted arc shows where it would fly if thrown where you look. "
             "A guide: the game's own throw speed is not known to the mod (throwarcspeed, 15 m/s), and walls do not stop the arc.");
    }
    {
        bool v = m_melee_swing.load();
        if (ImGui::Checkbox("Melee by swinging (log only)##meleeswing", &v)) {
            m_melee_swing = v;
            keep("meleeswing", v ? "meleeswing on" : "meleeswing off");
        }
        hint("Test of the detection only: a fast swing of your weapon hand (over meleespeed, 2.5 m/s) with something within reach in front is written "
             "to the log ([Melee]). The game's melee is not pressed yet.");
    }
    {
        bool v = m_skin_pose.load();
        if (ImGui::Checkbox("Gun and hands moved in their own draws##skinpose", &v)) {
            m_skin_pose = v;
            keep("skinpose", v ? "skinpose on" : "skinpose off");
        }
        hint("23 Oct test (no gain in Homard's headset): every piece of the gun and hands is moved onto the controller in its own draw instead of turning "
             "the weapon picture's camera.");
    }
    {
        int  ba = fc5::bullet::aim_mode();
        ImGui::TextUnformatted("Bullets from the barrel");
        bool ch = ImGui::RadioButton("Off##bulletaim", &ba, 0);
        ImGui::SameLine();
        ch |= ImGui::RadioButton("Along the barrel##bulletaim", &ba, 1);
        ImGui::SameLine();
        ch |= ImGui::RadioButton("From the muzzle##bulletaim", &ba, 2);
        if (ch) {
            fc5::bullet::set_aim_mode(ba);
            if (ba > 0) fc5::bullet::install_shot_hook(reinterpret_cast<uint8_t*>(GetModuleHandleA("FC_m64.dll")));
            keep("bulletaim", fmt::format("bulletaim {}", ba));
        }
        hint("Your shots are sent along the gun's barrel when the game fires them (the game's spread is kept), and while you fire the game's aim stays "
             "where your head looks: no swing of the aim, no trigger delay, nothing culled. From the muzzle: they also start at the gun's muzzle "
             "(not inside a wall). Off: as before.");
    }
}

// throwarc: the predicted throw drawn into this eye's image (as the aim dot: small discs written with ClearView at projected points).
void FarCry5Stereo::draw_throw_arc(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb)
{
    if (!m_throw_arc.load() || !m_motion.load() || !m_mo.valid || m_menu_screen_on || m_player_views_this_frame == 0 || m_hist[0].cur_frame != m_frame.load()) return;
    const bool held = fc5::gesture::throw_phase() == fc5::gesture::ThrowPhase::Held || m_rb_down.load();
    const glm::vec3 o  = m_mo.other_valid ? m_mo.other_pos_world : m_mo.aim_origin;
    glm::vec3       hf = glm::normalize(m_mo.head_world);
    if (m_arc_was_held && !held) throw_arc_release(o, hf);   // let go: the thrown object's launch measured
    m_arc_was_held = held;
    if (!held) {
        m_arc_drawn = false;
        return;
    }
    if (bb != m_arc_tex) {
        if (m_arc_view) m_arc_view->Release();
        m_arc_view = nullptr;
        m_arc_tex  = bb;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            D3D11_TEXTURE2D_DESC bd{};
            bb->GetDesc(&bd);
            if (bd.BindFlags & D3D11_BIND_RENDER_TARGET) {
                ID3D11RenderTargetView* v = nullptr;
                dev->CreateRenderTargetView(bb, nullptr, &v);
                m_arc_view = v;
            } else if (bd.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
                ID3D11UnorderedAccessView* v = nullptr;
                dev->CreateUnorderedAccessView(bb, nullptr, &v);
                m_arc_view = v;
            }
            dev->Release();
        }
    }
    if (!m_arc_view) return;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) || !ctx1) return;
    // From the left hand (the throwable's), where the head looks, raised by throwarcup; a throwable thrown before: its measured launch
    // (speed, angle above the head's direction, gravity).
    ArcFit fit{ m_arc_speed.load(), m_arc_up.load(), 9.81f };
    // (the held throwable's id is not steady while it is drawn: the last measured throw stands in for one not measured yet)
    bool measured = false;
    if (auto it = m_arc_fit.find(m_arc_sig.load()); it != m_arc_fit.end()) { fit = it->second; measured = true; }
    else if (auto it2 = m_arc_fit.find(m_arc_rel_sig); it2 != m_arc_fit.end()) { fit = it2->second; measured = true; }
    const float     yaw = std::atan2(-hf.x, hf.y), pit = std::asin(std::clamp(hf.z, -1.0f, 1.0f)) + glm::radians(fit.up);
    const glm::vec3 v  = glm::vec3{ -std::sin(yaw) * std::cos(pit), std::cos(yaw) * std::cos(pit), std::sin(pit) } * fit.speed;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    const float     r = std::max(3.0f, aim_marker_px(d.Width) * 1.0f);   // (0.6 of the aim dot was too small to see in a capture)
    const float     light[4]{ 1.0f, 0.95f, 0.55f, 1.0f }, dark[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    int             drawn = 0;
    for (int i = 1; i <= 60; ++i) {
        const float     tt = 0.04f * float(i);
        const glm::vec3 p  = o + v * tt + glm::vec3{ 0.0f, 0.0f, -0.5f * fit.g } * tt * tt;
        if (p.z < o.z - 30.0f) break;
        if (i % 2) continue;   // dotted
        const glm::vec4 cl = m_hist[0].cur_vrp * glm::vec4{ p - m_hist[0].cur_cam, 1.0f };
        if (cl.w <= 0.05f) continue;
        const float x = (cl.x / cl.w * 0.5f + 0.5f) * d.Width, y = (0.5f - cl.y / cl.w * 0.5f) * d.Height;
        const float rr = std::max(2.5f, r * std::min(1.0f, 6.0f / std::max(0.5f, cl.w)));   // smaller further away
        if (x < rr + 2 || y < rr + 2 || x > d.Width - rr - 2 || y > d.Height - rr - 2) continue;
        auto disc = [&](float rad, const float col[4]) {
            D3D11_RECT rows[64];
            UINT       n  = 0;
            const LONG ry = (LONG)std::floor(rad);
            for (LONG dy = -ry; dy <= ry && n < 64; ++dy) {
                const LONG hw = (LONG)std::floor(std::sqrt(std::max(0.0f, (rad + 0.5f) * (rad + 0.5f) - (float)(dy * dy))) - 0.5f);
                rows[n++]     = { (LONG)x - hw, (LONG)y + dy, (LONG)x + hw + 1, (LONG)y + dy + 1 };
            }
            ctx1->ClearView(m_arc_view, col, rows, n);
        };
        disc(rr + 1.5f, dark);
        disc(rr, light);
        drawn++;
    }
    ctx1->Release();
    if (drawn && m_arc_snap.exchange(false)) {
        const auto name = fmt::format("D:/SteamLibrary/steamapps/common/FarCry5/VRMod_dev/probes/arc_{}.bmp", m_presents);
        fc5::afw::save_image(ctx, bb, name);
        spdlog::info("[ThrowArc] image with the arc ({} dots) saved to {}", drawn, name);
    }
    if (!m_arc_drawn && drawn) spdlog::info("[ThrowArc] drawn: {} dots from ({:.2f}, {:.2f}, {:.2f}), {:.1f} m/s, {:.1f} deg up, gravity {:.2f} (throwable {}: {})", drawn, o.x, o.y, o.z, fit.speed, glm::degrees(pit), fit.g,
                                            m_arc_sig.load(), measured ? "measured" : "throwarcspeed");
    m_arc_drawn = drawn > 0;
}

// meleeswing (log only): the weapon hand's speed relative to the game camera each present; a swing past meleespeed with something within
// reach (the barrel ray's scene hit under 1.2 m) is logged, at most every 0.6 s.
void FarCry5Stereo::melee_frame()
{
    if (!m_melee_swing.load() || !m_motion.load() || !m_mo.valid || !m_mo.ctrl_valid || m_menu_screen_on) {
        m_melee_have = false;
        return;
    }
    const auto      now = std::chrono::steady_clock::now();
    const glm::vec3 rel = m_mo.ctrl_pos_world - m_world_cam_pos;
    if (m_melee_have) {
        const float dt = std::chrono::duration<float>(now - m_melee_t).count();
        if (dt > 1e-3f && dt < 0.2f) {
            const float v = glm::length(rel - m_melee_rel) / dt;
            m_melee_v     = 0.5f * m_melee_v + 0.5f * v;
            m_melee_peak  = std::max(m_melee_peak, m_melee_v);
            // The hand's motion: the direction marched each frame from the hand (0.8 m, aim channel 1) while it moves over 1 m/s.
            if (v > 1.0f) m_melee_dir = glm::normalize(glm::mix(m_melee_dir, glm::normalize(rel - m_melee_rel), 0.6f));
            m_melee_dir_ok = m_melee_v > 1.0f || m_melee_pending;
            if (m_melee_v > m_melee_speed.load() && now - m_melee_last > std::chrono::milliseconds(600)) {
                // A swing: judged once the marches dispatched along it from now on have answered (2 results, or 0.2 s): a result read
                // at once came from the frames before, along the previous motion (simulator 24 Oct: swings up "hit" the ground).
                m_melee_last           = now;
                m_melee_pending        = true;
                m_melee_pending_t      = now;
                m_melee_pending_v      = m_melee_v;
                m_melee_pending_barrel = fc5::aim::last_hit();
                fc5::aim::mark(1);
            }
            int         n    = 0;
            const float mhit = fc5::aim::hit_since_mark(1, n);   // the nearest scene within 0.8 m of the hand along its swing
            if (m_melee_pending && (n >= 2 || now - m_melee_pending_t > std::chrono::milliseconds(200))) {
                m_melee_pending  = false;
                const float hit  = m_melee_pending_barrel;
                const bool  reach = mhit > 0.0f && mhit <= 0.8f;
                m_melee_swings++;
                if (reach) m_melee_hits++;
                spdlog::info("[Melee] swing {:.1f} m/s (threshold {:.1f}) toward ({:.2f}, {:.2f}, {:.2f}); along the swing ({} marches): {}; the barrel ray: {}: {} ({} of {} swings would have)",
                             m_melee_pending_v, m_melee_speed.load(), m_melee_dir.x, m_melee_dir.y, m_melee_dir.z, n,
                             reach ? fmt::format("the scene {:.2f} m from the hand", mhit) : std::string{ "nothing within 0.8 m" },
                             hit > 0.0f ? fmt::format("the scene {:.1f} m ahead", hit) : std::string{ "nothing ahead" },
                             reach ? "would press melee (log only)" : "no melee", m_melee_hits, m_melee_swings);
            }
        }
    }
    m_melee_rel  = rel;
    m_melee_t    = now;
    m_melee_have = true;
}
// throwarc: at a release, the entity writer's probe follows what leaves the hand; 2.6 s later the launch of the object that flew
// furthest (velocity at launch, gravity) is taken for this throwable: speed, and its angle above the head's direction at the release.
void FarCry5Stereo::throw_arc_release(const glm::vec3& hand, const glm::vec3& head_dir)
{
    fc5::skelhand::throw_probe_arm(hand);
    m_arc_wait      = true;
    m_arc_rel_t     = std::chrono::steady_clock::now();
    m_arc_rel_sig   = m_arc_sig.load();
    m_arc_rel_head  = head_dir;
    m_arc_rel_pitch = glm::degrees(std::asin(std::clamp(head_dir.z, -1.0f, 1.0f)));
    spdlog::info("[ThrowArc] release (throwable {}): the thrown object followed from the hand at ({:.2f}, {:.2f}, {:.2f})", m_arc_rel_sig, hand.x, hand.y, hand.z);
}

void FarCry5Stereo::throw_arc_result()
{
    if (!m_arc_wait || std::chrono::steady_clock::now() - m_arc_rel_t < std::chrono::milliseconds(2600)) return;
    m_arc_wait  = false;
    const auto r = fc5::skelhand::throw_fit();
    if (!r.ok) {
        spdlog::info("[ThrowArc] measured throw: no object flew from the hand ({} entities followed, the furthest steady one moved {:.1f} m){}", r.tracked, r.dist, r.path);
        return;
    }
    const float sp    = glm::length(r.v0);
    const float elev  = glm::degrees(std::asin(std::clamp(r.v0.z / std::max(sp, 1e-3f), -1.0f, 1.0f)));
    const glm::vec2 hh{ m_arc_rel_head.x, m_arc_rel_head.y }, vh{ r.v0.x, r.v0.y };
    const float side = (glm::length(hh) > 1e-3f && glm::length(vh) > 1e-3f) ? glm::degrees(std::atan2(hh.x * vh.y - hh.y * vh.x, glm::dot(hh, vh))) : 0.0f;
    const bool  sane = sp > 3.0f && sp < 80.0f && r.g > 2.0f && r.g < 30.0f;
    spdlog::info("[ThrowArc] measured throw (throwable {}): launch {:.3f} s after the release, {:.1f} m/s, {:.1f} deg above level ({:.1f} above the head's direction, {:.1f} deg to its left), "
                 "gravity {:.2f} m/s2, flew {:.1f} m; {} samples fitted, {} entities followed{}; path from the hand:{}",
                 m_arc_rel_sig, r.t_launch, sp, elev, elev - m_arc_rel_pitch, side, r.g, r.dist, r.samples, r.tracked, sane ? "" : " (not taken: out of range)", r.path);
    if (sane && m_arc_rel_sig) m_arc_fit[m_arc_rel_sig] = ArcFit{ sp, elev - m_arc_rel_pitch, r.g };
}