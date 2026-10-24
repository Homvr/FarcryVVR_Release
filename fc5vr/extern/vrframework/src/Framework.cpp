#include <chrono>
#include <filesystem>
#include <fstream>

#include <windows.h>
#include <ShlObj.h>

//#include <CreationEngine/CreationEngineRendererModule.h>
#include <mods/VR.hpp>
#include "Fc5Boot.hpp"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/pattern_formatter.h>
// minhook, used for AllocateBuffer
extern "C" {
#include <../buffer.h>
};

#include "af_baidu.hpp"
#include "af_faprolight.hpp"
#include "font_robotomedium.hpp"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include <ImGuizmo.h>
#include <ModSettings.h>
#include <imgui.h>
#include <imgui_internal.h>
//#include <imnodes.h>
#include "utility/Module.hpp"
#include "utility/Patch.hpp"
#include "utility/Scan.hpp"
#include "utility/String.hpp"
#include "utility/Thread.hpp"

#include "Mods.hpp"
//#include "mods/PluginLoader.hpp"
//#include "sdk/REGlobals.hpp"
//#include "sdk/Application.hpp"
//#include "sdk/SDK.hpp"

#include "ExceptionHandler.hpp"
//#include "LicenseStrings.hpp"
#include "mods/VRConfig.hpp"
//#include "mods/IntegrityCheckBypass.hpp"
#include "Framework.hpp"

namespace fs = std::filesystem;
using namespace std::literals;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::unique_ptr<Framework> g_framework{};

void Framework::hook_monitor() {
    std::scoped_lock _{ m_hook_monitor_mutex };

    if (g_framework == nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    auto& d3d11 = get_d3d11_hook();
    auto& d3d12 = get_d3d12_hook();

    const auto renderer_type = get_renderer_type();

    if (d3d11 == nullptr || d3d12 == nullptr 
        || (renderer_type == Framework::RendererType::D3D11 && d3d11 != nullptr && !d3d11->is_inside_present())
        || (renderer_type == Framework::RendererType::D3D12 && d3d12 != nullptr && !d3d12->is_inside_present()))
    {
        // check if present time is more than 5 seconds ago
        if (now - m_last_present_time > std::chrono::seconds(5)) {
            if (m_has_last_chance) {
                // the purpose of this is to make sure that the game is not frozen
                // e.g. if we are debugging the game, so we don't rehook anything on accident
                m_has_last_chance = false;
                m_last_chance_time = now;

                spdlog::info("Last chance encountered for hooking");
            }

            if (!m_has_last_chance && now - m_last_chance_time > std::chrono::seconds(1)) {
                spdlog::info("Sending rehook request for D3D");

                // hook_d3d12 always gets called first.
                if (m_is_d3d11) {
                    hook_d3d11();
                } else {
                    hook_d3d12();
                }

                // so we don't immediately go and hook it again
                // add some additional time to it to give it some leeway
                m_last_present_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                m_last_message_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                m_last_chance_time = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                m_has_last_chance = true;
            }
        } else {
            m_last_chance_time = std::chrono::steady_clock::now();
            m_has_last_chance = true;
        }

        if (m_initialized && m_wnd != 0 && now - m_last_message_time > std::chrono::seconds(5)) {
            if (m_windows_message_hook != nullptr && m_windows_message_hook->is_hook_intact()) {
                spdlog::info("Windows message hook is still intact, ignoring...");
                m_last_message_time = now;
                m_last_sendmessage_time = now;
                m_sent_message = false;
                return;
            }

            // send dummy message to window to check if our hook is still intact
            if (!m_sent_message) {
                spdlog::info("Sending initial message hook test");

                auto proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);

                if (proc != nullptr) {
                    const auto ret = CallWindowProc(proc, m_wnd, WM_NULL, 0, 0);

                    spdlog::info("Hook test message sent");
                }

                m_last_sendmessage_time = std::chrono::steady_clock::now();
                m_sent_message = true;
            } else if (now - m_last_sendmessage_time > std::chrono::seconds(1)) {
                spdlog::info("Sending reinitialization request for message hook");

                // if we don't get a message for 5 seconds, assume the hook is broken
                //m_initialized = false; // causes the hook to be re-initialized next frame
                m_message_hook_requested = true;
                m_last_message_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                m_last_present_time = std::chrono::steady_clock::now() + std::chrono::seconds(5);

                m_sent_message = false;
            }
        } else {
            m_sent_message = false;
        }
    }
}

typedef NTSTATUS (WINAPI* PFN_LdrLockLoaderLock)(ULONG Flags, ULONG *State, ULONG_PTR *Cookie);
typedef NTSTATUS (WINAPI* PFN_LdrUnlockLoaderLock)(ULONG Flags, ULONG_PTR Cookie);

