#include <braurser/daemon/jobs/jobs.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/validate/validate.hpp>
#include <glib.h>
#include <glibmm/main.h>
#include <sys/random.h>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::jobs {
namespace {
Error failure(int code, std::string key = "error.generic_title") { return {code, std::move(key)}; }

std::string random_token() {
    std::array<unsigned char, 16> bytes{};
    if (getrandom(bytes.data(), bytes.size(), 0) != static_cast<ssize_t>(bytes.size()))
        throw std::runtime_error("random token unavailable");
    std::string token;
    token.reserve(32);
    for (const auto byte : bytes) token += std::format("{:02x}", byte);
    return token;
}

std::string digest(std::string_view data) {
    gchar* value = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar*>(data.data()), data.size());
    std::string hash(value);
    g_free(value);
    return hash;
}

bool same(const Snapshot& a, const Snapshot& b) {
    auto left = a.plan;
    auto right = b.plan;
    left.plan_id = right.plan_id = "comparison";
    left.revision = right.revision = 0;
    left.approval_token = right.approval_token = "comparison";
    return a.fingerprint == b.fingerprint && a.targets == b.targets &&
        a.planned_version == b.planned_version && a.helper == b.helper &&
        nlohmann::json(left) == nlohmann::json(right);
}

void passthrough(std::vector<std::pair<std::string, std::string>>& env, const char* key) {
    const auto* value = std::getenv(key);
    if (!value) return;
    std::string_view text(value);
    if (text.size() > 512 || text.find_first_of("\r\n\x1b") != std::string_view::npos) return;
    env.emplace_back(key, text);
}

bool valid_target(std::string_view name) {
    if (name.empty() || name.size() > 256) return false;
    std::size_t slash = 0;
    return br_split_target(name.data(), name.size(), &slash) != 0;
}
}

Result<protocol::TransactionPlan> PlanManager::create(const protocol::PlanParams& selection, api::ConnectionId owner) {
    if (owner == 0) return std::unexpected(failure(protocol::kAuthorizationUnavailable));
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.used || now_() >= it->second.expires) it = entries_.erase(it);
        else ++it;
    }
    if (entries_.size() >= 256) return std::unexpected(failure(protocol::kBusy));
    auto snapshot = source_.snapshot(selection);
    if (!snapshot) return std::unexpected(snapshot.error());
    if (snapshot->fingerprint.empty() || snapshot->fingerprint.size() > 16 * 1024 * 1024 ||
        snapshot->targets.size() > 512)
        return std::unexpected(failure(protocol::kUntrustedInput));
    for (const auto& target : snapshot->targets)
        if (target.size() > 4096 || target.find('\0') != std::string::npos)
            return std::unexpected(failure(protocol::kUntrustedInput));
    snapshot->selection = selection;
    snapshot->fingerprint = digest(snapshot->fingerprint);
    snapshot->plan.plan_id = random_token();
    snapshot->plan.revision = ++revision_;
    snapshot->plan.approval_token = random_token();
    try { static_cast<void>(nlohmann::json(snapshot->plan)); }
    catch (const std::exception&) { return std::unexpected(failure(protocol::kUntrustedInput)); }
    auto token = snapshot->plan.approval_token;
    auto plan = snapshot->plan;
    entries_.emplace(plan.plan_id, Entry{selection, std::move(*snapshot), std::move(token), owner,
                     now_() + std::chrono::minutes(10)});
    return plan;
}

bool PlanManager::unchanged(const Snapshot& snapshot, const protocol::PlanParams& selection) {
    try {
        auto current = source_.snapshot(selection);
        if (!current || current->fingerprint.empty() || current->fingerprint.size() > 16 * 1024 * 1024)
            return false;
        current->fingerprint = digest(current->fingerprint);
        return same(snapshot, *current);
    } catch (const std::exception&) { return false; }
}

Result<Snapshot> PlanManager::approve(std::string_view id, std::string_view token, api::ConnectionId owner) {
    auto entry = entries_.find(id);
    if (owner == 0 || entry == entries_.end() || entry->second.owner != owner ||
        entry->second.token != token || entry->second.used ||
        now_() >= entry->second.expires)
        return std::unexpected(failure(protocol::kStalePlan));
    entry->second.used = true;
    if (!unchanged(entry->second.snapshot, entry->second.selection))
        return std::unexpected(failure(protocol::kStalePlan));
    return entry->second.snapshot;
}

