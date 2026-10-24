#define NOMINMAX

#include <algorithm>
#include <string_view>
#include "math/Math.hpp"
#include <ModSettings.h>
#include <Xinput.h>
#include <aer/ConstantsPool.h>
#include <fstream>
#include <imgui.h>
#include <utility/ScopeGuard.hpp>

#include "memory/FunctionHook.h"
#include "utility/Memory.hpp"
#include "utility/Module.hpp"
#include "utility/Registry.hpp"

#include "VR.hpp"
#include "Fc5Boot.hpp"

thread_local bool inside_gui_draw = false;

std::shared_ptr<VR>& VR::get() {
    static auto inst = std::make_shared<VR>();
    return inst;
}

// Called when the mod is initialized
std::optional<std::string> VR::on_initialize_d3d_thread() try {
    auto openvr_error = initialize_openvr();
    fc5_boot_step("OpenVR tried");
    fc5_boot_result("OpenVR tried", m_openvr->loaded ? 1 : 0);

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            spdlog::info("OpenVR failed to load: {}", *m_openvr->error);
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error = 
R"(OpenVR loaded first.
If you want to use OpenXR, remove the openvr_api.dll from your game folder, 
and place the openxr_loader.dll in the same folder.)";
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    m_init_finished = true;

    // all OK
    return Mod::on_initialize();
} catch(...) {
    spdlog::error("Exception occurred in VR::on_initialize()");
    fc5_crash_log_caught("VR::on_initialize_d3d_thread");

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

/*void VR::on_lua_state_created(sol::state& lua) {
    lua.new_usertype<VR>("VR",
        "get_controllers", &VR::get_controllers,
        "get_position", &VR::get_position,
        "get_velocity", &VR::get_velocity,
        "get_angular_velocity", &VR::get_angular_velocity,
        "get_rotation", &VR::get_rotation,
        "get_transform", &VR::get_transform,
        "get_left_stick_axis", &VR::get_left_stick_axis,
        "get_right_stick_axis", &VR::get_right_stick_axis,
        "get_current_eye_transform", &VR::get_current_eye_transform,
        "get_current_projection_matrix", &VR::get_current_projection_matrix,
        "get_transform_offset", &VR::get_transform_offset,
        "set_transform_offset", &VR::set_transform_offset,
        "recenter_view", &VR::recenter_view,
        "get_gui_rotation_offset", &VR::get_gui_rotation_offset,
        "set_gui_rotation_offset", &VR::set_gui_rotation_offset,
        "recenter_gui", &VR::recenter_gui,
        "get_action_set", &VR::get_action_set,
        "get_active_action_set", &VR::get_active_action_set,
        "get_action_trigger", &VR::get_action_trigger,
        "get_action_grip", &VR::get_action_grip,
        "get_action_joystick", &VR::get_action_joystick,
        "get_action_joystick_click", &VR::get_action_joystick_click,
        "get_action_a_button", &VR::get_action_a_button,
        "get_action_b_button", &VR::get_action_b_button,
        "get_action_weapon_dial", &VR::get_action_weapon_dial,
        "get_action_minimap", &VR::get_action_minimap,
        "get_action_block", &VR::get_action_block,
        "get_action_dpad_up", &VR::get_action_dpad_up,
        "get_action_dpad_down", &VR::get_action_dpad_down,
        "get_action_dpad_left", &VR::get_action_dpad_left,
        "get_action_dpad_right", &VR::get_action_dpad_right,
        "get_action_heal", &VR::get_action_heal,
        "get_left_joystick", &VR::get_left_joystick,
        "get_right_joystick", &VR::get_right_joystick,
        "is_using_controllers", &VR::is_using_controllers,
        "is_openvr_loaded", &VR::is_openvr_loaded,
        "is_openxr_loaded", &VR::is_openxr_loaded,
        "is_hmd_active", &VR::is_hmd_active,
        "is_action_active", &VR::is_action_active,
        "is_using_hmd_oriented_audio", &VR::is_using_hmd_oriented_audio,
        "toggle_hmd_oriented_audio", &VR::toggle_hmd_oriented_audio,
        "apply_hmd_transform", [](VR* vr, glm::quat& rotation, Vector4f& position) {
            vr->apply_hmd_transform(rotation, position);
        },
        "trigger_haptic_vibration", &VR::trigger_haptic_vibration,
        "get_last_render_matrix", &VR::get_last_render_matrix,
        "should_handle_pause", [](VR* vr) {
            return vr->get_runtime()->handle_pause;
        },
        "set_handle_pause", [](VR* vr, bool state) {
            return vr->get_runtime()->handle_pause = state;
        },
        "unhide_crosshair", &VR::unhide_crosshair
    );

    lua["vrmod"] = this;
}*/

std::optional<std::string> VR::initialize_openvr() {
    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
        spdlog::info("[VR] Could not load openvr_api.dll");

        m_openvr->dll_missing = true;
        m_openvr->error = "Could not load openvr_api.dll";
        return Mod::on_initialize();
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    const auto module_directory = Framework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        spdlog::info("Writing default binding file {}", it.first);

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    m_openxr = std::make_shared<runtimes::OpenXR>();

    spdlog::info("[VR] Initializing OpenXR");
    fc5_boot_step("OpenXR: loader");

    if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
        spdlog::info("[VR] Could not load openxr_loader.dll");

//        m_openxr->loaded = false;
//        m_openxr->error = "Could not load openxr_loader.dll";
//
//        return std::nullopt;
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    spdlog::info("[VR] Creating OpenXR instance");

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }

        {
            uint32_t ext_count = 0;
            std::vector<XrExtensionProperties> ext_props{};
            if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr) == XR_SUCCESS && ext_count > 0) {
                ext_props.resize(ext_count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, ext_props.data());
            }
            const bool has_refresh_rate = std::any_of(ext_props.begin(), ext_props.end(), [](const XrExtensionProperties& p) {
                return std::string_view{p.extensionName} == XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
            });
            if (has_refresh_rate) {
                extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
                m_openxr->enabled_extensions.insert(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            }
            spdlog::info("[VR] {} offers {} instance extensions; display refresh rate control (XR_FB_display_refresh_rate): {}", "OpenXR runtime", ext_count,
                has_refresh_rate ? "yes" : "no");
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        strcpy(instance_create_info.applicationInfo.applicationName, g_framework->get_game_name());
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        fc5_boot_step("OpenXR: instance");
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);
        fc5_boot_result("OpenXR: instance", result);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr instance");
    }
    
    // Step 2: Create a system
    spdlog::info("[VR] Creating OpenXR system");

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        fc5_boot_step("OpenXR: system");
        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);
        fc5_boot_result("OpenXR: system", result);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr system");
    }

    // Step 3: Create a session
    spdlog::info("[VR] Initializing graphics info");

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    spdlog::info("[VR] Creating OpenXR session");
    session_create_info.systemId = m_openxr->system;
    fc5_boot_step("OpenXR: session");
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);
    fc5_boot_result("OpenXR: session", result);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    // Step 4: Create a space
    spdlog::info("[VR] Creating OpenXR space");
    fc5_boot_step("OpenXR: space");

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    spdlog::info("[VR] Getting OpenXR system properties");

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    spdlog::info("[VR] OpenXR system Name: {}", system_properties.systemName);
    spdlog::info("[VR] OpenXR system Vendor: {}", system_properties.vendorId);
    spdlog::info("[VR] OpenXR system max width: {}", system_properties.graphicsProperties.maxSwapchainImageWidth);
    spdlog::info("[VR] OpenXR system max height: {}", system_properties.graphicsProperties.maxSwapchainImageHeight);
    spdlog::info("[VR] OpenXR system supports {} layers", system_properties.graphicsProperties.maxLayerCount);
    spdlog::info("[VR] OpenXR system orientation: {}", system_properties.trackingProperties.orientationTracking);
    spdlog::info("[VR] OpenXR system position: {}", system_properties.trackingProperties.positionTracking);

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->init_pipline_state(m_openxr->view_configs.size());
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    fc5_boot_step("OpenXR: input");
    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    detect_controllers();

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name);
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchain");
    fc5_boot_step("OpenXR: swapchains");

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();
        fc5_boot_result("OpenXR: swapchains", err ? 1 : 0);

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

