#include <braurser/daemon/jobs/live.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <gtest/gtest.h>
#include <giomm/init.h>
#include <glibmm/main.h>
#include <atomic>
#include <charconv>
#include <csignal>
#include <glib.h>
#include <fstream>
#include <memory>
#include <set>
#include <unistd.h>

namespace braurser::daemon::jobs {
namespace {

template<class T, class Start>
Result<T> inline_result(Start start) {
    std::optional<Result<T>> result;
    start([&result](Result<T> value) { result = std::move(value); });
    EXPECT_TRUE(result.has_value());
    if (!result) throw std::runtime_error("inline completion was deferred");
    return std::move(*result);
}

Result<protocol::TransactionPlan> create_now(PlanManager& plans, const protocol::PlanParams& selection,
                                             api::ConnectionId owner) {
    return inline_result<protocol::TransactionPlan>([&](auto done) { plans.create(selection, owner, std::move(done)); });
}

Result<Snapshot> approve_now(PlanManager& plans, std::string_view id, std::string_view token, api::ConnectionId owner) {
    return inline_result<Snapshot>([&](auto done) { plans.approve(id, token, owner, std::move(done)); });
}

Result<protocol::JobIdResult> enqueue_now(JobRunner& runner, std::string_view id, std::string_view token,
                                         api::ConnectionId owner, std::string_view kind = {}) {
    return inline_result<protocol::JobIdResult>([&](auto done) { runner.enqueue(id, token, owner, kind, std::move(done)); });
}

class Source final : public PlanSource {
public:
    std::string fingerprint{"unchanged"};
    std::string inputs_token;
    bool inputs_throw{};
    std::string inputs(const protocol::PlanParams&) override {
        if (inputs_throw) throw std::runtime_error("inputs unavailable");
        return inputs_token;
    }
    std::string kind{"install"};
    std::string helper{"auto"};
    std::vector<std::string> review;
    std::vector<protocol::PackageRef> protected_blockers;
    std::vector<std::string> conflicts;
    Result<Snapshot> snapshot(const protocol::PlanParams& selection) override {
        Snapshot value;
        value.plan.kind = {selection.kind};
        value.plan.installs = {{{"official"}, "extra", selection.packageId.value_or("tree")}};
        value.plan.requires_build_review = std::ranges::find(review, selection.packageId.value_or("tree")) != review.end();
        if (value.plan.requires_build_review) value.plan.build_review_refs = value.plan.installs;
        value.plan.protected_blockers = protected_blockers;
        value.plan.conflicts = conflicts;
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
    void pipeline(JobRunner::BuildSpec build, JobRunner::Verify verify = {}) {
        runner = std::make_unique<JobRunner>(plans, [this] {
            auto process = std::make_shared<ProcessState>();
            processes.push_back(process);
            return std::make_unique<FakeProcess>(process);
        }, JobRunner::Notify{}, state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"},
        std::move(build), std::move(verify));
    }
    void TearDown() override {
        runner.reset();
        std::error_code error;
        std::filesystem::remove_all(state, error);
    }
    protocol::TransactionPlan plan(std::string name = "tree") {
        auto result = create_now(plans, protocol::PlanParams{.kind = source.kind, .packageId = std::move(name)}, 11);
        EXPECT_TRUE(result.has_value());
        return result.value();
    }
    std::string enqueue(const protocol::TransactionPlan& approved) {
        auto result = enqueue_now(*runner, approved.plan_id, approved.approval_token, 11);
        EXPECT_TRUE(result.has_value());
        return result->jobId;
    }
};

class DeferredPlanExecutor final : public PlanExecutor {
public:
    struct Pending {
        Ticket ticket;
        PlanLane lane;
        PlanTask task;
        Done done;
        bool cancelled{};
    };
    std::deque<Pending> pending;
    std::size_t cancellations{};
    bool refuse{};
    Ticket submit(PlanLane lane, PlanTask task, Done done) override {
        if (stopped_ || refuse) return 0;
        const auto ticket = ++next_;
        pending.push_back({ticket, lane, std::move(task), std::move(done), false});
        return ticket;
    }
    void cancel(Ticket ticket) override {
        for (auto& item : pending) {
            if (item.ticket != ticket) continue;
            item.cancelled = true;
            ++cancellations;
        }
    }
    void stop() override { stopped_ = true; pending.clear(); }
    void run_next(std::optional<int> error = {}) {
        ASSERT_FALSE(pending.empty());
        auto it = std::ranges::find_if(pending, [](const auto& item) { return item.lane == PlanLane::Approval; });
        if (it == pending.end()) it = pending.begin();
        auto item = std::move(*it);
        pending.erase(it);
        const std::atomic<bool> cancelled{item.cancelled};
        if (item.cancelled) item.done(std::unexpected(Error{protocol::kCancelled, "error.generic_title"}));
        else if (error) item.done(std::unexpected(Error{*error, "error.generic_title"}));
        else item.done(item.task(cancelled));
    }
    void run_all() { while (!pending.empty()) run_next(); }
private:
    Ticket next_{};
    bool stopped_{};
};

class JobsAsyncFixture : public JobsFixture {
protected:
    DeferredPlanExecutor executor;
    std::set<api::ConnectionId> closing;
    PlanManager async_plans{source, [this] { return now; }, &executor,
        [this](api::ConnectionId owner) { return !closing.contains(owner); }};
    using PlanResult = std::optional<Result<protocol::TransactionPlan>>;
    using JobResult = std::shared_ptr<std::optional<Result<protocol::JobIdResult>>>;

    void SetUp() override {
        JobsFixture::SetUp();
        runner = std::make_unique<JobRunner>(async_plans, [this] {
            auto process = std::make_shared<ProcessState>();
            processes.push_back(process);
            return std::make_unique<FakeProcess>(process);
        }, JobRunner::Notify{}, state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"},
        JobRunner::BuildSpec{}, JobRunner::Verify{}, JobRunner::Classify{}, [this] { return now; });
    }
    void TearDown() override {
        executor.stop();
        JobsFixture::TearDown();
    }
    protocol::TransactionPlan prepare(std::string name = "tree", api::ConnectionId owner = 11) {
        PlanResult result;
        async_plans.create({.kind = "install", .packageId = std::move(name)}, owner,
            [&result](auto value) { result = std::move(value); });
        executor.run_all();
        EXPECT_TRUE(result && result->has_value());
        return result->value();
    }
    JobResult submit(const protocol::TransactionPlan& value, api::ConnectionId owner = 11) {
        auto result = std::make_shared<std::optional<Result<protocol::JobIdResult>>>();
        runner->enqueue(value.plan_id, value.approval_token, owner, "install",
            [result](auto value_result) { *result = std::move(value_result); });
        return result;
    }
};

TEST_F(JobsAsyncFixture, ClosingOwnerCancelsCreateBeforeDisconnectCallback) {
    PlanResult result;
    int completions = 0;
    async_plans.create({.kind = "install", .packageId = "tree"}, 11, [&](auto value) {
        ++completions;
        result = std::move(value);
    });
    closing.insert(11);
    executor.run_all();
    ASSERT_TRUE(result);
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, protocol::kCancelled);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(approve_now(async_plans, "any-plan", "any-token", 11).error().code, protocol::kStalePlan);
    EXPECT_TRUE(processes.empty());
    EXPECT_FALSE(prepare("nano", 12).approval_token.empty());
}

TEST_F(JobsAsyncFixture, ClosingOwnerCancelsApprovalBeforeDisconnectCallback) {
    const auto value = prepare();
    const auto result = submit(value);
    closing.insert(11);
    executor.run_all();
    ASSERT_TRUE(*result);
    ASSERT_FALSE(result->value().has_value());
    EXPECT_EQ(result->value().error().code, protocol::kCancelled);
    EXPECT_TRUE(runner->list({.limit = 100}, 11)->jobs.empty());
    EXPECT_TRUE(processes.empty());
    EXPECT_EQ(approve_now(async_plans, value.plan_id, value.approval_token, 11).error().code, protocol::kStalePlan);
    const auto live = submit(prepare("nano", 12), 12);
    executor.run_all();
    ASSERT_TRUE(*live && live->value().has_value());
    EXPECT_EQ(runner->get({live->value()->jobId}, 12)->state.value, "running");
    EXPECT_EQ(processes.size(), 1U);
}

TEST_F(JobsAsyncFixture, ChangedInputsRejectCreateWithoutStoringAnEntry) {
    PlanResult result;
    async_plans.create({.kind = "install", .packageId = "tree"}, 11,
        [&](auto value) { result = std::move(value); });
    source.inputs_token = "replacement";
    executor.run_all();
    ASSERT_TRUE(result);
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, protocol::kStalePlan);
    EXPECT_EQ(approve_now(async_plans, "any-plan", "any-token", 11).error().code, protocol::kStalePlan);
    EXPECT_TRUE(processes.empty());
    EXPECT_FALSE(prepare().approval_token.empty());
}

TEST_F(JobsAsyncFixture, ChangedInputsRejectApprovalWithoutCreatingJob) {
    const auto value = prepare();
    const auto result = submit(value);
    source.inputs_token = "replacement";
    executor.run_all();
    ASSERT_TRUE(*result);
    ASSERT_FALSE(result->value().has_value());
    EXPECT_EQ(result->value().error().code, protocol::kStalePlan);
    EXPECT_TRUE(runner->list({.limit = 100}, 11)->jobs.empty());
    EXPECT_TRUE(processes.empty());
    EXPECT_EQ(approve_now(async_plans, value.plan_id, value.approval_token, 11).error().code, protocol::kStalePlan);
}

TEST_F(JobsAsyncFixture, ChangedHeadInputsReleaseNextJobWithoutStartingStaleJob) {
    const auto first = prepare("tree");
    const auto second = prepare("nano");
    const auto one = submit(first);
    const auto two = submit(second);
    executor.run_next();
    executor.run_next();
    ASSERT_TRUE(*one && one->value().has_value());
    ASSERT_TRUE(*two && two->value().has_value());
    EXPECT_TRUE(processes.empty());
    source.inputs_token = "replacement";
    executor.run_next();
    EXPECT_EQ(runner->get({one->value()->jobId}, 11)->state.value, "needs-attention");
    EXPECT_TRUE(processes.empty());
    executor.run_all();
    EXPECT_EQ(runner->get({two->value()->jobId}, 11)->state.value, "running");
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes.front()->spec.args.back(), "nano");
}

