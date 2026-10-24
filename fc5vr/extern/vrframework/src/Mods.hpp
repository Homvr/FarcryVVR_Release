#pragma once

#include <atomic>
#include <chrono>

#include "Mod.hpp"

class Mods {
public:
    Mods();
    virtual ~Mods() {}

    std::optional<std::string> on_initialize() const;
    std::optional<std::string> on_initialize_d3d_thread() const;
    void reload_config(bool set_defaults = false) const;

    void on_pre_imgui_frame() const;
    void on_frame() const;
    void on_present() const;
    void on_post_frame() const;
    void on_draw_ui() const;
    void on_device_reset() const;
    void on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc) const;
    void on_d3d11_initialize(ID3D11Device* pDevice, D3D11_TEXTURE2D_DESC& desc) const;
    
    void on_d3d12_set_render_targets(ID3D12GraphicsCommandList5* cmd_list, UINT num_rtvs, 
        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle, D3D12_CPU_DESCRIPTOR_HANDLE* dsv) const;
    void on_d3d12_set_scissor_rects(ID3D12GraphicsCommandList5* cmd_list, UINT num_rects, const D3D12_RECT* rects) const;
    void on_d3d12_set_viewports(ID3D12GraphicsCommandList5* cmd_list, UINT num_viewports, const D3D12_VIEWPORT* viewports) const;
    void on_d3d12_create_render_target_view(ID3D12Device* device, ID3D12Resource* pResource, 
        const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) const;

    // Far Cry 5 hitchlog: running totals in microseconds of each mod's on_present and on_post_present, by its place in get_mods().
    inline static std::atomic<int64_t> s_fc5_present_us[8]{}, s_fc5_post_present_us[8]{};

    const auto& get_mods() const {
        return m_mods;
    }

private:
    std::vector<std::shared_ptr<Mod>> m_mods;
};