Framework::Framework(HMODULE module)
    : m_module{module}
    , m_game_module{GetModuleHandle(0)}
    {

    // Far Cry 5: the mod's fc5vr_log.txt is open before the framework (src/Main.cpp): keep it as the default logger
    // with its own pattern, so nothing logged meanwhile by other threads goes to a null sink.
    const auto fc5_logger = spdlog::get("fc5vr");
#ifndef _DEBUG
    // Use null logger if possible
    if (fc5_logger) {
        m_logger = fc5_logger;
    } else {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        m_logger = std::make_shared<spdlog::logger>("null_logger", null_sink);
    }
#else
    m_logger = spdlog::basic_logger_mt("VRFramework", (get_persistent_dir("vr_log.txt")).string(), true);
#endif

    std::scoped_lock __{m_startup_mutex};

    spdlog::set_default_logger(m_logger);
    // Set pattern to include thread ID and seconds.milliseconds with short log level
    if (!fc5_logger) {
        spdlog::set_pattern("[%S.%e] [%L] [tid:%t] %v");
    }
    spdlog::flush_on(spdlog::level::info);

    if (s_fallback_appdata) {
        spdlog::warn("Failed to write to current directory, falling back to appdata folder");
    }

    spdlog::info("VR entry");

    const auto module_size = *utility::get_module_size(m_game_module);

    spdlog::info("Game Module Addr: {:x}", (uintptr_t)m_game_module);
    spdlog::info("Game Module Size: {:x}", module_size);

    // preallocate some memory for minhook to mitigate failures (temporarily at least... this should in theory fail when too many hooks are made)
    // but, 64 slots should be enough for now. 
    // so... TODO: modify minhook to use absolute jumps when failing to allocate memory nearby
    const auto halfway_module = (uintptr_t)m_game_module + (module_size / 2);
    const auto pre_allocated_buffer = (uintptr_t)AllocateBuffer((LPVOID)halfway_module); // minhook function
    spdlog::info("Preallocated buffer: {:x}", pre_allocated_buffer);

//    IntegrityCheckBypass::fix_virtual_protect();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

#ifdef _DEBUG
    spdlog::set_level(spdlog::level::debug);
#endif

    // Create the typedef for RtlGetVersion
    typedef LONG (*RtlGetVersionFunc)(PRTL_OSVERSIONINFOW);

    const auto ntdll = GetModuleHandle("ntdll.dll");

    if (ntdll != nullptr) {
        // Manually get RtlGetVersion
        auto rtl_get_version = (RtlGetVersionFunc)GetProcAddress(ntdll, "RtlGetVersion");

        if (rtl_get_version != nullptr) {
            spdlog::info("Getting OS version information...");

            // Create an initial log that prints out the user's Windows OS version information
            // With the major and minor version numbers
            // Using RtlGetVersion()
            OSVERSIONINFOW os_version_info{};
            ZeroMemory(&os_version_info, sizeof(OSVERSIONINFOW));
            os_version_info.dwOSVersionInfoSize = sizeof(OSVERSIONINFOW);
            os_version_info.dwMajorVersion = 0;
            os_version_info.dwMinorVersion = 0;
            os_version_info.dwBuildNumber = 0;
            os_version_info.dwPlatformId = 0;

            if (rtl_get_version(&os_version_info) != 0) {
                spdlog::info("RtlGetVersion() failed");
            } else {
                // Log the Windows version information
                spdlog::info("OS Version Information");
                spdlog::info("\tMajor Version: {}", os_version_info.dwMajorVersion);
                spdlog::info("\tMinor Version: {}", os_version_info.dwMinorVersion);
                spdlog::info("\tBuild Number: {}", os_version_info.dwBuildNumber);
                spdlog::info("\tPlatform Id: {}", os_version_info.dwPlatformId);

                spdlog::info("Disclaimer: VR does not send this information to the developers or any other third party.");
                spdlog::info("This information is only used to help with the development of VR.");
            }
        } else {
            spdlog::info("RtlGetVersion() not found");
        }
    } else {
        spdlog::info("ntdll.dll not found");
    }

    // Hooking D3D12 initially because we need to retrieve the command queue before the first frame then switch to D3D11 if it failed later
    // on
    // addendum: now we don't need to do that, we just grab the command queue offset from the swapchain we create
    /*if (!hook_d3d12()) {
        spdlog::error("Failed to hook D3D12 for initial test.");
    }*/

    std::scoped_lock _{m_hook_monitor_mutex};

    m_last_present_time = std::chrono::steady_clock::now();
    m_last_message_time = std::chrono::steady_clock::now();
    m_d3d_monitor_thread = std::make_unique<std::jthread>([this](std::stop_token stop_token) {
        // Load the plugins early right after executable unpacking
        //PluginLoader::get()->early_init();

        while (!stop_token.stop_requested() && !m_terminating) {
            this->hook_monitor();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });
}


bool Framework::hook_d3d11() {
    m_d3d11_hook.reset();
    m_d3d11_hook = std::make_unique<D3D11Hook>();
    m_d3d11_hook->on_present([this](D3D11Hook& hook) { on_frame_d3d11(); });
    m_d3d11_hook->on_post_present([this](D3D11Hook& hook) { on_post_present_d3d11(); });
    m_d3d11_hook->on_resize_buffers([this](D3D11Hook& hook) { on_reset(); });

    if (!m_is_d3d12) {
        if (m_d3d11_hook->hook()) {
            spdlog::info("Hooked DirectX 11");
            m_valid = true;
            m_is_d3d11 = true;
            return true;
        }

        if (m_d3d11_hook->unhook()) {
            spdlog::info("D3D11 Unhooked!");
        } else {
            spdlog::info("Cannot unhook D3D11, this might crash.");
        }

        m_valid = false;
        m_is_d3d11 = false;
        return false;
    }
    return false;
}

bool Framework::hook_d3d12() {
    if (LoadLibraryA("d3d12.dll") == nullptr) {
        spdlog::info("d3d12.dll not found, user is probably running Windows 7.");
        spdlog::info("Falling back to hooking D3D11.");

        m_is_d3d12 = false;
        return hook_d3d11();
    }

    //if (m_d3d12_hook == nullptr) {
        m_d3d12_hook.reset();
        m_d3d12_hook = std::make_unique<D3D12Hook>();
        m_d3d12_hook->on_present([this](D3D12Hook& hook) { on_frame_d3d12(); });
        m_d3d12_hook->on_post_present([this](D3D12Hook& hook) { on_post_present_d3d12(); });
        m_d3d12_hook->on_resize_buffers([this](D3D12Hook& hook) { on_reset(); });
        m_d3d12_hook->on_resize_target([this](D3D12Hook& hook) { on_reset(); });
        m_d3d12_hook->on_set_render_targets([this](D3D12Hook& hook, ID3D12GraphicsCommandList5* cmd_list, UINT num_rtvs, 
            const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle, D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
            on_d3d12_set_render_targets(cmd_list, num_rtvs, rtvs, single_handle, dsv);
        });
        m_d3d12_hook->on_set_scissor_rects([this](D3D12Hook& hook, ID3D12GraphicsCommandList5* cmd_list, UINT num_rects, const D3D12_RECT* rects) {
            on_d3d12_set_scissor_rects(cmd_list, num_rects, rects);
        });
        m_d3d12_hook->on_set_viewports([this](D3D12Hook& hook, ID3D12GraphicsCommandList5* cmd_list, UINT num_viewports, const D3D12_VIEWPORT* viewports) {
            on_d3d12_set_viewports(cmd_list, num_viewports, viewports);
        });
        m_d3d12_hook->on_create_render_target_view([this](D3D12Hook& hook, ID3D12Device* device, ID3D12Resource* pResource, 
            const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) {
            on_d3d12_create_render_target_view(device, pResource, pDesc, DestDescriptor);
        });
    //}
    //m_d3d12_hook->on_create_swap_chain([this](D3D12Hook& hook) { m_d3d12.command_queue = m_d3d12_hook->get_command_queue(); });

    // Making sure D3D11 is not hooked
    if (!m_is_d3d11) {
        if (m_d3d12_hook->hook()) {
            spdlog::info("Hooked DirectX 12");
            m_valid = true;
            m_is_d3d12 = true;
            return true;
        }
        // We make sure to no unhook any unwanted hooks if D3D12 didn't get hooked properly
        if (m_d3d12_hook->unhook()) {
            spdlog::info("D3D12 Unhooked!");
        } else {
            spdlog::info("Cannot unhook D3D12, this might crash.");
        }

        m_valid = false;
        m_is_d3d12 = false;

        // Try to hook d3d11 instead
        return hook_d3d11();
    }

    return false;
}

Framework::~Framework() {
    spdlog::info("VR shutting down...");

    m_terminating = true;
    m_d3d_monitor_thread->request_stop();
    if (m_d3d_monitor_thread->joinable()) {
        m_d3d_monitor_thread->join();
    }

    m_d3d_monitor_thread.reset();

    if (m_is_d3d11) {
        deinit_d3d11();
    }

    if (m_is_d3d12) {
        deinit_d3d12();
    }

    ImGui_ImplWin32_Shutdown();

    if (m_initialized) {
        ImGui::DestroyContext();
    }
}

void Framework::run_imgui_frame(bool from_present) {
    std::scoped_lock _{ m_imgui_mtx };

    m_has_frame = false;

    if (!m_initialized) {
        return;
    }

    const bool is_init_ok = m_error.empty() && m_game_data_initialized;

    consume_input();
    update_fonts();

    ImGui_ImplWin32_NewFrame();

    // Far Cry 5 builds every ImGui frame from Present: the pre-frame callbacks only feed input (the
    // VR controller pointer on the F11 panel), so they run there too.
    fc5_imgui_frames[from_present ? 1 : 0]++;
    if (is_init_ok) {
        fc5_pre_imgui_frames++;
        m_mods->on_pre_imgui_frame();
    }

    ImGui::NewFrame();

    if (!from_present) {
        call_on_frame();
    }

    draw_ui();
    m_last_draw_ui = m_draw_ui;

    IMGUIZMO_NAMESPACE::BeginFrame();

    ImGui::EndFrame();
    ImGui::Render();

    m_has_frame = true;

    if (!from_present && m_wants_save_config) {
        save_config();
        m_wants_save_config = false;
    }
}

void Framework::on_frame_d3d11() {
    std::scoped_lock _{ m_imgui_mtx };

    // spdlog::debug("on_frame (D3D11)");

    m_renderer_type = RendererType::D3D11;

    if (!m_initialized) {
        if (!initialize()) {
            return;
        }

        spdlog::info("REFramework initialized");
        m_initialized = true;
        return;
    }

    if (m_message_hook_requested) {
        initialize_windows_message_hook();
    }

    auto device = m_d3d11_hook->get_device();

    if (device == nullptr) {
        spdlog::error("D3D11 device was null when it shouldn't be, returning...");
        m_initialized = false;
        return;
    }

    bool is_init_ok = m_error.empty() && m_game_data_initialized;

    if (is_init_ok) {
        // Write default config once if it doesn't exist.
        if (!std::exchange(m_created_default_cfg, true)) {
            if (!fs::exists({utility::widen(get_persistent_dir("vr_config.txt").string())})) {
                save_config();
            }
        }
    }

    is_init_ok = first_frame_initialize();

    if (!m_has_frame) {
        // Far Cry 5 (no engine thread): if the game's next Present after initialize() came only once the
        // game-data thread had finished, no ImGui frame had been built yet and the old "else return" skipped
        // every mod's on_present for the whole run: OpenXR events never polled, session never begun, black
        // headset with game sound (logs: "imgui frames engine/present 0/0").
        if (!is_init_ok || !m_has_engine_thread) {
            if (is_init_ok) {
                static bool logged = false;
                if (!std::exchange(logged, true)) {
                    spdlog::info("[Start] first frame after mod initialization had no ImGui frame yet: built one here (black-headset start avoided)");
                }
            }
            update_fonts();
            invalidate_device_objects();

            ImGui_ImplDX11_NewFrame();
            // hooks don't run until after initialization, so we just render the imgui window while initalizing.
            run_imgui_frame(true);
        } else {
            return;
        }
    } else if (is_init_ok && !m_has_engine_thread) {
        // Far Cry 5 has no engine-thread hook building ImGui frames: without this the UI stays the
        // first frame forever (frozen, deaf to the mouse and the VR pointer).
        invalidate_device_objects();
        ImGui_ImplDX11_NewFrame();
        run_imgui_frame(true);
    } else {
        invalidate_device_objects();
        ImGui_ImplDX11_NewFrame();
    }

    if (is_init_ok) {
        m_mods->on_present();
    }

    ComPtr<ID3D11DeviceContext> context{};
    float clear_color[]{0.0f, 0.0f, 0.0f, 0.0f};

    m_d3d11_hook->get_device()->GetImmediateContext(&context);
    context->ClearRenderTargetView(m_d3d11.blank_rt_rtv.Get(), clear_color);

    // Only render this if VR is running.
    // TODO: Instead use this as an SRV to render to the back buffer so we don't render twice.
    if (VR::get()->is_hmd_active()) {
        context->ClearRenderTargetView(m_d3d11.rt_rtv.Get(), clear_color);
        context->OMSetRenderTargets(1, m_d3d11.rt_rtv.GetAddressOf(), NULL);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    // Set the back buffer to be the render target.
    context->OMSetRenderTargets(1, m_d3d11.bb_rtv.GetAddressOf(), nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (is_init_ok) {
        m_mods->on_post_frame();
    }
}

void Framework::on_post_present_d3d11() {
    if (!m_error.empty() || !m_initialized || !m_game_data_initialized) {
        if (m_last_present_time <= std::chrono::steady_clock::now()){
            m_last_present_time = std::chrono::steady_clock::now();
        }

        return;
    }

    size_t mod_index = 0;
    for (auto& mod : m_mods->get_mods()) {
        const auto t0 = std::chrono::steady_clock::now();
        mod->on_post_present();
        if (mod_index < 8) Mods::s_fc5_post_present_us[mod_index] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        ++mod_index;
    }

    if (m_last_present_time <= std::chrono::steady_clock::now()){
        m_last_present_time = std::chrono::steady_clock::now();
    }
}

// D3D12 Draw funciton
void Framework::on_frame_d3d12() {
    std::scoped_lock _{ m_imgui_mtx };

    m_renderer_type = RendererType::D3D12;

    auto command_queue = m_d3d12_hook->get_command_queue();
    //spdlog::debug("on_frame (D3D12)");
    
    if (!m_initialized) {
        if (!initialize()) {
            spdlog::error("Failed to initialize Framework on DirectX 12");
            return;
        }

        spdlog::info("Framework initialized");
        m_initialized = true;
        return;
    }

    if (command_queue == nullptr) {
        spdlog::error("Null Command Queue");
        return;
    }

    if (m_message_hook_requested) {
        initialize_windows_message_hook();
#ifdef HOOK_XINPUT
        initialize_xinput_hook();
#endif
        // initialize_dinput_hook();
    }

    auto device = m_d3d12_hook->get_device();

    if (device == nullptr) {
        spdlog::error("D3D12 Device was null when it shouldn't be, returning...");
        m_initialized = false;
        return;
    }

    bool is_init_ok = first_frame_initialize();

    //if (!m_has_frame) {
        //if (!is_init_ok) {
            //update_fonts();

            ImGui::GetIO().BackendRendererUserData = m_d3d12.imgui_backend_datas[0];
            const auto prev_cleanup = m_wants_device_object_cleanup;
            invalidate_device_objects();
            ImGui_ImplDX12_NewFrame();

            ImGui::GetIO().BackendRendererUserData = m_d3d12.imgui_backend_datas[1];
            m_wants_device_object_cleanup = prev_cleanup;
            invalidate_device_objects();
            ImGui_ImplDX12_NewFrame();
            // hooks don't run until after initialization, so we just render the imgui window while initalizing.
            if (!m_has_engine_thread) {
                run_imgui_frame(false);
            }
    /*    } else {
            return;
        }
    } else {
        do_per_frame_thing();
    } */

    if (is_init_ok) {
        m_mods->on_present();
    }

    if (m_d3d12.cmd_ctxs.empty()) {
        return;
    }

    auto& cmd_ctx = m_d3d12.cmd_ctxs[m_d3d12.cmd_ctx_index++ % m_d3d12.cmd_ctxs.size()];

    if (cmd_ctx == nullptr) {
        return;
    }

    cmd_ctx->wait(INFINITE);
    if (auto draw_data = ImGui::GetDrawData(); draw_data != nullptr) {
        std::scoped_lock _{ cmd_ctx->mtx };
        cmd_ctx->has_commands = true;

        // Draw to our render target.
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = m_d3d12.get_rt(D3D12::RTV::IMGUI).Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

        float clear_color[]{0.0f, 0.0f, 0.0f, 0.0f};
        D3D12_CPU_DESCRIPTOR_HANDLE rts[1]{};
        cmd_ctx->cmd_list->ClearRenderTargetView(m_d3d12.get_cpu_rtv(device, D3D12::RTV::IMGUI), clear_color, 0, nullptr);
        rts[0] = m_d3d12.get_cpu_rtv(device, D3D12::RTV::IMGUI);
        cmd_ctx->cmd_list->OMSetRenderTargets(1, rts, FALSE, NULL);
        cmd_ctx->cmd_list->SetDescriptorHeaps(1, m_d3d12.srv_desc_heap.GetAddressOf());

        ImGui::GetIO().BackendRendererUserData = m_d3d12.imgui_backend_datas[1];
        ImGui_ImplDX12_RenderDrawData(draw_data, cmd_ctx->cmd_list.Get());

        for (auto& mod : m_mods->get_mods()) {
            rts[0] = m_d3d12.get_cpu_rtv(device, D3D12::RTV::IMGUI);
            mod->on_post_render_vr_framework_dx12(cmd_ctx->cmd_list.Get(), m_d3d12.get_rt(D3D12::RTV::IMGUI).Get(), &rts[0]);
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

        // Draw to the back buffer.
        auto swapchain = m_d3d12_hook->get_swap_chain();
        auto bb_index = swapchain->GetCurrentBackBufferIndex();
        barrier.Transition.pResource = m_d3d12.rts[bb_index].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);
        rts[0] = m_d3d12.get_cpu_rtv(device, (D3D12::RTV)bb_index);
        cmd_ctx->cmd_list->OMSetRenderTargets(1, rts, FALSE, NULL);
        cmd_ctx->cmd_list->SetDescriptorHeaps(1, m_d3d12.srv_desc_heap.GetAddressOf());

        ImGui::GetIO().BackendRendererUserData = m_d3d12.imgui_backend_datas[0];
        ImGui_ImplDX12_RenderDrawData(draw_data, cmd_ctx->cmd_list.Get());

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd_ctx->cmd_list->ResourceBarrier(1, &barrier);

        cmd_ctx->execute();
    }

    if (is_init_ok) {
        m_mods->on_post_frame();
    }
}

void Framework::on_post_present_d3d12() {
    if (!m_error.empty() || !m_initialized || !m_game_data_initialized) {
        if (m_last_present_time <= std::chrono::steady_clock::now()){
            m_last_present_time = std::chrono::steady_clock::now();
        }

        return;
    }
    for (auto& mod : m_mods->get_mods()) {
        mod->on_post_present();
    }

    if (m_last_present_time <= std::chrono::steady_clock::now()){
        m_last_present_time = std::chrono::steady_clock::now();
    }
}

void Framework::on_d3d12_set_render_targets(ID3D12GraphicsCommandList5* cmd_list, UINT num_rtvs, 
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle, D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
    if (m_game_data_initialized) {
        m_mods->on_d3d12_set_render_targets(cmd_list, num_rtvs, rtvs, single_handle, dsv);
    }
}

void Framework::on_d3d12_set_scissor_rects(ID3D12GraphicsCommandList5* cmd_list, UINT num_rects, const D3D12_RECT* rects) {
    if (m_game_data_initialized) {
        m_mods->on_d3d12_set_scissor_rects(cmd_list, num_rects, rects);
    }
}


void Framework::on_d3d12_create_render_target_view(ID3D12Device* device, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    if (m_game_data_initialized) {
        m_mods->on_d3d12_create_render_target_view(device, pResource, pDesc, DestDescriptor);
    }
}

void Framework::on_d3d12_set_viewports(ID3D12GraphicsCommandList5* cmd_list, UINT num_viewports, const D3D12_VIEWPORT* viewports) {
    if (m_game_data_initialized) {
        m_mods->on_d3d12_set_viewports(cmd_list, num_viewports, viewports);
    }
}

void Framework::on_reset() {
    std::scoped_lock _{ m_imgui_mtx };

    spdlog::info("Reset!");

    if (m_initialized) {
        // fixes text boxes not being able to receive input
        //imgui::reset_keystates();
    }

    // Crashes if we don't release it at this point.
    if (m_is_d3d11) {
        deinit_d3d11();
    }

    if (m_is_d3d12) {
        deinit_d3d12();
    }

    if (m_game_data_initialized) {
        m_mods->on_device_reset();
    }

    m_has_frame = false;
    m_first_initialize = false;
    m_initialized = false;
}

void Framework::patch_set_cursor_pos() {
    std::scoped_lock _{ m_patch_mtx };

    if (m_set_cursor_pos_patch.get() == nullptr) {
        // Make SetCursorPos ret early
        const auto set_cursor_pos_addr = (uintptr_t)GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursorPos");

        if (set_cursor_pos_addr != 0) {
            spdlog::info("Patching SetCursorPos");
            m_set_cursor_pos_patch = Patch::create(set_cursor_pos_addr, {0xC3});
        }
    }
}

void Framework::remove_set_cursor_pos_patch() {
    std::scoped_lock _{ m_patch_mtx };

    if (m_set_cursor_pos_patch.get() != nullptr) {
        spdlog::info("Removing SetCursorPos patch");
    }

    m_set_cursor_pos_patch.reset();
}


void Framework::on_screen_to_client(auto result, auto wnd, auto point) {
    static auto vr = VR::get();

    if (wnd == m_wnd && vr->is_hmd_active() && *result == TRUE) {
        RECT real_client_rect{};
        if (WindowsMessageHook::GetClientRectOriginal(wnd, &real_client_rect)) {
            const float real_width = static_cast<float>(real_client_rect.right - real_client_rect.left);
            const float real_height = static_cast<float>(real_client_rect.bottom - real_client_rect.top);

            if (real_width > 0 && real_height > 0) {
                const float virtual_width = static_cast<float>(vr->get_hmd_width());
                const float virtual_height = static_cast<float>(vr->get_hmd_height());

                const float scale_x = virtual_width / real_width;
                const float scale_y = virtual_height / real_height;

                point->x = static_cast<LONG>(static_cast<float>(point->x) * scale_x);
                point->y = static_cast<LONG>(static_cast<float>(point->y) * scale_y);
            }
        }
    }
}


bool Framework::on_clip_cursor(auto lpRect) {
    if(m_draw_ui) {
        *lpRect = nullptr;
        return true;
    }
    return true;
}

void Framework::on_get_window_rect(auto result, auto wnd, auto rect) {
    static auto vr = VR::get();
    if(wnd == m_wnd && vr->is_hmd_active()) {
        int width = vr->get_hmd_width();
        int height = vr->get_hmd_height();
        RECT clientRect;
        WindowsMessageHook::GetClientRectOriginal(wnd, &clientRect);
        int origWidth = clientRect.right - clientRect.left;
        int origHeight = clientRect.bottom - clientRect.top;
        rect->right = rect->right - origWidth + width;
        rect->bottom = rect->bottom - origHeight + height;
    }
}

void Framework::on_get_client_rect(auto result, auto wnd, auto rect) {
    static auto vr = VR::get();
    if(wnd == m_wnd && vr->is_hmd_active()) {
        int width = vr->get_hmd_width();
        int height = vr->get_hmd_height();
        rect->right = rect->left + width;
        rect->bottom = rect->top + height;
    }
}

void Framework::on_adjust_window_rect(auto result, auto wnd, auto rect, auto menu) {
    static auto vr = VR::get();
//    if(wnd == m_wnd && vr->is_hmd_active()) {
//        int width = vr->get_hmd_width();
//        int height = vr->get_hmd_height();
//        rect->right = rect->left + width;
//        rect->bottom = rect->top + height;
//    }
}




bool Framework::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    m_last_message_time = std::chrono::steady_clock::now();

    if (!m_initialized) {
        return true;
    }

    bool is_mouse_moving{false};
    switch (message) {
    case WM_LBUTTONDOWN:
        m_last_keys[VK_LBUTTON] = true;
        break;
    case WM_LBUTTONUP:
        m_last_keys[VK_LBUTTON] = false;
        break;
    case WM_RBUTTONDOWN:
        m_last_keys[VK_RBUTTON] = true;
        break;
    case WM_RBUTTONUP:
        m_last_keys[VK_RBUTTON] = false;
        break;
    case WM_MBUTTONDOWN:
        m_last_keys[VK_MBUTTON] = true;
        break;
    case WM_MBUTTONUP:
        m_last_keys[VK_MBUTTON] = false;
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const auto menu_key = VRConfig::get()->get_menu_key()->value();

        if (w_param == menu_key && !m_last_keys[w_param] && !s_fc5_test_mode.load()) {
            std::lock_guard _{m_input_mutex};
            set_draw_ui(!m_draw_ui);
        }

        m_last_keys[w_param] = true;

        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        m_last_keys[w_param] = false;
        break;
    case WM_KILLFOCUS:
        std::fill(std::begin(m_last_keys), std::end(m_last_keys), false);
        break;
    case WM_INPUT: {
        // RIM_INPUT means the window has focus
        if (GET_RAWINPUT_CODE_WPARAM(w_param) == RIM_INPUT) {
            uint32_t size = sizeof(RAWINPUT);
            RAWINPUT raw{};

            // obtain size
            GetRawInputData((HRAWINPUT)l_param, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));

            auto result = GetRawInputData((HRAWINPUT)l_param, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER));

            if (raw.header.dwType == RIM_TYPEMOUSE) {
                m_accumulated_mouse_delta[0] += (float)raw.data.mouse.lLastX;
                m_accumulated_mouse_delta[1] += (float)raw.data.mouse.lLastY;

                // Allowing camera movement when the UI is hovered while not focused
                if (raw.data.mouse.lLastX || raw.data.mouse.lLastY) {
                    is_mouse_moving = true;
                }
            }
        }
    } break;

    case RE_TOGGLE_CURSOR: {
        const auto is_internal_message = l_param != 0;
        const auto return_value = is_internal_message || !m_draw_ui;

        if (!is_internal_message) {
            m_cursor_state = (bool)w_param;
            m_cursor_state_changed = true;
        }

        return return_value;
    } break;
    default:
        break;
    }

    ImGui_ImplWin32_WndProcHandler(wnd, message, w_param, l_param);

    {
        // If the user is interacting with the UI we block the message from going to the game.
        const auto& io = ImGui::GetIO();
        if (m_draw_ui && !m_ui_passthrough) {
            // Fix of a bug that makes the input key down register but the key up will never register
            // when clicking on the ui while the game is not focused
            if (message == WM_INPUT && GET_RAWINPUT_CODE_WPARAM(w_param) == RIM_INPUTSINK)
                return false;

            static std::unordered_set<UINT> forcefully_allowed_messages {
                WM_DEVICECHANGE,
                WM_SHOWWINDOW,
                WM_ACTIVATE,
                WM_ACTIVATEAPP,
                WM_CLOSE,
                WM_DPICHANGED,
                WM_SIZING,
                WM_MOUSEACTIVATE,
                RE_WM_DEVICECHANGE
            };

            if (!forcefully_allowed_messages.contains(message)) {
                if (m_is_ui_focused) {
                    if (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput)
                        return false;
                } else {
                    if (!is_mouse_moving && (io.WantCaptureMouse || io.WantCaptureKeyboard || io.WantTextInput))
                        return false;
                }
            }
        }
    }

    bool any_false = false;

    if (m_game_data_initialized) {
        for (auto& mod : m_mods->get_mods()) {
            if (!mod->on_message(wnd, message, w_param, l_param)) {
                any_false = true;
            }
        }
    }

    return !any_false;
}

