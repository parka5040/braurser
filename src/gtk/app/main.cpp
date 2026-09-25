#include <braurser/gtk/application.hpp>
#include <presenter/fixtures.hpp>

using braurser::presenter::fixtures::FakeServer;
#include <charconv>
#include <iostream>
#include <sys/stat.h>

namespace braurser::gtk {
struct Options {
    std::string page = "discover";
    std::string search;
    std::string render_png;
    kit::Theme theme = kit::Theme::System;
    int width = 1024;
};
Options parse_options(int argc, char** argv) {
    Options options;
    if (argc > 11) { throw std::invalid_argument("too many options"); }
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (i + 1 == argc) { throw std::invalid_argument("missing option value"); }
        const std::string_view value(argv[++i]);
        if (value.size() > 4096 || !wire::valid_utf8(value)) { throw std::invalid_argument("invalid option value"); }
        if (flag == "--page") { static_cast<void>(destination(value)); options.page = value; }
        else if (flag == "--search") { options.search = value; options.page = "search"; }
        else if (flag == "--render-png") { if (value.empty()) { throw std::invalid_argument("empty output path"); } options.render_png = value; }
        else if (flag == "--theme") {
            if (value == "light") { options.theme = kit::Theme::Light; }
            else if (value == "dark") { options.theme = kit::Theme::Dark; }
            else if (value == "system") { options.theme = kit::Theme::System; }
            else { throw std::invalid_argument("invalid theme"); }
        } else if (flag == "--width") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.width);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || options.width < 280 || options.width > 3840) { throw std::invalid_argument("invalid width"); }
        } else { throw std::invalid_argument("unknown option"); }
    }
    return options;
}
}

int main(int argc, char** argv) {
    using namespace braurser::gtk;
    try {
        const auto options = parse_options(argc, argv);
        ::umask(0077);
        const bool fixture = !options.render_png.empty();
        const auto app = Gtk::Application::create("org.braurser.Gtk", fixture ? Gio::Application::Flags::NON_UNIQUE : Gio::Application::Flags::DEFAULT_FLAGS);
        MainDispatcher dispatcher;
        MainTimer timer;
        std::unique_ptr<p::Rpc> rpc;
        if (fixture) { rpc = std::make_unique<FakeServer>(dispatcher); }
        else { rpc = std::make_unique<p::RpcClient>(dispatcher); }
        Models models(*rpc, timer);
        p::Copy copy;
        DesktopPlatform platform;
        std::unique_ptr<AppWindow> window;
        std::vector<p::Subscription> subscriptions;
        std::optional<std::string> watched_job;
        subscriptions.push_back(models.confirm.observe([&](const auto& state) { if (state.job_id && models.activity.state().job_id != state.job_id) { models.activity.attach(*state.job_id); } }));
        subscriptions.push_back(models.updates.observe([&](const auto& state) { if (state.check_job && models.activity.state().job_id != state.check_job) { models.activity.attach(*state.check_job); } }));
        subscriptions.push_back(models.git.observe([&](const auto& state) { if (state.job_id && models.activity.state().job_id != state.job_id) { models.activity.attach(*state.job_id); } }));
        subscriptions.push_back(models.activity.observe([&](const auto& state) {
            if (!state.job || state.job->state.value != "succeeded" || !state.job->result || watched_job == state.job_id) { return; }
            watched_job = state.job_id;
            try {
                const auto& result = *state.job->result;
                if (result.contains("report_id")) { models.updates.report(result.template get<wire::UpdateReport>()); }
                else if (result.contains("preview_id")) { models.git.preview(result.template get<wire::ManualPreview>()); }
                else if (result.contains("receipt_id")) { models.git.result(result.template get<wire::ManualReceipt>()); }
            } catch (const std::exception&) {}
        }));
        subscriptions.push_back(rpc->listen<wire::JobStateNotification>([&](const auto&) { models.shell.refresh(); }));
        subscriptions.push_back(rpc->listen<wire::JobQuestionNotification>([&](const auto& event) { if (models.git.state().job_id == event.jobId) { models.git.redirect(event.question); } }));
        subscriptions.push_back(rpc->listen<wire::CatalogChangedNotification>([&](const auto&) { models.confirm.invalidate(); models.shell.refresh(); }));
        if (auto* client = dynamic_cast<p::RpcClient*>(rpc.get())) { subscriptions.push_back(client->connection_changed([&](bool connected) { models.shell.connected(connected); })); }
        int render_status = 0;
        app->signal_activate().connect([&] {
            if (window) { window->present(); return; }
            models.shell.navigate({destination(options.page), options.page == "details" ? "krita" : "", 0});
            if (!options.search.empty()) { models.search.query(options.search); }
            window = std::make_unique<AppWindow>(models, copy, platform, options.width);
            window->set_theme(options.theme);
            app->add_window(*window);
            models.shell.refresh();
            rpc->call<p::Method<wire::HelloParams>>({wire::kApiVersion}, [&](p::Reply<wire::HelloResult> reply) {
                if (!reply) { return; }
                const auto helper = reply->capabilities.helper.value;
                const bool available = helper == "paru" || helper == "yay";
                models.settings.capabilities(available ? std::set<std::string>{helper} : std::set<std::string>{}, available ? helper : "", models.shell.state().active_job.has_value());
                models.first.refresh(); models.search.facets();
            });
            window->refresh();
            if (fixture) {
                static_cast<FakeServer&>(*rpc).populate(models);
                if (options.page == "activity") { models.activity.attach("fixture-active"); }
                for (int i = 0; i < 8; ++i) { dispatcher.drain(); }
                window->render_png(options.render_png, options.width, [&](bool success) {
                    render_status = success ? 0 : 1;
                    std::cout << (success ? "rendered " : "render failed ") << options.render_png << '\n';
                    app->quit();
                });
            } else { window->present(); }
        });
        const int status = app->run(1, argv);
        subscriptions.clear();
        window.reset();
        return render_status != 0 ? render_status : status;
    } catch (const std::exception&) { return 1; }
}
