// Throw arc and melee swing (24 Oct, Homard 22:10 "progress on all fronts"); their switches are in F11 Gestures.
// The "Research for alternatives" section (skelhand, skinpose, bulletaim) and its code were removed (Homard 30 Oct).
//  - throwarc on|off (saved, default off): while a throwable is held (the chest gesture, or the pad's RB held), a dotted arc in both
//    eyes from the left hand where the head looks: throwarcspeed m/s (15) launched throwarcup deg (10) above the head's direction,
//    gravity 9.81 m/s2, until 2.5 s or 30 m below the hand. The game's own throw speed per throwable is not known to the mod: the
//    arc is a guide, not the game's path; the scene does not stop it.
//  - meleeswing on|off (saved, default off; log only): a swing of the weapon hand faster than meleespeed m/s (2.5; relative to the game
//    camera, so walking does not count) with something within reach in front (the barrel ray meets the scene within 1.2 m) logs
//    "[Melee] ... would press melee". The game's melee is not pressed (the switch is a test of the detection: no attacks on NPCs).
#include "FarCry5Stereo.h"
#include "FarCry5PoseProbe.h"
#include "FarCry5Gesture.h"
#include "FarCry5Aim.h"
#include "FarCry5Afw.h"

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

bool FarCry5Stereo::throw_arc_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    if (cmd == "throwarc") { m_throw_arc = arg == "on" || arg == "1"; persist = true; return true; }
    if (cmd == "meleeswing") { m_melee_swing = arg == "on" || arg == "1"; persist = true; return true; }
    if (cmd == "meleespeed") { m_melee_speed = std::clamp(float(std::atof(arg.c_str())), 0.5f, 10.0f); persist = true; return true; }
    if (cmd == "throwarcspeed") { m_arc_speed = std::clamp(float(std::atof(arg.c_str())), 3.0f, 60.0f); persist = true; return true; }
    if (cmd == "throwarcsnap") { m_arc_snap = true; return true; }   // test: the next eye image with the arc drawn saved to probes\arc_N.bmp
    if (cmd == "throwarcup") { m_arc_up = std::clamp(float(std::atof(arg.c_str())), -30.0f, 60.0f); persist = true; return true; }
    return false;
}

void FarCry5Stereo::throw_melee_present()
{
    melee_frame();       // meleeswing (log only)
    throw_arc_result();  // throwarc: a throw measured
}


// throwarc: the predicted throw drawn into this eye's image (as the aim dot: small discs written with ClearView at projected points).
// The launch of the throwable in hand: measured (its own, else the last measured throw's), else throwarcspeed / throwarcup.
FarCry5Stereo::ArcFit FarCry5Stereo::throw_fit_now(bool& measured) const
{
    measured = false;
    if (fc5::gesture::throw_is_rock()) {   // the game's rock (582 indices): its own measure, else 26 m/s, 3 deg (headset 24 Oct)
        if (auto it = m_arc_fit.find(582u); it != m_arc_fit.end()) { measured = true; return it->second; }
        return ArcFit{ 26.0f, 3.0f, 9.81f };
    }
    if (auto it = m_arc_fit.find(m_arc_sig.load()); it != m_arc_fit.end()) { measured = true; return it->second; }
    if (auto it = m_arc_fit.find(m_arc_rel_sig); it != m_arc_fit.end()) { measured = true; return it->second; }
    // throwv2 before any measure: the knife's 36-40 m/s with no lift and the rock's 26 m/s, 3 deg (simulator 24-25 Oct) -> 25 m/s, 0 deg.
    if (fc5::gesture::throw_v2()) return ArcFit{ 25.0f, 0.0f, 9.81f };
    return ArcFit{ m_arc_speed.load(), m_arc_up.load(), 9.81f };
}

