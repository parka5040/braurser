#include <braurser/presenter/models.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>

namespace braurser::presenter {
namespace {
class FakeRpc final : public Rpc {
public:
    struct Request { std::string method; nlohmann::json params; Callback callback; };
    std::deque<Request> requests;
    std::map<std::string, std::function<void(const nlohmann::json&)>> listeners;
    void request(std::string_view method, nlohmann::json params, Callback callback, std::chrono::milliseconds) override {
        requests.push_back({std::string(method), std::move(params), std::move(callback)});
    }
    Subscription subscribe(std::string_view method, std::function<void(const nlohmann::json&)> callback) override {
        listeners.emplace(method, std::move(callback));
        return Subscription([this, method = std::string(method)] { listeners.erase(method); });
    }
    void reply(std::size_t index, nlohmann::json value) {
        auto callback = std::move(requests.at(index).callback);
        callback(std::move(value));
    }
    template<class Params> void event(const Params& event) { listeners.at(std::string(Event<Params>::name))(nlohmann::json(event)); }
};
class FakeTimer final : public Timer {
public:
    std::map<Token, std::function<void()>> callbacks;
    Token next = 1;
    std::chrono::milliseconds delay{};
    Token after(std::chrono::milliseconds value, std::function<void()> callback) override {
        delay = value; callbacks.emplace(next, std::move(callback)); return next++;
    }
    void cancel(Token token) override { callbacks.erase(token); }
    void fire() { auto current = std::move(callbacks); callbacks.clear(); for (auto& [id, callback] : current) { static_cast<void>(id); callback(); } }
};
nlohmann::json fixture(std::string_view name) {
    std::ifstream input(std::string(BRAURSER_FIXTURES) + "/" + std::string(name) + ".result.json");
    return nlohmann::json::parse(input);
}
protocol::UpdateItem item(std::string level, std::string source = "official") {
    protocol::PackageRef package{{source}, {}, "sample"};
    return {package, "1", "2", {source}, {package, {std::move(level)}, "updates.reason_unknown", {}}};
}
}
TEST(Shell, NavigationViewportThemeAndStatus) {
    FakeRpc rpc; Shell shell(rpc);
    std::size_t updates = 0;
    auto subscription = shell.observe([&](const auto&) { ++updates; });
    shell.navigate({Destination::Search, "paint", 42}); shell.navigate({Destination::Details, "krita", 0}); shell.back();
    EXPECT_EQ(shell.state().location.destination, Destination::Search);
    EXPECT_EQ(shell.state().location.scroll, 42);
    shell.width(479); EXPECT_EQ(shell.state().viewport, Viewport::Narrow);
    shell.width(480); EXPECT_EQ(shell.state().viewport, Viewport::Compact);
    shell.width(840); EXPECT_EQ(shell.state().viewport, Viewport::Wide);
    shell.width(1080); EXPECT_EQ(shell.state().viewport, Viewport::Large);
    shell.theme("system", {}); EXPECT_EQ(shell.state().resolved_theme, "light");
    shell.theme("system", true); EXPECT_EQ(shell.state().resolved_theme, "dark");
    shell.refresh(); rpc.reply(0, fixture("system.status")); rpc.reply(1, fixture("jobs.list"));
    EXPECT_TRUE(shell.state().connected); EXPECT_GT(updates, 0U);
    subscription.reset(); const auto before = updates; shell.back(); EXPECT_EQ(updates, before);
}
TEST(Presentation, CatalogCommunityIdentityIsConfirmedWithoutDetailLookup) {
    Copy copy("en", MissingCopy::Throw);
    protocol::PackageSummary package{{{"community"}, {}, "drawing-bin"}, "2.0", "Drawing", false};
    EXPECT_EQ(copy.text("source." + std::string(source_key(package.package))), "Community build");
    package.installed = true;
    EXPECT_EQ(source_key(package.package), "community");
    package.package.source.value = "outside";
    EXPECT_EQ(source_key(package.package), "foreign");
    package.package.source.value = "foreign";
    EXPECT_EQ(source_key(package.package), "foreign");
    package.package.source.value = "unrecognized";
    EXPECT_THROW(source_key(package.package), std::invalid_argument);
}
TEST(Presentation, DiscoverRetainsOnlyCollectionMetadata) {
    Discover discover;
    protocol::PackageSummary package{{{"official"}, "extra", "krita"}, "5", "Create paintings", false};
    package.appName = "Krita";
    const std::vector<std::string> ids{"krita"};
    discover.collection(1, ids, ids, std::vector{package});
    ASSERT_EQ(discover.state().apps.size(), 1U);
    EXPECT_EQ(discover.state().apps.front().appName, "Krita");
    EXPECT_EQ(discover.state().apps.front().description, "Create paintings");
    discover.collection(2, ids, {}, std::vector{package});
    EXPECT_TRUE(discover.state().apps.empty());
}
TEST(Presentation, AppGridUsesContentWidthBreakpoints) {
    for (const unsigned width : {360U, 479U, 480U, 599U}) { EXPECT_EQ(app_grid_columns(width), 1U); }
    EXPECT_EQ(app_grid_columns(600), 2U); EXPECT_EQ(app_grid_columns(899), 2U);
    EXPECT_EQ(app_grid_columns(900), 3U); EXPECT_EQ(app_grid_columns(1199), 3U);
    EXPECT_EQ(app_grid_columns(1200), 4U); EXPECT_EQ(app_grid_columns(1920), 4U);
}
TEST(Presentation, ActiveChipsRemoveOnlyTheirOwnFilter) {
    FilterSelection selection;
    selection.source = SourceFilter::Community; selection.category = "Graphics"; selection.updates = true;
    const auto chips = active_filters(selection);
    ASSERT_EQ(chips.size(), 4U);
    const auto source = std::ranges::find_if(chips, [](const auto& chip) { return chip.label.key == "filter.community"; });
    ASSERT_NE(source, chips.end());
    EXPECT_EQ(source->removed.source, SourceFilter::All);
    EXPECT_EQ(source->removed.category, "Graphics");
    EXPECT_TRUE(source->removed.apps_only); EXPECT_TRUE(source->removed.updates);
}
TEST(Discover, IntersectsShippedCollectionAndKeepsOfflineCategories) {
    Discover discover;
    std::vector<std::string> shipped{"a", "b", "b"}, available{"b", "c"};
    discover.collection(3, shipped, available);
    EXPECT_EQ(discover.state().app_ids, std::vector<std::string>{"b"});
    EXPECT_EQ(discover.state().collection_revision, 3U);
    discover.availability(true, true, true);
    EXPECT_EQ(discover.state().notice->key, "offline.local_ready");
    EXPECT_EQ(discover.state().categories.size(), 11U);
    discover.collection(4, shipped, {}); EXPECT_TRUE(discover.state().app_ids.empty());
}
TEST(Search, FilterTruthTable) {
    for (const auto installation : {Installation::Any, Installation::Installed, Installation::NotInstalled}) {
        for (const auto added : {AddedAs::Any, AddedAs::Explicit, AddedAs::Dependency}) {
            for (const bool orphans : {false, true}) {
                for (const auto source : {SourceFilter::All, SourceFilter::Official, SourceFilter::Community}) {
                    FilterSelection selection; selection.installation = installation; selection.added_as = added; selection.orphans = orphans; selection.source = source;
                    const auto mapping = map_filters(selection); const auto& filter = mapping.filters;
                    const bool implied = added != AddedAs::Any || orphans;
                    const bool installed = implied || installation == Installation::Installed;
                    EXPECT_EQ(filter.installed, installed);
                    EXPECT_EQ(filter.not_installed, !implied && installation == Installation::NotInstalled);
                    EXPECT_EQ(mapping.not_installed_enabled, !implied);
                    EXPECT_EQ(filter.native, installed && source == SourceFilter::Official);
                    EXPECT_EQ(filter.foreign, installed && source == SourceFilter::Community);
                    EXPECT_EQ(filter.repo_only, !installed && source == SourceFilter::Official);
                    EXPECT_EQ(filter.aur_only, !installed && source == SourceFilter::Community);
                    EXPECT_EQ(mapping.foreign_note.has_value(), installed && source == SourceFilter::Community);
                }
            }
        }
    }
}
TEST(Search, UnsupportedWirePredicatesAreNeverDropped) {
    FilterSelection selection; selection.repositories = {"extra"}; selection.source = SourceFilter::Community;
    const auto mapped = map_filters(selection);
    EXPECT_FALSE(mapped.community_enabled); EXPECT_TRUE(mapped.disabled_reason);
    EXPECT_FALSE(search_params({"", mapped.filters, {"relevance"}, {}, 50}));
    selection.source = SourceFilter::All; selection.repositories.push_back("core");
    EXPECT_FALSE(search_params({"", map_filters(selection).filters, {"relevance"}, {}, 50}));
    selection.repositories.clear(); selection.category = "Graphics";
    EXPECT_TRUE(search_params({"", map_filters(selection).filters, {"name"}, {}, 50}));
}
TEST(Search, DebouncesDiscardsStaleAndPages) {
    FakeRpc rpc; FakeTimer timer; Search search(rpc, timer);
    search.query("old"); EXPECT_TRUE(rpc.requests.empty());
    EXPECT_EQ(timer.delay, std::chrono::milliseconds(250)); timer.fire(); ASSERT_EQ(rpc.requests.size(), 1U);
    search.query("new"); search.submit(); ASSERT_EQ(rpc.requests.size(), 2U); EXPECT_TRUE(timer.callbacks.empty());
    auto page = fixture("catalog.search"); page["stale"] = true; rpc.reply(0, page);
    EXPECT_TRUE(search.state().results.empty()); EXPECT_TRUE(search.state().loading);
    page["stale"] = false; page["nextCursor"] = "next"; rpc.reply(1, page);
    EXPECT_FALSE(search.state().stale); EXPECT_FALSE(search.state().loading);
    const auto first_size = search.state().results.size(); search.next_page(); ASSERT_EQ(rpc.requests.size(), 3U);
    EXPECT_EQ(rpc.requests.back().params.at("cursor"), "next"); page.erase("nextCursor"); rpc.reply(2, page);
    EXPECT_EQ(search.state().results.size(), first_size * 2); EXPECT_EQ(search.state().total, first_size * 2);
}
TEST(Models, DestroyedModelIgnoresOutstandingReply) {
    FakeRpc rpc; FakeTimer timer;
    { Search search(rpc, timer); search.submit(); }
    EXPECT_NO_THROW(rpc.reply(0, fixture("catalog.search")));
}
TEST(Details, DetailsWikiAndVariantsStayIndependent) {
    FakeRpc rpc; Details details(rpc);
    details.load("sample"); rpc.reply(0, fixture("catalog.get"));
    ASSERT_TRUE(details.state().package); EXPECT_FALSE(details.state().loading);
    details.load_wiki(false); rpc.reply(1, fixture("wiki.get"));
    ASSERT_TRUE(details.state().wiki); EXPECT_EQ(details.state().wiki_attribution.key, "wiki.attribution");
    details.variants(); rpc.reply(2, fixture("catalog.variants")); EXPECT_TRUE(details.state().variants);
    details.build({"https://example.org/build", std::string(40, 'a'), {{"PKGBUILD", "pkgname=sample\n"}}, true});
    EXPECT_EQ(details.state().build_disclosure.key, "detail.build_identity");
    EXPECT_EQ(details.state().build_warning->key, "detail.build_mutable");
    details.offline(true); EXPECT_TRUE(details.state().wiki); EXPECT_TRUE(details.state().offline);
    details.load("other"); EXPECT_FALSE(details.state().wiki); EXPECT_FALSE(details.state().build);
}
TEST(Variant, UsesDaemonRecommendationAndEveryKind) {
    VariantChooser chooser; protocol::VariantChoices choices;
    for (const auto* kind : {"standard", "bin", "git", "appimage", "nightly", "beta", "stable", "wayland"}) {
        choices.options.push_back({{{"community"}, {}, "sample"}, {kind}, false, false});
    }
    choices.urls_differ = true; chooser.choices(choices); EXPECT_FALSE(chooser.state().selected);
    Copy copy("en", MissingCopy::Throw);
    for (const auto& option : chooser.state().options) {
        EXPECT_NO_THROW(option.title.render(copy));
        EXPECT_NO_THROW(option.body.render(copy));
    }
    choices.options[1].recommended = true; chooser.choices(choices); EXPECT_EQ(chooser.state().selected, 1U);
    EXPECT_EQ(chooser.state().note->key, "variant.different_sites");
    choices.options[2].recommended = true; EXPECT_THROW(chooser.choices(choices), std::invalid_argument);
}
TEST(Confirm, ReviewAcknowledgementStalenessAndSubmission) {
    FakeRpc rpc; Confirm confirm(rpc);
    auto plan = fixture("updates.plan").get<protocol::UpdatesPlanResult>().transactions.front();
    plan.kind.value = "install"; plan.requires_build_review = true; plan.build_review_refs = {{{"community"}, {}, "sample"}}; plan.blockers.clear(); plan.protected_blockers.clear(); plan.conflicts.clear();
    confirm.plan(plan); rpc.reply(0, protocol::BuildFilesResult{"sample", {{"PKGBUILD", "pkgname=sample"}}, false}); EXPECT_FALSE(confirm.can_confirm()); confirm.acknowledge_build(true); EXPECT_TRUE(confirm.can_confirm());
    confirm.invalidate(); EXPECT_FALSE(confirm.can_confirm());
    confirm.plan(plan); rpc.reply(1, protocol::BuildFilesResult{"sample", {{"PKGBUILD", "pkgname=sample"}}, false}); confirm.acknowledge_build(true); confirm.busy(true); EXPECT_EQ(confirm.action().key, "confirm.add_queue");
    confirm.submit(); ASSERT_EQ(rpc.requests.size(), 3U); EXPECT_FALSE(confirm.can_confirm());
    rpc.reply(2, fixture("jobs.install")); EXPECT_TRUE(confirm.state().job_id); EXPECT_TRUE(confirm.state().stale);
}
TEST(Confirm, StalePlanReplansWithoutSubmittingAgain) {
    FakeRpc rpc; Confirm confirm(rpc);
    confirm.request({"install", "sample"});
    auto plan = fixture("updates.plan").get<protocol::UpdatesPlanResult>().transactions.front();
    rpc.reply(0, plan); confirm.submit();
    rpc.requests[1].callback(std::unexpected(RpcError{protocol::kStalePlan, "confirm.plan_changed", nlohmann::json::object()}));
    ASSERT_EQ(rpc.requests.size(), 3U); EXPECT_EQ(rpc.requests.back().method, "jobs.plan");
    plan.plan_id = "replacement"; plan.approval_token = "replacement-token"; ++plan.revision;
    rpc.reply(2, plan);
    ASSERT_TRUE(confirm.state().error); EXPECT_EQ(confirm.state().error->key, "confirm.plan_changed");
    EXPECT_EQ(rpc.requests.size(), 3U); EXPECT_FALSE(confirm.state().build_acknowledged);
    confirm.submit(); EXPECT_EQ(rpc.requests.back().params["approvalToken"], "replacement-token");
    EXPECT_EQ(error_text(protocol::kSystemUpdateRequired).render(Copy{}), "Update system first, then update this app.");
}
TEST(Search, FacetsSortAndCategoryUseFinalProtocol) {
    FakeRpc rpc; FakeTimer timer; Search search(rpc, timer);
    search.facets(); rpc.reply(0, protocol::FacetsResult{{{"extra", 3}}, {{"graphics", 2}}, {{"Graphics", 1}}});
    EXPECT_EQ(search.state().facets.repos.front().packageCount, 3U);
    FilterSelection selection; selection.category = "Graphics"; search.filters(selection); search.sort("name");
    EXPECT_EQ(rpc.requests.back().params["category"], "Graphics");
    EXPECT_EQ(rpc.requests.back().params["sort"], "name");
}
TEST(Updates, UnknownIsNeverNoneAndNewsBlocksActions) {
    FakeRpc rpc; Updates updates(rpc);
    protocol::UpdateReport report; report.report_id = "report"; report.checked_at = "2026-09-25T12:00:00Z"; report.overall_restart.value = "reboot";
    for (const auto* level : {"reboot", "logout", "restart-app", "restart-service", "none", "unknown"}) { report.official.push_back(item(level)); }
    report.community.push_back(item("unknown", "community"));
    report.news.push_back({"Notice", "https://archlinux.org/news/notice/", "2026-09-25T12:00:00Z", true, false});
    updates.report(report);
    EXPECT_EQ(updates.state().groups[2].items.size(), 2U); EXPECT_EQ(updates.state().groups[3].items.size(), 1U);
    EXPECT_EQ(updates.state().groups[4].items.size(), 2U); EXPECT_TRUE(updates.state().groups[4].expanded);
    EXPECT_FALSE(updates.can_update()); updates.acknowledge_news(0, true); EXPECT_TRUE(updates.can_update());
    EXPECT_EQ(updates.item_blocker()->key, "updates.system_first");
    updates.news_available(false); EXPECT_FALSE(updates.can_update()); updates.acknowledge_missing_news(true); EXPECT_TRUE(updates.can_update());
    rpc.reply(0, fixture("updates.options")); updates.plan_system(); ASSERT_EQ(rpc.requests.size(), 2U);
    EXPECT_EQ(rpc.requests.back().params["kind"], "update-system");
}
TEST(Activity, BoundedRedactedLogsMeasuredProgressAndQuestions) {
    FakeRpc rpc; Activity activity(rpc, 64); activity.attach("job");
    rpc.reply(0, fixture("jobs.get")); rpc.reply(1, fixture("jobs.list"));
    rpc.event(protocol::JobProgressNotification{"job", 1, {{"building"}, 4, 10, {"items"}}}); EXPECT_FALSE(activity.state().fraction);
    rpc.event(protocol::JobProgressNotification{"job", 2, {{"downloading"}, 4, 10, {"items"}}}); EXPECT_EQ(activity.state().fraction, 0.4);
    rpc.event(protocol::JobLogNotification{"job", 3, "password=secret"});
    EXPECT_EQ(activity.state().log.back().find("secret"), std::string::npos); EXPECT_EQ(activity.state().log.back().find('\x1b'), std::string::npos);
    for (std::uint64_t index = 4; index < 20; ++index) { rpc.event(protocol::JobLogNotification{"job", index, "aaaaaaaaaa"}); }
    std::size_t size = 0; for (const auto& line : activity.state().log) { size += line.size(); } EXPECT_LE(size, 64U);
    rpc.event(protocol::JobAuthStateNotification{"job", 20, {"required"}}); EXPECT_EQ(activity.state().auth_message->key, "activity.auth_wait");
    EXPECT_FALSE(activity.state().can_cancel); activity.cancel_capability(true, true); activity.cancel(); EXPECT_FALSE(activity.state().can_cancel);
}
TEST(Installed, SwitchesOwnershipViewsWithoutInventingComponents) {
    FakeRpc rpc; Installed installed(rpc);
    installed.view(InstalledView::Git); rpc.reply(0, fixture("manual.list")); EXPECT_FALSE(installed.state().receipts.empty());
    installed.view(InstalledView::Outside); rpc.reply(1, fixture("unmanaged.list")); EXPECT_FALSE(installed.state().outside.empty());
    installed.view(InstalledView::Components); EXPECT_TRUE(installed.state().error);
    installed.view(InstalledView::Apps); EXPECT_TRUE(rpc.requests.back().params["filters"]["installed"].get<bool>());
}
TEST(Settings, ImmediateSerializedApplyAndUnavailableHelper) {
    FakeRpc rpc; Settings settings(rpc); settings.refresh(); rpc.reply(0, fixture("settings.get"));
    EXPECT_TRUE(settings.state().legacy); EXPECT_FALSE(settings.state().values);
    settings.capabilities({"paru"}, "paru", false); EXPECT_TRUE(settings.helper_disabled("yay")); EXPECT_FALSE(settings.helper_disabled("auto"));
    protocol::SettingsPatch patch; patch.notifications = true; settings.apply(patch); patch.notifications = false; settings.apply(patch);
    ASSERT_EQ(rpc.requests.size(), 2U); rpc.reply(1, fixture("settings.set")); ASSERT_EQ(rpc.requests.size(), 3U); rpc.reply(2, fixture("settings.set"));
    EXPECT_TRUE(settings.state().values); EXPECT_FALSE(settings.state().saving);
    settings.capabilities({"paru"}, "paru", true); EXPECT_EQ(settings.helper_disabled("auto")->key, "settings.helper_busy");
}
TEST(GitWizard, FourStepsRequireReviewScopeAndExplicitApproval) {
    FakeRpc rpc; GitWizard wizard(rpc);
    wizard.inspect("https://example.org/team/app.git", "install.sh", "user"); rpc.reply(0, fixture("manual.preview"));
    protocol::ManualPreview preview{"preview", "example.org", "team/app", std::string(40, 'a'), "install.sh", "printf ready\n", {{"user"}}, {"https://example.org/source"}, false};
    wizard.preview(preview); EXPECT_EQ(wizard.state().step, GitStep::Review);
    wizard.next(); EXPECT_EQ(wizard.state().step, GitStep::Review);
    wizard.acknowledge(true); wizard.next(); EXPECT_EQ(wizard.state().step, GitStep::Scope);
    ASSERT_EQ(rpc.requests.size(), 2U);
    EXPECT_EQ(rpc.requests.back().method, "jobs.plan");
    EXPECT_EQ(rpc.requests.back().params["previewId"], "preview");
    auto plan = fixture("updates.plan").get<protocol::UpdatesPlanResult>().transactions.front();
    plan.kind = {"manual-install"}; plan.blockers.clear(); plan.protected_blockers.clear(); plan.conflicts.clear();
    plan.approval_token = "server-token"; rpc.reply(1, plan);
    wizard.install(); ASSERT_EQ(rpc.requests.size(), 3U);
    EXPECT_EQ(rpc.requests.back().method, "manual.install");
    EXPECT_EQ(rpc.requests.back().params["approvalToken"], "server-token");
    EXPECT_EQ(rpc.requests.back().params["planId"], plan.plan_id); rpc.reply(2, fixture("manual.install"));
    EXPECT_EQ(wizard.state().step, GitStep::Progress); wizard.back(); EXPECT_EQ(wizard.state().step, GitStep::Progress);
    wizard.result({}); EXPECT_TRUE(wizard.state().failed);
}
TEST(GitWizard, RejectsUnsafeUrlsAndOversizedReview) {
    for (const auto* url : {"file:///tmp/a", "http://example.org/a", "https://user:pass@example.org/a", "https://example.org/a?token=secret", "https://example.org/a;id", "https://example.org/a/../b", "https://example.org/a%0a"}) {
        EXPECT_FALSE(GitWizard::valid_url(url)) << url;
    }
    FakeRpc rpc; GitWizard wizard(rpc); wizard.inspect("https://example.org/team/app", "install.sh", "user"); rpc.reply(0, fixture("manual.preview"));
    protocol::ManualPreview preview{"preview", "example.org", "team/app", std::string(40, 'a'), "install.sh", std::string(65537, 'x'), {{"user"}}, {}, false};
    wizard.preview(preview); EXPECT_EQ(wizard.state().error->key, "git.script_too_large"); EXPECT_FALSE(wizard.state().preview);
}
namespace {
class Credentials final : public CredentialChannel {
public:
    bool called = false;
    bool throws = false;
    bool submit(const AuthBinding&, std::string_view, std::span<const char> password) override {
        called = true; EXPECT_EQ(std::string(password.data(), password.size()), "secret");
        if (throws) { throw std::runtime_error("failure"); } return true;
    }
    void cancel(const AuthBinding&) override {}
};
AuthBinding binding() { return {"operation", "challenge", AuthAction::Install, "App", {"alex", "sam"}, std::chrono::steady_clock::now() + std::chrono::minutes(1), false, true}; }
void password(Auth& auth) { const std::string value = "secret"; auth.password(std::span(value.data(), value.size())); }
void zeroed(const Auth& auth) { EXPECT_TRUE(std::ranges::all_of(auth.buffer(), [](char byte) { return byte == 0; })); }
}
TEST(Auth, WipesOnSubmitCancelAccountChangeExpiryAndException) {
    Credentials channel; FakeTimer timer; Auth auth(channel, timer);
    auth.bind(binding()); password(auth); auth.submit(); zeroed(auth); EXPECT_TRUE(channel.called);
    auth.bind(binding()); password(auth); auth.cancel(); zeroed(auth);
    auth.bind(binding()); password(auth); auth.account("sam"); zeroed(auth);
    auth.bind(binding()); password(auth); timer.fire(); zeroed(auth); EXPECT_EQ(auth.state().message->key, "auth.timeout");
    auth.bind(binding()); password(auth); channel.throws = true; auth.submit(); zeroed(auth); EXPECT_EQ(auth.state().message->key, "auth.failed");
}
TEST(Auth, NativeAgentAndUntrustedFallbackNeverShowPasswordUi) {
    Credentials channel; FakeTimer timer; Auth auth(channel, timer); auto native = binding(); native.native_agent = true;
    auth.bind(native); EXPECT_FALSE(auth.state().password_ui); password(auth); zeroed(auth); auth.submit(); EXPECT_FALSE(channel.called);
    native.native_agent = false; native.trusted_local_fallback = false; auth.bind(native); EXPECT_FALSE(auth.state().active); EXPECT_FALSE(auth.state().password_ui);
}
TEST(EmptyStates, EveryErrorAndEmptyStateUsesCatalog) {
    Copy copy("en", MissingCopy::Throw);
    for (int code = -32001; code >= -32010; --code) { EXPECT_NO_THROW(error_text(code).render(copy)); }
    for (auto view : {InstalledView::Apps, InstalledView::Components, InstalledView::Git, InstalledView::Outside}) {
        const auto state = empty_installed(view);
        EXPECT_NO_THROW(state.title.render(copy));
        EXPECT_NO_THROW(state.body.render(copy));
    }
    EXPECT_NO_THROW(empty_search("paint").title.render(copy));
    FakeRpc rpc; FirstRun first(rpc); first.capabilities(false, true); EXPECT_TRUE(first.state().setup_supported); EXPECT_EQ(first.state().preferred_helper, "yay"); first.helper("yay"); EXPECT_EQ(first.state().preferred_helper, "yay");
}
}
