// Walk bob (Homard 23 Oct 10:15: "remove the game's up-and-down walk bob too"). The eyes stand on the game camera, which sits
// on the player's neck and rises and falls with each step while walking, running or sprinting. viewpivot (FarCry5ViewPivot.cpp)
// already took the camera's sideways neck sway out; the height bob stayed.
//  - walkbob off|on (default off = no bob, saved; F11 Motion control > View "No up-and-down bob while walking"): on foot the
//    eyes' height is the body root's own height plus a level over it (the stance), not the game camera's. The first version
//    (319c9aa) took the camera less a prediction of its bob; each update's prediction error came through as a small jump
//    (Homard 23 Oct 15:40: "a bit worse than before because its very little jump"). Now:
//      the level: standing still it follows the camera's height over the root (h, seen once per game update where the body
//        root is drawn), so the eyes are where the camera is; moving (over 0.5 m/s) it is held; a stance change while moving
//        (crouch, stand up, slide: h past the level by bobjump, 0.15 m) follows h until h swings by less than that over
//        0.25 s, then holds the median of that time. The eyes ease onto the level, critically damped over bobease (0.2 s).
//      the root: the last one seen (one update back) plus the camera's rise or fall since then past what a step's bob can
//        do (bobrootlead, 2 m/s: a jump or fall shows at once), smoothed for the root's own jitter (one-euro, bobrootcut 3 Hz
//        at rest, quicker as it moves).
//    On foot only (not in vehicles, cutscenes, menus); not while the steady view holds its own base. Eased in and out over
//    0.15 s. The drawn gun and hands move with the eyes (as viewpivot); the shots keep the game camera.
//  - bobjump M, bobease S, bobrootlead V, bobrootcut F (test).
//  - boblog N (test): N lines, one per body root seen (root, camera and eyes over it, level, root used, mode), and while it
//    runs one [BobPair] line per stereo pair (the camera's and the eyes' height).
#include "FarCry5Stereo.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

bool FarCry5Stereo::bob_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    // walkbob off|on: part of bodyview since 29 Oct (FarCry5ViewPivot.cpp), a test command, no longer saved; off = the game's bob taken out.
    // The test knobs bobjump, bobease, bobrootlead and bobrootcut are retired (their defaults kept).
    if (cmd == "walkbob") { m_no_bob = !on; return true; }
    if (cmd == "boblog") { m_bob_log = arg.empty() ? 600 : std::max(0, std::atoi(arg.c_str())); return true; }         // test
    return false;
}

// The body root seen (body_hide), with the camera's offset from it in the same update: the height over the root and its
// history, and the log line comparing the eyes' height in that update with the root.
void FarCry5Stereo::bob_note_root(int frame, const glm::vec3& root, const glm::vec3& camoff)
{
    const auto now  = std::chrono::steady_clock::now();
    // bobsnapguard (downedfoot): a sample 1 m off the last kept one is held back for 0.5 s (no new root meanwhile: the bob eases off once
    // it is 6 frames old), dropped if the root comes back (13:30:42-13:31:04 under the chute: the root 0.2-0.7 m and 1.85 m under the
    // camera in turn, ~30 snaps of 1.6 m), kept if it stays (knocked down, getting up).
    if (now - m_bob_root_t > std::chrono::seconds(1)) m_bsg_h_kept_ok = false;   // no root for a while: the next one starts afresh
    if (m_downed_foot.load() && m_bsg_h_kept_ok && std::abs(camoff.z - m_bsg_h_kept) > 1.0f) {
        if (!m_bsg_pend) { m_bsg_pend = true; m_bsg_pend_t0 = now; }
        if (now - m_bsg_pend_t0 < std::chrono::milliseconds(500)) return;
        m_bsg_pend = false;   // stayed for 0.5 s: a real change
    } else if (m_bsg_pend) {
        m_bsg_pend = false;   // back within 0.5 s: a flip
        m_bsg_flips++;
        m_bsg_last_t = now;
        if (m_bsg_episode == 0) m_bsg_episode = 1;
    }
    m_bsg_h_kept    = camoff.z;
    m_bsg_h_kept_ok = true;
    m_bob_h         = camoff.z;
    m_bob_h_frame   = frame;
    m_bob_root_z    = root.z;
    m_bob_rootcam_z = root.z + camoff.z;
    m_bob_root_t    = now;
    m_bob_h_good    = std::sqrt(camoff.x * camoff.x + camoff.y * camoff.y) < 0.4f && camoff.z > 0.2f && camoff.z < 2.5f;
    m_bob_hist[m_bob_hist_n++ % kBobHist] = { std::chrono::duration<float>(now - m_bob_t0).count(), camoff.z };
    if (m_bob_log.load() <= 0) return;
    m_bob_log--;
    const auto& b = m_bob_base_at[frame & 7];
    static const char* kMode[] = { "standing", "moving", "change" };
    spdlog::info("[BobLog] frame {} root {:.4f} | cam over root {:.4f} | eyes over root {} | level {:.4f} target {:.4f} | root used {:.4f} lead {:+.4f} | speed {:.2f} {} | bob {}",
                 frame, root.z, camoff.z, b.frame == frame ? fmt::format("{:.4f}", b.z - root.z) : std::string("-"), m_bob_lvl, m_bob_target, m_bob_r, m_bob_rl,
                 m_bob_speed, kMode[m_bob_mode], m_bob_applied ? "removed" : (m_no_bob.load() ? "kept (not on foot)" : "kept (walkbob on)"));
}

