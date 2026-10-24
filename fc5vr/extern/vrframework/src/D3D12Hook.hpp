#pragma once

#include <iostream>
#include <functional>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi")

#include <d3d12.h>
#include <dxgi1_4.h>

#include "utility/PointerHook.hpp"
#include "utility/VtableHook.hpp"
#include <wrl.h>

using Microsoft::WRL::ComPtr;

class D3D12Hook
{
public:
    typedef std::function<void(D3D12Hook&)>                                                OnPresentFn;
    typedef std::function<void(D3D12Hook&)>                                                OnResizeBuffersFn;
    typedef std::function<void(D3D12Hook&)>                                                OnResizeTargetFn;
    typedef std::function<void(D3D12Hook&, ID3D12GraphicsCommandList5*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, D3D12_CPU_DESCRIPTOR_HANDLE* )> OnSetRenderTargetsFn;
    typedef std::function<void(D3D12Hook&, ID3D12GraphicsCommandList5*, UINT, const D3D12_RECT* pRects)> OnSetScissorRectsFn;
    typedef std::function<void(D3D12Hook&, ID3D12GraphicsCommandList5*, UINT, const D3D12_VIEWPORT*)> OnSetViewportsFn;
    typedef std::function<void(D3D12Hook&, ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE)> OnCreateRenderTargetViewFn;
    typedef std::function<void(D3D12Hook&)>                                                OnCreateSwapChainFn;

    D3D12Hook() = default;
    virtual ~D3D12Hook();

    bool hook();
    bool unhook();

    inline bool is_hooked() { return m_hooked; }

    inline void on_present(OnPresentFn fn) { m_on_present = fn; }

    inline void on_post_present(OnPresentFn fn) { m_on_post_present = fn; }

    inline void on_resize_buffers(OnResizeBuffersFn fn) { m_on_resize_buffers = fn; }

    inline void on_resize_target(OnResizeTargetFn fn) { m_on_resize_target = fn; }

    inline void on_set_render_targets(OnSetRenderTargetsFn fn) {
        m_on_set_render_targets = fn;
    }

    inline void on_set_scissor_rects(OnSetScissorRectsFn fn) {
        m_on_set_scissor_rects = fn;
    }

    inline void on_set_viewports(OnSetViewportsFn fn) {
        m_on_set_viewports = fn;
    }

    inline void on_create_render_target_view(OnCreateRenderTargetViewFn fn) {
        m_on_create_render_target_view = fn;
    }

    /*void on_create_swap_chain(OnCreateSwapChainFn fn) {
        m_on_create_swap_chain = fn;
    }*/

    inline ID3D12Device4* get_device() const { return m_device; }

    inline IDXGISwapChain3* get_swap_chain() const { return m_swap_chain; }

    inline auto get_swapchain_0() { return m_swapchain_0; }

    inline auto get_swapchain_1() { return m_swapchain_1; }

    inline ID3D12CommandQueue* get_command_queue() const { return m_command_queue; }

    inline UINT get_display_width() const { return m_display_width; }

    inline UINT get_display_height() const { return m_display_height; }

    inline UINT get_render_width() const { return m_render_width; }

    inline UINT get_render_height() const { return m_render_height; }

    inline bool is_inside_present() const { return m_inside_present; }

    inline bool is_proton_swapchain() const { return m_using_proton_swapchain; }

    inline void ignore_next_present() { m_ignore_next_present = true; }

    inline void set_next_present_interval(uint32_t interval) { m_next_present_interval = interval; }

    ID3D12Resource* m_last_used_dsv_resource{ nullptr };
    //    D3D12_CPU_DESCRIPTOR_HANDLE* m_last_used_dsv_handle{ nullptr};

protected:
    ID3D12Device4*      m_device{ nullptr };
    IDXGISwapChain3*    m_swap_chain{ nullptr };
    IDXGISwapChain3*    m_swapchain_0{};
    IDXGISwapChain3*    m_swapchain_1{};
    ID3D12CommandQueue* m_command_queue{ nullptr };
    UINT                m_display_width{ NULL };
    UINT                m_display_height{ NULL };
    UINT                m_render_width{ NULL };
    UINT                m_render_height{ NULL };

    uint32_t m_command_queue_offset{};
    uint32_t m_proton_swapchain_offset{};

    std::optional<uint32_t> m_next_present_interval{};

    bool m_using_proton_swapchain{ false };
    bool m_hooked{ false };
    bool m_is_phase_1{ true };
    bool m_inside_present{ false };
    bool m_ignore_next_present{ false };