TEST_F(JobsAsyncFixture, InputExceptionsRejectSuccessfulCompletions) {
    const auto approved = prepare();
    const auto queued = prepare("nano");
    const auto head = submit(queued);
    executor.run_next();
    ASSERT_TRUE(*head && head->value().has_value());
    const auto approval = submit(approved);
    PlanResult created;
    async_plans.create({.kind = "install", .packageId = "htop"}, 11,
        [&](auto value) { created = std::move(value); });
    source.inputs_throw = true;
    executor.run_all();
    ASSERT_TRUE(created);
    ASSERT_FALSE(created->has_value());
    EXPECT_EQ(created->error().code, protocol::kStalePlan);
    ASSERT_TRUE(*approval);
    ASSERT_FALSE(approval->value().has_value());
    EXPECT_EQ(approval->value().error().code, protocol::kStalePlan);
    EXPECT_EQ(runner->get({head->value()->jobId}, 11)->state.value, "needs-attention");
    EXPECT_TRUE(processes.empty());
}

TEST_F(JobsAsyncFixture, InputExceptionsBeforePrepareFailClosed) {
    const auto value = prepare();
    source.inputs_throw = true;
    EXPECT_EQ(create_now(async_plans, {.kind = "install", .packageId = "tree"}, 11).error().code,
        protocol::kStalePlan);
    EXPECT_EQ(approve_now(async_plans, value.plan_id, value.approval_token, 11).error().code, protocol::kStalePlan);
    bool checked = true;
    EXPECT_EQ(async_plans.unchanged({}, {.kind = "install", .packageId = "tree"},
        [&](bool matches) { checked = matches; }), 0U);
    EXPECT_FALSE(checked);
    EXPECT_TRUE(executor.pending.empty());
}

TEST_F(JobsAsyncFixture, DisconnectCancelsCreateAndNeverStoresAnEntry) {
    PlanResult result;
    int completions = 0;
    async_plans.create({.kind = "install", .packageId = "tree"}, 11, [&](auto value) {
        ++completions;
        result = std::move(value);
    });
    EXPECT_FALSE(result);
    async_plans.disconnect(11);
    EXPECT_FALSE(result);
    executor.run_all();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->error().code, protocol::kCancelled);
    EXPECT_EQ(completions, 1);
    EXPECT_EQ(approve_now(async_plans, "any-plan", "any-token", 11).error().code, protocol::kStalePlan);
    EXPECT_TRUE(processes.empty());
}

TEST_F(JobsAsyncFixture, DisconnectCancelsApprovalAndReleasesCapacity) {
    std::vector<protocol::TransactionPlan> values;
    for (api::ConnectionId index = 0; index < 256; ++index)
        values.push_back(prepare("pkg" + std::to_string(index), 11 + index));
    std::vector<JobResult> results;
    for (std::size_t index = 0; index < values.size(); ++index)
        results.push_back(submit(values[index], 11 + static_cast<api::ConnectionId>(index)));
    EXPECT_EQ(executor.pending.size(), 256U);
    for (api::ConnectionId owner = 11; owner < 267; ++owner) {
        async_plans.disconnect(owner);
        runner->disconnect(owner);
    }
    executor.run_all();
    for (const auto& result : results) {
        ASSERT_TRUE(*result);
        EXPECT_EQ(result->value().error().code, protocol::kCancelled);
    }
    for (api::ConnectionId owner = 11; owner < 267; ++owner) EXPECT_TRUE(runner->list({.limit = 100}, owner)->jobs.empty());
    EXPECT_TRUE(processes.empty());
    const auto next = submit(prepare("other", 900), 900);
    executor.run_all();
    ASSERT_TRUE(*next && next->value().has_value());
    EXPECT_EQ(runner->get({next->value()->jobId}, 900)->state.value, "running");
    EXPECT_EQ(processes.size(), 1U);
}

TEST_F(JobsAsyncFixture, PerOwnerLimitIncludesCreatesAndApprovals) {
    const auto first = prepare();
    const auto approval = submit(first);
    std::vector<PlanResult> results(5);
    for (std::size_t index = 0; index < 4; ++index)
        async_plans.create({.kind = "install", .packageId = "nano"}, 11,
            [&, index](auto result) { results[index] = std::move(result); });
    ASSERT_TRUE(results[3]);
    EXPECT_EQ(results[3]->error().code, protocol::kBusy);
    async_plans.create({.kind = "install", .packageId = "htop"}, 12,
        [&](auto result) { results[4] = std::move(result); });
    EXPECT_FALSE(results[4]);
    executor.run_all();
    EXPECT_TRUE(*approval && approval->value().has_value());
    for (std::size_t index = 0; index < 3; ++index) EXPECT_TRUE(results[index] && results[index]->has_value());
    EXPECT_TRUE(results[4] && results[4]->has_value());
}

TEST_F(JobsAsyncFixture, FifthConcurrentCreateIsBusyButAnotherOwnerSucceeds) {
    std::vector<PlanResult> results(6);
    for (std::size_t index = 0; index < 6; ++index)
        async_plans.create({.kind = "install", .packageId = "tree"}, index == 5 ? 12 : 11,
            [&, index](auto result) { results[index] = std::move(result); });
    ASSERT_TRUE(results[4]);
    EXPECT_EQ(results[4]->error().code, protocol::kBusy);
    EXPECT_FALSE(results[5]);
    executor.run_all();
    EXPECT_TRUE(results[5] && results[5]->has_value());
}

TEST_F(JobsAsyncFixture, ApprovalSpendsTokenBeforeItsRecheck) {
    const auto value = prepare();
    std::optional<Result<Snapshot>> first;
    async_plans.approve(value.plan_id, value.approval_token, 11, [&](auto result) { first = std::move(result); });
    EXPECT_FALSE(first);
    EXPECT_EQ(approve_now(async_plans, value.plan_id, value.approval_token, 11).error().code, protocol::kStalePlan);
    executor.run_all();
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->has_value());
}

TEST_F(JobsAsyncFixture, HeadRecheckWaitsThenStaleHeadReleasesNextCheck) {
    const auto first = prepare("tree");
    const auto second = prepare("nano");
    const auto one = submit(first);
    const auto two = submit(second);
    executor.run_next();
    ASSERT_TRUE(*one && one->value().has_value());
    const auto first_id = one->value()->jobId;
    EXPECT_EQ(runner->get({first_id}, 11)->state.value, "queued");
    EXPECT_TRUE(processes.empty());
    executor.run_next();
    ASSERT_TRUE(*two && two->value().has_value());
    executor.run_next(protocol::kBusy);
    EXPECT_EQ(runner->get({first_id}, 11)->state.value, "needs-attention");
    EXPECT_TRUE(processes.empty());
    const auto second_id = two->value()->jobId;
    EXPECT_EQ(runner->get({second_id}, 11)->state.value, "queued");
    EXPECT_TRUE(processes.empty());
    executor.run_next();
    EXPECT_EQ(runner->get({second_id}, 11)->state.value, "running");
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes.front()->spec.args.back(), "nano");
}