std::optional<std::string> VR::hijack_resolution() {
    // moved to global hook class
    return std::nullopt;
}

bool VR::detect_controllers() {
    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex);
        spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex);
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        spdlog::info("Left Hand: {}", 1);
        spdlog::info("Right Hand: {}", 2);
    }

    return true;
}

bool VR::is_any_action_down() {
    if (!m_runtime->ready()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    for (auto& it : m_action_handles) {
        if (is_action_active(it.second, m_left_joystick) || is_action_active(it.second, m_right_joystick)) {
            return true;
        }
    }

    return false;
}

void VR::update_hmd_state(int frame) {
    auto runtime = get_runtime();
    if (frame % 2 == m_right_eye_interval && !is_using_async_aer()) {
        return;
    }

    runtime->update_poses(frame);

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        recenter_view();
        runtime->wants_reset_origin = false;
    }

    runtime->update_matrices(m_nearz, m_farz);
    if(runtime->is_openxr()) {
        auto& pipeline_state = m_openxr->get_pipeline_state();
        GlobalPool::submit_openxr_pose(pipeline_state.stage_views[frame % 2].pose, frame);
        GlobalPool::submit_openxr_fov(pipeline_state.active_fov[frame % 2], frame);
        if (is_native_stereo()) {
            for (int eye = 0; eye < 2; ++eye) {
                GlobalPool::submit_native_stereo_view(eye, pipeline_state.stage_views[eye].pose, m_openxr->m_horizontal_fov_scale < 1.0f || m_openxr->m_vertical_fov_scale < 1.0f
                                                                                                     ? pipeline_state.active_fov[eye]
                                                                                                     : pipeline_state.stage_views[eye].fov,
                                                      frame);
            }
        }
        if (!is_using_async_aer()) {
            GlobalPool::submit_openxr_pose(pipeline_state.stage_views[(frame + 1) % 2].pose, frame + 1);
            GlobalPool::submit_openxr_fov(pipeline_state.active_fov[(frame + 1) % 2], frame + 1);
        }
    }

    if(runtime->is_openvr()) {
        const auto& hmd_pose = m_openvr->render_poses[vr::k_unTrackedDeviceIndex_Hmd];
        GlobalPool::submit_openvr_pose(hmd_pose.mDeviceToAbsoluteTracking, frame);
        if (!is_using_async_aer()) {
            GlobalPool::submit_openvr_pose(hmd_pose.mDeviceToAbsoluteTracking, frame + 1);
        }
    }

    runtime->got_first_poses = true;
}

