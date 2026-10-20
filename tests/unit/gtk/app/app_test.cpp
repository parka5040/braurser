#include <braurser/gtk/application.hpp>
#include "../display.hpp"
#include <presenter/fixtures.hpp>

using braurser::presenter::fixtures::FakeServer;
#include "../../../../src/askpass/gtk/core.cpp"
#include <gtest/gtest.h>
#include <thread>

namespace braurser::gtk {
namespace {
class TestPlatform final : public Platform {
public:
    bool open_desktop(std::string_view id) override { opened = id; return valid_desktop_id(id); }
    bool open_link(std::string_view uri) override { opened = uri; return kit::allowed_link(uri); }
    void reboot(std::function<void(bool)> complete) override { complete(false); }
    std::string opened;
};
std::string contents(Gtk::Widget& widget) {
    std::string text;
    if (auto* label = dynamic_cast<Gtk::Label*>(&widget)) { text += label->get_text().raw() + '\n'; }
    if (auto* entry = dynamic_cast<Gtk::Entry*>(&widget)) { text += entry->get_text().raw() + '\n'; }
    for (auto* child = widget.get_first_child(); child; child = child->get_next_sibling()) { text += contents(*child); }
    return text;
}
std::size_t buttons(Gtk::Widget& widget, const std::string& text) {
    std::size_t count = 0;
    if (dynamic_cast<Gtk::Button*>(&widget) && contents(widget) == text + '\n') { ++count; }
    for (auto* child = widget.get_first_child(); child; child = child->get_next_sibling()) { count += buttons(*child, text); }
    return count;
}
template<class T> std::vector<T*> widgets(Gtk::Widget& root) {
    std::vector<T*> result;
    if (auto* widget = dynamic_cast<T*>(&root)) { result.push_back(widget); }
    for (auto* child = root.get_first_child(); child; child = child->get_next_sibling()) {
        auto nested = widgets<T>(*child);
        result.insert(result.end(), nested.begin(), nested.end());
    }
    return result;
}
class Screens : public testing::TestWithParam<std::tuple<p::Destination, FakeServer::Mode>> {};
TEST_P(Screens, BindsPresenterCopyAndStates) {
    if (!test::display_available()) { GTEST_SKIP() << "No GTK display; run render-headless.sh for Broadway coverage"; }
    const auto application = Gtk::Application::create({}, Gio::Application::Flags::NON_UNIQUE);
    application->register_application();
    MainDispatcher dispatcher; MainTimer timer; TestPlatform platform; p::Copy copy("en", p::MissingCopy::Throw);
    const auto [page, mode] = GetParam();
    FakeServer server(dispatcher, mode);
    Models models(server, timer);
    models.shell.navigate({page, page == p::Destination::Details ? "krita" : "", 0});
    AppWindow window(models, copy, platform);
    application->add_window(window);
    window.refresh();
    for (int i = 0; i < 5; ++i) { dispatcher.drain(); }
    window.render();
    window.set_decorated(false);
    int minimum = 0;
    int natural = 0;
    int baseline = -1;
    int natural_baseline = -1;
    window.measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, baseline, natural_baseline);
    EXPECT_LE(minimum, 360);
    window.measure(Gtk::Orientation::VERTICAL, 360, minimum, natural, baseline, natural_baseline);
    EXPECT_LE(minimum, 640);
    const auto text = contents(window);
    const auto title = page == p::Destination::Details ? copy.text("action.details") : page == p::Destination::Git ? copy.text("discover.git") : copy.text("nav." + page_id(page));
    EXPECT_EQ(window.get_title(), copy.text("window.title", {{"page", title}}));
    EXPECT_NE(text.find(title), std::string::npos);
    if (mode == FakeServer::Mode::Error && (page == p::Destination::Search || page == p::Destination::Installed || page == p::Destination::Details || page == p::Destination::Activity)) {
        EXPECT_NE(text.find(copy.text("offline.operation_block")), std::string::npos);
    }
    if (mode == FakeServer::Mode::Loading && (page == p::Destination::Search || page == p::Destination::Installed || page == p::Destination::Details)) {
        EXPECT_NE(text.find(copy.text("state.loading")), std::string::npos);
    }
    if (mode == FakeServer::Mode::Empty && page == p::Destination::Installed) { EXPECT_NE(text.find(copy.text("installed.empty_apps")), std::string::npos); }
    if (mode == FakeServer::Mode::Empty && page == p::Destination::Search) { EXPECT_NE(text.find(copy.text("search.empty_body")), std::string::npos); }
    if (mode == FakeServer::Mode::Populated && page == p::Destination::Search) { EXPECT_NE(text.find("krita"), std::string::npos); }
    if (mode == FakeServer::Mode::Populated && page == p::Destination::Details) {
        EXPECT_NE(text.find(copy.text("wiki.attribution")), std::string::npos);
        const auto all = widgets<Gtk::Widget>(window);
        const auto found = std::ranges::find_if(all, [](const auto* widget) { return widget->get_name() == "details-icon"; });
        ASSERT_NE(found, all.end());
        (*found)->measure(Gtk::Orientation::HORIZONTAL, -1, minimum, natural, baseline, natural_baseline);
        EXPECT_EQ(minimum, 64); EXPECT_EQ(natural, 64);
        EXPECT_NE(contents(*(*found)->get_parent()).find("Krita"), std::string::npos);
    }
}
INSTANTIATE_TEST_SUITE_P(GtkApp, Screens, testing::Combine(testing::Values(p::Destination::Discover, p::Destination::Search,
    p::Destination::Updates, p::Destination::Installed, p::Destination::Settings, p::Destination::Activity,
    p::Destination::Details, p::Destination::Git), testing::Values(FakeServer::Mode::Populated, FakeServer::Mode::Empty, FakeServer::Mode::Error, FakeServer::Mode::Loading)));

TEST(GtkAppPolish, DiscoverCardsReflowAndSearchHasInlineControls) {
    if (!test::display_available()) { GTEST_SKIP() << "No GTK display"; }
    const auto application = Gtk::Application::create({}, Gio::Application::Flags::NON_UNIQUE);
    application->register_application();
    MainDispatcher dispatcher; MainTimer timer; TestPlatform platform; p::Copy copy;
    FakeServer server(dispatcher); Models models(server, timer);
    server.populate(models); dispatcher.drain();
    AppWindow window(models, copy, platform); application->add_window(window); window.render();
    auto cards = widgets<kit::AppCard>(window);
    ASSERT_EQ(cards.size(), 1U);
    EXPECT_NE(contents(*cards.front()).find("Krita"), std::string::npos);
    EXPECT_NE(contents(*cards.front()).find("Create digital paintings"), std::string::npos);
    const std::vector<std::string> ids{"krita", "drawing-bin", "drawing-git", "fourth", "fifth"};
    models.discover.collection(2, ids, ids, p::fixtures::data().packages); window.render();
    cards = widgets<kit::AppCard>(window);
    ASSERT_EQ(cards.size(), 5U);
    auto* grid = cards.front()->get_parent();
    ASSERT_EQ(grid->get_name(), "available-apps");
    for (const int width : {360, 599, 600, 899, 900, 1199, 1200, 1600, 360}) {
        grid->size_allocate(Gtk::Allocation(0, 0, width, 1200), -1);
        const auto columns = p::app_grid_columns(static_cast<unsigned>(width));
        EXPECT_GE(cards.front()->get_allocation().get_width(), 240);
        EXPECT_EQ(cards[columns - 1]->get_allocation().get_y(), cards.front()->get_allocation().get_y());
        EXPECT_GT(cards[columns]->get_allocation().get_y(), cards.front()->get_allocation().get_y());
    }
    models.shell.navigate({p::Destination::Search, {}, 0}); window.render();
    EXPECT_EQ(buttons(window, copy.text("action.clear_search")), 0U);
    EXPECT_EQ(buttons(window, copy.text("filter.count", {{"count", std::int64_t{1}}})), 1U);
    EXPECT_EQ(widgets<kit::FilterChip>(window).size(), 1U);
    const auto entries = widgets<Gtk::Entry>(window);
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries.front()->get_icon_name(Gtk::Entry::IconPosition::SECONDARY), "edit-clear-symbolic");
    EXPECT_NE(contents(window).find(copy.text("source.community")), std::string::npos);
    EXPECT_EQ(contents(window).find(copy.text("source.foreign")), std::string::npos);
}