Result<core::ProcessSpec> official_spec(std::string_view kind, const std::vector<std::string>& targets,
                                         const ElevationOptions& auth) {
    if (auth.method != elevation::Method::Pkexec && auth.method != elevation::Method::SudoAskpass)
        return std::unexpected(failure(protocol::kAuthorizationUnavailable, "auth.unavailable_title"));
    if (auth.ui != "gtk" && auth.ui != "qt") return std::unexpected(failure(protocol::kInvalidParams));
    if (targets.size() > 512 || (targets.empty() && kind != "update-system"))
        return std::unexpected(failure(protocol::kInvalidParams));
    std::vector<std::string> args;
    if (kind == "install") args = {"--sync", "--needed", "--noconfirm", "--"};
    else if (kind == "update-system" || kind == "upgrade-system") {
        if (!targets.empty()) return std::unexpected(failure(protocol::kInvalidParams));
        args = {"--sync", "--refresh", "--sysupgrade", "--noconfirm"};
    } else if (kind == "update-and-install") {
        args = {"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--needed", "--"};
    } else if (kind == "remove" || kind == "manual-remove") {
        args = {"--remove", "--nosave", "--recursive", "--noconfirm", "--"};
    } else if (kind == "manual-install" || kind == "manual-upgrade" || kind == "setup-helper") {
        if (targets.size() > 64) return std::unexpected(failure(protocol::kInvalidParams));
        args = {"--upgrade", "--noconfirm", "--"};
    } else return std::unexpected(failure(protocol::kInvalidParams));
    for (const auto& target : targets) {
        if (args.front() == "--upgrade") {
            if (!std::filesystem::path(target).is_absolute() || target.size() > 4096 ||
                target.find("..") != std::string::npos || target.find('\0') != std::string::npos)
                return std::unexpected(failure(protocol::kUntrustedInput));
        } else if (!valid_target(target) ||
                   (args.front() == "--remove" && target.find('/') != std::string::npos))
            return std::unexpected(failure(protocol::kUntrustedInput));
        args.push_back(target);
    }
    if (!auth.libexec.is_absolute() || auth.libexec.lexically_normal() != auth.libexec)
        return std::unexpected(failure(protocol::kUntrustedInput));
    const auto helper = (auth.libexec / "braurser-pacman-helper").string();
    core::ProcessSpec spec;
    if (auth.method == elevation::Method::Pkexec) {
        spec.executable = "/usr/bin/pkexec";
        spec.args = {helper};
    } else {
        spec.executable = "/usr/bin/sudo";
        spec.args = {"-A", "--", helper};
    }
    spec.args.insert(spec.args.end(), args.begin(), args.end());
    spec.env = {{"PATH", "/usr/bin"}, {"LC_ALL", "C"}, {"LANG", "C"}};
    if (auth.method == elevation::Method::SudoAskpass) {
        spec.env.emplace_back("SUDO_ASKPASS", (auth.libexec / ("braurser-askpass-" + auth.ui)).string());
        const auto action = args.front() == "--remove" ? "remove" :
            args.front() == "--upgrade" ? "git" : kind == "install" ? "install" : "update";
        spec.env.emplace_back("BRAURSER_AUTH_ACTION", action);
        for (const auto* key : {"DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY"})
            passthrough(spec.env, key);
    }
    spec.timeout = std::chrono::hours(4);
    return spec;
}

JobRunner::JobRunner(PlanManager& plans, Factory factory, Notify notify, std::filesystem::path state,
                     ElevationOptions auth, BuildSpec build_spec, Verify verify, Classify classify,
                     PlanManager::Now now, Success success, ManualInstaller* manual_installer)
    : plans_(plans), factory_(std::move(factory)), notify_(std::move(notify)),
      state_(std::move(state)), elevation_(std::move(auth)),
      build_spec_(std::move(build_spec)), verify_(std::move(verify)), classify_(std::move(classify)),
      now_(std::move(now)), success_(std::move(success)), manual_installer_(manual_installer) {}
JobRunner::~JobRunner() {
    lifetime_.reset();
    if (process_) process_->kill_group();
    if (manual_worker_.joinable()) manual_worker_.join();
}

void JobRunner::add_event(Job& job, std::string_view kind, nlohmann::json payload) {
    protocol::JobEvent event{++job.next_sequence, std::string(kind), std::move(payload)};
    job.events.push_back(event);
    if (kind == "job.log") ++job.log_count;
    while (job.log_count > 2000 || job.events.size() > 2500) {
        if (job.events.front().kind == "job.log") --job.log_count;
        job.events.pop_front();
    }
    if (notify_) {
        try { notify_(job.id, event, job.owner); }
        catch (const std::exception&) {}
    }
}
void JobRunner::transition(Job& job, std::string_view state) {
    job.state = state;
    add_event(job, "job.state", protocol::JobStateNotification{job.id, job.next_sequence + 1,
                                                            {std::string(state)}});
}

