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
        a.expected_transaction == b.expected_transaction &&
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

void PlanManager::create(const protocol::PlanParams& selection, api::ConnectionId owner, Planned done) {
    if (owner == 0) { done(std::unexpected(failure(protocol::kAuthorizationUnavailable))); return; }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.used || now_() >= it->second.expires) it = entries_.erase(it);
        else ++it;
    }
    const auto creating = std::ranges::count_if(flights_, [](const auto& item) { return item.second.creating; });
    const auto owned = std::ranges::count_if(flights_, [owner](const auto& item) { return item.second.owner == owner; });
    if (entries_.size() + static_cast<std::size_t>(creating) >= 256 || owned >= 4) {
        done(std::unexpected(failure(protocol::kBusy)));
        return;
    }
    PlanTask task;
    std::string inputs;
    try { inputs = source_.inputs(selection); }
    catch (...) { done(std::unexpected(failure(protocol::kStalePlan))); return; }
    try { task = source_.prepare(selection); }
    catch (const std::exception&) { done(std::unexpected(failure(protocol::kUntrustedInput))); return; }
    const auto flight = ++next_flight_;
    flights_.emplace(flight, InFlight{owner, 0, true, false});
    const auto ticket = executor_.submit(PlanLane::Plan, std::move(task),
        [this, lifetime = std::weak_ptr<int>(lifetime_), flight, selection, owner,
         inputs = std::move(inputs), done](Result<Snapshot> snapshot) {
            if (lifetime.expired()) return;
            const auto it = flights_.find(flight);
            if (it == flights_.end()) return;
            const bool cancelled = it->second.cancelled;
            flights_.erase(it);
            if (cancelled) { done(std::unexpected(failure(protocol::kCancelled))); return; }
            if (live_ && !live_(owner)) { done(std::unexpected(failure(protocol::kCancelled))); return; }
            auto result = [&]() -> Result<protocol::TransactionPlan> {
                if (!snapshot) return std::unexpected(snapshot.error());
                try {
                    if (source_.inputs(selection) != inputs) return std::unexpected(failure(protocol::kStalePlan));
                } catch (...) { return std::unexpected(failure(protocol::kStalePlan)); }
                try {
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
                    static_cast<void>(nlohmann::json(snapshot->plan));
                    auto token = snapshot->plan.approval_token;
                    auto plan = snapshot->plan;
                    entries_.emplace(plan.plan_id, Entry{selection, std::move(*snapshot), std::move(token), owner,
                                     now_() + std::chrono::minutes(10)});
                    return plan;
                } catch (const std::exception&) { return std::unexpected(failure(protocol::kUntrustedInput)); }
            }();
            done(std::move(result));
        });
    if (auto it = flights_.find(flight); it != flights_.end()) {
        if (ticket == 0) {
            flights_.erase(it);
            done(std::unexpected(failure(protocol::kBusy)));
        } else {
            it->second.ticket = ticket;
            if (it->second.cancelled) executor_.cancel(ticket);
        }
    }
}

PlanManager::Flight PlanManager::unchanged(const Snapshot& snapshot, const protocol::PlanParams& selection, Checked done) {
    PlanTask task;
    std::string inputs;
    try {
        inputs = source_.inputs(selection);
        task = source_.prepare(selection);
    } catch (...) { done(false); return 0; }
    const auto flight = ++next_flight_;
    flights_.emplace(flight, InFlight{0, 0, false, false});
    const auto ticket = executor_.submit(PlanLane::Approval, std::move(task),
        [this, lifetime = std::weak_ptr<int>(lifetime_), flight, snapshot, selection,
         inputs = std::move(inputs), done](Result<Snapshot> current) {
            if (lifetime.expired()) return;
            const auto it = flights_.find(flight);
            if (it == flights_.end()) return;
            const bool cancelled = it->second.cancelled;
            flights_.erase(it);
            bool matches = false;
            try {
                if (!cancelled && current && source_.inputs(selection) == inputs && !current->fingerprint.empty() &&
                    current->fingerprint.size() <= 16 * 1024 * 1024) {
                    current->fingerprint = digest(current->fingerprint);
                    matches = same(snapshot, *current);
                }
            } catch (...) {}
            done(matches);
        });
    if (auto it = flights_.find(flight); it != flights_.end()) {
        if (ticket == 0) { flights_.erase(it); done(false); return 0; }
        it->second.ticket = ticket;
        if (it->second.cancelled) executor_.cancel(ticket);
    }
    return flight;
}