// this is unfortunate.
void Framework::on_direct_input_keys(const std::array<uint8_t, 256>& keys) {
    /*const auto menu_key = REFrameworkConfig::get()->get_menu_key()->value();

    if (keys[menu_key] && m_last_keys[menu_key] == 0) {
        std::lock_guard _{m_input_mutex};

        set_draw_ui(!m_draw_ui);
    }

    m_last_keys = keys;*/
}

std::filesystem::path Framework::get_persistent_dir() {
    auto return_appdata_dir = []() -> std::filesystem::path {
        char app_data_path[MAX_PATH]{};
        SHGetSpecialFolderPathA(0, app_data_path, CSIDL_APPDATA, false);

        static const auto exe_name = [&]() {
            const auto result = std::filesystem::path(*utility::get_module_path(utility::get_executable())).stem().string();
            const auto dir = std::filesystem::path(app_data_path) / "VR" / result;
            std::filesystem::create_directories(dir);

            return result;
        }();

        return std::filesystem::path(app_data_path) / "VR" / exe_name;
    };

    if (s_fallback_appdata) {
        return return_appdata_dir();
    }

    if (s_checked_file_permissions) {
        static const auto result = std::filesystem::path(*utility::get_module_path(utility::get_executable())).parent_path();
        return result;
    }

    // Do some tests on the file creation/writing permissions of the current directory
    // If we can't write to the current directory, we fallback to the appdata folder
    try {
        const auto dir = std::filesystem::path(*utility::get_module_path(utility::get_executable())).parent_path();
        const auto test_file = dir / "test.txt";
        std::ofstream test_stream{test_file};
        test_stream << "test";
        test_stream.close();

        std::filesystem::create_directories(dir / "test_dir");
        std::filesystem::remove(test_file);
        std::filesystem::remove(dir / "test_dir");

        s_checked_file_permissions = true;
        s_fallback_appdata = false;
    } catch(...) {
        s_fallback_appdata = true;
        s_checked_file_permissions = true;
        return return_appdata_dir();
    }
    
    return std::filesystem::path(*utility::get_module_path(utility::get_executable())).parent_path();
}