TEST_F(JobsAsyncFixture, CancelDuringHeadCheckNeverStartsCancelledJob) {
    const auto first = prepare("tree");
    const auto second = prepare("nano");
    const auto one = submit(first);
    const auto two = submit(second);
    executor.run_next();
    ASSERT_TRUE(*one && one->value().has_value());
    const auto id = one->value()->jobId;
    EXPECT_TRUE(runner->cancel(id, 11)->accepted);
    EXPECT_EQ(executor.cancellations, 1U);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "cancelled");
    EXPECT_TRUE(processes.empty());
    executor.run_all();
    EXPECT_EQ(runner->get({id}, 11)->state.value, "cancelled");
    ASSERT_TRUE(*two && two->value().has_value());
    EXPECT_EQ(runner->get({two->value()->jobId}, 11)->state.value, "running");
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes.front()->spec.args.back(), "nano");
}

TEST_F(JobsAsyncFixture, ApprovalsAndChecksPreserveFifo) {
    const auto first = prepare("tree");
    const auto second = prepare("nano");
    const auto one = submit(first);
    const auto two = submit(second);
    executor.run_all();
    ASSERT_TRUE(*one && one->value().has_value());
    ASSERT_TRUE(*two && two->value().has_value());
    EXPECT_EQ(runner->get({one->value()->jobId}, 11)->state.value, "running");
    EXPECT_EQ(runner->get({two->value()->jobId}, 11)->state.value, "queued");
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes.front()->spec.args.back(), "tree");
    processes.front()->finish(0);
    EXPECT_EQ(processes.size(), 1U);
    EXPECT_EQ(runner->get({two->value()->jobId}, 11)->state.value, "queued");
    executor.run_all();
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(processes.back()->spec.args.back(), "nano");
}

TEST_F(JobsAsyncFixture, StopIgnoresLateApprovalAndHeadCheckCompletions) {
    const auto first = prepare("tree");
    const auto second = prepare("nano");
    const auto one = submit(first);
    executor.run_next();
    ASSERT_TRUE(*one && one->value().has_value());
    const auto two = submit(second);
    EXPECT_EQ(executor.pending.size(), 2U);
    runner->stop();
    executor.run_next();
    EXPECT_FALSE(*two);
    EXPECT_TRUE(processes.empty());
    EXPECT_EQ(runner->get({one->value()->jobId}, 11)->state.value, "queued");
    runner.reset();
    executor.run_all();
    EXPECT_FALSE(*two);
    EXPECT_TRUE(processes.empty());
}

TEST_F(JobsAsyncFixture, HeadFingerprintChangeFailsClosed) {
    const auto value = prepare();
    const auto result = submit(value);
    executor.run_next();
    ASSERT_TRUE(*result && result->value().has_value());
    const auto id = result->value()->jobId;
    source.fingerprint = "changed";
    executor.run_next();
    EXPECT_EQ(runner->get({id}, 11)->state.value, "needs-attention");
    EXPECT_TRUE(processes.empty());
}

TEST_F(JobsAsyncFixture, RefusedHeadCheckDoesNotStartAProcess) {
    const auto value = prepare();
    const auto result = submit(value);
    executor.refuse = true;
    executor.run_next();
    ASSERT_TRUE(*result && result->value().has_value());
    EXPECT_EQ(runner->get({result->value()->jobId}, 11)->state.value, "needs-attention");
    EXPECT_TRUE(processes.empty());
}

TEST_F(JobsAsyncFixture, PurgingUsedEntryDoesNotInvalidateApprovalSnapshot) {
    const auto value = prepare();
    std::optional<Result<Snapshot>> approved;
    async_plans.approve(value.plan_id, value.approval_token, 11, [&](auto result) { approved = std::move(result); });
    PlanResult created;
    async_plans.create({.kind = "install", .packageId = "nano"}, 12, [&](auto result) { created = std::move(result); });
    executor.run_all();
    ASSERT_TRUE(approved && approved->has_value());
    EXPECT_EQ(approved->value().targets, std::vector<std::string>{"tree"});
    EXPECT_TRUE(created && created->has_value());
}

TEST_F(JobsAsyncFixture, DisconnectDropsOnlyItsOwnersUnapprovedEntries) {
    const auto first = prepare();
    const auto second = prepare("nano", 12);
    async_plans.disconnect(11);
    EXPECT_EQ(approve_now(async_plans, first.plan_id, first.approval_token, 11).error().code, protocol::kStalePlan);
    const auto other = submit(second, 12);
    executor.run_all();
    ASSERT_TRUE(*other && other->value().has_value());
    EXPECT_EQ(processes.size(), 1U);
}

TEST_F(JobsAsyncFixture, RefusedSubmissionCompletesOnceAndReleasesFlight) {
    executor.refuse = true;
    for (int index = 0; index < 5; ++index) {
        int completions = 0;
        async_plans.create({.kind = "install", .packageId = "tree"}, 11, [&](auto result) {
            ++completions;
            EXPECT_EQ(result.error().code, protocol::kBusy);
        });
        EXPECT_EQ(completions, 1);
    }
    executor.refuse = false;
    const auto value = prepare();
    executor.refuse = true;
    const auto result = submit(value);
    ASSERT_TRUE(*result);
    EXPECT_EQ(result->value().error().code, protocol::kBusy);
    EXPECT_TRUE(processes.empty());
}

TEST(JobsSpec, CanonicalOfficialTransactionsForBothElevationMethods) {
    ElevationOptions auth{elevation::Method::Pkexec, "/usr/local/libexec/braurser"};
    const auto helper = "/usr/local/libexec/braurser/braurser-pacman-helper";
    struct Case { std::string kind; std::vector<std::string> targets; std::vector<std::string> args; };
    for (const auto& value : std::vector<Case>{
        {"install", {"extra/tree"}, {"--sync", "--needed", "--noconfirm", "--", "extra/tree"}},
        {"update-system", {}, {"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--braurser-transaction=" + std::string(64, 'a')}},
        {"update-and-install", {"tree"}, {"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--needed", "--braurser-transaction=" + std::string(64, 'a'), "--", "tree"}},
        {"install-deps", {"tree"}, {"--sync", "--needed", "--asdeps", "--noconfirm", "--", "tree"}},
        {"mark-deps", {"tree"}, {"--database", "--asdeps", "--noconfirm", "--", "tree"}},
        {"install-archives", {"/home/user/.cache/braurser/build/manual/job/packages/tree-1-1-any.pkg.tar.zst"},
            {"--upgrade", "--noconfirm", "--", "/home/user/.cache/braurser/build/manual/job/packages/tree-1-1-any.pkg.tar.zst"}},
        {"remove", {"tree"}, {"--remove", "--nosave", "--recursive", "--noconfirm", "--", "tree"}},
        {"manual-install", {"/home/user/.cache/braurser/build/manual/job/pkg/pkg-1.pkg.tar.zst"},
            {"--upgrade", "--noconfirm", "--", "/home/user/.cache/braurser/build/manual/job/pkg/pkg-1.pkg.tar.zst"}}}) {
        auto spec = official_spec(value.kind, value.targets, auth, std::string(64, 'a'));
        ASSERT_TRUE(spec.has_value()) << value.kind;
        EXPECT_EQ(spec->executable, "/usr/bin/pkexec");
        std::vector<std::string> expected{helper};
        expected.insert(expected.end(), value.args.begin(), value.args.end());
        EXPECT_EQ(spec->args, expected);
        auth.method = elevation::Method::SudoAskpass;
        auth.ui = "qt";
        spec = official_spec(value.kind, value.targets, auth, std::string(64, 'a'));
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
    EXPECT_EQ(official_spec("update-system", {"tree"}, auth, std::string(64, 'a')).error().code,
              protocol::kInvalidParams);
}

TEST(JobsSpec, SystemTransactionsRequireExactReviewedDigest) {
    const ElevationOptions auth{elevation::Method::Pkexec, "/usr/local/libexec/braurser"};
    for (const auto* kind : {"update-system", "upgrade-system", "update-and-install"}) {
        const auto targets = std::string_view(kind) == "update-and-install" ? std::vector<std::string>{"tree"} : std::vector<std::string>{};
        for (const auto& digest : {std::string{}, std::string(63, 'a'), std::string(64, 'A'), std::string(64, 'g')})
            EXPECT_EQ(official_spec(kind, targets, auth, digest).error().code, protocol::kStalePlan);
        EXPECT_TRUE(official_spec(kind, targets, auth, std::string(64, 'a')));
    }
}

TEST_F(JobsFixture, PipelineStartsLazilyAndVerifiesOnlyAfterLastSuccess) {
    int produced = 0;
    int verified = 0;
    pipeline([&](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Pipeline value;
        for (int index = 0; index < 3; ++index) value.steps.push_back([&, index]() -> Result<core::ProcessSpec> {
            ++produced;
            core::ProcessSpec spec;
            spec.executable = "/usr/bin/makepkg";
            spec.args = {std::to_string(index)};
            return spec;
        });
        return value;
    }, [&](const Snapshot&) { ++verified; return true; });
    const auto id = enqueue(plan());
    ASSERT_EQ(produced, 1);
    processes[0]->finish(0);
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(verified, 0);
    EXPECT_EQ(processes[1]->spec.args, std::vector<std::string>{"1"});
    processes[1]->finish(0);
    ASSERT_EQ(processes.size(), 3U);
    processes[2]->finish(0);
    EXPECT_EQ(produced, 3);
    EXPECT_EQ(verified, 1);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "succeeded");
}