void PlanManager::approve(std::string_view id, std::string_view token, api::ConnectionId owner, Approved done) {
    auto entry = entries_.find(id);
    if (owner == 0 || entry == entries_.end() || entry->second.owner != owner ||
        entry->second.token != token || entry->second.used || now_() >= entry->second.expires) {
        done(std::unexpected(failure(protocol::kStalePlan)));
        return;
    }
    if (std::ranges::count_if(flights_, [owner](const auto& item) { return item.second.owner == owner; }) >= 4) {
        done(std::unexpected(failure(protocol::kBusy)));
        return;
    }
    entry->second.used = true;
    auto snapshot = std::move(entry->second.snapshot);
    auto selection = std::move(entry->second.selection);
    PlanTask task;
    std::string inputs;
    try {
        inputs = source_.inputs(selection);
        task = source_.prepare(selection);
    } catch (...) { done(std::unexpected(failure(protocol::kStalePlan))); return; }
    const auto flight = ++next_flight_;
    flights_.emplace(flight, InFlight{owner, 0, false, false});
    const auto ticket = executor_.submit(PlanLane::Approval, std::move(task),
        [this, lifetime = std::weak_ptr<int>(lifetime_), flight, snapshot = std::move(snapshot),
         selection = std::move(selection), owner, inputs = std::move(inputs), done](Result<Snapshot> current) mutable {
            if (lifetime.expired()) return;
            const auto it = flights_.find(flight);
            if (it == flights_.end()) return;
            const bool cancelled = it->second.cancelled;
            flights_.erase(it);
            if (cancelled || (!current && current.error().code == protocol::kCancelled)) {
                done(std::unexpected(failure(protocol::kCancelled)));
                return;
            }
            if (live_ && !live_(owner)) { done(std::unexpected(failure(protocol::kCancelled))); return; }
            bool matches = false;
            try {
                if (current && source_.inputs(selection) == inputs && !current->fingerprint.empty() &&
                    current->fingerprint.size() <= 16 * 1024 * 1024) {
                    current->fingerprint = digest(current->fingerprint);
                    matches = same(snapshot, *current);
                }
            } catch (...) {}
            if (matches) {
                snapshot.selection = std::move(selection);
                done(std::move(snapshot));
            } else done(std::unexpected(failure(protocol::kStalePlan)));
        });
    if (auto it = flights_.find(flight); it != flights_.end()) {
        if (ticket == 0) {
            flights_.erase(it);
            done(std::unexpected(failure(protocol::kBusy)));
        } else {
            it->second.ticket = ticket;
            if (it->second.cancelled) executor_.cancel(ticket);
        }
    }
}

void PlanManager::cancel(Flight flight) {
    const auto it = flights_.find(flight);
    if (it == flights_.end()) return;
    it->second.cancelled = true;
    executor_.cancel(it->second.ticket);
}

void PlanManager::disconnect(api::ConnectionId owner) {
    if (owner == 0) return;
    std::erase_if(entries_, [owner](const auto& item) { return item.second.owner == owner; });
    for (auto& [flight, request] : flights_) {
        static_cast<void>(flight);
        if (request.owner != owner) continue;
        request.cancelled = true;
        executor_.cancel(request.ticket);
    }
}

