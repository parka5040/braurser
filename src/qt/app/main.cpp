#include <braurser/qt/app.hpp>
#include <braurser/qt/fixtures.hpp>
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QImage>
#include <QTimer>
#include <QUrl>
#include <sys/stat.h>
#include <algorithm>

namespace braurser::qt {
namespace {
class Connection final : public QObject {
public:
    Connection(presenter::Rpc& rpc, Models& models) : rpc_(rpc), models_(models) {
        if (auto* client = dynamic_cast<presenter::RpcClient*>(&rpc_)) {
            subscriptions_.push_back(client->connection_changed([this](bool connected) { models_.shell.connected(connected); }));
        }
        subscriptions_.push_back(rpc_.listen<protocol::CatalogChangedNotification>([this](const auto&) {
            if (models_.confirm.state().plan) { models_.confirm.invalidate(); }
        }));
        subscriptions_.push_back(models_.updates.observe([this](const auto& state) {
            if (state.check_job && checking_ != state.check_job) { checking_ = state.check_job; inspect(*checking_); }
        }));
        subscriptions_.push_back(models_.confirm.observe([this](const auto& state) { if (state.job_id && models_.activity.state().job_id != state.job_id) { models_.activity.attach(*state.job_id); } }));
        subscriptions_.push_back(models_.git.observe([this](const auto& state) {
            if (state.job_id && git_job_ != state.job_id) {
                git_job_ = state.job_id; inspect(*git_job_);
                if (state.step == presenter::GitStep::Progress) { models_.activity.attach(*git_job_); }
            }
        }));
        subscriptions_.push_back(rpc_.listen<protocol::JobStateNotification>([this](const auto& event) {
            if (event.jobId == checking_ || event.jobId == git_job_) { inspect(event.jobId); }
            models_.shell.refresh();
        }));
        subscriptions_.push_back(rpc_.listen<protocol::JobQuestionNotification>([this](const auto& event) {
            if (event.jobId == git_job_ && std::holds_alternative<protocol::GitRedirectQuestion>(event.question.choice.value)) { models_.git.redirect(event.question); }
        }));
        subscriptions_.push_back(models_.shell.observe([this](const auto& state) {
            if (state.active_job && models_.activity.state().job_id != state.active_job) { models_.activity.attach(*state.active_job); }
        }));
        rpc_.call<presenter::Method<protocol::HelloParams>>({1}, [self = QPointer<Connection>(this)](auto reply) {
            if (!self || !reply) { return; }
            const auto helper = reply->capabilities.helper.value;
            const bool found = helper == "paru" || helper == "yay";
            self->models_.settings.capabilities(found ? std::set<std::string>{helper} : std::set<std::string>{}, found ? helper : "", false);
            self->models_.first.refresh();
        });
        models_.settings.refresh(); models_.shell.refresh(); models_.first.refresh(); models_.search.facets();
    }
private:
    void inspect(std::string id) {
        rpc_.call<presenter::Method<protocol::JobsGetParams>>({id, {}}, [self = QPointer<Connection>(this), id](auto reply) {
            if (!self || !reply) { return; }
            if (reply->state.value == "queued" || reply->state.value == "running") {
                QTimer::singleShot(1000, self, [self, id] { if (self) { self->inspect(id); } });
                return;
            }
            if (reply->state.value == "succeeded" && reply->result) {
                try {
                    if (id == self->checking_) { self->models_.updates.report(reply->result->template get<protocol::UpdateReport>()); }
                    else if (id == self->git_job_) {
                        if (self->models_.git.state().step == presenter::GitStep::Progress) { self->models_.git.result(reply->result->template get<protocol::ManualReceipt>()); }
                        else { self->models_.git.preview(reply->result->template get<protocol::ManualPreview>()); }
                    }
                } catch (const std::exception&) {}
            } else if (id == self->git_job_ && reply->state.value == "failed") { self->models_.git.result({}); }
        });
    }
    presenter::Rpc& rpc_;
    Models& models_;
    std::optional<std::string> checking_;
    std::optional<std::string> git_job_;
    std::vector<presenter::Subscription> subscriptions_;
};
}
}

int main(int argc, char** argv) {
    ::umask(0077);
    bool rendering = false;
    for (int index = 1; index < argc; ++index) { if (std::string_view(argv[index]) == "--render-png") { rendering = true; } }
    if (rendering) { qputenv("QT_QPA_PLATFORM", "offscreen"); qputenv("TZ", "UTC"); }
    QApplication app(argc, argv);
    app.setApplicationName("braurser");
    QCommandLineParser parser;
    parser.addOption({"page", {}, "page", "discover"});
    parser.addOption({"search", {}, "text"});
    parser.addOption({"render-png", {}, "file"});
    parser.addOption({"theme", {}, "theme", "system"});
    parser.addOption({"width", {}, "width", "1024"});
    if (!parser.parse(app.arguments())) { return 2; }
    const QStringList pages{"discover", "search", "updates", "installed", "settings", "activity", "details", "git"};
    const auto page = parser.isSet("search") && !parser.isSet("page") ? QString("search") : parser.value("page");
    const auto theme = parser.value("theme");
    bool width_valid = false;
    const int width = parser.value("width").toInt(&width_valid);
    if (!pages.contains(page) || (theme != "system" && theme != "light" && theme != "dark") || !width_valid || width < 280 || width > 3840 ||
        parser.value("search").toUtf8().size() > 4096 || !parser.positionalArguments().empty()) { return 2; }
    try {
        braurser::qt::Dispatcher dispatcher;
        braurser::qt::Timer timer;
        std::unique_ptr<braurser::presenter::Rpc> rpc;
        if (rendering) { rpc = std::make_unique<braurser::qt::FakeServer>(dispatcher); }
        else { rpc = std::make_unique<braurser::presenter::RpcClient>(dispatcher); }
        braurser::qt::Models models(*rpc, timer);
        braurser::presenter::Copy copy;
        braurser::qt::DesktopPlatform platform;
        braurser::qt::Connection connection(*rpc, models);
        if (rendering) { static_cast<braurser::qt::FakeServer&>(*rpc).populate(models); }
        models.shell.theme(theme.toStdString(), {});
        if (parser.isSet("search")) { models.search.query(parser.value("search").toStdString()); }
        if (rendering) {
            braurser::protocol::SettingsPatch patch;
            patch.theme = braurser::protocol::Theme{theme.toStdString()};
            models.settings.apply(patch);
        }
        braurser::qt::Window window(models, copy, platform);
        window.navigate(page.toStdString());
        if (rendering && page == "details") { models.details.load("krita"); models.details.load_wiki(false); }
        if (rendering && page == "activity") { models.activity.attach("fixture-active"); }
        window.resize(width, rendering ? 640 : width < 480 ? 640 : width < 840 ? 720 : 768);
        window.show();
        if (rendering) {
            for (int index = 0; index < 40; ++index) { app.processEvents(); }
            window.render();
            for (int index = 0; index < 10; ++index) { app.processEvents(); }
            if (auto* focused = QApplication::focusWidget()) { focused->clearFocus(); }
            const auto image = window.grab();
            if (image.size() != QSize(width, 640)) { return 1; }
            return image.save(parser.value("render-png"), "PNG") ? 0 : 1;
        }
        return app.exec();
    } catch (const std::exception&) { return 1; }
}