    std::unique_ptr<PointerHook> m_present_hook{};
    std::unique_ptr<VtableHook>  m_swapchain_hook{};
    std::unique_ptr<PointerHook> m_set_render_targets_hook{};
    std::unique_ptr<PointerHook> m_on_set_scissor_rects_hook{};
    std::unique_ptr<PointerHook> m_set_viewports_hook{};
    std::unique_ptr<PointerHook> m_create_depth_stencil_view_hook{};
    std::unique_ptr<PointerHook> m_create_render_target_view_hook{};
    std::unique_ptr<PointerHook> m_create_commited_resource_hook{};
    // std::unique_ptr<FunctionHook> m_create_swap_chain_hook{};

    std::unordered_map<SIZE_T, D3D12_RESOURCE_DESC> m_dsvs{};
    std::unordered_map<SIZE_T, ID3D12Resource*>     m_dsv_to_resource{};

    OnPresentFn          m_on_present{ nullptr };
    OnPresentFn          m_on_post_present{ nullptr };
    OnResizeBuffersFn    m_on_resize_buffers{ nullptr };
    OnResizeTargetFn     m_on_resize_target{ nullptr };
    OnSetRenderTargetsFn m_on_set_render_targets{ nullptr };
    OnSetScissorRectsFn m_on_set_scissor_rects{ nullptr };
    OnSetViewportsFn m_on_set_viewports{ nullptr };
    OnCreateRenderTargetViewFn m_on_create_render_target_view{ nullptr };
    // OnCreateSwapChainFn m_on_create_swap_chain{ nullptr };

    static HRESULT WINAPI            present(IDXGISwapChain3* swap_chain, UINT sync_interval, UINT flags);
    static HRESULT WINAPI            resize_buffers(IDXGISwapChain3* swap_chain, UINT buffer_count, UINT width, UINT height, DXGI_FORMAT new_format, UINT swap_chain_flags);
    static HRESULT WINAPI            resize_target(IDXGISwapChain3* swap_chain, const DXGI_MODE_DESC* new_target_parameters);
    static void STDMETHODCALLTYPE    set_render_targets(ID3D12GraphicsCommandList5* cmd_list, UINT NumRenderTargetDescriptors,
                                                        const D3D12_CPU_DESCRIPTOR_HANDLE* pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_descriptor);
    static void STDMETHODCALLTYPE    set_scissor_rects(ID3D12GraphicsCommandList5* cmd_list, _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT* pRects);
    static void STDMETHODCALLTYPE    set_viewports(ID3D12GraphicsCommandList5* cmd_list, _In_ UINT NumViewports, _In_reads_(NumViewports) const D3D12_VIEWPORT* pViewports);
//    static void STDMETHODCALLTYPE    dispatch(ID3D12GraphicsCommandList* cmd_list, _In_ UINT ThreadGroupCountX, _In_ UINT ThreadGroupCountY, UINT ThreadGroupCountZ);
//    static void STDMETHODCALLTYPE    execute_indirect(ID3D12GraphicsCommandList* cmd_list, _In_ ID3D12CommandSignature* pCommandSignature, _In_ UINT MaxCommandCount,
//                                                      _In_ ID3D12Resource* pArgumentBuffer, _In_ UINT64 ArgumentBufferOffset, _In_opt_ ID3D12Resource* pCountBuffer,
//                                                      _In_ UINT64 CountBufferOffset);
//    static void STDMETHODCALLTYPE    resource_barriers(ID3D12GraphicsCommandList* cmd_list, _In_ UINT NumBarriers, _In_reads_(NumBarriers) const D3D12_RESOURCE_BARRIER* pBarriers);
    static void STDMETHODCALLTYPE    create_depth_stencil_view(ID3D12Device* device, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc,
                                                               D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
    static void STDMETHODCALLTYPE    create_render_target_view(ID3D12Device* device, ID3D12Resource* pResource, const D3D12_RENDER_TARGET_VIEW_DESC* pDesc,
                                                                D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor);
//    static void STDMETHODCALLTYPE    set_pipeline_state(ID3D12GraphicsCommandList* cmd_list, ID3D12PipelineState* pPipelineState);
//    static HRESULT STDMETHODCALLTYPE create_graphics_pipeline_state(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC * pDesc, const IID& riid, void** ppPipelineState);
    static HRESULT STDMETHODCALLTYPE create_committed_resource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* pHeapProperties, D3D12_HEAP_FLAGS HeapFlags,
                                                               const D3D12_RESOURCE_DESC* pDesc, D3D12_RESOURCE_STATES InitialResourceState,
                                                               const D3D12_CLEAR_VALUE* pOptimizedClearValue, REFIID riid, void** ppvResource);
    // static HRESULT WINAPI create_swap_chain(IDXGIFactory4* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*
    // p_fullscreen_desc, IDXGIOutput* p_restrict_to_output, IDXGISwapChain** swap_chain);
};