void Framework::save_config() {
    std::scoped_lock _{m_config_mtx};

    m_wants_save_config = false;

    spdlog::info("Saving config vr_config.txt");

    utility::Config cfg{};

    for (auto& mod : m_mods->get_mods()) {
        mod->on_config_save(cfg);
    }

    try {
        if (!cfg.save((get_persistent_dir() / "vr_config.txt").string())) {
            spdlog::error("Failed to save config");
            return;
        }
    } catch(const std::exception& e) {
        spdlog::error("Failed to save config: {}", e.what());
        return;
    } catch(...) {
        spdlog::error("Unexpected error while saving config");
        return;
    }

    spdlog::info("Saved config");
}

void Framework::set_draw_ui(bool state, bool should_save) {
    std::scoped_lock _{m_config_mtx};

    bool prev_state = m_draw_ui;
    m_draw_ui = state;

    if (m_game_data_initialized) {
        VRConfig::get()->get_menu_open()->value() = state;
    }

    // Far Cry 5 (F11 blink report F2): not written at the moment the menu opens or closes (a disk write under the config and
    // input locks): 2 s later from draw_ui, once the toggling has stopped.
    if (state != prev_state && should_save && m_game_data_initialized) {
        m_fc5_save_due     = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        m_fc5_save_pending = true;
    }
}

