#ifndef BRAURSER_DAEMON_UPDATES_UPDATES_HPP
#define BRAURSER_DAEMON_UPDATES_UPDATES_HPP

#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/process.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <braurser/domain/restart.hpp>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace braurser::daemon::updates {

enum class UpdateErrorCode { InvalidInput, Unavailable, FileFailure, ProcessFailure, InvalidOutput };
struct UpdateError { UpdateErrorCode code; std::string detail; };
struct PackageUpdate {
    std::string name;
    std::string from;
    std::string to;
    bool ignored{};
    friend bool operator==(const PackageUpdate&, const PackageUpdate&) = default;
};

class TempSyncDb {
public:
    TempSyncDb(const TempSyncDb&) = delete;
    TempSyncDb& operator=(const TempSyncDb&) = delete;
    TempSyncDb(TempSyncDb&& other) noexcept;
    TempSyncDb& operator=(TempSyncDb&& other) noexcept;
    ~TempSyncDb();

    /// in: real_db and cache are absolute; report_id matches [A-Za-z0-9_-]{1,128}.
    /// out: a private disk-backed copy under cache/braurser/syncdb-check/report_id.
    /// err: refresh requires executable fakeroot; failures remove the incomplete copy.
    static std::expected<TempSyncDb, UpdateError> create(core::ProcessRunner& runner,
        const std::filesystem::path& real_db, const std::filesystem::path& cache,
        std::string_view report_id, bool refresh = true,
        const std::filesystem::path& fakeroot = "/usr/bin/fakeroot");
    /// out: retains the snapshot after this object's lifetime.
    void retain() noexcept { retained_ = true; }
    /// in: cache is absolute; keep is empty or a valid current report id.
    /// out: removes older report directories without following symbolic links.
    static void sweep(const std::filesystem::path& cache, std::string_view keep = {});
    /// out: absolute database root valid until destruction, or the next sweep if retained.
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    TempSyncDb(std::filesystem::path root, std::filesystem::path path);
    std::filesystem::path root_;
    std::filesystem::path path_;
    bool retained_{};
};

class OfficialUpdateChecker {
public:
    explicit OfficialUpdateChecker(core::ProcessRunner& runner) : runner_(runner) {}
    /// in: temp_db is a private database root copied for this report, optionally refreshed.
    /// out: validated newer versions in package-name order, including ignored updates.
    std::expected<std::vector<PackageUpdate>, UpdateError> check(const std::filesystem::path& temp_db) const;
private:
    core::ProcessRunner& runner_;
};

class CommunityUpdateChecker {
public:
    CommunityUpdateChecker(core::ProcessRunner& runner, const helpers::AURHelper& helper)
        : runner_(runner), helper_(helper) {}
    /// in: foreign_names is the caller's installed foreign-package set.
    /// out: newer foreign updates in package-name order.
    std::expected<std::vector<PackageUpdate>, UpdateError> check(const std::set<std::string>& foreign_names) const;
private:
    core::ProcessRunner& runner_;
    const helpers::AURHelper& helper_;
};

/// in: temp_db and real_db are absolute database roots; official_updates_found is the check result.
/// out: true if updates exist or any repository database has changed in size or SHA-256 digest.
/// err: unreadable, missing, symbolic or oversized databases fail closed.
std::expected<bool, UpdateError> is_real_db_stale(const std::filesystem::path& temp_db,
    const std::filesystem::path& real_db, bool official_updates_found);

struct ManualUpdate { std::string receipt_id; };
struct UpdateReport {
    std::vector<PackageUpdate> official;
    std::vector<PackageUpdate> community;
    std::vector<ManualUpdate> manual;
};
enum class SelectionKind { System, Community, Manual };
struct UpdateSelection { SelectionKind kind; std::string id; };
struct SelectionTransaction { std::string action; std::vector<std::string> args; };
struct SelectionBlocker { std::string code; std::string detail; };
struct SelectionDecision {
    bool allowed{};
    std::vector<SelectionTransaction> transactions;
    std::vector<SelectionBlocker> blockers;
    std::string reason;
};
class UpdateSelectionPolicy {
public:
    /// in: repo_dependencies contains resolved repo build and runtime dependencies for a community target.
    /// out: an indivisible system transaction or one selected community or manual transaction.
    SelectionDecision decide(const UpdateReport& report, const UpdateSelection& selection,
        const std::set<std::string>& repo_dependencies = {}) const;
    /// in: name is a new official package and real_db_fresh comes from is_real_db_stale.
    /// out: refreshes and upgrades everything before install when the real database is stale.
    SelectionDecision install_official(std::string_view name, bool real_db_fresh) const;
};

class RestartRuleLoader {
public:
    /// in: path identifies a bounded restart-rules.json file.
    /// out: validated rules in document order.
    std::expected<std::vector<domain::RestartRule>, UpdateError> load(const std::filesystem::path& path) const;
};

inline constexpr std::size_t kMaxConsumerAssociations = 65536;

struct RunningConsumers {
    bool service{};
};
struct ConsumerScan {
    std::map<std::string, RunningConsumers> files;
    std::size_t unreadable_processes{};
    bool coverage_complete{};
};
class ConsumerScanner {
public:
    virtual ~ConsumerScanner() = default;
    /// out: running consumers and explicit incomplete-coverage indication.
    virtual ConsumerScan scan() const = 0;
};
class RunningConsumerScanner final : public ConsumerScanner {
public:
    explicit RunningConsumerScanner(std::filesystem::path proc_root = "/proc");
    /// out: maps executable and mapped files of stable current-uid processes to aggregate service flags.
    /// err: unreadable or changed processes and exhausted association budgets clear coverage_complete.
    ConsumerScan scan() const override;
private:
    std::filesystem::path proc_root_;
};

class InstalledFilesSource {
public:
    virtual ~InstalledFilesSource() = default;
    /// in: name and version identify an installed local-database record.
    /// out: bounded pacman files-record text or a failure.
    virtual std::expected<std::string, UpdateError> files(std::string_view name, std::string_view version) const = 0;
};
class LocalDbFilesSource final : public InstalledFilesSource {
public:
    LocalDbFilesSource(const catalog::LocalDbReader& reader, std::filesystem::path db_path)
        : reader_(reader), db_path_(std::move(db_path)) {}
    std::expected<std::string, UpdateError> files(std::string_view name, std::string_view version) const override;
private:
    const catalog::LocalDbReader& reader_;
    std::filesystem::path db_path_;
};
struct RestartReport {
    std::vector<domain::RestartAdvice> packages;
    domain::RestartLevel overall{domain::RestartLevel::Unknown};
    bool coverage_complete{};
};
class RestartAdvisor {
public:
    RestartAdvisor(const InstalledFilesSource& files, const ConsumerScanner& scanner)
        : files_(files), scanner_(scanner) {}
    /// in: updates are installed package updates and rules have passed loader validation.
    /// out: advice sorted by package name; missing files or scan coverage leave unknown classifications unknown.
    RestartReport advise(const std::vector<PackageUpdate>& updates,
        const std::vector<domain::RestartRule>& rules) const;
private:
    const InstalledFilesSource& files_;
    const ConsumerScanner& scanner_;
};

}
#endif
