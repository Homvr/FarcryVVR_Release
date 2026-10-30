// planelook (Homard's 27 Oct playtest: "When in a plane I could not look down, and when going fully up or down I could barely look
// on the side"). airfollow reads the vehicle's attitude from the game camera with the head look offset it carries taken out, and the
// eyes hang on that attitude. The game does not apply all of a vehicle's head look (vehflyguard's 25 Oct log: a parked car read 80 deg
// of tilt with the head 74 deg down), so looking far down read as the plane nosing up and the view was pushed back up; with the nose
// straight up or down the offset no longer turns the camera the way it was handed, and looking aside was taken back the same way.
// Cars have vehpitchguard; an aircraft's nose moves too fast for it.
//  - planelook off|fly|all (default fly, saved; F11 Comfort > Cinematics and vehicles): once the vehicle flies (veh_flying), its
//    attitude comes from its own model (the largest Vehicle_ draw within 8 m of the camera) turned onto the seat's frame, learnt while
//    the head looks ahead (both offsets under 8 deg, the vehicle within 25 deg of level). The model of a frame is known only after the
//    frame's camera, so what is carried onto this frame's camera is the last frame's correction (the model's attitude against the
//    camera's reading of that same frame): the plane's own turn keeps no lag, only what the game held back is put right. A correction
//    that jumps over 10 deg in a frame (another part of the plane taken for it) is refused. all: every vehicle, cars too (vehpitchguard
//    is then skipped), a test. Off: as before 27 Oct.
//  - The log prints a [Vehicle] planelook line every 5 s while it acts: how far the game held its camera off the head look.
#include "FarCry5Stereo.h"

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace
{
    float quat_deg(const glm::quat& a, const glm::quat& b)
    {
        return glm::degrees(2.0f * std::acos(std::clamp(std::abs(glm::dot(a, b)), 0.0f, 1.0f)));
    }
}

bool FarCry5Stereo::planelook_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    if (cmd == "vehnose") {   // vehnose on|off (saved): the front learnt at the entry's settle is the model's forward axis
        m_veh_nose = arg != "off" && arg != "0";
        spdlog::info("[Vehicle] vehnose {}", m_veh_nose.load() ? "on (the front from the vehicle model's forward axis)" : "off (the model axis nearest the camera)");
        persist = true;
        return true;
    }
    if (cmd != "planelook") return false;
    m_plane_look = arg == "off" || arg == "0" ? 0 : arg == "all" || arg == "2" ? 2 : 1;   // planelook off|fly|all (saved)
    spdlog::info("[Vehicle] planelook {}", m_plane_look.load() == 0 ? "off" : m_plane_look.load() == 2 ? "all (every vehicle)" : "fly (flying vehicles)");
    persist = true;
    return true;
}

void FarCry5Stereo::planelook_reset()
{
    for (auto& r : m_plane_mdl) r = PlaneRec{};
    for (auto& r : m_plane_cam) r = PlaneRec{};
    m_plane_seat       = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    m_plane_seat_n     = 0;
    m_plane_seat_axes  = false;
    if (m_stick_view.load()) {
        // stickview: the seat starts as the model's axes (every vehicle's front is its y in the logs, z up, x right) -> the view's
        // (x right, y up, -z ahead), so a helicopter that takes off within seconds is not left on the game camera. The first look
        // ahead checks it (plane_look_fix).
        m_plane_seat      = glm::normalize(glm::quat_cast(glm::mat3{ glm::vec3{ 1.0f, 0.0f, 0.0f }, glm::vec3{ 0.0f, 0.0f, -1.0f }, glm::vec3{ 0.0f, 1.0f, 0.0f } }));
        m_plane_seat_n    = 10;
        m_plane_seat_axes = true;
    }
    m_plane_seat_bad   = 0;
    m_plane_fix        = glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
    m_plane_fix_frame  = -100;
    m_plane_fix_bad    = 0;
    m_plane_blend      = 0.0f;
}