void VR::update_action_states() {
    SCOPE_PROFILER();
    auto runtime = get_runtime();

    if (runtime->wants_reinitialize) {
        return;
    }

    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("VRInput failed to update action state: {}", (uint32_t)error);
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    bool actively_using_controller = false;

    if (is_any_action_down()) {
        m_last_controller_update = std::chrono::steady_clock::now();
        actively_using_controller = true;
    }

    const auto last_xinput_update_is_late = std::chrono::steady_clock::now() - m_last_xinput_update >= std::chrono::seconds(2);
    const auto should_be_spoofing = (actively_using_controller || get_runtime()->handle_pause);

    if (m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        m_spoofed_gamepad_connection = false;
    }

    if (!m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        spdlog::info("[VR] Attempting to spoof gamepad connection");
        g_framework->post_message(RE_WM_DEVICECHANGE, 0x8000, 0);
//        g_framework->activate_window();

        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }
}


bool VR::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
//    ZoneScopedN(__FUNCTION__);

    if ((message == WM_DEVICECHANGE || RE_WM_DEVICECHANGE) && !m_spoofed_gamepad_connection) {
//        spdlog::info("[VR] Received WM_DEVICECHANGE {} {} {}", w_param, l_param, m_spoofed_gamepad_connection);
        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    return true;
}