Result<protocol::JobIdResult> JobRunner::enqueue(std::string_view id, std::string_view token, api::ConnectionId owner,
                                                  std::string_view requested_kind) {
    if (pending_.size() >= 256 || jobs_.size() >= 306)
        return std::unexpected(failure(protocol::kBusy));
    auto approved = plans_.approve(id, token, owner);
    if (!approved) return std::unexpected(approved.error());
    const auto& kind = approved->plan.kind.value;
    if (!requested_kind.empty() && requested_kind != kind &&
        !(requested_kind == "install" && kind == "update-and-install"))
        return std::unexpected(failure(protocol::kInvalidParams));
    auto job_id = random_token();
    Job job;
    job.id = job_id;
    job.owner = owner;
    job.snapshot = std::move(*approved);
    job.selection = job.snapshot.selection;
    jobs_.emplace(job_id, std::move(job));
    pending_.push_back(job_id);
    transition(jobs_.at(job_id), "queued");
    if (jobs_.at(job_id).snapshot.plan.requires_build_review) {
        protocol::QuestionChoice choice{protocol::BuildReviewQuestion{
            jobs_.at(job_id).snapshot.plan.build_review_refs,
            jobs_.at(job_id).snapshot.helper_build ?
                std::optional(jobs_.at(job_id).snapshot.helper_build->pkgbuild) : std::nullopt}};
        ask(job_id, std::move(choice), {"ack"});
    }
    pump();
    return protocol::JobIdResult{job_id};
}

void JobRunner::line(Job& job, std::string_view text) {
    const auto cleaned = safe_line(text);
    if (cleaned.empty()) return;
    add_event(job, "job.log", protocol::JobLogNotification{job.id, job.next_sequence + 1, cleaned});
    if (cleaned.find(":: Processing package changes") != std::string::npos ||
        cleaned.find("checking for file conflicts") != std::string::npos) job.commit = true;
    std::string phase;
    if (cleaned.find(":: Running post-transaction hooks") != std::string::npos) phase = "finishing";
    else if (cleaned.find(":: Retrieving packages") != std::string::npos ||
             cleaned.find("downloading...") != std::string::npos) phase = "downloading";
    else if (cleaned.find("==> Making package") != std::string::npos) phase = "building";
    else if (cleaned.find("removing ") != std::string::npos) phase = "removing";
    else if (job.commit) phase = "installing";
    if (classify_ && job.snapshot.helper != "auto") {
        try {
            if (const auto event = classify_(cleaned)) {
                switch (*event) {
                case helpers::PhaseEvent::UnexpectedPrompt:
                    job.unexpected_prompt = true;
                    if (process_) process_->kill_group();
                    break;
                case helpers::PhaseEvent::Resolving: phase = "checking"; break;
                case helpers::PhaseEvent::Downloading: phase = "downloading"; break;
                case helpers::PhaseEvent::Building: phase = "building"; break;
                case helpers::PhaseEvent::Installing: phase = "installing"; break;
                case helpers::PhaseEvent::Finishing: phase = "finishing"; break;
                case helpers::PhaseEvent::Error: break;
                }
            }
        } catch (const std::exception&) {
            job.unexpected_prompt = true;
            if (process_) process_->kill_group();
        }
    }
    if (!phase.empty() && (!job.progress || job.progress->phase.value != phase)) {
        job.progress = protocol::Progress{{phase}, {}, {}, {"items"}};
        add_event(job, "job.progress", protocol::JobProgressNotification{job.id, job.next_sequence + 1,
                                                                             *job.progress});
    }
}

void JobRunner::complete(Job& job, const core::ProcessResult& result) {
    struct stat marker{};
    const bool auth_failed = !job.auth_marker.empty() && lstat(job.auth_marker.c_str(), &marker) == 0 &&
                             S_ISREG(marker.st_mode) && marker.st_uid == getuid();
    if (auth_failed) add_event(job, "job.authState", protocol::JobAuthStateNotification{
        job.id, job.next_sequence + 1, {"failed"}});
    const bool exited_clean = result.exit_code == 0 && !result.signaled && !result.timed_out &&
                              !result.out_truncated && !result.err_truncated && !job.interrupted &&
                              !job.unexpected_prompt && !auth_failed;
    bool success = exited_clean;
    if (success) {
        try {
            success = (!verify_ || verify_(job.snapshot)) && (!success_ || success_(job.snapshot));
        } catch (const std::exception&) { success = false; }
    }
    const bool uncertain_cancel = job.interrupted && (job.commit || result.exit_code == 0 ||
        result.out_truncated || result.err_truncated || result.timed_out);
    transition(job, job.unexpected_prompt || uncertain_cancel ? "needs-attention" :
                    job.interrupted ? "cancelled" : success ? "succeeded" : "failed");
    finish_job(job);
    active_.clear();
    process_.reset();
    pump();
}

