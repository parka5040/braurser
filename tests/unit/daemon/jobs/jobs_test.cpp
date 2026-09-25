#include <braurser/daemon/jobs/live.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <gtest/gtest.h>
#include <giomm/init.h>
#include <glibmm/main.h>
#include <atomic>
#include <fstream>
#include <memory>
#include <unistd.h>

namespace braurser::daemon::jobs {
namespace {

class Source final : public PlanSource {
public:
    std::string fingerprint{"unchanged"};
    std::string kind{"install"};
    std::string helper{"auto"};
    Result<Snapshot> snapshot(const protocol::PlanParams& selection) override {
        Snapshot value;
        value.plan.kind = {selection.kind};
        value.plan.installs = {{{"official"}, "extra", selection.packageId.value_or("tree")}};
        value.fingerprint = fingerprint;
        value.targets = {selection.packageId.value_or("tree")};
        value.planned_version = "1-1";
        value.helper = helper;
        return value;
    }
};

struct ProcessState {
    core::ProcessSpec spec;
    StreamingProcess::Line line;
    StreamingProcess::Done done;
    int interruptions{};
    int kills{};
    void finish(int code) {
        auto callback = done;
        core::ProcessResult result;
        result.exit_code = code;
        callback(result);
    }
};

class FakeProcess final : public StreamingProcess {
public:
    explicit FakeProcess(std::shared_ptr<ProcessState> state) : state_(std::move(state)) {}
    bool start(const core::ProcessSpec& spec, Line line, Done done) override {
        state_->spec = spec;
        state_->line = std::move(line);
        state_->done = std::move(done);
        return true;
    }
    void interrupt() override { ++state_->interruptions; }
    void kill_group() override { ++state_->kills; }
private:
    std::shared_ptr<ProcessState> state_;
};

class JobsFixture : public ::testing::Test {
protected:
    Source source;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    PlanManager plans{source, [this] { return now; }};
    std::filesystem::path state = std::filesystem::temp_directory_path() /
        ("braurser-jobs-test-" + std::to_string(getpid()) + "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::vector<std::shared_ptr<ProcessState>> processes;
    std::vector<protocol::JobEvent> notifications;
    std::unique_ptr<JobRunner> runner;

    void SetUp() override {
        ASSERT_TRUE(core::ensure_private_dir(state).has_value());
        runner = std::make_unique<JobRunner>(plans, [this] {
            auto process = std::make_shared<ProcessState>();
            processes.push_back(process);
            return std::make_unique<FakeProcess>(process);
        }, [this](std::string_view, const protocol::JobEvent& event, int) { notifications.push_back(event); },
        state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"},
        JobRunner::BuildSpec{}, JobRunner::Verify{}, JobRunner::Classify{}, [this] { return now; });
    }
    void TearDown() override {
        runner.reset();
        std::error_code error;
        std::filesystem::remove_all(state, error);
    }
    protocol::TransactionPlan plan(std::string name = "tree") {
        auto result = plans.create(protocol::PlanParams{.kind = source.kind, .packageId = std::move(name)}, 11);
        EXPECT_TRUE(result.has_value());
        return result.value();
    }
    std::string enqueue(const protocol::TransactionPlan& approved) {
        auto result = runner->enqueue(approved.plan_id, approved.approval_token, 11);
        EXPECT_TRUE(result.has_value());
        return result->jobId;
    }
};

TEST(JobsSpec, CanonicalOfficialTransactionsForBothElevationMethods) {
    ElevationOptions auth{elevation::Method::Pkexec, "/usr/local/libexec/braurser"};
    const auto helper = "/usr/local/libexec/braurser/braurser-pacman-helper";
    struct Case { std::string kind; std::vector<std::string> targets; std::vector<std::string> args; };
    for (const auto& value : std::vector<Case>{
        {"install", {"extra/tree"}, {"--sync", "--needed", "--noconfirm", "--", "extra/tree"}},
        {"update-system", {}, {"--sync", "--refresh", "--sysupgrade", "--noconfirm"}},
        {"update-and-install", {"tree"}, {"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--needed", "--", "tree"}},
        {"remove", {"tree"}, {"--remove", "--nosave", "--recursive", "--noconfirm", "--", "tree"}},
        {"manual-install", {"/home/user/.cache/braurser/build/manual/job/pkg/pkg-1.pkg.tar.zst"},
            {"--upgrade", "--noconfirm", "--", "/home/user/.cache/braurser/build/manual/job/pkg/pkg-1.pkg.tar.zst"}}}) {
        auto spec = official_spec(value.kind, value.targets, auth);
        ASSERT_TRUE(spec.has_value()) << value.kind;
        EXPECT_EQ(spec->executable, "/usr/bin/pkexec");
        std::vector<std::string> expected{helper};
        expected.insert(expected.end(), value.args.begin(), value.args.end());
        EXPECT_EQ(spec->args, expected);
        auth.method = elevation::Method::SudoAskpass;
        auth.ui = "qt";
        spec = official_spec(value.kind, value.targets, auth);
        ASSERT_TRUE(spec.has_value());
        expected.insert(expected.begin(), {"-A", "--"});
        EXPECT_EQ(spec->executable, "/usr/bin/sudo");
        EXPECT_EQ(spec->args, expected);
        EXPECT_NE(std::ranges::find(spec->env, std::pair<std::string, std::string>{
            "SUDO_ASKPASS", "/usr/local/libexec/braurser/braurser-askpass-qt"}), spec->env.end());
        auth.method = elevation::Method::Pkexec;
    }
    auth.method = elevation::Method::Unavailable;
    auto refused = official_spec("install", {"tree"}, auth);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, protocol::kAuthorizationUnavailable);
    EXPECT_EQ(refused.error().message_key, "auth.unavailable_title");
    auth.method = static_cast<elevation::Method>(99);
    EXPECT_EQ(official_spec("install", {"tree"}, auth).error().code,
              protocol::kAuthorizationUnavailable);
    auth.method = elevation::Method::Pkexec;
    EXPECT_EQ(official_spec("update-system", {"tree"}, auth).error().code,
              protocol::kInvalidParams);
}

TEST(PlanApproval, ConnectionBindingSingleUseExpiryAndFreshness) {
    Source source;
    auto now = std::chrono::steady_clock::now();
    PlanManager plans(source, [&] { return now; });
    const protocol::PlanParams request{.kind = "install", .packageId = "tree"};
    auto plan = plans.create(request, 18);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->approval_token.size(), 32);
    EXPECT_EQ(plans.approve(plan->plan_id, plan->approval_token, 19).error().code, protocol::kStalePlan);
    EXPECT_EQ(plans.approve(plan->plan_id, "wrong", 18).error().code, protocol::kStalePlan);
    source.fingerprint = "changed";
    EXPECT_EQ(plans.approve(plan->plan_id, plan->approval_token, 18).error().code, protocol::kStalePlan);
    source.fingerprint = "unchanged";
    EXPECT_EQ(plans.approve(plan->plan_id, plan->approval_token, 18).error().code, protocol::kStalePlan);
    auto other = plans.create(request, 18);
    ASSERT_TRUE(other);
    ASSERT_TRUE(plans.approve(other->plan_id, other->approval_token, 18));
    EXPECT_EQ(plans.approve(other->plan_id, other->approval_token, 18).error().code, protocol::kStalePlan);
    other = plans.create(request, 18);
    now += std::chrono::minutes(10);
    EXPECT_EQ(plans.approve(other->plan_id, other->approval_token, 18).error().code, protocol::kStalePlan);
}

TEST(PlanApproval, ReclaimsUsedAndExpiredEntries) {
    Source source;
    auto now = std::chrono::steady_clock::now();
    PlanManager plans(source, [&] { return now; });
    const protocol::PlanParams selection{.kind = "install", .packageId = "tree"};
    auto first = plans.create(selection, 11);
    ASSERT_TRUE(first);
    ASSERT_TRUE(plans.approve(first->plan_id, first->approval_token, 11));
    for (int count = 0; count < 256; ++count) ASSERT_TRUE(plans.create(selection, 11));
    EXPECT_EQ(plans.create(selection, 11).error().code, protocol::kBusy);
    now += std::chrono::minutes(10);
    EXPECT_TRUE(plans.create(selection, 11));
}

TEST_F(JobsFixture, MutationEndpointMustMatchApprovedKind) {
    auto approved = plan("tree");
    auto rejected = runner->enqueue(approved.plan_id, approved.approval_token, 11, "remove");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, protocol::kInvalidParams);
    EXPECT_TRUE(processes.empty());
    approved = plan("nano");
    ASSERT_TRUE(runner->enqueue(approved.plan_id, approved.approval_token, 11, "install"));
    ASSERT_EQ(processes.size(), 1);
}

