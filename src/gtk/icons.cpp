#include <braurser/gtk/kit.hpp>
#include <braurser/gtk/internal.hpp>
#include <theme_tokens.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::gtk::kit {
namespace {
using Key = std::tuple<std::string, std::string, int, int>;
std::map<Key, Glib::RefPtr<Gdk::Texture>> cache;
std::size_t cache_bytes = 0;
sigc::signal<void()> changed;

std::string icon_name(std::string_view icon) {
    if (icon.empty()) { icon = "braurser-package-symbolic"; }
    if (icon.size() > 128 || (icon != "braurser" && !icon.starts_with("braurser-")) ||
        icon.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string_view::npos) {
        throw std::invalid_argument("invalid icon name");
    }
    return std::string(icon);
}

void dimension(std::string& svg, std::string_view name, int pixels) {
    const auto attribute = std::string(name) + "=\"";
    const auto start = svg.find(attribute);
    const auto root_end = svg.find('>');
    if (start == std::string::npos || start > root_end) { throw std::runtime_error("missing icon dimension"); }
    const auto value = start + attribute.size();
    const auto end = svg.find('"', value);
    if (end == std::string::npos || end > root_end) { throw std::runtime_error("invalid icon dimension"); }
    svg.replace(value, end - value, std::to_string(pixels));
}
}

Glib::RefPtr<Gdk::Texture> icon_texture(std::string_view icon, const Gdk::RGBA& color, int size, int scale) {
    const auto name = icon_name(icon);
    if (size < 1 || size > 512 || scale < 1 || scale > 4) { throw std::invalid_argument("invalid icon dimensions"); }
    for (const auto channel : {color.get_red(), color.get_green(), color.get_blue(), color.get_alpha()}) {
        if (!std::isfinite(channel) || channel < 0 || channel > 1) { throw std::invalid_argument("invalid icon color"); }
    }
    const auto paint = color.to_string().raw();
    const Key key{name, paint, size, scale};
    if (const auto found = cache.find(key); found != cache.end()) { return found->second; }
    ensure_resources();
    const auto bytes = Gio::Resource::lookup_data_global("/org/braurser/kit/icons/scalable/actions/" + name + ".svg");
    gsize length = 0;
    const auto* data = static_cast<const char*>(bytes->get_data(length));
    if (length > 65536) { throw std::length_error("icon resource exceeds limit"); }
    std::string svg(data, length);
    for (std::size_t position = 0; (position = svg.find("currentColor", position)) != std::string::npos; position += paint.size()) {
        svg.replace(position, 12, paint);
    }
    const int pixels = size * scale;
    dimension(svg, "width", pixels);
    dimension(svg, "height", pixels);
    auto texture = Gdk::Texture::create_from_bytes(Glib::Bytes::create(svg.data(), svg.size()));
    const auto cost = static_cast<std::size_t>(pixels) * static_cast<std::size_t>(pixels) * 4;
    if (cache.size() >= 64 || cache_bytes + cost > 16 * 1024 * 1024) { cache.clear(); cache_bytes = 0; }
    cache.emplace(key, texture);
    cache_bytes += cost;
    return texture;
}

Glib::RefPtr<Gdk::Texture> app_icon_texture(std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() != '/' || path.find('\0') != std::string_view::npos) { return {}; }
    const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { return {}; }
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 24 || metadata.st_size > 1024 * 1024) {
        ::close(fd); return {};
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(metadata.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) { continue; }
        if (count <= 0) { break; }
        offset += static_cast<std::size_t>(count);
    }
    ::close(fd);
    const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (offset != bytes.size() || !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        bytes[8] != 0 || bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 13 ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 12), 4) != "IHDR") { return {}; }
    const auto dimension = [&bytes](std::size_t at) {
        return (static_cast<std::uint32_t>(bytes[at]) << 24) | (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
            (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | bytes[at + 3];
    };
    if (dimension(16) == 0 || dimension(16) > 1024 || dimension(20) == 0 || dimension(20) > 1024) { return {}; }
    try { return Gdk::Texture::create_from_bytes(Glib::Bytes::create(bytes.data(), bytes.size())); }
    catch (const Glib::Error&) { return {}; }
}

Icon::Icon(std::string_view icon, int size, std::string_view token)
    : Glib::ObjectBase("BraurserIcon"), icon_(icon_name(icon)), token_(token), size_(size) {
    if (size < 1 || size > 512 || std::ranges::none_of(theme::kColors, [token](const auto& color) { return color.name == token; })) {
        throw std::invalid_argument("invalid icon size or token");
    }
    ensure_resources();
    Gio::Resource::lookup_data_global("/org/braurser/kit/icons/scalable/actions/" + icon_ + ".svg");
    set_halign(Gtk::Align::CENTER);
    set_valign(Gtk::Align::CENTER);
    property_accessible_role() = Gtk::Accessible::Role::PRESENTATION;
    theme_connection_ = changed.connect([this] { queue_draw(); });
}

Icon::~Icon() { theme_connection_.disconnect(); }

void Icon::measure_vfunc(Gtk::Orientation, int, int& minimum, int& natural,
                         int& minimum_baseline, int& natural_baseline) const {
    minimum = size_;
    natural = size_;
    minimum_baseline = -1;
    natural_baseline = -1;
}

void Icon::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
    auto color = get_color();
    auto token = token_;
    if (!is_sensitive()) { token = "br_muted"; }
    else {
        for (auto* parent = get_parent(); parent; parent = parent->get_parent()) {
            if (const auto* button = dynamic_cast<Gtk::ToggleButton*>(parent); button && button->get_active()) {
                token = "br_on_selected";
                break;
            }
        }
    }
    get_style_context()->lookup_color(token, color);
    const auto texture = icon_texture(icon_, color, size_, std::clamp(get_scale_factor(), 1, 4));
    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0, 0, static_cast<float>(size_), static_cast<float>(size_));
    gtk_snapshot_append_texture(snapshot->gobj(), texture->gobj(), &bounds);
}

void detail::refresh_icons() {
    cache.clear();
    cache_bytes = 0;
    changed.emit();
}

}
