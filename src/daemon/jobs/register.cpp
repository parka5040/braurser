#include <braurser/daemon/jobs/register.hpp>
#include <braurser/daemon/core/process.hpp>
#include <braurser/domain/vercmp.hpp>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#include <format>
#include <git2.h>
#include <memory>
#include <chrono>
#include <set>
#include <sys/stat.h>

namespace braurser::daemon::jobs {
namespace {
Error unavailable() { return {protocol::kAuthorizationUnavailable, "auth.unavailable_title"}; }

helpers::HelperEnvironment helper_environment(const core::XdgPaths& paths) {
    passwd user{};
    passwd* found{};
    std::array<char, 16384> buffer{};
    if (getpwuid_r(getuid(), &user, buffer.data(), buffer.size(), &found) != 0 ||
        !found || !found->pw_name || !found->pw_dir) throw std::runtime_error("local user unavailable");
    helpers::HelperEnvironment environment;
    environment.home = found->pw_dir;
    environment.user = found->pw_name;
    environment.runtime_dir = paths.runtime;
    if (const char* display = std::getenv("DISPLAY")) environment.display = display;
    if (const char* display = std::getenv("WAYLAND_DISPLAY")) environment.wayland_display = display;
    if (const char* authority = std::getenv("XAUTHORITY")) environment.xauthority = authority;
    return environment;
}

class UserManualInstaller final : public ManualInstaller {
public:
    UserManualInstaller(core::ProcessRunner& runner, const core::XdgPaths& paths)
        : runner_(runner), paths_(paths) {}
    Result<void> run(const Snapshot& snapshot, std::string_view job_id) override {
        if (snapshot.plan.kind.value == "setup-helper") return setup_helper(snapshot, job_id);
        const auto scope = snapshot.selection.scope;
        if (!scope || (scope->value != "user" && scope->value != "system")) return refused();
        const auto environment = helper_environment(paths_);
        const auto kind = snapshot.plan.kind.value;
        if (kind == "manual-remove") {
            if (!snapshot.manual_receipt || snapshot.targets != std::vector<std::string>{
                scope->value == "user" ? snapshot.manual_receipt->id : snapshot.manual_receipt->pkgname}) return refused();
            if (scope->value == "user") {
                auto removed = manual::remove_user(runner_, environment.home, paths_.state, *snapshot.manual_receipt);
                if (!removed || !removed->modified_files.empty()) return refused();
                return {};
            }
            auto request = manual::system_remove_request(snapshot.manual_receipt->pkgname);
            if (!request || !official("manual-remove", {snapshot.manual_receipt->pkgname})) return refused();
            if (!verify_installed(snapshot.manual_receipt->pkgname, false)) return refused();
            const auto receipt = paths_.state / "braurser/manual/receipts" / (snapshot.manual_receipt->id + ".json");
            if (unlink(receipt.c_str()) != 0) return refused();
            return {};
        }
        if ((kind != "manual-install" && kind != "manual-upgrade") || !snapshot.manual_preview ||
            snapshot.targets.size() != 1 || job_id.empty() || job_id.size() > 128 ||
            !std::ranges::all_of(job_id, [](unsigned char ch) { return std::isalnum(ch) || ch == '-'; }))
            return refused();
        const auto& preview = *snapshot.manual_preview;
        if (preview.entry_points.size() != 1 || preview.selected_script_text.empty()) return refused();
        const auto source_url = manual::validate_git_url("https://" + preview.host + "/" + preview.owner_repo);
        if (!source_url || source_url->host != preview.host) return refused();
        const auto build_cache = scope->value == "system" ? environment.home / ".cache" : paths_.cache;
        const auto job_root = build_cache / "braurser/build/manual" / std::string(job_id);
        for (const auto& path : {build_cache / "braurser", build_cache / "braurser/build",
                                 build_cache / "braurser/build/manual", job_root, job_root / "source", job_root / "stage"})
            if (!core::ensure_private_dir(path)) return refused();
        const auto bare = paths_.cache / "braurser/manual" / preview.preview_id / "repo.git";
        auto checked = manual::preview_repository(bare, preview.commit, *source_url, preview.preview_id,
                                                   preview.entry_points.front().kind);
        if (!checked || checked->commit != preview.commit ||
            checked->selected_script_text != preview.selected_script_text ||
            !manual::materialize_commit(bare, preview.commit, job_root / "source")) return refused();
        const auto selected = preview.entry_points.front();
        if (selected.path != "install.sh" && selected.path != "Makefile") return refused();
        const bool system = scope->value == "system";
        if (!manual::run_staging(runner_, job_root / "source", job_root / "stage", environment.home,
                                 selected.kind, system ? manual::Scope::System : manual::Scope::User)) return refused();
        auto manifest = manual::validate_stage(job_root / "stage", system ? manual::Scope::System : manual::Scope::User);
        if (!manifest) return refused();
        manual::Receipt receipt;
        receipt.id = kind == "manual-upgrade" && !system ? snapshot.manual_receipt->id : preview.preview_id;
        receipt.name = snapshot.targets.front();
        receipt.url = source_url->url;
        receipt.host = preview.host;
        receipt.commit = preview.commit;
        receipt.entry_point = selected.path;
        receipt.scope = system ? manual::Scope::System : manual::Scope::User;
        receipt.files = manifest->files;
        receipt.launcher_ids = manifest->launchers;
        if (!system) {
            if (kind == "manual-upgrade") {
                if (!snapshot.manual_receipt || !manual::upgrade_user(runner_, job_root / "stage",
                        environment.home, paths_.state, *snapshot.manual_receipt, std::move(receipt))) return refused();
            } else if (!manual::install_user(runner_, job_root / "stage", environment.home, paths_.state,
                                             std::move(receipt))) return refused();
            return {};
        }
        auto built = manual::build_system_package(runner_, job_root / "stage", environment.home,
            environment.home / ".cache/braurser/build/manual" / std::string(job_id), receipt,
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        if (!built) return refused();
        auto request = manual::system_upgrade_request(*built);
        if (!request || !official(kind, {*built})) return refused();
        auto name = manual::render_pkgbuild(receipt,
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()), "any");
        if (!name) return refused();
        receipt.pkgname = name->first;
        if (!verify_installed(receipt.pkgname, true)) return refused();
        receipt.installed_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        if (!manual::record_system_install(paths_.state, receipt)) return refused();
        if (kind == "manual-upgrade" && snapshot.manual_receipt && snapshot.manual_receipt->id != receipt.id) {
            const auto old = paths_.state / "braurser/manual/receipts" / (snapshot.manual_receipt->id + ".json");
            if (unlink(old.c_str()) != 0) return refused();
        }
        return {};
    }
private:
    core::ProcessRunner& runner_;
    const core::XdgPaths& paths_;
    elevation::ProcAgentProbe probe_;
    elevation::ElevationSelector selector_{probe_};
    static Result<void> refused() {
        return std::unexpected(Error{protocol::kUnsafeTransaction, "error.generic_title"});
    }
    Result<void> setup_helper(const Snapshot& snapshot, std::string_view job_id) {
        if (!snapshot.helper_build || !snapshot.selection.helper || snapshot.targets.size() != 1 ||
            job_id.empty() || job_id.size() > 128 ||
            !std::ranges::all_of(job_id, [](unsigned char ch) { return std::isalnum(ch) || ch == '-'; }))
            return refused();
        const std::string name = snapshot.selection.helper->value == "yay" ? "yay-bin" :
            snapshot.selection.helper->value == "paru" ? "paru" : "";
        if (name.empty() || snapshot.targets.front() != name) return refused();
        const auto environment = helper_environment(paths_);
        const auto job_root = environment.home / ".cache/braurser/build/manual" / std::string(job_id);
        const auto root = job_root / name;
        for (const auto& path : {environment.home / ".cache/braurser",
                                 environment.home / ".cache/braurser/build",
                                 environment.home / ".cache/braurser/build/manual", job_root,
                                 root, job_root / "packages", job_root / "sources", job_root / "build"})
            if (!core::ensure_private_dir(path)) return refused();
        const auto bare = paths_.cache / "braurser/manual" /
            ("setup-" + snapshot.selection.helper->value) / "repo.git";
        if (!manual::materialize_commit(bare, snapshot.helper_build->commit, root)) return refused();
        auto build_file = core::read_bounded(root / "PKGBUILD", 256 * 1024);
        if (!build_file || *build_file != snapshot.helper_build->pkgbuild) return refused();
        core::ProcessSpec makepkg;
        makepkg.executable = "/usr/bin/makepkg";
        makepkg.args = {"--nodeps", "--noconfirm", "--noprogressbar"};
        makepkg.cwd = root;
        makepkg.env = {{"PATH", "/usr/bin"}, {"HOME", environment.home.string()}, {"LANG", "C.UTF-8"},
                       {"PKGDEST", (job_root / "packages").string()}, {"SRCDEST", (job_root / "sources").string()},
                       {"BUILDDIR", (job_root / "build").string()}};
        makepkg.timeout = std::chrono::minutes(30);
        makepkg.max_stdout = makepkg.max_stderr = 2 * 1024 * 1024;
        auto built = runner_.run(makepkg);
        if (!built || built->exit_code != 0 || built->signaled || built->timed_out ||
            built->out_truncated || built->err_truncated) return refused();
        std::optional<std::filesystem::path> archive;
        for (const auto& item : std::filesystem::directory_iterator(job_root / "packages")) {
            const auto filename = item.path().filename().string();
            if (!item.is_regular_file() || item.is_symlink() || !filename.starts_with(name + "-") ||
                !br_is_pkg_archive_name(filename.data(), filename.size())) continue;
            if (archive) return refused();
            archive = item.path();
        }
        if (!archive || !official("setup-helper", {archive->string()}) || !verify_installed(name, true))
            return refused();
        return {};
    }
    bool official(std::string_view kind, const std::vector<std::string>& targets) {
        elevation::Environment env;
        env.uid = getuid();
        env.askpass = BRAURSER_LIBEXEC_DIR "/braurser-askpass-gtk";
        const ElevationOptions options{selector_.select(env).method, BRAURSER_LIBEXEC_DIR, "gtk", "install"};
        auto spec = official_spec(kind, targets, options);
        if (!spec) return false;
        spec->max_stdout = spec->max_stderr = 8 * 1024 * 1024;
        auto result = runner_.run(*spec);
        return result && result->exit_code == 0 && !result->signaled && !result->timed_out &&
               !result->out_truncated && !result->err_truncated;
    }
    bool verify_installed(std::string_view name, bool expected) {
        auto configured = catalog::PacmanConfig(runner_).read();
        if (!configured) return false;
        auto installed = catalog::LocalDbReader{}.read(configured->db_path);
        if (!installed) return false;
        const bool found = std::ranges::any_of(*installed, [&](const auto& item) {
            return item.package.name == name;
        });
        return found == expected;
    }
};

class Coordinator {
public:
    Coordinator(api::Services& services, const core::XdgPaths& paths,
                api::Broadcaster& broadcaster, LivePlanSource::Report report, LivePlanSource::PreviewLookup preview)
        : services_(services), paths_(paths), broadcaster_(broadcaster),
          report_(std::move(report)), source_(services, read_runner_, report_, paths.state, paths.cache, std::move(preview)),
          plans_(source_), manual_installer_(read_runner_, paths_),
          runner_(plans_, [] { return std::make_unique<GioStreamingProcess>(); },
          [this](std::string_view, const protocol::JobEvent& event, api::ConnectionId owner) { notify(event, owner); },
          paths.state, auth(),
          [this](const Snapshot& snapshot, std::string_view id, const ElevationOptions& selection) {
              return build_spec(snapshot, id, selection);
          }, [this](const Snapshot& snapshot) { return verify(snapshot); },
          [this](std::string_view text) -> std::optional<helpers::PhaseEvent> {
              auto env = helper_environment(paths_);
              helpers::ParuHelper helper(env, read_runner_);
              return helper.classify_line(text);
          }, std::chrono::steady_clock::now,
          [this](const Snapshot& snapshot) { return after_success(snapshot); }, &manual_installer_) {}