class FakeManualInstaller final : public ManualInstaller {
public:
    std::atomic<int> runs{};
    Result<void> run(const Snapshot&, std::string_view) override { ++runs; return {}; }
};

TEST_F(JobsFixture, UserRemovalRunsWithoutElevation) {
    FakeManualInstaller installer;
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, JobRunner::Notify{}, state, ElevationOptions{elevation::Method::Unavailable},
    JobRunner::BuildSpec{}, JobRunner::Verify{}, JobRunner::Classify{}, [this] { return now; },
    JobRunner::Success{}, &installer);
    source.kind = "install";
    auto first = plans.create({.kind = "install", .packageId = "tree"}, 11);
    ASSERT_TRUE(first);
    auto first_id = enqueue(*first);
    ASSERT_EQ(runner->get({first_id}, 11)->state.value, "failed");
    source.kind = "manual-remove";
    auto removal = plans.create({.kind = "manual-remove", .receiptId = "receipt-a", .scope = protocol::ReceiptScope{"user"}}, 11);
    ASSERT_TRUE(removal);
    Gio::init();
    const auto id = enqueue(*removal);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "running");
    EXPECT_FALSE(runner->cancel(id, 11)->accepted);
    auto loop = Glib::MainLoop::create();
    auto monitor = Glib::signal_timeout().connect([&] {
        if (runner->get({id}, 11)->state.value == "running") return true;
        loop->quit();
        return false;
    }, 10);
    auto timeout = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, 5000);
    loop->run();
    monitor.disconnect();
    timeout.disconnect();
    EXPECT_EQ(installer.runs.load(), 1);
    EXPECT_EQ(processes.size(), 0);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "succeeded");
}

