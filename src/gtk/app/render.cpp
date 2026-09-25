#include <braurser/gtk/render.hpp>
#include <stdexcept>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::gtk {
namespace {
bool write_png(GdkTexture* texture, const std::string& path) {
    auto* bytes = gdk_texture_save_to_png_bytes(texture);
    if (!bytes) { return false; }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat metadata{};
    bool success = fd >= 0 && ::fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
        metadata.st_uid == ::geteuid() && metadata.st_nlink == 1 && ::fchmod(fd, 0600) == 0 && ::ftruncate(fd, 0) == 0;
    gsize size = 0;
    const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
    std::size_t offset = 0;
    while (success && offset < size) {
        const auto written = ::write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) { continue; }
        if (written <= 0) { success = false; }
        else { offset += static_cast<std::size_t>(written); }
    }
    if (fd >= 0 && ::close(fd) != 0) { success = false; }
    g_bytes_unref(bytes);
    return success;
}
}

FrameCapture::FrameCapture(Gtk::Window& window, Gtk::Widget& content, std::string path, int width,
                 std::function<void(bool)> complete)
        : window_(window), content_(content), path_(std::move(path)), width_(width), complete_(std::move(complete)) {
    if (width_ < 280 || width_ > 3840 || path_.empty() || path_.size() > 4096 || path_.find('\0') != std::string::npos) {
        throw std::invalid_argument("invalid render size or path");
    }
    Gtk::Settings::get_default()->property_gtk_enable_animations() = false;
    window_.set_decorated(false);
    window_.set_default_size(width_, 640);
    window_.set_size_request(width_, 640);
    window_.set_resizable(false);
    window_.present();
    clock_ = window_.get_frame_clock();
    if (!clock_) { throw std::runtime_error("frame clock unavailable"); }
    paint_ = g_signal_connect(clock_->gobj(), "after-paint", G_CALLBACK(+[](GdkFrameClock*, gpointer data) {
        static_cast<FrameCapture*>(data)->painted();
    }), this);
    timeout_ = Glib::signal_timeout().connect([this] { finish(false); return false; }, 10000);
    content_.queue_draw();
}

FrameCapture::~FrameCapture() { disconnect_paint(); idle_.disconnect(); timeout_.disconnect(); }

void FrameCapture::painted() {
    if (!window_.get_mapped() || idle_.connected()) { return; }
    idle_ = Glib::signal_idle().connect([this] {
        if (++stage_ < 3) {
            content_.queue_draw();
        } else {
            try { save(); finish(true); }
            catch (const std::exception& error) { g_printerr("Render failed: %s\n", error.what()); finish(false); }
        }
        return false;
    });
}

void FrameCapture::save() {
    if (content_.get_width() != width_ || content_.get_height() != 640) {
        content_.size_allocate(Gtk::Allocation(0, 0, width_, 640), -1);
    }
    const int width = content_.get_width();
    const int height = content_.get_height();
    if (width != width_ || height != 640) { throw std::runtime_error("unexpected render allocation"); }
    auto content_snapshot = Gtk::Snapshot::create();
    gtk_widget_snapshot_child(GTK_WIDGET(window_.gobj()), content_.gobj(), content_snapshot->gobj());
    auto* content_node = gtk_snapshot_to_node(content_snapshot->gobj());
    if (!content_node) { throw std::runtime_error("empty content snapshot"); }
    auto snapshot = Gtk::Snapshot::create();
    Gdk::RGBA background;
    window_.get_style_context()->lookup_color("br_bg", background);
    graphene_rect_t bounds;
    graphene_rect_init(&bounds, 0, 0, static_cast<float>(width), static_cast<float>(height));
    gtk_snapshot_append_color(snapshot->gobj(), background.gobj(), &bounds);
    gtk_snapshot_append_node(snapshot->gobj(), content_node);
    gsk_render_node_unref(content_node);
    auto* node = gtk_snapshot_to_node(snapshot->gobj());
    if (!node) { throw std::runtime_error("empty snapshot"); }
    auto* renderer = gsk_cairo_renderer_new();
    GError* error = nullptr;
    if (!gsk_renderer_realize(renderer, nullptr, &error)) {
        g_clear_error(&error);
        g_object_unref(renderer);
        gsk_render_node_unref(node);
        throw std::runtime_error("renderer unavailable");
    }
    auto* texture = gsk_renderer_render_texture(renderer, node, &bounds);
    const bool saved = texture && write_png(texture, path_);
    if (texture) { g_object_unref(texture); }
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    gsk_render_node_unref(node);
    if (!saved) { throw std::runtime_error("PNG write failed"); }
}

void FrameCapture::finish(bool success) {
    disconnect_paint();
    timeout_.disconnect();
    complete_(success);
}

void FrameCapture::disconnect_paint() { if (paint_) { g_signal_handler_disconnect(clock_->gobj(), paint_); paint_ = 0; } }

}
