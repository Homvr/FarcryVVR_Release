#include "VRConfig.hpp"
#include "../Framework.hpp"

std::shared_ptr<VRConfig>& VRConfig::get() {
     static std::shared_ptr<VRConfig> instance{std::make_shared<VRConfig>()};
     return instance;
}

std::optional<std::string> VRConfig::on_initialize() {
    return Mod::on_initialize();
}

void VRConfig::on_draw_ui() {
    if (ImGui::CollapsingHeader("Configuration")) {
        ImGui::TreePush("Configuration");

        m_menu_key->draw("Menu Key");
//        m_show_cursor_key->draw("Show Cursor Key");
        m_remember_menu_state->draw("Remember Menu Open/Closed State");
//        m_always_show_cursor->draw("Draw Cursor With Menu Open");

//        if (m_font_size->draw("Font Size")) {
//            g_framework->set_font_size(m_font_size->value());
//        }

        ImGui::TreePop();
    }
}

void VRConfig::on_frame() {
    if (m_show_cursor_key->is_key_down_once()) {
        m_always_show_cursor->toggle();
    }
}

void VRConfig::on_config_load(const utility::Config& cfg, bool set_defaults) {
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (m_remember_menu_state->value()) {
        g_framework->set_draw_ui(m_menu_open->value(), false);
    }
    
    g_framework->set_font_size(m_font_size->value());
}

void VRConfig::on_config_save(utility::Config& cfg) {
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}
