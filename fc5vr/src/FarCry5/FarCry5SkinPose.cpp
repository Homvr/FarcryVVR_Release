// skinpose (FC2 VR item 1, route A; Homard 23 Oct 19:17, "a first test switch"). Today the weapon pass is placed on the
// controller by its camera: the pass's camera is turned and moved so that the game's first-person geometry (fixed to the
// game camera) shows at the controller. That is one world-space move of everything in the pass,
//     world' = Mw(world) = A (world - cam) + off,  A = Wc Cr wrot, off = controller + Wc (Ct - grip)
// (on_viewport_constants: w_full and w_off; gunstill, gunanchor and the grip calibration are folded in).
//  - skinpose on|off (default off, saved; F11 Motion control > Weapon placement): the weapon pass's camera stays the eye's,
//    and each weapon-pass draw gets the same move in its own object constants (VS slot 1, a 3x4 world matrix Om in rows):
//    Om' = Mw Om, on a stand-in buffer bound for that draw only. Skinned meshes (arms, gloves, gun parts with a palette)
//    and rigid ones (object constants only) are moved alike, since world = Om (palette (rest)). The mod's hand edits change
//    the palettes in the object space the game draws them in, so they land where they do today. Stereo only (AFW's held eye
//    rebuilds the weapon pass with its own camera: off there).
//  - skinposelog N (test): N weapon-pass draws logged (slot 1 and slot 3 sizes, the 32 floats of slot 1), and for each frame
//    while it runs the order of the palette writer's calls (FC_m64+0x6ac03f0, hooked), the weapon camera upload and the
//    weapon pass's draws, by one sequence number.
#include "FarCry5Stereo.h"
#include "FarCry5Record.h"

#include <glm/gtc/matrix_access.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// The original of a hooked context method (as in FarCry5Stereo.cpp).
template <typename R, typename... A>
static R call_orig(safetyhook::InlineHook& h, int idx, ID3D11DeviceContext* ctx, A... a)
{
    if (fc5::rec::is_deferred(ctx)) return reinterpret_cast<R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...)>(fc5::rec::deferred_method(idx))(ctx, a...);
    return h.call<R>(ctx, a...);
}

// The palette writer's call order (FarCry5Stereo.cpp, next to skinsrc).
namespace fc5::skinorder
{
    bool     install(const std::atomic<int>* frame);
    void     set(bool on);
    uint64_t next();
    bool     calls(int frame, uint64_t& first, uint64_t& last, uint32_t& n, unsigned long& tid);
}

bool FarCry5Stereo::skin_pose_active() const { return m_skin_pose.load() && m_motion.load() && !afw_mode(); }

bool FarCry5Stereo::skin_pose_command(const std::string& cmd, const std::string& arg, bool& persist)
{
    const bool on = arg == "on" || arg == "1" || arg == "true";
    if (cmd == "skinpose") {   // skinpose on|off (saved)
        m_skin_pose = on;
        persist     = true;
        spdlog::info("[SkinPose] skinpose {}{}", on ? "on" : "off", on && afw_mode() ? " (AFW: not applied, Stereo only)" : "");
        return true;
    }
    if (cmd == "skinposelog") {   // skinposelog N (test)
        const int n = arg.empty() ? 60 : std::max(0, std::atoi(arg.c_str()));
        if (n > 0 && !fc5::skinorder::install(&m_frame)) spdlog::info("[SkinPose] skinposelog: the palette writer could not be hooked: no call order");
        fc5::skinorder::set(n > 0);
        m_skp_log = n;
        return true;
    }
    return false;
}

