#ifndef BRAURSER_DAEMON_JOBS_JOBS_HPP
#define BRAURSER_DAEMON_JOBS_JOBS_HPP

#include <braurser/daemon/jobs/process.hpp>
#include <braurser/daemon/jobs/srcinfo.hpp>
#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/api/services.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/elevation/selector.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <braurser/daemon/manual/manual.hpp>
#include <braurser/protocol/dto.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace braurser::daemon::jobs {

using Error = protocol::ErrorObject;
template<class T> using Result = std::expected<T, Error>;

struct AurPin {
    std::string pkgbase;
    std::vector<std::string> pkgnames;
    std::string commit;
    std::string tree;
    std::string srcinfo;
    bool target{};
    bool dynamic_version{};
    std::vector<std::pair<std::string, Dependency>> constraints{};
};

struct Snapshot {
    protocol::TransactionPlan plan;
    std::string fingerprint;
    std::vector<std::string> targets;
    std::string planned_version;
    std::string helper{"auto"};
    std::string expected_transaction;
    std::vector<AurPin> aur_pins;
    std::vector<std::string> official_deps;
    std::vector<catalog::RepoPackage> aur_official;
    std::vector<catalog::LocalRecord> aur_installed;
    protocol::PlanParams selection;
    std::optional<manual::Receipt> manual_receipt;
    std::optional<manual::Preview> manual_preview;
    std::optional<manual::BuildRepository> helper_build;
};

using PlanTask = std::function<Result<Snapshot>(const std::atomic<bool>& cancelled)>;

class PlanSource {
public:
    virtual ~PlanSource() = default;
    /// in: runs on the daemon thread.
    /// out: a digest of the daemon-thread inputs prepare copies for selection; empty when there are none.
    virtual std::string inputs(const protocol::PlanParams&) { return {}; }
    /// in: selection identifies a bounded read-only transaction request.
    /// out: transaction details and fingerprint of every dependency and installed input.
    virtual Result<Snapshot> snapshot(const protocol::PlanParams& selection) = 0;
    /// in: runs on the daemon thread; selection and any daemon-thread state are copied into the task.
    /// out: a task that may run on the planning thread and returns early once cancelled is set.
    virtual PlanTask prepare(const protocol::PlanParams& selection) {
        return [this, selection](const std::atomic<bool>&) { return snapshot(selection); };
    }
};

enum class PlanLane { Approval, Plan };

class PlanExecutor {
public:
    using Ticket = std::uint64_t;
    using Done = std::function<void(Result<Snapshot>)>;
    virtual ~PlanExecutor() = default;
    /// in: task is self-contained; Approval tasks run before queued Plan tasks, each lane in FIFO order.
    /// out: a nonzero ticket; done runs exactly once on the daemon thread, possibly before submit returns.
    /// err: returns 0 without calling done when stopped or the lane is full.
    virtual Ticket submit(PlanLane lane, PlanTask task, Done done) = 0;
    /// in: ticket came from submit; unknown or delivered tickets are ignored.
    /// out: the task's cancel flag is set and done later receives kCancelled, never from inside cancel.
    virtual void cancel(Ticket ticket) = 0;
    /// out: cancels every task, joins the planning thread and never calls a pending done afterwards.
    virtual void stop() = 0;
};

class InlinePlanExecutor final : public PlanExecutor {
public:
    Ticket submit(PlanLane, PlanTask task, Done done) override {
        if (stopped_ || !task || !done) return 0;
        const std::atomic<bool> cancelled{false};
        auto result = [&]() -> Result<Snapshot> {
            try { return task(cancelled); }
            catch (const std::exception&) { return std::unexpected(Error{protocol::kUntrustedInput, "error.generic_title"}); }
        }();
        const auto ticket = ++next_;
        done(std::move(result));
        return ticket;
    }
    void cancel(Ticket) override {}
    void stop() override { stopped_ = true; }
private:
    Ticket next_{};
    bool stopped_{};
};