TEST_F(JobsFixture, PipelineStopsAtFailedMiddleStep) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Step step = []() -> Result<core::ProcessSpec> { core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec; };
        return JobRunner::Pipeline{{step, step, step}};
    });
    const auto id = enqueue(plan());
    processes[0]->finish(0);
    processes[1]->finish(1);
    EXPECT_EQ(processes.size(), 2U);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "failed");
}

TEST_F(JobsFixture, PipelineDoesNotAdvanceOnSignalsTimeoutsOrTruncation) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Step step = []() -> Result<core::ProcessSpec> { core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec; };
        return JobRunner::Pipeline{{step, step}};
    });
    for (unsigned flag = 0; flag < 4; ++flag) {
        const auto id = enqueue(plan());
        ASSERT_EQ(processes.size(), static_cast<std::size_t>(flag + 1));
        core::ProcessResult result;
        result.exit_code = 0;
        result.signaled = flag == 0;
        result.timed_out = flag == 1;
        result.out_truncated = flag == 2;
        result.err_truncated = flag == 3;
        processes.back()->done(result);
        EXPECT_EQ(runner->get({id}, 11)->state.value, "failed");
        EXPECT_EQ(processes.size(), static_cast<std::size_t>(flag + 1));
    }
}

TEST_F(JobsFixture, PipelineCancellationBetweenStepsDoesNotStartRootHelper) {
    std::string id;
    pipeline([&](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[]() -> Result<core::ProcessSpec> {
            core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
        }, [&, auth]() -> Result<core::ProcessSpec> {
            EXPECT_TRUE(runner->cancel(id, 11)->accepted);
            return official_spec("install-deps", {"tree"}, auth);
        }}};
    });
    id = enqueue(plan());
    processes[0]->finish(0);
    EXPECT_EQ(processes.size(), 1U);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "cancelled");
}

TEST_F(JobsFixture, PipelineRefusesCancellationOnceRootStepStarts) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[]() -> Result<core::ProcessSpec> {
            core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
        }, [auth] { return official_spec("install-deps", {"tree"}, auth); }}};
    });
    const auto id = enqueue(plan());
    processes[0]->finish(0);
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_FALSE(runner->cancel(id, 11)->accepted);
    processes[1]->finish(0);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "succeeded");
}

TEST_F(JobsFixture, PipelineCommitGuardRearmsForLaterRootStep) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[auth] { return official_spec("install-deps", {"tree"}, auth); },
            []() -> Result<core::ProcessSpec> {
                core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
            }, [auth] { return official_spec("mark-deps", {"tree"}, auth); }}};
    });
    const auto id = enqueue(plan());
    EXPECT_FALSE(runner->cancel(id, 11)->accepted);
    processes[0]->finish(0);
    processes[1]->finish(0);
    ASSERT_EQ(processes.size(), 3U);
    EXPECT_FALSE(runner->cancel(id, 11)->accepted);
    EXPECT_EQ(processes[2]->interruptions, 0);
    processes[2]->finish(0);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "succeeded");
}

TEST_F(JobsFixture, PipelineCancellationDuringBuildAfterRootSuccessNeedsAttention) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[auth] { return official_spec("install-deps", {"tree"}, auth); },
            []() -> Result<core::ProcessSpec> {
                core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
            }, [auth] { return official_spec("install-deps", {"nano"}, auth); }}};
    });
    const auto id = enqueue(plan());
    EXPECT_FALSE(runner->cancel(id, 11)->accepted);
    EXPECT_EQ(processes[0]->interruptions, 0);
    processes[0]->finish(0);
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_TRUE(runner->cancel(id, 11)->accepted);
    EXPECT_EQ(processes[1]->interruptions, 1);
    processes[1]->finish(130);
    EXPECT_EQ(processes.size(), 2U);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "needs-attention");
}

TEST_F(JobsFixture, PipelineCancellationDuringFirstBuildStaysCancelled) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[]() -> Result<core::ProcessSpec> {
                core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
            }, [auth] { return official_spec("install-deps", {"tree"}, auth); }}};
    });
    for (const auto code : {130, 0}) {
        const auto id = enqueue(plan());
        auto process = processes.back();
        process->line(":: Processing package changes...");
        process->line("checking for file conflicts...");
        EXPECT_TRUE(runner->cancel(id, 11)->accepted);
        EXPECT_EQ(process->interruptions, 1);
        process->finish(code);
        EXPECT_EQ(runner->get({id}, 11)->state.value, "cancelled");
    }
    EXPECT_EQ(processes.size(), 2U);
}

TEST_F(JobsFixture, PipelineCancellationBetweenStepsAfterRootSuccessNeedsAttention) {
    std::string id;
    pipeline([&](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[auth] { return official_spec("install-deps", {"tree"}, auth); },
            [&]() -> Result<core::ProcessSpec> {
                EXPECT_TRUE(runner->cancel(id, 11)->accepted);
                core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
            }}};
    });
    id = enqueue(plan());
    processes[0]->finish(0);
    EXPECT_EQ(processes.size(), 1U);
    EXPECT_EQ(runner->get({id}, 11)->state.value, "needs-attention");
}

TEST_F(JobsFixture, ChangedRootTransactionRequiresNewReview) {
    source.kind = "update-system";
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[auth] { return official_spec("update-system", {}, auth, std::string(64, 'a')); }}};
    });
    const auto id = enqueue(plan());
    processes[0]->finish(75);
    const auto job = runner->get({id}, 11);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->state.value, "needs-attention");
    ASSERT_TRUE(job->error);
    EXPECT_EQ(job->error->at("message_key"), "job.updates_changed");
}

TEST_F(JobsFixture, MissingSourceSigningKeyHasDedicatedFailure) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Pipeline value;
        value.steps.emplace_back([]() -> Result<core::ProcessSpec> {
            core::ProcessSpec spec; spec.executable = "/usr/bin/makepkg"; return spec;
        });
        return value;
    });
    const auto id = enqueue(plan());
    processes[0]->line("==> ERROR: unknown public key 0123456789ABCDEF");
    processes[0]->finish(1);
    const auto job = runner->get({id}, 11);
    ASSERT_TRUE(job);
    EXPECT_EQ(job->state.value, "failed");
    ASSERT_TRUE(job->error);
    EXPECT_EQ(job->error->at("message_key"), "job.unknown_public_key");
    EXPECT_TRUE(std::ranges::any_of(job->events, [](const auto& event) {
        return event.kind == "job.log" && event.payload.dump().find("haven't imported") != std::string::npos;
    }));
}

TEST(PlanApproval, ConnectionBindingSingleUseExpiryAndFreshness) {
    Source source;
    auto now = std::chrono::steady_clock::now();
    PlanManager plans(source, [&] { return now; });
    const protocol::PlanParams request{.kind = "install", .packageId = "tree"};
    auto plan = create_now(plans, request, 18);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->approval_token.size(), 32);
    EXPECT_EQ(approve_now(plans, plan->plan_id, plan->approval_token, 19).error().code, protocol::kStalePlan);
    EXPECT_EQ(approve_now(plans, plan->plan_id, "wrong", 18).error().code, protocol::kStalePlan);
    source.fingerprint = "changed";
    EXPECT_EQ(approve_now(plans, plan->plan_id, plan->approval_token, 18).error().code, protocol::kStalePlan);
    source.fingerprint = "unchanged";
    EXPECT_EQ(approve_now(plans, plan->plan_id, plan->approval_token, 18).error().code, protocol::kStalePlan);
    auto other = create_now(plans, request, 18);
    ASSERT_TRUE(other);
    ASSERT_TRUE(approve_now(plans, other->plan_id, other->approval_token, 18));
    EXPECT_EQ(approve_now(plans, other->plan_id, other->approval_token, 18).error().code, protocol::kStalePlan);
    other = create_now(plans, request, 18);
    now += std::chrono::minutes(10);
    EXPECT_EQ(approve_now(plans, other->plan_id, other->approval_token, 18).error().code, protocol::kStalePlan);
}