TEST_F(JobsFixture, UserRemovalWaitsForActiveProcess) {
    FakeManualInstaller installer;
    Gio::init();
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, JobRunner::Notify{}, state,
    ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"},
    JobRunner::BuildSpec{}, JobRunner::Verify{}, JobRunner::Classify{}, [this] { return now; },
    JobRunner::Success{}, &installer);
    const auto first = enqueue(plan());
    const auto removal = plans.create({.kind = "manual-remove", .receiptId = "receipt-a",
        .scope = protocol::ReceiptScope{"user"}}, 11);
    ASSERT_TRUE(removal);
    const auto second = enqueue(*removal);
    EXPECT_EQ(processes.size(), 1);
    EXPECT_EQ(installer.runs.load(), 0);
    EXPECT_EQ(runner->get({second}, 11)->state.value, "queued");
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "succeeded");
    auto loop = Glib::MainLoop::create();
    auto monitor = Glib::signal_timeout().connect([&] {
        if (runner->get({second}, 11)->state.value == "running") return true;
        loop->quit();
        return false;
    }, 10);
    auto timeout = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, 5000);
    loop->run();
    monitor.disconnect();
    timeout.disconnect();
    EXPECT_EQ(installer.runs.load(), 1);
    EXPECT_EQ(processes.size(), 1);
    EXPECT_EQ(runner->get({second}, 11)->state.value, "succeeded");
}

