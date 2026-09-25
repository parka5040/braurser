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

    /// in: real_db is pacman's absolute DBPath; runtime is XDG_RUNTIME_DIR; fakeroot is an absolute executable.
    /// out: a privately refreshed sync snapshot without writing the real sync database.
    /// err: Unavailable means fakeroot is missing or not executable.
    static std::expected<TempSyncDb, UpdateError> create(core::ProcessRunner& runner,
        const std::filesystem::path& real_db, const std::filesystem::path& runtime,
        const std::filesystem::path& fakeroot = "/usr/bin/fakeroot");
    /// out: absolute database root valid for this object's lifetime.
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    TempSyncDb(std::filesystem::path root, std::filesystem::path path);
    std::filesystem::path root_;
    std::filesystem::path path_;
};

class OfficialUpdateChecker {
public:
    explicit OfficialUpdateChecker(core::ProcessRunner& runner) : runner_(runner) {}
    /// in: temp_db is a refreshed private database root.
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

struct RunningConsumer {
    int pid{};
    std::string comm;
    bool service{};
    friend auto operator<=>(const RunningConsumer&, const RunningConsumer&) = default;
};
struct ConsumerScan {
    std::map<std::string, std::set<RunningConsumer>> files;
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
    /// out: maps executable and mapped files of current-uid processes to consumers.
    /// err: unreadable processes increase unreadable_processes and clear coverage_complete.
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