TEST(PlanApproval, ReclaimsUsedAndExpiredEntries) {
    Source source;
    auto now = std::chrono::steady_clock::now();
    PlanManager plans(source, [&] { return now; });
    const protocol::PlanParams selection{.kind = "install", .packageId = "tree"};
    auto first = create_now(plans, selection, 11);
    ASSERT_TRUE(first);
    ASSERT_TRUE(approve_now(plans, first->plan_id, first->approval_token, 11));
    for (int count = 0; count < 256; ++count) ASSERT_TRUE(create_now(plans, selection, 11));
    EXPECT_EQ(create_now(plans, selection, 11).error().code, protocol::kBusy);
    now += std::chrono::minutes(10);
    EXPECT_TRUE(create_now(plans, selection, 11));
}

TEST_F(JobsFixture, MutationEndpointMustMatchApprovedKind) {
    auto approved = plan("tree");
    auto rejected = enqueue_now(*runner, approved.plan_id, approved.approval_token, 11, "remove");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, protocol::kInvalidParams);
    EXPECT_TRUE(processes.empty());
    approved = plan("nano");
    ASSERT_TRUE(enqueue_now(*runner, approved.plan_id, approved.approval_token, 11, "install"));
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
    auto first = create_now(plans, {.kind = "install", .packageId = "tree"}, 11);
    ASSERT_TRUE(first);
    auto first_id = enqueue(*first);
    ASSERT_EQ(runner->get({first_id}, 11)->state.value, "failed");
    source.kind = "manual-remove";
    auto removal = create_now(plans, {.kind = "manual-remove", .receiptId = "receipt-a", .scope = protocol::ReceiptScope{"user"}}, 11);
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
    const auto removal = create_now(plans, {.kind = "manual-remove", .receiptId = "receipt-a",
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

TEST_F(JobsFixture, ProtectedBlockersAndConflictsAreRefusedBeforeQueueing) {
    source.kind = "remove";
    source.protected_blockers = {{{"official"}, {}, "linux"}};
    auto blocked = plan("linux");
    auto refused = enqueue_now(*runner, blocked.plan_id, blocked.approval_token, 11, "remove");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, protocol::kUnsafeTransaction);
    source.protected_blockers.clear();
    source.conflicts = {"sudo"};
    auto conflicting = plan("sudo-rs");
    refused = enqueue_now(*runner, conflicting.plan_id, conflicting.approval_token, 11, "remove");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, protocol::kUnsafeTransaction);
    EXPECT_TRUE(processes.empty());
    EXPECT_TRUE(runner->list({.limit = 100}, 11)->jobs.empty());
    source.conflicts.clear();
    auto allowed = plan("tree");
    ASSERT_TRUE(enqueue_now(*runner, allowed.plan_id, allowed.approval_token, 11, "remove"));
    EXPECT_EQ(processes.size(), 1U);
}

TEST_F(JobsFixture, ReviewedBuildNeverStartsWithoutAnswer) {
    source.review = {"nano"};
    const auto review = enqueue(plan("nano"));
    EXPECT_TRUE(processes.empty());
    EXPECT_EQ(runner->get({review}, 11)->state.value, "needs-attention");
    const auto next = enqueue(plan("htop"));
    EXPECT_TRUE(processes.empty());
    EXPECT_EQ(runner->get({next}, 11)->state.value, "queued");
    const auto question = runner->get({review}, 11)->events.back()
        .payload.get<protocol::JobQuestionNotification>().question;
    ASSERT_TRUE(runner->answer({review, question.question_id, "ack", question.token}, 11));
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(runner->get({review}, 11)->state.value, "running");
}

TEST_F(JobsFixture, ExpiredReviewIsCancelledAndReleasesQueue) {
    const auto first = enqueue(plan("tree"));
    source.review = {"nano"};
    const auto review = enqueue(plan("nano"));
    const auto third = enqueue(plan("htop"));
    ASSERT_EQ(processes.size(), 1U);
    now += std::chrono::minutes(10);
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "succeeded");
    EXPECT_EQ(runner->get({review}, 11)->state.value, "cancelled");
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(runner->get({third}, 11)->state.value, "running");
}

TEST_F(JobsFixture, ReentrantDisconnectDuringStaleHeadStartsOnlyOneFollowingJob) {
    std::string stale;
    bool disconnected = false;
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, [&](std::string_view id, const protocol::JobEvent& event, api::ConnectionId owner) {
        if (id == stale && event.kind == "job.state" && runner->get({stale}, owner)->state.value == "needs-attention") {
            disconnected = true;
            runner->disconnect(owner);
        }
    }, state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"});
    enqueue(plan("active"));
    stale = enqueue(plan("stale"));
    source.fingerprint = "changed";
    const auto first_ready = enqueue(plan("first-ready"));
    const auto second_ready = enqueue(plan("second-ready"));
    processes.front()->finish(0);
    ASSERT_TRUE(disconnected);
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(runner->get({stale}, 11)->state.value, "needs-attention");
    EXPECT_EQ(runner->get({first_ready}, 11)->state.value, "running");
    EXPECT_EQ(runner->get({second_ready}, 11)->state.value, "queued");
    processes.back()->finish(0);
    ASSERT_EQ(processes.size(), 3U);
    EXPECT_EQ(runner->get({second_ready}, 11)->state.value, "running");
}

TEST_F(JobsFixture, ReentrantDisconnectDuringExpiryStartsOnlyOneFollowingJob) {
    std::string expired;
    bool disconnected = false;
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, [&](std::string_view id, const protocol::JobEvent& event, api::ConnectionId owner) {
        if (id == expired && event.kind == "job.state" && runner->get({expired}, owner)->state.value == "cancelled") {
            disconnected = true;
            runner->disconnect(owner);
        }
    }, state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"},
        JobRunner::BuildSpec{}, JobRunner::Verify{}, JobRunner::Classify{}, [this] { return now; });
    enqueue(plan("active"));
    source.review = {"review"};
    expired = enqueue(plan("review"));
    const auto first_ready = enqueue(plan("first-ready"));
    const auto second_ready = enqueue(plan("second-ready"));
    now += std::chrono::minutes(10);
    processes.front()->finish(0);
    ASSERT_TRUE(disconnected);
    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(runner->get({first_ready}, 11)->state.value, "running");
    EXPECT_EQ(runner->get({second_ready}, 11)->state.value, "queued");
    processes.back()->finish(0);
    ASSERT_EQ(processes.size(), 3U);
    EXPECT_EQ(runner->get({second_ready}, 11)->state.value, "running");
}

TEST_F(JobsFixture, PipelineFinishRunsOnceAfterSuccessAndFailure) {
    std::vector<bool> outcomes;
    pipeline([&](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Pipeline value;
        value.steps.emplace_back([]() -> Result<core::ProcessSpec> {
            core::ProcessSpec spec;
            spec.executable = "/usr/bin/true";
            return spec;
        });
        value.finish = [&](bool success) -> Result<void> { outcomes.push_back(success); return {}; };
        return value;
    });
    enqueue(plan("success"));
    processes.back()->finish(0);
    enqueue(plan("failure"));
    processes.back()->finish(1);
    EXPECT_EQ(outcomes, (std::vector<bool>{true, false}));
}

TEST_F(JobsFixture, FailedWorkspaceFinalizationNeedsAttentionAndReleasesQueue) {
    pipeline([](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Pipeline value;
        value.steps.emplace_back([]() -> Result<core::ProcessSpec> {
            core::ProcessSpec spec;
            spec.executable = "/usr/bin/true";
            return spec;
        });
        value.finish = [](bool) -> Result<void> {
            return std::unexpected(Error{protocol::kUnsafeTransaction, "job.build_cleanup"});
        };
        return value;
    });
    const auto first = enqueue(plan("first"));
    const auto second = enqueue(plan("second"));
    processes.front()->finish(0);
    const auto result = runner->get({first}, 11);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->state.value, "needs-attention");
    ASSERT_TRUE(result->error);
    EXPECT_EQ(result->error->at("message_key"), "job.build_cleanup");
    EXPECT_EQ(runner->get({second}, 11)->state.value, "running");
}