class PlanManager {
public:
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    using Flight = std::uint64_t;
    using Planned = std::function<void(Result<protocol::TransactionPlan>)>;
    using Approved = std::function<void(Result<Snapshot>)>;
    using Checked = std::function<void(bool)>;
    using Live = std::function<bool(api::ConnectionId)>;
    /// in: executor outlives this manager; null runs every snapshot inline on the calling thread; live checks connection liveness on the daemon thread.
    explicit PlanManager(PlanSource& source, Now now = std::chrono::steady_clock::now, PlanExecutor* executor = nullptr,
                         Live live = {})
        : source_(source), now_(std::move(now)), executor_(executor ? *executor : inline_), live_(std::move(live)) {}
    /// in: owner identifies the authenticated socket connection; done runs once on the daemon thread.
    /// out: plan with an unpredictable single-use approval token.
    /// err: kBusy at capacity; kCancelled after disconnect or closing; kStalePlan when captured inputs change.
    void create(const protocol::PlanParams& selection, api::ConnectionId owner, Planned done);
    /// in: owner must match the planning socket and token must be unused; the token is spent before the re-check starts.
    /// err: a changed fingerprint or captured inputs fail with kStalePlan; kCancelled after disconnect or closing.
    void approve(std::string_view id, std::string_view token, api::ConnectionId owner, Approved done);
    /// in: snapshot is the previously approved transaction; done runs once on the daemon thread.
    /// out: a flight id for cancel; done receives true only when the live read-only inputs still match.
    Flight unchanged(const Snapshot& snapshot, const protocol::PlanParams& selection, Checked done);
    /// in: flight came from unchanged; done later receives false.
    void cancel(Flight flight);
    /// in: owner is a closed connection.
    /// out: its unapproved plans are dropped and its in-flight requests finish with kCancelled.
    void disconnect(api::ConnectionId owner);
private:
    struct Entry {
        protocol::PlanParams selection;
        Snapshot snapshot;
        std::string token;
        api::ConnectionId owner{};
        std::chrono::steady_clock::time_point expires;
        bool used{};
    };
    struct InFlight {
        api::ConnectionId owner{};
        PlanExecutor::Ticket ticket{};
        bool creating{};
        bool cancelled{};
    };
    PlanSource& source_;
    Now now_;
    InlinePlanExecutor inline_;
    PlanExecutor& executor_;
    Live live_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<Flight, InFlight> flights_;
    std::uint64_t revision_{};
    Flight next_flight_{};
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
};

struct ElevationOptions {
    elevation::Method method{elevation::Method::Unavailable};
    std::filesystem::path libexec{};
    std::string ui{"gtk"};
    std::string action{"install"};
};

/// in: kind and validated targets identify one canonical privileged transaction.
/// out: only helper-allowlisted argv and explicit environment, or an authorization error.
Result<core::ProcessSpec> official_spec(std::string_view kind, const std::vector<std::string>& targets,
                                        const ElevationOptions& elevation, std::string_view transaction = {});

class ManualInstaller {
public:
    virtual ~ManualInstaller() = default;
    /// in: snapshot holds approved user-scope Git metadata and job_id is private.
    /// out: applies a user-scope operation and writes a receipt only on success.
    virtual Result<void> run(const Snapshot& snapshot, std::string_view job_id) = 0;
};

