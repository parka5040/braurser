#include "selection.hpp"
#include <braurser/presenter/rpc_client.hpp>
#include <giomm/init.h>
#include <glibmm/main.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
struct MainDispatcher final : braurser::presenter::Dispatcher {
    void post(std::function<void()> callback) override {
        Glib::MainContext::get_default()->invoke([callback = std::move(callback)] { callback(); return false; });
    }
};
std::string variable(const char* name) {
    const auto* value = std::getenv(name);
    return value ? value : "";
}
}

int main(int argc, char** argv) {
    Gio::init();
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || !self.is_absolute()) { std::fputs("braurser: executable location unavailable\n", stderr); return 1; }
    const auto directory = self.parent_path();
    MainDispatcher dispatcher;
    braurser::presenter::RpcOptions options;
    options.daemon_path = (directory / "braurserd").string();
    std::string preference;
    bool ready = false;
    try {
        braurser::presenter::RpcClient client(dispatcher, options);
        const auto loop = Glib::MainLoop::create();
        auto timeout = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, 15000);
        client.call<braurser::presenter::Method<braurser::protocol::SettingsGetParams>>({},
            [&](auto result) {
                if (result) {
                    preference = result->uiPreference.value;
                    ready = true;
                }
                loop->quit();
            }, std::chrono::seconds(12));
        loop->run();
        timeout.disconnect();
    } catch (const std::exception&) {}
    if (!ready) { std::fputs("braurser: software service unavailable\n", stderr); return 1; }
    const auto ui = braurser::launcher::select_ui(variable("BRAURSER_UI"), preference,
                                                  variable("XDG_CURRENT_DESKTOP"));
    if (!ui) { std::fputs("braurser: invalid UI preference\n", stderr); return 1; }
    const auto selected = braurser::launcher::select_binary(directory, *ui);
    if (!selected) { std::fputs("braurser: no UI binary available\n", stderr); return 1; }
    if (selected->fallback) std::fprintf(stderr, "braurser: %s UI unavailable; using %s instead\n",
                                         ui->c_str(), *ui == "qt" ? "gtk" : "qt");
    std::string binary = selected->binary.string();
    std::vector<char*> arguments;
    arguments.reserve(static_cast<std::size_t>(argc) + 1);
    arguments.push_back(binary.data());
    for (int index = 1; index < argc; ++index) arguments.push_back(argv[index]);
    arguments.push_back(nullptr);
    execv(binary.c_str(), arguments.data());
    std::fputs("braurser: cannot launch UI\n", stderr);
    return 1;
}
