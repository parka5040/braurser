#include "fixtures.hpp"
#include <gtest/gtest.h>
#include <set>

namespace braurser::presenter::fixtures {
namespace {
class Queue final : public Dispatcher {
public:
    void post(std::function<void()> callback) override { callbacks.push_back(std::move(callback)); }
    void drain() {
        while (!callbacks.empty()) {
            auto callback = std::move(callbacks.front());
            callbacks.pop_front();
            callback();
        }
    }
    std::deque<std::function<void()>> callbacks;
};

template<class T> void roundtrip(const T& value) {
    const nlohmann::json encoded = value;
    EXPECT_EQ(nlohmann::json(encoded.get<T>()), encoded);
}

TEST(PresenterFixtures, CanonicalDataCoversBothFrontends) {
    const auto& fixture = data();
    EXPECT_EQ(fixture.official.details.package.name, "krita");
    ASSERT_TRUE(fixture.official.appMetadata);
    EXPECT_EQ(fixture.official.appMetadata->name, "Krita");
    EXPECT_EQ(fixture.community.details.package.source.value, "community");
    EXPECT_TRUE(fixture.community_info.maintainer);
    EXPECT_TRUE(fixture.community_info.out_of_date_since);
    EXPECT_GT(fixture.community_info.votes, 0U);
    ASSERT_EQ(fixture.variants.choices.options.size(), 2U);
    EXPECT_EQ(fixture.variants.choices.options[0].kind.value, "bin");
    EXPECT_EQ(fixture.variants.choices.options[1].kind.value, "git");
    EXPECT_FALSE(fixture.receipt.launcher_ids.empty());
    EXPECT_EQ(fixture.outside.owner.value, "outside");
    std::set<std::string> impacts;
    for (const auto& item : fixture.updates.official) { impacts.insert(item.restart.level.value); }
    for (const auto& item : fixture.updates.community) { impacts.insert(item.restart.level.value); }
    EXPECT_EQ(impacts, (std::set<std::string>{"reboot", "logout", "restart-app", "none", "unknown"}));
    ASSERT_EQ(fixture.updates.news.size(), 1U);
    EXPECT_TRUE(fixture.updates.news.front().requires_manual_intervention);
    EXPECT_FALSE(fixture.updates.news.front().acknowledged);
    EXPECT_EQ(fixture.running.state.value, "running");
    ASSERT_TRUE(fixture.running.progress);
    EXPECT_EQ(fixture.running.progress->current, 48U);
    EXPECT_TRUE(std::holds_alternative<protocol::ProviderQuestion>(fixture.question.choice.value));
    roundtrip(fixture.official);
    roundtrip(fixture.community);
    roundtrip(fixture.community_info);
    roundtrip(fixture.variants);
    roundtrip(fixture.receipt);
    roundtrip(fixture.preview);
    roundtrip(fixture.outside);
    roundtrip(fixture.wiki);
    roundtrip(fixture.updates);
    roundtrip(fixture.running);
    roundtrip(fixture.question);
}

TEST(PresenterFixtures, WikiIncludesHeadingsNotesWarningsCodeAndHttpsLink) {
    const auto& document = data().wiki.document;
    ASSERT_EQ(document.blocks.size(), 6U);
    EXPECT_EQ(std::get<protocol::WikiHeading>(document.blocks[0].value).level, 2U);
    EXPECT_EQ(std::get<protocol::WikiHeading>(document.blocks[2].value).level, 3U);
    const auto& paragraph = std::get<protocol::WikiParagraph>(document.blocks[1].value);
    EXPECT_TRUE(std::get<protocol::WikiLink>(paragraph.inlines[1].value).href.starts_with("https://"));
    EXPECT_TRUE(std::holds_alternative<protocol::WikiNote>(document.blocks[3].value));
    EXPECT_TRUE(std::holds_alternative<protocol::WikiWarning>(document.blocks[4].value));
    EXPECT_TRUE(std::holds_alternative<protocol::WikiCode>(document.blocks[5].value));
}

TEST(PresenterFixtures, RunningActivityHasProgressAndPendingQuestion) {
    Queue queue;
    FakeServer server(queue);
    Activity activity(server);
    activity.attach("fixture-active");
    EXPECT_FALSE(activity.state().job);
    queue.drain();
    ASSERT_TRUE(activity.state().job);
    EXPECT_EQ(activity.state().fraction, 0.48);
    ASSERT_TRUE(activity.state().question);
    EXPECT_EQ(activity.state().question->question_id, data().question.question_id);
}

TEST(PresenterFixtures, UnsubscribingDropsAlreadyQueuedNotifications) {
    Queue queue;
    unsigned called = 0;
    Subscription subscription;
    {
        FakeServer server(queue);
        subscription = server.subscribe("fixture.event", [&](const auto&) { ++called; });
        server.emit_event("fixture.event", nlohmann::json::object());
        subscription.reset();
        queue.drain();
        EXPECT_EQ(called, 0U);
        subscription = server.subscribe("fixture.event", [&](const auto&) { ++called; });
        server.emit_event("fixture.event", nlohmann::json::object());
    }
    queue.drain();
    EXPECT_EQ(called, 0U);
    subscription.reset();
}

TEST(PresenterFixtures, UpdatesRemainBlockedUntilNewsIsAcknowledged) {
    Queue queue;
    FakeServer server(queue);
    Updates updates(server);
    updates.report(FakeServer::update_report());
    queue.drain();
    EXPECT_FALSE(updates.can_update());
    updates.acknowledge_news(0, true);
    EXPECT_TRUE(updates.can_update());
    EXPECT_EQ(updates.item_blocker()->key, "updates.system_first");
}

TEST(PresenterFixtures, EveryMutationUsesReviewedPlanToken) {
    const std::vector<std::pair<std::string, std::string>> methods{
        {"install", "jobs.install"}, {"remove", "jobs.remove"}, {"update-system", "jobs.upgradeSystem"},
        {"update-community", "jobs.upgradeCommunity"}, {"update-and-install", "jobs.install"},
        {"manual-install", "manual.install"}, {"manual-upgrade", "manual.upgrade"},
        {"manual-remove", "manual.remove"}, {"setup-helper", "setup.helper"}};
    for (const auto& [kind, method] : methods) {
        Queue queue; FakeServer server(queue); Confirm confirm(server);
        protocol::PlanParams params; params.kind = kind;
        if (kind == "manual-install") { params.previewId = "fixture-preview"; params.scope = protocol::ReceiptScope{"user"}; }
        else if (kind.starts_with("manual")) { params.receiptId = "fixture-git"; if (kind == "manual-upgrade") { params.previewId = "fixture-preview"; } }
        else if (kind == "setup-helper") { params.helper = protocol::HelperPreference{"yay"}; }
        else if (kind != "update-system") { params.packageId = "drawing-bin"; }
        confirm.request(params); queue.drain();
        ASSERT_TRUE(confirm.state().plan) << kind;
        EXPECT_EQ(server.requests.size(), 1U);
        const auto plan = *confirm.state().plan;
        confirm.submit(); queue.drain();
        ASSERT_TRUE(confirm.state().job_id) << kind;
        EXPECT_EQ(server.requests.back().first, method);
        EXPECT_EQ(server.requests.back().second, (nlohmann::json{{"planId", plan.plan_id}, {"approvalToken", plan.approval_token}}));
        const auto count = server.requests.size(); confirm.submit(); EXPECT_EQ(server.requests.size(), count);
    }
}

TEST(PresenterFixtures, FacetsBuildFilesSummaryRestartSetupCacheAndDequeue) {
    Queue queue; FakeServer server(queue);
    Details details(server); details.load("drawing-bin"); details.summary(data().packages[1]); queue.drain();
    ASSERT_TRUE(details.state().build); EXPECT_EQ(details.state().build->files.front().path, "PKGBUILD");
    ASSERT_TRUE(details.state().summary); EXPECT_EQ(details.state().summary->installedVersion, "1.9");
    Shell shell(server);
    server.reply("restart.state", protocol::RestartStateResult{{"reboot"}, {"updates.reason_system"}, "2026-09-25T12:00:00Z"});
    shell.refresh(); queue.drain(); EXPECT_EQ(shell.state().restart.level.value, "reboot");
    EXPECT_TRUE(shell.state().community_available); EXPECT_EQ(shell.state().catalog_state, "ready");
    shell.acknowledge_restart(); queue.drain(); EXPECT_EQ(shell.state().restart.level.value, "none");
    FirstRun first(server); first.refresh(); queue.drain(); EXPECT_TRUE(first.state().setup_supported);
    Confirm confirm(server); first.helper("yay"); first.review(confirm); queue.drain(); ASSERT_TRUE(confirm.state().plan);
    EXPECT_EQ(confirm.state().plan->kind.value, "setup-helper");
    Settings settings(server); const auto count = server.requests.size(); settings.clear_cache(true); EXPECT_EQ(server.requests.size(), count);
    settings.request_clear_cache(); settings.clear_cache(false); EXPECT_EQ(server.requests.size(), count);
    settings.request_clear_cache(); settings.clear_cache(true); queue.drain(); EXPECT_EQ(settings.state().cache_bytes, 0U);
    Activity activity(server); activity.refresh(); queue.drain(); ASSERT_EQ(activity.state().queue.size(), 1U);
    activity.dequeue("fixture-queued"); queue.drain(); EXPECT_TRUE(activity.state().queue.empty());
}

TEST(PresenterFixtures, ModesAndUnknownMethodsFailClosed) {
    for (const auto mode : {FakeServer::Mode::Populated, FakeServer::Mode::Empty, FakeServer::Mode::Error, FakeServer::Mode::Loading, FakeServer::Mode::Offline}) {
        Queue queue;
        FakeServer server(queue, mode);
        bool called = false;
        protocol::SearchParams params{"", {}, {}, 50};
        server.call<Method<protocol::SearchParams>>(params, [&](Reply<protocol::SearchResult> result) {
            called = true;
            if (mode == FakeServer::Mode::Error) { EXPECT_FALSE(result); return; }
            ASSERT_TRUE(result);
            EXPECT_EQ(result->packages.empty(), mode == FakeServer::Mode::Empty);
            EXPECT_EQ(result->stale, mode == FakeServer::Mode::Offline);
        });
        queue.drain();
        EXPECT_EQ(called, mode != FakeServer::Mode::Loading);
        if (mode == FakeServer::Mode::Loading) { continue; }
        server.request("unknown.method", nlohmann::json::object(), [](auto result) { EXPECT_FALSE(result); }, std::chrono::seconds(1));
        queue.drain();
    }
}
}
}