TEST_F(JobsFixture, FailedStepGeneratorKeepsItsSpecificErrorAndFinishesWorkspace) {
    bool finished = false;
    pipeline([&](const Snapshot&, std::string_view, const ElevationOptions&) -> Result<JobRunner::Pipeline> {
        JobRunner::Pipeline value;
        value.steps.emplace_back([]() -> Result<core::ProcessSpec> {
            return std::unexpected(Error{protocol::kUntrustedInput, "job.build_mismatch"});
        });
        value.finish = [&](bool success) -> Result<void> { EXPECT_FALSE(success); finished = true; return {}; };
        return value;
    });
    const auto id = enqueue(plan());
    EXPECT_TRUE(finished);
    EXPECT_TRUE(processes.empty());
    const auto result = runner->get({id}, 11);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->state.value, "failed");
    ASSERT_TRUE(result->error);
    EXPECT_EQ(result->error->at("message_key"), "job.build_mismatch");
}

TEST_F(JobsFixture, StopStartsNoFurtherJobsAndRefusesNewOnes) {
    const auto first = enqueue(plan("tree"));
    const auto second = enqueue(plan("nano"));
    ASSERT_EQ(processes.size(), 1U);
    runner->stop();
    runner->stop();
    EXPECT_GE(processes.front()->kills, 1);
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({first}, 11)->state.value, "succeeded");
    EXPECT_EQ(runner->get({second}, 11)->state.value, "queued");
    EXPECT_EQ(processes.size(), 1U);
    auto late = plan("htop");
    auto refused = enqueue_now(*runner, late.plan_id, late.approval_token, 11);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, protocol::kBusy);
    runner->disconnect(11);
    EXPECT_EQ(processes.size(), 1U);
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
    [this](const Snapshot&, std::string_view, const ElevationOptions& auth) -> Result<JobRunner::Pipeline> {
        return JobRunner::Pipeline{{[this, auth] {
            auto spec = official_spec("install", {"tree"}, auth);
            if (spec) spec->env.emplace_back("BRAURSER_JOB_DIR", (state / "job").string());
            return spec;
        }}};
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

TEST(JobsStream, ShortLivedChildrenAlwaysStartAndReportTheirStatus) {
    Gio::init();
    for (int attempt = 0; attempt < 200; ++attempt) {
        GioStreamingProcess process;
        core::ProcessSpec spec;
        spec.executable = attempt % 2 ? "/usr/bin/true" : "/usr/bin/false";
        spec.env = {{"LC_ALL", "C"}};
        spec.timeout = std::chrono::seconds(3);
        auto loop = Glib::MainLoop::create();
        std::optional<core::ProcessResult> exit;
        ASSERT_TRUE(process.start(spec, [](std::string_view) {},
            [&](const core::ProcessResult& result) { exit = result; loop->quit(); })) << attempt;
        auto timer = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, 5000);
        loop->run();
        timer.disconnect();
        ASSERT_TRUE(exit.has_value()) << attempt;
        EXPECT_FALSE(exit->signaled);
        EXPECT_EQ(exit->exit_code, attempt % 2 ? 0 : 1);
    }
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

TEST_F(JobsFixture, ReentrantDisconnectCanEvictTheJobBeingAsked) {
    bool disconnect_on_question = false;
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, [&](std::string_view id, const protocol::JobEvent& event, api::ConnectionId owner) {
        notifications.push_back(event);
        if (disconnect_on_question && event.kind == "job.state" &&
            runner->get({std::string(id)}, owner)->state.value == "needs-attention") {
            disconnect_on_question = false;
            runner->disconnect(owner);
            runner->disconnect(owner);
        }
    }, state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"});
    const auto active = enqueue(plan("active"));
    std::vector<std::string> queued;
    for (int index = 0; index < 81; ++index) queued.push_back(enqueue(plan("pkg" + std::to_string(index))));
    std::ranges::sort(queued);
    for (std::size_t index = 1; index < queued.size(); ++index)
        runner->ask(queued[index], protocol::QuestionChoice{protocol::BuildReviewQuestion{}}, {"ack"});
    disconnect_on_question = true;
    runner->ask(queued.front(), protocol::QuestionChoice{protocol::BuildReviewQuestion{}}, {"ack"});
    EXPECT_FALSE(disconnect_on_question);
    EXPECT_FALSE(runner->get({queued.front()}, 11));
    EXPECT_EQ(runner->list({.limit = 100}, 11)->jobs.size(), 51);
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({active}, 11)->state.value, "succeeded");
    const auto next = enqueue(plan("next"));
    EXPECT_EQ(runner->get({next}, 11)->state.value, "running");
}

TEST_F(JobsFixture, ReentrantDisconnectDuringEnqueueDoesNotRequeueAnOrphan) {
    bool disconnect_on_question = false;
    runner = std::make_unique<JobRunner>(plans, [this] {
        auto process = std::make_shared<ProcessState>();
        processes.push_back(process);
        return std::make_unique<FakeProcess>(process);
    }, [&](std::string_view id, const protocol::JobEvent& event, api::ConnectionId owner) {
        if (disconnect_on_question && event.kind == "job.state" &&
            runner->get({std::string(id)}, owner)->state.value == "needs-attention") {
            disconnect_on_question = false;
            runner->disconnect(owner);
        }
    }, state, ElevationOptions{elevation::Method::Pkexec, "/usr/local/libexec/braurser"});
    const auto active = enqueue(plan("active"));
    for (int index = 0; index < 80; ++index) {
        const auto name = "pkg" + std::to_string(index);
        source.review.push_back(name);
        enqueue(plan(name));
    }
    source.review.push_back("last");
    disconnect_on_question = true;
    const auto orphan = enqueue(plan("last"));
    ASSERT_FALSE(disconnect_on_question);
    EXPECT_EQ(runner->get({orphan}, 11)->state.value, "needs-attention");
    EXPECT_EQ(runner->list({.limit = 100}, 11)->jobs.size(), 51);
    processes.front()->finish(0);
    EXPECT_EQ(runner->get({active}, 11)->state.value, "succeeded");
    const auto next = enqueue(plan("next"));
    EXPECT_EQ(runner->get({next}, 11)->state.value, "running");
}

TEST_F(JobsFixture, RetainedLogsRespectByteBudgetAndKeepSequenceNumbers) {
    const auto id = enqueue(plan());
    for (int index = 0; index < 2100; ++index)
        processes.front()->line(std::to_string(index) + ":" + std::string(4092, 'x'));
    processes.front()->finish(0);
    auto result = runner->get({id}, 11);
    ASSERT_TRUE(result);
    std::size_t bytes = 0;
    std::size_t count = 0;
    std::uint64_t previous = 0;
    std::string last;
    for (const auto& event : result->events) {
        EXPECT_GT(event.sequence, previous);
        previous = event.sequence;
        if (event.kind != "job.log") continue;
        last = event.payload.get<protocol::JobLogNotification>().line;
        bytes += last.size();
        ++count;
    }
    EXPECT_LE(bytes, 256 * 1024);
    EXPECT_GT(bytes, 252 * 1024);
    EXPECT_EQ(count, 64);
    EXPECT_TRUE(last.starts_with("2099:"));
    EXPECT_EQ(previous, 2103);
    EXPECT_FALSE(result->nextCursor);
    EXPECT_EQ(result->state.value, "succeeded");
}

TEST_F(JobsFixture, FinishedLogsStayWithinAggregateBudgetAndEvictOldSummaries) {
    std::vector<std::string> ids;
    const std::string line(4096, 'x');
    for (int index = 0; index < 55; ++index) {
        ids.push_back(enqueue(plan("pkg" + std::to_string(index))));
        for (int entry = 0; entry < 70; ++entry) processes.back()->line(line);
        processes.back()->finish(0);
    }
    std::size_t total_bytes = 0;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto job = runner->get({ids[index]}, 11);
        if (index < 5) {
            EXPECT_FALSE(job);
            continue;
        }
        ASSERT_TRUE(job);
        std::size_t bytes = 0;
        for (const auto& event : job->events)
            if (event.kind == "job.log") bytes += event.payload.get<protocol::JobLogNotification>().line.size();
        EXPECT_LE(bytes, 256 * 1024);
        total_bytes += bytes;
    }
    EXPECT_LE(total_bytes, 16 * 1024 * 1024);
    EXPECT_EQ(runner->list({.limit = 100}, 11)->jobs.size(), 50);
}

