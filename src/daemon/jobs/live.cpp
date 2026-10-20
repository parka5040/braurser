#include <braurser/daemon/jobs/live.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/domain/protection.hpp>
#include <braurser/domain/vercmp.hpp>
#include <braurser/daemon/jobs/aur.hpp>
#include <braurser/daemon/cache/store.hpp>
#include <sys/utsname.h>
#include <braurser/validate/validate.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <set>
#include <sys/stat.h>
#include <sstream>
#include <pwd.h>
#include <array>
#include <unistd.h>
#include <glib.h>

namespace braurser::daemon::jobs {
namespace {
Error failure(int code) { return {code, "error.generic_title"}; }

class CancellableRunner final : public core::ProcessRunner {
public:
    CancellableRunner(core::ProcessRunner& runner, const std::atomic<bool>& cancelled)
        : runner_(runner), cancelled_(cancelled) {}
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec& spec) override {
        if (cancelled_) return std::unexpected(core::ProcessError{ECANCELED, "cancelled"});
        auto copy = spec;
        copy.cancel = &cancelled_;
        return runner_.run(copy);
    }
private:
    core::ProcessRunner& runner_;
    const std::atomic<bool>& cancelled_;
};

std::vector<std::string> words(std::string_view line) {
    std::istringstream stream{std::string(line)};
    std::vector<std::string> result;
    for (std::string field; stream >> field;) result.push_back(std::move(field));
    return result;
}

bool parse_print(std::string_view output, Snapshot& snapshot, std::string_view kind,
                 const std::vector<catalog::LocalRecord>& installed) {
    if (output.size() > 4 * 1024 * 1024) return false;
    std::set<std::string> seen;
    for (std::string_view remaining = output; !remaining.empty();) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        if (line.size() > 1024 || line.find('\0') != std::string_view::npos) return false;
        if (!line.empty()) {
            auto fields = words(line);
            if (fields.size() != (kind == "remove" ? 2U : 4U) ||
                !br_valid_pkgname(fields[0].c_str(), fields[0].size()) ||
                fields[1].size() > 128 || fields[1].empty() || !seen.insert(fields[0]).second)
                return false;
            protocol::PackageRef ref{{"official"}, {}, fields[0]};
            if (kind == "remove") {
                snapshot.plan.removals.push_back(ref);
            } else {
                if (!br_valid_repo_name(fields[2].c_str(), fields[2].size())) return false;
                ref.repo = fields[2];
                std::uint64_t size{};
                auto [finish, error] = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), size);
                if (error != std::errc{} || finish != fields[3].data() + fields[3].size()) return false;
                if (!snapshot.plan.download_bytes) snapshot.plan.download_bytes = 0;
                if (size > UINT64_MAX - *snapshot.plan.download_bytes) return false;
                *snapshot.plan.download_bytes += size;
                auto current = std::ranges::find_if(installed, [&](const auto& item) {
                    return item.package.name == fields[0];
                });
                if (current != installed.end()) {
                    if (domain::vercmp(current->package.version, fields[1]) < 0)
                        snapshot.plan.upgrades.push_back(ref);
                } else snapshot.plan.installs.push_back(ref);
            }
        }
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
    return seen.size() <= 512;
}

