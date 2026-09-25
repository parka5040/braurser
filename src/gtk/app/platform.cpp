#include <braurser/gtk/platform.hpp>
#include <giomm/desktopappinfo.h>
#include <algorithm>

namespace braurser::gtk {

bool valid_desktop_id(std::string_view id) {
    return id.size() > 8 && id.size() <= 255 && id.ends_with(".desktop") && id.front() != '.' &&
        id.find("..") == std::string_view::npos &&
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string_view::npos;
}

MainDispatcher::MainDispatcher() { connection_ = wake_.connect([this] { drain(); }); }

MainDispatcher::~MainDispatcher() { connection_.disconnect(); }

void MainDispatcher::post(std::function<void()> callback) {
    { std::lock_guard lock(mutex_); callbacks_.push_back(std::move(callback)); }
    wake_.emit();
}

void MainDispatcher::drain() {
    std::deque<std::function<void()>> callbacks;
    { std::lock_guard lock(mutex_); callbacks.swap(callbacks_); }
    for (auto& callback : callbacks) { callback(); }
}

MainTimer::~MainTimer() { for (auto& [id, connection] : connections_) { static_cast<void>(id); connection.disconnect(); } }

MainTimer::Token MainTimer::after(std::chrono::milliseconds delay, std::function<void()> callback) {
    const auto token = next_++;
    connections_[token] = Glib::signal_timeout().connect([this, token, callback = std::move(callback)] {
        connections_.erase(token);
        callback();
        return false;
    }, static_cast<unsigned>(std::clamp<std::int64_t>(delay.count(), 1, 600000)));
    return token;
}

void MainTimer::cancel(Token token) {
    const auto it = connections_.find(token);
    if (it != connections_.end()) { it->second.disconnect(); connections_.erase(it); }
}

bool DesktopPlatform::open_desktop(std::string_view id) {
    if (!valid_desktop_id(id)) { return false; }
    try {
        const auto entry = Gio::DesktopAppInfo::create(std::string(id));
        if (!entry || entry->is_hidden()) { return false; }
        return entry->launch(std::vector<Glib::RefPtr<Gio::File>>{}, Gdk::Display::get_default()->get_app_launch_context());
    } catch (const Glib::Error&) { return false; }
}

bool DesktopPlatform::open_link(std::string_view uri) {
    if (!kit::allowed_link(uri)) { return false; }
    try { return Gio::AppInfo::launch_default_for_uri(std::string(uri), Gdk::Display::get_default()->get_app_launch_context()); }
    catch (const Glib::Error&) { return false; }
}

void DesktopPlatform::reboot(std::function<void(bool)> complete) {
    auto* callback = new std::function<void(bool)>(std::move(complete));
    g_bus_get(G_BUS_TYPE_SYSTEM, nullptr, [](GObject*, GAsyncResult* result, gpointer data) {
        auto* completion = static_cast<std::function<void(bool)>*>(data);
        GError* error = nullptr;
        auto* bus = g_bus_get_finish(result, &error);
        if (!bus) { g_clear_error(&error); (*completion)(false); delete completion; return; }
        g_dbus_connection_call(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
            "Reboot", g_variant_new("(b)", TRUE), G_VARIANT_TYPE_UNIT, G_DBUS_CALL_FLAGS_NONE, 30000, nullptr,
            [](GObject* source, GAsyncResult* response, gpointer state) {
                std::unique_ptr<std::function<void(bool)>> done(static_cast<std::function<void(bool)>*>(state));
                GError* failure = nullptr;
                auto* value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), response, &failure);
                const bool success = value != nullptr;
                if (value) { g_variant_unref(value); }
                g_clear_error(&failure);
                (*done)(success);
            }, completion);
        g_object_unref(bus);
    }, callback);
}

}