class LivePlanServices final : public api::Services {
public:
    int get_calls{};
    api::ServiceResult<protocol::HelloResult> hello(const protocol::HelloParams&) override { return {}; }
    api::ServiceResult<protocol::StatusResult> status(const protocol::StatusParams&) override { return {}; }
    api::ServiceResult<protocol::SearchResult> search(const protocol::SearchParams&) override { return {}; }
    api::ServiceResult<protocol::GetResult> get(const protocol::GetParams&) override {
        ++get_calls;
        protocol::GetResult result;
        result.details.package = {{"official"}, "extra", "target"};
        result.details.version = "1-1";
        result.source = {"official"};
        return result;
    }
    api::ServiceResult<protocol::VariantsResult> variants(const protocol::VariantsParams&) override { return {}; }
    api::ServiceResult<protocol::JobIdResult> refresh(const protocol::RefreshParams&) override { return {}; }
    api::ServiceResult<protocol::WikiGetResult> wiki(const protocol::WikiGetParams&) override { return {}; }
    api::ServiceResult<protocol::NewsListResult> news(const protocol::NewsListParams&) override { return {}; }
    api::ServiceResult<protocol::JobIdResult> check_updates(const protocol::UpdatesCheckParams&) override { return {}; }
    api::ServiceResult<protocol::UpdatesOptionsResult> update_options(const protocol::UpdatesOptionsParams&) override { return {}; }
    api::ServiceResult<protocol::JobIdResult> preview(const protocol::ManualPreviewParams&) override { return {}; }
    api::ServiceResult<protocol::ManualListResult> manual_list(const protocol::ManualListParams&) override { return {}; }
    api::ServiceResult<protocol::UnmanagedListResult> unmanaged(const protocol::UnmanagedListParams&) override { return {}; }
    api::ServiceResult<protocol::SettingsGetResult> settings_get(const protocol::SettingsGetParams&) override { return {}; }
    api::ServiceResult<protocol::SettingsSetResult> settings_set(const protocol::SettingsSetParams&) override { return {}; }
    api::ServiceResult<protocol::NotifierCheckResult> notifier_check(const protocol::NotifierCheckParams&) override { return {}; }
};

class LivePlanProcess final : public core::ProcessRunner {
public:
    std::filesystem::path database;
    core::ProcessResult preview;
    std::vector<core::ProcessSpec> specs;
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec& spec) override {
        specs.push_back(spec);
        if (spec.executable == "/usr/bin/pacman") return preview;
        core::ProcessResult result;
        result.exit_code = 0;
        if (spec.args == std::vector<std::string>{"--repo-list"}) result.out = "core\nextra\n";
        else if (spec.args == std::vector<std::string>{"DBPath"}) result.out = database.string() + '\n';
        else return std::unexpected(core::ProcessError{1, "unexpected process"});
        return result;
    }
};

class JobsLivePlan : public ::testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("braurser-live-plan-test-" + std::to_string(getpid()) + "-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    LivePlanServices services;
    LivePlanProcess process;
    protocol::UpdateReport report;
    std::unique_ptr<LivePlanSource> source;
    void SetUp() override {
        process.database = root / "db";
        ASSERT_TRUE(core::ensure_private_dir(process.database / "local"));
        report.report_id = "report_1-checked";
        report.checked_at = "2026-10-19T12:00:00Z";
        protocol::UpdateItem update;
        update.package = {{"official"}, "core", "existing"};
        update.from_version = "1-1";
        update.to_version = "2-1";
        update.source = {"official"};
        update.restart.package = update.package;
        update.restart.level = {"none"};
        update.restart.reason_key = "restart.reason.none";
        report.overall_restart = {"none"};
        report.official.push_back(update);
        ASSERT_TRUE(core::ensure_private_dir(root / "braurser/syncdb-check" / report.report_id));
        process.preview.exit_code = 0;
        process.preview.out = "core/existing 2-1\nextra/new-dependency 1:3.0~rc1-2\n";
        source = std::make_unique<LivePlanSource>(services, process, [this] { return report; }, root, root);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

TEST_F(JobsLivePlan, InputsAreStableAndBindTheCurrentReport) {
    const protocol::PlanParams selection{.kind = "install", .packageId = "target"};
    const auto original = source->inputs(selection);
    EXPECT_EQ(original.size(), 64U);
    EXPECT_EQ(source->inputs(selection), original);
    report.official.clear();
    const auto replacement = source->inputs(selection);
    EXPECT_NE(replacement, original);
    EXPECT_EQ(source->inputs(selection), replacement);
    source = std::make_unique<LivePlanSource>(services, process, [] { return std::nullopt; }, root, root);
    const auto missing = source->inputs(selection);
    EXPECT_NE(missing, replacement);
    EXPECT_EQ(source->inputs(selection), missing);
    EXPECT_TRUE(process.specs.empty());
}

TEST_F(JobsLivePlan, ManualInputsBindEveryCapturedPreviewField) {
    manual::Preview preview;
    preview.preview_id = "preview-1";
    preview.host = "example.org";
    preview.owner_repo = "owner/project";
    preview.commit = std::string(40, 'a');
    preview.selected_script_text = "install";
    preview.entry_points = {{"install.sh", manual::EntryKind::Script}};
    source = std::make_unique<LivePlanSource>(services, process, [this] { return report; }, root, root,
        [&](std::string_view id) -> std::optional<manual::Preview> {
            EXPECT_EQ(id, "preview-1");
            return preview;
        });
    for (const auto* kind : {"manual-install", "manual-upgrade"}) {
        const protocol::PlanParams selection{.kind = kind, .previewId = "preview-1"};
        auto previous = source->inputs(selection);
        EXPECT_EQ(source->inputs(selection), previous);
        const auto changed = [&] {
            const auto current = source->inputs(selection);
            EXPECT_NE(current, previous);
            EXPECT_EQ(source->inputs(selection), current);
            previous = current;
        };
        preview.commit = std::string(40, preview.commit.front() == 'a' ? 'b' : 'a');
        changed();
        preview.preview_id += "x";
        changed();
        preview.host += "x";
        changed();
        preview.owner_repo += "x";
        changed();
        preview.selected_script_text += "x";
        changed();
        preview.staging_supported = !preview.staging_supported;
        changed();
        preview.entry_points.front().path += "x";
        changed();
        preview.entry_points.front().kind = preview.entry_points.front().kind == manual::EntryKind::Script ?
            manual::EntryKind::MakeInstall : manual::EntryKind::Script;
        changed();
        preview.entry_points.push_back({"other.sh", manual::EntryKind::Script});
        changed();
    }
    const protocol::PlanParams official{.kind = "install", .packageId = "target"};
    const auto before = source->inputs(official);
    preview.commit = "different";
    EXPECT_EQ(source->inputs(official), before);
    EXPECT_TRUE(process.specs.empty());
}

TEST_F(JobsLivePlan, PrepareCapturesReportWithoutCallingItFromTask) {
    int report_calls = 0;
    bool report_valid = true;
    source = std::make_unique<LivePlanSource>(services, process, [&] {
        ++report_calls;
        EXPECT_TRUE(report_valid);
        return report;
    }, root, root);
    const auto expected = source->snapshot({.kind = "update-system"});
    ASSERT_TRUE(expected);
    auto task = source->prepare({.kind = "update-system"});
    EXPECT_EQ(report_calls, 2);
    report.report_id = "missing";
    report.official.clear();
    report_valid = false;
    const std::atomic<bool> cancelled{false};
    const auto snapshot = task(cancelled);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(report_calls, 2);
    EXPECT_EQ(snapshot->fingerprint, expected->fingerprint);
    EXPECT_EQ(snapshot->expected_transaction, expected->expected_transaction);
    EXPECT_EQ(snapshot->plan.kind.value, expected->plan.kind.value);
    EXPECT_EQ(nlohmann::json(snapshot->plan.upgrades), nlohmann::json(expected->plan.upgrades));
    EXPECT_EQ(nlohmann::json(snapshot->plan.installs), nlohmann::json(expected->plan.installs));
}

TEST_F(JobsLivePlan, TaskUsesProvidedDetailInsteadOfServices) {
    int detail_calls = 0;
    source = std::make_unique<LivePlanSource>(services, process, [this] { return report; }, root, root,
        LivePlanSource::PreviewLookup{}, [&](const protocol::GetParams& params) -> api::ServiceResult<protocol::GetResult> {
            ++detail_calls;
            EXPECT_EQ(params.packageId, "target");
            protocol::GetResult detail;
            detail.details.package = {{"official"}, "extra", "target"};
            detail.details.version = "3-1";
            detail.source = {"official"};
            return detail;
        });
    auto task = source->prepare({.kind = "install", .packageId = "target"});
    const std::atomic<bool> cancelled{false};
    const auto snapshot = task(cancelled);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(detail_calls, 1);
    EXPECT_EQ(services.get_calls, 0);
    EXPECT_EQ(snapshot->planned_version, "3-1");
}

TEST_F(JobsLivePlan, CancelledTaskStartsNoProcesses) {
    auto task = source->prepare({.kind = "update-system"});
    const std::atomic<bool> cancelled{true};
    const auto snapshot = task(cancelled);
    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, protocol::kCancelled);
    EXPECT_TRUE(process.specs.empty());
    EXPECT_EQ(services.get_calls, 0);
}

TEST_F(JobsLivePlan, EveryTaskProcessReceivesItsCancelFlag) {
    const std::atomic<bool> cancelled{false};
    auto task = source->prepare({.kind = "update-system"});
    ASSERT_TRUE(task(cancelled));
    ASSERT_EQ(process.specs.size(), 3);
    for (const auto& spec : process.specs) EXPECT_EQ(spec.cancel, &cancelled);
    process.specs.clear();
    report.official.clear();
    process.preview.out = "target 1-1 extra 42\n";
    auto install = source->prepare({.kind = "install", .packageId = "target"});
    ASSERT_TRUE(install(cancelled));
    ASSERT_EQ(process.specs.size(), 3);
    for (const auto& spec : process.specs) EXPECT_EQ(spec.cancel, &cancelled);
}

TEST_F(JobsLivePlan, UpdateDigestBindsExactOutputAndListsNewInstalls) {
    const auto snapshot = source->snapshot({.kind = "update-system"});
    ASSERT_TRUE(snapshot);
    gchar* hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar*>(process.preview.out.data()), process.preview.out.size());
    EXPECT_EQ(snapshot->expected_transaction, hash);
    g_free(hash);
    EXPECT_NE(snapshot->fingerprint.find(snapshot->expected_transaction), std::string::npos);
    ASSERT_EQ(snapshot->plan.installs.size(), 1);
    EXPECT_EQ(snapshot->plan.installs.front().name, "new-dependency");
    EXPECT_EQ(snapshot->plan.installs.front().repo, "extra");
    EXPECT_EQ(snapshot->plan.installs.front().source.value, "official");
    ASSERT_EQ(snapshot->plan.upgrades.size(), 1);
    EXPECT_EQ(snapshot->plan.upgrades.front().name, "existing");
    const auto& spec = process.specs.back();
    EXPECT_EQ(spec.executable, "/usr/bin/pacman");
    EXPECT_EQ(spec.args, (std::vector<std::string>{"--sync", "--sysupgrade", "--print", "--print-format",
        "%r/%n %v", "--dbpath", (root / "braurser/syncdb-check" / report.report_id).string()}));
    EXPECT_EQ(spec.env, (std::vector<std::pair<std::string, std::string>>{{"LC_ALL", "C"}, {"PATH", "/usr/bin"}}));
    EXPECT_EQ(spec.timeout, std::chrono::seconds(60));
    EXPECT_EQ(spec.max_stdout, 4 * 1024 * 1024);
    process.preview.out.pop_back();
    const auto changed = source->snapshot({.kind = "upgrade-system"});
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->plan.kind.value, "update-system");
    EXPECT_NE(snapshot->expected_transaction, changed->expected_transaction);
    EXPECT_NE(snapshot->fingerprint, changed->fingerprint);
}