// The weapon camera upload (on_viewport_constants, weapon view with motion controls): this frame's move, and the floating camera
// it replaces (fvrp = projection x rotation, fcam its position), kept per frame for the motion vectors.
void FarCry5Stereo::skin_pose_camera(int frame, const glm::mat3& A, const glm::vec3& cam, const glm::vec3& off, const glm::mat4& fvrp, const glm::vec3& fcam)
{
    m_skp.frame = frame;
    m_skp.A     = A;
    m_skp.cam   = cam;
    m_skp.off   = off;
    m_skp.fvrp  = fvrp;
    m_skp.fcam  = fcam;
    m_skp.seq   = m_skp_log.load() > 0 ? fc5::skinorder::next() : 0;
    m_skp_hist[frame & 3] = m_skp;
}

// Motion vectors: the weapon pass's previous camera, as the floating camera's (the same eye two frames back with sameeyeprev, else
// the last frame's, as on_viewport_constants picks it), seen through this frame's move. A point the draws now place at
// Mw(x) = A (x - cam) + off maps to Vpw (x - Cpw), the floating camera's previous view of the game's x: so the previous camera is
// V' = Vpw A^-1 at C' = Mw(Cpw). (Its own motion, slot 1's floats 16-18, is turned by A in skin_pose_draw.)
void FarCry5Stereo::skin_pose_prev(int frame, float* vrp_pv, float* cam_pv)
{
    if (m_skp.frame != frame) return;
    const SkinPoseMove* p  = nullptr;
    const auto&         h2 = m_skp_hist[(frame - 2) & 3];
    const auto&         h1 = m_skp_hist[(frame - 1) & 3];
    if (m_same_eye_prev.load() && h2.frame == frame - 2) p = &h2;
    else if (h1.frame == frame - 1) p = &h1;
    const glm::mat4 Vpw = p ? p->fvrp : m_skp.fvrp;
    const glm::vec3 Cpw = p ? p->fcam : m_skp.fcam;
    const glm::mat4 V   = Vpw * glm::mat4{ glm::inverse(m_skp.A) };
    const glm::vec3 C   = m_skp.A * (Cpw - m_skp.cam) + m_skp.off;
    for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) vrp_pv[r * 4 + col] = V[col][r];
    cam_pv[0] = C.x;
    cam_pv[1] = C.y;
    cam_pv[2] = C.z;
    if (m_skp_log.load() > 0) {
        // Check: a game point 0.5 m in front of the game camera, its previous clip position both ways.
        const glm::vec3 x  = m_skp.cam + glm::vec3{ 0.3f, 0.4f, -0.2f };
        const glm::vec4 a  = Vpw * glm::vec4{ x - Cpw, 1.0f };
        const glm::vec4 b  = V * glm::vec4{ m_skp.A * (x - m_skp.cam) + m_skp.off - C, 1.0f };
        const float     da = glm::length(glm::vec2{ a } / std::max(1e-6f, a.w) - glm::vec2{ b } / std::max(1e-6f, b.w));
        spdlog::info("[SkinPose] frame {}: previous weapon camera from frame {} ({}); check, a point's previous screen position floating vs skinpose: {:.2e} (NDC)", frame,
                     p ? p->frame : frame, p == &h2 ? "same eye" : p ? "last frame" : "none: this frame's", da);
    }
}