TEST(GtkAppPolish, SearchWindowShowsPreviousNextAndOnlyTwoHundredRows) {
    if (!test::display_available()) { GTEST_SKIP() << "No GTK display"; }
    const auto application = Gtk::Application::create({}, Gio::Application::Flags::NON_UNIQUE);
    application->register_application();
    MainDispatcher dispatcher; MainTimer timer; TestPlatform platform; p::Copy copy;
    FakeServer server(dispatcher); Models models(server, timer);
    protocol::SearchResult page;
    page.packages.assign(50, p::fixtures::data().packages.front()); page.nextCursor = "next";
    server.reply("catalog.search", page);
    models.shell.navigate({p::Destination::Search, {}, 0}); models.search.submit(); dispatcher.drain();
    AppWindow window(models, copy, platform); application->add_window(window);
    for (int index = 0; index < 4; ++index) {
        models.search.next_page(); dispatcher.drain(); window.render();
    }
    EXPECT_EQ(widgets<kit::AppCard>(window).size(), 200U);
    EXPECT_EQ(buttons(window, copy.text("search.next")), 1U);
    EXPECT_EQ(buttons(window, copy.text("search.previous")), 1U);
    EXPECT_EQ(models.search.state().visible_first, 51U);
    for (auto* button : widgets<Gtk::Button>(window)) {
        if (contents(*button) == copy.text("search.previous") + '\n') { g_signal_emit_by_name(button->gobj(), "clicked"); break; }
    }
    dispatcher.drain(); window.render();
    EXPECT_EQ(models.search.state().visible_first, 1U);
    EXPECT_EQ(buttons(window, copy.text("search.previous")), 0U);
    EXPECT_EQ(widgets<kit::AppCard>(window).size(), 200U);
}