void Framework::consume_input() {
    m_mouse_delta[0] = m_accumulated_mouse_delta[0];
    m_mouse_delta[1] = m_accumulated_mouse_delta[1];

    m_accumulated_mouse_delta[0] = 0.0f;
    m_accumulated_mouse_delta[1] = 0.0f;
}

int Framework::add_font(const std::filesystem::path& filepath, int size, const std::vector<ImWchar>& ranges) {
    // Look for a font already matching this description.
    for (int i = 0; i < m_additional_fonts.size(); ++i) {
        const auto& font = m_additional_fonts[i];

        if (font.filepath == filepath && font.size == size && font.ranges == ranges) {
            return i;
        }
    }

    m_additional_fonts.emplace_back(Framework::AdditionalFont{filepath, size, ranges, (ImFont*)nullptr});
    m_fonts_need_updating = true;

    return m_additional_fonts.size() - 1;
}

void Framework::update_fonts() {
    if (!m_fonts_need_updating) {
        return;
    }

    m_fonts_need_updating = false;

    auto& fonts = ImGui::GetIO().Fonts;
    fonts->Clear();

    // using 'vr_pictographic.mode' file to
    // replace '?' to most flag in WorldObjectsViewer
    ImFontConfig custom_icons{};
    custom_icons.FontDataOwnedByAtlas = false;
    ImFont* fsload = (INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA("vr_pictographic.mode"))
        ? fonts->AddFontFromMemoryTTF((void*)af_baidu_ptr, af_baidu_size, (float)m_font_size, &custom_icons, fonts->GetGlyphRangesChineseFull())
        : fonts->AddFontFromMemoryCompressedTTF(RobotoMedium_compressed_data, RobotoMedium_compressed_size, (float)m_font_size);

    // https://fontawesome.com/
    custom_icons.PixelSnapH = true;
    custom_icons.MergeMode = true;
    custom_icons.FontDataOwnedByAtlas = false;
    static const ImWchar icon_ranges[] = {0xF000, 0xF976, 0}; // ICON_MIN_FA ICON_MAX_FA
    fonts->AddFontFromMemoryTTF((void*)af_faprolight_ptr, af_faprolight_size, (float)m_font_size, &custom_icons, icon_ranges);

    for (auto& font : m_additional_fonts) {
        const ImWchar* ranges = nullptr;

        if (!font.ranges.empty()) {
            ranges = font.ranges.data();
        }

        if (fs::exists(font.filepath)) {
            font.font = fonts->AddFontFromFileTTF(font.filepath.string().c_str(), (float)font.size, nullptr, ranges);
        } else {
            font.font = fsload; // fonts->AddFontFromMemoryCompressedTTF(RobotoMedium_compressed_data, RobotoMedium_compressed_size, (float)font.size, nullptr, ranges);
        }
    }

    fonts->Build();
    m_wants_device_object_cleanup = true;
}

void Framework::invalidate_device_objects() {
    if (!m_wants_device_object_cleanup) {
        return;
    }

    if (m_renderer_type == RendererType::D3D11) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    } else if (m_renderer_type == RendererType::D3D12) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
    }

    m_wants_device_object_cleanup = false;
}

void Framework::draw_ui() {
    std::lock_guard _{m_input_mutex};

    if (m_fc5_save_pending && std::chrono::steady_clock::now() > m_fc5_save_due) {
        m_fc5_save_pending = false;
        std::scoped_lock _c{m_config_mtx};
        save_config();
    }

    ImGui::GetIO().MouseDrawCursor = m_draw_ui || VRConfig::get()->is_always_show_cursor();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // causes bugs with the cursor
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    if (!m_draw_ui) {
        // remove SetCursorPos patch
        if (!VRConfig::get()->is_always_show_cursor()) {
            remove_set_cursor_pos_patch();
        } else {
            patch_set_cursor_pos();
        }

        m_is_ui_focused = false;
        if (m_last_draw_ui) {
            m_windows_message_hook->window_toggle_cursor(m_cursor_state);
        }
        //m_dinput_hook->acknowledge_input();
        // ImGui::GetIO().MouseDrawCursor = false;
        return;
    }

    // UI Specific code:
    m_is_ui_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);

    if (m_ui_option_transparent) {
        auto& style = ImGui::GetStyle();
        if (m_is_ui_focused) {
            style.Alpha = 1.0f;
        } else {
            if (ImGui::IsWindowHovered(ImGuiFocusedFlags_AnyWindow)) {
                style.Alpha = 0.9f;
            } else {
                style.Alpha = 0.8f;
            }
        }
    } else {
        auto& style = ImGui::GetStyle();
        style.Alpha = 1.0f;
    }

    auto& io = ImGui::GetIO();

    if (io.WantCaptureKeyboard) {
        m_dinput_hook->ignore_input();
    } else {
        m_dinput_hook->acknowledge_input();
    }

    // ImGui::GetIO().MouseDrawCursor = true;
    if (!m_last_draw_ui || m_cursor_state_changed) {
        m_cursor_state_changed = false;
        m_windows_message_hook->window_toggle_cursor(true);
    }

    bool is_open = true;
    if (m_custom_menu && m_error.empty() && m_game_data_initialized) {
        const auto framework_items = [this] {
            ImGui::Checkbox("Transparency", &m_ui_option_transparent);
            ImGui::SameLine();
            ImGui::TextDisabled("(makes the menu transparent when not focused)");
            ImGui::Checkbox("Input Passthrough", &m_ui_passthrough);
            ImGui::SameLine();
            ImGui::TextDisabled("(mouse and keyboard also reach the game while the menu is open)");
            m_mods->on_draw_ui();
        };
        m_custom_menu(0, &is_open, framework_items);
        ImGui::Begin("FAR CRY 5 VR###fc5vr_menu", &is_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        m_custom_menu(1, &is_open, framework_items);
        m_last_window_pos  = ImGui::GetWindowPos();
        m_last_window_size = ImGui::GetWindowSize();
        ImGui::End();
        m_custom_menu(2, &is_open, framework_items);
    } else {
    ImGui::SetNextWindowPos(ImVec2(1000, 1000), ImGuiCond_::ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 500), ImGuiCond_::ImGuiCond_FirstUseEver);
    ImGui::Begin("VR Options", &is_open);
    ImGui::Text("Default Menu Key: F11");
    ImGui::Checkbox("Transparency", &m_ui_option_transparent);
    ImGui::SameLine();
    ImGui::Text("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Makes the UI transparent when not focused.");
    ImGui::Checkbox("Input Passthrough", &m_ui_passthrough);
    ImGui::SameLine();
    ImGui::Text("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Allows mouse and keyboard inputs to register to the game while the UI is focused.");

    // Mods:
//    draw_about();

    if (m_error.empty() && m_game_data_initialized) {
        m_mods->on_draw_ui();
    } else if (!m_game_data_initialized) {
        ImGui::TextWrapped("VR is currently initializing...");
        ImGui::TextWrapped("This menu will close after initialization if you have the remember option enabled.");
    } else if (!m_error.empty()) {
        ImGui::TextWrapped("VR error: %s", m_error.c_str());
    }

    m_last_window_pos = ImGui::GetWindowPos();
    m_last_window_size = ImGui::GetWindowSize();

    ImGui::End();
    }

    // save the menu state in config
    if (!is_open) {
        set_draw_ui(is_open, true);
    } else if (m_draw_ui != m_last_draw_ui) {
        set_draw_ui(m_draw_ui, true);
    }

    // if we pressed the X button to close the menu.
    if (m_last_draw_ui && !m_draw_ui) {
        m_windows_message_hook->window_toggle_cursor(m_cursor_state);
    }
}