Result<void> bind_transaction(Snapshot& snapshot, const protocol::UpdateReport& report,
                              const std::filesystem::path& cache, core::ProcessRunner& runner) {
    if (report.report_id.empty() || report.report_id.size() > 128 || !cache.is_absolute() ||
        !std::ranges::all_of(report.report_id, [](unsigned char ch) {
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        })) return std::unexpected(failure(protocol::kStalePlan));
    const auto directory = cache / "braurser/syncdb-check" / report.report_id;
    struct stat info{};
    if (lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        return std::unexpected(failure(protocol::kStalePlan));
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/pacman";
    spec.args = {"--sync", "--sysupgrade", "--print", "--print-format", "%r/%n %v",
                 "--dbpath", directory.string()};
    if (snapshot.plan.kind.value == "update-and-install") {
        spec.args.insert(spec.args.end(), {"--needed", "--"});
        spec.args.insert(spec.args.end(), snapshot.targets.begin(), snapshot.targets.end());
    }
    spec.env = {{"LC_ALL", "C"}, {"PATH", "/usr/bin"}};
    spec.timeout = std::chrono::seconds(60);
    spec.max_stdout = 4 * 1024 * 1024;
    spec.max_stderr = 64 * 1024;
    auto preview = runner.run(spec);
    if (!preview || preview->exit_code != 0 || preview->signaled || preview->timed_out ||
        preview->out_truncated || preview->err_truncated || preview->out.size() > spec.max_stdout)
        return std::unexpected(failure(protocol::kUntrustedInput));
    if (snapshot.plan.upgrades.size() > 256 || snapshot.plan.installs.size() > 256 ||
        snapshot.plan.restart.size() > 256) return std::unexpected(failure(protocol::kUntrustedInput));
    std::set<std::string> seen;
    for (std::string_view remaining = preview->out; !remaining.empty();) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        const auto space = line.find(' ');
        const auto slash = line.find('/');
        if (line.size() > 512 || space == line.npos || slash == line.npos || slash >= space)
            return std::unexpected(failure(protocol::kUntrustedInput));
        const auto repo = line.substr(0, slash);
        const auto name = line.substr(slash + 1, space - slash - 1);
        const auto version = line.substr(space + 1);
        if (!br_valid_repo_name(repo.data(), repo.size()) || !br_valid_pkgname(name.data(), name.size()) ||
            version.empty() || version.size() > 128 ||
            !std::ranges::all_of(version, [](unsigned char ch) {
                return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' || ch == ':' || ch == '-' ||
                    ch == '_' || ch == '+' || ch == '~';
            }) || !seen.insert(std::string(name)).second)
            return std::unexpected(failure(protocol::kUntrustedInput));
        if (std::ranges::none_of(snapshot.plan.upgrades, [&](const auto& item) { return item.name == name; }) &&
            std::ranges::none_of(snapshot.plan.installs, [&](const auto& item) { return item.name == name; })) {
            snapshot.plan.installs.push_back({{"official"}, std::string(repo), std::string(name)});
            if (snapshot.plan.installs.size() > 256) return std::unexpected(failure(protocol::kUntrustedInput));
        }
        if (end == remaining.npos) break;
        remaining.remove_prefix(end + 1);
    }
    gchar* hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar*>(preview->out.data()), preview->out.size());
    snapshot.expected_transaction = hash;
    g_free(hash);
    snapshot.fingerprint += '\n';
    snapshot.fingerprint += snapshot.expected_transaction;
    return {};
}

std::optional<std::vector<std::string>> protected_names(
    const catalog::PacmanConfig& config, const catalog::LocalDbReader& reader,
    const std::filesystem::path& db_path, const std::vector<protocol::PackageRef>& removals,
    const std::vector<catalog::LocalRecord>& installed) {
    auto hold = config.hold_packages();
    if (!hold) return std::nullopt;
    std::vector<std::string> names;
    names.reserve(removals.size());
    for (const auto& item : removals) names.push_back(item.name);
    const std::set<std::string, std::less<>> removing(names.begin(), names.end());
    std::vector<std::string> kernels;
    std::size_t inspected = 0;
    for (const auto& record : installed) {
        const auto& name = record.package.name;
        const bool candidate = name.starts_with("linux");
        if (!candidate && !removing.contains(name)) continue;
        if (++inspected > 1024) return std::nullopt;
        auto files = reader.files(db_path, name + "-" + record.package.version);
        if (!files) {
            if (candidate) return std::nullopt;
            continue;
        }
        if (domain::installs_kernel_image(*files)) kernels.push_back(name);
    }
    return domain::protected_removals(names, *hold, kernels);
}

}

