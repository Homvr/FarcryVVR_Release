#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <ModSettings.h>
#include <aer/ConstantsPool.h>
#include <experimental/DebugUtils.h>
#include <imgui.h>
#include <json.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"
#include "Fc5Boot.hpp"

#include "OpenXR.hpp"
#include "mods/VR.hpp"

using namespace nlohmann;

namespace runtimes {
VRRuntime::Error OpenXR::synchronize_frame(int frame) {
    SCOPE_PROFILER();
    std::scoped_lock _{sync_mtx};

    // cant sync frame between begin and endframe
    if (!this->session_ready || this->frame_began) {
        return VRRuntime::Error::UNSPECIFIED;
    }

    m_last_synchronized_frame = frame;

    if (this->frame_synced) {
        return VRRuntime::Error::SUCCESS;
    }

    this->begin_profile();

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    {
        // Far Cry 5 framelock (29 Oct, replaces xrhalf; Homard 29 Oct "the movement are too juddery and janky for vr"): every
        // picture is shown for the same number of refreshes. His log: AFW made 74-78 pictures a second for a 90 Hz headset (GPU
        // ~12.5 ms each), Stereo ~50 pairs (~21 ms each), so the headset showed each picture for one refresh or two, unevenly,
        // and the world stalled and jumped while moving. auto: two refreshes per picture at 85 Hz or more once over 10 % of the
        // last 2 s of pictures took longer than a refresh (their work, from the last xrWaitFrame's return to this call), back to
        // one under 3 % for 4 s; one below 85 Hz (two would be 40 or 36 a second). The period is the runtime's own.
        // Back-off (Homard 29 Oct 12:49 log: auto changed 19 times in 7 minutes, every 4-10 s, as at 45 a second the GPU has room and
        // the pictures come in early): each time it has to halve again within 20 s of going back to one, the calm time needed to go back
        // doubles (4, 8, 16, 32, 60 s); a minute at one refresh resets it.
        // Mode switch (Homard 29 Oct 13:54, "lost a lot of FPS and ... trouble getting them back up": F11 switched Stereo to AFW at
        // 14:50:28 and auto stayed at two refreshes, its wait grown to a minute in Stereo): Stereo pairs at 85 Hz or more are always two
        // (a pair is ~20-22 ms of GPU, never inside one refresh); a switch between Stereo and AFW starts auto afresh (AFW at one refresh,
        // the back-off at 4 s).
        static double period_ms = 0.0;
        if (const auto p = this->pipeline_state.frame_state.predictedDisplayPeriod; p > 0) period_ms = double(p) / 1e6;
        const auto last_return = s_fc5_last_wait_return;
        const auto t0          = std::chrono::steady_clock::now();
        if (period_ms > 5.0 && period_ms < 20.0 && last_return.time_since_epoch().count() != 0) {
            static uint32_t frames = 0, over = 0, calm = 0, hold = 2;   // hold: calm 2 s windows needed to go back to one
            static auto     window = t0;
            static std::chrono::steady_clock::time_point back{};      // when it last went back to one
            static int      pairs_was = -1;
            const int       pairs     = s_fc5_lock_pairs.load();
            if (pairs != pairs_was && s_fc5_frame_lock.load() < 0 && pairs_was >= 0 && pairs >= 0) {
                const int n = pairs == 1 && period_ms <= 11.8 ? 2 : 1;
                frames = over = calm = 0;
                hold   = 2;
                back   = {};
                window = t0;
                s_fc5_lock_n = n;
                spdlog::info("[VR] framelock auto: {} now, starting afresh: {} refresh{} per picture ({:.0f} a second)", pairs ? "Stereo pairs" : "one eye a frame (AFW)",
                             n, n == 1 ? "" : "es", 1000.0 / (n * period_ms));
            }
            pairs_was = pairs;
            const double    work   = std::chrono::duration<double, std::milli>(t0 - last_return).count();
            if (work < 250.0) {
                ++frames;
                if (work > 0.95 * period_ms) ++over;
            }
            if (t0 - window >= std::chrono::seconds(2)) {
                const int mode  = s_fc5_frame_lock.load();
                const int pct   = frames ? int(100.0 * over / frames + 0.5) : 0;
                const int was   = s_fc5_lock_n.load();
                int       n     = was;
                if (mode >= 0) n = std::clamp(mode, 1, 2);
                else if (period_ms > 11.8) n = 1;
                else if (pairs == 1) n = 2;   // Stereo pairs at 85 Hz or more
                else if (n == 1) {
                    if (pct > 10) {
                        n = 2;
                        if (t0 - back < std::chrono::seconds(20)) hold = std::min(hold * 2, 30u);
                    } else if (t0 - back > std::chrono::seconds(60)) hold = 2;
                } else {
                    calm = pct < 3 ? calm + 1 : 0;
                    if (calm >= hold) {
                        n    = 1;
                        back = t0;
                    }
                }
                if (n != 2) calm = 0;
                s_fc5_lock_n        = n;
                s_fc5_lock_over_pct = pct;
                if (n != was)
                    spdlog::info("[VR] framelock {}: {} refresh{} per picture ({:.0f} a second); {}% of the last {} pictures took longer than a refresh ({:.2f} ms); "
                                 "back to one after {} s under 3 %",
                                 mode < 0 ? "auto" : mode == 0 ? "off" : "fixed", n, n == 1 ? "" : "es", 1000.0 / (n * period_ms), pct, frames, period_ms, 2 * hold);
                frames = over = 0;
                window = t0;
            }
        }
        if (s_fc5_lock_n.load() == 2 && period_ms > 5.0 && period_ms < 20.0 && last_return.time_since_epoch().count() != 0) {
            const auto target = last_return + std::chrono::microseconds(int64_t((2.0 * period_ms - 2.0) * 1000.0));
            if (target > t0 && target - t0 < std::chrono::milliseconds(40)) {
                while (std::chrono::steady_clock::now() + std::chrono::milliseconds(2) < target) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                while (std::chrono::steady_clock::now() < target) std::this_thread::yield();
                s_fc5_half_waits++;
                s_fc5_half_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            }
        }
    }
    this->pipeline_state.frame_state = {XR_TYPE_FRAME_STATE};
    const auto wait_t0 = std::chrono::steady_clock::now();
    auto result = xrWaitFrame(this->session, &frame_wait_info, &this->pipeline_state.frame_state);
    s_fc5_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_t0).count();
    s_fc5_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_t0).count();
    s_fc5_last_wait_return = std::chrono::steady_clock::now();

    this->end_profile("xrWaitFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrWaitFrame failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    } else {
        this->got_first_sync = true;
        this->frame_synced = true;
        // Far Cry 5 pacing census (black-flicker hunt): display times handed out by xrWaitFrame.
        static XrTime   last_display = 0;
        static uint32_t waits = 0, repeats = 0, backwards = 0, no_render = 0, skipped_periods = 0;
        static double   min_ms = 1e9, max_ms = 0.0;
        static auto     last_log = std::chrono::steady_clock::now();
        const auto&     fs = this->pipeline_state.frame_state;
        ++waits;
        if (fs.shouldRender != XR_TRUE) ++no_render;
        if (last_display != 0) {
            const double d = double(fs.predictedDisplayTime - last_display) / 1e6;
            if (fs.predictedDisplayTime == last_display) ++repeats;
            else if (fs.predictedDisplayTime < last_display) ++backwards;
            else {
                min_ms = std::min(min_ms, d);
                max_ms = std::max(max_ms, d);
                if (fs.predictedDisplayPeriod > 0 && d > 1.5 * double(fs.predictedDisplayPeriod) / 1e6) ++skipped_periods;
            }
        }
        last_display = fs.predictedDisplayTime;
        if (std::chrono::steady_clock::now() - last_log > std::chrono::seconds(5)) {
            last_log = std::chrono::steady_clock::now();
            spdlog::info("[VR] pacing: {} waits, display period {:.2f} ms, step min {:.2f} max {:.2f} ms, {} steps over 1.5 periods, {} repeated, {} backwards, {} shouldRender=false; CPU ms per XR frame: xrWaitFrame {:.2f}, xrEndFrame {:.2f}, eye copies {:.2f}; framelock {} ({} per picture, {}% over a refresh)",
                         waits, double(fs.predictedDisplayPeriod) / 1e6, min_ms, max_ms, skipped_periods, repeats, backwards, no_render, s_fc5_wait_ms / std::max(1u, waits),
                         s_fc5_end_ms / std::max(1u, waits), s_fc5_copy_ms / std::max(1u, waits), s_fc5_frame_lock.load() < 0 ? "auto" : s_fc5_frame_lock.load() == 0 ? "off" : "fixed",
                         s_fc5_lock_n.load(), s_fc5_lock_over_pct.load());
            s_fc5_wait_ms = s_fc5_end_ms = s_fc5_copy_ms = 0.0;
            waits = repeats = backwards = no_render = skipped_periods = 0;
            min_ms = 1e9;
            max_ms = 0.0;
        }
    }

    return VRRuntime::Error::SUCCESS;
}