bool VR::is_hand_behind_head(VRRuntime::Hand hand, float sensitivity) const {
    if (hand > VRRuntime::Hand::RIGHT || !is_using_controllers()) {
        return false;
    }

    const auto hand_index = get_controllers()[(uint32_t)hand];

    const auto hmd = get_transform(0);
    const auto hand_pos = get_position(hand_index);
    const auto hmd_delta = Vector3f{hand_pos - hmd[3]};
    const auto distance = glm::length(hmd_delta);

    if (distance >= 0.3f) {
        return false;
    }

    const auto hmd_dir = glm::normalize(hmd_delta);

    const auto& hmd_forward = hmd[2];
    const auto flattened_forward = glm::normalize(Vector3f{hmd_forward.x, 0.0f, hmd_forward.z});

    const auto hand_dot_flat_raw = glm::dot(flattened_forward, hmd_dir);
    return hand_dot_flat_raw >= sensitivity;
}

Matrix4x4f VR::get_transform_offset() {
    std::shared_lock _{ m_rotation_mtx };

    return m_transform_offset;
}

void VR::set_transform_offset(const Matrix4x4f& offset) {
    std::unique_lock _{ m_rotation_mtx };

    m_transform_offset = offset;
}

void VR::recenter_view() {
    auto current_hmd_state = get_transform(0);
    auto new_rotation_offset = glm::mat4_cast(glm::normalize(utility::math::flatten(glm::quat{get_rotation(0)})));
    new_rotation_offset[3] = current_hmd_state[3];
    get_runtime()->recenter_view(new_rotation_offset);
    set_transform_offset(glm::inverse(new_rotation_offset));
}

glm::quat VR::get_gui_rotation_offset() {
    std::shared_lock _{ m_gui_mtx };

    return m_gui_rotation_offset;
}

void VR::set_gui_rotation_offset(const glm::quat& offset) {
    std::unique_lock _{ m_gui_mtx };

    m_gui_rotation_offset = offset;
}

void VR::recenter_gui(const glm::quat& from) {
    const auto new_gui_offset = glm::normalize(glm::inverse(utility::math::flatten(from)));
    set_gui_rotation_offset(new_gui_offset);
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (is_native_stereo() || m_engine_frame_count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    if (is_native_stereo()) {
        return get_eye_transform(flip ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT);
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_engine_frame_count % 2 == mod_count) {
        auto leye = get_runtime()->eyes[vr::Eye_Left];
        leye[3] = glm::vec4(glm::vec3(leye[3]) * m_world_scale_option->value(), 1.0f);
        return leye;
    }

    auto reye = get_runtime()->eyes[vr::Eye_Right];
    reye[3] = glm::vec4(glm::vec3(reye[3]) * m_world_scale_option->value(), 1.0f);
    return reye;
}

Matrix4x4f VR::get_eye_transform(VRRuntime::Eye eye) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto transform = get_runtime()->eyes[eye == VRRuntime::Eye::LEFT ? vr::Eye_Left : vr::Eye_Right];
    transform[3] = glm::vec4(glm::vec3(transform[3]) * m_world_scale_option->value(), 1.0f);
    return transform;
}