// A draw about to be made (weapon_draw, after weapon_draw_impl: the hand edits have read and swapped what they need).
void FarCry5Stereo::skin_pose_draw(ID3D11DeviceContext* ctx, UINT count)
{
    if (m_skp_swapped || !m_vs_cb0 || !is_weapon_cb(m_vs_cb0) || !skin_pose_active()) return;
    const int  frame = m_frame.load();
    const bool log   = m_skp_log.load() > 0;
    if (log) {
        const uint64_t q = fc5::skinorder::next();
        if (m_skp_draw_frame != frame) {
            m_skp_draw_frame = frame;
            m_skp_draw_first = q;
            m_skp_draw_n     = 0;
        }
        m_skp_draw_last = q;
        m_skp_draw_n++;
    }
    ID3D11Buffer* cb[3]{};
    ctx->VSGetConstantBuffers(1, 3, cb);   // slots 1, 2, 3
    ObjConsts o1{}, o3{};
    bool      h1 = false, h3 = false;
    {
        std::lock_guard _{ m_obj_mtx };
        if (cb[0]) if (auto it = m_obj_consts.find(cb[0]); it != m_obj_consts.end()) { o1 = it->second; h1 = true; }
        if (cb[2]) if (auto it = m_obj_consts.find(cb[2]); it != m_obj_consts.end()) { o3 = it->second; h3 = true; }
    }
    for (auto* b : cb) if (b) b->Release();
    const UINT sig = m_weapon_sig.load();
    auto&      c   = m_skp_counts[sig];
    if (c.pass_frame != frame) {
        c.pass_frame = frame;
        c.passes++;
    }
    const bool skinned = h3 && o3.size == 12288;
    (skinned ? c.skinned : c.rigid)++;
    auto log_draw = [&](const char* what) {
        if (!log) return;
        std::string fl;
        for (uint32_t i = 0; i < std::min<uint32_t>(32, o1.size / 4); ++i) fl += fmt::format("{:.8g} ", o1.f[i]);
        spdlog::info("[SkinPose] frame {} draw {} ({} indices, weapon {}): {}; slot 1 {} B, slot 3 {} B ({}) | {}", frame, m_skp_draw_n, count, sig, what, h1 ? o1.size : 0,
                     h3 ? o3.size : 0, skinned ? "skinned" : "rigid", fl);
        if (--m_skp_log <= 0) fc5::skinorder::set(false);
    };
    if (m_skp.frame != frame) {
        m_skp_stale++;
        log_draw("no weapon camera upload this frame: left");
        return;
    }
    if (!h1 || o1.size < 48 || o1.size > 128 || (o1.size & 15) != 0) {
        m_skp_no_obj++;
        c.left++;
        log_draw("no object constants: left");
        return;
    }
    glm::mat3 R{ 1.0f };
    glm::vec3 t{ 0.0f };
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) R[col][r] = o1.f[r * 4 + col];
        t[r] = o1.f[r * 4 + 3];
    }
    const float det = glm::determinant(R);
    if (!std::isfinite(det + t.x + t.y + t.z) || std::abs(det) < 0.05f || std::abs(det) > 20.0f || glm::length(t - m_skp.cam) > 10.0f) {
        c.left++;
        log_draw("slot 1 is not an object matrix near the camera: left");
        return;
    }
    const glm::mat3 R2 = m_skp.A * R;
    const glm::vec3 t2 = m_skp.A * (t - m_skp.cam) + m_skp.off;
    float           data[32]{};
    std::memcpy(data, o1.f, o1.size);
    for (int r = 0; r < 3; ++r) {
        for (int col = 0; col < 3; ++col) data[r * 4 + col] = R2[col][r];
        data[r * 4 + 3] = t2[r];
    }
    if (o1.size >= 80) {
        // Floats 16-18: the low part of the world position (~1e-4: one float step at 3000 m is 2.4e-4; it does not follow the
        // object's motion, simulator 23 Oct), turned with the move, plus the rounding of the new translation (in doubles).
        const glm::vec3 lo = m_skp.A * glm::vec3{ o1.f[16], o1.f[17], o1.f[18] };
        for (int r = 0; r < 3; ++r) {
            double v = double(m_skp.off[r]);
            for (int col = 0; col < 3; ++col) v += double(m_skp.A[col][r]) * (double(t[col]) - double(m_skp.cam[col]));
            data[16 + r] = float(double(lo[r]) + (v - double(data[r * 4 + 3])));
        }
    }
    ID3D11Buffer*& standin = m_skp_standins[o1.size];
    if (!standin) {
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth      = o1.size;
        sd.Usage          = D3D11_USAGE_DEFAULT;
        sd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) {
            dev->CreateBuffer(&sd, nullptr, &standin);
            dev->Release();
        }
        if (!standin) return;
    }
    call_orig<void>(m_update_sub_hook, 48, ctx, static_cast<ID3D11Resource*>(standin), 0u, static_cast<const D3D11_BOX*>(nullptr), static_cast<const void*>(data), 0u, 0u);
    ctx->VSGetConstantBuffers(1, 1, &m_skp_orig);
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 1u, static_cast<ID3D11Buffer* const*>(&standin));
    m_skp_swapped = true;
    c.moved++;
    log_draw("moved");
}