// Far Cry 5 test tool: scripted head motion for the Meta XR Simulator, whose head cannot be moved from
// a script. bin/fc5vr_headpath.txt, one axis per line: "<axis> <amplitude> <period s> [phase deg]",
// axes yaw/pitch/roll (degrees) and x/y/z (metres, head right/up/back); each axis follows
// amplitude * sin(2 pi t / period + phase) with t the predicted display time. Applied to the head and
// eye poses where they are read from the runtime, so the eyes, the aim loop (game camera) and the
// submitted layers all see the same motion. Delete the file (or write zero amplitudes) to stop.
namespace {
struct Fc5HeadPath {
    struct Axis {
        float amp{0.0f}, period{2.0f}, phase{0.0f};
    };
    Axis axes[6]{};
    bool active{false};
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point checked{};
    std::chrono::steady_clock::time_point logged{};
};
Fc5HeadPath g_fc5_head_path{};

// Far Cry 5 test tool: another headset's view in the Meta XR Simulator, which only offers Quest views.
// bin/fc5vr_viewtest.txt: "fov <outer> <inner> <up> <down>" (degrees from the head's forward, positive; the left eye's
// outer side is its left, the right eye mirrored) and/or "cant <deg>" (each eye's display turned outward, as on Pimax
// or Varjo headsets; the eye's angles then measured from its turned forward). Applied to both eyes' views where they
// are read from the runtime, so the mod renders and submits those views; the compositor shows them on the simulator's
// own display (parts past it are not seen; the mod's eye image captures show them). Delete the file to stop.
struct Fc5ViewTest {
    bool active{false}, fov{false};
    float outer{0.0f}, inner{0.0f}, up{0.0f}, down{0.0f}, cant{0.0f};
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point checked{};
};
Fc5ViewTest g_fc5_view_test{};

void fc5_reload_view_test() {
    auto& vt = g_fc5_view_test;
    const auto now = std::chrono::steady_clock::now();
    if (now - vt.checked < std::chrono::seconds(1)) {
        return;
    }
    vt.checked = now;
    std::error_code ec;
    const std::filesystem::path path{"fc5vr_viewtest.txt"};
    if (!std::filesystem::exists(path, ec)) {
        if (vt.active) {
            spdlog::info("[VR] view test off (fc5vr_viewtest.txt removed): the runtime's own views");
        }
        vt = Fc5ViewTest{};
        vt.checked = now;
        return;
    }
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec || stamp == vt.stamp) {
        return;
    }
    Fc5ViewTest n{};
    n.stamp = stamp;
    n.checked = now;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls{line};
        std::string name;
        ls >> name;
        if (name == "fov" && (ls >> n.outer >> n.inner >> n.up >> n.down)) {
            n.fov = n.outer > 1.0f && n.inner > 1.0f && n.up > 1.0f && n.down > 1.0f && n.outer < 85.0f && n.inner < 85.0f && n.up < 85.0f && n.down < 85.0f;
        }
        if (name == "cant") {
            ls >> n.cant;
            n.cant = std::clamp(n.cant, -30.0f, 30.0f);
        }
    }
    n.active = n.fov || n.cant != 0.0f;
    vt = n;
    spdlog::info("[VR] view test {}: field of view {}, eyes turned outward {:.1f} deg", vt.active ? "on" : "off",
                 vt.fov ? fmt::format("outer {:.1f} inner {:.1f} up {:.1f} down {:.1f} deg", vt.outer, vt.inner, vt.up, vt.down) : std::string{"the runtime's"}, vt.cant);
}

void fc5_apply_view_test(std::vector<XrView>& views) {
    const auto& vt = g_fc5_view_test;
    if (!vt.active || views.size() < 2) {
        return;
    }
    for (int e = 0; e < 2; ++e) {
        auto& f = views[e].fov;
        // Head-frame angles (radians, left and down negative), then measured from the eye's turned forward.
        float l = f.angleLeft, r = f.angleRight, u = f.angleUp, d = f.angleDown;
        if (vt.fov) {
            l = -glm::radians(e == 0 ? vt.outer : vt.inner);
            r = glm::radians(e == 0 ? vt.inner : vt.outer);
            u = glm::radians(vt.up);
            d = -glm::radians(vt.down);
        }
        const float turn = glm::radians(vt.cant) * (e == 0 ? 1.0f : -1.0f);   // + turns left (about +Y)
        f.angleLeft = l + turn;
        f.angleRight = r + turn;
        f.angleUp = u;
        f.angleDown = d;
        if (turn != 0.0f) {
            auto& o = views[e].pose.orientation;
            o = runtimes::OpenXR::to_openxr(glm::normalize(runtimes::OpenXR::to_glm(o) * glm::angleAxis(turn, glm::vec3{0, 1, 0})));
        }
    }
}

void fc5_reload_head_path() {
    auto& hp = g_fc5_head_path;
    const auto now = std::chrono::steady_clock::now();
    if (now - hp.checked < std::chrono::seconds(1)) {
        return;
    }
    hp.checked = now;
    std::error_code ec;
    const std::filesystem::path path{"fc5vr_headpath.txt"};
    if (!std::filesystem::exists(path, ec)) {
        if (hp.active) {
            spdlog::info("[VR] synthetic head motion off (fc5vr_headpath.txt removed)");
        }
        hp.active = false;
        hp.stamp = {};
        return;
    }
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec || stamp == hp.stamp) {
        return;
    }
    hp.stamp = stamp;
    static const char* names[6]{"yaw", "pitch", "roll", "x", "y", "z"};
    Fc5HeadPath::Axis axes[6]{};
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls{line};
        std::string name;
        Fc5HeadPath::Axis a{};
        if (!(ls >> name >> a.amp)) {
            continue;
        }
        ls >> a.period >> a.phase;
        a.period = std::max(a.period, 0.1f);
        for (int i = 0; i < 6; ++i) {
            if (name == names[i]) {
                axes[i] = a;
            }
        }
    }
    hp.active = false;
    for (int i = 0; i < 6; ++i) {
        hp.axes[i] = axes[i];
        hp.active = hp.active || axes[i].amp != 0.0f;
    }
    spdlog::info("[VR] synthetic head motion {}: yaw {} deg/{} s, pitch {} deg/{} s, roll {} deg/{} s, x {} m/{} s, y {} m/{} s, z {} m/{} s",
                 hp.active ? "on" : "off", axes[0].amp, axes[0].period, axes[1].amp, axes[1].period, axes[2].amp, axes[2].period, axes[3].amp,
                 axes[3].period, axes[4].amp, axes[4].period, axes[5].amp, axes[5].period);
}
}