Result<core::ProcessSpec> official_spec(std::string_view kind, const std::vector<std::string>& targets,
                                         const ElevationOptions& auth, std::string_view transaction) {
    if (auth.method != elevation::Method::Pkexec && auth.method != elevation::Method::SudoAskpass)
        return std::unexpected(failure(protocol::kAuthorizationUnavailable, "auth.unavailable_title"));
    if (auth.ui != "gtk" && auth.ui != "qt") return std::unexpected(failure(protocol::kInvalidParams));
    const bool system = kind == "update-system" || kind == "upgrade-system" || kind == "update-and-install";
    if (targets.size() > 512 || (targets.empty() && kind != "update-system" && kind != "upgrade-system"))
        return std::unexpected(failure(protocol::kInvalidParams));
    if (system && (transaction.size() != 64 || !std::ranges::all_of(transaction, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }))) return std::unexpected(failure(protocol::kStalePlan));
    std::vector<std::string> args;
    if (kind == "install") args = {"--sync", "--needed", "--noconfirm", "--"};
    else if (kind == "install-deps") args = {"--sync", "--needed", "--asdeps", "--noconfirm", "--"};
    else if (kind == "mark-deps") args = {"--database", "--asdeps", "--noconfirm", "--"};
    else if (kind == "update-system" || kind == "upgrade-system") {
        if (!targets.empty()) return std::unexpected(failure(protocol::kInvalidParams));
        args = {"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--braurser-transaction=" + std::string(transaction)};
    } else if (kind == "update-and-install") {
        args = {"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--needed", "--braurser-transaction=" + std::string(transaction), "--"};
    } else if (kind == "remove" || kind == "manual-remove") {
        args = {"--remove", "--nosave", "--recursive", "--noconfirm", "--"};
    } else if (kind == "manual-install" || kind == "manual-upgrade" || kind == "setup-helper" || kind == "install-archives") {
        if (targets.size() > 64) return std::unexpected(failure(protocol::kInvalidParams));
        args = {"--upgrade", "--noconfirm", "--"};
    } else return std::unexpected(failure(protocol::kInvalidParams));
    for (const auto& target : targets) {
        if (args.front() == "--upgrade") {
            if (!std::filesystem::path(target).is_absolute() || target.size() > 4096 ||
                target.find("..") != std::string::npos || target.find('\0') != std::string::npos)
                return std::unexpected(failure(protocol::kUntrustedInput));
        } else if (!valid_target(target) ||
                   ((args.front() == "--remove" || args.front() == "--database") && target.find('/') != std::string::npos))
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
JobRunner::~JobRunner() { stop(); }

void JobRunner::stop() {
    stopping_ = true;
    lifetime_.reset();
    if (process_) process_->kill_group();
    if (manual_worker_.joinable()) manual_worker_.join();
}

void JobRunner::erase_event(Job& job, std::deque<protocol::JobEvent>::iterator event) {
    if (event->kind == "job.log") {
        const auto bytes = event->payload.at("line").get_ref<const std::string&>().size();
        --job.log_count;
        job.log_bytes -= bytes;
        if (job.finished) finished_log_bytes_ -= bytes;
    }
    job.events.erase(event);
}

void JobRunner::trim_logs(Job& job, std::size_t budget) {
    while (job.log_bytes > budget) {
        const auto event = std::ranges::find_if(job.events, [](const auto& value) {
            return value.kind == "job.log";
        });
        if (event == job.events.end()) break;
        erase_event(job, event);
    }
}

void JobRunner::trim_finished_logs() {
    constexpr std::size_t budget = 16 * 1024 * 1024;
    for (const auto& id : finished_) {
        if (finished_log_bytes_ <= budget) break;
        const auto it = jobs_.find(id);
        if (it == jobs_.end()) continue;
        auto& job = it->second;
        const auto excess = finished_log_bytes_ - budget;
        trim_logs(job, excess >= job.log_bytes ? 0 : job.log_bytes - excess);
    }
}

void JobRunner::add_event(Job& job, std::string_view kind, nlohmann::json payload) {
    protocol::JobEvent event{++job.next_sequence, std::string(kind), std::move(payload)};
    job.events.push_back(event);
    if (kind == "job.log") {
        ++job.log_count;
        const auto bytes = event.payload.at("line").get_ref<const std::string&>().size();
        job.log_bytes += bytes;
        if (job.finished) finished_log_bytes_ += bytes;
    }
    while (job.log_count > 2000 || job.events.size() > 2500) erase_event(job, job.events.begin());
    trim_logs(job, 256 * 1024);
    trim_finished_logs();
    if (notify_) {
        const auto id = job.id;
        const auto owner = job.owner;
        try { notify_(id, event, owner); }
        catch (const std::exception&) {}
    }
}
void JobRunner::transition(Job& job, std::string_view state) {
    job.state = state;
    add_event(job, "job.state", protocol::JobStateNotification{job.id, job.next_sequence + 1,
                                                            {std::string(state)}});
}

void JobRunner::enqueue(std::string_view id, std::string_view token, api::ConnectionId owner,
                        std::string_view requested_kind, Enqueued done) {
    if (stopping_ || pending_.size() + approving_ >= 256 || jobs_.size() + approving_ >= 306) {
        done(std::unexpected(failure(protocol::kBusy)));
        return;
    }
    ++approving_;
    plans_.approve(id, token, owner,
        [this, lifetime = std::weak_ptr<int>(lifetime_), owner, requested_kind = std::string(requested_kind),
         done = std::move(done)](Result<Snapshot> approved) {
            if (lifetime.expired()) return;
            --approving_;
            if (!approved) { done(std::unexpected(approved.error())); return; }
            if (stopping_) { done(std::unexpected(failure(protocol::kBusy))); return; }
            const auto& kind = approved->plan.kind.value;
            if (!requested_kind.empty() && requested_kind != kind &&
                !(requested_kind == "install" && kind == "update-and-install")) {
                done(std::unexpected(failure(protocol::kInvalidParams)));
                return;
            }
            if (!approved->plan.protected_blockers.empty() || !approved->plan.blockers.empty() ||
                !approved->plan.conflicts.empty()) {
                done(std::unexpected(failure(protocol::kUnsafeTransaction)));
                return;
            }
            auto job_id = random_token();
            Job job;
            job.enqueuing = true;
            job.id = job_id;
            job.owner = owner;
            job.snapshot = std::move(*approved);
            job.selection = job.snapshot.selection;
            auto& queued = jobs_.emplace(job_id, std::move(job)).first->second;
            transition(queued, "queued");
            if (queued.snapshot.plan.requires_build_review) {
                try {
                    protocol::QuestionChoice choice{protocol::BuildReviewQuestion{
                        queued.snapshot.plan.build_review_refs,
                        queued.snapshot.helper_build ?
                            std::optional(queued.snapshot.helper_build->pkgbuild) : std::nullopt}};
                    ask(job_id, std::move(choice), {"ack"});
                } catch (const std::exception&) {}
                if (!queued.question) {
                    queued.enqueuing = false;
                    transition(queued, "failed");
                    finish_job(queued);
                    done(std::unexpected(failure(protocol::kUnsafeTransaction)));
                    return;
                }
            }
            queued.enqueuing = false;
            if (queued.orphaned) {
                finish_job(queued);
                done(protocol::JobIdResult{job_id});
                return;
            }
            pending_.push_back(job_id);
            pump();
            done(protocol::JobIdResult{job_id});
        });
}

void JobRunner::line(Job& job, std::string_view text) {
    const auto cleaned = safe_line(text);
    if (cleaned.empty()) return;
    add_event(job, "job.log", protocol::JobLogNotification{job.id, job.next_sequence + 1, cleaned});
    if (job.snapshot.aur_pins.empty() && job.pipeline.steps.size() <= 1 &&
        (cleaned.find(":: Processing package changes") != std::string::npos ||
         cleaned.find("checking for file conflicts") != std::string::npos)) job.commit = true;
    std::string phase;
    if (cleaned.find(":: Running post-transaction hooks") != std::string::npos) phase = "finishing";
    else if (cleaned.find(":: Retrieving packages") != std::string::npos ||
             cleaned.find("downloading...") != std::string::npos) phase = "downloading";
    else if (cleaned.find("==> Making package") != std::string::npos) phase = "building";
    else if (cleaned.find("removing ") != std::string::npos) phase = "removing";
    else if (job.commit) phase = "installing";
    if (cleaned.find("unknown public key") != std::string::npos) job.missing_key = true;
    if (classify_ && (job.snapshot.helper != "auto" || !job.snapshot.aur_pins.empty())) {
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
                              !job.unexpected_prompt && !job.missing_key && !auth_failed;
    bool success = exited_clean;
    const bool multi_step = !job.snapshot.aur_pins.empty() || job.pipeline.steps.size() > 1;
    if (exited_clean && job.privileged) job.mutated = true;
    if (multi_step) job.commit = false;
    bool between_steps = false;
    if (success && job.step + 1 < job.pipeline.steps.size()) {
        ++job.step;
        process_.reset();
        if (start_step(job)) return;
        between_steps = true;
        success = false;
    } else if (success) {
        try {
            success = (!verify_ || verify_(job.snapshot)) && (!success_ || success_(job.snapshot));
        } catch (const std::exception&) { success = false; }
    }
    const auto& kind = job.snapshot.plan.kind.value;
    const bool stale = result.exit_code == 75 && !result.signaled &&
        (kind == "update-system" || kind == "upgrade-system" || kind == "update-and-install");
    if (stale) {
        job.error = failure(protocol::kStalePlan, "job.updates_changed");
        line(job, "Updates changed since you reviewed them. Check for updates and review again.");
    }
    if (job.missing_key) {
        job.error = failure(protocol::kUnsafeTransaction, "job.unknown_public_key");
        line(job, "This package's sources are signed with a key you haven't imported");
    }
    const bool uncertain_cancel = job.interrupted && (job.mutated || job.commit ||
        (!multi_step && ((result.exit_code == 0 && !between_steps) ||
                        result.out_truncated || result.err_truncated || result.timed_out)));
    transition(job, stale || job.unexpected_prompt || uncertain_cancel ? "needs-attention" :
                    job.interrupted ? "cancelled" : success ? "succeeded" : "failed");
    finish_job(job);
    active_.clear();
    process_.reset();
    pump();
}

void JobRunner::finish_job(Job& job) {
    if (!job.finished) {
        job.finished = true;
        if (job.pipeline.finish) {
            auto cleaned = [&]() -> Result<void> {
                try { return job.pipeline.finish(job.state == "succeeded"); }
                catch (const std::exception&) { return std::unexpected(failure(protocol::kUntrustedInput)); }
            }();
            if (!cleaned) {
                if (!job.error) job.error = cleaned.error();
                if (job.state == "succeeded") transition(job, "needs-attention");
            }
        }
        finished_log_bytes_ += job.log_bytes;
        finished_.push_back(job.id);
    }
    trim_logs(job, 256 * 1024);
    trim_finished_logs();
    while (finished_.size() > 50) {
        const auto oldest = std::ranges::find_if(finished_, [&](const auto& id) {
            const auto it = jobs_.find(id);
            return id != job.id && id != active_ &&
                std::ranges::find(pending_, id) == pending_.end() &&
                (it == jobs_.end() || !it->second.enqueuing);
        });
        if (oldest == finished_.end()) break;
        const auto it = jobs_.find(*oldest);
        if (it != jobs_.end()) {
            finished_log_bytes_ -= it->second.log_bytes;
            jobs_.erase(it);
        }
        finished_.erase(oldest);
    }
    persist();
}

void JobRunner::pump() {
    if (pumping_) {
        pump_again_ = true;
        return;
    }
    pumping_ = true;
    struct Reset {
        bool& pumping;
        ~Reset() { pumping = false; }
    } reset{pumping_};
    do {
        pump_again_ = false;
        if (stopping_ || !active_.empty() || !checking_.empty()) return;
        while (!pending_.empty()) {
            if (stopping_ || !active_.empty() || !checking_.empty()) return;
            const auto id = pending_.front();
            auto& job = jobs_.at(id);
            if (job.question) {
                if (now_() < job.question_expires) return;
                pending_.pop_front();
                job.question.reset();
                job.choices.clear();
                transition(job, "cancelled");
                finish_job(job);
                continue;
            }
            if (job.snapshot.plan.requires_build_review && !job.reviewed) {
                pending_.pop_front();
                transition(job, "failed");
                finish_job(job);
                continue;
            }
            if (job.check == Job::Check::None) {
                job.check = Job::Check::Running;
                checking_ = id;
                const auto flight = plans_.unchanged(job.snapshot, job.selection,
                    [this, lifetime = std::weak_ptr<int>(lifetime_), id](bool matches) {
                        if (lifetime.expired()) return;
                        if (checking_ == id) checking_.clear();
                        const auto it = jobs_.find(id);
                        if (it != jobs_.end() && !it->second.finished && it->second.check == Job::Check::Running)
                            it->second.check = matches ? Job::Check::Passed : Job::Check::Failed;
                        pump();
                    });
                if (auto it = jobs_.find(id); it != jobs_.end() && !it->second.finished)
                    it->second.check_flight = flight;
                break;
            }
            if (job.check == Job::Check::Running) return;
            pending_.pop_front();
            if (job.check == Job::Check::Failed) {
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
            auto pipeline = [&]() -> Result<Pipeline> {
                try {
                    if (build_spec_) return build_spec_(job.snapshot, id, elevation_);
                    return Pipeline{{[snapshot = job.snapshot, auth = elevation_] {
                        return official_spec(snapshot.plan.kind.value, snapshot.targets, auth, snapshot.expected_transaction);
                    }}};
                } catch (const std::exception&) {
                    return std::unexpected(failure(protocol::kUntrustedInput));
                }
            }();
            if (!pipeline || pipeline->steps.empty() || pipeline->steps.size() > 64) {
                transition(job, "failed");
                finish_job(job);
                continue;
            }
            job.pipeline = std::move(*pipeline);
            active_ = id;
            transition(job, "running");
            if (!start_step(job)) {
                process_.reset();
                active_.clear();
                transition(job, job.interrupted ? "cancelled" : "failed");
                finish_job(job);
                continue;
            }
            return;
        }
    } while (pump_again_);
}

bool JobRunner::start_step(Job& job) {
    if (stopping_ || job.interrupted || job.step >= job.pipeline.steps.size()) return false;
    try {
        auto spec = job.pipeline.steps[job.step]();
        if (!spec) {
            job.error = spec.error();
            return false;
        }
        if (job.interrupted || stopping_) return false;
        auto process = factory_();
        if (!process) return false;
        job.auth_marker.clear();
        for (const auto& [key, value] : spec->env)
            if (key == "BRAURSER_JOB_DIR") job.auth_marker = std::filesystem::path(value) / "auth-failed";
        const auto id = job.id;
        const bool privileged = (spec->executable == "/usr/bin/pkexec" || spec->executable == "/usr/bin/sudo") &&
            std::ranges::any_of(spec->args, [](const auto& argument) {
                return std::filesystem::path(argument).filename() == "braurser-pacman-helper";
            });
        process_ = std::move(process);
        job.privileged = privileged;
        if (!job.snapshot.aur_pins.empty() || job.pipeline.steps.size() > 1) job.commit = privileged;
        return process_->start(*spec,
            [this, id](std::string_view text) { if (auto it = jobs_.find(id); it != jobs_.end()) line(it->second, text); },
            [this, id](const core::ProcessResult& result) { if (auto it = jobs_.find(id); it != jobs_.end()) complete(it->second, result); });
    } catch (const std::exception&) { return false; }
}

Result<protocol::JobsCancelResult> JobRunner::cancel(std::string_view id, api::ConnectionId owner) {
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second.owner != owner) return std::unexpected(failure(protocol::kInvalidParams));
    auto& job = it->second;
    if (job.state == "queued" || (job.state == "needs-attention" && job.question)) {
        std::erase(pending_, job.id);
        if (job.check == Job::Check::Running) {
            plans_.cancel(job.check_flight);
            if (checking_ == job.id) checking_.clear();
        }
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
    if (it->second.error) result.error = nlohmann::json{{"code", it->second.error->code},
        {"message_key", it->second.error->message_key}, {"args", it->second.error->args}};
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
    const auto job_id = job.id;
    transition(job, "needs-attention");
    const auto current = jobs_.find(job_id);
    if (current == jobs_.end() || current->second.orphaned || !current->second.question) return;
    auto& questioned = current->second;
    add_event(questioned, "job.question", protocol::JobQuestionNotification{
        questioned.id, questioned.next_sequence + 1, *questioned.question});
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
    if (std::holds_alternative<protocol::BuildReviewQuestion>(job.question->choice.value)) job.reviewed = true;
    job.question.reset();
    job.choices.clear();
    const auto job_id = job.id;
    transition(job, "queued");
    it = jobs_.find(job_id);
    if (it == jobs_.end() || it->second.orphaned || it->second.finished)
        return std::unexpected(failure(protocol::kExpiredQuestion));
    pump();
    return protocol::AcceptedResult{true};
}

void JobRunner::disconnect(api::ConnectionId owner) {
    std::vector<std::string> ids;
    for (const auto& [id, job] : jobs_)
        if (job.owner == owner && job.question && !job.orphaned) ids.push_back(id);
    for (const auto& id : ids) {
        const auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second.orphaned) continue;
        it->second.orphaned = true;
        std::erase(pending_, id);
        finish_job(it->second);
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