Result<Snapshot> user_removal_snapshot(const std::filesystem::path& state, std::string_view id) {
    auto invalid = [] { return std::unexpected(failure(protocol::kUntrustedInput)); };
    if (!state.is_absolute() || state.lexically_normal() != state || id.empty() || id.size() > 128 ||
        !std::ranges::all_of(id, [](unsigned char ch) { return std::isalnum(ch) || ch == '-'; }))
        return invalid();
    const auto directory = state / "braurser/manual/receipts";
    for (const auto& part : {state / "braurser", state / "braurser/manual", directory}) {
        struct stat info{};
        if (lstat(part.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) ||
            info.st_uid != getuid() || (info.st_mode & 0777) != 0700) return invalid();
    }
    const auto file = directory / (std::string(id) + ".json");
    struct stat info{};
    if (lstat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0600) return invalid();
    auto bytes = core::read_bounded(file, manual::kMaxReceiptBytes);
    if (!bytes) return invalid();
    auto data = nlohmann::json::parse(*bytes, nullptr, false);
    if (!data.is_object()) return invalid();
    manual::Receipt receipt;
    try {
        receipt.id = data.at("id").get<std::string>();
        receipt.name = data.at("name").get<std::string>();
        receipt.url = data.at("url").get<std::string>();
        receipt.host = data.at("host").get<std::string>();
        receipt.commit = data.at("commit").get<std::string>();
        receipt.entry_point = data.at("entry_point").get<std::string>();
        receipt.installed_at = data.at("installed_at").get<std::int64_t>();
        if (!data.at("scope").is_string() || receipt.id != id ||
            (data.at("scope") != "user" && data.at("scope") != "system") ||
            !br_valid_pkgname(receipt.name.c_str(), receipt.name.size()) ||
            (receipt.entry_point != "install.sh" && receipt.entry_point != "Makefile")) return invalid();
        receipt.scope = data.at("scope") == "system" ? manual::Scope::System : manual::Scope::User;
        auto url = manual::validate_git_url(receipt.url);
        if (!url || url->host != receipt.host || receipt.commit.size() != 40 ||
            !std::ranges::all_of(receipt.commit, [](unsigned char ch) { return std::isxdigit(ch); })) return invalid();
        auto valid_path = [](const std::string& path) {
            if (path.empty() || path.size() > 4096 || path.find('\0') != std::string::npos) return false;
            const std::filesystem::path relative(path);
            if (relative.is_absolute()) return false;
            for (const auto& part : relative)
                if (part.empty() || part == "." || part == ".." || part.native().size() > 255) return false;
            return true;
        };
        const auto& files = data.at("files");
        const auto& backups = data.at("backups");
        const auto& launchers = data.at("launcher_ids");
        if (!files.is_array() || files.empty() || files.size() > manual::kMaxReceiptFiles ||
            !backups.is_array() || backups.size() > manual::kMaxReceiptFiles ||
            !launchers.is_array() || launchers.size() > manual::kMaxReceiptFiles) return invalid();
        for (const auto& item : files) {
            manual::StagedFile entry{item.at("path").get<std::string>(),
                                     item.at("sha256").get<std::string>(), item.at("mode").get<unsigned>()};
            if (!valid_path(entry.path) || entry.sha256.size() != 64 ||
                !std::ranges::all_of(entry.sha256, [](unsigned char ch) { return std::isxdigit(ch); }) ||
                (entry.mode != 0120000U && entry.mode > 0777U)) return invalid();
            receipt.files.push_back(std::move(entry));
        }
        for (const auto& item : backups) {
            auto path = item.at("path").get<std::string>();
            auto backup = item.at("backup").get<std::string>();
            if (!valid_path(path) || !valid_path(backup)) return invalid();
            receipt.backups.emplace_back(std::move(path), std::move(backup));
        }
        for (const auto& item : launchers) {
            auto launcher = item.get<std::string>();
            if (launcher.empty() || launcher.size() > 255 ||
                launcher.find_first_of("/\r\n") != std::string::npos ||
                launcher.find('\0') != std::string::npos)
                return invalid();
            receipt.launcher_ids.push_back(std::move(launcher));
        }
        if (data.contains("pkgname")) {
            receipt.pkgname = data.at("pkgname").get<std::string>();
            if (receipt.pkgname.size() > 256 ||
                (receipt.scope == manual::Scope::System &&
                 (!br_valid_pkgname(receipt.pkgname.data(), receipt.pkgname.size()) ||
                  !receipt.pkgname.starts_with("braurser-git-")))) return invalid();
        }
    } catch (const std::exception&) { return invalid(); }
    Snapshot result;
    result.plan.kind = {"manual-remove"};
    result.plan.removals = {{{"git"}, {}, receipt.name}};
    result.fingerprint = std::move(*bytes);
    result.targets = receipt.scope == manual::Scope::System ?
        std::vector<std::string>{receipt.pkgname} : std::vector<std::string>{receipt.id};
    result.manual_receipt = std::move(receipt);
    return result;
}