TEST_F(JobsLivePlan, MissingSymlinkedAndInvalidReportDirectoriesAreStale) {
    report.report_id = "missing";
    EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kStalePlan);
    std::filesystem::create_directory_symlink(root / "db", root / "braurser/syncdb-check/linked");
    report.report_id = "linked";
    EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kStalePlan);
    for (const auto& id : {"../db", "", "a/b", "bad.id"}) {
        report.report_id = id;
        EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kStalePlan);
    }
    EXPECT_EQ(process.specs.size(), 12);
}

TEST_F(JobsLivePlan, MalformedPrintOutputIsUntrusted) {
    for (const auto& output : std::vector<std::string>{
        "existing 2-1\n", "core/existing\n", "core/existing 2-1 extra\n", "core/existing 2-1\r\n",
        "core/existing 2-1\n\n", "core/existing 2-1\nextra/existing 3-1\n", "../existing 2-1\n",
        "core/-bad 2-1\n", "core/existing \n", std::string("core/existing 2\0-1\n", 19)}) {
        process.preview.out = output;
        const auto snapshot = source->snapshot({.kind = "update-system"});
        ASSERT_FALSE(snapshot) << output;
        EXPECT_EQ(snapshot.error().code, protocol::kUntrustedInput);
    }
}

TEST_F(JobsLivePlan, TruncatedFailedOrTimedOutPrintIsUntrusted) {
    process.preview.out_truncated = true;
    EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kUntrustedInput);
    process.preview.out_truncated = false;
    process.preview.err_truncated = true;
    EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kUntrustedInput);
    process.preview.err_truncated = false;
    process.preview.timed_out = true;
    EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kUntrustedInput);
    process.preview.timed_out = false;
    process.preview.exit_code = 1;
    EXPECT_EQ(source->snapshot({.kind = "update-system"}).error().code, protocol::kUntrustedInput);
}

TEST_F(JobsLivePlan, UpdateAndInstallPrintUsesTheReportDbAndQualifiedTarget) {
    process.preview.out += "extra/target 1-1\n";
    const auto snapshot = source->snapshot({.kind = "install", .packageId = "target"});
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->plan.kind.value, "update-and-install");
    EXPECT_EQ(snapshot->targets, (std::vector<std::string>{"extra/target"}));
    EXPECT_EQ(snapshot->plan.installs.size(), 2);
    EXPECT_EQ(snapshot->expected_transaction.size(), 64);
    EXPECT_EQ(process.specs.size(), 3);
    EXPECT_EQ(process.specs.back().args, (std::vector<std::string>{"--sync", "--sysupgrade", "--print",
        "--print-format", "%r/%n %v", "--dbpath", (root / "braurser/syncdb-check" / report.report_id).string(),
        "--needed", "--", "extra/target"}));
}

TEST_F(JobsLivePlan, PrintedInstallsCannotExceedProtocolArrayLimit) {
    process.preview.out.clear();
    for (int index = 0; index < 257; ++index)
        process.preview.out += "extra/pkg" + std::to_string(index) + " 1-1\n";
    const auto snapshot = source->snapshot({.kind = "update-system"});
    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, protocol::kUntrustedInput);
}

void check_escaped_pipe(bool timeout) {
    if (!std::filesystem::exists("/usr/bin/python3")) GTEST_SKIP() << "python3 unavailable";
    Gio::init();
    GioStreamingProcess process;
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/python3";
    spec.args = {"-c", "import os,signal,time\n"
        "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "pid=os.fork()\n"
        "if pid == 0:\n"
        " os.setsid()\n"
        " time.sleep(30)\n"
        " os._exit(0)\n"
        "print(pid, flush=True)\n" + std::string(timeout ? "time.sleep(30)\n" : "os._exit(0)\n")};
    spec.env = {{"LC_ALL", "C"}};
    spec.timeout = timeout ? std::chrono::milliseconds(100) : std::chrono::seconds(60);
    auto loop = Glib::MainLoop::create();
    std::optional<core::ProcessResult> result;
    int descendant = 0;
    int completions = 0;
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(process.start(spec, [&](std::string_view line) {
        const auto parsed = std::from_chars(line.data(), line.data() + line.size(), descendant);
        EXPECT_EQ(parsed.ec, std::errc{});
    }, [&](const core::ProcessResult& exit) {
        ++completions;
        result = exit;
        loop->quit();
    }));
    auto watchdog = Glib::signal_timeout().connect([&] { loop->quit(); return false; }, timeout ? 6000 : 15000);
    loop->run();
    watchdog.disconnect();
    if (descendant > 1) ::kill(descendant, SIGKILL);
    ASSERT_TRUE(result);
    EXPECT_EQ(completions, 1);
    EXPECT_GT(descendant, 1);
    EXPECT_TRUE(result->out_truncated);
    EXPECT_TRUE(result->err_truncated);
    EXPECT_EQ(result->timed_out, timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - started, timeout ? std::chrono::seconds(6) : std::chrono::seconds(15));
    if (!timeout) { EXPECT_EQ(result->exit_code, 0); }
}

TEST(JobsStream, ReapedChildCannotLeaveDescendantPipesOpenForever) {
    check_escaped_pipe(false);
}

TEST(JobsStream, TimeoutClosesDescendantPipesAfterKillingTheGroup) {
    check_escaped_pipe(true);
}

}
}