TEST_F(JobsFixture, FailedJobsRetainOnlyFiftySummaries) {
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, JobRunner::Notify{}, state, ElevationOptions{elevation::Method::Unavailable});
    std::string first;
    std::string last;
    for (int count = 0; count < 60; ++count) {
        last = enqueue(plan());
        if (count == 0) first = last;
    }
    EXPECT_EQ(runner->get({first}, 11).error().code, protocol::kInvalidParams);
    EXPECT_EQ(runner->get({last}, 11)->state.value, "failed");
    EXPECT_EQ(runner->list({.limit = 100}, 11)->jobs.size(), 50);
    EXPECT_TRUE(processes.empty());
}

TEST_F(JobsFixture, QueueIsFifoAndRechecksInputsAtHead) {
    const auto first = enqueue(plan("tree"));
    const auto second = enqueue(plan("nano"));
    const auto third = enqueue(plan("htop"));
    ASSERT_EQ(processes.size(), 1);
    source.fingerprint = "other-db";
    processes[0]->finish(0);
    EXPECT_EQ(processes.size(), 1);
    EXPECT_EQ(runner->get({second}, 11)->state.value, "needs-attention");
    EXPECT_EQ(runner->get({third}, 11)->state.value, "needs-attention");
    EXPECT_EQ(runner->get({first}, 11)->state.value, "succeeded");
    auto contents = core::read_bounded(state / "braurser/jobs.json", 8192);
    ASSERT_TRUE(contents);
    EXPECT_EQ(nlohmann::json::parse(*contents).size(), 3);
}

TEST_F(JobsFixture, ListingRejectsZeroLimitAndForeignCursor) {
    const auto id = enqueue(plan());
    EXPECT_EQ(runner->list({.limit = 0}, 11).error().code, protocol::kInvalidParams);
    EXPECT_EQ(runner->list({.cursor = id, .limit = 1}, 12).error().code, protocol::kInvalidParams);
    auto owned = runner->list({.limit = 1}, 11);
    ASSERT_TRUE(owned);
    ASSERT_EQ(owned->jobs.size(), 1);
    EXPECT_EQ(owned->jobs.front().jobId, id);
}

TEST_F(JobsFixture, PendingReviewCanBeCancelledAfterExpiry) {
    enqueue(plan("tree"));
    const auto waiting = enqueue(plan("nano"));
    protocol::QuestionChoice choice{protocol::BuildReviewQuestion{{{{"community"}, {}, "nano"}}}};
    runner->ask(waiting, choice, {"ack"});
    const auto notification = runner->get({waiting}, 11)->events.back()
        .payload.get<protocol::JobQuestionNotification>();
    now += std::chrono::minutes(10);
    EXPECT_EQ(runner->answer({waiting, notification.question.question_id, "ack", notification.question.token}, 11)
        .error().code, protocol::kExpiredQuestion);
    EXPECT_TRUE(runner->cancel(waiting, 11)->accepted);
    EXPECT_EQ(runner->get({waiting}, 11)->state.value, "cancelled");
}