void JobRunner::finish_job(Job& job) {
    finished_.push_back(job.id);
    while (finished_.size() > 50) {
        jobs_.erase(finished_.front());
        finished_.pop_front();
    }
    persist();
}

void JobRunner::pump() {
    if (!active_.empty()) return;
    while (!pending_.empty()) {
        const auto id = pending_.front();
        auto& job = jobs_.at(id);
        if (job.question) return;
        pending_.pop_front();
        if (!plans_.unchanged(job.snapshot, job.selection)) {
            transition(job, "needs-attention");
            finish_job(job);
            continue;
        }
        if ((job.snapshot.plan.kind.value.starts_with("manual-") && job.selection.scope) ||
            job.snapshot.plan.kind.value == "setup-helper") {
            if (!manual_installer_) {
                transition(job, "failed");
                finish_job(job);
                continue;
            }
            if (manual_worker_.joinable()) manual_worker_.join();
            active_ = id;
            job.commit = true;
            transition(job, "running");
            try {
                manual_worker_ = std::jthread([this, installer = manual_installer_, snapshot = job.snapshot,
                                                id, lifetime = std::weak_ptr<int>(lifetime_)] {
                    core::ProcessResult result;
                    try { result.exit_code = installer->run(snapshot, id) ? 0 : 1; }
                    catch (const std::exception&) { result.exit_code = 1; }
                    Glib::MainContext::get_default()->invoke([this, id, result, lifetime] {
                        if (lifetime.expired()) return false;
                        if (auto it = jobs_.find(id); it != jobs_.end() && active_ == id)
                            complete(it->second, result);
                        return false;
                    });
                });
            } catch (const std::exception&) {
                active_.clear();
                transition(job, "failed");
                finish_job(job);
                continue;
            }
            return;
        }
        auto spec = [&]() -> Result<core::ProcessSpec> {
            try {
                return build_spec_ ? build_spec_(job.snapshot, id, elevation_) :
                    official_spec(job.snapshot.plan.kind.value, job.snapshot.targets, elevation_);
            } catch (const std::exception&) {
                return std::unexpected(failure(protocol::kUntrustedInput));
            }
        }();
        if (!spec) {
            transition(job, "failed");
            finish_job(job);
            continue;
        }
        std::unique_ptr<StreamingProcess> process;
        try { process = factory_(); }
        catch (const std::exception&) {}
        if (!process) {
            transition(job, "failed");
            finish_job(job);
            continue;
        }
        for (const auto& [key, value] : spec->env)
            if (key == "BRAURSER_JOB_DIR") job.auth_marker = std::filesystem::path(value) / "auth-failed";
        active_ = id;
        process_ = std::move(process);
        transition(job, "running");
        if (!process_->start(*spec,
            [this, id](std::string_view text) { if (auto it = jobs_.find(id); it != jobs_.end()) line(it->second, text); },
            [this, id](const core::ProcessResult& result) { if (auto it = jobs_.find(id); it != jobs_.end()) complete(it->second, result); })) {
            process_.reset();
            active_.clear();
            transition(job, "failed");
            finish_job(job);
            continue;
        }
        return;
    }
}

Result<protocol::JobsCancelResult> JobRunner::cancel(std::string_view id, api::ConnectionId owner) {
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.owner != owner) return std::unexpected(failure(protocol::kInvalidParams));
    auto& job = it->second;
    if (job.state == "queued" || (job.state == "needs-attention" && job.question)) {
        std::erase(pending_, job.id);
        job.question.reset();
        job.choices.clear();
        transition(job, "cancelled");
        finish_job(job);
        pump();
        return protocol::JobsCancelResult{true, {}};
    }
    if (job.state != "running" && job.state != "waiting-auth") return protocol::JobsCancelResult{false, "activity.cannot_cancel"};
    if (job.commit) return protocol::JobsCancelResult{false, "activity.cannot_cancel"};
    job.interrupted = true;
    transition(job, "cancelling");
    if (process_) process_->interrupt();
    return protocol::JobsCancelResult{true, {}};
}