//Matrix4x4f VR::get_current_projection_matrix(bool flip) {
//    if (!is_hmd_active()) {
//        return glm::identity<Matrix4x4f>();
//    }
//
//    std::shared_lock _{get_runtime()->eyes_mtx};
//
//    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;
//
//    if (m_engine_frame_count % 2 == mod_count) {
//        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
//    }
//
//    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
//}

void VR::on_pre_imgui_frame() {
    if (!get_runtime()->ready()) {
        return;
    }

    m_overlay_component.on_pre_imgui_frame();
}

VRRuntime::Eye VR::get_current_render_eye() const {
    if (is_native_stereo() || m_engine_frame_count % 2 == m_left_eye_interval) {
        return VRRuntime::Eye::LEFT;
    }

    return VRRuntime::Eye::RIGHT;
}

void VR::on_present() {
    SCOPE_PROFILER();
    if(m_skip_next_present) {
        m_skip_next_present = false;
        return;
    }
//    m_presenter_frame_count = m_render_frame_count;
    utility::ScopeGuard _guard {[&]() {
        if (is_using_async_aer() || (m_presenter_frame_count + 1) % 2 == m_left_eye_interval) {
            SetEvent(m_present_finished_event);
        }

    }};

    if (is_using_async_aer() || (m_presenter_frame_count + 1) % 2 == m_left_eye_interval) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    //TODO move to after game tick
    runtime->consume_events(nullptr);

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (((m_presenter_frame_count % 2 == m_left_eye_interval) || is_using_async_aer()) && runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::LATE) {
        //TODO LATE does not work
        const auto had_sync = runtime->got_first_sync;
        runtime->synchronize_frame(m_presenter_frame_count + 1);

        if (!runtime->got_first_poses || !had_sync) {
            update_hmd_state(m_presenter_frame_count + 1);
        }
    }

    if (renderer == Framework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            runtime->synchronize_frame(m_presenter_frame_count + 1);
            runtime->update_poses(m_presenter_frame_count + 1);
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == Framework::RendererType::D3D12) {
        m_is_d3d12 = true;
//        spdlog::info("Copying frame to D3D12 texture f={}", m_presenter_frame_count % 2 == m_left_eye_interval ? "left" : "right");
        e = m_d3d12.on_frame(this);
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

//    m_last_frame_count = m_render_frame_count;

    if (m_submitted || runtime->needs_pose_update) {
        if (m_submitted) {
            m_overlay_component.on_post_compositor_submit();

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
                vr::VRCompositor()->PostPresentHandoff();
            }
        }

        runtime->needs_pose_update = true;
        m_submitted = false;
    }
}