void Framework::draw_about() {
    if (!ImGui::CollapsingHeader("About")) {
        return;
    }

    ImGui::TreePush("About");

//    ImGui::Text("Author: mutar");
//    ImGui::Text("Inspired by the Kanan project.");
//    ImGui::Text("https://github.com/mutars/");

    if (ImGui::CollapsingHeader("Licenses")) {
        ImGui::TreePush("Licenses");

        struct License {
            std::string name;
            std::string text;
        };

//        static std::array<License, 15> licenses{
//            License{ "glm", license::glm },
//            License{ "imgui", license::imgui },
//            License{ "minhook", license::minhook },
//            License{ "spdlog", license::spdlog },
//            License{ "robotomedium", license::roboto },
//            License{ "openvr", license::openvr },
//            License{ "lua", license::lua },
//            License{ "sol", license::sol },
//            License{ "json", license::json },
//            License{ "asmjit", license::asmjit },
//            License{ "bddisasm", utility::narrow(license::bddisasm) },
//            License{ "openxr", license::openxr },
//            License{ "imguizmo", license::imguizmo },
//            License{ "DirectXTK", license::directxtk },
//            License{ "DirectXTK12", license::directxtk },
//        };

        static std::array<License, 0> licenses{
        };

        for (const auto& license : licenses) {
            if (ImGui::CollapsingHeader(license.name.c_str())) {
                ImGui::TextWrapped(license.text.c_str());
            }
        }

        ImGui::TreePop();
    }

    ImGui::Separator();

    if (m_game_data_initialized && m_error.empty()) {
        try {
//            static auto version_t = sdk::find_type_definition("via.version");
//            static std::string clean_version{};
//            static std::string engine_config{};
//            static auto tdb_version = sdk::RETypeDB::get()->version;
//
//            if (version_t != nullptr && clean_version.empty()) {
//                auto m = version_t->get_method("getPrettyVersionString");
//
//                if (m != nullptr) {
//                    auto pretty_string = m->call<::SystemString*>(sdk::get_thread_context(), nullptr);
//
//                    if (pretty_string != nullptr) {
//                        clean_version = utility::re_string::get_string(pretty_string);
//                    }
//                }
//            }
//
//            if (version_t != nullptr && engine_config.empty()) {
//                auto m = version_t->get_method("getConfigName");
//
//                if (m != nullptr) {
//                    auto config_name = m->call<::SystemString*>(sdk::get_thread_context(), nullptr);
//
//                    if (config_name != nullptr) {
//                        engine_config = utility::re_string::get_string(config_name);
//                    }
//                }
//            }

            ImGui::Text("Engine information");
            ImGui::Text(" Config: %s", "engine_config.c_str()");
            ImGui::Text(" Version: %s", "clean_version.c_str()");
            ImGui::Text(" TDB Version: %i", "tdb_version");
        } catch(...) {
            ImGui::Text("Unable to determine engine version.");
        }
    } else {
        ImGui::Text("Unable to determine engine version.");
    }

    ImGui::TreePop();
}

void Framework::set_imgui_style() noexcept {
    ImGui::StyleColorsDark();

    auto& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.ScrollbarRounding = 2.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.WindowBorderSize = 2.0f;
    style.WindowPadding = ImVec2(2.0f, 0.0f);

    auto& colors = ImGui::GetStyle().Colors;
    // Window BG
    colors[ImGuiCol_WindowBg] = ImVec4{0.1f, 0.105f, 0.11f, 1.0f};

    // Navigatation highlight
    colors[ImGuiCol_NavHighlight] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};

    // Progress Bar
    colors[ImGuiCol_PlotHistogram] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};

    // Headers
    colors[ImGuiCol_Header] = ImVec4{0.2f, 0.205f, 0.21f, 1.0f};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.55f, 0.5505f, 0.551f, 1.0f};

    // Buttons
    colors[ImGuiCol_Button] = ImVec4{0.2f, 0.205f, 0.21f, 1.0f};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.55f, 0.5505f, 0.551f, 1.0f};

    // Checkbox
    colors[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.5505f, 0.551f, 1.0f);

    // Frame BG
    colors[ImGuiCol_FrameBg] = ImVec4{0.211f, 0.210f, 0.25f, 1.0f};
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.55f, 0.5505f, 0.551f, 1.0f};

    // Tabs
    colors[ImGuiCol_Tab] = ImVec4{0.25f, 0.2505f, 0.251f, 1.0f};
    colors[ImGuiCol_TabHovered] = ImVec4{0.38f, 0.3805f, 0.381f, 1.0f};
    colors[ImGuiCol_TabActive] = ImVec4{0.28f, 0.2805f, 0.281f, 1.0f};
    colors[ImGuiCol_TabUnfocused] = ImVec4{0.25f, 0.2505f, 0.251f, 1.0f};
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4{0.8f, 0.805f, 0.81f, 1.0f};

    // Resize Grip
    colors[ImGuiCol_ResizeGrip] = ImVec4{0.2f, 0.205f, 0.21f, 0.0f};
    colors[ImGuiCol_ResizeGripHovered] = ImVec4{0.3f, 0.305f, 0.31f, 1.0f};
    colors[ImGuiCol_ResizeGripActive] = ImVec4{0.55f, 0.5505f, 0.551f, 1.0f};

    // Title
    colors[ImGuiCol_TitleBg] = ImVec4{0.25f, 0.2505f, 0.251f, 1.0f};
    colors[ImGuiCol_TitleBgActive] = ImVec4{0.55f, 0.5505f, 0.551f, 1.0f};
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{0.25f, 0.2505f, 0.251f, 1.0f};

    // Font
    set_font_size(m_font_size);
}

