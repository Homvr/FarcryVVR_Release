#include "ModConfig.h"
#include "Mods.hpp"
#include <mods/VR.hpp>
#include <mods/VRConfig.hpp>
#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif

#include "ModSettings.h"
#include "FarCry5/FarCry5Stereo.h"
#include "FarCry5/FarCry5Probe.h"

namespace ModSettings {
    // HudScale g_hudScale;
    // CrosshairTranslate g_crosshairTranslate;
    // AW2InternalSettings g_aw2_settings;
    // AW2GameFlow g_game_state;
    // DebugAndCalibration g_debugAndCalibration;
}

Mods::Mods() {
    m_mods.emplace_back(VRConfig::get());
    // Before VR: its on_present prepares the AFW held eye that VR's submission then uses.
    m_mods.emplace_back(FarCry5Stereo::Get());
    m_mods.emplace_back(VR::get());
#ifdef _DEBUG
    m_mods.emplace_back(ShaderDebugOverlay::Get());
#endif
}




// The game's pad rumble on the controllers. A pad has a heavy left motor and a light right one, and the game puts its
// shots mostly on the heavy one: the framework's motor-by-side routing (rumble game) buzzed only the left hand when firing
// (tester, Homard 24 Oct). FarCry5Stereo::rumble_hands decides which hand gets what (rumble gun|both|game, rumblegain).
void VR::on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) {
    if (user_index != m_lowest_xinput_user_index || vibration == nullptr) {
        return;
    }
    if (!is_using_controllers()) {
        return;
    }
    float amp[2]{};   // the left controller, the right controller
    FarCry5Stereo::Get()->rumble_hands(vibration->wLeftMotorSpeed / 65535.0f, vibration->wRightMotorSpeed / 65535.0f, amp);
    if (amp[0] > 0.0f) trigger_haptic_vibration(0.0f, 0.1f, 1.0f, amp[0], get_left_joystick());
    if (amp[1] > 0.0f) trigger_haptic_vibration(0.0f, 0.1f, 1.0f, amp[1], get_right_joystick());
}

void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
//    ZoneScopedN(__FUNCTION__);
    // The hooked XInputGetState hands over a whole XINPUT_STATE: the pad is after the packet number
    // (the Starfield code this comes from passed the pad itself; cast here, every write landed 4 bytes
    // early: stick X on the buttons, stick Y on the triggers).
    auto pXinputGamepad = &state->Gamepad;
    // The game only polls XInput while some pad is plugged in, so the mod plugs in a virtual one
    // (ViGEm); the state the game reads is still built here, in the hook: the controllers replace
    // whatever the polled pad reports and every other pad is hidden.
    const bool from_pad = FarCry5Stereo::Get()->in_pad_update();
    const uint32_t probe_ret_in = *retval;
    struct ProbeOnExit
    {
        uint32_t* r; uint32_t in; uint32_t idx; XINPUT_GAMEPAD* g;
        ~ProbeOnExit() { fc5::probe::note_xinput(FarCry5Stereo::Get()->in_pad_update(), false, idx, in, *r, g); }
    } probe_on_exit{ retval, probe_ret_in, user_index, pXinputGamepad };

    const auto now = std::chrono::steady_clock::now();

    if (!from_pad) {
    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }
        if (is_using_controllers()) {
            // Only the pad the VR controllers drive exists for the game (a headset runtime's own
            // gamepad emulation would add its own buttons and sticks).
            *retval = ERROR_DEVICE_NOT_CONNECTED;
            std::memset(state, 0, sizeof(*state));
        }
        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }

    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;
    }

    if (is_using_controllers_within(std::chrono::minutes(5)))
        if (is_using_controllers_within(std::chrono::minutes(5)))
        {
            *retval = ERROR_SUCCESS;

        }

    // Tests (gesturegrip, throwtest): the simulated hands count as controller use (the simulator's controllers send no input).
    if (fc5::gesture::test_input()) m_last_controller_update = now;
    // padsim (test command): grips and sticks of the physical left / right controller replaced.
    const auto pad_sim = FarCry5Stereo::Get()->pad_sim();
    if (pad_sim.on) m_last_controller_update = now;
    const auto axis_of = [&](vr::VRInputValueHandle_t h) {
        if (!pad_sim.on) return get_joystick_axis(h);
        const float* a = h == m_left_joystick ? pad_sim.l : pad_sim.r;
        return Vector2f{ a[0], a[1] };
    };
    const auto grip_of = [&](vr::VRInputValueHandle_t h) {
        if (!pad_sim.on) return is_action_active(m_action_grip, h);
        return h == m_left_joystick ? pad_sim.lgrip : pad_sim.rgrip;
    };
    if (!is_using_controllers()) {
        return;
    }
    // The VR controllers are the whole pad: whatever a physical or emulated pad in this slot reports
    // is dropped (the packet number must still change for the game to take the new state).
    {
        static DWORD packet = 0;
        std::memset(pXinputGamepad, 0, sizeof(*pXinputGamepad));
        state->dwPacketNumber = ++packet;
    }