void VR::on_post_present() {
    SCOPE_PROFILER();
    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    const auto renderer = g_framework->get_renderer_type();

    if (renderer == Framework::RendererType::D3D12) {
        m_d3d12.on_post_present(this);
    }

    //TODO move to after engine tick
    detect_controllers();

    if (m_presenter_frame_count % 2 == m_left_eye_interval || is_using_async_aer()) {
        if (runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::VERY_LATE || !runtime->got_first_sync) {
            const auto had_sync = runtime->got_first_sync;
            runtime->synchronize_frame(m_presenter_frame_count);

            if (!runtime->got_first_poses || !had_sync) {
                update_hmd_state(m_presenter_frame_count + 1);
            }
        }

        if (runtime->is_openxr() && runtime->ready() && runtime->get_synchronize_stage() > VRRuntime::SynchronizeStage::EARLY) {
            if (!m_openxr->frame_began) {
                m_openxr->begin_frame(m_presenter_frame_count);
            }
        }
    }

    if (runtime->wants_reinitialize) {
        std::scoped_lock _{m_openvr_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

//thread_local bool timed_out = false;

void VR::on_begin_rendering(int frame) {
    SCOPE_PROFILER();
    auto runtime = get_runtime();

    if (!runtime->loaded) {
        return;
    }
    m_in_render = true;
//    m_render_frame_count = m_engine_frame_count;
//    on_wait_rendering(entry);
    if (frame % 2 == m_left_eye_interval || is_using_async_aer()) {
        if(runtime->get_synchronize_stage() == VRRuntime::SynchronizeStage::EARLY) {
            if (runtime->is_openxr()) {
                if (g_framework->get_renderer_type() == Framework::RendererType::D3D11) {
                    if (!runtime->got_first_sync || runtime->synchronize_frame(frame) != VRRuntime::Error::SUCCESS) {
                        return;
                    }
                } else if (runtime->synchronize_frame(frame) != VRRuntime::Error::SUCCESS) {
                    return;
                }
                m_openxr->begin_frame(frame);
            } else {
                if (runtime->synchronize_frame(frame) != VRRuntime::Error::SUCCESS) {
                    return;
                }
            }
        }
    }
}

void VR::on_end_rendering(void* entry) {
//    SCOPE_PROFILER();
    // we set this because we've enabled asynchronous rendering
    // by the time the next frame (right eye) starts,
    // the frame count might get modified, screwing up our logic
    // so we need a render frame count to compare against
//    m_render_frame_count = m_frame_count;
}

void VR::on_wait_rendering(int frame) {
    if (!get_runtime()->loaded) {
        return;
    }

//    timed_out = false;

    if (!is_hmd_active()) {
        return;
    }

    // wait for m_present_finished (std::condition_variable)
    // to be signaled
    // only on the left eye interval because we need the right eye
    // to start render work as soon as possible
    if (((frame) % 2) == m_left_eye_interval || is_using_async_aer()) {
        if (WaitForSingleObject(m_present_finished_event, 333) == WAIT_TIMEOUT) {
//            timed_out = true;
        }
        ResetEvent(m_present_finished_event);
    }
}

void VR::on_xinput_get_capabilities(uint32_t* retval, uint32_t user_index, uint32_t flags, XINPUT_CAPABILITIES* capabilities) {
    if(m_lowest_xinput_user_index == user_index) {
        *retval = ERROR_SUCCESS;
    }
}

// on_xinput_set_state: ModConfig.cpp (the game's pad rumble goes to the hands by role, FarCry5Stereo::rumble_hands).

void VR::on_draw_ui() {
    // create VR tree entry in menu (imgui)
    if (get_runtime()->loaded) {
        ImGui::SetNextItemOpen(m_has_hw_scheduling, ImGuiCond_::ImGuiCond_FirstUseEver);
    } else {
        if (m_openvr->error && !m_openvr->dll_missing) {
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_FirstUseEver);
        } else {
            ImGui::SetNextItemOpen(false, ImGuiCond_::ImGuiCond_FirstUseEver);
        }
    }

    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please drop the %s file into the game's directory if you want to use %s", dll_name.data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    display_error(m_openxr, "openxr_loader.dll");
    display_error(m_openvr, "openvr_api.dll");

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");
        return;
    }

    ImGui::Text("Diagonal FOV: %f", get_runtime()->diagonal_fov);
    ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");

/*    if (m_has_hw_scheduling) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This will cause the game to run slower.");
        ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
        ImGui::PopStyleColor();
    }*/

    ImGui::Separator();

    ImGui::TextWrapped("VR Runtime: %s", get_runtime()->name().data());
    ImGui::TextWrapped("Render Resolution: %d x %d", get_runtime()->get_width(), get_runtime()->get_height());

    m_use_async_aer->draw("Use Async AER");

    if (get_runtime()->is_openvr()) {
        ImGui::TextWrapped("Resolution can be changed in SteamVR");
    } else if (get_runtime()->is_openxr()) {
        if (ImGui::TreeNode("Bindings")) {
            m_openxr->display_bindings_editor();
            ImGui::TreePop();
        }

        m_horizontal_fov_scale->draw("Horizontal FOV Scale");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            get_runtime()->m_horizontal_fov_scale = m_horizontal_fov_scale->value();
        }
        m_vertical_fov_scale->draw("Vertical FOV Scale");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            get_runtime()->m_vertical_fov_scale = m_vertical_fov_scale->value();
        }
        if (m_extended_fov_rage->draw("Extended FOV Scale Range"))
        {
            get_runtime()->m_extended_fov_range = m_extended_fov_rage->value();
        }
    }

