#pragma once

#include "Mod.hpp"

class VRConfig : public Mod {
public:
    static std::shared_ptr<VRConfig>& get();

public:
    std::string_view get_name() const {
        return "VRConfig";
    }

    std::optional<std::string> on_initialize() override;
    void on_draw_ui() override;
    void on_frame() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;

    auto& get_menu_key() {
        return m_menu_key;
    }

    auto& get_menu_open() {
        return m_menu_open;
    }

    bool is_always_show_cursor() const {
        return m_always_show_cursor->value();
    }

private:
    ModKey::Ptr m_menu_key{ ModKey::create(generate_name("MenuKey_V2"), VK_F11) };
    ModToggle::Ptr m_menu_open{ ModToggle::create(generate_name("MenuOpen"), false) };
    ModToggle::Ptr m_remember_menu_state{ ModToggle::create(generate_name("RememberMenuState"), false) };
    ModToggle::Ptr m_always_show_cursor{ ModToggle::create(generate_name("DrawCursorWithMenuOpen"), false) };
    ModKey::Ptr m_show_cursor_key{ ModKey::create(generate_name("ShowCursorKey")) };
    ModInt32::Ptr m_font_size{ModInt32::create(generate_name("FontSize"), 16)};


    ValueList m_options {
        *m_menu_key,
        *m_menu_open,
        *m_remember_menu_state,
//        *m_always_show_cursor,
//        *m_show_cursor_key,
//        *m_font_size
    };
};