TEST_F(JobsFixture, BuildReviewPreservesFifoUntilAnswered) {
    const auto first = enqueue(plan("tree"));
    const auto review = enqueue(plan("nano"));
    protocol::QuestionChoice choice{protocol::BuildReviewQuestion{{{{"community"}, {}, "nano"}}}};
    runner->ask(review, choice, {"ack"});
    const auto third = enqueue(plan("htop"));
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "succeeded");
    EXPECT_EQ(processes.size(), 1);
    EXPECT_EQ(runner->get({third}, 11)->state.value, "queued");
    const auto question = runner->get({review}, 11)->events.back()
        .payload.get<protocol::JobQuestionNotification>().question;
    ASSERT_TRUE(runner->answer({review, question.question_id, "ack", question.token}, 11));
    ASSERT_EQ(processes.size(), 2);
    EXPECT_EQ(runner->get({review}, 11)->state.value, "running");
    EXPECT_EQ(runner->get({third}, 11)->state.value, "queued");
    processes[1]->finish(0);
    ASSERT_EQ(processes.size(), 3);
    EXPECT_EQ(runner->get({third}, 11)->state.value, "running");
}

TEST_F(JobsFixture, CancellingBuildReviewReleasesNextJob) {
    const auto first = enqueue(plan("tree"));
    const auto review = enqueue(plan("nano"));
    protocol::QuestionChoice choice{protocol::BuildReviewQuestion{{{{"community"}, {}, "nano"}}}};
    runner->ask(review, choice, {"ack"});
    const auto third = enqueue(plan("htop"));
    processes.front()->finish(0);
    ASSERT_EQ(processes.size(), 1);
    ASSERT_TRUE(runner->cancel(review, 11)->accepted);
    EXPECT_EQ(runner->get({review}, 11)->state.value, "cancelled");
    ASSERT_EQ(processes.size(), 2);
    EXPECT_EQ(runner->get({third}, 11)->state.value, "running");
}

TEST_F(JobsFixture, UnsupportedQuestionTypesDoNotAdvanceQueue) {
    enqueue(plan("tree"));
    const auto pending = enqueue(plan("nano"));
    runner->ask(pending, protocol::QuestionChoice{protocol::GitRedirectQuestion{"a", "b"}}, {"allow"});
    EXPECT_EQ(runner->get({pending}, 11)->state.value, "queued");
    EXPECT_EQ(processes.size(), 1);
}

TEST_F(JobsFixture, CancellationAllowedBeforeCommitButRefusedAfter) {
    const auto first = enqueue(plan("tree"));
    processes[0]->line(":: Retrieving packages...");
    auto cancelled = runner->cancel(first, 11);
    ASSERT_TRUE(cancelled);
    EXPECT_TRUE(cancelled->accepted);
    EXPECT_EQ(processes[0]->interruptions, 1);
    processes[0]->finish(130);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "cancelled");
    const auto second = enqueue(plan("nano"));
    processes[1]->line("checking for file conflicts...");
    auto refused = runner->cancel(second, 11);
    ASSERT_TRUE(refused);
    EXPECT_FALSE(refused->accepted);
    EXPECT_EQ(refused->reason, "activity.cannot_cancel");
    EXPECT_EQ(processes[1]->interruptions, 0);
    processes[1]->finish(0);
    EXPECT_EQ(runner->get({second}, 11)->state.value, "succeeded");
}

TEST_F(JobsFixture, CancellationThatReachesCommitNeedsAttention) {
    const auto id = enqueue(plan());
    ASSERT_TRUE(runner->cancel(id, 11)->accepted);
    processes.front()->line(":: Processing package changes...");
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "needs-attention");
}

TEST_F(JobsFixture, DisconnectOrphansQuestionAndReleasesFollowingJob) {
    const auto first = enqueue(plan("tree"));
    const auto review = enqueue(plan("nano"));
    protocol::QuestionChoice choice{protocol::BuildReviewQuestion{{{{"community"}, {}, "nano"}}}};
    runner->ask(review, choice, {"ack"});
    const auto question = runner->get({review}, 11)->events.back()
        .payload.get<protocol::JobQuestionNotification>().question;
    const auto next = enqueue(plan("htop"));
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "succeeded");
    EXPECT_EQ(processes.size(), 1U);
    runner->disconnect(11);
    EXPECT_EQ(runner->answer({review, question.question_id, "ack", question.token}, 11)
        .error().code, protocol::kExpiredQuestion);
    EXPECT_EQ(runner->answer({review, question.question_id, "ack", question.token}, 12)
        .error().code, protocol::kExpiredQuestion);
    EXPECT_EQ(runner->get({review}, 11)->state.value, "needs-attention");
    EXPECT_EQ(processes.size(), 2U);
    EXPECT_EQ(runner->get({next}, 11)->state.value, "running");
}

