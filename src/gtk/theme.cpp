#include <braurser/gtk/kit.hpp>
#include <braurser/gtk/internal.hpp>
#include <algorithm>
#include <mutex>
#include <stdexcept>

extern "C" void braurser_kit_register_resource();

namespace braurser::gtk::kit {
namespace {
unsigned scheme(GVariant* value) {
    if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) { return 0; }
    auto* inner = g_variant_get_variant(value);
    const auto result = g_variant_is_of_type(inner, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(inner) : 0;
    g_variant_unref(inner);
    return result <= 2 ? result : 0;
}
}

void ensure_resources() {
    static std::once_flag once;
    std::call_once(once, [] { braurser_kit_register_resource(); });
}

ThemeManager::ThemeManager() : provider_(Gtk::CssProvider::create()), preferences_(Gtk::CssProvider::create()),
    display_(Gdk::Display::get_default()), settings_(Gtk::Settings::get_default()) {
    ensure_resources();
    if (!display_ || !settings_) { throw std::runtime_error("theme requires a GTK display"); }
    fallback_dark_ = settings_->property_gtk_application_prefer_dark_theme().get_value();
    settings_connection_ = settings_->property_gtk_application_prefer_dark_theme().signal_changed().connect([this] {
        if (!applying_) {
            fallback_dark_ = settings_->property_gtk_application_prefer_dark_theme().get_value();
            apply();
        }
    });
    contrast_connection_ = settings_->property_gtk_theme_name().signal_changed().connect(sigc::mem_fun(*this, &ThemeManager::apply));
    font_connection_ = settings_->property_gtk_font_name().signal_changed().connect(sigc::mem_fun(*this, &ThemeManager::apply));
    motion_connection_ = settings_->property_gtk_enable_animations().signal_changed().connect(sigc::mem_fun(*this, &ThemeManager::apply));
    try {
        portal_ = Gio::DBus::Proxy::create_for_bus_sync(Gio::DBus::BusType::SESSION,
            "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", {}, Gio::DBus::ProxyFlags::DO_NOT_AUTO_START |
            Gio::DBus::ProxyFlags::DO_NOT_LOAD_PROPERTIES);
        portal_connection_ = portal_->signal_signal().connect([this](const Glib::ustring&, const Glib::ustring& name,
                                                                    Glib::VariantContainerBase args) {
            if (name != "SettingChanged" || !g_variant_is_of_type(args.gobj(), G_VARIANT_TYPE("(ssv)"))) { return; }
            const gchar* ns = nullptr;
            const gchar* key = nullptr;
            GVariant* value = nullptr;
            g_variant_get(args.gobj(), "(&s&s@v)", &ns, &key, &value);
            if (std::string_view(ns) == "org.freedesktop.appearance" && std::string_view(key) == "color-scheme") {
                portal_scheme_ = scheme(value);
                apply();
            }
            g_variant_unref(value);
        });
        owner_connection_ = portal_->property_g_name_owner().signal_changed().connect([this] {
            read_portal();
            apply();
        });
        read_portal();
    } catch (const Glib::Error&) {
        portal_scheme_ = 0;
    }
    apply();
}

ThemeManager::~ThemeManager() {
    portal_connection_.disconnect();
    owner_connection_.disconnect();
    settings_connection_.disconnect();
    contrast_connection_.disconnect();
    font_connection_.disconnect();
    motion_connection_.disconnect();
    if (attached_) {
        Gtk::StyleContext::remove_provider_for_display(display_, provider_);
        Gtk::StyleContext::remove_provider_for_display(display_, preferences_);
    }
}

void ThemeManager::read_portal() {
    portal_scheme_ = 0;
    if (!portal_) { return; }
    try {
        const auto parameters = Glib::Variant<std::tuple<Glib::ustring, Glib::ustring>>::create(
            {"org.freedesktop.appearance", "color-scheme"});
        auto reply = portal_->call_sync("ReadOne", parameters, 1000);
        if (g_variant_is_of_type(reply.gobj(), G_VARIANT_TYPE("(v)"))) {
            auto* value = g_variant_get_child_value(reply.gobj(), 0);
            portal_scheme_ = scheme(value);
            g_variant_unref(value);
        }
    } catch (const Glib::Error&) {
        portal_scheme_ = 0;
    }
}

void ThemeManager::manage(Gtk::Window& window) {
    window.add_css_class("braurser");
}

void ThemeManager::set_theme(Theme theme) {
    switch (theme) {
    case Theme::System:
    case Theme::Light:
    case Theme::Dark:
        choice_ = theme;
        apply();
        return;
    }
    throw std::invalid_argument("unknown theme");
}

Theme ThemeManager::resolved() const { return resolved_; }

void ThemeManager::apply() {
    if (applying_) { return; }
    resolved_ = choice_ == Theme::System ?
        (portal_scheme_ == 1 || (portal_scheme_ == 0 && fallback_dark_) ? Theme::Dark : Theme::Light) : choice_;
    const auto theme_name = settings_->property_gtk_theme_name().get_value().lowercase();
    const bool high_contrast = theme_name.find("highcontrast") != Glib::ustring::npos ||
                               theme_name.find("high-contrast") != Glib::ustring::npos;
    if (high_contrast) {
        if (attached_) {
            Gtk::StyleContext::remove_provider_for_display(display_, provider_);
            Gtk::StyleContext::remove_provider_for_display(display_, preferences_);
            attached_ = false;
        }
        detail::refresh_icons();
        return;
    }
    provider_->load_from_resource(resolved_ == Theme::Dark ? "/org/braurser/kit/braurser-dark.css" :
                                                          "/org/braurser/kit/braurser-light.css");
    const Pango::FontDescription font(settings_->property_gtk_font_name().get_value());
    auto family = safe_text(font.get_family().raw());
    std::string quoted_family;
    for (const char character : family) {
        if (character == '\\' || character == '"') { quoted_family += '\\'; }
        if (static_cast<unsigned char>(character) >= 32) { quoted_family += character; }
    }
    const auto size = std::clamp(static_cast<double>(font.get_size()) / Pango::SCALE, 1.0, 512.0);
    std::string css = ".braurser {font-family: \"" + quoted_family + "\", sans-serif; font-size: " +
        std::to_string(size) + (font.get_size_is_absolute() ? "px;}" : "pt;}");
    css += ".braurser button.navigation-row {border: none; box-shadow: none; background-image: none;"
           "background-color: transparent; border-radius: 6px; min-height: 44px; padding: 0 8px;}"
           ".braurser button.navigation-row:hover {background-color: @br_hover;}"
           ".braurser button.navigation-row:checked {background-color: @br_selected; color: @br_on_selected;}"
           ".braurser .navigation-count {border-radius: 6px; padding: 0 4px;}";
    css += ".braurser .display {font-size: 1.866667em;} .braurser .page-title {font-size: 1.6em;}"
           ".braurser .section-title {font-size: 1.2em; line-height: 1.444444; font-weight: 600;}"
           ".braurser .row-title {font-weight: 600;}"
           ".braurser .secondary, .braurser .code {font-size: 0.866667em;}";
    if (!settings_->property_gtk_enable_animations().get_value()) {
        css += ".braurser, .braurser * {transition: none; animation: none;}";
    }
    preferences_->load_from_string(css);
    if (!attached_) {
        Gtk::StyleContext::add_provider_for_display(display_, provider_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        Gtk::StyleContext::add_provider_for_display(display_, preferences_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        attached_ = true;
    }
    applying_ = true;
    settings_->property_gtk_application_prefer_dark_theme() = resolved_ == Theme::Dark;
    applying_ = false;
    detail::refresh_icons();
}

}