VRRuntime::Error OpenXR::update_poses(int frame) {
    SCOPE_PROFILER();
    std::scoped_lock _{ this->sync_mtx };
    std::unique_lock __{ this->pose_mtx };

    if (!this->session_ready) {
        return VRRuntime::Error::SUCCESS;
    }

    /*if (!this->needs_pose_update) {
        return VRRuntime::Error::SUCCESS;
    }*/

    this->view_state = {XR_TYPE_VIEW_STATE};
    this->stage_view_state = {XR_TYPE_VIEW_STATE};

    uint32_t view_count{};

    auto& current_pipeline_state = this->pipeline_state;

    // Only signal that we got the first valid pose if the display time becomes a sane value
    if (!this->got_first_valid_poses) {
        // Seen on VDXR
        if (current_pipeline_state.frame_state.predictedDisplayTime <= current_pipeline_state.frame_state.predictedDisplayPeriod) {
            spdlog::info("[VR] Frame state predicted display time is less than predicted display period!");
            return VRRuntime::Error::SUCCESS;
        }

        // Seen on VDXR. If for some reason the above if statement doesn't work, this will catch it.
        if (current_pipeline_state.frame_state.predictedDisplayTime == 11111111) {
            spdlog::info("[VR] Frame state predicted display time is 11111111!");
            return VRRuntime::Error::SUCCESS;
        }
    }

    if (current_pipeline_state.frame_state.predictedDisplayTime <= 1000) {
        spdlog::info("[VR] Frame state predicted display time is less than 1000!");
        return VRRuntime::Error::SUCCESS;
    }

    auto ps = frame > m_last_synchronized_frame ? this->prediction_scale : 0.0;
    const auto display_time = current_pipeline_state.frame_state.predictedDisplayTime + (XrDuration)(current_pipeline_state.frame_state.predictedDisplayPeriod * ps);

    XrViewLocateInfo view_locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->view_space;

    auto result = xrLocateViews(this->session, &view_locate_info, &this->view_state, (uint32_t)current_pipeline_state.views.size(), &view_count, current_pipeline_state.views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    view_locate_info = {XR_TYPE_VIEW_LOCATE_INFO};
    view_locate_info.viewConfigurationType = this->view_config;
    view_locate_info.displayTime = display_time;
    view_locate_info.space = this->stage_space;

    result = xrLocateViews(this->session, &view_locate_info, &this->stage_view_state, (uint32_t)current_pipeline_state.stage_views.size(), &view_count, current_pipeline_state.stage_views.data());

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateViews for stage space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    fc5_reload_view_test();
    fc5_apply_view_test(current_pipeline_state.views);
    fc5_apply_view_test(current_pipeline_state.stage_views);

    result = xrLocateSpace(this->view_space, this->stage_space, display_time, &current_pipeline_state.view_space_location);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrLocateSpace for view space failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    for (auto& hand : this->hands) {
        hand.location.next = &hand.velocity;
        result = xrLocateSpace(hand.space, this->stage_space, display_time, &hand.location);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrLocateSpace for hand space failed: {}", this->get_result_string(result));
            return (VRRuntime::Error)result;
        }
    }
    s_fc5_pose_time = (int64_t)display_time;
    s_fc5_pose_seq++;

    fc5_reload_head_path();
    if (const auto& hp = g_fc5_head_path; hp.active) {
        const double t = double(display_time) * 1e-9;
        float v[6]{};
        for (int i = 0; i < 6; ++i) {
            const auto& a = hp.axes[i];
            v[i] = a.amp == 0.0f ? 0.0f : a.amp * (float)std::sin(6.283185307179586 * t / a.period + glm::radians((double)a.phase));
        }
        auto& head = current_pipeline_state.view_space_location.pose;
        const glm::vec3 head_pos{head.position.x, head.position.y, head.position.z};
        const glm::quat head_rot = to_glm(head.orientation);
        // Yaw about the stage's up axis through the head; pitch and roll about the head's own axes.
        const glm::quat local = glm::angleAxis(glm::radians(v[1]), glm::vec3{1, 0, 0}) * glm::angleAxis(glm::radians(v[2]), glm::vec3{0, 0, 1});
        const glm::quat delta = glm::angleAxis(glm::radians(v[0]), glm::vec3{0, 1, 0}) * head_rot * local * glm::inverse(head_rot);
        const glm::vec3 shift = head_rot * glm::vec3{v[3], v[4], v[5]};
        auto move = [&](XrPosef& p) {
            const glm::vec3 pos = head_pos + shift + delta * (glm::vec3{p.position.x, p.position.y, p.position.z} - head_pos);
            p.position = {pos.x, pos.y, pos.z};
            p.orientation = to_openxr(glm::normalize(delta * to_glm(p.orientation)));
        };
        for (auto& view : current_pipeline_state.stage_views) {
            move(view.pose);
        }
        move(head);
        const auto now = std::chrono::steady_clock::now();
        if (now - g_fc5_head_path.logged > std::chrono::seconds(5)) {
            g_fc5_head_path.logged = now;
            spdlog::info("[VR] synthetic head motion at display time {:.3f} s (frame {}): yaw {:.2f} pitch {:.2f} roll {:.2f} deg, shift {:.3f} {:.3f} {:.3f} m", t,
                         frame, v[0], v[1], v[2], v[3], v[4], v[5]);
        }
    }

    if (!this->got_first_valid_poses) {
        this->got_first_valid_poses = (current_pipeline_state.view_space_location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) != 0;
    }

    this->needs_pose_update = false;
    this->got_first_poses = true;
    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_render_target_size() {
    uint32_t view_count{};
    auto result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, 0, &view_count, nullptr); 
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    this->view_configs.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(this->instance, this->system, this->view_config, view_count, &view_count, this->view_configs.data());
    if (result != XR_SUCCESS) {
        this->error = "Could not get view configuration properties: " + this->get_result_string(result);
        spdlog::error("[VR] {}", this->error.value());

        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

uint32_t OpenXR::get_width() const {
    if (this->view_configs.empty()) {
        return 0;
    }
//    return (uint32_t)((float)this->view_configs[0].recommendedImageRectWidth * this->resolution_scale);
    return (uint32_t)((float)this->view_configs[0].recommendedImageRectWidth * this->resolution_scale * eye_width_adjustment);
}

uint32_t OpenXR::get_height() const {
    if (this->view_configs.empty()) {
        return 0;
    }
    return (uint32_t)((float)this->view_configs[0].recommendedImageRectHeight * this->resolution_scale * eye_height_adjustment);
//    return (uint32_t)((float)this->view_configs[0].recommendedImageRectHeight * this->resolution_scale);
}

VRRuntime::Error OpenXR::consume_events(std::function<void(void*)> callback) {
    std::scoped_lock _{sync_mtx};

    XrEventDataBuffer edb{XR_TYPE_EVENT_DATA_BUFFER};
    auto result = xrPollEvent(this->instance, &edb);

    const auto bh = (XrEventDataBaseHeader*)&edb;

    while (result == XR_SUCCESS) {
        spdlog::info("VR: xrEvent: {}", this->get_structure_string(bh->type));

        if (callback) {
            callback(&edb);
        }

        if (bh->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto ev = (XrEventDataSessionStateChanged*)&edb;
            this->session_state = ev->state;

            spdlog::info("VR: XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED {}", ev->state);
            spdlog::default_logger()->flush();   // fc5vr: a quit often follows a focus change; the buffered log keeps its last second

            if (ev->state == XR_SESSION_STATE_READY) {
                spdlog::info("VR: XR_SESSION_STATE_READY");
                
                // Begin the session
                XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
                session_begin_info.primaryViewConfigurationType = this->view_config;

                fc5_boot_step("OpenXR: session ready, begin");
                result = xrBeginSession(this->session, &session_begin_info);
                fc5_boot_result("OpenXR: session ready, begin", result);

                if (result != XR_SUCCESS) {
                    this->error = std::string{"xrBeginSessionFailed: "} + this->get_result_string(result);
                    spdlog::error("VR: xrBeginSession failed: {}", this->get_result_string(result));
                } else {
                    this->session_ready = true;
                    synchronize_frame(m_last_synchronized_frame);
                }
            } else if (ev->state == XR_SESSION_STATE_LOSS_PENDING) {
                spdlog::info("VR: XR_SESSION_STATE_LOSS_PENDING");
                this->wants_reinitialize = true;
            } else if (ev->state == XR_SESSION_STATE_STOPPING) {
                spdlog::info("VR: XR_SESSION_STATE_STOPPING");

                if (this->ready()) {
                    xrEndSession(this->session);
                    this->session_ready = false;
                    this->frame_synced = false;
                    this->frame_began = false;

                    if (this->wants_reinitialize) {
                        //initialize_openxr();
                    }
                }
            }
        } else if (bh->type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            this->wants_reset_origin = true;
            const auto ev = (XrEventDataReferenceSpaceChangePending*)&edb;
            spdlog::info("VR: reference space change pending: space type {}, change time {}, pose valid {}", (int)ev->referenceSpaceType, (int64_t)ev->changeTime,
                         ev->poseValid == XR_TRUE);
            if (ev->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                s_fc5_local_space_change_time = (int64_t)ev->changeTime;
                s_fc5_local_space_changes++;
            }
        }

        edb = {XR_TYPE_EVENT_DATA_BUFFER};
        result = xrPollEvent(this->instance, &edb);
    } 
    
    if (result != XR_EVENT_UNAVAILABLE) {
        spdlog::error("VR: xrPollEvent failed: {}", this->get_result_string(result));
        return (VRRuntime::Error)result;
    }

    return VRRuntime::Error::SUCCESS;
}

VRRuntime::Error OpenXR::update_matrices(float nearz, float farz) {
    SCOPE_PROFILER();
    auto& current_pipeline_state = this->pipeline_state;
    if (!this->session_ready || current_pipeline_state.views.empty()) {
        return VRRuntime::Error::SUCCESS;
    }

    std::unique_lock __{ this->eyes_mtx };
    std::unique_lock ___{ this->pose_mtx };

    auto calc_fov = [&](PipelineState& pipelineState) {
        const auto& fov_l = pipelineState.views[0].fov;
        const auto& fov_r = pipelineState.views[1].fov;
        auto& active_fov_l = pipelineState.active_fov[0];
        auto& active_fov_r = pipelineState.active_fov[1];
        if(HORIZONTAL_SYMMETRIC) {
            float min_h = std::min(std::min(-fov_l.angleLeft, fov_l.angleRight), std::min(-fov_r.angleLeft, fov_r.angleRight));
            min_h *= m_extended_fov_range ? .9f : 1.f;
            const float max_h = std::max(std::max(-fov_l.angleLeft, fov_l.angleRight), std::max(-fov_r.angleLeft, fov_r.angleRight));
            const float scaled_h = std::lerp(min_h, max_h, m_horizontal_fov_scale);
            active_fov_l.angleLeft = -scaled_h;
            active_fov_l.angleRight = scaled_h;
            active_fov_r.angleLeft = -scaled_h;
            active_fov_r.angleRight = scaled_h;
        } else if (HORIZONTAL_MIRROR) {
            float max_outer = std::max(-fov_l.angleLeft, fov_r.angleRight);
            float max_inner = std::max(fov_l.angleRight, -fov_r.angleLeft);
            active_fov_l.angleLeft = -max_outer;
            active_fov_l.angleRight = max_inner;
            active_fov_r.angleLeft = -max_inner;
            active_fov_r.angleRight = max_outer;
        } else {
            active_fov_l.angleLeft = fov_l.angleLeft;
            active_fov_l.angleRight = fov_l.angleRight;
            active_fov_r.angleLeft = fov_r.angleLeft;
            active_fov_r.angleRight = fov_r.angleRight;
        }

        if(VERTICAL_SYMMETRIC) {
            float min_v = std::min(std::min(fov_l.angleUp, -fov_l.angleDown), std::min(fov_r.angleUp, -fov_r.angleDown));
            min_v *= m_extended_fov_range ? .9f : 1.f;
            const float max_v = std::max(std::max(fov_l.angleUp, -fov_l.angleDown), std::max(fov_r.angleUp, -fov_r.angleDown));
            float scaled_v = std::lerp(min_v, max_v, m_vertical_fov_scale);
            active_fov_l.angleUp = scaled_v;
            active_fov_l.angleDown = -scaled_v;
            active_fov_r.angleUp = scaled_v;
            active_fov_r.angleDown = -scaled_v;
        } else if (VERTICAL_MATCHED) {
            float max_top = std::max(fov_l.angleUp, fov_r.angleUp);
            float max_bottom = std::max(-fov_l.angleDown, -fov_r.angleDown);
            active_fov_l.angleUp = max_top;
            active_fov_l.angleDown = -max_bottom;
            active_fov_r.angleUp = max_top;
            active_fov_r.angleDown = -max_bottom;
        } else {
            active_fov_l.angleUp = fov_l.angleUp;
            active_fov_l.angleDown = fov_l.angleDown;
            active_fov_r.angleUp = fov_r.angleUp;
            active_fov_r.angleDown = fov_r.angleDown;
        }
        if (fc5_fov_margin > 0.0f) {
            // Rendered margin around each eye's view: the compositor's late reprojection reveals it instead of
            // smearing the image edge.
            for (auto* f : { &active_fov_l, &active_fov_r }) {
                f->angleLeft -= fc5_fov_margin;
                f->angleRight += fc5_fov_margin;
                f->angleUp += fc5_fov_margin;
                f->angleDown -= fc5_fov_margin;
            }
        }
        if (fc5_fov_scale > 0.0f && fc5_fov_scale < 0.999f) {
            // Narrower view at the same image size: more pixels per degree; the compositor leaves the rest of the lens black.
            for (auto* f : { &active_fov_l, &active_fov_r }) {
                for (float* a : { &f->angleLeft, &f->angleRight, &f->angleUp, &f->angleDown }) *a = std::atan(std::tan(*a) * fc5_fov_scale);
            }
        }
    };

    auto get_mat = [&](int eye, PipelineState& pipelineState) {
        std::array<float, 4> tan_half_fov{};
        tan_half_fov[0] = tan(pipelineState.active_fov[eye].angleLeft);
        tan_half_fov[1] = tan(pipelineState.active_fov[eye].angleRight);
        tan_half_fov[2] = tan(pipelineState.active_fov[eye].angleUp);
        tan_half_fov[3] = tan(pipelineState.active_fov[eye].angleDown);
        view_bounds[eye][0] = 0.5f - 0.5f * this->raw_projections[eye][0] / tan_half_fov[0];
        view_bounds[eye][1] = 0.5f + 0.5f * this->raw_projections[eye][1] / tan_half_fov[1];
        view_bounds[eye][2] = 0.5f - 0.5f * this->raw_projections[eye][2] / tan_half_fov[2];
        view_bounds[eye][3] = 0.5f + 0.5f * this->raw_projections[eye][3] / tan_half_fov[3];

        // if we've derived the right eye, we have up to date view bounds for both so adjust the render target if necessary
        if (eye == 1) {
            if (should_grow_rectangle_for_projection_cropping) {
                eye_width_adjustment = 1 / std::max(view_bounds[0][1] - view_bounds[0][0], view_bounds[1][1] - view_bounds[1][0]);
                eye_height_adjustment = 1 / std::max(view_bounds[0][3] - view_bounds[0][2], view_bounds[1][3] - view_bounds[1][2]);
            } else {
                eye_width_adjustment = 1;
                eye_height_adjustment = 1;
            }
        }

        const auto left =   tan_half_fov[0];
        const auto right =  tan_half_fov[1];
        const auto top =    tan_half_fov[2];
        const auto bottom = tan_half_fov[3];
        return Vector4f { left, right, top, bottom };
    };

    for (auto i = 0; i < 2; ++i) {
        const auto& pose = current_pipeline_state.views[i].pose;
        const auto& fov = current_pipeline_state.views[i].fov;
        this->raw_projections[i][0] = tan(fov.angleLeft);
        this->raw_projections[i][1] = tan(fov.angleRight);
        this->raw_projections[i][2] = tan(fov.angleUp);
        this->raw_projections[i][3] = tan(fov.angleDown);
        this->eyes[i] = Matrix4x4f{*(glm::quat*)&pose.orientation};
        this->eyes[i][3] = Vector4f{*(Vector3f*)&pose.position, 1.0f};
    }
    calc_fov(current_pipeline_state);
    frustums[0] = get_mat(0, current_pipeline_state);
    frustums[1] = get_mat(1, current_pipeline_state);

    this->ipd = glm::distance(this->eyes[0][3], this->eyes[1][3]);
    this->diagonal_fov = glm::degrees(2.0f * std::atan(std::sqrt(frustums[0][0]*frustums[0][0] + frustums[0][2]*frustums[0][2])));

    return VRRuntime::Error::SUCCESS;
}


VRRuntime::Error OpenXR::update_input() {
    SCOPE_PROFILER();
    if (!this->ready() || this->session_state != XR_SESSION_STATE_FOCUSED) {
        return (VRRuntime::Error)XR_ERROR_SESSION_NOT_READY;
    }

    XrActiveActionSet active_action_set{this->action_set.handle, XR_NULL_PATH};
    XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active_action_set;

    auto result = xrSyncActions(this->session, &sync_info);
    this->fc5_last_sync = result;
    ++this->fc5_syncs;

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to sync actions: {}", this->get_result_string(result));

        return (VRRuntime::Error)result;
    }

    const auto current_interaction_profile = this->get_current_interaction_profile();

    for (auto i = 0; i < 2; ++i) {
        auto& hand = this->hands[i];
        hand.forced_actions.clear();

        // Update controller pose state
        XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
        get_info.subactionPath = hand.path;
        get_info.action = this->action_set.action_map["pose"];

        XrActionStatePose pose_state{XR_TYPE_ACTION_STATE_POSE};

        result = xrGetActionStatePose(this->session, &get_info, &pose_state);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state pose {}: {}", i, this->get_result_string(result));

            return (VRRuntime::Error)result;
        }

        hand.active = pose_state.isActive;

        // Handle vector activator stuff
        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            if (this->is_action_active(activator, (VRRuntime::Hand)i)) {
                const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)i);
                
                for (const auto& output : it.second) {
                    const auto distance = glm::length(output.value - axis);

                    if (distance < 0.7f) {
                        hand.forced_actions[output.action] = true;
                    }
                }
            }
        }

        if (this->is_action_active_once("systembutton", (VRRuntime::Hand)i)) {
            this->handle_pause = true;
        }
    }

    // TODO: Other non-hand specific inputs
    return VRRuntime::Error::SUCCESS;
}