TEST_F(JobsFixture, QuestionsAreBoundSingleUseAndExpire) {
    enqueue(plan("tree"));
    const auto queued = enqueue(plan("nano"));
    protocol::QuestionChoice choice{protocol::BuildReviewQuestion{{{{"community"}, {}, "nano"}}}};
    runner->ask(queued, choice, {"ack"});
    const auto events = runner->get({queued}, 11)->events;
    const auto question = events.back().payload.get<protocol::JobQuestionNotification>().question;
    protocol::JobsAnswerParams answer{queued, question.question_id, "ack", question.token};
    EXPECT_EQ(runner->answer(answer, 12).error().code, protocol::kExpiredQuestion);
    EXPECT_TRUE(runner->answer(answer, 11));
    EXPECT_EQ(runner->answer(answer, 11).error().code, protocol::kExpiredQuestion);
    runner->ask(queued, choice, {"ack"});
    const auto new_question = runner->get({queued}, 11)->events.back().payload.get<protocol::JobQuestionNotification>().question;
    answer.questionId = new_question.question_id;
    answer.questionToken = new_question.token;
    now += std::chrono::minutes(10);
    EXPECT_EQ(runner->answer(answer, 11).error().code, protocol::kExpiredQuestion);
}

class NoRun final : public core::ProcessRunner {
public:
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec&) override {
        return std::unexpected(core::ProcessError{1, "not used"});
    }
};

TEST_F(JobsFixture, CommunityMarkerVersionAndUnexpectedPromptFailClosed) {
    source.helper = "paru";
    NoRun core_runner;
    helpers::HelperEnvironment env;
    env.home = "/home/builder";
    env.user = "builder";
    env.runtime_dir = "/run/user/1000";
    helpers::ParuHelper helper(env, core_runner);
    bool installed = false;
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, JobRunner::Notify{}, state,
    ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"},
    [this](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<core::ProcessSpec> {
        auto spec = official_spec("install", {"tree"}, auth);
        if (spec) spec->env.emplace_back("BRAURSER_JOB_DIR", (state / "job").string());
        return spec;
    }, [&installed](const Snapshot&) { return installed; },
    [&helper](std::string_view line) { return helper.classify_line(line); });
    ASSERT_TRUE(core::ensure_private_dir(state / "job"));
    const auto first = enqueue(plan());
    processes[0]->line("==> Making package: tree 1-1");
    processes[0]->finish(0);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "failed");
    installed = true;
    const auto second = enqueue(plan());
    processes[1]->line("Enter a number (default=1):");
    EXPECT_EQ(processes[1]->kills, 1);
    processes[1]->finish(1);
    EXPECT_EQ(runner->get({second}, 11)->state.value, "needs-attention");
    const auto third = enqueue(plan());
    ASSERT_TRUE(core::atomic_write(state / "job/auth-failed", "", 0600));
    processes[2]->finish(0);
    EXPECT_EQ(runner->get({third}, 11)->state.value, "failed");
    const auto events = runner->get({third}, 11)->events;
    EXPECT_NE(std::ranges::find_if(events, [](const auto& event) {
        return event.kind == "job.authState";
    }), events.end());
}

