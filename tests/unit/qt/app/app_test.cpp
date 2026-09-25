#include "app.hpp"
#include "fixtures.hpp"
#include "core.hpp"
#include "dialog.hpp"
#include <QApplication>
#include <QDialog>
#include <QProcess>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QUrl>
#include <gtest/gtest.h>
#include <algorithm>
#include <thread>

namespace braurser::qt {
namespace {
class PlatformStub final : public Platform {
public:
    std::vector<QUrl> urls;
    std::vector<std::string> desktops;
    unsigned reboots = 0;
    bool open_url(const QUrl& url) override { urls.push_back(url); return true; }
    bool open_desktop(std::string_view id) override { desktops.emplace_back(id); return true; }
    void reboot(std::function<void(bool)> completed) override { ++reboots; completed(false); }
};
void pump() { for (int index = 0; index < 25; ++index) { QApplication::processEvents(); } }
QString contents(const QWidget& window) {
    QStringList result;
    for (auto* value : window.findChildren<QLabel*>()) { result.push_back(value->text()); }
    for (auto* value : window.findChildren<QAbstractButton*>()) { result.push_back(value->accessibleName()); result.push_back(value->text()); }
    return result.join('\n');
}
QPushButton* action(QWidget& window, std::string_view label) {
    for (auto* value : window.findChildren<QPushButton*>()) { if (value->accessibleName() == kit::safe_text(label)) { return value; } }
    return nullptr;
}
struct Screen {
    Dispatcher dispatcher;
    Timer timer;
    FakeServer server;
    Models models;
    presenter::Copy copy{"en", presenter::MissingCopy::Throw};
    PlatformStub platform;
    Window window;
    explicit Screen(FakeServer::Mode mode = FakeServer::Mode::Populated)
        : server(dispatcher, mode), models(server, timer), window(models, copy, platform) {
        server.populate(models); window.show(); pump();
    }
    void page(std::string_view name) { window.navigate(name); pump(); }
    bool has(std::string_view key) const { return contents(window).contains(kit::safe_text(copy.text(key))); }
};
}

TEST(QtApp, QueuedDispatcherRunsOnUiThread) {
    Dispatcher dispatcher;
    QThread* executed = nullptr;
    std::jthread worker([&] { dispatcher.post([&] { executed = QThread::currentThread(); }); });
    worker.join();
    EXPECT_EQ(executed, nullptr);
    pump();
    EXPECT_EQ(executed, QApplication::instance()->thread());
}

TEST(QtApp, TimerCanCancelAndDebounce) {
    Timer timer;
    unsigned called = 0;
    const auto token = timer.after(std::chrono::milliseconds(0), [&] { ++called; });
    timer.cancel(token); pump(); EXPECT_EQ(called, 0U);
    timer.after(std::chrono::milliseconds(0), [&] { ++called; }); pump(); EXPECT_EQ(called, 1U);
}

TEST(QtApp, DiscoverPopulatedEmptyAndOffline) {
    Screen populated;
    EXPECT_TRUE(populated.has("discover.categories"));
    EXPECT_TRUE(populated.has("discover.available"));
    EXPECT_TRUE(populated.has("discover.git"));
    populated.models.discover.collection(2, {}, {}); pump();
    EXPECT_FALSE(populated.has("discover.available"));
    populated.models.discover.availability(true, true, true); pump();
    EXPECT_TRUE(populated.has("offline.local_ready"));
    EXPECT_TRUE(populated.has("discover.categories"));
}

TEST(QtApp, DiscoverUsesAppCardsAndContentWidthGrid) {
    Screen screen;
    auto* grid = screen.window.findChild<QWidget*>("available-apps");
    ASSERT_NE(grid, nullptr);
    ASSERT_EQ(grid->layout()->count(), 1);
    EXPECT_NE(dynamic_cast<kit::AppCard*>(grid->layout()->itemAt(0)->widget()), nullptr);
    EXPECT_TRUE(contents(*grid).contains("Krita"));
    EXPECT_TRUE(contents(*grid).contains("Create digital paintings"));
    const std::vector<std::string> ids{"krita", "drawing-bin", "drawing-git", "fourth", "fifth"};
    screen.models.discover.collection(2, ids, ids); pump();
    grid = screen.window.findChild<QWidget*>("available-apps");
    ASSERT_NE(grid, nullptr);
    auto* layout = grid->layout();
    for (const int width : {360, 479, 599, 600, 899, 900, 1199, 1200, 1600}) {
        layout->setGeometry(QRect(0, 0, width, layout->heightForWidth(width)));
        const int columns = static_cast<int>(presenter::app_grid_columns(static_cast<unsigned>(width)));
        EXPECT_GE(layout->itemAt(0)->geometry().width(), 240);
        EXPECT_EQ(layout->itemAt(columns - 1)->geometry().top(), layout->itemAt(0)->geometry().top());
        EXPECT_GT(layout->itemAt(columns)->geometry().top(), layout->itemAt(0)->geometry().top());
    }
}

TEST(QtApp, SearchToolbarWrapsAndCommunityIsNotForeign) {
    Screen screen;
    screen.window.resize(360, 640); screen.page("search");
    auto* toolbar = screen.window.findChild<QWidget*>("search-toolbar");
    ASSERT_NE(toolbar, nullptr);
    EXPECT_LE(toolbar->width(), 360);
    auto* entry = screen.window.findChild<QLineEdit*>("search-query");
    ASSERT_NE(entry, nullptr); EXPECT_TRUE(entry->isClearButtonEnabled());
    EXPECT_LT(entry->mapToGlobal(QPoint()).y(), toolbar->mapToGlobal(QPoint()).y());
    EXPECT_TRUE(screen.has("source.community"));
    EXPECT_FALSE(screen.has("source.foreign"));
    EXPECT_EQ(action(screen.window, screen.copy.text("action.clear_search")), nullptr);
    auto* filters = action(screen.window, screen.copy.text("filter.count", {{"count", std::int64_t{1}}}));
    auto* sort = toolbar->findChild<QComboBox*>();
    ASSERT_NE(filters, nullptr); ASSERT_NE(sort, nullptr);
    EXPECT_EQ(filters->mapToGlobal(QPoint()).y(), sort->mapToGlobal(QPoint()).y());
    EXPECT_LT(filters->mapToGlobal(QPoint()).x(), sort->mapToGlobal(QPoint()).x());
}

TEST(QtApp, AcknowledgementKeepsEightPixelLabelSpacing) {
    Screen screen; screen.page("updates");
    QCheckBox* acknowledgement = nullptr;
    for (auto* check : screen.window.findChildren<QCheckBox*>()) {
        if (check->accessibleName() == kit::safe_text(screen.copy.text("updates.news_ack"))) { acknowledgement = check; }
    }
    ASSERT_NE(acknowledgement, nullptr);
    EXPECT_EQ(acknowledgement->width(), 44);
    EXPECT_GE(acknowledgement->height(), 44);
    EXPECT_EQ(acknowledgement->layoutDirection(), Qt::RightToLeft);
    EXPECT_EQ(acknowledgement->parentWidget()->layout()->spacing(), 8);
}

TEST(QtApp, SearchPopulatedEmptyLoadingAndError) {
    for (const auto mode : {FakeServer::Mode::Populated, FakeServer::Mode::Empty, FakeServer::Mode::Loading, FakeServer::Mode::Error}) {
        Screen screen(mode); screen.page("search");
        EXPECT_NE(screen.window.findChild<QLineEdit*>("search-query"), nullptr);
        if (mode == FakeServer::Mode::Populated) { EXPECT_TRUE(contents(screen.window).contains("krita")); }
        if (mode == FakeServer::Mode::Empty) { EXPECT_TRUE(screen.has("search.empty_body")); }
        if (mode == FakeServer::Mode::Loading) { EXPECT_TRUE(screen.has("state.loading")); }
        if (mode == FakeServer::Mode::Error) { EXPECT_TRUE(screen.has("offline.operation_block")); }
    }
}

TEST(QtApp, CategoryIsSentToCatalog) {
    Screen screen;
    presenter::FilterSelection selection;
    selection.category = "Graphics";
    screen.models.search.filters(selection);
    screen.models.shell.navigate({presenter::Destination::Search, "Graphics", 0}); pump();
    EXPECT_TRUE(screen.has("category.Graphics"));
    EXPECT_FALSE(screen.models.search.state().error);
    EXPECT_EQ(screen.models.search.state().selection.category, "Graphics");
}

TEST(QtApp, FiltersAreNativeAndApplyWithoutTransaction) {
    Screen screen; screen.page("search");
    auto* filters = action(screen.window, screen.copy.text("filter.count", {{"count", std::int64_t{1}}})); ASSERT_NE(filters, nullptr); filters->click(); pump();
    auto* dialog = screen.window.findChild<QDialog*>(); ASSERT_NE(dialog, nullptr);
    EXPECT_TRUE(contents(*dialog).contains(kit::safe_text(screen.copy.text("filter.orphans_help"))));
    auto* apply = action(*dialog, screen.copy.text("filter.apply")); ASSERT_NE(apply, nullptr); apply->click(); pump();
    EXPECT_TRUE(std::ranges::none_of(screen.server.requests, [](const auto& request) { return request.first == "jobs.install"; }));
}

TEST(QtApp, DetailsWikiAttributionAndExternalSearchRequireActivation) {
    Screen screen;
    screen.models.details.load("krita"); screen.models.details.load_wiki(false);
    screen.models.shell.navigate({presenter::Destination::Details, "krita", 0}); pump();
    EXPECT_TRUE(screen.has("detail.about"));
    auto* icon = screen.window.findChild<QWidget*>("details-icon");
    ASSERT_NE(icon, nullptr); EXPECT_EQ(icon->size(), QSize(64, 64));
    EXPECT_TRUE(contents(*icon->parentWidget()).contains("Krita"));
    const auto primary = kit::safe_text(screen.models.details.state().primary.render(screen.copy));
    EXPECT_EQ(std::ranges::count_if(screen.window.findChildren<QPushButton*>(), [&primary](const auto* button) { return button->accessibleName() == primary; }), 1);
    EXPECT_TRUE(screen.has("wiki.attribution"));
    EXPECT_FALSE(screen.window.findChildren<kit::DocumentView*>().empty());
    EXPECT_TRUE(screen.platform.urls.empty());
    auto* search = action(screen.window, screen.copy.text("wiki.search")); ASSERT_NE(search, nullptr); search->click();
    ASSERT_EQ(screen.platform.urls.size(), 1U);
    EXPECT_EQ(screen.platform.urls.front().host(), "wiki.archlinux.org");
    EXPECT_TRUE(screen.platform.urls.front().query().contains("krita"));
    screen.models.details.offline(true); pump();
    EXPECT_TRUE(contents(screen.window).contains("Showing a saved article"));
    EXPECT_TRUE(screen.has("wiki.attribution"));
}

TEST(QtApp, DetailsLoadingAndErrorDoNotInventActions) {
    for (const auto mode : {FakeServer::Mode::Loading, FakeServer::Mode::Error}) {
        Screen screen(mode); screen.models.details.load("krita");
        screen.models.shell.navigate({presenter::Destination::Details, "krita", 0}); pump();
        EXPECT_TRUE(screen.has(mode == FakeServer::Mode::Loading ? "state.loading" : "offline.operation_block"));
        EXPECT_EQ(action(screen.window, screen.copy.text("action.install")), nullptr);
    }
}

TEST(QtApp, UpdatesNeverOfferPostponeOrOfficialItemUpdates) {
    Screen screen; screen.page("updates");
    EXPECT_TRUE(screen.has("updates.all"));
    EXPECT_TRUE(screen.has("updates.system_first"));
    EXPECT_FALSE(contents(screen.window).contains("postpone", Qt::CaseInsensitive));
    const auto rows = screen.window.findChildren<QWidget*>(QRegularExpression("update-row-.*"));
    ASSERT_EQ(rows.size(), 6);
    unsigned per_item = 0;
    for (auto* row : rows) {
        auto* update = row->findChild<QPushButton*>("update-item");
        if (row->objectName() == "update-row-drawing-bin" || row->objectName() == "update-row-fixture-git") {
            ASSERT_NE(update, nullptr); EXPECT_FALSE(update->isEnabled()); ++per_item;
        } else { EXPECT_EQ(update, nullptr); }
    }
    EXPECT_EQ(per_item, 2U);
    EXPECT_EQ(screen.window.findChildren<QPushButton*>("update-system").size(), 1);
}

TEST(QtApp, UpdatesHierarchyKeepsHeaderNoticeAndGroupedCards) {
    Screen screen; screen.page("updates");
    auto* header = screen.window.findChild<QWidget*>("page-header");
    ASSERT_NE(header, nullptr);
    for (const auto* key : {"nav.back", "nav.activity", "action.check_again"}) {
        EXPECT_NE(action(*header, screen.copy.text(key)), nullptr);
    }
    unsigned warnings = 0;
    unsigned cards = 0;
    for (auto* frame : screen.window.findChildren<QFrame*>()) {
        if (frame->property("brRole") == "warning" && frame->property("brBanner").toBool()) {
            ++warnings;
            EXPECT_EQ(frame->findChildren<QCheckBox*>().size(), 1);
            EXPECT_NE(action(*frame, screen.copy.text("updates.news_read")), nullptr);
        }
        if (frame->property("brRole") == "card") { ++cards; }
    }
    EXPECT_EQ(warnings, 1U); EXPECT_EQ(cards, 5U);
    auto* primary = screen.window.findChild<QPushButton*>("update-system");
    ASSERT_NE(primary, nullptr);
    EXPECT_EQ(primary->property("brRole"), "primary");
    EXPECT_FALSE(primary->isEnabled());
}

TEST(QtApp, UpdatesEmptyLoadingErrorAndNewsAcknowledgement) {
    for (const auto mode : {FakeServer::Mode::Empty, FakeServer::Mode::Loading, FakeServer::Mode::Error}) {
        Screen screen(mode); screen.page("updates");
        EXPECT_TRUE(screen.has(mode == FakeServer::Mode::Empty ? "updates.current_title" :
            mode == FakeServer::Mode::Loading ? "updates.checking" : "offline.operation_block"));
    }
    Screen screen;
    auto report = *screen.models.updates.state().report;
    report.news.clear();
    report.news.push_back({"Manual intervention", "https://archlinux.org/news/", "2026-09-25T12:00:00Z", true, false});
    screen.models.updates.report(report); screen.page("updates");
    ASSERT_NE(action(screen.window, screen.copy.text("updates.all")), nullptr);
    EXPECT_FALSE(action(screen.window, screen.copy.text("updates.all"))->isEnabled());
    screen.models.updates.acknowledge_news(0, true); pump();
    EXPECT_TRUE(action(screen.window, screen.copy.text("updates.all"))->isEnabled());
}

TEST(QtApp, InstalledFourOwnershipViewsAndEmptyStates) {
    Screen screen; screen.page("installed");
    EXPECT_TRUE(contents(screen.window).contains("drawing-bin"));
    screen.models.installed.view(presenter::InstalledView::Git); pump();
    EXPECT_TRUE(screen.has("confirm.removal_unavailable"));
    screen.models.installed.view(presenter::InstalledView::Outside); pump();
    EXPECT_TRUE(screen.has("installed.outside_notice"));
    EXPECT_EQ(action(screen.window, screen.copy.text("action.remove")), nullptr);
    auto* open = action(screen.window, screen.copy.text("action.open")); ASSERT_NE(open, nullptr); open->click();
    EXPECT_EQ(screen.platform.desktops, std::vector<std::string>{"local-notes.desktop"});
    screen.models.installed.view(presenter::InstalledView::Components); pump();
    EXPECT_TRUE(screen.has("error.unsupported_body"));
    Screen empty(FakeServer::Mode::Empty); empty.page("installed");
    EXPECT_TRUE(empty.has("installed.empty_apps"));
    empty.models.installed.view(presenter::InstalledView::Git); pump(); EXPECT_TRUE(empty.has("installed.empty_git"));
    empty.models.installed.view(presenter::InstalledView::Outside); pump(); EXPECT_TRUE(empty.has("installed.empty_outside"));
}

TEST(QtApp, InstalledLoadingAndError) {
    Screen loading(FakeServer::Mode::Loading); loading.page("installed"); EXPECT_TRUE(loading.has("state.loading"));
    Screen failed(FakeServer::Mode::Error); failed.page("installed"); EXPECT_TRUE(failed.has("offline.operation_block"));
}

TEST(QtApp, SettingsImmediateApplyAndNoSave) {
    Screen screen; screen.page("settings");
    EXPECT_TRUE(screen.has("settings.community"));
    EXPECT_TRUE(screen.has("settings.appearance"));
    EXPECT_FALSE(contents(screen.window).contains("Save\n"));
    QCheckBox* notifications = nullptr;
    for (auto* item : screen.window.findChildren<QCheckBox*>()) {
        if (item->accessibleName() == kit::safe_text(screen.copy.text("settings.notifications"))) { notifications = item; }
    }
    ASSERT_NE(notifications, nullptr);
    notifications->setChecked(false); pump();
    ASSERT_TRUE(screen.models.settings.state().values);
    EXPECT_FALSE(screen.models.settings.state().values->notifications);
    Screen loading(FakeServer::Mode::Loading); loading.page("settings"); EXPECT_TRUE(loading.has("state.loading"));
    Screen error(FakeServer::Mode::Error); error.page("settings"); EXPECT_TRUE(error.has("offline.operation_block"));
}

TEST(QtApp, ActivityEmptyErrorAndMeasuredProgress) {
    Screen screen; screen.page("activity"); EXPECT_TRUE(screen.has("activity.recent"));
    screen.models.activity.attach("job"); pump();
    screen.server.emit_event("job.progress", protocol::JobProgressNotification{"job", 1, {{"building"}, 2, 10, {"items"}}}); pump();
    auto bars = screen.window.findChildren<QProgressBar*>(); ASSERT_FALSE(bars.empty()); EXPECT_EQ(bars.front()->maximum(), 0);
    screen.server.emit_event("job.progress", protocol::JobProgressNotification{"job", 2, {{"downloading"}, 2, 10, {"items"}}}); pump();
    bars = screen.window.findChildren<QProgressBar*>(); ASSERT_FALSE(bars.empty()); EXPECT_EQ(bars.front()->value(), 200);
    Screen failed(FakeServer::Mode::Error); failed.page("activity"); EXPECT_TRUE(failed.has("offline.operation_block"));
}

TEST(QtApp, GitUrlRejectsUnsafeInputAndNeverSubmitsImplicitly) {
    Screen screen; screen.page("git");
    EXPECT_TRUE(screen.has("git.url_help"));
    auto* field = screen.window.findChild<QLineEdit*>("git-url"); ASSERT_NE(field, nullptr);
    field->setText("file:///untrusted");
    auto* next = action(screen.window, screen.copy.text("action.next")); ASSERT_NE(next, nullptr); next->click(); pump();
    EXPECT_TRUE(screen.has("git.url_invalid"));
    EXPECT_TRUE(std::ranges::none_of(screen.server.requests, [](const auto& request) { return request.first == "manual.install"; }));
}

TEST(QtApp, GitReviewScopeProgressAndFailureBindPresenter) {
    Screen screen; screen.page("git");
    screen.models.git.inspect("https://example.org/team/canvas", "install.sh", "user"); pump();
    protocol::ManualPreview preview{"preview", "example.org", "team/canvas", std::string(40, 'a'), "install.sh", "printf ready\\n", {{"user"}}, {}, false};
    screen.models.git.preview(preview); pump();
    EXPECT_TRUE(screen.has("git.risk")); EXPECT_TRUE(screen.has("git.review_ack"));
    screen.models.git.acknowledge(true); screen.models.git.next(); pump();
    EXPECT_TRUE(screen.has("git.user_body")); EXPECT_TRUE(screen.has("git.scope_unavailable"));
    auto* install = action(screen.window, screen.copy.text("git.install_user")); ASSERT_NE(install, nullptr);
    EXPECT_TRUE(install->isEnabled());
    screen.models.git.install(); pump();
    EXPECT_EQ(screen.models.git.state().step, presenter::GitStep::Progress);
    screen.models.git.result({}); pump(); EXPECT_TRUE(screen.has("git.cleanup_unknown"));
    EXPECT_TRUE(screen.has("action.retry"));
    protocol::ManualReceipt receipt{"canvas", "Canvas", {"user"}, std::string(40, 'a'), "2026-09-25T12:00:00Z", {}, false};
    screen.models.git.result(receipt); pump(); EXPECT_TRUE(screen.has("git.no_launcher"));
    EXPECT_EQ(action(screen.window, screen.copy.text("action.open")), nullptr);
}

TEST(QtApp, VariantChoiceIsExplicitAndConfirmRequiresDependentAcknowledgement) {
    Screen screen;
    screen.models.details.load("krita"); screen.models.details.variants(); pump();
    screen.window.show_variants(); pump();
    auto* variants = screen.window.findChild<QDialog*>(); ASSERT_NE(variants, nullptr);
    EXPECT_TRUE(contents(*variants).contains(kit::safe_text(screen.copy.text("variant.body"))));
    EXPECT_EQ(screen.models.variants.state().selected, 0U);
    variants->reject(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); pump();
    protocol::TransactionPlan plan;
    plan.plan_id = "fixture-plan"; plan.approval_token = "fixture-token"; plan.revision = 1; plan.kind.value = "remove";
    plan.removals.push_back({{"official"}, "extra", "krita"});
    screen.models.confirm.plan(plan, true);
    screen.window.show_confirm(); pump();
    auto* confirm = screen.window.findChild<QDialog*>(); ASSERT_NE(confirm, nullptr);
    EXPECT_TRUE(contents(*confirm).contains(kit::safe_text(screen.copy.text("confirm.remove_dependents_ack"))));
    auto* remove = action(*confirm, screen.copy.text("action.remove")); ASSERT_NE(remove, nullptr);
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_FALSE(contents(*confirm).contains(kit::safe_text(screen.copy.text("confirm.keep_files"))));
}

TEST(QtApp, RestartRequiresConfirmationAndReportsFailure) {
    Screen screen;
    screen.window.restart(); pump();
    EXPECT_EQ(screen.platform.reboots, 0U);
    auto* dialog = screen.window.findChild<QDialog*>(); ASSERT_NE(dialog, nullptr);
    auto* restart = action(*dialog, screen.copy.text("restart.now")); ASSERT_NE(restart, nullptr); restart->click(); pump();
    EXPECT_EQ(screen.platform.reboots, 1U);
    bool failed = false;
    for (auto* widget : QApplication::topLevelWidgets()) { failed = failed || contents(*widget).contains(kit::safe_text(screen.copy.text("restart.failed"))); }
    EXPECT_TRUE(failed);
}

TEST(QtApp, BadgesStayCompactAndDesktopIdsFailClosed) {
    Screen screen; screen.page("search");
    unsigned badges = 0;
    for (const auto* widget : screen.window.findChildren<QWidget*>()) {
        if (const auto* badge = dynamic_cast<const kit::SourceBadge*>(widget)) {
            ++badges; EXPECT_EQ(badge->sizePolicy().horizontalPolicy(), QSizePolicy::Maximum);
        }
    }
    EXPECT_GT(badges, 0U);
    EXPECT_TRUE(valid_desktop_id("org.kde.krita.desktop"));
    for (const auto* id : {"../evil.desktop", "/usr/share/evil.desktop", "app.desktop;id", "app", "a\nb.desktop", "-bad/evil.desktop"}) { EXPECT_FALSE(valid_desktop_id(id)); }
}

TEST(QtApp, NarrowPagesKeepViewportWidth) {
    Screen screen;
    screen.window.resize(360, 640);
    for (const auto* page : {"discover", "search", "updates", "installed", "settings", "activity", "git"}) {
        screen.page(page);
        EXPECT_EQ(screen.window.width(), 360) << page;
        EXPECT_FALSE(dynamic_cast<kit::ResponsiveShell*>(screen.window.centralWidget())->sidebar_visible()) << page;
    }
}

TEST(QtApp, AskpassWritesExactlyOneLineAndWipesOnEveryOutcome) {
    std::vector<char> password{'s', 'e', 'c', 'r', 'e', 't'};
    std::string output;
    ASSERT_TRUE(askpass::write_password(password, [&](std::span<const char> bytes) { output.assign(bytes.begin(), bytes.end()); return true; }));
    EXPECT_EQ(output, "secret\n");
    EXPECT_TRUE(std::ranges::all_of(password, [](char value) { return value == 0; }));
    password.assign({'x', '\n', 'y'});
    EXPECT_FALSE(askpass::write_password(password, [](auto) { ADD_FAILURE(); return true; }));
    EXPECT_TRUE(std::ranges::all_of(password, [](char value) { return value == 0; }));
    password.assign(5000, 's');
    EXPECT_FALSE(askpass::write_password(password, [](auto) { ADD_FAILURE(); return true; }));
    EXPECT_TRUE(std::ranges::all_of(password, [](char value) { return value == 0; }));
    password.assign(4, 's');
    EXPECT_FALSE(askpass::write_password(password, [](auto) -> bool { throw std::runtime_error("write failed"); }));
    EXPECT_TRUE(std::ranges::all_of(password, [](char value) { return value == 0; }));
}

TEST(QtApp, AskpassUsesOnlyAllowlistedActionCodes) {
    EXPECT_EQ(askpass::action("install"), presenter::AuthAction::Install);
    EXPECT_EQ(askpass::action("remove"), presenter::AuthAction::Remove);
    EXPECT_EQ(askpass::action("git"), presenter::AuthAction::Git);
    EXPECT_EQ(askpass::action("update"), presenter::AuthAction::Update);
    EXPECT_EQ(askpass::action("Enter your banking password"), presenter::AuthAction::Update);
    EXPECT_EQ(askpass::action(""), presenter::AuthAction::Update);
    EXPECT_TRUE(askpass::current_account().has_value());
}

TEST(QtApp, AskpassDialogClearsOnSubmitCancelCloseTimeoutAndAccountChange) {
    class Channel final : public presenter::CredentialChannel {
    public:
        std::string received;
        unsigned submissions = 0;
        bool submit(const presenter::AuthBinding&, std::string_view, std::span<const char> bytes) override {
            ++submissions; received.assign(bytes.begin(), bytes.end()); return true;
        }
        void cancel(const presenter::AuthBinding&) override {}
    };
    for (const auto* outcome : {"submit", "cancel", "close", "timeout", "account"}) {
        Channel channel;
        Timer timer;
        presenter::Auth auth(channel, timer);
        presenter::Copy copy;
        auth.bind({"operation", "challenge", presenter::AuthAction::Update, {}, {"account", "other"},
            std::chrono::steady_clock::now() + std::chrono::minutes(2), false, true});
        askpass::Dialog dialog(auth, copy);
        dialog.show(); pump();
        auto* field = dialog.findChild<QLineEdit*>(); ASSERT_NE(field, nullptr);
        field->setText("secret");
        if (std::string_view(outcome) == "submit") {
            auto* submit = action(dialog, copy.text("auth.authenticate")); ASSERT_NE(submit, nullptr); submit->click();
        } else if (std::string_view(outcome) == "cancel") {
            auto* cancel = action(dialog, copy.text("action.cancel")); ASSERT_NE(cancel, nullptr); cancel->click();
        } else if (std::string_view(outcome) == "close") { dialog.close(); }
        else if (std::string_view(outcome) == "timeout") { auth.expire(); }
        else { auth.account("other"); }
        pump();
        EXPECT_TRUE(field->text().isEmpty()) << outcome;
        EXPECT_TRUE(std::ranges::all_of(auth.buffer(), [](char value) { return value == 0; })) << outcome;
        EXPECT_FALSE(auth.state().active) << outcome;
        EXPECT_EQ(channel.submissions, std::string_view(outcome) == "submit" ? 1U : 0U) << outcome;
        if (channel.submissions) { EXPECT_EQ(channel.received, "secret"); EXPECT_EQ(dialog.result(), QDialog::Accepted); }
        else { EXPECT_EQ(dialog.result(), QDialog::Rejected); }
    }
}

TEST(QtApp, AskpassIgnoresArgvPromptIncludingQtOptions) {
    QProcess process;
    process.setProgram(QString::fromUtf8(BRAURSER_QT_ASKPASS));
    process.setArguments({"--version", "--help", "-platform", "invalid"});
    process.start();
    ASSERT_TRUE(process.waitForStarted(3000));
    EXPECT_FALSE(process.waitForFinished(300));
    EXPECT_TRUE(process.readAllStandardOutput().isEmpty());
    process.kill();
    EXPECT_TRUE(process.waitForFinished(3000));
    EXPECT_TRUE(process.readAllStandardOutput().isEmpty());
}

}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
