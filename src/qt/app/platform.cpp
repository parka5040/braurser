#include <gio/gdesktopappinfo.h>
#include <braurser/qt/app.hpp>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDesktopServices>
#include <QUrl>
#include <algorithm>

namespace braurser::qt {

void Dispatcher::post(std::function<void()> callback) {
    QMetaObject::invokeMethod(this, std::move(callback), Qt::QueuedConnection);
}

presenter::Timer::Token Timer::after(std::chrono::milliseconds delay, std::function<void()> callback) {
    const auto token = next_++;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    timers_.emplace(token, timer);
    connect(timer, &QTimer::timeout, this, [this, token, callback = std::move(callback)] {
        cancel(token);
        callback();
    });
    timer->start(static_cast<int>(std::clamp<std::int64_t>(delay.count(), 0, 600000)));
    return token;
}

void Timer::cancel(Token token) {
    const auto found = timers_.find(token);
    if (found == timers_.end()) { return; }
    found->second->stop();
    found->second->deleteLater();
    timers_.erase(found);
}

bool valid_desktop_id(std::string_view id) {
    return id.size() > 8 && id.size() <= 255 && id.ends_with(".desktop") && id.front() != '.' &&
        id.find("..") == std::string_view::npos && std::ranges::all_of(id, [](unsigned char value) {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
        });
}

bool DesktopPlatform::open_url(const QUrl& url) {
    const auto bytes = url.toEncoded();
    if (!kit::allowed_link(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size()))) ||
        !url.isValid() || !url.userInfo().isEmpty() || url.host().isEmpty()) { return false; }
    return QDesktopServices::openUrl(url);
}

bool DesktopPlatform::open_desktop(std::string_view id) {
    if (!valid_desktop_id(id)) { return false; }
    GDesktopAppInfo* app = g_desktop_app_info_new(std::string(id).c_str());
    if (!app) { return false; }
    const bool launched = g_app_info_launch(G_APP_INFO(app), nullptr, nullptr, nullptr);
    g_object_unref(app);
    return launched;
}

void DesktopPlatform::reboot(std::function<void(bool)> completed) {
    auto message = QDBusMessage::createMethodCall("org.freedesktop.login1", "/org/freedesktop/login1",
                                                "org.freedesktop.login1.Manager", "Reboot");
    message << true;
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, completed = std::move(completed)] {
        const QDBusPendingReply<> reply = *watcher;
        completed(!reply.isError());
        watcher->deleteLater();
    });
}

Models::Models(presenter::Rpc& rpc, presenter::Timer& timer)
    : shell(rpc), search(rpc, timer), details(rpc), confirm(rpc), activity(rpc), updates(rpc),
      installed(rpc), git(rpc), settings(rpc), first(rpc) {}

}