void OpenXR::destroy() {
    if (!this->loaded) {
        return;
    }

    std::scoped_lock _{sync_mtx};

    if (this->session != nullptr) {
        if (this->session_ready) {
            xrEndSession(this->session);
        }

        xrDestroySession(this->session);
    }

    if (this->instance != nullptr) {
        xrDestroyInstance(this->instance);
        this->instance = nullptr;
    }

    this->session = nullptr;
    this->session_ready = false;
    this->system = XR_NULL_SYSTEM_ID;
    this->frame_synced = false;
    this->frame_began = false;
//    this->internal_frame_counter = 0;
}

std::string OpenXR::get_result_string(XrResult result) const {
    std::string result_string{};
    result_string.resize(XR_MAX_RESULT_STRING_SIZE);
    xrResultToString(this->instance, result, result_string.data());

    return result_string;
}

std::string OpenXR::get_structure_string(XrStructureType type) const {
    std::string structure_string{};
    structure_string.resize(XR_MAX_STRUCTURE_NAME_SIZE);
    xrStructureTypeToString(this->instance, type, structure_string.data());

    return structure_string;
}

std::string OpenXR::get_path_string(XrPath path) const {
    if (path == XR_NULL_PATH) {
        return "XR_NULL_PATH";
    }

    std::string path_string{};
    path_string.resize(XR_MAX_PATH_LENGTH);

    uint32_t real_size{};

    if (auto result = xrPathToString(this->instance, path, XR_MAX_PATH_LENGTH, &real_size, path_string.data()); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path string: {}", this->get_result_string(result));
        return "";
    }

    path_string.resize(real_size-1);
    return path_string;
}

XrPath OpenXR::get_path(const std::string& path) const {
    XrPath path_handle{XR_NULL_PATH};

    if (auto result = xrStringToPath(this->instance, path.c_str(), &path_handle); result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get path: {}", this->get_result_string(result));
        return XR_NULL_PATH;
    }

    return path_handle;
}

std::string OpenXR::get_current_interaction_profile() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return "";
    }

    return this->get_path_string(state.interactionProfile);
}

XrPath OpenXR::get_current_interaction_profile_path() const {
    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
    if (xrGetCurrentInteractionProfile(this->session, this->hands[0].path, &state) != XR_SUCCESS) {
        return XR_NULL_PATH;
    }

    return state.interactionProfile;
}