void FarCry5Stereo::draw_throw_arc(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb)
{
    // throwv2: its own trajectory (both grips held), along where the left controller points, from the holding hand; measured at the throw.
    const bool v2 = fc5::gesture::throw_v2();
    if ((!m_throw_arc.load() && !v2) || !m_motion.load() || !m_mo.valid || m_menu_screen_on || m_player_views_this_frame == 0 || m_hist[0].cur_frame != m_frame.load()) return;
    const auto ph   = fc5::gesture::throw_phase();
    const bool held = v2 ? fc5::gesture::throw_arc_shown() : ph == fc5::gesture::ThrowPhase::Held || m_rb_down.load();
    glm::vec3  o    = m_mo.other_valid ? m_mo.other_pos_world : m_mo.aim_origin;
    if (v2) {
        glm::mat3 r{ 1.0f };
        throwable_hold_pose(o, r);
    }
    glm::vec3 hf = glm::normalize(m_mo.head_world);
    if (v2) {
        // The game's throw goes once RB is let go (Throw): measured from then, against the game camera's direction (the lift it adds).
        const bool gone = ph == fc5::gesture::ThrowPhase::Throw || ph == fc5::gesture::ThrowPhase::Follow;
        // Against the trajectory's own launch direction (kept from while it showed): the fit's "above" and "to its left" are then the throw's miss.
        if (gone && !m_arc_v2_gone) {
            bool meas         = false;
            m_arc_rel_up_used = throw_fit_now(meas).up;   // the lift the camera was lowered by: the measured miss is added to it
            throw_arc_release(o, m_arc_v2_dir_ok ? m_arc_v2_dir : glm::normalize(-glm::vec3{ glm::row(glm::mat4{ m_mo.game_rot }, 2) }));
            m_arc_rel_v2 = m_arc_v2_dir_ok;
        }
        if (ph == fc5::gesture::ThrowPhase::Idle) m_arc_v2_dir_ok = false;
        m_arc_v2_gone = gone;
    } else if (m_arc_was_held && !held) {
        throw_arc_release(o, hf);   // let go: the thrown object's launch measured
        m_arc_rel_v2 = false;
    }
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
    // (the held throwable's id is not steady while it is drawn: the last measured throw stands in for one not measured yet)
    bool         measured = false;
    const ArcFit fit      = throw_fit_now(measured);
    // throwv2: launched along the left controller (the throw turns the game camera to it minus the lift, throw_dir_world).
    if (v2 && m_mo.other_valid) {
        hf               = glm::normalize(-m_mo.other_rot_world[2]);
        m_arc_v2_dir     = hf;
        m_arc_v2_dir_ok  = true;
    }
    const float     yaw = std::atan2(-hf.x, hf.y), pit = std::asin(std::clamp(hf.z, -1.0f, 1.0f)) + (v2 ? 0.0f : glm::radians(fit.up));
    const glm::vec3 v  = glm::vec3{ -std::sin(yaw) * std::cos(pit), std::cos(yaw) * std::cos(pit), std::sin(pit) } * fit.speed;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    const float     r = std::max(3.0f, aim_marker_px(d.Width) * 1.0f);   // (0.6 of the aim dot was too small to see in a capture)
    const float     light[4]{ 1.0f, 0.95f, 0.55f, 1.0f }, dark[4]{ 0.0f, 0.0f, 0.0f, 1.0f }, land[4]{ 1.0f, 0.45f, 0.1f, 1.0f };
    int             drawn = 0;
    // One dot (a dark rim, then the colour) at a world point in this eye's image; false off the image.
    auto put = [&](const glm::vec3& p, float scale, const float col[4]) {
        const glm::vec4 cl = m_hist[0].cur_vrp * glm::vec4{ p - m_hist[0].cur_cam, 1.0f };
        if (cl.w <= 0.05f) return false;
        const float x = (cl.x / cl.w * 0.5f + 0.5f) * d.Width, y = (0.5f - cl.y / cl.w * 0.5f) * d.Height;
        const float rr = scale * std::max(2.5f, r * std::min(1.0f, 6.0f / std::max(0.5f, cl.w)));   // smaller further away
        if (x < rr + 2 || y < rr + 2 || x > d.Width - rr - 2 || y > d.Height - rr - 2) return false;
        auto disc = [&](float rad, const float cc[4]) {
            D3D11_RECT rows[64];
            UINT       n  = 0;
            const LONG ry = (LONG)std::floor(rad);
            for (LONG dy = -ry; dy <= ry && n < 64; ++dy) {
                const LONG hw = (LONG)std::floor(std::sqrt(std::max(0.0f, (rad + 0.5f) * (rad + 0.5f) - (float)(dy * dy))) - 0.5f);
                rows[n++]     = { (LONG)x - hw, (LONG)y + dy, (LONG)x + hw + 1, (LONG)y + dy + 1 };
            }
            ctx1->ClearView(m_arc_view, cc, rows, n);
        };
        disc(rr + 1.5f, dark);
        disc(rr, col);
        return true;
    };
    // throwarcstop (Homard 26 Oct: "the path ... goes into the ground so it make it difficult to evaluate distance"): the arc is marched
    // through the scene depth at present (aim channel 2, from the next frame on); it stops where it meets the scene, with a ring there.
    m_arc_live_o     = o;
    m_arc_live_v     = v;
    m_arc_live_g     = fit.g;
    m_arc_live_frame = m_frame.load();
    if (!m_arc_drawn) fc5::aim::mark(2);   // a new trajectory: no hit of an earlier one
    int         nres  = 0;
    fc5::aim::hit_since_mark(2, nres);
    const float t_hit = (m_arc_stop.load() && nres > 0) ? fc5::aim::last_hit(2) : -1.0f;
    for (int i = 1; i <= 60; ++i) {
        const float     tt = 0.04f * float(i);
        if (t_hit > 0.0f && tt > t_hit) break;
        const glm::vec3 p  = o + v * tt + glm::vec3{ 0.0f, 0.0f, -0.5f * fit.g } * tt * tt;
        if (p.z < o.z - 30.0f) break;
        if (i % 2) continue;   // dotted
        if (put(p, 1.0f, light)) drawn++;
    }
    if (t_hit > 0.0f && drawn > 0) {
        // Where it lands: a ring of dots 1.2 m across (0.8 m was hard to see 18 m away, simulator 26 Oct) on the level there, and its centre.
        const glm::vec3 c = o + v * t_hit + glm::vec3{ 0.0f, 0.0f, -0.5f * fit.g } * t_hit * t_hit;
        for (int k = 0; k < 16; ++k) {
            const float a = glm::two_pi<float>() * float(k) / 16.0f;
            put(c + glm::vec3{ std::cos(a), std::sin(a), 0.0f } * 0.6f, 1.2f, land);
        }
        put(c, 2.0f, land);
        if (m_arc_hit_said++ % 300 == 0) spdlog::info("[ThrowArc] trajectory stops where it meets the scene: {:.2f} s, {:.1f} m away", t_hit, glm::length(c - o));
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
    melee_hit_frame();   // meleehit (FarCry5Melee.cpp)
    // One eye's presents only: from one eye's present to the other's the hand moved 4-8 cm with the controller still (7-16 m/s,
    // simulator 25 Oct), so the speeds of 24 Oct's swings were partly that.
    if (m_frame.load() & 1) return;
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
            // Where the swing started (the hand before it went over 1 m/s), so a fast swing that is already at the wall when it is seen
            // is still caught (Homard's headset, 24 Oct: the 12 swings of 7.9-15.7 m/s all "nothing within 0.8 m").
            const bool moving = m_melee_v > 1.0f || m_melee_pending;
            if (moving && !m_melee_dir_ok) m_melee_rel0 = m_melee_rel;
            m_melee_dir_ok = moving;
            m_melee_travel = m_melee_dir_ok ? glm::length(rel - m_melee_rel0) : 0.0f;
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
                // The ray from the swing's start reaches 0.8 m past the hand's travel; the gun itself within 0.4 m of the scene counts too.
                const float hit   = std::max(m_melee_pending_barrel, fc5::aim::last_hit());
                const float reach_m = 0.8f + m_melee_travel;
                const bool  by_ray  = mhit > 0.0f && mhit <= reach_m;
                const bool  by_gun  = hit > 0.0f && hit <= 0.4f;
                const bool  reach   = by_ray || by_gun;
                m_melee_swings++;
                if (reach) m_melee_hits++;
                spdlog::info("[Melee] swing {:.1f} m/s (threshold {:.1f}) toward ({:.2f}, {:.2f}, {:.2f}), the hand {:.2f} m from where it started; along the swing from there ({} marches): {}; the barrel ray: {}: {} ({} of {} swings would have)",
                             m_melee_pending_v, m_melee_speed.load(), m_melee_dir.x, m_melee_dir.y, m_melee_dir.z, m_melee_travel, n,
                             by_ray ? fmt::format("the scene {:.2f} m from the start (reach {:.2f})", mhit, reach_m) : fmt::format("nothing within {:.2f} m", reach_m),
                             hit > 0.0f ? fmt::format("the scene {:.2f} m ahead{}", hit, by_gun ? " (the gun at it)" : "") : std::string{ "nothing ahead" },
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
    fc5::throwprobe::arm(hand);
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
    const auto r = fc5::throwprobe::fit(m_arc_rel_head);
    if (!r.ok) {
        spdlog::info("[ThrowArc] measured throw: no object flew from the hand ({} entities followed, {} not starting at the hand or flying off the look, the furthest kept one moved {:.1f} m){}", r.tracked, r.rejected, r.dist, r.path);
        return;
    }
    const float sp    = glm::length(r.v0);
    const float elev  = glm::degrees(std::asin(std::clamp(r.v0.z / std::max(sp, 1e-3f), -1.0f, 1.0f)));
    const glm::vec2 hh{ m_arc_rel_head.x, m_arc_rel_head.y }, vh{ r.v0.x, r.v0.y };
    const float side = (glm::length(hh) > 1e-3f && glm::length(vh) > 1e-3f) ? glm::degrees(std::atan2(hh.x * vh.y - hh.y * vh.x, glm::dot(hh, vh))) : 0.0f;
    // The last good value per throwable stays unless this one is sane and within 60 deg (sideways) of the look.
    const bool  sane = sp > 3.0f && sp < 80.0f && r.g > 2.0f && r.g < 30.0f && std::abs(side) <= 60.0f;
    spdlog::info("[ThrowArc] measured throw (throwable {}): launch {:.3f} s after the release, {:.1f} m/s, {:.1f} deg above level ({:.1f} above the head's direction, {:.1f} deg to its left), "
                 "gravity {:.2f} m/s2, flew {:.1f} m; {} samples fitted, {} entities followed{}; path from the hand:{}",
                 m_arc_rel_sig, r.t_launch, sp, elev, elev - m_arc_rel_pitch, side, r.g, r.dist, r.samples, r.tracked, sane ? "" : " (not taken: out of range)", r.path);
    // throwv2: measured against the trajectory, so the lift is the one used plus the miss.
    if (sane && m_arc_rel_sig) m_arc_fit[m_arc_rel_sig] = ArcFit{ sp, (m_arc_rel_v2 ? m_arc_rel_up_used : 0.0f) + elev - m_arc_rel_pitch, r.g };
}