// How far h swung over the roots since `since` (seconds on m_bob_t0's clock), and its median there; -1 if under 3 roots.
float FarCry5Stereo::bob_hist_range(float since, float& median) const
{
    float v[kBobHist];
    int   n = 0;
    for (int i = m_bob_hist_n - 1; i >= 0 && i >= m_bob_hist_n - kBobHist; --i) {
        const auto& e = m_bob_hist[i % kBobHist];
        if (e.t < since) break;
        v[n++] = e.h;
    }
    if (n < 3) { median = m_bob_h; return -1.0f; }
    std::sort(v, v + n);
    median = n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    return v[n - 1] - v[0];
}

// The eyes' height shift from the game camera (zero when the bob is kept), once per stereo pair; computed every pair so the
// filters keep up while another base is used.
float FarCry5Stereo::walk_bob_shift(int frame, const glm::vec3& cam)
{
    const int pair = afw_mode() ? frame : (frame & ~1);
    if (pair == m_bob_pair) return m_bob_applied ? m_bob_out : 0.0f;
    const auto  now = std::chrono::steady_clock::now();
    const float dt  = m_bob_pair >= 0 ? std::chrono::duration<float>(now - m_bob_t).count() : 1.0f;
    m_bob_pair = pair;
    m_bob_t    = now;
    const bool gap = dt > 0.25f;
    // Horizontal speed (the camera's, eased over ~0.2 s).
    if (!gap && dt > 1e-4f) {
        const float v = glm::length(glm::vec2{ cam.x - m_bob_cam.x, cam.y - m_bob_cam.y }) / dt;
        if (v < 30.0f) m_bob_speed += (v - m_bob_speed) * (1.0f - std::exp(-dt / 0.2f));
    }
    m_bob_cam = cam;
    const bool est = m_mo.valid && m_bob_h_good && m_bob_h_frame >= 0 && pair - m_bob_h_frame <= 6 && pair >= m_bob_h_frame;
    const bool ok  = est && m_no_bob.load() && !m_in_vehicle.load() && !m_cine_on && !m_menu_screen_on;
    if (est) {
        if (!m_bob_init || gap) {
            m_bob_lvl = m_bob_target = m_bob_h;
            m_bob_lvl_v = 0.0f;
            m_bob_r     = m_bob_root_z;
            m_bob_rd    = 0.0f;
            m_bob_mode  = 0;
            m_bob_init  = true;
        }
        // The level, once per new root.
        if (m_bob_h_frame != m_bob_h_used) {
            m_bob_h_used  = m_bob_h_frame;
            const float t = std::chrono::duration<float>(m_bob_root_t - m_bob_t0).count();
            if (!ok || m_bob_speed <= 0.5f) {
                m_bob_mode   = 0;
                m_bob_target = m_bob_h;
            } else {
                if (m_bob_mode == 0) m_bob_mode = 1;   // held where it stood
                if (m_bob_mode == 1 && std::abs(m_bob_h - m_bob_target) > m_bob_jump.load()) { m_bob_mode = 2; m_bob_chg_t = t; }
                if (m_bob_mode == 2) {
                    m_bob_target      = m_bob_h;
                    float       med   = 0.0f;
                    const float range = bob_hist_range(t - 0.25f, med);
                    if (t - m_bob_chg_t >= 0.25f && range >= 0.0f && range < m_bob_jump.load()) { m_bob_mode = 1; m_bob_target = med; }
                }
            }
            // downedfoot: the camera over 0.5 m from the eyes' level (knocked down, getting up; the game animates a crouch, so the level keeps up with it): the eyes follow at once.
            // bobsnapguard: never under a chute, in a fall, in a vehicle or with downview easing the view itself; one snap, then none until
            // the level has stayed within 0.5 m for 1 s (the rest eased as usual). One line per episode, its counts when it ends.
            if (ok && m_downed_foot.load() && std::abs(m_bob_h - m_bob_lvl) > 0.5f) {
                const bool held = m_dv_active || m_falling || m_co_suit || m_chute_vehicle || m_in_vehicle.load();
                if (held || !m_bsg_armed) {
                    if (m_bsg_episode == 0)
                        spdlog::info("[Bob] downedfoot: the camera {} {:.2f} m over the body: eased, not snapped ({})", m_bob_h < m_bob_lvl ? "dropped" : "rose",
                                     std::abs(m_bob_h - m_bob_lvl), held ? (m_dv_active ? "downview" : m_in_vehicle.load() ? "in a vehicle" : "a fall or a parachute") : "a snap under 1 s ago");
                    m_bsg_blocked++;
                    m_bsg_episode = std::max(m_bsg_episode, 1);
                } else {
                    if (m_bsg_episode == 0 && m_bob_drop_snaps++ < 50)
                        spdlog::info("[Bob] downedfoot: the camera {} {:.2f} m over the body: the eyes follow at once (not eased)", m_bob_h < m_bob_lvl ? "dropped" : "rose",
                                     std::abs(m_bob_h - m_bob_lvl));
                    m_bob_mode   = 0;
                    m_bob_target = m_bob_lvl = m_bob_h;
                    m_bob_lvl_v  = 0.0f;
                    m_bsg_armed  = false;
                    m_bsg_ref_lvl  = m_bob_h;
                    m_bsg_quiet_t0 = now;
                    m_bsg_snaps++;
                    m_bsg_episode = 1;
                }
                m_bsg_last_t = now;
            }
            if (!m_bsg_armed) {
                if (std::abs(m_bob_h - m_bsg_ref_lvl) > 0.5f) { m_bsg_ref_lvl = m_bob_h; m_bsg_quiet_t0 = now; }
                else if (now - m_bsg_quiet_t0 >= std::chrono::seconds(1)) m_bsg_armed = true;
            }
            if (m_bsg_episode > 0 && m_bsg_armed && now - m_bsg_last_t >= std::chrono::seconds(1)) {
                if (m_bsg_snaps > 1 || m_bsg_blocked > 0 || m_bsg_flips > 0)
                    spdlog::info("[Bob] downedfoot: bobsnapguard episode over: {} snap(s), {} more eased, {} root sample(s) 1 m off held back and dropped",
                                 m_bsg_snaps, m_bsg_blocked, m_bsg_flips);
                m_bsg_episode = m_bsg_blocked = m_bsg_flips = m_bsg_snaps = 0;
            }
        }
        // The root now: the last one seen, plus the camera's rise or fall since its update past what the bob can do in that
        // time (a jump, a fall: at once); not in a stance change (that is h moving).
        const float age = std::max(0.0f, std::chrono::duration<float>(now - m_bob_root_t).count());
        const float x = cam.z - m_bob_rootcam_z, lim = m_bob_rlead.load() * age;
        m_bob_rl      = m_bob_mode == 2 || m_bob_rlead.load() <= 0.0f ? 0.0f : std::clamp(x - std::clamp(x, -lim, lim), -2.0f, 2.0f);
        // rootsteady: that root's height from the last three roots (uphill, samples with long and short steps in turn made the
        // eyes' height step long and short in turn too).
        glm::vec3   rs{ 0.0f, 0.0f, m_bob_root_z };
        if (!root_steady_at(float(m_bob_h_frame), rs)) rs.z = m_bob_root_z;
        const float rt = rs.z + m_bob_rl;
        // One-euro smoothing of the root's height (the cutoff rises with its rate: little lag in jumps and falls).
        const float h  = std::max(dt, 1e-4f);
        auto alpha     = [h](float hz) { return 1.0f / (1.0f + 1.0f / (6.2831853f * hz * h)); };
        if (gap) m_bob_r = rt;
        m_bob_rd += alpha(8.0f) * ((rt - m_bob_r) / h - m_bob_rd);
        m_bob_r += alpha(m_bob_rcut.load() + 10.0f * std::abs(m_bob_rd)) * (rt - m_bob_r);
        // The eyes onto the level: critically damped (exact step), settled in about bobease.
        const float w  = 4.75f / m_bob_ease.load();
        const float e0 = m_bob_lvl - m_bob_target, v0 = m_bob_lvl_v;
        const float ex = std::exp(-w * h);
        m_bob_lvl      = m_bob_target + (e0 + (v0 + w * e0) * h) * ex;
        m_bob_lvl_v    = (v0 - w * (v0 + w * e0) * h) * ex;
        m_bob_shift    = m_bob_r + m_bob_lvl - cam.z;
    } else {
        m_bob_init = false;
    }
    // Applied on foot, eased in and out over ~0.15 s (vehicle, cutscene, root lost, switched off).
    m_bob_w += ((ok ? 1.0f : 0.0f) - m_bob_w) * (1.0f - std::exp(-std::min(dt, 1.0f) / 0.15f));
    if (est) m_bob_out = m_bob_w * m_bob_shift;
    else m_bob_out *= std::exp(-std::min(dt, 1.0f) / 0.15f);
    m_bob_applied = ok || std::abs(m_bob_out) > 1e-4f;
    if (m_bob_log.load() > 0)
        spdlog::info("[BobPair] frame {} cam {:.4f} eyes {:.4f} | root used {:.4f} level {:.4f} | shift {:+.4f} w {:.2f} mode {}", frame, cam.z,
                     cam.z + (m_bob_applied ? m_bob_out : 0.0f), m_bob_r, m_bob_lvl, m_bob_shift, m_bob_w, m_bob_mode);
    return m_bob_applied ? m_bob_out : 0.0f;
}

void FarCry5Stereo::bob_note_base(int frame, float z)
{
    auto& b = m_bob_base_at[frame & 7];
    b.frame = frame;
    b.z     = z;
}
