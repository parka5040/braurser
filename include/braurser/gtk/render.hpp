#ifndef BRAURSER_GTK_RENDER_HPP
#define BRAURSER_GTK_RENDER_HPP

#include <braurser/gtk/kit.hpp>

namespace braurser::gtk {

class FrameCapture final {
public:
    FrameCapture(Gtk::Window& window, Gtk::Widget& content, std::string path, int width,
                 std::function<void(bool)> complete);
    ~FrameCapture();
private:
    void painted();
    void save();
    void finish(bool success);
    void disconnect_paint();
    Gtk::Window& window_;
    Gtk::Widget& content_;
    std::string path_;
    int width_;
    unsigned stage_ = 0;
    std::function<void(bool)> complete_;
    Glib::RefPtr<Gdk::FrameClock> clock_;
    gulong paint_ = 0;
    sigc::connection idle_;
    sigc::connection timeout_;
};

}

#endif