Result<protocol::JobsDequeueResult> JobRunner::dequeue(std::string_view id, api::ConnectionId owner) {
    const auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.owner != owner) return std::unexpected(failure(protocol::kInvalidParams));
    if (it->second.state != "queued") return protocol::JobsDequeueResult{false};
    return protocol::JobsDequeueResult{cancel(id, owner)->accepted};
}

Result<protocol::JobsGetResult> JobRunner::get(const protocol::JobsGetParams& params, api::ConnectionId owner) const {
    const auto it = jobs_.find(params.jobId);
    if (it == jobs_.end() || it->second.owner != owner) return std::unexpected(failure(protocol::kInvalidParams));
    protocol::JobsGetResult result;
    result.state = {it->second.state};
    result.plan = it->second.snapshot.plan;
    result.plan->approval_token = "redacted";
    result.progress = it->second.progress;
    std::uint64_t after{};
    if (params.eventsCursor) {
        const auto& text = *params.eventsCursor;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), after);
        if (error != std::errc{} || end != text.data() + text.size())
            return std::unexpected(failure(protocol::kInvalidParams));
    }
    for (const auto& event : it->second.events)
        if (event.sequence > after && result.events.size() < 100) result.events.push_back(event);
    if (!result.events.empty() && result.events.back().sequence < it->second.next_sequence)
        result.nextCursor = std::to_string(result.events.back().sequence);
    return result;
}

Result<protocol::JobsListResult> JobRunner::list(const protocol::JobsListParams& params, api::ConnectionId owner) const {
    protocol::JobsListResult result;
    if (params.limit == 0 || params.limit > 100) return std::unexpected(failure(protocol::kInvalidParams));
    if (params.cursor) {
        const auto cursor = jobs_.find(*params.cursor);
        if (cursor == jobs_.end() || cursor->second.owner != owner)
            return std::unexpected(failure(protocol::kInvalidParams));
    }
    bool after = !params.cursor;
    for (const auto& [id, job] : jobs_) {
        if (!after) { if (id == *params.cursor) after = true; continue; }
        if (job.owner != owner) continue;
        if (result.jobs.size() == params.limit) { result.nextCursor = result.jobs.back().jobId; break; }
        result.jobs.push_back({id, {job.state}});
    }
    if (!after) return std::unexpected(failure(protocol::kInvalidParams));
    return result;
}

void JobRunner::ask(std::string_view id, protocol::QuestionChoice choice, std::vector<std::string> choices) {
    const auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.state != "queued" ||
        !std::holds_alternative<protocol::BuildReviewQuestion>(choice.value) ||
        choices != std::vector<std::string>{"ack"}) return;
    auto& job = it->second;
    job.question = protocol::Question{random_token(), random_token(),
        std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(
            std::chrono::system_clock::now() + std::chrono::minutes(10))), std::move(choice)};
    job.choices = std::move(choices);
    job.question_expires = now_() + std::chrono::minutes(10);
    transition(job, "needs-attention");
    add_event(job, "job.question", protocol::JobQuestionNotification{job.id, job.next_sequence + 1, *job.question});
}

Result<protocol::AcceptedResult> JobRunner::answer(const protocol::JobsAnswerParams& params, api::ConnectionId owner) {
    auto it = jobs_.find(params.jobId);
    if (it == jobs_.end() || it->second.owner != owner) return std::unexpected(failure(protocol::kExpiredQuestion));
    auto& job = it->second;
    if (job.orphaned || !job.question || job.question->question_id != params.questionId ||
        job.question->token != params.questionToken ||
        now_() >= job.question_expires ||
        std::ranges::find(job.choices, params.choiceId) == job.choices.end())
        return std::unexpected(failure(protocol::kExpiredQuestion));
    job.question.reset();
    job.choices.clear();
    transition(job, "queued");
    pump();
    return protocol::AcceptedResult{true};
}

void JobRunner::disconnect(api::ConnectionId owner) {
    for (auto& [id, job] : jobs_) {
        if (job.owner != owner || !job.question || job.orphaned) continue;
        job.orphaned = true;
        std::erase(pending_, id);
        finish_job(job);
    }
    pump();
}

void JobRunner::persist() const {
    if (!core::ensure_private_dir(state_ / "braurser")) return;
    nlohmann::json summaries = nlohmann::json::array();
    for (const auto& id : finished_) {
        const auto it = jobs_.find(id);
        if (it != jobs_.end()) summaries.push_back(protocol::JobSummary{id, {it->second.state}});
    }
    static_cast<void>(core::atomic_write(state_ / "braurser/jobs.json", summaries.dump(), 0600));
}

}