class JobRunner {
public:
    using Factory = std::function<std::unique_ptr<StreamingProcess>()>;
    using Notify = std::function<void(std::string_view, const protocol::JobEvent&, api::ConnectionId)>;
    using Step = std::function<Result<core::ProcessSpec>()>;
    struct Pipeline { std::vector<Step> steps; std::function<Result<void>(bool)> finish{}; };
    using BuildSpec = std::function<Result<Pipeline>(const Snapshot&, std::string_view, const ElevationOptions&)>;
    using Verify = std::function<bool(const Snapshot&)>;
    using Classify = std::function<std::optional<helpers::PhaseEvent>(std::string_view)>;
    using Success = std::function<bool(const Snapshot&)>;
    JobRunner(PlanManager& plans, Factory factory, Notify notify, std::filesystem::path state,
              ElevationOptions elevation, BuildSpec build_spec = {}, Verify verify = {}, Classify classify = {},
              PlanManager::Now now = std::chrono::steady_clock::now, Success success = {},
              ManualInstaller* manual_installer = nullptr);
    ~JobRunner();
    using Enqueued = std::function<void(Result<protocol::JobIdResult>)>;
    /// in: approval is unused, owned by the caller, and matches the requested endpoint kind; done runs once on the daemon thread.
    /// out: enqueue intent in approval order; only the head may mutate the system, after its inputs are re-checked off-thread.
    void enqueue(std::string_view plan_id, std::string_view approval_token, api::ConnectionId owner,
                 std::string_view requested_kind, Enqueued done);
    /// in: job_id identifies a queued or running job on its owner connection.
    /// out: cancellation refused during pipeline root-helper steps or a single-step commit.
    Result<protocol::JobsCancelResult> cancel(std::string_view job_id, api::ConnectionId owner);
    /// in: job_id identifies a queued job belonging to owner.
    Result<protocol::JobsDequeueResult> dequeue(std::string_view job_id, api::ConnectionId owner);
    Result<protocol::JobsGetResult> get(const protocol::JobsGetParams& params, api::ConnectionId owner) const;
    Result<protocol::JobsListResult> list(const protocol::JobsListParams& params, api::ConnectionId owner) const;
    /// in: owner, token and choice must match an unexpired single-use question.
    Result<protocol::AcceptedResult> answer(const protocol::JobsAnswerParams& params, api::ConnectionId owner);
    /// in: owner is a disconnected generation; outstanding questions become orphaned.
    void disconnect(api::ConnectionId owner);
    /// in: job_id is queued; only a build-review acknowledgement is supported.
    void ask(std::string_view job_id, protocol::QuestionChoice choice, std::vector<std::string> choices);
    /// out: no further job starts; the active process group is killed and the manual worker joined.
    void stop();
private:
    struct Job {
        std::string id;
        api::ConnectionId owner{};
        protocol::PlanParams selection;
        Snapshot snapshot;
        std::string state{"queued"};
        std::optional<protocol::Progress> progress;
        std::deque<protocol::JobEvent> events;
        std::uint64_t next_sequence{};
        std::size_t log_count{};
        std::size_t log_bytes{};
        bool finished{};
        bool enqueuing{};
        bool commit{};
        bool privileged{};
        bool mutated{};
        bool interrupted{};
        bool unexpected_prompt{};
        bool missing_key{};
        enum class Check : std::uint8_t { None, Running, Passed, Failed } check{};
        PlanManager::Flight check_flight{};
        std::optional<Error> error;
        Pipeline pipeline;
        std::size_t step{};
        std::filesystem::path auth_marker;
        std::optional<protocol::Question> question;
        bool orphaned{};
        bool reviewed{};
        std::vector<std::string> choices;
        std::chrono::steady_clock::time_point question_expires{};
    };
    PlanManager& plans_;
    Factory factory_;
    Notify notify_;
    std::filesystem::path state_;
    ElevationOptions elevation_;
    BuildSpec build_spec_;
    Verify verify_;
    Classify classify_;
    PlanManager::Now now_;
    Success success_;
    ManualInstaller* manual_installer_;
    std::map<std::string, Job, std::less<>> jobs_;
    std::deque<std::string> pending_;
    std::deque<std::string> finished_;
    std::size_t finished_log_bytes_{};
    std::unique_ptr<StreamingProcess> process_;
    std::jthread manual_worker_;
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
    std::string active_;
    std::string checking_;
    std::size_t approving_{};
    bool stopping_{};
    bool pumping_{};
    bool pump_again_{};
    void pump();
    bool start_step(Job& job);
    void transition(Job& job, std::string_view state);
    void add_event(Job& job, std::string_view kind, nlohmann::json payload);
    void line(Job& job, std::string_view text);
    void complete(Job& job, const core::ProcessResult& result);
    void finish_job(Job& job);
    void erase_event(Job& job, std::deque<protocol::JobEvent>::iterator event);
    void trim_logs(Job& job, std::size_t budget);
    void trim_finished_logs();
    void persist() const;
};

}

#endif
