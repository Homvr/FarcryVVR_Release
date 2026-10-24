// vehfovwatch: a read-only probe for where the game's vehicle view angle comes from (Homard 19 Oct: the parachute is drawn
// 65 deg wide; nudging candidate floats to find it crashed the game in a plane). With the view angle steady and narrower
// than on foot, the floats equal to it are found (near the camera, then anywhere in memory), then put under CPU write
// watches (debug registers, all threads) 4 at a time for 15 frames each. fc5::camscan::unwatch() logs which code wrote
// each one, from which thread, with its call chain. Nothing in the game's memory is changed. Once per vehicle entry.
#include "FarCry5Stereo.h"
#include "FarCry5CamScan.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

void FarCry5Stereo::veh_fov_watch_tick()
{
    auto&      s  = m_fov_scan;
    const bool in = m_vw_on.load() && m_motion.load() && m_in_vehicle.load() && !m_menu_screen_on && m_player_views_last_frame != 0;
    if (!in) {
        if (m_vw_state == 3) {
            fc5::camscan::unwatch();
            spdlog::info("[VehFovWatch] stopped: left the vehicle");
        }
        if (m_vw_state == 1 || m_vw_state == 2) {
            fov_scan_abort();
            s.watch_only = false;
        }
        m_vw_state = m_vw_pass = m_vw_stable = 0;
        m_vw_done  = false;
        m_vw_cands.clear();
        return;
    }
    if (m_vw_done) return;
    if (m_vw_state == 0) {
        if (s.state != 0 || m_veh_settle.load() > 0) return;   // another scan is running, or getting in / changing seats
        if (m_game_hfov.load() >= 110.0f) return;              // as wide as on foot: nothing to look for
        const float fov = m_game_fov_deg;
        m_vw_stable     = std::abs(fov - m_vw_last) < 0.01f ? m_vw_stable + 1 : 0;
        m_vw_last       = fov;
        if (m_vw_stable < 90) return;
        m_vw_stable = 0;
        m_vw_cands.clear();
        m_vw_pass   = 0;
        m_vw_state  = 1;
    }
    if (m_vw_state == 1) {
        // Candidate scans (memory reads only): 0 degrees near the camera, 1 radians near the camera, 2 degrees anywhere.
        if (m_vw_pass >= 3) {
            m_vw_state = 3;
            m_vw_idx   = 0;
            m_vw_round = 0;
            m_vw_frames = 0;
            spdlog::info("[VehFovWatch] {} candidates to watch for the view angle {:.2f} deg (game view {:.1f} deg wide)", m_vw_cands.size(), m_vw_last, m_game_hfov.load());
            if (m_vw_cands.empty()) { m_vw_done = true; m_vw_state = 0; }
            return;
        }
        s.watch_only     = true;
        s.any            = m_vw_pass == 2;
        s.found          = nullptr;
        m_fov_scan_value = m_vw_pass == 1 ? glm::radians(m_game_fov_deg) : 0.0f;
        s.state          = 1;
        m_vw_state       = 2;
        spdlog::info("[VehFovWatch] scan {} ({}, {}) for {:.2f} deg", m_vw_pass + 1, m_vw_pass == 1 ? "radians" : "degrees", s.any ? "anywhere" : "near the camera", m_game_fov_deg);
        return;
    }
    if (m_vw_state == 2) {
        if (s.state != 0) return;
        // Up to 64 per scan (the near-camera ones first), no duplicates.
        size_t added = 0;
        for (auto* f : s.cands) {
            if (added >= 64) break;
            if (std::find(m_vw_cands.begin(), m_vw_cands.end(), f) != m_vw_cands.end()) continue;
            m_vw_cands.push_back(f);
            added++;
        }
        spdlog::info("[VehFovWatch] scan {}: {} candidates, {} kept for watching", m_vw_pass + 1, s.cands.size(), added);
        s.cands.clear();
        s.watch_only = false;
        s.any        = false;
        m_vw_pass++;
        m_vw_state = 1;
        return;
    }
    if (m_vw_state == 3) {
        if (m_vw_frames == 0) {
            std::vector<void*> addrs;
            std::string        list;
            for (size_t i = m_vw_idx; i < std::min(m_vw_idx + 4, m_vw_cands.size()); ++i) {
                addrs.push_back(m_vw_cands[i]);
                list += fmt::format(" {}", (void*)m_vw_cands[i]);
            }
            m_vw_round++;
            spdlog::info("[VehFovWatch] round {}: watching writes to{}", m_vw_round, list);
            fc5::camscan::watch_addresses(addrs, GetCurrentThreadId(), false);
        }
        if (++m_vw_frames < 15) return;
        fc5::camscan::unwatch();   // logs each writer: "[CamScan] watch N (addr) written Kx by ... at module+offset <- callers"
        m_vw_frames = 0;
        m_vw_idx += 4;
        if (m_vw_idx >= m_vw_cands.size()) {
            spdlog::info("[VehFovWatch] done: {} candidates watched in {} rounds", m_vw_cands.size(), m_vw_round);
            m_vw_done  = true;
            m_vw_state = 0;
            m_vw_cands.clear();
        }
    }
}