    PlanManager& plans() { return plans_; }
    JobRunner& runner() { return runner_; }
    bool matches_report(std::string_view id) const {
        if (id.empty() || id.size() > 128 || !report_) return false;
        const auto report = report_();
        return report && report->report_id == id;
    }
private:
    api::Services& services_;
    core::XdgPaths paths_;
    api::Broadcaster& broadcaster_;
    LivePlanSource::Report report_;
    core::PosixProcessRunner read_runner_;
    LivePlanSource source_;
    PlanManager plans_;
    UserManualInstaller manual_installer_;
    elevation::ProcAgentProbe probe_;
    elevation::ElevationSelector selector_{probe_};
    JobRunner runner_;

    ElevationOptions auth() {
        elevation::Environment env;
        env.uid = getuid();
        env.askpass = BRAURSER_LIBEXEC_DIR "/braurser-askpass-gtk";
        return {selector_.select(env).method, BRAURSER_LIBEXEC_DIR, "gtk", "install"};
    }
    Result<core::ProcessSpec> build_spec(const Snapshot& snapshot, std::string_view id,
                                         const ElevationOptions& selection) {
        if (snapshot.helper == "auto" || snapshot.plan.kind.value == "remove")
            return official_spec(snapshot.plan.kind.value, snapshot.targets, selection);
        if (selection.method == elevation::Method::Unavailable)
            return std::unexpected(unavailable());
        if (snapshot.targets.size() != 1) return std::unexpected(Error{protocol::kInvalidParams, "error.generic_title"});
        auto name = domain::PackageName::make(snapshot.targets.front());
        if (!name) return std::unexpected(Error{protocol::kInvalidParams, "error.generic_title"});
        auto environment = helper_environment(paths_);
        std::unique_ptr<helpers::AURHelper> helper;
        if (snapshot.helper == "paru") helper = std::make_unique<helpers::ParuHelper>(environment, read_runner_);
        else if (snapshot.helper == "yay") helper = std::make_unique<helpers::YayHelper>(environment, read_runner_);
        else return std::unexpected(Error{protocol::kUnsupportedCapability, "error.generic_title"});
        const auto root = environment.home / ".cache/braurser/build" / snapshot.helper / std::string(id);
        helpers::CommunityRequest request{*name, root, selection.libexec / "braurser-elevate",
            selection.method == elevation::Method::Pkexec ? "pkexec" : "sudo-askpass",
            selection.method == elevation::Method::SudoAskpass ?
                std::optional(selection.libexec / ("braurser-askpass-" + selection.ui)) : std::nullopt,
            environment.home};
        auto spec = helper->build_install_spec(request);
        if (!spec) return std::unexpected(Error{protocol::kUntrustedInput, "error.generic_title"});
        spec->env.emplace_back("BRAURSER_AUTH_ACTION", "install");
        return *spec;
    }
    bool verify(const Snapshot& snapshot) {
        if (snapshot.plan.kind.value == "manual-remove" && snapshot.manual_receipt) {
            const auto path = paths_.state / "braurser/manual/receipts" /
                (snapshot.manual_receipt->id + ".json");
            struct stat info{};
            return lstat(path.c_str(), &info) != 0 && errno == ENOENT;
        }
        if (snapshot.plan.kind.value.starts_with("manual-") && snapshot.manual_preview &&
            snapshot.selection.scope && snapshot.selection.scope->value == "user") {
            const auto id = snapshot.plan.kind.value == "manual-upgrade" ?
                snapshot.manual_receipt->id : snapshot.manual_preview->preview_id;
            auto bytes = core::read_bounded(paths_.state / "braurser/manual/receipts" / (id + ".json"), 512 * 1024);
            if (!bytes) return false;
            auto receipt = nlohmann::json::parse(*bytes, nullptr, false);
            return receipt.is_object() && receipt.value("commit", std::string{}) == snapshot.manual_preview->commit &&
                   receipt.value("scope", std::string{}) == "user";
        }
        if (snapshot.manual_preview && snapshot.selection.scope && snapshot.selection.scope->value == "system") {
            const auto receipt_file = paths_.state / "braurser/manual/receipts" /
                ((snapshot.plan.kind.value == "manual-upgrade" && snapshot.manual_receipt ?
                  snapshot.manual_receipt->id : snapshot.manual_preview->preview_id) + ".json");
            if (snapshot.plan.kind.value == "manual-upgrade" && snapshot.manual_receipt) {
                const auto new_receipt = paths_.state / "braurser/manual/receipts" /
                    (snapshot.manual_preview->preview_id + ".json");
                return core::read_bounded(new_receipt, 512 * 1024).has_value();
            }
            return core::read_bounded(receipt_file, 512 * 1024).has_value();
        }
        auto config = catalog::PacmanConfig(read_runner_).read();
        if (!config) return false;
        auto installed = catalog::LocalDbReader{}.read(config->db_path);
        if (!installed) return false;
        for (const auto& target : snapshot.targets) {
            const auto slash = target.find('/');
            const auto name = slash == std::string::npos ? target : target.substr(slash + 1);
            const auto found = std::ranges::find_if(*installed, [&](const auto& item) {
                return item.package.name == name;
            });
            if (snapshot.plan.kind.value == "remove" || snapshot.plan.kind.value == "manual-remove") {
                if (found != installed->end()) return false;
            } else if (found == installed->end() ||
                       (!snapshot.planned_version.empty() &&
                        domain::vercmp(found->package.version, snapshot.planned_version) < 0)) return false;
        }
        return true;
    }
    bool after_success(const Snapshot& snapshot) {
        const bool official = !snapshot.plan.kind.value.starts_with("manual-") ||
            (snapshot.selection.scope && snapshot.selection.scope->value == "system");
        if (official || !snapshot.plan.restart.empty()) {
            auto current = services_.restart_state({});
            if (!current) return false;
            auto next = *current;
            constexpr std::array<std::string_view, 6> priority{
                "none", "restart-app", "restart-service", "logout", "reboot", "unknown"};
            auto severity = [&](std::string_view value) {
                const auto it = std::ranges::find(priority, value);
                return static_cast<std::size_t>(it - priority.begin());
            };
            std::set<std::string> reasons(next.reason_keys.begin(), next.reason_keys.end());
            for (const auto& advice : snapshot.plan.restart) {
                if (severity(advice.level.value) > severity(next.level.value)) next.level = advice.level;
                reasons.insert(advice.reason_key);
            }
            if (snapshot.plan.restart.empty()) {
                next.level = {"unknown"};
                reasons.insert("restart.unknown");
            }
            next.reason_keys.assign(reasons.begin(), reasons.end());
            if (!next.since) next.since = std::format("{:%FT%TZ}",
                std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
            if (!core::ensure_private_dir(paths_.state / "braurser") ||
                !core::atomic_write(paths_.state / "braurser/restart.json", nlohmann::json(next).dump(), 0600))
                return false;
        }
        static_cast<void>(services_.refresh(protocol::RefreshParams{}));
        return true;
    }
    void notify(const protocol::JobEvent& event, api::ConnectionId owner) {
        if (event.kind == "job.progress") broadcaster_.emit<api::JobProgressNotificationEvent>(
            event.payload.get<protocol::JobProgressNotification>());
        else if (event.kind == "job.log") broadcaster_.emit<api::JobLogNotificationEvent>(
            event.payload.get<protocol::JobLogNotification>());
        else if (event.kind == "job.state") broadcaster_.emit<api::JobStateNotificationEvent>(
            event.payload.get<protocol::JobStateNotification>());
        else if (event.kind == "job.authState") broadcaster_.emit<api::JobAuthStateNotificationEvent>(
            event.payload.get<protocol::JobAuthStateNotification>());
        else if (event.kind == "job.question") broadcaster_.emit_to<api::JobQuestionNotificationEvent>(owner,
            event.payload.get<protocol::JobQuestionNotification>());
    }
};

template<class Method, class Action>
void bind(api::HandlerRegistry& registry, std::shared_ptr<Coordinator> coordinator,
          Action action) {
    registry.register_handler<Method>([coordinator, action](const auto& params, api::RequestContext request, auto reply) {
        const auto owner = request.connection;
        if (!owner || request.uid != getuid()) { reply.error(unavailable()); return; }
        auto result = action(*coordinator, params, owner);
        if (result) reply.ok(*result);
        else reply.error(result.error());
    });
}
}

void register_handlers(api::HandlerRegistry& registry, api::Services& services,
                       const core::XdgPaths& paths, api::Broadcaster& broadcaster,
                       LivePlanSource::Report report, api::Server& server, LivePlanSource::PreviewLookup preview) {
    auto coordinator = std::make_shared<Coordinator>(services, paths, broadcaster, std::move(report), std::move(preview));
    server.on_disconnect([weak = std::weak_ptr(coordinator)](api::ConnectionId id) {
        if (auto owner = weak.lock()) owner->runner().disconnect(id);
    });
    bind<api::PlanParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::PlanParams& params, api::ConnectionId owner) { return context.plans().create(params, owner); });
    bind<api::UpdatesPlanParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::UpdatesPlanParams& params, api::ConnectionId owner) -> Result<protocol::UpdatesPlanResult> {
            protocol::PlanParams selection;
            if (params.selection.kind.value != "system" || params.selection.id || params.alsoInstallPackageId)
                return std::unexpected(Error{protocol::kUnsupportedCapability, "error.generic_title"});
            if (!context.matches_report(params.reportId))
                return std::unexpected(Error{protocol::kStalePlan, "error.generic_title"});
            selection.kind = "update-system";
            auto plan = context.plans().create(selection, owner);
            if (!plan) return std::unexpected(plan.error());
            return protocol::UpdatesPlanResult{plan->plan_id, {*plan}, plan->restart, true, plan->blockers};
        });
    auto enqueue = [](std::string_view kind) {
        return [kind](auto& context, const auto& params, api::ConnectionId owner) {
            return context.runner().enqueue(params.planId, params.approvalToken, owner, kind);
        };
    };
    bind<api::InstallParamsMethod>(registry, coordinator, enqueue("install"));
    bind<api::RemoveParamsMethod>(registry, coordinator, enqueue("remove"));
    bind<api::UpgradeSystemParamsMethod>(registry, coordinator, enqueue("update-system"));
    bind<api::UpgradeCommunityParamsMethod>(registry, coordinator, enqueue("update-community"));
    bind<api::ManualInstallParamsMethod>(registry, coordinator, enqueue("manual-install"));
    bind<api::ManualUpgradeParamsMethod>(registry, coordinator, enqueue("manual-upgrade"));
    bind<api::ManualRemoveParamsMethod>(registry, coordinator, enqueue("manual-remove"));
    bind<api::SetupHelperParamsMethod>(registry, coordinator, enqueue("setup-helper"));
    bind<api::JobsGetParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::JobsGetParams& params, api::ConnectionId owner) { return context.runner().get(params, owner); });
    bind<api::JobsListParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::JobsListParams& params, api::ConnectionId owner) { return context.runner().list(params, owner); });
    bind<api::JobsCancelParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::JobsCancelParams& params, api::ConnectionId owner) { return context.runner().cancel(params.jobId, owner); });
    bind<api::JobsAnswerParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::JobsAnswerParams& params, api::ConnectionId owner) { return context.runner().answer(params, owner); });
    bind<api::JobsDequeueParamsMethod>(registry, coordinator,
        [](auto& context, const protocol::JobsDequeueParams& params, api::ConnectionId owner) { return context.runner().dequeue(params.jobId, owner); });
}

}
