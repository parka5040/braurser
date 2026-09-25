#include "policy.hpp"
#include <braurser/daemon/core/files.hpp>
#include <braurser/presenter/rpc_client.hpp>
#include <giomm/init.h>
#include <glibmm/main.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct MainDispatcher final : braurser::presenter::Dispatcher {
    void post(std::function<void()> callback) override {
        Glib::MainContext::get_default()->invoke([callback = std::move(callback)] { callback(); return false; });
    }
};

std::optional<braurser::protocol::Settings> load_settings(const std::filesystem::path& config) {
    const auto path = config / "braurser/settings.json";
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0600) return std::nullopt;
    const auto bytes = braurser::daemon::core::read_bounded(path, 8192);
    if (!bytes) return std::nullopt;
    try { return nlohmann::json::parse(*bytes).get<braurser::protocol::Settings>(); }
    catch (const std::exception&) { return std::nullopt; }
}
}

int main() {
    Gio::init();
    const auto paths = braurser::daemon::core::resolve_xdg_paths();
    if (!paths) return 0;
    const auto initial_settings = load_settings(paths->config);
    if (!initial_settings || !braurser::notifier::interval(*initial_settings)) return 0;
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || !executable.is_absolute()) return 1;
    try {
        MainDispatcher dispatcher;
        braurser::presenter::RpcOptions options;
        options.daemon_path = (executable.parent_path() / "braurserd").string();
        braurser::presenter::RpcClient client(dispatcher, options);
        const auto loop = Glib::MainLoop::create();
        sigc::connection pending;
        std::function<void(unsigned)> schedule;
        schedule = [&](unsigned seconds) {
            pending.disconnect();
            pending = Glib::signal_timeout().connect_seconds([&] {
                auto settings = load_settings(paths->config);
                if (!settings) { loop->quit(); return false; }
                auto next = braurser::notifier::interval(*settings);
                if (!next) { loop->quit(); return false; }
                client.call<braurser::presenter::Method<braurser::protocol::NotifierCheckParams>>({},
                    [&, wait = static_cast<unsigned>(next->count())](auto result) {
                        schedule(!result || (result->reason && *result->reason == "pending") ? 120U : wait);
                    },
                    std::chrono::seconds(20));
                return false;
            }, seconds);
        };
        auto monitor = Glib::signal_timeout().connect_seconds([&] {
            const auto settings = load_settings(paths->config);
            if (settings && braurser::notifier::interval(*settings)) return true;
            loop->quit();
            return false;
        }, 60);
        schedule(120);
        loop->run();
        pending.disconnect();
        monitor.disconnect();
    } catch (const std::exception&) { std::fputs("braurser-notifier: service unavailable\n", stderr); return 1; }
    return 0;
}