void FarCry5Stereo::skin_pose_restore(ID3D11DeviceContext* ctx)
{
    m_skp_swapped = false;
    call_orig<void>(m_vs_cb_hook, 7, ctx, 1u, 1u, static_cast<ID3D11Buffer* const*>(&m_skp_orig));
    if (m_skp_orig) m_skp_orig->Release();
    m_skp_orig = nullptr;
}

// Each present: the call order of the frame just drawn (skinposelog), and every 10 s the draw counts per weapon.
void FarCry5Stereo::skin_pose_present()
{
    if (m_skp_draw_frame >= 0 && m_skp_order_logged != m_skp_draw_frame && (m_skp_log.load() > 0 || m_skp_order_tail > 0)) {
        m_skp_order_logged = m_skp_draw_frame;
        if (m_skp_log.load() > 0) m_skp_order_tail = 3;
        else m_skp_order_tail--;
        const int     f = m_skp_draw_frame;
        uint64_t      w0 = 0, w1 = 0, p0 = 0, p1 = 0;
        uint32_t      wn = 0, pn = 0;
        unsigned long wt = 0, pt = 0;
        const bool    w  = fc5::skinorder::calls(f, w0, w1, wn, wt);
        const bool    pw = fc5::skinorder::calls(f - 1, p0, p1, pn, pt);
        spdlog::info("[SkinPose] order, frame {}: weapon camera upload seq {} (frame {}); weapon-pass draws seq {}-{} ({}); palette writer calls this frame {} (seq {}-{}, thread {}), "
                     "last frame {} (seq {}-{}); render thread {}",
                     f, m_skp.frame == f ? m_skp.seq : 0, m_skp.frame, m_skp_draw_first, m_skp_draw_last, m_skp_draw_n, w ? wn : 0, w0, w1, wt, pw ? pn : 0, p0, p1, GetCurrentThreadId());
    }
    if (!skin_pose_active()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - m_skp_stat_t < std::chrono::seconds(10)) return;
    m_skp_stat_t = now;
    for (const auto& [sig, c] : m_skp_counts) {
        if (c.passes == 0) continue;
        spdlog::info("[SkinPose] weapon {}: {} passes, draws per pass skinned {:.1f} rigid {:.1f}; moved {}, left as drawn {}", sig, c.passes, double(c.skinned) / c.passes,
                     double(c.rigid) / c.passes, c.moved, c.left);
    }
    spdlog::info("[SkinPose] draws without this frame's weapon camera {}, without object constants {}", m_skp_stale, m_skp_no_obj);
}

void FarCry5Stereo::draw_skin_pose_menu(const std::function<void(const char*, std::string)>& keep, const std::function<void(const char*)>& hint,
                                        const std::function<void(const char*)>& section)
{
    section("Weapon placement (test)");
    bool v = m_skin_pose.load();
    if (ImGui::Checkbox("Move the gun and hands in their own draws (test)", &v)) {
        m_skin_pose = v;
        keep("skinpose", v ? "skinpose on" : "skinpose off");
    }
    hint("Test of another way of holding the gun (23 Oct). Off (default): the gun, the arms and the hands are placed on the controller by turning the "
         "camera of the game's weapon picture. On: that camera stays your eye's, and every piece of the gun and hands is moved onto the controller in its "
         "own draw, with the pose the game gives it that same draw. Stereo only (no effect in AFW). Look for: the gun in the left and right eye in the same "
         "place, no doubling or lag of the gun when turning or walking, the hands still on the controllers.");
}