namespace {
std::vector<manual::Receipt> recorded_receipts(const std::filesystem::path& state) {
    std::vector<manual::Receipt> result;
    const auto directory = state / "braurser/manual/receipts";
    std::error_code error;
    std::filesystem::directory_iterator entries(directory, error);
    if (error) return result;
    std::size_t count = 0;
    for (const auto end = std::filesystem::directory_iterator{}; entries != end && ++count <= 4096;
         entries.increment(error)) {
        if (error) break;
        const auto file = entries->path().filename().string();
        if (!file.ends_with(".json")) continue;
        auto recorded = user_removal_snapshot(state, std::string_view(file).substr(0, file.size() - 5));
        if (recorded && recorded->manual_receipt) result.push_back(std::move(*recorded->manual_receipt));
    }
    return result;
}
}

LivePlanSource::LivePlanSource(api::Services& services, core::ProcessRunner& runner, Report report,
                               std::filesystem::path state, std::filesystem::path cache, PreviewLookup preview,
                               Detail detail)
    : services_(services), runner_(runner), report_(std::move(report)),
      state_(std::move(state)), cache_(std::move(cache)), preview_(std::move(preview)), detail_(std::move(detail)) {}

Result<Snapshot> LivePlanSource::snapshot(const protocol::PlanParams& selection) {
    const std::atomic<bool> cancelled{false};
    return prepare(selection)(cancelled);
}

std::string LivePlanSource::inputs(const protocol::PlanParams& selection) {
    const auto report = report_ ? report_() : std::nullopt;
    auto fields = nlohmann::json::array({report ? nlohmann::json(*report) : nlohmann::json("no-report")});
    if ((selection.kind == "manual-install" || selection.kind == "manual-upgrade") && selection.previewId) {
        const auto preview = preview_ ? preview_(*selection.previewId) : std::nullopt;
        if (preview) {
            auto entries = nlohmann::json::array();
            for (const auto& entry : preview->entry_points)
                entries.push_back(nlohmann::json::array({entry.path, static_cast<int>(entry.kind)}));
            fields.push_back(nlohmann::json::array({preview->preview_id, preview->host, preview->owner_repo,
                preview->commit, preview->selected_script_text, preview->staging_supported, std::move(entries)}));
        } else fields.push_back("no-preview");
    }
    const auto bytes = fields.dump();
    gchar* hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar*>(bytes.data()), bytes.size());
    std::string result(hash);
    g_free(hash);
    return result;
}

PlanTask LivePlanSource::prepare(const protocol::PlanParams& selection) {
    Inputs inputs;
    inputs.report = report_ ? report_() : std::nullopt;
    if (selection.kind == "manual-install" || selection.kind == "manual-upgrade") {
        if (!preview_) return [](const std::atomic<bool>& cancelled) -> Result<Snapshot> {
            return std::unexpected(failure(cancelled ? protocol::kCancelled : protocol::kInvalidParams));
        };
        if (selection.previewId) inputs.preview = preview_(*selection.previewId);
    }
    return [this, selection, inputs = std::move(inputs)](const std::atomic<bool>& cancelled) {
        return compute(selection, inputs, cancelled);
    };
}