TEST(GtkAppUpdates, OnlyCommunityAndGitHaveIndividualActions) {
    if (!test::display_available()) { GTEST_SKIP() << "No GTK display"; }
    const auto application = Gtk::Application::create({}, Gio::Application::Flags::NON_UNIQUE);
    application->register_application();
    MainDispatcher dispatcher; MainTimer timer; TestPlatform platform; p::Copy copy;
    FakeServer server(dispatcher); Models models(server, timer);
    models.shell.navigate({p::Destination::Updates, {}, 0}); models.updates.report(FakeServer::update_report()); dispatcher.drain();
    AppWindow window(models, copy, platform); application->add_window(window); window.render();
    EXPECT_EQ(buttons(window, copy.text("updates.all")), 1U);
    EXPECT_EQ(buttons(window, copy.text("updates.item_update")), 2U);
    EXPECT_EQ(contents(window).find("Postpone"), std::string::npos);
    EXPECT_NE(contents(window).find(copy.text("updates.system_first")), std::string::npos);
    EXPECT_NE(contents(window).find(copy.text("updates.news_ack")), std::string::npos);
    for (auto* action : widgets<Gtk::Button>(window)) {
        if (contents(*action) == copy.text("updates.all") + '\n') {
            EXPECT_TRUE(action->has_css_class("primary"));
            EXPECT_FALSE(action->get_sensitive());
        }
        if (contents(*action) == copy.text("action.check_again") + '\n') {
            EXPECT_FALSE(action->has_css_class("primary"));
            const auto headings = widgets<Gtk::Label>(*action->get_parent());
            EXPECT_TRUE(std::ranges::any_of(headings, [](const auto* value) { return value->has_css_class("page-title"); }));
        }
    }
    const auto banners = widgets<kit::Banner>(window);
    ASSERT_EQ(banners.size(), 1U);
    EXPECT_TRUE(banners.front()->has_css_class("banner-warning"));
    EXPECT_EQ(widgets<Gtk::CheckButton>(*banners.front()).size(), 1U);
    std::size_t groups = 0;
    for (auto* caption : widgets<Gtk::Label>(window)) {
        if (caption->has_css_class("section-title") && caption->get_text().find("(1)") != Glib::ustring::npos) {
            EXPECT_NE(dynamic_cast<kit::Icon*>(caption->get_parent()->get_first_child()), nullptr);
            ++groups;
        }
    }
    EXPECT_EQ(groups, FakeServer::update_report().official.size());
    std::size_t cards = 0;
    for (const auto* box : widgets<Gtk::Box>(window)) {
        if (box->has_css_class("card") && !dynamic_cast<const kit::Banner*>(box)) { ++cards; }
    }
    EXPECT_EQ(cards, 5U);
    models.updates.acknowledge_news(0, true);
    EXPECT_TRUE(models.updates.can_update());
    window.render();
    for (auto* action : widgets<Gtk::Button>(window)) {
        if (contents(*action) == copy.text("updates.all") + '\n') { EXPECT_TRUE(action->get_sensitive()); }
    }
}
TEST(GtkAppPlatform, DesktopIdsAreBoundedAndNotCommands) {
    EXPECT_TRUE(valid_desktop_id("org.kde.krita.desktop"));
    for (const auto* id : {"../evil.desktop", "/tmp/app.desktop", "app.desktop;id", "app desktop", "a..desktop", "a.desktop\n"}) { EXPECT_FALSE(valid_desktop_id(id)); }
    EXPECT_FALSE(valid_desktop_id(std::string(256, 'a') + ".desktop"));
}
TEST(GtkAppDispatcher, PostsAcrossThreadsWithoutRunningOnWorker) {
    MainDispatcher dispatcher;
    const auto owner = std::this_thread::get_id();
    bool called = false;
    std::thread worker([&] { dispatcher.post([&] { EXPECT_EQ(owner, std::this_thread::get_id()); called = true; }); });
    worker.join(); EXPECT_FALSE(called); dispatcher.drain(); EXPECT_TRUE(called);
}
TEST(GtkAppAskpass, WritesOneLineAndWipesBothBuffers) {
    std::vector<char> password{'s', 'e', 'c', 'r', 'e', 't'};
    std::vector<char> output(4097, '\0');
    std::string received;
    EXPECT_TRUE(askpass::write_password(password, output, [&](std::span<const char> bytes) { received.assign(bytes.data(), bytes.size()); return true; }));
    EXPECT_EQ(received, "secret\n");
    EXPECT_TRUE(std::ranges::all_of(password, [](char c) { return c == 0; }));
    EXPECT_TRUE(std::ranges::all_of(output, [](char c) { return c == 0; }));
}
TEST(GtkAppAskpass, FailureExceptionAndInvalidInputStillWipe) {
    for (const auto& value : {std::string("password"), std::string("line\nbreak"), std::string(4097, 'a')}) {
        std::vector<char> password(value.begin(), value.end()); std::vector<char> output(4097, '\0');
        EXPECT_FALSE(askpass::write_password(password, output, [](std::span<const char>) -> bool { throw std::runtime_error("test"); }));
        EXPECT_TRUE(std::ranges::all_of(password, [](char c) { return c == 0; }));
        EXPECT_TRUE(std::ranges::all_of(output, [](char c) { return c == 0; }));
    }
}
TEST(GtkAppAskpass, IgnoresArbitraryArgvPromptAndDefaultsUnknownAction) {
    p::Copy copy;
    for (auto prompt : {std::string("Untrusted remote prompt"), std::string("<b>Give away your password</b>"), std::string(100000, 'x')}) {
        std::array<char*, 2> argv{nullptr, prompt.data()};
        const auto binding = askpass::local_binding(2, argv.data(), "unexpected", "account", copy);
        EXPECT_EQ(binding.action, p::AuthAction::Update);
        EXPECT_EQ(binding.accounts, std::vector<std::string>{"account"});
        EXPECT_EQ(binding.app.find(prompt), std::string::npos);
        EXPECT_EQ(binding.expires > std::chrono::steady_clock::now(), true);
    }
    EXPECT_EQ(askpass::action_from_environment("install"), p::AuthAction::Install);
    EXPECT_EQ(askpass::action_from_environment("remove"), p::AuthAction::Remove);
    EXPECT_EQ(askpass::action_from_environment("git"), p::AuthAction::Git);
}
}
}
