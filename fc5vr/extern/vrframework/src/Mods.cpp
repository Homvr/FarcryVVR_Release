#include "Mods.hpp"

std::optional<std::string> Mods::on_initialize() const {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_initialize()", mod->get_name().data());

        if (auto e = mod->on_initialize(); e != std::nullopt) {
            spdlog::info("{:s}::on_initialize() has failed: {:s}", mod->get_name().data(), *e);
            return e;
        }
    }

    return std::nullopt;
}

std::optional<std::string> Mods::on_initialize_d3d_thread() const {
    std::scoped_lock _{g_framework->get_hook_monitor_mutex()};

    // once here to at least setup the values
    reload_config();

    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_initialize_d3d_thread()", mod->get_name().data());

        if (auto e = mod->on_initialize_d3d_thread(); e != std::nullopt) {
            spdlog::info("{:s}::on_initialize_d3d_thread() has failed: {:s}", mod->get_name().data(), *e);
            return e;
        }
    }

    reload_config();

    return std::nullopt;
}

void Mods::reload_config(bool set_defaults) const {
    utility::Config cfg{ (Framework::get_persistent_dir() / "vr_config.txt").string() };

    for (auto& mod : m_mods) {
        spdlog::info("{:s}::on_config_load()", mod->get_name().data());
        mod->on_config_load(cfg, set_defaults);
    }
}

void Mods::on_pre_imgui_frame() const {
    for (auto& mod : m_mods) {
        mod->on_pre_imgui_frame();
    }
}

void Mods::on_frame() const {
    for (auto& mod : m_mods) {
        mod->on_frame();
    }
}

void Mods::on_present() const {
    size_t i = 0;
    for (auto& mod : m_mods) {
        const auto t0 = std::chrono::steady_clock::now();
        mod->on_present();
        if (i < 8) s_fc5_present_us[i] += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        ++i;
    }
}

void Mods::on_post_frame() const {
    for (auto& mod : m_mods) {
        mod->on_post_frame();
    }
}

void Mods::on_draw_ui() const {
    for (auto& mod : m_mods) {
        mod->on_draw_ui();
    }
}

void Mods::on_device_reset() const {
    for (auto& mod : m_mods) {
        mod->on_device_reset();
    }
}

void Mods::on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc) const {
    for (auto& mod : m_mods) {
        mod->on_d3d12_initialize(pDevice4, desc);
    }
}

void Mods::on_d3d11_initialize(ID3D11Device* pDevice, D3D11_TEXTURE2D_DESC& desc) const {
    for (auto& mod : m_mods) {
        mod->on_d3d11_initialize(pDevice, desc);
    }
}

void Mods::on_d3d12_set_render_targets(ID3D12GraphicsCommandList5* cmd_list, UINT num_rtvs, 
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_handle, D3D12_CPU_DESCRIPTOR_HANDLE* dsv) const {
    for (auto& mod : m_mods) {
        mod->on_d3d12_set_render_targets(cmd_list, num_rtvs, rtvs, single_handle, dsv);
    }
}

void Mods::on_d3d12_set_scissor_rects(ID3D12GraphicsCommandList5* cmd_list, UINT num_rects, const D3D12_RECT* rects) const {
    for (auto& mod : m_mods) {
        mod->on_d3d12_set_scissor_rects(cmd_list, num_rects, rects);
    }
}

void Mods::on_d3d12_set_viewports(ID3D12GraphicsCommandList5* cmd_list, UINT num_viewports, const D3D12_VIEWPORT* viewports) const {
    for (auto& mod : m_mods) {
        mod->on_d3d12_set_viewports(cmd_list, num_viewports, viewports);
    }
}

void Mods::on_d3d12_create_render_target_view(ID3D12Device* device, ID3D12Resource* pResource, 
    const D3D12_RENDER_TARGET_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor) const {
    for (auto& mod : m_mods) {
        mod->on_d3d12_create_render_target_view(device, pResource, pDesc, DestDescriptor);
    }
}