std::optional<std::string> OpenXR::initialize_actions(const std::string& json_string) {
    spdlog::info("[VR] Initializing actions");

    if (auto result = xrStringToPath(this->instance, "/user/hand/left", &this->hands[VRRuntime::Hand::LEFT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (left): " + this->get_result_string(result);
    }

    if (auto result = xrStringToPath(this->instance, "/user/hand/right", &this->hands[VRRuntime::Hand::RIGHT].path); result != XR_SUCCESS) {
        return "xrStringToPath failed (right): " + this->get_result_string(result);
    }

    std::array<XrPath, 2> hand_paths{
        this->hands[VRRuntime::Hand::LEFT].path, 
        this->hands[VRRuntime::Hand::RIGHT].path
    };

    if (json_string.empty()) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating action set");

    XrActionSetCreateInfo action_set_create_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(action_set_create_info.actionSetName, "defaultopenxr");
    strcpy(action_set_create_info.localizedActionSetName, "Default");
    action_set_create_info.priority = 0;

    if (auto result = xrCreateActionSet(this->instance, &action_set_create_info, &this->action_set.handle); result != XR_SUCCESS) {
        return "xrCreateActionSet failed: " + this->get_result_string(result);
    }

    // Parse the JSON string using nlohmann
    json actions_json{};

    try {
        actions_json = json::parse(json_string);
    } catch (const std::exception& e) {
        return std::string{"json parse failed: "} + e.what();
    }

    if (actions_json.count("actions") == 0) {
        return "json missing actions";
    }

    auto actions_list = actions_json["actions"];
    bool has_pose_action = false;

    std::unordered_map<std::string, std::vector<XrActionSuggestedBinding>> profile_bindings{};

    for (const auto& controller : s_supported_controllers) {
        profile_bindings[controller] = {};
    }

    auto attempt_add_binding = [&](const std::string& interaction_profile, const XrActionSuggestedBinding& binding) -> bool {
        XrPath interaction_profile_path{};
        auto result = xrStringToPath(this->instance, interaction_profile.c_str(), &interaction_profile_path);
        auto& bindings = profile_bindings[interaction_profile];

        if (result == XR_SUCCESS) {
            bindings.push_back(binding);

            XrInteractionProfileSuggestedBinding suggested_bindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggested_bindings.interactionProfile = interaction_profile_path;
            suggested_bindings.countSuggestedBindings = (uint32_t)bindings.size();
            suggested_bindings.suggestedBindings = bindings.data();

            result = xrSuggestInteractionProfileBindings(this->instance, &suggested_bindings);

            if (result != XR_SUCCESS) {
                bindings.pop_back();
                spdlog::info("Bad binding passed to xrSuggestInteractionProfileBindings from {}: {} (path {}, action {})", interaction_profile, this->get_result_string(result), this->get_path_string(binding.binding), this->action_set.action_names.contains(binding.action) ? this->action_set.action_names.at(binding.action) : std::string("?"));
                return false;
            }

            return true;
        } else {
            spdlog::info("Bad interaction profile passed to xrStringToPath: {}", this->get_result_string(result));
            return false;
        }
    };

    for (auto& action : actions_list) {
        XrActionCreateInfo action_create_info{XR_TYPE_ACTION_CREATE_INFO};
        auto action_name = action["name"].get<std::string>();

        if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
            action_name = action_name.substr(it + 1);
        }

        auto localized_action_name = action_name;
        std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);

        strcpy(action_create_info.actionName, action_name.c_str());
        strcpy(action_create_info.localizedActionName, localized_action_name.c_str());

        action_create_info.countSubactionPaths = (uint32_t)hand_paths.size();
        action_create_info.subactionPaths = hand_paths.data();

        if (action_name == "pose") {
            has_pose_action = true;
        }

        std::unordered_set<XrAction>* out_actions = nullptr;

        // Translate the OpenVR action types to OpenXR action types
        switch (utility::hash(action["type"].get<std::string>())) {
            case "boolean"_fnv:
                if (action["type"].get<std::string>().ends_with("/value")) {
                    action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                    out_actions = &this->action_set.float_actions;
                } else {
                    action_create_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
                    out_actions = &this->action_set.bool_actions;
                }
                
                break;
            case "skeleton"_fnv: // idk what this is in OpenXR
                continue;
            case "pose"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
                out_actions = &this->action_set.pose_actions;
                break;
            case "vector1"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
                out_actions = &this->action_set.float_actions;
                break;
            case "vector2"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
                out_actions = &this->action_set.vector2_actions;
                break;
            case "vibration"_fnv:
                action_create_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
                out_actions = &this->action_set.vibration_actions;
                break;
            default:
                continue;
        }
        
        // Create the action
        XrAction xr_action{XR_NULL_HANDLE};
        if (auto result = xrCreateAction(this->action_set.handle, &action_create_info, &xr_action); result != XR_SUCCESS) {
            return "xrCreateAction failed for " + action_name + ": " + this->get_result_string(result);
        }

        if (out_actions != nullptr) {
            out_actions->insert(xr_action);
        }

        spdlog::info("[VR] Created action {} with handle {:x}", action_name, (uintptr_t)xr_action);

        this->action_set.actions.push_back(xr_action);
        this->action_set.action_map[action_name] = xr_action;
        this->action_set.action_names[xr_action] = action_name;

        // Suggest bindings
        for (const auto& map_it : OpenXR::s_bindings_map) {
            if (map_it.action_name != action_name) {
                continue;
            }

            const auto& interaction_string = map_it.interaction_path_name;

            for (auto i = 0; i < 2; ++i) {
                auto hand_string = interaction_string;
                auto it = hand_string.find('*');
                auto index = i;
                bool wildcard = false;

                if (it != std::string::npos) {
                    if (i == VRRuntime::Hand::LEFT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "left");
                    } else if (i == VRRuntime::Hand::RIGHT) {
                        hand_string.erase(it, 1);
                        hand_string.insert(it, "right");
                    }

                    wildcard = true;
                } else {
                    if (hand_string.find("left") != std::string::npos) {
                        index = VRRuntime::Hand::LEFT;
                    } else if (hand_string.find("right") != std::string::npos) {
                        index = VRRuntime::Hand::RIGHT;
                    }
                }

                spdlog::info("[VR] {}", hand_string);

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, hand_string.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", hand_string);

                    if (!wildcard) {
                        break;
                    }

                    continue;
                }

                if (this->action_set.action_map.contains(map_it.action_name)) {
                    for (const auto& controller : s_supported_controllers) {
                        if (attempt_add_binding(controller, { this->action_set.action_map[map_it.action_name], p })) {
                            this->hands[index].profiles[controller].path_map[map_it.action_name] = p;
                        }
                    }
                }

                if (!wildcard) {
                    break;
                }
            }
        }
    }

    if (!has_pose_action) {
        return "json missing pose action";
    }

    // Check for json files that will override the default suggested bindings
    for (const auto& controller : s_supported_controllers) {
        // Create default action vector associations
        for (const auto& association : s_action_vector_associations) {
            auto& hand = this->hands[association.hand];
            auto& hand_profile = hand.profiles[controller];
            auto& action_map = this->action_set.action_map;
            const auto action_activator = action_map[association.action_activator];
            const auto action_modifier = action_map[association.action_modifier];

            hand_profile.action_vector_associations[action_activator] = action_modifier;

            for (const auto& vector_activator : association.vector_activators) {
                const auto output_action = action_map[vector_activator.action_name];
                hand_profile.vector_activators[action_activator].push_back({ vector_activator.value, output_action });
            }
        }

        auto filename = controller + ".json";

        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        filename = (Framework::get_persistent_dir() / filename).string();

        // check if the file exists
        if (std::filesystem::exists(filename)) {
            spdlog::info("[VR] Loading bindings for {}", filename);

            profile_bindings[controller].clear();

            this->hands[VRRuntime::Hand::LEFT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].vector_activators.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].action_vector_associations.clear();
            this->hands[VRRuntime::Hand::LEFT].profiles[controller].path_map.clear();
            this->hands[VRRuntime::Hand::RIGHT].profiles[controller].path_map.clear();

            // load the json file
            auto j = nlohmann::json::parse(std::ifstream(filename));

            for (auto it : j["bindings"]) {
                auto action_str = it["action"].get<std::string>();
                auto path_str = it["path"].get<std::string>();

                XrPath p{XR_NULL_PATH};
                auto result = xrStringToPath(this->instance, path_str.c_str(), &p);

                if (result != XR_SUCCESS || p == XR_NULL_PATH) {
                    spdlog::error("[VR] Failed to find path for {}", path_str);
                    continue;
                }

                const auto hand_idx = path_str.find("/left/") != std::string::npos ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;

                if (this->action_set.action_map.contains(action_str)) {
                    if (attempt_add_binding(controller, { this->action_set.action_map[action_str], p })) {
                        this->hands[hand_idx].profiles[controller].path_map[action_str] = p;
                    }
                }
            }

            for (auto it : j["vector2_associations"]) {
                const auto activator_name = it["activator"].get<std::string>();
                const auto modifier_name = it["modifier"].get<std::string>();
                const auto path_name = it["path"].get<std::string>();
                const auto path = this->get_path(path_name);

                for (auto output : it["outputs"]) {
                    const auto action_name = output["action"].get<std::string>();
                    
                    auto value_json = output["value"];
                    const auto value = Vector2f{value_json["x"].get<float>(), value_json["y"].get<float>()};

                    if (this->action_set.action_map.contains(action_name)) {
                        auto& hand = path == this->hands[VRRuntime::Hand::LEFT].path ? this->hands[VRRuntime::Hand::LEFT] : this->hands[VRRuntime::Hand::RIGHT];
                        auto& hand_profile = hand.profiles[controller];

                        spdlog::info("[VR] Adding vector2 association for {} {}", controller, path_name);

                        const auto output_action = this->action_set.action_map[action_name];
                        const auto action_modifier = this->action_set.action_map[modifier_name];
                        const auto action_activator = this->action_set.action_map[activator_name];

                        hand_profile.vector_activators[action_activator].push_back({ value, output_action });
                        hand_profile.action_vector_associations[action_activator] = action_modifier;
                    }
                }
            }
        }
    }

    // Create the action spaces for each hand
    for (auto i = 0; i < 2; ++i) {
        spdlog::info("[VR] Creating action space for hand {}", i);
        
        XrActionSpaceCreateInfo action_space_create_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        action_space_create_info.action = this->action_set.action_map["pose"];
        action_space_create_info.subactionPath = this->hands[i].path;
        action_space_create_info.poseInActionSpace.orientation.w = 1.0f;

        if (auto result = xrCreateActionSpace(this->session, &action_space_create_info, &this->hands[i].space); result != XR_SUCCESS) {
            return "xrCreateActionSpace failed (" + std::to_string(i) + ")" + this->get_result_string(result);
        }
    }

    // Attach the action set to the session
    spdlog::info("[VR] Attaching action set to session");

    XrSessionActionSetsAttachInfo action_sets_attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    action_sets_attach_info.countActionSets = 1;
    action_sets_attach_info.actionSets = &this->action_set.handle;

    if (auto result = xrAttachSessionActionSets(this->session, &action_sets_attach_info); result != XR_SUCCESS) {
        return "xrAttachSessionActionSets failed: " + this->get_result_string(result);
    }

    return std::nullopt;
}