// From vehicle_probe, for each Vehicle_ draw of the world pass within 8 m of the camera while in a vehicle: the largest of the
// frame is kept (the body, not a door, a wheel or a propeller).
void FarCry5Stereo::plane_model_probe(int frame, const float* f, UINT count, float dist)
{
    auto& w = m_plane_mdl[frame & 3];
    if (w.frame == frame && count <= w.count) return;
    // The model's axes without its scale (its handedness kept), as afwvehown.
    const glm::vec3 x  = glm::normalize(glm::vec3{ f[0], f[4], f[8] });
    const glm::vec3 y0 = glm::vec3{ f[1], f[5], f[9] };
    const glm::vec3 y  = glm::normalize(y0 - glm::dot(y0, x) * x);
    glm::vec3       z  = glm::cross(x, y);
    if (glm::dot(z, glm::vec3{ f[2], f[6], f[10] }) < 0.0f) z = -z;
    if (!std::isfinite(x.x + y.x + z.x + x.y + y.y + z.y + x.z + y.z + z.z)) return;
    w.frame = frame;
    w.count = count;
    w.rot   = glm::transpose(glm::mat3{ x, y, z });   // world -> the model's axes
    w.dist  = dist;
}

// From update_motion's airfollow: a_cam = the vehicle's aim frame read from this frame's camera less its head look (world -> view).
// Returns the attitude the eyes and the head look use.
glm::mat3 FarCry5Stereo::plane_look_fix(int frame, const glm::mat3& a_cam)
{
    const int  mode = m_plane_look.load();
    const bool want = mode == 2 || (mode == 1 && veh_flying());
    auto&      cr   = m_plane_cam[frame & 3];
    cr.frame        = frame;
    cr.rot          = a_cam;
    {
        // Head ahead and the vehicle near level: the camera's reading is the seat's own frame (nothing held back).
        const glm::vec3 fw = glm::transpose(a_cam) * glm::vec3{ 0.0f, 0.0f, -1.0f };
        const glm::vec3 up = glm::transpose(a_cam) * glm::vec3{ 0.0f, 1.0f, 0.0f };
        const float     lim = glm::radians(8.0f);
        cr.count = std::abs(m_mo.ev_oy) < lim && std::abs(m_mo.ev_op) < lim && std::abs(fw.z) < 0.42f && up.z > 0.9f ? 1 : 0;   // 25 deg
        // stickview: not while the right stick turns the game's camera ahead of the aircraft (nor 0.5 s after).
        if (m_stick_view.load() && GetTickCount64() - m_veh_rstick_t.load() < 500) cr.count = 0;
    }
    // Pairs: one correction for both eyes, chosen at the pair's first frame (from the last pair's second frame: one game update).
    const bool pairs = m_pair_body.load() && m_frame_double.load() && m_stereo2.load();
    if (!pairs || (frame & 1) == 0) {
        int m = -1;
        for (int k = 1; k <= 3 && m < 0; ++k) {
            const int g = frame - k;
            if (m_plane_mdl[g & 3].frame == g && m_plane_cam[g & 3].frame == g) m = g;
        }
        if (m >= 0) {
            const PlaneRec& md = m_plane_mdl[m & 3];
            const PlaneRec& ca = m_plane_cam[m & 3];
            if (ca.count == 1) {
                // The seat: the model's axes -> the aim frame's, while the head looks ahead.
                const glm::quat s = glm::normalize(glm::quat_cast(ca.rot * glm::transpose(md.rot)));
                if (m_plane_seat_axes) {
                    // stickview: the first look ahead against the model's axes: close, the look takes over and keeps learning; far,
                    // the axes were wrong for this vehicle and the seat is learnt from the camera as before.
                    const float d     = quat_deg(s, m_plane_seat);
                    m_plane_seat      = s;
                    m_plane_seat_n    = d < 25.0f ? 10 : 1;
                    m_plane_seat_axes = false;
                    spdlog::info("[Vehicle] planelook: the first look ahead is {:.1f} deg off the model's axes ({})", d,
                                 d < 25.0f ? "kept, learning on" : "the axes dropped, the seat learnt from the camera");
                } else if (m_plane_seat_n == 0) {
                    m_plane_seat   = s;
                    m_plane_seat_n = 1;
                } else if (quat_deg(s, m_plane_seat) < 6.0f) {
                    m_plane_seat = glm::normalize(glm::slerp(m_plane_seat, s, m_plane_seat_n < 30 ? 1.0f / float(m_plane_seat_n + 1) : 0.05f));
                    if (++m_plane_seat_n == 10)
                        spdlog::info("[Vehicle] planelook: the seat learnt from the vehicle's model ({} indices, {:.1f} m from the camera){}", md.count, md.dist,
                                     want ? "" : " (not used until the vehicle flies)");
                    m_plane_seat_bad = 0;
                } else if (++m_plane_seat_bad > 45) {
                    spdlog::info("[Vehicle] planelook: the model read {:.0f} deg off the learnt seat for 45 looks ahead: learnt again ({} indices, {:.1f} m)",
                                 quat_deg(s, m_plane_seat), md.count, md.dist);
                    m_plane_seat     = s;
                    m_plane_seat_n   = 1;
                    m_plane_seat_bad = 0;
                }
            }
            if (m_plane_seat_n >= 10) {
                // What the game held back: the model's attitude against the camera's reading, both of frame m (view -> view).
                const glm::quat fix = glm::normalize(glm::quat_cast(glm::mat3_cast(m_plane_seat) * md.rot * glm::transpose(ca.rot)));
                if (frame - m_plane_fix_frame > 30 || quat_deg(fix, m_plane_fix) < 10.0f || ++m_plane_fix_bad > 30) {
                    m_plane_fix       = fix;
                    m_plane_fix_frame = m;
                    m_plane_fix_bad   = 0;
                } else {
                    m_plane_refused++;
                }
            }
        }
        const auto  now = std::chrono::steady_clock::now();
        const float dt  = std::clamp(std::chrono::duration<float>(now - m_plane_t).count(), 0.0f, 0.1f);
        m_plane_t       = now;
        const bool ok   = want && m_plane_seat_n >= 10 && frame - m_plane_fix_frame <= 8;
        if (frame - m_plane_fix_frame > 60) m_plane_blend = 0.0f;   // back after a pause (menu, head look off): no old correction faded out
        m_plane_blend   =std::clamp(m_plane_blend + (ok ? 1.0f : -1.0f) * dt / 0.5f, 0.0f, 1.0f);
        if (want && !ok) m_plane_nomodel++;
        if (m_plane_blend > 0.0f) {
            const float d = quat_deg(m_plane_fix, glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f });
            m_plane_used++;
            m_plane_fix_sum += d;
            m_plane_fix_max = std::max(m_plane_fix_max, d);
        }
        if (want && now - m_plane_log_t > std::chrono::seconds(5)) {
            m_plane_log_t = now;
            spdlog::info("[Vehicle] planelook: {} frames from the vehicle's model (blend {:.2f}), the game's camera held {:.1f} deg off the head look on average (max {:.1f}); "
                         "head look offset now yaw {:.0f} pitch {:.0f} deg; seat from {} looks ahead; {} frames without the model, {} refused",
                         m_plane_used, m_plane_blend, m_plane_used ? m_plane_fix_sum / float(m_plane_used) : 0.0f, m_plane_fix_max, glm::degrees(m_mo.ev_oy),
                         glm::degrees(m_mo.ev_op), m_plane_seat_n, m_plane_nomodel, m_plane_refused);
            m_plane_used = m_plane_nomodel = m_plane_refused = 0;
            m_plane_fix_sum = m_plane_fix_max = 0.0f;
        }
    }
    if (m_plane_blend <= 0.0f) return a_cam;
    return glm::mat3_cast(glm::slerp(glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f }, m_plane_fix, m_plane_blend)) * a_cam;
}

