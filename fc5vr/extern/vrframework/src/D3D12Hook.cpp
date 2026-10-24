#include <thread>
#include <future>
#include <unordered_set>

#include <mods/VR.hpp>
#include <spdlog/spdlog.h>
#include <utility/Module.hpp>
#include <utility/Thread.hpp>

#include "Framework.hpp"

#include "WindowFilter.hpp"

#include "D3D12Hook.hpp"

static D3D12Hook* g_d3d12_hook = nullptr;

D3D12Hook::~D3D12Hook() {
    unhook();
}

bool D3D12Hook::hook() {
    spdlog::info("Hooking D3D12");

    g_d3d12_hook = this;

    IDXGISwapChain1* swap_chain1{ nullptr };
    IDXGISwapChain3* swap_chain{ nullptr };
    ID3D12Device* device{ nullptr };

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc1;

    ZeroMemory(&swap_chain_desc1, sizeof(swap_chain_desc1));

    swap_chain_desc1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swap_chain_desc1.BufferCount = 2;
    swap_chain_desc1.SampleDesc.Count = 1;
    swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    swap_chain_desc1.Width = 1;
    swap_chain_desc1.Height = 1;

    // Manually get D3D12CreateDevice export because the user may be running Windows 7
    const auto d3d12_module = LoadLibraryA("d3d12.dll");
    if (d3d12_module == nullptr) {
        spdlog::error("Failed to load d3d12.dll");
        return false;
    }

    auto d3d12_create_device = (decltype(D3D12CreateDevice)*)GetProcAddress(d3d12_module, "D3D12CreateDevice");
    if (d3d12_create_device == nullptr) {
        spdlog::error("Failed to get D3D12CreateDevice export");
        return false;
    }

    spdlog::info("Creating dummy device");

    // Get the original on-disk bytes of the D3D12CreateDevice export
    const auto original_bytes = utility::get_original_bytes(d3d12_create_device);

    // Temporarily unhook D3D12CreateDevice
    // it allows compatibility with ReShade and other overlays that hook it
    // this is just a dummy device anyways, we don't want the other overlays to be able to use it
    if (original_bytes) {
        spdlog::info("D3D12CreateDevice appears to be hooked, temporarily unhooking");

        std::vector<uint8_t> hooked_bytes(original_bytes->size());
        memcpy(hooked_bytes.data(), d3d12_create_device, original_bytes->size());

        ProtectionOverride protection_override{ d3d12_create_device, original_bytes->size(), PAGE_EXECUTE_READWRITE };
        memcpy(d3d12_create_device, original_bytes->data(), original_bytes->size());
        
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
            return false;
        }

        spdlog::info("Restoring hooked bytes for D3D12CreateDevice");
        memcpy(d3d12_create_device, hooked_bytes.data(), hooked_bytes.size());
    } else { // D3D12CreateDevice is not hooked
        if (FAILED(d3d12_create_device(nullptr, feature_level, IID_PPV_ARGS(&device)))) {
            spdlog::error("Failed to create D3D12 Dummy device");
            return false;
        }
    }

    spdlog::info("Dummy device: {:x}", (uintptr_t)device);

    // Manually get CreateDXGIFactory export because the user may be running Windows 7
    const auto dxgi_module = LoadLibraryA("dxgi.dll");
    if (dxgi_module == nullptr) {
        spdlog::error("Failed to load dxgi.dll");
        return false;
    }

    auto create_dxgi_factory = (decltype(CreateDXGIFactory)*)GetProcAddress(dxgi_module, "CreateDXGIFactory");

    if (create_dxgi_factory == nullptr) {
        spdlog::error("Failed to get CreateDXGIFactory export");
        return false;
    }

    spdlog::info("Creating dummy DXGI factory");

    IDXGIFactory4* factory{ nullptr };
    if (FAILED(create_dxgi_factory(IID_PPV_ARGS(&factory)))) {
        spdlog::error("Failed to create D3D12 Dummy DXGI Factory");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = 0;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queue_desc.NodeMask = 0;

    spdlog::info("Creating dummy command queue");

    ID3D12CommandQueue* command_queue{ nullptr };
    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue)))) {
        spdlog::error("Failed to create D3D12 Dummy Command Queue");
        return false;
    }

    spdlog::info("Creating dummy swapchain");

    // used in CreateSwapChainForHwnd fallback
    HWND hwnd = 0;
    WNDCLASSEX wc{};

    auto init_dummy_window = [&]() {
        // fallback to CreateSwapChainForHwnd
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hIcon = NULL;
        wc.hCursor = NULL;
        wc.hbrBackground = NULL;
        wc.lpszMenuName = NULL;
        wc.lpszClassName = TEXT("VR_DX12_DUMMY");
        wc.hIconSm = NULL;

        ::RegisterClassEx(&wc);

        hwnd = ::CreateWindow(wc.lpszClassName, TEXT("REF DX Dummy Window"), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

        swap_chain_desc1.BufferCount = 3;
        swap_chain_desc1.Width = 0;
        swap_chain_desc1.Height = 0;
        swap_chain_desc1.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_chain_desc1.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        swap_chain_desc1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_chain_desc1.SampleDesc.Count = 1;
        swap_chain_desc1.SampleDesc.Quality = 0;
        swap_chain_desc1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_chain_desc1.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swap_chain_desc1.Scaling = DXGI_SCALING_STRETCH;
        swap_chain_desc1.Stereo = FALSE;
    };

    std::vector<std::function<bool ()>> swapchain_attempts{
        // we call CreateSwapChainForComposition instead of CreateSwapChainForHwnd
        // because some overlays will have hooks on CreateSwapChainForHwnd
        // and all we're doing is creating a dummy swapchain
        // we don't want to screw up the overlay
        [&]() {
            return !FAILED(factory->CreateSwapChainForComposition(command_queue, &swap_chain_desc1, nullptr, &swap_chain1));
        },
        [&]() {
            init_dummy_window();

            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, hwnd, &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
        [&]() {
            return !FAILED(factory->CreateSwapChainForHwnd(command_queue, GetDesktopWindow(), &swap_chain_desc1, nullptr, nullptr, &swap_chain1));
        },
    };

    bool any_succeed = false;

    for (auto i = 0; i < swapchain_attempts.size(); i++) {
        auto& attempt = swapchain_attempts[i];
        
        try {
            spdlog::info("Trying swapchain attempt {}", i);

            if (attempt()) {
                spdlog::info("Created dummy swapchain on attempt {}", i);
                any_succeed = true;
                break;
            }
        } catch (std::exception& e) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: {}", i, e.what());
        } catch(...) {
            spdlog::error("Failed to create dummy swapchain on attempt {}: unknown exception", i);
        }

        spdlog::error("Attempt {} failed", i);
    }

    if (!any_succeed) {
        spdlog::error("Failed to create D3D12 Dummy Swap Chain");

        if (hwnd) {
            ::DestroyWindow(hwnd);
        }

        if (wc.lpszClassName != nullptr) {
            ::UnregisterClass(wc.lpszClassName, wc.hInstance);
        }

        return false;
    }

    spdlog::info("Querying dummy swapchain");

    if (FAILED(swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain)))) {
        spdlog::error("Failed to retrieve D3D12 DXGI SwapChain");
        return false;
    }

    spdlog::info("Finding command queue offset");

    // Find the command queue offset in the swapchain
    for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
        const auto base = (uintptr_t)swap_chain1 + i;

        // reached the end
        if (IsBadReadPtr((void*)base, sizeof(void*))) {
            break;
        }

        auto data = *(ID3D12CommandQueue**)base;

        if (data == command_queue) {
            m_command_queue_offset = i;
            spdlog::info("Found command queue offset: {:x}", i);
            break;
        }
    }

    // Scan throughout the swapchain for a valid pointer to scan through
    // this is usually only necessary for Proton
    if (m_command_queue_offset == 0) {
        for (auto base = 0; base < 512 * sizeof(void*); base += sizeof(void*)) {
            const auto pre_scan_base = (uintptr_t)swap_chain1 + base;

            // reached the end
            if (IsBadReadPtr((void*)pre_scan_base, sizeof(void*))) {
                break;
            }

            const auto scan_base = *(uintptr_t*)pre_scan_base;

            if (scan_base == 0 || IsBadReadPtr((void*)scan_base, sizeof(void*))) {
                continue;
            }

            for (auto i = 0; i < 512 * sizeof(void*); i += sizeof(void*)) {
                const auto pre_data = scan_base + i;

                if (IsBadReadPtr((void*)pre_data, sizeof(void*))) {
                    break;
                }

                auto data = *(ID3D12CommandQueue**)pre_data;

                if (data == command_queue) {
                    m_using_proton_swapchain = true;
                    m_command_queue_offset = i;
                    m_proton_swapchain_offset = base;

                    spdlog::info("Proton potentially detected");
                    spdlog::info("Found command queue offset: {:x}", i);
                    break;
                }
            }

            if (m_using_proton_swapchain) {
                break;
            }
        }
    }

    if (m_command_queue_offset == 0) {
        spdlog::error("Failed to find command queue offset");
        return false;
    }

    ID3D12CommandAllocator* cmd_allocator{};
    ID3D12GraphicsCommandList* cmd_list{};

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd_allocator)))) {
        spdlog::error("[VR] Failed to create command allocator  for hooking");
    }


    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd_allocator, nullptr, IID_PPV_ARGS(&cmd_list)))) {
        spdlog::error("[VR] Failed to create command list for hooking");
    }

    ID3D12GraphicsCommandList5* cmd_list5{};
    if (FAILED(cmd_list->QueryInterface(IID_PPV_ARGS(&cmd_list5)))) {
        spdlog::error("[VR] Failed to query ID3D12GraphicsCommandList5 for hooking");
    }


    utility::ThreadSuspender suspender{};

    try {
        spdlog::info("Initializing hooks");

        m_present_hook.reset();
        m_swapchain_hook.reset();
        m_create_render_target_view_hook.reset();
        m_create_depth_stencil_view_hook.reset();
        m_is_phase_1 = true;

        auto& present_fn = (*(void***)swap_chain)[8]; // Present
        m_present_hook = std::make_unique<PointerHook>(&present_fn, (void*)&D3D12Hook::present);
        auto& set_render_targets_fn = (*(void***)cmd_list)[46];
        auto& set_scissor_rects_fn = (*(void***)cmd_list)[22];
        auto& set_viewports_fn = (*(void***)cmd_list)[21];
        auto& create_render_target_view_fn = (*(void***)device)[20];
        // auto& set_shading_rate_image_fn = (*(void***)cmd_list)[78];
        // auto& set_shading_rate = (*(void***)cmd_list)[77];
//        auto& set_pipeline_state_fn = (*(void***)cmd_list)[25];
//        auto Barriers = VtableIndexFinder::getIndexOf(&ID3D12GraphicsCommandList::ResourceBarrier);
//        spdlog::info("ResourceBarrier offset: {}", Barriers);
//        auto ExecuteIndirectOff = VtableIndexFinder::getIndexOf(&ID3D12GraphicsCommandList::ExecuteIndirect);

//        spdlog::info("Dispatch offset: 14, ExecuteIndirect offset: 59");
        // we hook Dispatch and ExecuteIndirect because they are called after SetRenderTargets
#ifdef VRMOD_EXPERIMENTAL
        m_set_render_targets_hook = std::make_unique<PointerHook>(&set_render_targets_fn, (void*)&D3D12Hook::set_render_targets);
        m_on_set_scissor_rects_hook = std::make_unique<PointerHook>(&set_scissor_rects_fn, (void*)&D3D12Hook::set_scissor_rects);
        m_set_viewports_hook = std::make_unique<PointerHook>(&set_viewports_fn, (void*)&D3D12Hook::set_viewports);
        m_create_render_target_view_hook = std::make_unique<PointerHook>(&create_render_target_view_fn, (void*)&D3D12Hook::create_render_target_view);
#endif
        //        m_commandlist_dispatch_hook = std::make_unique<PointerHook>(&(*(void***)cmd_list)[14], (void*)&D3D12Hook::dispatch);
//        m_commandlist_execute_indirect_hook = std::make_unique<PointerHook>(&(*(void***)cmd_list)[59], (void*)&D3D12Hook::execute_indirect);
//        m_commandlist_resource_barriers_hook = std::make_unique<PointerHook>(&(*(void***)cmd_list)[Barriers], (void*)&D3D12Hook::resource_barriers);
//        m_set_pipeline_state_hook =  std::make_unique<PointerHook>(&set_pipeline_state_fn, (void*)&D3D12Hook::set_pipeline_state);

//        auto& create_commited_resource_fn = (*(void***)device)[27];
//        m_create_commited_resource_hook = std::make_unique<PointerHook>(&create_commited_resource_fn, (void*)&D3D12Hook::create_committed_resource);
//        auto& create_depth_stencil_view_fn = (*(void***)device)[21];
//        m_create_depth_stencil_view_hook = std::make_unique<PointerHook>(&create_depth_stencil_view_fn, (void*)&D3D12Hook::create_depth_stencil_view);
//        auto& create_graphics_pipeline_state_fn = (*(void***)device)[10];
//        m_create_graphics_pipeline_state_hook = std::make_unique<PointerHook>(&create_graphics_pipeline_state_fn, (void*)&D3D12Hook::create_graphics_pipeline_state);


        m_hooked = true;
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize hooks: {}", e.what());
        m_hooked = false;
    }

    suspender.resume();

    device->Release();
    command_queue->Release();
    factory->Release();
    swap_chain1->Release();
    swap_chain->Release();
    cmd_allocator->Release();
    cmd_list->Release();

    if (hwnd) {
        ::DestroyWindow(hwnd);
    }

    if (wc.lpszClassName != nullptr) {
        ::UnregisterClass(wc.lpszClassName, wc.hInstance);
    }

    return m_hooked;
}