TEST(JobsManual, ReceiptPlanIsReadOnlyBoundedAndPrivate) {
    const auto state = std::filesystem::temp_directory_path() /
        ("braurser-manual-plan-" + std::to_string(getpid()));
    const auto receipts = state / "braurser/manual/receipts";
    ASSERT_TRUE(core::ensure_private_dir(receipts));
    nlohmann::json data{{"id", "receipt-1"}, {"name", "widget"},
        {"url", "https://example.org/widget"}, {"host", "example.org"},
        {"commit", std::string(40, 'a')}, {"entry_point", "install.sh"},
        {"scope", "user"}, {"installed_at", 1},
        {"files", nlohmann::json::array({{{"path", "bin/widget"},
            {"sha256", std::string(64, '0')}, {"mode", 0700}}})},
        {"backups", nlohmann::json::array()}, {"launcher_ids", nlohmann::json::array()}};
    const auto path = receipts / "receipt-1.json";
    ASSERT_TRUE(core::atomic_write(path, data.dump(), 0600));
    auto plan = user_removal_snapshot(state, "receipt-1");
    ASSERT_TRUE(plan) << plan.error().message_key;
    EXPECT_EQ(plan->plan.kind.value, "manual-remove");
    EXPECT_EQ(plan->plan.removals.front().source.value, "git");
    ASSERT_TRUE(plan->manual_receipt);
    EXPECT_EQ(plan->manual_receipt->files.front().path, "bin/widget");
    EXPECT_EQ(plan->fingerprint, data.dump());
    data["files"][0]["path"] = "../outside";
    ASSERT_TRUE(core::atomic_write(path, data.dump(), 0600));
    EXPECT_EQ(user_removal_snapshot(state, "receipt-1").error().code, protocol::kUntrustedInput);
    data["files"][0]["path"] = "bin/widget";
    ASSERT_TRUE(core::atomic_write(path, data.dump(), 0600));
    ASSERT_EQ(chmod(path.c_str(), 0644), 0);
    EXPECT_EQ(user_removal_snapshot(state, "receipt-1").error().code, protocol::kUntrustedInput);
    std::error_code error;
    std::filesystem::remove_all(state, error);
}

TEST(JobsManual, SavedUserReceiptCanBePlannedAndRemoved) {
    const auto root = std::filesystem::temp_directory_path() /
        ("braurser-manual-remove-" + std::to_string(getpid()));
    const auto state = root / "state";
    const auto home = root / "home";
    const auto stage = root / "stage";
    ASSERT_TRUE(core::ensure_private_dir(stage / "bin"));
    ASSERT_TRUE(core::ensure_private_dir(home));
    ASSERT_TRUE(core::ensure_private_dir(state));
    ASSERT_TRUE(core::atomic_write(stage / "bin/widget", "owned contents", 0700));
    manual::Receipt receipt;
    receipt.id = "receipt-1";
    receipt.name = "widget";
    receipt.url = "https://example.org/widget";
    receipt.host = "example.org";
    receipt.commit = std::string(40, 'a');
    receipt.entry_point = "install.sh";
    NoRun runner;
    auto installed = manual::install_user(runner, stage, home, state, receipt);
    ASSERT_TRUE(installed) << installed.error().code;
    auto planned = user_removal_snapshot(state, receipt.id);
    ASSERT_TRUE(planned) << planned.error().message_key;
    ASSERT_TRUE(planned->manual_receipt);
    ASSERT_EQ(planned->manual_receipt->files.size(), installed->files.size());
    EXPECT_EQ(planned->manual_receipt->files.front().sha256, installed->files.front().sha256);
    auto removed = manual::remove_user(runner, home, state, *planned->manual_receipt);
    ASSERT_TRUE(removed) << removed.error().code;
    EXPECT_TRUE(removed->modified_files.empty());
    EXPECT_FALSE(std::filesystem::exists(home / ".local/bin/widget"));
    EXPECT_FALSE(std::filesystem::exists(state / "braurser/manual/receipts/receipt-1.json"));
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST(JobsStream, GioSubprocessUsesExplicitEnvironmentAndStreamsLines) {
    Gio::init();
    GioStreamingProcess process;
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/printf";
    spec.args = {"%s\\n", "success"};
    spec.env = {{"LC_ALL", "C"}};
    spec.timeout = std::chrono::seconds(3);
    std::vector<std::string> lines;
    auto loop = Glib::MainLoop::create();
    std::optional<core::ProcessResult> exit;
    ASSERT_TRUE(process.start(spec, [&](std::string_view text) { lines.emplace_back(text); },
        [&](const core::ProcessResult& result) { exit = result; loop->quit(); }));
    auto timer = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, 5000);
    loop->run();
    timer.disconnect();
    ASSERT_TRUE(exit.has_value());
    EXPECT_EQ(exit->exit_code, 0);
    EXPECT_EQ(lines, std::vector<std::string>{"success"});
}

