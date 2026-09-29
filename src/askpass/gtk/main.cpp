#include "core.cpp"
#include "../../gtk/app/platform.cpp"
#include <csignal>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>

namespace braurser::askpass {
class AuthWindow final : public Gtk::Window {
public:
    AuthWindow(presenter::Auth& auth, const presenter::Copy& copy, std::function<void()> done)
        : auth_(auth), done_(std::move(done)), password_(copy.text("auth.password", {{"user", auth.state().account}}), copy.text("auth.caps")) {
        set_title(copy.text("auth.window_title"));
        set_modal(true);
        set_default_size(440, 400);
        root_.set_margin(20);
        const auto add = [this](const std::string& text) {
            auto* label = Gtk::make_managed<Gtk::Label>(text);
            label->set_wrap(true); label->set_wrap_mode(Pango::WrapMode::WORD_CHAR); label->set_xalign(0);
            root_.append(*label);
        };
        add(copy.text("auth.title"));
        add(auth.state().operation.render(copy));
        add(copy.text("auth.account"));
        add(auth.state().account);
        auto* delegate = gtk_editable_get_delegate(GTK_EDITABLE(password_.entry().gobj()));
        if (!delegate || !GTK_IS_TEXT(delegate)) { throw std::runtime_error("password buffer unavailable"); }
        secure_ = gtk_password_entry_buffer_new();
        gtk_entry_buffer_set_max_length(secure_, 4096);
        gtk_text_set_buffer(GTK_TEXT(delegate), secure_);
        root_.append(password_);
        add(copy.text("auth.context"));
        cancel_.set_label(copy.text("action.cancel")); cancel_.set_size_request(44, 44);
        submit_.set_label(copy.text("auth.authenticate")); submit_.set_size_request(44, 44);
        buttons_.append(cancel_); buttons_.append(submit_); root_.append(buttons_);
        set_child(root_);
        cancel_.signal_clicked().connect([this] { cancel(); });
        submit_.signal_clicked().connect([this] { submit(); });
        password_.entry().signal_activate().connect([this] { submit(); });
        signal_close_request().connect([this] { cancel(); return true; }, false);
        observation_ = auth.observe([this](const auto& state) { if (!state.active) { wipe(); done_(); } });
        caps_ = Glib::signal_timeout().connect([this] {
            const auto seat = get_display()->get_default_seat();
            if (seat && seat->get_keyboard()) { password_.set_caps_lock(seat->get_keyboard()->get_caps_lock_state()); }
            return true;
        }, 250);
        password_.entry().grab_focus();
    }
    ~AuthWindow() override {
        caps_.disconnect(); observation_.reset(); wipe();
        if (secure_) { g_object_unref(secure_); }
    }
private:
    void wipe() {
        if (secure_) { gtk_entry_buffer_delete_text(secure_, 0, -1); }
        password_.clear();
    }
    void submit() {
        if (!auth_.state().active) { return; }
        const char* text = gtk_entry_buffer_get_text(secure_);
        const auto length = gtk_entry_buffer_get_bytes(secure_);
        if (length > 4096) { cancel(); return; }
        try { auth_.password(std::span(text, length)); }
        catch (...) { cancel(); return; }
        wipe();
        auth_.submit();
    }
    void cancel() { wipe(); auth_.cancel(); }
    presenter::Auth& auth_;
    std::function<void()> done_;
    Gtk::Box root_{Gtk::Orientation::VERTICAL, 12};
    gtk::kit::PasswordField password_;
    Gtk::Box buttons_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button cancel_;
    Gtk::Button submit_;
    GtkEntryBuffer* secure_ = nullptr;
    presenter::Subscription observation_;
    sigc::connection caps_;
};
}

int main(int argc, char** argv) {
    using namespace braurser;
    ::umask(0077);
    if (::prctl(PR_SET_DUMPABLE, 0) != 0) { return 1; }
    const rlimit limit{0, 0};
    static_cast<void>(::setrlimit(RLIMIT_CORE, &limit));
    std::signal(SIGPIPE, SIG_IGN);
    try {
        presenter::Copy copy;
        const auto app = Gtk::Application::create("org.braurser.Askpass", Gio::Application::Flags::NON_UNIQUE);
        gtk::MainTimer timer;
        askpass::StdoutChannel channel;
        presenter::Auth auth(channel, timer);
        const char* environment = std::getenv("BRAURSER_AUTH_ACTION");
        std::string_view action;
        if (environment && ::strnlen(environment, 32) < 32) { action = environment; }
        auth.bind(askpass::local_binding(argc, argv, action, askpass::current_account(), copy));
        if (!auth.state().password_ui) { return 1; }
        std::unique_ptr<askpass::AuthWindow> window;
        app->signal_activate().connect([&] {
            window = std::make_unique<askpass::AuthWindow>(auth, copy, [&] { app->quit(); });
            app->add_window(*window); window->present();
        });
        app->run();
        window.reset();
        return channel.submitted ? 0 : 1;
    } catch (...) { return 1; }
}