bool Framework::initialize() {
    if (m_initialized) {
        return true;
    }

    if (static bool first = true; std::exchange(first, false)) {
        fc5_boot_step("first present (Framework::initialize)");
    }
    gowvr::setup_exception_handler();

    if (m_first_initialize) {
        m_frames_since_init = 0;
        m_first_initialize = false;
    }

    if (m_frames_since_init < 60) {
        m_frames_since_init++;
        return false;
    }

    if (m_is_d3d11) {
        spdlog::info("Attempting to initialize DirectX 11");

        if (!m_d3d11_hook->is_hooked()) {
            return false;
        }

        auto device = m_d3d11_hook->get_device();
        auto swap_chain = m_d3d11_hook->get_swap_chain();

        // Wait.
        if (device == nullptr || swap_chain == nullptr) {
            m_first_initialize = true;

            spdlog::info("Device or SwapChain null. DirectX 12 may be in use. Unhooking D3D11...");

            // We unhook D3D11
            if (m_d3d11_hook->unhook()) {
                spdlog::info("D3D11 unhooked!");
            } else {
                spdlog::error("Cannot unhook D3D11, this might crash.");
            }

            m_is_d3d11 = false;
            m_valid = false;

            // We hook D3D12
            if (!hook_d3d12()) {
                spdlog::error("Failed to hook D3D12 after unhooking D3D11.");
            }
            return false;
        }

        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swap_chain->GetDesc(&swap_desc);

        m_wnd = swap_desc.OutputWindow;


        spdlog::info("Window Handle: {0:x}", (uintptr_t)m_wnd);
        spdlog::info("Initializing ImGui");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
//        ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
//        ImNodes::CreateContext();

        set_imgui_style();

        static const auto imgui_ini = (get_persistent_dir() / "ref_ui.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();

        spdlog::info("Initializing ImGui Win32");

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            spdlog::error("Failed to initialize ImGui.");
            return false;
        }

        spdlog::info("Creating render target");

        if (!init_d3d11()) {
            spdlog::error("Failed to init D3D11");
            return false;
        }
    } else if (m_is_d3d12) {
        spdlog::info("Attempting to initialize DirectX 12");

        if (!m_d3d12_hook->is_hooked()) {
            return false;
        }

        auto device = m_d3d12_hook->get_device();
        auto swap_chain = m_d3d12_hook->get_swap_chain();

        if (device == nullptr || swap_chain == nullptr) {
            m_first_initialize = true;

            spdlog::info("Device: {:x}", (uintptr_t)device);
            spdlog::info("SwapChain: {:x}", (uintptr_t)swap_chain);

            spdlog::info("Device or SwapChain null. DirectX 11 may be in use. Unhooking D3D12...");

            // We unhook D3D12
            if (m_d3d12_hook->unhook())
                spdlog::info("D3D12 unhooked!");
            else
                spdlog::error("Cannot unhook D3D12, this might crash.");

            m_valid = false;
            m_is_d3d12 = false;

            // We hook D3D11
            if (!hook_d3d11()) {
                spdlog::error("Failed to hook D3D11 after unhooking D3D12.");
            }
            return false;
        }

        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swap_chain->GetDesc(&swap_desc);

        m_wnd = swap_desc.OutputWindow;


        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
//        ImNodes::SetImGuiContext(ImGui::GetCurrentContext());
//        ImNodes::CreateContext();

        set_imgui_style();

        static const auto imgui_ini = (get_persistent_dir() / "vr_ui.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();

        if (!ImGui_ImplWin32_Init(m_wnd)) {
            spdlog::error("Failed to initialize ImGui ImplWin32.");
            return false;
        }

        if (!init_d3d12()) {
            spdlog::error("Failed to init D3D12.");
            return false;
        }
    } else {
        return false;
    }

    initialize_windows_message_hook();
#ifdef HOOK_XINPUT
        initialize_xinput_hook();
#endif

    if (m_first_frame) {
        m_dinput_hook = std::make_unique<DInputHook>(m_wnd);
    } else {
        m_dinput_hook->set_window(m_wnd);
    }

    if (m_first_frame) {
        m_first_frame = false;
        initialize_game_data();
    }

    return true;
}

bool Framework::initialize_game_data() {
    if (m_game_data_initialized || m_started_game_data_thread) {
        return true;
    }

    m_started_game_data_thread = true;

    spdlog::info("Starting game data initialization thread");

    // Game specific initialization stuff
    std::thread init_thread([this]() {
        std::scoped_lock _{this->m_startup_mutex};

        try {

            //gowvr::initialize_sdk();


            fc5_boot_step("mods initializing");
            m_mods = std::make_unique<Mods>();

            auto e = m_mods->on_initialize();
            fc5_boot_step("mods initialized");

            if (e) {
                if (e->empty()) {
                    m_error = "An unknown error has occurred.";
                } else {
                    m_error = *e;
                }

                spdlog::error("Initialization of mods failed. Reason: {}", m_error);
            }

            m_game_data_initialized = true;
        } catch(const std::exception& e) {
            m_error = e.what();
            m_game_data_initialized = true;
            spdlog::error("Initialization of mods failed. Reason: {}", m_error);
        }
        catch(...) {
            m_error = "An exception has occurred during initialization.";
            m_game_data_initialized = true;
            spdlog::error("Initialization of mods failed. Reason: exception thrown.");
            fc5_crash_log_caught("mods initialization");
        }
        spdlog::info("Game data initialization thread finished");
    });

    init_thread.detach();

    return true;
}


bool Framework::initialize_xinput_hook() {
    if (m_first_frame || m_xinput_hook == nullptr) {
        m_xinput_hook.reset();
        m_xinput_hook = std::make_unique<XInputHook>();
    }

    return true;
}


void Framework::post_message(UINT message, WPARAM w_param, LPARAM l_param) {
    if (m_wnd == nullptr) {
        return;
    }

    PostMessage(m_wnd, message, w_param, l_param);
}


bool Framework::initialize_windows_message_hook() {
    if (m_wnd == 0) {
        return false;
    }

    if (m_first_frame || m_message_hook_requested || m_windows_message_hook == nullptr) {
        m_last_message_time = std::chrono::steady_clock::now();
        m_windows_message_hook.reset();
        m_windows_message_hook = std::make_unique<WindowsMessageHook>(m_wnd);
        m_windows_message_hook->on_message = [this](auto wnd, auto msg, auto w_param, auto l_param) {
            return on_message(wnd, msg, w_param, l_param);
        };
        m_windows_message_hook->on_get_window_rect = [this](auto result, auto hWnd, auto rect) {
            on_get_window_rect(result, hWnd, rect);
        };
        m_windows_message_hook->on_get_client_rect = [this](auto result, auto hWnd, auto rect) {
            on_get_client_rect(result, hWnd, rect);
        };
        m_windows_message_hook->on_adjust_window_rect = [this](auto result, auto wnd, auto rect, auto menu) {
            on_adjust_window_rect(result, wnd, rect, menu);
        };
        m_windows_message_hook->on_clip_cursor = [this](auto rect) {
            return on_clip_cursor(rect);
        };
        m_windows_message_hook->on_screen_to_client = [this](auto result, auto hWnd, auto point) {
            on_screen_to_client(result, hWnd, point);
        };

        m_message_hook_requested = false;
        return true;
    }

    m_message_hook_requested = false;
    return false;
}

// Ran on the first valid frame after pre-initialization of mods has taken place and hasn't failed
// This one allows mods to run any initialization code in the context of the D3D thread (like VR code)
// It also is the one that actually loads any config files
bool Framework::first_frame_initialize() {
    const bool is_init_ok = m_error.empty() && m_game_data_initialized;

    if (!is_init_ok || !m_first_frame_d3d_initialize) {
        return is_init_ok;
    }

    std::scoped_lock _{get_hook_monitor_mutex()};

    spdlog::info("Running first frame D3D initialization of mods...");

    m_first_frame_d3d_initialize = false;
    fc5_boot_step("mods: D3D thread init");
    auto e = m_mods->on_initialize_d3d_thread();

    if (e) {
        if (e->empty()) {
            m_error = "An unknown error has occurred.";
        } else {
            m_error = *e;
        }

        spdlog::error("Initialization of mods failed. Reason: {}", m_error);
        m_game_data_initialized = false;
        m_mods_fully_initialized = false;
        return false;
    } else {
        // Do an initial config save to set the default values for the frontend
        save_config();
        m_mods_fully_initialized = true;
    }

    return true;
}

void Framework::call_on_frame() {
    const bool is_init_ok = m_error.empty() && m_game_data_initialized;

    if (is_init_ok) {
        // Run mod frame callbacks.
        m_mods->on_frame();
    }
}

// DirectX 11 Initialization methods

bool Framework::init_d3d11() {
    deinit_d3d11();

    auto swapchain = m_d3d11_hook->get_swap_chain();
    auto device = m_d3d11_hook->get_device();

    // Get back buffer.
    spdlog::info("[D3D11] Creating RTV of back buffer...");

    ComPtr<ID3D11Texture2D> backbuffer{};

    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[D3D11] Failed to get back buffer!");
        return false;
    }

    // Create a render target view of the back buffer.
    if (FAILED(device->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_d3d11.bb_rtv))) {
        spdlog::error("[D3D11] Failed to create back buffer render target view!");
        return false;
    }

    // Get backbuffer description.
    D3D11_TEXTURE2D_DESC backbuffer_desc{};

    backbuffer->GetDesc(&backbuffer_desc);
    backbuffer_desc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    spdlog::info("[D3D11] Back buffer format is {}", backbuffer_desc.Format);

    // Create our blank render target.
    spdlog::info("[D3D11] Creating render targets...");
    {
        // Create our blank render target.
        auto d3d11_rt_desc = backbuffer_desc;
        d3d11_rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // For VR

        if (FAILED(device->CreateTexture2D(&d3d11_rt_desc, nullptr, &m_d3d11.blank_rt))) {
            spdlog::error("[D3D11] Failed to create render target texture!");
            return false;
        }

        // Create our render target
        if (FAILED(device->CreateTexture2D(&d3d11_rt_desc, nullptr, &m_d3d11.rt))) {
            spdlog::error("[D3D11] Failed to create render target texture!");
            return false;
        }
    }

    // Create our blank render target view.
    spdlog::info("[D3D11] Creating rtvs...");

    if (FAILED(device->CreateRenderTargetView(m_d3d11.blank_rt.Get(), nullptr, &m_d3d11.blank_rt_rtv))) {
        spdlog::error("[D3D11] Failed to create render terget view!");
        return false;
    }


    // Create our render target view.
    if (FAILED(device->CreateRenderTargetView(m_d3d11.rt.Get(), nullptr, &m_d3d11.rt_rtv))) {
        spdlog::error("[D3D11] Failed to create render terget view!");
        return false;
    }

    // Create our render target shader resource view.
    spdlog::info("[D3D11] Creating srvs...");

    if (FAILED(device->CreateShaderResourceView(m_d3d11.rt.Get(), nullptr, &m_d3d11.rt_srv))) {
        spdlog::error("[D3D11] Failed to create shader resource view!");
        return false;
    }

    m_d3d11.rt_width = backbuffer_desc.Width;
    m_d3d11.rt_height = backbuffer_desc.Height;

    spdlog::info("[D3D11] Initializing ImGui D3D11...");

    ComPtr<ID3D11DeviceContext> context{};

    device->GetImmediateContext(&context);

    if (!ImGui_ImplDX11_Init(device, context.Get())) {
        spdlog::error("[D3D11] Failed to initialize ImGui.");
        return false;
    }

    {
        ImGuiIO& io = ImGui::GetIO();

        // Get your window and swapchain dimensions
        RECT windowRect;
        ::GetClientRect(m_wnd, &windowRect);
        auto windowWidth = static_cast<float>(windowRect.right - windowRect.left);
        auto windowHeight = static_cast<float>(windowRect.bottom - windowRect.top);

        auto textureWidth = (float ) backbuffer_desc.Width;
        auto textureHeight = (float) backbuffer_desc.Height;

        float scaleX = textureWidth / windowWidth;
        float scaleY = textureHeight / windowHeight;

        io.DisplayFramebufferScale = ImVec2(scaleX, scaleY);
    }
    if(m_game_data_initialized) {
        m_mods->on_d3d11_initialize(device, backbuffer_desc);
    }

    return true;
}

void Framework::deinit_d3d11() {
    auto bd = ImGui::GetIO().BackendRendererUserData;
    if(bd) {
        ImGui_ImplDX11_Shutdown();
    }
    m_d3d11 = {};
}

// DirectX 12 Initialization methods