//    ImGui::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0");
    if(m_sync_interval->draw("Sync Interval")) {
        *(int*)&get_runtime()->custom_stage = m_sync_interval->value();
    }
    ImGui::Separator();

    if (ImGui::Button("Reinitialize Runtime")) {
        get_runtime()->wants_reinitialize = true;
    }

    //ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
    //ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);

    ImGui::Separator();

    m_recenter_view_key->draw("Recenter View Key");

    ImGui::Separator();

//    m_use_afr->draw("Use AFR");
//    m_decoupled_pitch->draw("Decoupled Camera Pitch");
//
//    if (ImGui::Checkbox("Positional Tracking", &m_positional_tracking)) {
//    }

//    m_hmd_oriented_audio->draw("Head Oriented Audio");
//    m_use_custom_view_distance->draw("Use Custom View Distance");
//    m_view_distance->draw("View Distance/FarZ");
//    m_motion_controls_inactivity_timer->draw("Inactivity Timer");
//    m_joystick_deadzone->draw("Joystick Deadzone");

//    m_ui_scale_option->draw("2D UI Scale");
//    m_ui_distance_option->draw("2D UI Distance");

//    ImGui::DragFloat3("Overlay Rotation", (float*)&m_overlay_rotation, 0.01f, -360.0f, 360.0f);
//    ImGui::DragFloat3("Overlay Position", (float*)&m_overlay_position, 0.01f, -100.0f, 100.0f);

//    ImGui::Separator();
//    ImGui::Text("Graphical Options");

//    m_force_fps_settings->draw("Force Uncap FPS");
//    m_force_aa_settings->draw("Force Disable TAA");
//    m_force_motionblur_settings->draw("Force Disable Motion Blur");
//    m_force_vsync_settings->draw("Force Disable V-Sync");
//    m_force_lensdistortion_settings->draw("Force Disable Lens Distortion");
//    m_force_volumetrics_settings->draw("Force Disable Volumetrics");
//    m_force_lensflares_settings->draw("Force Disable Lens Flares");
//    m_force_dynamic_shadows_settings->draw("Force Enable Dynamic Shadows");
//    m_allow_engine_overlays->draw("Allow Engine Overlays");
//    m_enable_asynchronous_rendering->draw("Enable Asynchronous Rendering");

    if (ImGui::TreeNode("Desktop Recording Fix")) {
        ImGui::PushID("Desktop");
        m_desktop_fix->draw("Enabled");
        m_desktop_fix_skip_present->draw("Skip Present");
        ImGui::PopID();
        ImGui::TreePop();
    }

    ImGui::Separator();
    ImGui::Text("Debug info");
//    ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
//    ImGui::Checkbox("Disable GUI Projection Matrix Override", &m_disable_gui_camera_projection_matrix_override);
//    ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
//    ImGui::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
//    ImGui::Checkbox("Disable Temporal Fix", &m_disable_temporal_fix);
//    ImGui::Checkbox("Disable Post Effect Fix", &m_disable_post_effect_fix);

    const double min_ = 0.0;
    const double max_ = 25.0;
//    ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

//    ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
//    ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

    // convert m_avg_input_delay (std::chrono::nanoseconds) to milliseconds (float)
    auto duration_float = std::chrono::duration<float, std::milli>(m_avg_input_delay).count();

    ImGui::DragFloat("Avg Input Processing Delay (MS)", &duration_float, 0.00001f);

}


void VR::on_device_reset() {
    std::scoped_lock _{m_openxr->sync_mtx};

    spdlog::info("VR: on_device_reset");
    m_backbuffer_inconsistency = false;
    if (g_framework->is_dx11()) {
        m_d3d11.on_reset(this);
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    }


    m_overlay_component.on_reset();
}

