#include <braurser/daemon/connectivity/checker.hpp>

#include <gio/gio.h>
#include <memory>

namespace braurser::daemon::connectivity {

Status GioNetworkManagerStatus::query() {
    GError* error = nullptr;
    auto proxy = std::unique_ptr<GDBusProxy, decltype(&g_object_unref)>(
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
                                      static_cast<GDBusProxyFlags>(G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                                                  G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
                                      nullptr, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                                      "org.freedesktop.NetworkManager", nullptr, &error), g_object_unref);
    if (error) g_error_free(error);
    if (!proxy) return Status::Unknown;
    error = nullptr;
    auto response = std::unique_ptr<GVariant, decltype(&g_variant_unref)>(
        g_dbus_connection_call_sync(g_dbus_proxy_get_connection(proxy.get()), "org.freedesktop.NetworkManager",
                                    "/org/freedesktop/NetworkManager", "org.freedesktop.DBus.Properties", "Get",
                                    g_variant_new("(ss)", "org.freedesktop.NetworkManager", "Connectivity"),
                                    G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1500, nullptr, &error),
        g_variant_unref);
    if (error) g_error_free(error);
    if (!response) return Status::Unknown;
    GVariant* value = nullptr;
    g_variant_get(response.get(), "(v)", &value);
    if (!value) return Status::Unknown;
    auto property = std::unique_ptr<GVariant, decltype(&g_variant_unref)>(value, g_variant_unref);
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) return Status::Unknown;
    switch (g_variant_get_uint32(value)) {
        case 4: return Status::Online;
        case 1: case 2: case 3: return Status::Offline;
        default: return Status::Unknown;
    }
}

ConnectivityChecker::ConnectivityChecker(NetworkManagerStatus& nm, core::HttpClient& http,
                                         const core::Clock& clock)
    : nm_(nm), http_(http), clock_(clock) {}

Status ConnectivityChecker::check() {
    const auto now = clock_.now();
    if (cached_ && now >= checked_at_ && now - checked_at_ < std::chrono::seconds(60)) return *cached_;
    auto status = nm_.query();
    if (status == Status::Unknown) {
        core::HttpRequest request;
        request.url = "https://ping.archlinux.org/nm-check.txt";
        request.allowed_hosts = {"ping.archlinux.org"};
        request.max_bytes = 64;
        request.timeout = std::chrono::seconds(3);
        const auto response = http_.get(request);
        status = response && response->status == 200 && (response->body == "NetworkManager is online" || response->body == "NetworkManager is online\n")
                     ? Status::Online : Status::Offline;
    }
    cached_ = status;
    checked_at_ = now;
    return status;
}

}