bool OpenXR::is_action_active(XrAction action, VRRuntime::Hand hand) const {
    if (hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    if (this->action_set.bool_actions.contains(action)) {
        XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
        auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
    } else if (this->action_set.float_actions.contains(action)) {
        XrActionStateFloat active{XR_TYPE_ACTION_STATE_FLOAT};
        auto result = xrGetActionStateFloat(this->session, &get_info, &active);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to get action state: {}", this->get_result_string(result));
            return false;
        }

        return active.isActive == XR_TRUE && active.currentState > 0.0f;
    } // idk?

    return false;
}

bool OpenXR::is_action_active(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE;
}

bool OpenXR::is_action_active_once(std::string_view action_name, VRRuntime::Hand hand) const {
    if (!this->action_set.action_map.contains(action_name.data()) || hand > VRRuntime::Hand::RIGHT) {
        return false;
    }

    auto action = this->action_set.action_map.find(action_name.data())->second;

    if (auto it = this->hands[hand].forced_actions.find(action); it != this->hands[hand].forced_actions.end()) {
        if (it->second) {
            return true;
        }
    }

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;
    
    XrActionStateBoolean active{XR_TYPE_ACTION_STATE_BOOLEAN};
    auto result = xrGetActionStateBoolean(this->session, &get_info, &active);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get action state for {}: {}", action_name, this->get_result_string(result));
        return false;
    }

    return active.isActive == XR_TRUE && active.currentState == XR_TRUE && active.changedSinceLastSync == XR_TRUE;
}

Vector2f OpenXR::get_action_axis(XrAction action, VRRuntime::Hand hand) const {
    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = this->hands[hand].path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

std::string OpenXR::translate_openvr_action_name(std::string action_name) const {
    if (action_name.empty()) {
        return action_name;
    }

    if (auto it = action_name.find_last_of("/"); it != std::string::npos) {
        action_name = action_name.substr(it + 1);
    }

    std::transform(action_name.begin(), action_name.end(), action_name.begin(), ::tolower);
    return action_name;
}

Vector2f OpenXR::get_left_stick_axis() const {
    if (!this->action_set.action_map.contains("joystick")) {
        return Vector2f{};
    }

    const auto& hand = this->hands[VRRuntime::Hand::LEFT];
    auto profile_it = hand.profiles.find(this->get_current_interaction_profile());

    if (profile_it == hand.profiles.end()) {
        return Vector2f{};
    }

    const auto& hand_profile = profile_it->second;

    auto joystick_action = this->action_set.action_map.find("joystick")->second;
    auto touchpad_action = this->action_set.action_map.find("touchpad")->second;

    auto action = hand_profile.path_map.contains("joystick") ? joystick_action : touchpad_action;

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand.path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

Vector2f OpenXR::get_right_stick_axis() const {
    if (!this->action_set.action_map.contains("joystick")) {
        return Vector2f{};
    }

    const auto& hand = this->hands[VRRuntime::Hand::RIGHT];
    auto profile_it = hand.profiles.find(this->get_current_interaction_profile());

    if (profile_it == hand.profiles.end()) {
        return Vector2f{};
    }

    const auto& hand_profile = profile_it->second;

    auto joystick_action = this->action_set.action_map.find("joystick")->second;
    auto touchpad_action = this->action_set.action_map.find("touchpad")->second;

    auto action = hand_profile.path_map.contains("joystick") ? joystick_action : touchpad_action;

    XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
    get_info.action = action;
    get_info.subactionPath = hand.path;

    XrActionStateVector2f axis{XR_TYPE_ACTION_STATE_VECTOR2F};
    auto result = xrGetActionStateVector2f(this->session, &get_info, &axis);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to get stick action state: {}", this->get_result_string(result));
        return Vector2f{};
    }

    return *(Vector2f*)&axis.currentState;
}

void OpenXR::trigger_haptic_vibration(float duration, float frequency, float amplitude, VRRuntime::Hand source) const {
    if (!this->action_set.action_map.contains("haptic")) {
        return;
    }

    XrHapticActionInfo haptic_info{XR_TYPE_HAPTIC_ACTION_INFO};
    haptic_info.action = this->action_set.action_map.find("haptic")->second;
    haptic_info.subactionPath = this->hands[source].path;

    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = amplitude;
    vibration.frequency = frequency;

    // cast the duration from seconds to nanoseconds
    vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<float>(duration)).count();

    auto result = xrApplyHapticFeedback(this->session, &haptic_info, (XrHapticBaseHeader*)&vibration);

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] Failed to apply haptic feedback: {}", this->get_result_string(result));
    }
}

void OpenXR::display_bindings_editor() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    ImGui::Text("Interaction Profile: %s", current_interaction_profile.c_str());

    if (ImGui::Button("Restore Default Bindings")) {
        auto filename = current_interaction_profile + ".json";

        // replace the slashes with underscores
        std::replace(filename.begin(), filename.end(), '/', '_');

        if (std::filesystem::exists(filename)) {
            // Delete the file
            std::filesystem::remove(filename);
            this->wants_reinitialize = true;
        }
    }

    if (ImGui::Button("Save Bindings")) {
        this->save_bindings();
    }

    auto display_hand = [&](const std::string& name, uint32_t index) {
        if (current_interaction_profile.empty() || current_interaction_profile == "XR_NULL_PATH") {
            ImGui::Text("Interaction profile not loaded, try putting on your headset.");
            return;
        }

        if (ImGui::TreeNode(name.data())) {
            ImGui::PushID(name.data());

            auto& hand = this->hands[index];

            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                if (this->is_action_active(activator, (VRRuntime::Hand)index)) {
                    const auto axis = this->get_action_axis(modifier, (VRRuntime::Hand)index);

                    for (const auto& output : it.second) {
                        const auto distance = glm::length(output.value - axis);
                        ImGui::Text("%s: %.2f", this->action_set.action_names[output.action].data(), distance);
                    }
                }
            }

            auto& path_map = hand.profiles[current_interaction_profile].path_map;

            std::vector<std::string> known_actions{};
            std::vector<const char*> known_actions_cstr{};

            std::vector<std::string> known_vector2_actions{};
            std::vector<const char*> known_vector2_actions_cstr{};

            for (auto action :this->action_set.actions) {
                known_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_actions) {
                known_actions_cstr.push_back(it.data());
            };

            for (auto action : this->action_set.vector2_actions) {
                known_vector2_actions.push_back(this->action_set.action_names[action]);
            }

            for (auto& it : known_vector2_actions) {
                known_vector2_actions_cstr.push_back(it.data());
            }

            for (auto& it : path_map) {
                ImGui::PushID(it.first.data());

                int current_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == it.first) {
                        current_combo_index = i;
                        break;
                    }
                }

                if (ImGui::Button("X")) {
                    path_map.erase(it.first);

                    this->save_bindings();
                    ImGui::PopID();
                    break;
                }

                auto combo_name = this->get_path_string(it.second) + ": " + known_actions[current_combo_index];

                ImGui::SameLine();
                if (ImGui::Combo(combo_name.c_str(), &current_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    path_map.erase(it.first);
                    path_map[known_actions[current_combo_index]] = it.second;

                    this->save_bindings();
                    ImGui::PopID();

                    break;
                }

                ImGui::PopID();
            }

            // Create a way to add a completely new binding
            // Create a textbox for inputting the path for the new binding
            ImGui::InputText("New Binding (e.g. /user/hand/left/input/trigger)", hand.ui.new_path_name, XR_MAX_PATH_LENGTH);
            ImGui::Combo("Action", &hand.ui.action_combo_index, known_actions_cstr.data(), known_actions_cstr.size());

            if (ImGui::Button("Add Binding")) {
                XrPath p{};
                if (xrStringToPath(this->instance, hand.ui.new_path_name, &p) != XR_SUCCESS) {
                    spdlog::error("[VR] Failed to convert path: {}", hand.ui.new_path_name);
                } else {
                    path_map[known_actions[hand.ui.action_combo_index]] = p;
                    this->save_bindings();
                }
            }

            ImGui::Text("Vector2 Associations");
            for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
                ImGui::PushID(&it.first);

                const auto activator = it.first;
                const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

                const auto activator_name = this->action_set.action_names[activator];
                const auto modifier_name = this->action_set.action_names[modifier];

                int activator_combo_index = 0;
                int modifier_combo_index = 0;

                for (auto i = 0; i < known_actions.size(); i++) {
                    if (known_actions[i] == activator_name) {
                        activator_combo_index = i;
                        break;
                    }
                }

                for (auto i = 0; i < known_vector2_actions.size(); i++) {
                    if (known_vector2_actions[i] == modifier_name) {
                        modifier_combo_index = i;
                        break;
                    }
                }

                ImGui::PushID(modifier_name.data());

                if (ImGui::Combo(modifier_name.data(), &modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size())) {
                    hand.profiles[current_interaction_profile].action_vector_associations[activator] = this->action_set.action_map[known_vector2_actions[modifier_combo_index]];
                }

                ImGui::PushID(activator_name.data());
                ImGui::Indent();

                if (ImGui::Combo(activator_name.data(), &activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                    const auto old_outputs = hand.profiles[current_interaction_profile].vector_activators[activator];
                    const auto new_activator = this->action_set.action_map[known_actions[activator_combo_index]];

                    hand.profiles[current_interaction_profile].action_vector_associations.erase(activator);
                    hand.profiles[current_interaction_profile].action_vector_associations[new_activator] = modifier;
                    hand.profiles[current_interaction_profile].vector_activators.erase(activator);
                    hand.profiles[current_interaction_profile].vector_activators[new_activator] = old_outputs;
                }

                ImGui::Indent();
                for (auto& output : it.second) {
                    int output_combo_index = 0;
                    const auto output_name = this->action_set.action_names[output.action];

                    for (auto i = 0; i < known_actions.size(); i++) {
                        if (known_actions[i] == output_name) {
                            output_combo_index = i;
                            break;
                        }
                    }

                    ImGui::PushID(output_name.data());

                    if (ImGui::Combo(output_name.data(), &output_combo_index, known_actions_cstr.data(), known_actions_cstr.size())) {
                        output.action = this->action_set.action_map[known_actions[output_combo_index]];
                    }

                    ImGui::SliderFloat2("Value", &output.value[0], -1.0f, 1.0f);
                    ImGui::PopID();
                }

                if (ImGui::Button("Insert New Output")) {
                    hand.profiles[current_interaction_profile].vector_activators[activator].push_back({});
                }

                ImGui::Unindent();
                ImGui::Unindent();
                ImGui::PopID();
                ImGui::PopID();

                ImGui::PopID();
            }

            ImGui::Combo("New Vector2 Activator", &hand.ui.activator_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::Combo("New Vector2 Modifier", &hand.ui.modifier_combo_index, known_vector2_actions_cstr.data(), known_vector2_actions_cstr.size());
            ImGui::Combo("New Vector2 Output", &hand.ui.output_combo_index, known_actions_cstr.data(), known_actions_cstr.size());
            ImGui::SliderFloat2("New Vector2 Value", &hand.ui.output_vector2[0], -1.0f, 1.0f);

            if (ImGui::Button("Add Vector2 Association")) {
                const auto activator = this->action_set.action_map[known_actions[hand.ui.activator_combo_index]];
                const auto modifier = this->action_set.action_map[known_vector2_actions[hand.ui.modifier_combo_index]];
                const auto output = this->action_set.action_map[known_actions[hand.ui.output_combo_index]];

                hand.profiles[current_interaction_profile].action_vector_associations[activator] = modifier;
                hand.profiles[current_interaction_profile].vector_activators[activator].push_back({hand.ui.output_vector2, output});
            }

            ImGui::PopID();
            ImGui::TreePop();
        }
    };

    display_hand("Left", 0);
    display_hand("Right", 1);
}