void VR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    // Run the rest of OpenXR initialization code here that depends on config values
    if (get_runtime()->is_openxr() && get_runtime()->loaded) {
        spdlog::info("[VR] Finishing up OpenXR initialization");

        m_openxr->resolution_scale = m_resolution_scale->value();
        initialize_openxr_swapchains();
    }
    if(get_runtime()->loaded) {
        *(int*)&get_runtime()->custom_stage = m_sync_interval->value();
        get_runtime()->m_horizontal_fov_scale = m_horizontal_fov_scale->value();
        get_runtime()->m_vertical_fov_scale = m_vertical_fov_scale->value();
        get_runtime()->m_extended_fov_range = m_extended_fov_rage->value();
        get_runtime()->m_flat_screen_distance = m_flat_screen_distance->value();
    }
    m_overlay_component.on_config_load(cfg, set_defaults);


    if (m_motion_controls_inactivity_timer->value() <= 10.0f) {
        m_motion_controls_inactivity_timer->value() = 30.0f;
    }
}

void VR::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
    m_overlay_component.on_config_save(cfg);
}

Vector4f VR::get_position(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };
    std::shared_lock __{ get_runtime()->eyes_mtx };

    return get_position_unsafe(index);
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && m_openxr->get_stage_views_count()) {
            return Vector4f{ *(Vector3f*)&m_openxr->get_view_space_location().pose.position, 1.0f };
        } else if (index > 0) {
            return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].location.pose.position, 1.0f };
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

Matrix4x4f VR::get_rotation(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };
        std::shared_lock __{ get_runtime()->eyes_mtx };

        // HMD rotation
        if (index == 0 && m_openxr->get_stage_views_count()) {
            auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(m_openxr->get_view_space_location().pose.orientation)};
//            mat[3] = Vector4f{*(Vector3f*)&m_openxr->view_space_location.pose.position, 1.0f};

            return mat;
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                return Matrix4x4f{runtimes::OpenXR::to_glm(m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation)};
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                return Matrix4x4f{runtimes::OpenXR::to_glm(m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation)};
            }
        }

        return glm::identity<Matrix4x4f>();
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        // HMD rotation
        if (index == 0 && m_openxr->get_stage_views_count()) {
            auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(m_openxr->get_view_space_location().pose.orientation)};
            mat[3] = Vector4f{*(Vector3f*)&m_openxr->get_view_space_location().pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == VRRuntime::Hand::LEFT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::LEFT].location.pose.position, 1.0f};
                return mat;
            } else if (index == VRRuntime::Hand::RIGHT+1) {
                auto mat = Matrix4x4f{*(glm::quat*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.orientation};
                mat[3] = Vector4f{*(Vector3f*)&m_openxr->hands[VRRuntime::Hand::RIGHT].location.pose.position, 1.0f};
                return mat;
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name());
        return vr::HmdMatrix34_t{};
    }
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    if (!get_runtime()->loaded) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }
    return active;
}

bool VR::is_action_active_any_joystick(vr::VRActionHandle_t action) const {
    if (is_action_active(action, m_left_joystick)) {
        return true;
    }

    if (is_action_active(action, m_right_joystick)) {
        return true;
    }

    return false;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        const auto out = Vector2f{ data.x, data.y };

        return glm::length(out) > deadzone ? out : Vector2f{};
    } else if (get_runtime()->is_openxr()) {
        if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::LEFT) {
            auto out = m_openxr->get_left_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        } else if (handle == (vr::VRInputValueHandle_t)VRRuntime::Hand::RIGHT) {
            auto out = m_openxr->get_right_stick_axis();
            return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
        }
    }

    return Vector2f{};
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(m_left_joystick);
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(m_right_joystick);
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}
