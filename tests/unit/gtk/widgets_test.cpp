#include <braurser/gtk/kit.hpp>
#include "display.hpp"
#include <gtest/gtest.h>
#include <array>
#include <filesystem>
#include <unistd.h>

namespace braurser::gtk::kit {
namespace {
class DisplayTest : public testing::Test {
protected:
    void SetUp() override {
        if (!test::display_available()) { GTEST_SKIP() << "No GTK display available"; }
        app_ = Gtk::Application::create({}, Gio::Application::Flags::NON_UNIQUE);
        ensure_resources();
    }
    Glib::RefPtr<Gtk::Application> app_;
};

TEST_F(DisplayTest, LoadsCssAndResourceTextures) {
    ThemeManager theme;
    Gtk::Window window;
    theme.manage(window);
    EXPECT_TRUE(window.has_css_class("braurser"));
    theme.set_theme(Theme::Dark);
    EXPECT_EQ(theme.resolved(), Theme::Dark);
    theme.set_theme(Theme::Light);
    EXPECT_EQ(theme.resolved(), Theme::Light);
    EXPECT_TRUE(icon_texture("braurser-package-symbolic", Gdk::RGBA("#123456"), 24));
    EXPECT_TRUE(icon_texture("braurser-community-symbolic", Gdk::RGBA("#123456"), 24));
    auto provider = Gtk::CssProvider::create();
    unsigned errors = 0;
    provider->signal_parsing_error().connect([&errors](const Glib::RefPtr<const Gtk::CssSection>&, const Glib::Error&) { ++errors; });
    provider->load_from_resource("/org/braurser/kit/braurser-light.css");
    provider->load_from_resource("/org/braurser/kit/braurser-dark.css");
    EXPECT_EQ(errors, 0U);
}

TEST_F(DisplayTest, ThemeChangesInvalidateCachedIconTextures) {
    ThemeManager theme;
    theme.set_theme(Theme::Light);
    const auto color = Gdk::RGBA("#123456");
    const auto light = icon_texture("braurser-official-symbolic", color, 24);
    theme.set_theme(Theme::Dark);
    EXPECT_NE(light, icon_texture("braurser-official-symbolic", color, 24));
    Icon glyph("braurser-official-symbolic", 24, "br_accent");
    EXPECT_THROW(Icon("official-symbolic", 24), std::invalid_argument);
    EXPECT_THROW(Icon("braurser-official-symbolic", 24, "unknown"), std::invalid_argument);
}

TEST_F(DisplayTest, LocalAppIconsAreBoundedAndFailClosed) {
    auto path = (std::filesystem::temp_directory_path() / "braurser-icon-XXXXXX").string();
    const int fd = ::mkstemp(path.data());
    ASSERT_GE(fd, 0); ::close(fd);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    auto image = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, true, 8, 128, 64);
    image->fill(0x123456ff); image->save(path, "png");
    const auto icon = app_icon_texture(path);
    ASSERT_TRUE(icon); EXPECT_EQ(icon->get_width(), 64); EXPECT_EQ(icon->get_height(), 32);
    const auto scaled = app_icon_texture(path, 2);
    ASSERT_TRUE(scaled); EXPECT_EQ(scaled->get_width(), 128); EXPECT_EQ(scaled->get_height(), 64);
    EXPECT_FALSE(app_icon_texture(path, 0)); EXPECT_FALSE(app_icon_texture(path, 5));
    for (const auto* invalid : {"", "relative.png", "https://example.org/app.png", "/dev/null", "/missing/app.png"}) {
        EXPECT_FALSE(app_icon_texture(invalid));
    }
    EXPECT_FALSE(app_icon_texture(path + std::string("\0.png", 5)));
    const auto link = path + "-link";
    Cleanup link_cleanup{link};
    std::filesystem::create_symlink(path, link);
    EXPECT_FALSE(app_icon_texture(link));
    image = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, true, 8, 1025, 1);
    image->fill(0x123456ff); image->save(path, "png");
    EXPECT_FALSE(app_icon_texture(path));
    std::filesystem::resize_file(path, 1024 * 1024 + 1);
    EXPECT_FALSE(app_icon_texture(path));
}