bool D3D12Hook::unhook() {
    if (!m_hooked) {
        return true;
    }

    spdlog::info("Unhooking D3D12");

    m_present_hook.reset();
    m_swapchain_hook.reset();
//    m_commandlist_hook.reset();
    m_last_used_dsv_resource = nullptr;
//    m_last_used_dsv_handle = nullptr;
//    m_dsv_to_resource.clear();
//    m_dsvs.clear();

    m_hooked = false;
    m_is_phase_1 = true;

    return true;
}

thread_local int32_t g_present_depth = 0;



HRESULT WINAPI D3D12Hook::present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags) {
//    spdlog::info("Present called");
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    auto d3d12 = g_d3d12_hook;

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    decltype(D3D12Hook::present)* present_fn{nullptr};

    //if (d3d12->m_is_phase_1) {
        present_fn = d3d12->m_present_hook->get_original<decltype(D3D12Hook::present)*>();
    /*} else {
        present_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::present)*>(8);
    }*/

    if ((flags & DXGI_PRESENT_TEST) == DXGI_PRESENT_TEST)
    {
        // Ignore TEST_PRESENT flag
        return present_fn(swap_chain, sync_interval, flags);
    }

    if (d3d12->m_is_phase_1 && WindowFilter::get().is_filtered(swapchain_wnd)) {
        //present_fn = d3d12->m_present_hook->get_original<decltype(D3D12Hook::present)*>();
        return present_fn(swap_chain, sync_interval, flags);
    }

    if (!d3d12->m_is_phase_1 && swap_chain != d3d12->m_swapchain_hook->get_instance()) {
        const auto og_instance = d3d12->m_swapchain_hook->get_instance();

        // If the original swapchain instance is invalid, then we should not proceed, and rehook the swapchain
        if (IsBadReadPtr(og_instance, sizeof(void*)) || IsBadReadPtr(og_instance.deref(), sizeof(void*))) {
            spdlog::error("Bad read pointer for original swapchain instance, re-hooking");
            d3d12->m_is_phase_1 = true;
        }

        if (!d3d12->m_is_phase_1) {
            return present_fn(swap_chain, sync_interval, flags);
        }
    }

    if (d3d12->m_is_phase_1) {
        //d3d12->m_present_hook.reset();
        d3d12->m_swapchain_hook.reset();

        // vtable hook the swapchain instead of global hooking
        // this seems safer for whatever reason
        // if we globally hook the vtable pointers, it causes all sorts of weird conflicts with other hooks
        // dont hook present though via this hook so other hooks dont get confused
        d3d12->m_swapchain_hook = std::make_unique<VtableHook>(swap_chain);
        //d3d12->m_swapchain_hook->hook_method(8, (uintptr_t)&D3D12Hook::present);
        d3d12->m_swapchain_hook->hook_method(13, (uintptr_t)&D3D12Hook::resize_buffers);
        d3d12->m_swapchain_hook->hook_method(14, (uintptr_t)&D3D12Hook::resize_target);
        d3d12->m_is_phase_1 = false;
    }

    d3d12->m_inside_present = true;
    d3d12->m_swap_chain = swap_chain;

    swap_chain->GetDevice(IID_PPV_ARGS(&d3d12->m_device));

    ID3D12CommandQueue * command_queue = nullptr;

    if (d3d12->m_device != nullptr) {

        auto real_swap_chain = (uintptr_t)swap_chain;
        if (d3d12->m_using_proton_swapchain) {
            real_swap_chain = *(uintptr_t*)((uintptr_t)swap_chain + d3d12->m_proton_swapchain_offset);
        }
        command_queue = *(ID3D12CommandQueue**)(real_swap_chain + d3d12->m_command_queue_offset);
        if (command_queue == nullptr || IsBadReadPtr(command_queue, sizeof(void*)) && d3d12->m_using_proton_swapchain) {
            //double wrap issue after alt tab for starfield, don't want to recurse, hope one level is suffitient
            real_swap_chain = *(uintptr_t*)((uintptr_t)real_swap_chain + d3d12->m_proton_swapchain_offset);
            command_queue = *(ID3D12CommandQueue**)(real_swap_chain + d3d12->m_command_queue_offset);
        }
    }

    if (command_queue != nullptr && !IsBadReadPtr(command_queue, sizeof(void*))) {
        d3d12->m_command_queue = command_queue;
    }

    if (d3d12->m_swapchain_0 == nullptr) {
        d3d12->m_swapchain_0 = swap_chain;
    } else if (d3d12->m_swapchain_1 == nullptr && swap_chain != d3d12->m_swapchain_0) {
        d3d12->m_swapchain_1 = swap_chain;
    }
    
    // Restore the original bytes
    // if an infinite loop occurs, this will prevent the game from crashing
    // while keeping our hook intact
    if (g_present_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{present_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{present_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(present_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Present fixed");
        }

        if ((uintptr_t)present_fn != (uintptr_t)D3D12Hook::present && g_present_depth == 1) {
            spdlog::info("Attempting to call real present function");

            ++g_present_depth;
            const auto result = present_fn(swap_chain, sync_interval, flags);
            --g_present_depth;

            if (result != S_OK) {
                spdlog::error("Present failed: {:x}", result);
            }

            return result;
        }

        spdlog::info("Just returning S_OK");
        return S_OK;
    }

    if (d3d12->m_on_present) {
        d3d12->m_on_present(*d3d12);

        if (d3d12->m_next_present_interval) {
            sync_interval = *d3d12->m_next_present_interval;
            d3d12->m_next_present_interval = std::nullopt;

            if (sync_interval == 0) {
                BOOL is_fullscreen = 0;
                swap_chain->GetFullscreenState(&is_fullscreen, nullptr);
                flags &= ~DXGI_PRESENT_DO_NOT_SEQUENCE;

                DXGI_SWAP_CHAIN_DESC swap_desc{};
                swap_chain->GetDesc(&swap_desc);

                if (!is_fullscreen && (swap_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0) {
                    flags |= DXGI_PRESENT_ALLOW_TEARING;
                }
            }
        }
    }



    auto swap_chain_desc = DXGI_SWAP_CHAIN_DESC1{};
    swap_chain->GetDesc1(&swap_chain_desc);
    d3d12->m_render_height = swap_chain_desc.Height;
    d3d12->m_render_width = swap_chain_desc.Width;
//    spdlog::info("D3D12 present called {} {}", d3d12->m_render_width, d3d12->m_render_height);
    ++g_present_depth;

    auto result = S_OK;
    
    if (!d3d12->m_ignore_next_present) {
        result = present_fn(swap_chain, sync_interval, flags);

        if (result != S_OK) {
            spdlog::error("Present failed: {:x}", result);
        }
    } else {
        d3d12->m_ignore_next_present = false;
    }

    --g_present_depth;

    if (d3d12->m_on_post_present) {
        d3d12->m_on_post_present(*d3d12);
    }

    d3d12->m_inside_present = false;
    d3d12->m_last_used_dsv_resource = nullptr;
    return result;
}

thread_local int32_t g_resize_buffers_depth = 0;

HRESULT WINAPI D3D12Hook::resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize buffers called");
    spdlog::info(" Parameters: buffer_count {} width {} height {} new_format {} swap_chain_flags {}", buffer_count, width, height, new_format, swap_chain_flags);

    auto d3d12 = g_d3d12_hook;
    //auto& hook = d3d12->m_resize_buffers_hook;
    //auto resize_buffers_fn = hook->get_original<decltype(D3D12Hook::resize_buffers)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    auto resize_buffers_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_buffers)*>(13);

    if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    }

    d3d12->m_display_width = width;
    d3d12->m_display_height = height;
    d3d12->m_last_used_dsv_resource = nullptr;
//    d3d12->m_last_used_dsv_handle = nullptr;
//    d3d12->m_dsv_to_resource.clear();
//    d3d12->m_dsvs.clear();

    if (g_resize_buffers_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_buffers_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_buffers_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_buffers_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize buffers fixed");
        }

        if ((uintptr_t)resize_buffers_fn != (uintptr_t)&D3D12Hook::resize_buffers && g_resize_buffers_depth == 1) {
            spdlog::info("Attempting to call the real resize buffers function");

            ++g_resize_buffers_depth;
            const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
            --g_resize_buffers_depth;

            if (result != S_OK) {
                spdlog::error("Resize buffers failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_buffers) {
        d3d12->m_on_resize_buffers(*d3d12);
    }

    ++g_resize_buffers_depth;

    const auto result = resize_buffers_fn(swap_chain, buffer_count, width, height, new_format, swap_chain_flags);
    
    if (result != S_OK) {
        spdlog::error("Resize buffers failed: {:x}", result);
    }

    --g_resize_buffers_depth;

    return result;
}

thread_local int32_t g_resize_target_depth = 0;

HRESULT WINAPI D3D12Hook::resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters) {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    spdlog::info("D3D12 resize target called");
    spdlog::info(" Parameters: new_target_parameters {:x}", (uintptr_t)new_target_parameters);

    auto d3d12 = g_d3d12_hook;
    //auto resize_target_fn = d3d12->m_resize_target_hook->get_original<decltype(D3D12Hook::resize_target)*>();

    HWND swapchain_wnd{nullptr};
    swap_chain->GetHwnd(&swapchain_wnd);

    auto resize_target_fn = d3d12->m_swapchain_hook->get_method<decltype(D3D12Hook::resize_target)*>(14);

    if (WindowFilter::get().is_filtered(swapchain_wnd)) {
        return resize_target_fn(swap_chain, new_target_parameters);
    }

    d3d12->m_render_width = new_target_parameters->Width;
    d3d12->m_render_height = new_target_parameters->Height;
    d3d12->m_last_used_dsv_resource = nullptr;
//    d3d12->m_last_used_dsv_handle = nullptr;
//    d3d12->m_dsv_to_resource.clear();
//    d3d12->m_dsvs.clear();

    // Restore the original code to the resize_buffers function.
    if (g_resize_target_depth > 0) {
        auto original_bytes = utility::get_original_bytes(Address{resize_target_fn});

        if (original_bytes) {
            ProtectionOverride protection_override{resize_target_fn, original_bytes->size(), PAGE_EXECUTE_READWRITE};

            memcpy(resize_target_fn, original_bytes->data(), original_bytes->size());

            spdlog::info("Resize target fixed");
        }

        if ((uintptr_t)resize_target_fn != (uintptr_t)&D3D12Hook::resize_target && g_resize_target_depth == 1) {
            spdlog::info("Attempting to call the real resize target function");

            ++g_resize_target_depth;
            const auto result = resize_target_fn(swap_chain, new_target_parameters);
            --g_resize_target_depth;

            if (result != S_OK) {
                spdlog::error("Resize target failed: {:x}", result);
            }

            return result;
        } else {
            spdlog::info("Just returning S_OK");
            return S_OK;
        }
    }

    if (d3d12->m_on_resize_target) {
        d3d12->m_on_resize_target(*d3d12);
    }

    ++g_resize_target_depth;

    const auto result = resize_target_fn(swap_chain, new_target_parameters);
    
    if (result != S_OK) {
        spdlog::error("Resize target failed: {:x}", result);
    }

    --g_resize_target_depth;

    return result;
}

void D3D12Hook::set_render_targets(ID3D12GraphicsCommandList5* cmd_list, UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors,
                                   BOOL RTsSingleHandleToDescriptorRange, D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_descriptor)
{
    auto d3d12 = g_d3d12_hook;
    auto set_render_targets_fn = g_d3d12_hook->m_set_render_targets_hook->get_original<decltype(D3D12Hook::set_render_targets)*>();
    if (d3d12->m_on_set_render_targets) {
        d3d12->m_on_set_render_targets(*d3d12, cmd_list, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, depth_stencil_descriptor);
    }
    set_render_targets_fn(cmd_list, NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, depth_stencil_descriptor);
}

bool matches_texture_size(int width, int height, int render_width, int render_height) {
    return abs(width - render_width) <= 5 || abs(height - render_height) <= 5;
}

void D3D12Hook::create_depth_stencil_view(ID3D12Device* device, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    auto create_depth_stencil_view_fn = g_d3d12_hook->m_create_depth_stencil_view_hook->get_original<decltype(D3D12Hook::create_depth_stencil_view)*>();
    create_depth_stencil_view_fn(device, pResource, pDesc, DestDescriptor);
//    auto d3d12 = g_d3d12_hook;
//    auto desc = pResource->GetDesc();
//    if((pDesc->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) && matches_texture_size(desc.Width, desc.Height,  d3d12->m_render_width, d3d12->m_render_height)) {
//        d3d12->m_dsvs[DestDescriptor.ptr] = desc;
//        d3d12->m_dsv_to_resource[DestDescriptor.ptr] = pResource;
////        spdlog::info("D3D12 create depth stencil view called format[{}] rFormat[{}]  dimension[{}] flags[{}] width[{}] height[{}] renderer W[{}] H[{}] dsv_handle[{:x}] resrouce[{}]" , pDesc->Format, desc.Format,  pDesc->ViewDimension, pDesc->Flags, desc.Width, desc.Height, d3d12->m_render_width, d3d12->m_render_height, DestDescriptor.ptr, fmt::ptr(pResource));
//    }
}

HRESULT D3D12Hook::create_committed_resource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC* pDesc,
                                             D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE* pOptimizedClearValue, const IID& riid, void** ppvResource)
{
    auto create_committed_resource_fn = g_d3d12_hook->m_create_commited_resource_hook->get_original<decltype(D3D12Hook::create_committed_resource)*>();
    if(pDesc->Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
    }

    auto result =  create_committed_resource_fn(device, pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riid, ppvResource);
//    spdlog::info("D3D12 create commited Resource called format[{}] flags[{}] width[{}] height[{}] renderer W[{}] H[{}] ptr[{}]" , pDesc->Format, pDesc->Flags, pDesc->Width, pDesc->Height, g_d3d12_hook->m_render_width, g_d3d12_hook->m_render_height, fmt::ptr(*ppvResource));

    return result;
}

void D3D12Hook::set_scissor_rects(ID3D12GraphicsCommandList5* cmd_list, UINT NumRects, const D3D12_RECT* pRects) {
    auto d3d12 = g_d3d12_hook;

    auto on_set_scissor_rects_original_fn = d3d12->m_on_set_scissor_rects_hook->get_original<decltype(D3D12Hook::set_scissor_rects)*>();
    if(d3d12->m_on_set_scissor_rects) {
        d3d12->m_on_set_scissor_rects(*d3d12, cmd_list, NumRects, pRects);
    }
    on_set_scissor_rects_original_fn(cmd_list, NumRects, pRects);
}

void D3D12Hook::set_viewports(ID3D12GraphicsCommandList5* cmd_list, UINT NumViewports, const D3D12_VIEWPORT* pViewports) {
    auto d3d12 = g_d3d12_hook;
    auto set_viewports_original_fn = d3d12->m_set_viewports_hook->get_original<decltype(D3D12Hook::set_viewports)*>();
    
    if(d3d12->m_on_set_viewports) {
        d3d12->m_on_set_viewports(*d3d12, cmd_list, NumViewports, pViewports);
    }
    
    set_viewports_original_fn(cmd_list, NumViewports, pViewports);
}

void D3D12Hook::create_render_target_view(ID3D12Device* device, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
    auto d3d12 = g_d3d12_hook;
    auto create_render_target_view_fn = d3d12->m_create_render_target_view_hook->get_original<decltype(D3D12Hook::create_render_target_view)*>();
    create_render_target_view_fn(device, pResource, pDesc, DestDescriptor);
    if (d3d12->m_on_create_render_target_view) {
        d3d12->m_on_create_render_target_view(*d3d12, device, pResource, pDesc, DestDescriptor);
    }
}

//HRESULT D3D12Hook::create_graphics_pipeline_state(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC * pDesc, const IID& riid, void** ppPipelineState)
//{
//    auto create_pipeline_state_fn = g_d3d12_hook->m_create_graphics_pipeline_state_hook->get_original<decltype(D3D12Hook::create_graphics_pipeline_state)*>();
//    auto result = create_pipeline_state_fn(device, pDesc, riid, ppPipelineState);
////    pso_to_desc[uintptr_t(*ppPipelineState)] = *pDesc;
////    log_pso_fingerprint(pDesc);
//    return result;
//}
//
//bool is_pso_a_ui_pass(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)
//{
//    // A UI pass must write to exactly one render target.
//    if (desc->NumRenderTargets != 1) return false;
//
//    // Rule 1: Must use standard alpha blending.
//    if (!desc->BlendState.RenderTarget[0].BlendEnable) return false;
//    const auto& rt_blend = desc->BlendState.RenderTarget[0];
//    if (rt_blend.SrcBlend != D3D12_BLEND_SRC_ALPHA ||
//        rt_blend.DestBlend != D3D12_BLEND_INV_SRC_ALPHA ||
//        rt_blend.BlendOp != D3D12_BLEND_OP_ADD) {
//        return false;
//    }
//
//    // Rule 2: Must not write to the depth buffer.
//    if (desc->DepthStencilState.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO) return false;
//    // This is almost certainly a UI PSO.
//    return true;
//}
//
//bool is_pso_eligible_for_vrs(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc)
//{
//    // --- Universal Exclusions ---
//    if (desc->RasterizerState.FillMode != D3D12_FILL_MODE_SOLID) return false;
//    if (desc->PS.pShaderBytecode == nullptr || desc->PS.BytecodeLength == 0) return false;
//    if (desc->RasterizerState.DepthBias != 0 || desc->RasterizerState.SlopeScaledDepthBias != 0.0f) return false; // Exclude shadows
//
//    // --- The NEW UI Exclusion ---
//    // The most important new rule, based on your findings.
//    if (is_pso_a_ui_pass(desc)) {
//        return false;
//    }
//
//    // If it's not a shadow pass and not a UI pass, it's a candidate.
//    // This now correctly includes both the MRT G-Buffer pass AND the heavy single-RT lighting pass.
//    return true;
//}

//void D3D12Hook::set_pipeline_state(ID3D12GraphicsCommandList* cmd_list, ID3D12PipelineState* pPipelineState) {
//    auto d3d12 = g_d3d12_hook;
//    auto set_pipeline_state_original_fn = d3d12->m_set_pipeline_state_hook->get_original<decltype(D3D12Hook::set_pipeline_state)*>();
//
//    ID3D12GraphicsCommandList5* cmd_list5 = nullptr;
//    cmd_list->QueryInterface(IID_PPV_ARGS(&cmd_list5));
//    static const D3D12_SHADING_RATE_COMBINER combiners[D3D12_RS_SET_SHADING_RATE_COMBINER_COUNT] = {
//        D3D12_SHADING_RATE_COMBINER_MAX, D3D12_SHADING_RATE_COMBINER_MAX};
//    ID3D12Resource* pVRSResource = current_shading_image;
//    bool test_pso = pVRSResource && ModSettings::g_internalSettings.useVRS && cmd_list5 && five_rtv;
//    bool clean_vrs = false;
//    if(test_pso) {
//        if(get_pso_blob_size(pPipelineState) > ModSettings::g_internalSettings.psoSizeThreshold) {
//            cmd_list5->RSSetShadingRateImage(nullptr);
//        } else {
//            cmd_list5->RSSetShadingRateImage(pVRSResource);
//        }
//    }
////    spdlog::info("D3D12 set pipeline state called pass[{}] apply_vrs[{}]", pass_number, apply_vrs);
//    if(cmd_list5) {
//        cmd_list5->Release();
//    }
//    set_pipeline_state_original_fn(cmd_list, pPipelineState);
//}

//
//void D3D12Hook::dispatch(ID3D12GraphicsCommandList* cmd_list, UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) {
//    auto d3d12 = g_d3d12_hook;
//    auto dispatch_original_fn = d3d12->m_commandlist_dispatch_hook->get_original<decltype(D3D12Hook::dispatch)*>();
//    dispatch_original_fn(cmd_list, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
//}
//
//void D3D12Hook::execute_indirect(ID3D12GraphicsCommandList* cmd_list, ID3D12CommandSignature* pCommandSignature, UINT MaxCommandCount, ID3D12Resource* pArgumentBuffer,
//                                 UINT64 ArgumentBufferOffset, ID3D12Resource* pCountBuffer, UINT64 CountBufferOffset)
//{
//    auto d3d12 = g_d3d12_hook;
//    auto execute_indirect_original_fn = d3d12->m_commandlist_execute_indirect_hook->get_original<decltype(D3D12Hook::execute_indirect)*>();
//    execute_indirect_original_fn(cmd_list, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
//}
//
//void D3D12Hook::resource_barriers(ID3D12GraphicsCommandList* cmd_list, UINT NumBarriers, const D3D12_RESOURCE_BARRIER* pBarriers) {
//    auto d3d12 = g_d3d12_hook;
//    auto resource_barriers_original_fn = d3d12->m_commandlist_resource_barriers_hook->get_original<decltype(D3D12Hook::resource_barriers)*>();
//    resource_barriers_original_fn(cmd_list, NumBarriers, pBarriers);
//}

/*HRESULT WINAPI D3D12Hook::create_swap_chain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain)
{
    spdlog::info("D3D12 create swapchain called");

    auto d3d12 = g_d3d12_hook;

    d3d12->m_command_queue = (ID3D12CommandQueue*)device;
    
    if (d3d12->m_on_create_swap_chain) {
        d3d12->m_on_create_swap_chain(*d3d12);
    }

    auto create_swap_chain_fn = d3d12->m_create_swap_chain_hook->get_original<decltype(D3D12Hook::create_swap_chain)>();

    return create_swap_chain_fn(factory, device, hwnd, desc, p_fullscreen_desc, p_restrict_to_output, swap_chain);
}*/

