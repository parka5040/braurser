#include "services.hpp"
#include <braurser/daemon/api/registry.hpp>
#include <braurser/daemon/api/logger.hpp>
#include <braurser/daemon/api/server.hpp>
#include <braurser/daemon/api/services.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/notifications/service.hpp>
#include <braurser/daemon/jobs/register.hpp>
#include <giomm/init.h>
#include <glibmm/main.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fcntl.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t terminate_requested = 0;
extern "C" void terminate_handler(int) { terminate_requested = 1; }
}

int main(int argc, char** argv) {
    Gio::init();
    bool background = false;
    std::filesystem::path socket;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--background") background = true;
        else if (option == "--socket" && index + 1 < argc) socket = argv[++index];
        else { std::fputs("usage: braurserd [--background] [--socket path]\n", stderr); return 2; }
    }
    auto paths = braurser::daemon::core::resolve_xdg_paths();
    if (!paths) { std::fputs("private XDG runtime directory required\n", stderr); return 1; }
    if (background) {
        if (setsid() < 0) return 1;
        const int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd < 0) return 1;
        for (int fd = 0; fd < 3; ++fd) if (dup2(null_fd, fd) < 0) return 1;
        if (null_fd > 2) close(null_fd);
    }
    std::unique_ptr<braurser::daemon::api::DaemonLog> log;
    try {
        log = std::make_unique<braurser::daemon::api::DaemonLog>(paths->state);
        log->write("info", "daemon", "startup");
    } catch (const std::exception&) {
        std::fputs("braurserd: cannot initialize daemon log\n", stderr);
        return 1;
    }
    braurser::daemon::api::HandlerRegistry registry;
    braurser::daemon::api::Server server(registry, paths->runtime, socket, nullptr, log.get());
    auto started = server.start();
    if (!started) { std::fprintf(stderr, "braurserd: %s\n", started.error().c_str()); return 1; }
    braurser::daemon::api::Broadcaster broadcaster(server);
    std::unique_ptr<braurser::daemon::app::RealServices> services;
    try { services = std::make_unique<braurser::daemon::app::RealServices>(*paths, broadcaster, log.get()); }
    catch (const std::exception&) {
        std::fputs("braurserd: cannot initialize services\n", stderr);
        server.stop();
        return 1;
    }
    braurser::daemon::api::register_handlers(registry, *services);
    const auto stop_jobs = braurser::daemon::jobs::register_handlers(registry, *services, *paths, broadcaster,
        [&services] { return services->latest_report(); }, server,
        [&services](std::string_view id) { return services->preview_result(id); });
    braurser::daemon::notifications::register_service(registry, *services, paths->state, std::filesystem::read_symlink("/proc/self/exe").parent_path() / "braurser");
    services->start_monitors();
    services->startup_refresh();
    struct sigaction action{};
    action.sa_handler = terminate_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    auto signals = Glib::signal_timeout().connect([&server, &stop_jobs] {
        if (!terminate_requested) return true;
        stop_jobs();
        server.stop();
        return false;
    }, 100);
    server.run();
    signals.disconnect();
    stop_jobs();
    return 0;
}
