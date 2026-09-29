#ifndef BRAURSER_DAEMON_JOBS_JOBS_HPP
#define BRAURSER_DAEMON_JOBS_JOBS_HPP

#include <braurser/daemon/jobs/process.hpp>
#include <braurser/daemon/api/services.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/elevation/selector.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <braurser/daemon/manual/manual.hpp>
#include <braurser/protocol/dto.hpp>
#include <chrono>
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

struct Snapshot {
    protocol::TransactionPlan plan;
    std::string fingerprint;
    std::vector<std::string> targets;
    std::string planned_version;
    std::string helper{"auto"};
    protocol::PlanParams selection;
    std::optional<manual::Receipt> manual_receipt;
    std::optional<manual::Preview> manual_preview;
    std::optional<manual::BuildRepository> helper_build;
};

class PlanSource {
public:
    virtual ~PlanSource() = default;
    /// in: selection identifies a bounded read-only transaction request.
    /// out: transaction details and fingerprint of every dependency and installed input.
    virtual Result<Snapshot> snapshot(const protocol::PlanParams& selection) = 0;
};

class PlanManager {
public:
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    explicit PlanManager(PlanSource& source, Now now = std::chrono::steady_clock::now)
        : source_(source), now_(std::move(now)) {}
    /// in: owner identifies the authenticated socket connection.
    /// out: plan with an unpredictable single-use approval token.
    Result<protocol::TransactionPlan> create(const protocol::PlanParams& selection, api::ConnectionId owner);
    /// in: owner must match the planning socket and token must be unused.
    /// err: a changed fingerprint fails with kStalePlan.
    Result<Snapshot> approve(std::string_view id, std::string_view token, api::ConnectionId owner);
    /// in: snapshot is the previously approved transaction.
    /// out: true only when the live read-only inputs still match.
    bool unchanged(const Snapshot& snapshot, const protocol::PlanParams& selection);
private:
    struct Entry {
        protocol::PlanParams selection;
        Snapshot snapshot;
        std::string token;
        api::ConnectionId owner{};
        std::chrono::steady_clock::time_point expires;
        bool used{};
    };
    PlanSource& source_;
    Now now_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::uint64_t revision_{};
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
                                        const ElevationOptions& elevation);

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
    using BuildSpec = std::function<Result<core::ProcessSpec>(const Snapshot&, std::string_view, const ElevationOptions&)>;
    using Verify = std::function<bool(const Snapshot&)>;
    using Classify = std::function<std::optional<helpers::PhaseEvent>(std::string_view)>;
    using Success = std::function<bool(const Snapshot&)>;
    JobRunner(PlanManager& plans, Factory factory, Notify notify, std::filesystem::path state,
              ElevationOptions elevation, BuildSpec build_spec = {}, Verify verify = {}, Classify classify = {},
              PlanManager::Now now = std::chrono::steady_clock::now, Success success = {},
              ManualInstaller* manual_installer = nullptr);
    ~JobRunner();
    /// in: approval is unused, owned by the caller, and matches the requested endpoint kind.
    /// out: enqueue intent in FIFO order; only the head may mutate the system.
    Result<protocol::JobIdResult> enqueue(std::string_view plan_id, std::string_view approval_token, api::ConnectionId owner,
                                          std::string_view requested_kind = {});
    /// in: job_id identifies a queued or running job on its owner connection.
    /// out: cancellation refused once pacman has begun committing.
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
        bool commit{};
        bool interrupted{};
        bool unexpected_prompt{};
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
    std::unique_ptr<StreamingProcess> process_;
    std::jthread manual_worker_;
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};
    std::string active_;
    bool stopping_{};
    void pump();
    void transition(Job& job, std::string_view state);
    void add_event(Job& job, std::string_view kind, nlohmann::json payload);
    void line(Job& job, std::string_view text);
    void complete(Job& job, const core::ProcessResult& result);
    void finish_job(Job& job);
    void persist() const;
};

}

#endif