TEST(JobsStream, RejectsUnsafeEnvironmentAndLimitsCapturedOutput) {
    Gio::init();
    GioStreamingProcess refused;
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/printf";
    spec.args = {"12345678"};
    spec.env = {{std::string("BAD\0KEY", 7), "value"}};
    spec.timeout = std::chrono::seconds(3);
    EXPECT_FALSE(refused.start(spec, [](std::string_view) {}, [](const core::ProcessResult&) {}));
    spec.env = {{"PATH", "/usr/bin"}, {"PATH", "/bin"}};
    EXPECT_FALSE(refused.start(spec, [](std::string_view) {}, [](const core::ProcessResult&) {}));
    spec.env = {{"LC_ALL", "C"}};
    spec.max_stdout = 4;
    GioStreamingProcess bounded;
    auto loop = Glib::MainLoop::create();
    std::optional<core::ProcessResult> exit;
    ASSERT_TRUE(bounded.start(spec, [](std::string_view) {}, [&](const core::ProcessResult& result) {
        exit = result;
        loop->quit();
    }));
    auto timer = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, 5000);
    loop->run();
    timer.disconnect();
    ASSERT_TRUE(exit);
    EXPECT_TRUE(exit->out_truncated);
}

TEST(JobsFixtures, RecordedConsolePhasesAndPromptClassification) {
    NoRun runner;
    helpers::HelperEnvironment environment;
    environment.home = "/home/builder";
    environment.user = "builder";
    environment.runtime_dir = "/run/user/1000";
    helpers::ParuHelper helper(environment, runner);
    std::ifstream input(std::string(JOBS_FIXTURES) + "/console-run3.log");
    ASSERT_TRUE(input);
    bool in_job = false;
    std::vector<helpers::PhaseEvent> phases;
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with("=== paru aur_install_prefixed:")) { in_job = true; continue; }
        if (line.starts_with("=== exit=0 paru aur_install_prefixed")) break;
        if (!in_job) continue;
        if (const auto phase = helper.classify_line(line)) phases.push_back(*phase);
    }
    EXPECT_NE(std::ranges::find(phases, helpers::PhaseEvent::Resolving), phases.end());
    EXPECT_NE(std::ranges::find(phases, helpers::PhaseEvent::Downloading), phases.end());
    EXPECT_NE(std::ranges::find(phases, helpers::PhaseEvent::Building), phases.end());
    EXPECT_NE(std::ranges::find(phases, helpers::PhaseEvent::Installing), phases.end());
    EXPECT_NE(std::ranges::find(phases, helpers::PhaseEvent::Finishing), phases.end());
    EXPECT_EQ(helper.classify_line("Enter a number (default=1):"), helpers::PhaseEvent::UnexpectedPrompt);
    EXPECT_EQ(helper.classify_line(":: Proceed with installation? [Y/n]"), std::nullopt);
    EXPECT_EQ(safe_line("\x1b[31mhello\x1b[0m\x02world"), "helloworld");
    EXPECT_EQ(safe_line("Authorization: Bearer private-key"), "[REDACTED]");
    EXPECT_EQ(safe_line("https://user:password@example.org/archive"),
              "https://[REDACTED]@example.org/archive");
}

}
}