void OpenXR::save_bindings() {
    const auto current_interaction_profile = this->get_current_interaction_profile();
    nlohmann::json j;

    for (auto& hand : this->hands) {
        for (auto& it : hand.profiles[current_interaction_profile].path_map) {
            nlohmann::json binding{};
            binding["action"] = it.first;
            binding["path"] = this->get_path_string(it.second);
            j["bindings"].push_back(binding);
        }

        for (auto& it : hand.profiles[current_interaction_profile].vector_activators) {
            const auto activator = it.first;
            const auto modifier = hand.profiles[current_interaction_profile].action_vector_associations[activator];

            const auto activator_name = this->action_set.action_names[activator];
            const auto modifier_name = this->action_set.action_names[modifier];

            nlohmann::json vector2_association{};
            vector2_association["path"] = this->get_path_string(hand.path);
            vector2_association["activator"] = activator_name;
            vector2_association["modifier"] = modifier_name;

            nlohmann::json outputs{};
            for (auto& output : it.second) {
                const auto output_name = this->action_set.action_names[output.action];

                nlohmann::json output_json{};
                output_json["action"] = output_name;
                output_json["value"]["x"] = output.value.x;
                output_json["value"]["y"] = output.value.y;

                outputs.push_back(output_json);
            }

            vector2_association["outputs"] = outputs;
            j["vector2_associations"].push_back(vector2_association);
        }
    }

    auto filename = current_interaction_profile + ".json";

    // replace the slashes with underscores
    std::replace(filename.begin(), filename.end(), '/', '_');
    std::ofstream(filename) << j.dump(4);

    this->wants_reinitialize = true;
}

XrResult OpenXR::begin_frame(int frame) {
    SCOPE_PROFILER();
    std::scoped_lock _{sync_mtx};

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        spdlog::info("VR: begin_frame: not ready");
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (this->frame_began) {
        spdlog::info("[VR] begin_frame called while frame already began");
        return XR_SUCCESS;
    }

    this->begin_profile();

    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto begin_t0 = std::chrono::steady_clock::now();
    auto result = xrBeginFrame(this->session, &frame_begin_info);
    s_fc5_begin_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin_t0).count();

    this->end_profile("xrBeginFrame");

    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrBeginFrame failed: {}", this->get_result_string(result));
    }

    if (result == XR_ERROR_CALL_ORDER_INVALID) {
        synchronize_frame(frame);
        result = xrBeginFrame(this->session, &frame_begin_info);
    }

    this->frame_began = result == XR_SUCCESS || result == XR_FRAME_DISCARDED; // discarded means endFrame was not called

    return result;
}