// F11 Comfort > Cinematics and vehicles, under "In vehicles, the view".
void FarCry5Stereo::draw_planelook_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint)
{
    int                pl = std::clamp(m_plane_look.load(), 0, 2);
    static const char* kPlane[]{ "Off (as before 27 Oct)", "Flying vehicles (default)", "Every vehicle (test)" };
    if (ImGui::Combo("Free head look from the vehicle's model", &pl, kPlane, 3)) {
        m_plane_look = pl;
        keep("planelook", pl == 0 ? "planelook off" : pl == 2 ? "planelook all" : "planelook fly");
    }
    hint("The game does not turn its camera all the way where you look in a vehicle: in a plane looking down pushed the view back up, and with "
         "the nose straight up or down you could barely look aside. Flying vehicles: the plane or helicopter's attitude is read from its own model, "
         "so you look anywhere. Every vehicle: cars, quads and boats too (a test). Off: as before 27 Oct.");
    bool vn = m_veh_nose.load();
    if (ImGui::Checkbox("Getting in: face the vehicle's nose", &vn)) {
        m_veh_nose = vn;
        keep("vehnose", vn ? "vehnose on" : "vehnose off");
    }
    hint("After the get-in animation the view faces the vehicle's front, read from its own model, even when the game's camera still looks at "
         "the door (planes). Off: the front is the side of the vehicle nearest where the game's camera looked (a plane's front was found only at take-off).");
}
