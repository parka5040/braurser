#include <braurser/gtk/kit.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <gdkmm/wrap_init.h>

namespace braurser::gtk::kit {
namespace {
TEST(Resources, ContainsBothPalettesWithoutDisplay) {
    Gio::init();
    ensure_resources();
    for (const auto* mode : {"light", "dark"}) {
        const auto path = std::string("/org/braurser/kit/braurser-") + mode + ".css";
        const auto bytes = Gio::Resource::lookup_data_global(path);
        gsize size = 0;
        const auto* data = static_cast<const char*>(bytes->get_data(size));
        const std::string_view css(data, size);
        EXPECT_NE(css.find("@define-color br_bg"), std::string_view::npos);
        EXPECT_NE(css.find(".braurser"), std::string_view::npos);
    }
}

TEST(Resources, ContainsEverySourceIconWithoutDisplay) {
    Gio::init();
    ensure_resources();
    std::size_t count = 0;
    for (const auto& file : std::filesystem::directory_iterator(BRAURSER_KIT_ICON_DIRECTORY)) {
        if (file.path().extension() != ".svg") { continue; }
        const auto path = "/org/braurser/kit/icons/scalable/actions/" + file.path().filename().string();
        const auto bytes = Gio::Resource::lookup_data_global(path);
        gsize size = 0;
        const auto* data = static_cast<const char*>(bytes->get_data(size));
        EXPECT_NE(std::string_view(data, size).find("<svg"), std::string_view::npos) << path;
        ++count;
    }
    EXPECT_GE(count, 30U);
}

TEST(Resources, StrokeIconsKeepTransparentInteriorsAndRequestedColor) {
    Gio::init();
    Gdk::wrap_init();
    const auto texture = icon_texture("braurser-official-symbolic", Gdk::RGBA("#ff0000"), 24);
    ASSERT_TRUE(texture);
    ASSERT_EQ(texture->get_width(), 24);
    ASSERT_EQ(texture->get_height(), 24);
    std::vector<std::uint32_t> pixels(24 * 24);
    texture->download(reinterpret_cast<guchar*>(pixels.data()), 24 * sizeof(std::uint32_t));
    EXPECT_EQ(pixels[9 * 24 + 12] >> 24, 0U);
    EXPECT_GT(pixels[6 * 24 + 4] >> 24, 200U);
    EXPECT_GT((pixels[6 * 24 + 4] >> 16) & 255U, 200U);
    EXPECT_EQ(pixels[6 * 24 + 4] & 65535U, 0U);
}

TEST(Resources, TextureCacheSeparatesColorSizeAndScale) {
    Gio::init();
    Gdk::wrap_init();
    const auto red = Gdk::RGBA("#ff0000");
    const auto first = icon_texture("braurser-official-symbolic", red, 24);
    EXPECT_EQ(first, icon_texture("braurser-official-symbolic", red, 24));
    EXPECT_NE(first, icon_texture("braurser-official-symbolic", Gdk::RGBA("#0000ff"), 24));
    EXPECT_NE(first, icon_texture("braurser-official-symbolic", red, 48));
    const auto scaled = icon_texture("braurser-official-symbolic", red, 24, 2);
    EXPECT_EQ(scaled->get_width(), 48);
    EXPECT_NE(scaled, icon_texture("braurser-official-symbolic", red, 48));
    for (const auto* name : {"official-symbolic", "../braurser", "braurser-icon.svg", "braurser-<svg>"}) {
        EXPECT_THROW(icon_texture(name, red, 24), std::invalid_argument);
    }
    EXPECT_THROW(icon_texture("braurser-official-symbolic", red, 513), std::invalid_argument);
    EXPECT_THROW(icon_texture("braurser-official-symbolic", red, 24, 0), std::invalid_argument);
    EXPECT_THROW(icon_texture("braurser-official-symbolic", red, 24, 5), std::invalid_argument);
}

TEST(Resources, EveryBundledGlyphDecodesAtRequestedScale) {
    Gio::init();
    Gdk::wrap_init();
    for (const auto& file : std::filesystem::directory_iterator(BRAURSER_KIT_ICON_DIRECTORY)) {
        if (file.path().extension() != ".svg") { continue; }
        const auto name = file.path().stem().string();
        EXPECT_TRUE(name == "braurser" || name.starts_with("braurser-"));
        const auto texture = icon_texture(name, Gdk::RGBA("#123456"), 20, 2);
        ASSERT_TRUE(texture) << name;
        EXPECT_EQ(texture->get_width(), 40) << name;
        EXPECT_EQ(texture->get_height(), 40) << name;
    }
}
}
}