bool Framework::init_d3d12() {
    deinit_d3d12();
    
    auto device = m_d3d12_hook->get_device();

    m_d3d12.cmd_ctxs.clear();

    for (auto i = 0; i < 3; ++i) {
        auto& ctx = m_d3d12.cmd_ctxs.emplace_back(std::make_unique<d3d12::CommandContext>());

        if (!ctx->setup(L"Framework::m_d3d12.cmd_ctx")) {
            spdlog::error("[D3D12] Failed to create command context.");
            return false;
        }
    }

    spdlog::info("[D3D12] Creating RTV descriptor heap...");

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};

        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = (int)D3D12::RTV::COUNT; 
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;

        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_d3d12.rtv_desc_heap)))) {
            spdlog::error("[D3D12] Failed to create RTV descriptor heap.");
            return false;
        }

        m_d3d12.rtv_desc_heap->SetName(L"Framework::m_d3d12.rtv_desc_heap");
    }

    spdlog::info("[D3D12] Creating SRV descriptor heap...");

    { 
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = (int)D3D12::SRV::COUNT;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_d3d12.srv_desc_heap)))) {
            spdlog::error("[D3D12] Failed to create SRV descriptor heap.");
            return false;
        }

        m_d3d12.srv_desc_heap->SetName(L"Framework::m_d3d12.srv_desc_heap");
    }

    spdlog::info("[D3D12] Creating render targets...");

    auto swapchain = m_d3d12_hook->get_swap_chain();

    DXGI_SWAP_CHAIN_DESC swapchain_desc{};

    if (FAILED(swapchain->GetDesc(&swapchain_desc))) {
        spdlog::error("[D3D12] Failed to get swap chain description.");
        return false;
    }

    spdlog::info("[D3D12] Swapchain buffer count: {}", swapchain_desc.BufferCount);

    {
        // Create back buffer rtvs.
        if (swapchain_desc.BufferCount > (int)D3D12::RTV::BACKBUFFER_LAST + 1) {
            spdlog::warn("[D3D12] Too many back buffers ({} vs {}).", swapchain_desc.BufferCount, (int)D3D12::RTV::BACKBUFFER_LAST + 1);
        }

        for (auto i = 0; i < (int)swapchain_desc.BufferCount; ++i) {
            if (SUCCEEDED(swapchain->GetBuffer(i, IID_PPV_ARGS(&m_d3d12.rts[i])))) {
                device->CreateRenderTargetView(m_d3d12.rts[i].Get(), nullptr, m_d3d12.get_cpu_rtv(device, (D3D12::RTV)i));
            } else {
                spdlog::error("[D3D12] Failed to get back buffer for rtv {}", i);
                m_d3d12.rts[i].Reset(); // just in case
            }
        }

        // Create our imgui and blank rts.
        auto& backbuffer = m_d3d12.get_rt(D3D12::RTV::BACKBUFFER_0);

        if (backbuffer == nullptr) {
            spdlog::error("[D3D12] Failed to get first back buffer RTV.");
            deinit_d3d12();
            return false;
        }

        auto desc = backbuffer->GetDesc();

        spdlog::info("[D3D12] Back buffer format is {}", desc.Format);

        D3D12_HEAP_PROPERTIES props{};
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto d3d12_rt_desc = desc;
        d3d12_rt_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // For VR

        D3D12_CLEAR_VALUE clear_value{};
        clear_value.Format = d3d12_rt_desc.Format;

        if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d3d12_rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value,
                IID_PPV_ARGS(&m_d3d12.get_rt(D3D12::RTV::IMGUI))))) {
            spdlog::error("[D3D12] Failed to create the imgui render target.");
            return false;
        }

        m_d3d12.get_rt(D3D12::RTV::IMGUI)->SetName(L"Framework::m_d3d12.rts[IMGUI]");

        if (FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d3d12_rt_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear_value,
                IID_PPV_ARGS(&m_d3d12.get_rt(D3D12::RTV::BLANK))))) {
            spdlog::error("[D3D12] Failed to create the blank render target.");
            return false;
        }

        m_d3d12.get_rt(D3D12::RTV::BLANK)->SetName(L"Framework::m_d3d12.rts[BLANK]");

        // Create imgui and blank rtvs and srvs.
        device->CreateRenderTargetView(m_d3d12.get_rt(D3D12::RTV::IMGUI).Get(), nullptr, m_d3d12.get_cpu_rtv(device, D3D12::RTV::IMGUI));
        device->CreateRenderTargetView(m_d3d12.get_rt(D3D12::RTV::BLANK).Get(), nullptr, m_d3d12.get_cpu_rtv(device, D3D12::RTV::BLANK));
        device->CreateShaderResourceView(
            m_d3d12.get_rt(D3D12::RTV::IMGUI).Get(), nullptr, m_d3d12.get_cpu_srv(device, D3D12::SRV::IMGUI_VR));
        device->CreateShaderResourceView(m_d3d12.get_rt(D3D12::RTV::BLANK).Get(), nullptr, m_d3d12.get_cpu_srv(device, D3D12::SRV::BLANK));

        m_d3d12.rt_width = (uint32_t)desc.Width;
        m_d3d12.rt_height = (uint32_t)desc.Height;

        m_last_rt_size = {desc.Width, desc.Height};
    }

    spdlog::info("[D3D12] Initializing ImGui...");

    auto& bb = m_d3d12.get_rt(D3D12::RTV::BACKBUFFER_0);
    auto bb_desc = bb->GetDesc();

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = device;
    init_info.CommandQueue = g_framework->get_d3d12_hook()->get_command_queue();
    init_info.NumFramesInFlight = swapchain_desc.BufferCount;
    init_info.RTVFormat = bb_desc.Format;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = m_d3d12.srv_desc_heap.Get();
    init_info.LegacySingleSrvCpuDescriptor = m_d3d12.get_cpu_srv(device, D3D12::SRV::IMGUI_FONT_BACKBUFFER);
    init_info.LegacySingleSrvGpuDescriptor = m_d3d12.get_gpu_srv(device, D3D12::SRV::IMGUI_FONT_BACKBUFFER);

    if (!ImGui_ImplDX12_Init(&init_info)) {
        spdlog::error("[D3D12] Failed to initialize ImGui.");
        return false;
    }


    m_d3d12.imgui_backend_datas[0] = ImGui::GetIO().BackendRendererUserData;

    ImGui::GetIO().BackendRendererUserData = nullptr;

    // Now initialize another one for the VR texture.
    auto& bb_vr = m_d3d12.get_rt(D3D12::RTV::IMGUI);
    auto bb_vr_desc = bb_vr->GetDesc();

    ImGui_ImplDX12_InitInfo init_info_vr{};
    init_info_vr.Device = device;
    init_info_vr.CommandQueue = g_framework->get_d3d12_hook()->get_command_queue();
    init_info_vr.NumFramesInFlight = swapchain_desc.BufferCount;
    init_info_vr.RTVFormat = bb_vr_desc.Format;
    init_info_vr.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info_vr.SrvDescriptorHeap = m_d3d12.srv_desc_heap.Get();
    init_info_vr.LegacySingleSrvCpuDescriptor = m_d3d12.get_cpu_srv(device, D3D12::SRV::IMGUI_FONT_VR);
    init_info_vr.LegacySingleSrvGpuDescriptor = m_d3d12.get_gpu_srv(device, D3D12::SRV::IMGUI_FONT_VR);

    if (!ImGui_ImplDX12_Init(&init_info_vr)) {
        spdlog::error("[D3D12] Failed to initialize ImGui.");
        return false;
    }

    {
        ImGuiIO& io = ImGui::GetIO();

        // Get your window and swapchain dimensions
        RECT windowRect;
        ::GetClientRect(m_wnd, &windowRect);
        auto windowWidth = static_cast<float>(windowRect.right - windowRect.left);
        auto windowHeight = static_cast<float>(windowRect.bottom - windowRect.top);

        auto textureWidth = (float)swapchain_desc.BufferDesc.Width;
        auto textureHeight = (float)swapchain_desc.BufferDesc.Height;

        float scaleX = textureWidth / windowWidth;
        float scaleY = textureHeight / windowHeight;

        io.DisplayFramebufferScale = ImVec2(scaleX, scaleY);
    }



    m_d3d12.imgui_backend_datas[1] = ImGui::GetIO().BackendRendererUserData;
    if(m_game_data_initialized) {
        m_mods->on_d3d12_initialize(device, bb_desc);
    }
    return true;
}

void Framework::deinit_d3d12() {
    for (auto& ctx : m_d3d12.cmd_ctxs) {
        if (ctx != nullptr) {
            ctx->reset();
        }
    }

    m_d3d12.cmd_ctxs.clear();

    for (auto userdata : m_d3d12.imgui_backend_datas) {
        if (userdata != nullptr) {
            ImGui::GetIO().BackendRendererUserData = userdata;
            ImGui_ImplDX12_Shutdown();
        }
    }

    ImGui::GetIO().BackendRendererUserData = nullptr;
    m_d3d12 = {};
}