Result<Snapshot> LivePlanSource::compute(const protocol::PlanParams& selection, const Inputs& inputs,
                                       const std::atomic<bool>& cancelled) {
    if (cancelled) return std::unexpected(failure(protocol::kCancelled));
    CancellableRunner cancellable(runner_, cancelled);
    catalog::PacmanConfig config(cancellable);
    manual::CurlGitHttp git(cache_, &cancelled);
    const auto lookup = [this](const protocol::GetParams& params) {
        return detail_ ? detail_(params) : services_.get(params);
    };
    if (selection.kind == "manual-remove") {
        if (!selection.receiptId || !selection.scope ||
            selection.packageId || selection.previewId || selection.variantId || selection.helper)
            return std::unexpected(failure(protocol::kInvalidParams));
        auto removed = user_removal_snapshot(state_, *selection.receiptId);
        if (!removed) return removed;
        if (selection.scope->value != (removed->manual_receipt->scope == manual::Scope::User ? "user" : "system"))
            return std::unexpected(failure(protocol::kInvalidParams));
        return removed;
    }
    if (selection.kind == "setup-helper") {
        if (!selection.helper || (selection.helper->value != "paru" && selection.helper->value != "yay") ||
            !cache_.is_absolute() || selection.packageId || selection.receiptId || selection.previewId ||
            selection.scope || selection.variantId) return std::unexpected(failure(protocol::kInvalidParams));
        const std::string pkgname = selection.helper->value == "yay" ? "yay-bin" : "paru";
        const auto url = "https://aur.archlinux.org/" + pkgname + ".git";
        if (cancelled) return std::unexpected(failure(protocol::kCancelled));
        auto fetched = manual::fetch_build_repository(git, url, cache_, "setup-" + selection.helper->value);
        if (!fetched) return std::unexpected(failure(protocol::kUntrustedInput));
        Snapshot result;
        result.plan.kind = {"setup-helper"};
        result.plan.installs.push_back({{"community"}, {}, pkgname});
        result.plan.requires_build_review = true;
        result.plan.build_review_refs = result.plan.installs;
        result.fingerprint = fetched->commit + fetched->pkgbuild;
        result.targets = {pkgname};
        result.helper_build = std::move(*fetched);
        return result;
    }
    if (selection.kind == "manual-install" || selection.kind == "manual-upgrade") {
        if (!selection.previewId || !selection.scope || !cache_.is_absolute() ||
            selection.packageId || selection.helper || selection.variantId ||
            (selection.kind == "manual-install" && selection.receiptId))
            return std::unexpected(failure(protocol::kInvalidParams));
        const auto& preview = inputs.preview;
        if (!preview || preview->preview_id != *selection.previewId ||
            preview->commit.size() != 40 || preview->entry_points.empty() ||
            preview->selected_script_text.empty() || !preview->staging_supported)
            return std::unexpected(failure(protocol::kStalePlan));
        const auto url = manual::validate_git_url("https://" + preview->host + "/" + preview->owner_repo);
        if (!url || url->host != preview->host || url->owner_repo != preview->owner_repo)
            return std::unexpected(failure(protocol::kUntrustedInput));
        if (selection.kind == "manual-install" && selection.scope->value == "system") {
            auto pkgname = manual::system_package_name(url->url);
            if (!pkgname) return std::unexpected(failure(protocol::kUntrustedInput));
            auto repositories = config.read();
            if (!repositories) return std::unexpected(failure(protocol::kBusy));
            auto installed = local_.read(repositories->db_path);
            if (!installed) return std::unexpected(failure(protocol::kBusy));
            std::set<std::string> names;
            for (const auto& record : *installed) names.insert(record.package.name);
            if (!manual::system_package_available(*pkgname, url->url, recorded_receipts(state_), names))
                return std::unexpected(failure(protocol::kUnsafeTransaction));
        }
        const auto kind = preview->entry_points.front().kind;
        if (preview->entry_points.size() != 1) return std::unexpected(failure(protocol::kUntrustedInput));
        auto checked = manual::preview_repository(cache_ / "braurser/manual" / *selection.previewId / "repo.git",
            preview->commit, *url, *selection.previewId, kind);
        if (!checked || checked->commit != preview->commit ||
            checked->selected_script_text != preview->selected_script_text)
            return std::unexpected(failure(protocol::kStalePlan));
        Snapshot result;
        result.plan.kind = {selection.kind};
        result.manual_preview = *preview;
        std::string name = preview->owner_repo.substr(preview->owner_repo.find_last_of('/') + 1);
        std::ranges::transform(name, name.begin(), [](unsigned char ch) {
            ch = static_cast<unsigned char>(std::tolower(ch));
            return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '+' ? ch : '-';
        });
        if (!br_valid_pkgname(name.data(), name.size())) return std::unexpected(failure(protocol::kUntrustedInput));
        if (selection.kind == "manual-upgrade") {
            if (!selection.receiptId) return std::unexpected(failure(protocol::kInvalidParams));
            auto previous = user_removal_snapshot(state_, *selection.receiptId);
            if (!previous || previous->manual_receipt->url != url->url ||
                previous->manual_receipt->scope != (selection.scope->value == "user" ? manual::Scope::User : manual::Scope::System) ||
                previous->manual_receipt->commit == preview->commit)
                return std::unexpected(failure(protocol::kStalePlan));
            result.manual_receipt = *previous->manual_receipt;
            result.fingerprint = previous->fingerprint;
            result.plan.upgrades.push_back({{"git"}, {}, name});
        } else result.plan.installs.push_back({{"git"}, {}, name});
        result.fingerprint += preview->commit + url->url + preview->selected_script_text;
        result.targets = {name};
        return result;
    }
    Snapshot result;
    auto repositories = config.read();
    if (!repositories) return std::unexpected(failure(protocol::kBusy));
    auto local = local_.read(repositories->db_path);
    if (!local) return std::unexpected(failure(protocol::kBusy));
    const auto& report = inputs.report;
    const auto kind = selection.kind;
    result.plan.kind = {kind};
    if (kind == "update-system" || kind == "upgrade-system") {
        if (!report) return std::unexpected(failure(protocol::kStalePlan));
        result.plan.kind = {"update-system"};
        for (const auto& item : report->official) {
            result.plan.upgrades.push_back(item.package);
            result.plan.restart.push_back(item.restart);
        }
        auto bound = bind_transaction(result, *report, cache_, cancellable);
        if (!bound) return std::unexpected(bound.error());
        result.fingerprint = nlohmann::json(*report).dump() + result.fingerprint;
    } else if (kind == "install" || kind == "remove" || kind == "update-community" ||
               kind == "upgrade-community") {
        if (!selection.packageId || selection.packageId->size() > 256) return std::unexpected(failure(protocol::kInvalidParams));
        std::size_t slash{};
        const auto& id = *selection.packageId;
        if (!br_split_target(id.data(), id.size(), &slash)) return std::unexpected(failure(protocol::kInvalidParams));
        const auto name = id.substr(slash == id.size() ? 0 : slash + 1);
        auto detail = lookup(protocol::GetParams{id});
        if (!detail) return std::unexpected(detail.error());
        result.fingerprint = nlohmann::json(*detail).dump();
        const bool community = detail->details.package.source.value == "community";
        if (kind == "update-community" || kind == "upgrade-community") {
            if (!community || !report) return std::unexpected(failure(protocol::kStalePlan));
            updates::UpdateReport converted;
            for (const auto& item : report->official)
                converted.official.push_back({item.package.name, item.from_version, item.to_version});
            for (const auto& item : report->community)
                converted.community.push_back({item.package.name, item.from_version, item.to_version});
            auto decision = policy_.decide(converted, {updates::SelectionKind::Community, name});
            if (!decision.allowed) return std::unexpected(failure(protocol::kSystemUpdateRequired));
            result.plan.kind = {"update-community"};
            result.fingerprint += nlohmann::json(*report).dump();
        } else if (kind == "remove" && std::ranges::none_of(*local, [&](const auto& record) {
                       return record.package.name == name;
                   })) {
            return std::unexpected(failure(protocol::kInvalidParams));
        }
        result.targets = {name};
        result.planned_version = detail->details.version;
        if (community && kind != "remove") {
            result.plan.kind = {kind == "install" ? "install" : "update-community"};
            if (access("/usr/bin/makepkg", X_OK) != 0 || access("/usr/bin/bwrap", X_OK) != 0 ||
                access("/usr/bin/bsdtar", X_OK) != 0 || !cache_.is_absolute())
                return std::unexpected(failure(protocol::kUnsupportedCapability));
            utsname machine{};
            if (uname(&machine) != 0) return std::unexpected(failure(protocol::kUnsupportedCapability));
            cache::Store store(cache_);
            std::optional<std::vector<catalog::RepoPackage>> official_packages;
            auto resolved = resolve_aur(result, name, machine.machine, *local,
                [&](std::string_view package) { return lookup(protocol::GetParams{std::string(package)}); },
                [&](std::string_view package) -> std::optional<std::string> {
                    catalog::SearchQuery query{std::string(package), {}, catalog::SortOrder::Relevance, {}, 200};
                    query.filters.aur_only = true;
                    for (int attempt = 0; attempt < 3; ++attempt) {
                        auto page = store.search(query);
                        if (!page) return std::nullopt;
                        bool moved = false;
                        for (const auto& hit : page->hits) {
                            if (hit.name != package || hit.source != domain::SourceKind::Community) continue;
                            auto record = store.package(hit.id);
                            if (record && *record && (*record)->community_record &&
                                (*record)->summary.name == hit.name && (*record)->summary.source == hit.source)
                                return (*record)->community_record->package_base;
                            moved = true;
                            break;
                        }
                        if (!moved) return std::nullopt;
                    }
                    return std::nullopt;
                },
                [&](std::string_view base) -> Result<manual::BuildTree> {
                    if (cancelled) return std::unexpected(failure(protocol::kCancelled));
                    auto fetched = manual::fetch_build_tree(git, base, cache_);
                    if (!fetched) return std::unexpected(Error{protocol::kUntrustedInput, fetched.error().code == "build-text-too-large" ? "detail.build_too_large" : "error.generic_title"});
                    return std::move(*fetched);
                },
                [&](const Dependency& dependency) -> std::optional<catalog::RepoPackage> {
                    if (!official_packages) {
                        official_packages.emplace();
                        for (const auto& repo : repositories->names) {
                            static_cast<void>(catalog::SyncDbReader{}.read(repositories->db_path, repo,
                                [&](catalog::RepoPackage&& package) {
                                    catalog::RepoPackage slim;
                                    slim.repo = std::move(package.repo);
                                    slim.name = std::move(package.name);
                                    slim.version = std::move(package.version);
                                    slim.provides = std::move(package.provides);
                                    official_packages->push_back(std::move(slim));
                                }));
                        }
                    }
                    for (const auto& package : *official_packages) {
                        if (package.name == dependency.name && satisfies_version(package.version, dependency)) return package;
                        for (const auto& value : package.provides) {
                            const auto provided = parse_dependency(value);
                            if (provided && provided->name == dependency.name &&
                                (dependency.comparison.empty() ||
                                 (provided->comparison == "=" && satisfies_version(provided->version, dependency)))) return package;
                        }
                    }
                    return std::nullopt;
                }, report && !report->official.empty());
            if (!resolved) return std::unexpected(resolved.error());
            auto transaction = plan_aur_official(result, cancellable);
            if (!transaction) return std::unexpected(transaction.error());
        } else {
            if (!community && detail->details.package.source.value != "official")
                return std::unexpected(failure(protocol::kInvalidParams));
            if (kind == "install") {
                result.targets = {id};
                if (report && !report->official.empty()) {
                    const auto& package = detail->details.package;
                    if (!package.repo || !br_valid_repo_name(package.repo->data(), package.repo->size()) ||
                        !br_valid_pkgname(package.name.data(), package.name.size()) || package.name != name)
                        return std::unexpected(failure(protocol::kUntrustedInput));
                    result.targets = {*package.repo + "/" + package.name};
                    result.plan.kind = {"update-and-install"};
                    for (const auto& update : report->official) {
                        result.plan.upgrades.push_back(update.package);
                        result.plan.restart.push_back(update.restart);
                    }
                    auto bound = bind_transaction(result, *report, cache_, cancellable);
                    if (!bound) return std::unexpected(bound.error());
                    result.fingerprint += nlohmann::json(*report).dump();
                }
            }
            if (kind == "remove") result.targets = {name};
            core::ProcessSpec spec;
            spec.executable = "/usr/bin/pacman";
            spec.env = {{"LC_ALL", "C"}};
            if (kind == "remove") spec.args = {"-R", "--print", "--print-format", "%n %v", "-s", "--", name};
            else spec.args = {"-S", "--print", "--print-format", "%n %v %r %s", "--needed", "--", id};
            spec.timeout = std::chrono::seconds(30);
            spec.max_stdout = 4 * 1024 * 1024;
            spec.max_stderr = 64 * 1024;
            if (result.plan.kind.value != "update-and-install") {
                auto preview = cancellable.run(spec);
                if (!preview || preview->exit_code != 0 || preview->out_truncated || preview->err_truncated ||
                    !parse_print(preview->out, result, kind, *local))
                    return std::unexpected(failure(protocol::kUnsafeTransaction));
                result.fingerprint += preview->out;
            }
            if (kind == "remove") {
                auto guarded = protected_names(config, local_, repositories->db_path, result.plan.removals, *local);
                if (!guarded) return std::unexpected(failure(protocol::kBusy));
                for (auto& blocked : *guarded)
                    result.plan.protected_blockers.push_back({{"official"}, {}, std::move(blocked)});
            }
        }
    } else return std::unexpected(failure(protocol::kUnsupportedCapability));
    if (result.targets.size() > 512) return std::unexpected(failure(protocol::kUntrustedInput));
    for (const auto& item : *local) {
        result.fingerprint += item.package.name;
        result.fingerprint += '@';
        result.fingerprint += item.package.version;
        result.fingerprint += '\n';
    }
    return result;
}

}