TEST_F(DisplayTest, DisabledPrimaryUsesNeutralForeground) {
    ThemeManager theme;
    Gtk::Window window;
    Gtk::Button button;
    Gtk::Label caption("Update everything");
    button.set_child(caption); button.add_css_class("primary"); button.set_sensitive(false);
    window.set_child(button); theme.manage(window);
    for (const auto mode : {Theme::Light, Theme::Dark}) {
        theme.set_theme(mode);
        Gdk::RGBA muted;
        ASSERT_TRUE(button.get_style_context()->lookup_color("br_muted", muted));
        EXPECT_EQ(caption.get_color(), muted);
        EXPECT_EQ(button.get_opacity(), 1.0);
    }
}

TEST_F(DisplayTest, CreatesWidgetsAndBoundsProgress) {
    const Source source{SourceKind::Official, "Official", "Distribution repository"};
    AppCard card("Krita", "Paint", source, "Krita. Paint. Source: Official.");
    PackageRow row("krita", "Paint", source, "Installed", "krita. Paint. Installed.");
    FilterChip chip("Installed", "Remove filter: Installed", true);
    EXPECT_TRUE(chip.toggle().get_active());
    PasswordField password("Password", "Caps Lock is on.");
    EXPECT_TRUE(password.entry().get_show_peek_icon());
    password.entry().set_text("test");
    password.clear();
    EXPECT_TRUE(password.entry().get_text().empty());
    ProgressPanel progress({"Downloading", "Downloading", 0.5, {"Downloading"}, "Cancel", true}, {});
    EXPECT_THROW(progress.update({"Downloading", "Downloading", 1.5, {}, "Cancel", true}), std::invalid_argument);
    progress.update({"Building", "Progress amount not available", {}, {"Building"}, "Cancel after this step", false});
}

TEST_F(DisplayTest, NavigationRowsAreFlatAndCountsDoNotExpand) {
    ThemeManager theme;
    const std::array<NavigationItem, 2> items{{
        {"discover", "Discover", "braurser-discover-symbolic", {}, "Discover"},
        {"updates", "Updates", "braurser-updates-symbolic", 12, "Updates, 12 available"}
    }};
    NavigationSidebar sidebar(items, {});
    auto* first = dynamic_cast<Gtk::ToggleButton*>(sidebar.get_first_child());
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(first->has_css_class("navigation-row"));
    EXPECT_FALSE(first->has_css_class("chip"));
    EXPECT_TRUE(first->get_hexpand());
    sidebar.select("updates");
    auto* second = dynamic_cast<Gtk::ToggleButton*>(first->get_next_sibling());
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(second->get_active());
    auto* row = dynamic_cast<Gtk::Box*>(second->get_child());
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->get_last_child()->get_hexpand());
    EXPECT_EQ(row->get_last_child()->get_halign(), Gtk::Align::END);
}

TEST_F(DisplayTest, ShellMeasurementNeverConstrainChildrenBelowMinimum) {
    Gtk::Box content;
    content.set_size_request(158, 100);
    const std::array<NavigationItem, 1> items{{{"discover", "Discover", "braurser-discover-symbolic", {}, "Discover"}}};
    ResponsiveShell shell(content, items, "Navigation", {});
    int minimum = 0;
    int natural = 0;
    int baseline = 0;
    int natural_baseline = 0;
    for (const int width : {-1, 1, 158, 360, 840, 1024}) {
        shell.measure(Gtk::Orientation::VERTICAL, width, minimum, natural, baseline, natural_baseline);
        EXPECT_GE(minimum, 100);
        EXPECT_GE(natural, minimum);
    }
}

TEST_F(DisplayTest, ThemePreservesDesktopFontPreference) {
    const auto settings = Gtk::Settings::get_default();
    const auto original = settings->property_gtk_font_name().get_value();
    settings->property_gtk_font_name() = "CaskaydiaCove Nerd Font 10";
    {
        ThemeManager theme;
        theme.set_theme(Theme::Light);
        theme.set_theme(Theme::Dark);
        EXPECT_EQ(settings->property_gtk_font_name().get_value(), "CaskaydiaCove Nerd Font 10");
    }
    settings->property_gtk_font_name() = original;
}

TEST_F(DisplayTest, ShellHasNarrowMinimum) {
    Gtk::ScrolledWindow content;
    Gtk::Label child("Content");
    content.set_child(child);
    const std::array<NavigationItem, 1> items{{{"discover", "Discover", "braurser-discover-symbolic", {}, "Discover"}}};
    ResponsiveShell shell(content, items, "Navigation", {});
    int minimum = 0;
    int natural = 0;
    int baseline = 0;
    int natural_baseline = 0;
    shell.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, baseline, natural_baseline);
    EXPECT_LE(minimum, 360);
}
}
}