//    // Clear button state for VR controllers
//    if (is_using_controllers_within(std::chrono::seconds(5))) {
//        state->Gamepad.wButtons = 0;
//        state->Gamepad.bLeftTrigger = 0;
//        state->Gamepad.bRightTrigger = 0;
//        state->Gamepad.sThumbLX = 0;
//        state->Gamepad.sThumbLY = 0;
//        state->Gamepad.sThumbRX = 0;
//        state->Gamepad.sThumbRY = 0;
//    }


    const auto wants_swap = m_swap_controllers->value();
    const auto left_joystick = !wants_swap ? get_left_joystick():get_right_joystick();
    const auto right_joystick = !wants_swap? get_right_joystick():get_left_joystick();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    static bool ui_toggle_active = false;
    bool both_clicks_down = is_left_joystick_click_down && is_right_joystick_click_down;

    if (both_clicks_down && !ui_toggle_active && !Framework::s_fc5_test_mode.load()) {
        ui_toggle_active = true;
        g_framework->set_draw_ui(!g_framework->is_drawing_ui());
    } else if (!is_left_joystick_click_down || !is_right_joystick_click_down) {
        ui_toggle_active = false;
    }

    if (g_framework->is_drawing_ui()) {
        // The F11 menu: right stick scrolls / nudges sliders, the right trigger clicks.
        const auto axis = axis_of(right_joystick);
        FarCry5Stereo::Get()->set_menu_input(axis.x, axis.y, is_action_active(m_action_trigger, right_joystick));
        return;
    }
    FarCry5Stereo::Get()->set_menu_input(0.0f, 0.0f, false);

    // One button layout (the F11 Layout tab; the "A jump, B reload, X interact, Y switch weapon" choice was removed 24 Oct, Homard).
    if (is_right_a_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_A;
    }

    if (is_left_a_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_B;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_button_down = is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);

    if (is_right_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_Y;
    }

    if (is_left_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_X;
    }


    {
        if (is_left_joystick_click_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        }

        if (is_right_joystick_click_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        }
    }


    const auto is_left_trigger_down = is_action_active(m_action_trigger, left_joystick);
    const auto is_right_trigger_down = is_action_active(m_action_trigger, right_joystick);
    // Body gestures (FarCry5Gesture.cpp): a grip press that starts in a gesture's zone (the weapon hand at its shoulder:
    // switch weapon) taps the gesture's pad buttons, and that press is not the grip's usual action until released.
    const auto is_left_grip_raw  = grip_of(left_joystick);
    const auto is_right_grip_raw = grip_of(right_joystick);
    {
        // fingergrip: the index and the thumb follow the real ones (trigger pressed / touched; the thumb on a face button,
        // the thumbrest, or moving or pressing the stick).
        auto thumb_on = [&](vr::VRActionHandle_t a, vr::VRActionHandle_t b, vr::VRActionHandle_t rest, vr::VRInputValueHandle_t j) {
            const auto s = axis_of(j);
            return is_action_active(a, j) || is_action_active(b, j) || is_action_active(rest, j) || is_action_active(m_action_joystick_click, j) ||
                   std::abs(s.x) > 0.2f || std::abs(s.y) > 0.2f;
        };
        FarCry5Stereo::Get()->finger_inputs(is_right_trigger_down, is_left_trigger_down, is_action_active(m_action_trigger_touch, right_joystick),
                                            is_action_active(m_action_trigger_touch, left_joystick),
                                            thumb_on(m_action_a_button_touch_right, m_action_b_button_touch_right, m_action_thumbrest_touch_right, right_joystick),
                                            thumb_on(m_action_a_button_touch_left, m_action_b_button_touch_left, m_action_thumbrest_touch_left, left_joystick));
    }
    const bool lh                = FarCry5Stereo::Get()->left_handed();
    const auto gesture           = FarCry5Stereo::Get()->gesture_poll(lh ? is_left_grip_raw : is_right_grip_raw, lh ? is_right_grip_raw : is_left_grip_raw);
    const int  left_role         = lh ? fc5::gesture::kWeaponHand : fc5::gesture::kOtherHand;
    const int  right_role        = lh ? fc5::gesture::kOtherHand : fc5::gesture::kWeaponHand;
    if (gesture.buzz[left_role] > 0.0f) trigger_haptic_vibration(0.0f, gesture.buzz_s[left_role], 1.0f, gesture.buzz[left_role], left_joystick);
    if (gesture.buzz[right_role] > 0.0f) trigger_haptic_vibration(0.0f, gesture.buzz_s[right_role], 1.0f, gesture.buzz[right_role], right_joystick);
    const auto is_left_grip_button_down = is_left_grip_raw && !gesture.grip_taken[left_role];
    const auto is_right_grip_down       = is_right_grip_raw && !gesture.grip_taken[right_role];
    // Two-handed grip: the camera code grabs the weapon when the left grip is pressed at
    // the weapon. While the left hand holds it, the grip's other functions (left stick as
    // D-pad, grip combos) are suspended, so you can still walk and shoot.
    static std::chrono::steady_clock::time_point left_grip_pressed_at{};
    static bool                                  left_grip_was_down = false;
    const auto                                   input_now          = std::chrono::steady_clock::now();
    if (is_left_grip_button_down && !left_grip_was_down) {
        left_grip_pressed_at = input_now;
    }
    left_grip_was_down          = is_left_grip_button_down;
    // The two-handed hold (FarCry5Stereo) takes the off hand's grip; while it holds, the grip is not a modifier.
    FarCry5Stereo::Get()->set_other_grip(FarCry5Stereo::Get()->left_handed() ? is_right_grip_down : is_left_grip_button_down);
    // A press is no modifier until the two-handed hold has decided on it (one frame later in update_motion; 150 ms
    // when the motion code is not running): the first polls of a grab sent the D-pad (Homard 17 Oct 21:12, the
    // binoculars toggled at each grab while sprinting).
    const bool grab_attempt     = !pad_sim.old && !FarCry5Stereo::Get()->other_grip_decided() &&
                              input_now - left_grip_pressed_at < std::chrono::milliseconds(150);
    const auto is_left_grip_down = is_left_grip_button_down && !FarCry5Stereo::Get()->two_hand_held() && !grab_attempt;
    // A stick already pushed when the grip becomes the modifier (walking or turning while reaching for the gun, or the
    // hold ending while walking) keeps its own use until it comes back to the centre; then it is the D-pad.
    static bool grip_mod_was = false, grip_dpad_left = false, grip_dpad_right = false;
    {
        const auto la      = axis_of(m_left_joystick);
        const auto ra      = axis_of(right_joystick);
        const bool l_centre = std::abs(la.x) < 0.3f && std::abs(la.y) < 0.3f;
        const bool r_centre = std::abs(ra.x) < 0.3f && std::abs(ra.y) < 0.3f;
        if (!is_left_grip_down) {
            grip_dpad_left = grip_dpad_right = false;
        } else if (!grip_mod_was) {
            grip_dpad_left  = l_centre;
            grip_dpad_right = r_centre;
        } else {
            grip_dpad_left  = grip_dpad_left || l_centre;
            grip_dpad_right = grip_dpad_right || r_centre;
        }
        grip_mod_was = is_left_grip_down;
        if (pad_sim.old) grip_dpad_left = grip_dpad_right = is_left_grip_down;
    }


    if(!Framework::s_fc5_test_mode.load() && ((is_left_joystick_click_down && is_left_grip_down) || (GetAsyncKeyState(VK_F12) & 1))) {
        static std::chrono::steady_clock::time_point m_last_grip_click = std::chrono::steady_clock::now();
        if(std::chrono::steady_clock::now() - m_last_grip_click > std::chrono::milliseconds(500)) {
            m_last_grip_click                           = std::chrono::steady_clock::now();
            auto& flatScreen = ModSettings::g_internalSettings.forceFlatScreen;
            flatScreen = !flatScreen;
        }
    }

    const auto system_button_down = is_action_active_any_joystick(m_action_system_button);
    if(system_button_down) {
        if(is_left_grip_down) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_BACK;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
        } else {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_START;
            pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
        }

    } else {
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_START;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_BACK;
    }

    // Right grip: LT in a vehicle (brake / reverse) and on the flat screen. Right trigger: RT (fire, throttle).
    // On foot LT is aim down sights, which narrows the game camera the game culls with: the grip never sends it there
    // ("Right grip aims down sights", gripads, was removed 24 Oct, Homard).
    // scopelearn: a short aim-down-sights press to read a gun's scope zoom (FarCry5Scope.cpp).
    if ((is_right_grip_down && FarCry5Stereo::Get()->grip_sends_lt()) || FarCry5Stereo::Get()->debug_lt() || FarCry5Stereo::Get()->scope_probe_lt()) {
        pXinputGamepad->bLeftTrigger = 255;
    }
    FarCry5Stereo::Get()->note_lt(pXinputGamepad->bLeftTrigger > 128);
    FarCry5Stereo::Get()->note_grip(FarCry5Stereo::Get()->left_handed() ? is_left_grip_button_down : is_right_grip_down);

    if (is_right_trigger_down) {
        pXinputGamepad->bRightTrigger = 255;
    }

    // Left trigger: LB (hold: the weapon wheel on foot; shoot from a vehicle).
    if (is_left_trigger_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }


    // Left grip (the modifier) + right B: RB (throw), without resting a thumb on the thumbrest.
    if (is_left_grip_down && is_right_b_button_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        pXinputGamepad->wButtons &= ~XINPUT_GAMEPAD_Y;
    }

    // Thumbrest touches are no buttons (Homard 16 Oct: a resting thumb threw or opened the wheel).

    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up);

    if (is_dpad_up_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right);

    if (is_dpad_right_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down);

    if (is_dpad_down_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left);

    if (is_dpad_left_down) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = axis_of(left_joystick);
    const auto right_joystick_axis = axis_of(right_joystick);

    const auto true_left_joystick_axis = axis_of(m_left_joystick);
    const auto true_right_joystick_axis = axis_of(m_right_joystick);

    pXinputGamepad->sThumbLX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbLY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    pXinputGamepad->sThumbRX = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    pXinputGamepad->sThumbRY = (int16_t)std::clamp<float>(((float)pXinputGamepad->sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);


    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    if (is_left_grip_down) {

        float ty{0.0f};
        float tx{0.0f};
        //SHORT ThumbY{0};
        //SHORT ThumbX{0};
        // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.

        // grip_dpad_left / grip_dpad_right: that stick was centred since the grip became the modifier (above).
        const bool left_dpad  = grip_dpad_left;
        const bool right_dpad = grip_dpad_right;
        if (left_dpad) {
            ty = true_left_joystick_axis.y; // ? wants_swap
            tx = true_left_joystick_axis.x;
        }

        if (ty >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if (ty <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if (tx >= 0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if (tx <= -0.5f) {
            pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        // The other stick is the D-pad too while the grip is held, so the left stick can keep walking:
        // grip + right stick = D-pad (the right stick neither turns nor looks meanwhile).
        const auto other = right_joystick_axis;
        if (right_dpad && (std::abs(other.x) >= 0.5f || std::abs(other.y) >= 0.5f)) {
            if (other.y >= 0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            if (other.y <= -0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (other.x >= 0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (other.x <= -0.5f) pXinputGamepad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }
        if (right_dpad) {   // the right stick is the D-pad: it does not turn meanwhile
            pXinputGamepad->sThumbRX = 0;
            pXinputGamepad->sThumbRY = 0;
        }
        if (std::abs(tx) >= 0.5f || std::abs(ty) >= 0.5f) {
            // The left stick used as the D-pad (as before): it does not walk meanwhile.
            pXinputGamepad->sThumbLY = 0;
            pXinputGamepad->sThumbLX = 0;
        }
    }


    // walkbasis (FarCry5Walk.cpp): the left stick turned so forward is where the head looks, also with a weapon in hand.
    FarCry5Stereo::Get()->walk_left_stick(pXinputGamepad->sThumbLX, pXinputGamepad->sThumbLY);

    pXinputGamepad->wButtons |= gesture.buttons;
    {
        // Each D-pad press sent to the game, with what made it (the left grip as modifier, trigger touch, a gesture).
        static WORD dpad_was = 0;
        const WORD  dpad     = pXinputGamepad->wButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
        if (dpad != dpad_was) {
            spdlog::info("[Pad] D-pad {:x} (was {:x}): left grip modifier {} (two-handed hold {}, grip decided {}), gesture {:x}; sticks L ({:.2f},{:.2f}) R ({:.2f},{:.2f})",
                         dpad, dpad_was, is_left_grip_down, FarCry5Stereo::Get()->two_hand_held(), FarCry5Stereo::Get()->other_grip_decided(),
                         gesture.buttons & 0xF, true_left_joystick_axis.x, true_left_joystick_axis.y, right_joystick_axis.x, right_joystick_axis.y);
            dpad_was = dpad;
        }
    }
    FarCry5Stereo::Get()->note_rb((pXinputGamepad->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    FarCry5Stereo::Get()->note_pad_buttons(pXinputGamepad->wButtons);   // gunstill: reload (X), sprint (left stick press)

    // Motion controls: the right stick turns the mod's body frame instead of the game camera, which
    // follows the gun.
    // On the flat screen (menus, the world map) the controllers are a plain pad: sticks pan and zoom,
    // the trigger selects.
    // Driving: the right trigger is the throttle, never held back for the aim.
    const bool in_vehicle = FarCry5Stereo::Get()->in_vehicle();
    // Sling: a weapon hanging on the sling does not fire.
    if (gesture.slung) pXinputGamepad->bRightTrigger = 0;
    // Throwable: from the grab to the end of the throw the right trigger does not fire (the game is throwing).
    if (gesture.no_fire) pXinputGamepad->bRightTrigger = 0;
    const bool motion_view = FarCry5Stereo::Get()->motion_enabled() && !FarCry5Stereo::Get()->flat_screen_on();
    if (float wx = 0.0f, wy = 0.0f; FarCry5Stereo::Get()->wheel_test(wx, wy)) {
        pXinputGamepad->wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        pXinputGamepad->sThumbRX = (int16_t)(wx * 32767.0f);
        pXinputGamepad->sThumbRY = (int16_t)(wy * 32767.0f);
    }
    // Weapon wheel (wheelstick): LB held on foot opens it; the game picks the slot from the right stick.
    FarCry5Stereo::Get()->note_wheel(motion_view && !in_vehicle && (pXinputGamepad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    if (motion_view && FarCry5Stereo::Get()->wheel_open()) {
        FarCry5Stereo::Get()->set_trigger(false);
        pXinputGamepad->bRightTrigger = 0;
        FarCry5Stereo::Get()->set_turn_input(0.0f);
    } else if (motion_view) {
        // Fire waits for the game camera to reach the barrel line (see FarCry5Stereo::motion_post_present).
        FarCry5Stereo::Get()->set_trigger(!in_vehicle && pXinputGamepad->bRightTrigger > 128);
        if (!in_vehicle) pXinputGamepad->bRightTrigger = FarCry5Stereo::Get()->fire_open() ? 255 : 0;
        FarCry5Stereo::Get()->set_turn_input(float(pXinputGamepad->sThumbRX) / 32767.0f);
        // vehstick: piloting with head look the mouse no longer turns the aircraft: the right stick does (left / right only).
        const bool veh_stick = FarCry5Stereo::Get()->vehicle_stick_pass();
        // scopezoomstick: on foot the right stick's up / down is not sent to the game; it zooms a scope held with two hands.
        FarCry5Stereo::Get()->note_zoom_stick(in_vehicle ? 0.0f : right_joystick_axis.x, in_vehicle ? 0.0f : right_joystick_axis.y);
        if (!veh_stick) pXinputGamepad->sThumbRX = 0;
        pXinputGamepad->sThumbRY = 0;
        if (FarCry5Stereo::Get()->stick_aim()) {
            // The game camera is steered onto the barrel / head direction through the right stick.
            pXinputGamepad->sThumbRX = (int16_t)std::clamp(FarCry5Stereo::Get()->stick_x() * 32767.0f, -32767.0f, 32767.0f);
            pXinputGamepad->sThumbRY = (int16_t)std::clamp(FarCry5Stereo::Get()->stick_y() * 32767.0f, -32767.0f, 32767.0f);
        }
    }
}