XrResult OpenXR::end_frame(const std::vector<XrCompositionLayerBaseHeader*>& quad_layers, int frame, bool has_depth) {

    SCOPE_PROFILER();
    std::scoped_lock _{sync_mtx};

    // Submission census (Far Cry 5 bring-up): every 5 s, how many frames reached xrEndFrame and why not.
    static uint32_t c_calls = 0, c_not_ready = 0, c_not_begun = 0, c_render = 0, c_ok = 0, c_fail = 0;
    static auto     c_last  = std::chrono::steady_clock::now();
    ++c_calls;
    if (std::chrono::steady_clock::now() - c_last > std::chrono::seconds(5)) {
        spdlog::info("[VR] submit census: end_frame calls={} not-ready={} (ready={} poses={} synced={}) not-begun={} shouldRender={} ok={} failed={}", c_calls, c_not_ready,
                     this->ready(), this->got_first_poses, this->frame_synced, c_not_begun, c_render, c_ok, c_fail);
        c_calls = c_not_ready = c_not_begun = c_render = c_ok = c_fail = 0;
        c_last = std::chrono::steady_clock::now();
    }

    if (!this->ready() || !this->got_first_poses || !this->frame_synced) {
        ++c_not_ready;
        return XR_ERROR_SESSION_NOT_READY;
    }

    if (!this->frame_began) {
        ++c_not_begun;
        spdlog::info("[VR] end_frame called while frame not begun");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    if (this->pipeline_state.frame_state.shouldRender == XR_TRUE) ++c_render;

    std::vector<XrCompositionLayerBaseHeader*> layers{};
    std::vector<XrCompositionLayerProjectionView> projection_layer_views{};
    std::vector<XrCompositionLayerQuad> l_quad_layers{};


    // we CANT push the layers every time, it cause some layer error
    // in xrEndFrame, so we must only do it when shouldRender is true
    auto current_pipeline = &this->pipeline_state;

    auto l_frame = frame % 2 == 0 ? frame : frame - 1;
    auto r_frame = frame % 2 == 0 ? frame - 1 : frame;

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const bool flat            = ModSettings::showFlatScreenDisplay();
    const bool keep_projection = flat && fc5_simulator_runtime() && s_fc5_sim_projection.load();
    if (current_pipeline->frame_state.shouldRender == XR_TRUE) {
        projection_layer_views.resize(current_pipeline->stage_views.size(), {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        if (!flat || keep_projection) {
            for (auto i = 0; i < projection_layer_views.size(); ++i) {
                const auto& swapchain = this->swapchains[this->eye_swapchain((uint32_t)i)];
                int         actual_frame = i == 0 ? l_frame : r_frame;
                auto& constants = GlobalPool::get_xr_constants(actual_frame);

                projection_layer_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projection_layer_views[i].pose = constants.pose;
                projection_layer_views[i].subImage.swapchain = swapchain.handle;
                int32_t offset_x = 0, offset_y = 0, extent_x = 0, extent_y = 0;
                int texture_area_width = swapchain.width;
    
                offset_x = (int32_t)(view_bounds[i][0] * (float)texture_area_width);
                extent_x = (int32_t)(view_bounds[i][1] * (float)texture_area_width) - offset_x;
    
                offset_y = (int32_t)(view_bounds[i][2] * (float)swapchain.height);
                extent_y = (int32_t)(view_bounds[i][3] * (float)swapchain.height) - offset_y;
                if(m_horizontal_fov_scale < 1.0f || m_vertical_fov_scale < 1.0f) {
                    projection_layer_views[i].subImage.imageRect.offset = {0, 0};
                    projection_layer_views[i].subImage.imageRect.extent = {swapchain.width, swapchain.height};
                    projection_layer_views[i].fov = current_pipeline->active_fov[i];
                } else {
                    // The FOV stored with the pose this eye's image was rendered (or rebuilt) for.
                    // The FOV the eye image was rendered with, and the whole image (it is stretched to fill the swapchain).
                    if (constants.fov.angleRight > constants.fov.angleLeft) {
                        projection_layer_views[i].fov = constants.fov;
                        projection_layer_views[i].subImage.imageRect.offset = {0, 0};
                        projection_layer_views[i].subImage.imageRect.extent = {swapchain.width, swapchain.height};
                    } else {
                        projection_layer_views[i].fov = current_pipeline->stage_views[i].fov;
                        projection_layer_views[i].subImage.imageRect.offset = {offset_x, offset_y};
                        projection_layer_views[i].subImage.imageRect.extent = {extent_x, extent_y};
                    }
                }

                // Both native stereo eyes were rendered from this frame's poses; pairing them with
                // the alternate-eye frames would reproject one eye by a frame of head motion.
                if (VR::get()->is_native_stereo()) {
                    const auto& native = GlobalPool::get_native_stereo_views(frame);
                    const auto& q      = native.pose[i].orientation;
                    // Right after native stereo is switched on, a frame may have no stored views
                    // yet; an all-zero orientation is rejected by xrEndFrame (XR_ERROR_POSE_INVALID).
                    if (q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 0.0f) {
                        projection_layer_views[i].pose = native.pose[i];
                        projection_layer_views[i].fov  = native.fov[i];
                    } else {
                        projection_layer_views[i].pose = current_pipeline->stage_views[i].pose;
                        projection_layer_views[i].fov  = current_pipeline->stage_views[i].fov;
                    }
                }
            }
            if (VR::get()->m_fc5_views_set) {
                auto& vrm = VR::get();
                for (size_t i = 0; i < projection_layer_views.size() && i < 2; ++i) {
                    const float* p = vrm->m_fc5_view_pose[i];
                    const float* f = vrm->m_fc5_view_fov[i];
                    projection_layer_views[i].pose = XrPosef{ { p[3], p[4], p[5], p[6] }, { p[0], p[1], p[2] } };
                    projection_layer_views[i].fov  = XrFovf{ f[0], f[1], f[2], f[3] };
                }
                vrm->m_fc5_views_set = false;
            }
            // A view whose stored pose is far from where the head is now (a stale or unwritten slot)
            // would be shown displaced, the rest of the view black: use the current pose instead.
            {
                static uint32_t c_bad_pose = 0;
                static auto     last_bad   = std::chrono::steady_clock::now();
                for (size_t i = 0; i < projection_layer_views.size() && i < current_pipeline->stage_views.size(); ++i) {
                    auto&       v  = projection_layer_views[i];
                    const auto& sv = current_pipeline->stage_views[i];
                    const auto& q  = v.pose.orientation;
                    const auto& r  = sv.pose.orientation;
                    const float len = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                    const float dot = std::abs(q.x * r.x + q.y * r.y + q.z * r.z + q.w * r.w) / std::sqrt(std::max(len, 1e-12f));
                    const float dx = v.pose.position.x - sv.pose.position.x, dy = v.pose.position.y - sv.pose.position.y, dz = v.pose.position.z - sv.pose.position.z;
                    const bool  bad = len < 0.5f || len > 1.5f || dot < 0.985f || (dx * dx + dy * dy + dz * dz) > 0.09f || !(v.fov.angleRight > v.fov.angleLeft);
                    if (bad) {
                        if (c_bad_pose < 5 || c_bad_pose % 200 == 0) {
                            spdlog::info("[VR] view {} of frame {}: stored pose off by {:.1f} deg / {:.2f} m (len {:.2f}, fov L{:.2f} R{:.2f}): current pose used", i, frame,
                                         glm::degrees(2.0f * std::acos(std::min(1.0f, dot))), std::sqrt(dx * dx + dy * dy + dz * dz), len, v.fov.angleLeft, v.fov.angleRight);
                        }
                        ++c_bad_pose;
                        v.pose = sv.pose;
                        v.fov  = sv.fov;
                    }
                }
                if (std::chrono::steady_clock::now() - last_bad > std::chrono::seconds(5)) {
                    last_bad = std::chrono::steady_clock::now();
                    if (c_bad_pose) spdlog::info("[VR] {} views with a stale pose replaced in the last 5 s", c_bad_pose);
                    c_bad_pose = 0;
                }
            }
            {
                static auto last_dump = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - last_dump > std::chrono::seconds(5)) {
                    last_dump = std::chrono::steady_clock::now();
                    for (size_t i = 0; i < projection_layer_views.size(); ++i) {
                        const auto& v = projection_layer_views[i];
                        spdlog::info("[VR] layer view {}: pos ({:.3f},{:.3f},{:.3f}) rot ({:.3f},{:.3f},{:.3f},{:.3f}) fov L{:.3f} R{:.3f} U{:.3f} D{:.3f} rect ({},{} {}x{}) swapchain {}x{}",
                                     i, v.pose.position.x, v.pose.position.y, v.pose.position.z, v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z,
                                     v.pose.orientation.w, v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown, v.subImage.imageRect.offset.x,
                                     v.subImage.imageRect.offset.y, v.subImage.imageRect.extent.width, v.subImage.imageRect.extent.height, this->swapchains[this->eye_swapchain((uint32_t)i)].width,
                                     this->swapchains[this->eye_swapchain((uint32_t)i)].height);
                    }
                }
            }
            layer.space = this->stage_space;
            layer.viewCount = (uint32_t)projection_layer_views.size();
            layer.views = projection_layer_views.data();
            layers.push_back((XrCompositionLayerBaseHeader*)&layer);

        }
        if (flat) {
            l_quad_layers.resize(1);
            // Initialize quad layers for each eye
            for (size_t i = 0; i < 1; ++i) {
                const auto& swapchain = this->swapchains[this->eye_swapchain((uint32_t)i)];
                
                l_quad_layers[i] = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                l_quad_layers[i].next = nullptr;
                l_quad_layers[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                l_quad_layers[i].space = this->stage_space;
                l_quad_layers[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                
                // Configure swapchain
                l_quad_layers[i].subImage.swapchain = swapchain.handle;
                l_quad_layers[i].subImage.imageRect.offset = {0, 0};
                l_quad_layers[i].subImage.imageRect.extent = {(int32_t)swapchain.width, (int32_t)swapchain.height};

                {
                    auto flat_screen = glm::mat4(1.0f);
                    flat_screen[3][2] = -m_flat_screen_distance;
                    flat_screen = (fc5_flat_anchor_on ? fc5_flat_anchor : m_center_stage) * flat_screen;
                    auto rotation = glm::normalize(glm::quat_cast(flat_screen));
                    l_quad_layers[i].pose.orientation = to_openxr(rotation);
                    l_quad_layers[i].pose.position = {flat_screen[3][0], flat_screen[3][1], flat_screen[3][2]};
                }

                // Set size (2m wide, maintain aspect ratio)
                float aspect_ratio = (float)swapchain.width / (float)swapchain.height;
                l_quad_layers[i].size = {2.0f, 2.0f / aspect_ratio};

                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&l_quad_layers[i]));
            }
        }
        for(auto layer : quad_layers) {
            layers.push_back(layer);
        }
    }

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = current_pipeline->frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = this->blend_mode;
    frame_end_info.layerCount = (uint32_t)layers.size();
    frame_end_info.layers = layers.data();

    //spdlog::info("[VR] Ending frame, {} layers", frame_end_info.layerCount);
    //spdlog::info("[VR] Ending frame, layer ptr: {:x}", (uintptr_t)frame_end_info.layers);

    this->begin_profile();
    const auto end_t0 = std::chrono::steady_clock::now();
    auto result = xrEndFrame(this->session, &frame_end_info);
    s_fc5_end_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - end_t0).count();
    s_fc5_end_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - end_t0).count();
    this->end_profile("xrEndFrame");
    if (result != XR_SUCCESS) {
        ++c_fail;
        spdlog::error("[VR] xrEndFrame failed: {}", this->get_result_string(result));
    } else {
        ++c_ok;
    }

    this->fc5_eye_set = this->fc5_double_eyes.load() ? (this->fc5_eye_set ^ 1) : 0;

//    internal_frame_counter++;
    this->frame_began = false;
    this->frame_synced = false;

    return result;
}
}

namespace runtimes {
std::string OpenXR::fc5_probe_actions() const {
    std::string out = fmt::format("sync {} x{} ({}) profiles: ", (int)this->fc5_last_sync, this->fc5_syncs, this->get_result_string(this->fc5_last_sync));
    const char* hand_names[2]{ "L", "R" };
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
        const auto r = xrGetCurrentInteractionProfile(this->session, this->hands[h].path, &st);
        out += fmt::format("{}={} ", hand_names[h], r == XR_SUCCESS ? (st.interactionProfile == XR_NULL_PATH ? std::string("none") : this->get_path_string(st.interactionProfile))
                                                                  : this->get_result_string(r));
    }
    int active = 0, total = 0;
    std::string states;
    for (const auto& [name, action] : this->action_set.action_map) {
        for (int h = 0; h < 2; ++h) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action        = action;
            gi.subactionPath = this->hands[h].path;
            bool  is_active  = false;
            float value      = 0.0f;
            if (this->action_set.bool_actions.contains(action)) {
                XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
                if (xrGetActionStateBoolean(this->session, &gi, &b) == XR_SUCCESS) { is_active = b.isActive; value = b.currentState ? 1.0f : 0.0f; }
            } else if (this->action_set.float_actions.contains(action)) {
                XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
                if (xrGetActionStateFloat(this->session, &gi, &f) == XR_SUCCESS) { is_active = f.isActive; value = f.currentState; }
            } else if (this->action_set.vector2_actions.contains(action)) {
                XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
                if (xrGetActionStateVector2f(this->session, &gi, &v) == XR_SUCCESS) { is_active = v.isActive; value = std::abs(v.currentState.x) + std::abs(v.currentState.y); }
            } else if (this->action_set.pose_actions.contains(action)) {
                XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
                if (xrGetActionStatePose(this->session, &gi, &ps) == XR_SUCCESS) { is_active = ps.isActive; value = 0.0f; }
            } else {
                continue;
            }
            ++total;
            if (is_active) ++active;
            if (is_active && value != 0.0f) states += fmt::format(" {}:{}={:.2f}", name, hand_names[h], value);
        }
    }
    out += fmt::format("| actions active {}/{}; pressed:{}", active, total, states.empty() ? std::string(" none") : states);
    return out;
}
}
