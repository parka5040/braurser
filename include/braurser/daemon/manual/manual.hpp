#ifndef BRAURSER_DAEMON_MANUAL_MANUAL_HPP
#define BRAURSER_DAEMON_MANUAL_MANUAL_HPP

#include <braurser/daemon/core/process.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::manual {

struct ManualError {
    std::string code;
    std::string detail;
    std::optional<std::string> from_host;
    std::optional<std::string> to_host;
};

struct GitUrl {
    std::string url;
    std::string host;
    std::string owner_repo;
};

/// in: url is an untrusted repository URL.
/// out: normalized HTTPS URL, host and repository path.
/// err: rejects credentials, unsafe paths, ports and nonpublic IP literals.
std::expected<GitUrl, ManualError> validate_git_url(std::string_view url);

struct GitReply {
    class Body {
    public:
        static constexpr std::size_t kMaxBytes = 512ULL * 1024 * 1024;
        static constexpr std::size_t kReadChunk = 64 * 1024;
        /// out: an empty response without an open descriptor.
        Body() = default;
        /// out: closes the anonymous spool descriptor.
        ~Body();
        Body(const Body&) = delete;
        Body& operator=(const Body&) = delete;
        /// out: transfers ownership of the spool descriptor.
        Body(Body&& other) noexcept;
        /// out: closes the previous descriptor and transfers ownership.
        Body& operator=(Body&& other) noexcept;
        /// in: cache_root is an absolute XDG cache root; max_bytes may lower the response cap.
        /// out: an anonymous close-on-exec spool under braurser/manual/spool.
        /// err: rejects unsafe directories and file creation failures.
        static std::expected<Body, ManualError> create(
            const std::filesystem::path& cache_root, std::size_t max_bytes = kMaxBytes);
        /// in: bytes are the next response chunk.
        /// out: appends bytes without retaining them in memory.
        /// err: closes the spool on size-limit or write failure.
        std::expected<void, ManualError> append(std::string_view bytes);
        /// in: offset is within the response and buffer is writable.
        /// out: reads at most kReadChunk bytes; returns zero at EOF or for an empty buffer.
        /// err: rejects invalid offsets and truncated or unreadable spools.
        std::expected<std::size_t, ManualError> read(std::span<char> buffer, std::size_t offset) const;
        /// out: the number of spooled bytes.
        std::size_t size() const { return size_; }
        /// out: the borrowed descriptor, or -1 for an empty response.
        int native_handle() const { return fd_; }
    private:
        int fd_{-1};
        std::size_t size_{};
        std::size_t limit_{kMaxBytes};
    };
    long status{};
    Body body;
    std::string location;
};

class GitHttp {
public:
    virtual ~GitHttp() = default;
    /// in: url is validated; body is empty for GET and set for POST.
    /// out: bounded single-response transfer, without following redirects.
    virtual std::expected<GitReply, ManualError> request(
        std::string_view url, std::string_view body, bool post) = 0;
    /// in: max_bytes is the remaining aggregate response budget.
    /// out: a single response within both the per-response and aggregate caps.
    /// err: rejects responses exceeding either cap.
    virtual std::expected<GitReply, ManualError> request_bounded(
        std::string_view url, std::string_view body, bool post, std::size_t max_bytes);
};

class CurlGitHttp final : public GitHttp {
public:
    /// in: cache_root is an absolute XDG cache root, or empty for the environment default.
    /// out: configures anonymous response spools beneath the manual cache.
    explicit CurlGitHttp(std::filesystem::path cache_root = {}) : cache_root_(std::move(cache_root)) {}
    /// in: url is HTTPS and body is bounded smart-HTTP request data.
    /// out: a bounded anonymous response spool without following redirects.
    std::expected<GitReply, ManualError> request(
        std::string_view url, std::string_view body, bool post) override;
    /// in: max_bytes is the remaining aggregate response budget.
    /// out: streams into an anonymous spool bounded by both response caps.
    /// err: rejects unsafe connections, oversized transfers and spool failures.
    std::expected<GitReply, ManualError> request_bounded(
        std::string_view url, std::string_view body, bool post, std::size_t max_bytes) override;
private:
    std::filesystem::path cache_root_;
};

class GuardedGitTransport {
public:
    explicit GuardedGitTransport(GitHttp& http) : http_(http) {}
    /// in: url is a validated smart-HTTP git repository.
    /// out: an anonymous reader for the upload-pack advertisement or response.
    /// err: returns RedirectRequired when a different validated host redirects.
    std::expected<GitReply::Body, ManualError> exchange(
        std::string_view url, std::string_view payload = {});
private:
    GitHttp& http_;
};

enum class EntryKind { Script, MakeInstall };

struct EntryPoint {
    std::string path;
    EntryKind kind;
};

struct Preview {
    std::string preview_id;
    std::string host;
    std::string owner_repo;
    std::string commit;
    std::vector<EntryPoint> entry_points;
    std::string selected_script_text;
    std::vector<std::string> referenced_files;
    std::vector<std::string> external_urls;
    bool staging_supported{true};
    std::vector<std::string> scope_options{"user", "system"};
};

/// in: url is validated and cache_root is an absolute private cache directory.
/// out: fetches via guarded smart HTTP into a private bare repository and previews it.
std::expected<Preview, ManualError> fetch_preview(
    GitHttp& http, std::string_view url, const std::filesystem::path& cache_root,
    std::string_view preview_id, std::string_view revision = "HEAD",
    std::optional<EntryKind> selected = std::nullopt);

struct BuildRepository {
    std::string commit;
    std::string pkgbuild;
};

/// in: url identifies a guarded HTTPS repository and cache_root is private.
/// out: fetches HEAD and reads a bounded PKGBUILD from the pinned commit.
std::expected<BuildRepository, ManualError> fetch_build_repository(
    GitHttp& http, std::string_view url, const std::filesystem::path& cache_root,
    std::string_view id);

struct BuildFile {
    std::string path;
    unsigned mode{};
    std::uint64_t size{};
    std::string sha256;
    std::optional<std::string> text;
};

struct BuildTree {
    std::string commit;
    std::string tree;
    std::string srcinfo;
    std::vector<BuildFile> files;
};

/// in: pkgbase is an AUR package base and cache_root is an absolute private cache.
/// out: fetches guarded HEAD and retains a bounded complete build-tree pin.
std::expected<BuildTree, ManualError> fetch_build_tree(
    GitHttp& http, std::string_view pkgbase, const std::filesystem::path& cache_root);

/// in: bare is a local repository and commit is a full lowercase object ID.
/// out: reads every bounded build file without network access and retains the commit.
/// err: build-text-too-large rejects UTF-8 text exceeding 448 KiB.
std::expected<BuildTree, ManualError> read_build_tree(
    const std::filesystem::path& bare, std::string_view commit);

/// in: cache_root is absolute; max_age bounds pin age; keep contains live commit IDs.
/// out: expires old pins and evicts least-recently-used AUR repos above 256 MiB, preserving keep.
/// err: rejects unsafe cache paths and failed filesystem operations.
std::expected<void, ManualError> prune_pins(
    const std::filesystem::path& cache_root, std::chrono::seconds max_age,
    std::span<const std::string> keep);

/// in: repo is an existing bare repository and revision is a branch, tag or HEAD.
/// out: a bounded review preview; multiple entry points require explicit selection.
std::expected<Preview, ManualError> preview_repository(
    const std::filesystem::path& repo, std::string_view revision, const GitUrl& url,
    std::string_view preview_id, std::optional<EntryKind> selected = std::nullopt);

/// in: repo is a bare repository and commit is a full validated object ID.
/// out: writes only validated entries beneath an empty destination directory.
std::expected<void, ManualError> materialize_commit(
    const std::filesystem::path& repo, std::string_view commit,
    const std::filesystem::path& destination);

struct Receipt;

struct UpdateCheck {
    std::optional<Preview> preview;
    std::string entry_point_diff;
};

/// in: receipt records an approved commit and cache preserves the original bare repository.
/// out: a fresh preview and bounded unified entry-point diff when the remote HEAD changed.
std::expected<UpdateCheck, ManualError> check_update(
    GitHttp& http, const Receipt& receipt, const std::filesystem::path& cache_root,
    std::optional<EntryKind> selected = std::nullopt);

enum class Scope { User, System };

inline constexpr std::size_t kMaxReceiptFiles = 4096;
inline constexpr std::size_t kMaxReceiptBytes = 512 * 1024;

struct StagedFile {
    std::string path;
    std::string sha256;
    unsigned mode{};
};

struct StageManifest {
    std::vector<StagedFile> files;
    std::vector<std::string> launchers;
};

/// in: stage is an absolute directory mounted as the selected prefix.
/// out: a bounded validated manifest or a list of refused paths.
std::expected<StageManifest, ManualError> validate_stage(
    const std::filesystem::path& stage, Scope scope);

/// in: stage is an absolute prefix stage; system_root is the absolute tree system-scope names must not shadow.
/// out: a bounded validated manifest or a list of refused paths.
std::expected<StageManifest, ManualError> validate_stage(
    const std::filesystem::path& stage, Scope scope, const std::filesystem::path& system_root);

/// in: source contains the approved materialized tree; stage is an empty private directory.
/// out: a completed isolated installation attempt and bounded output.
std::expected<std::string, ManualError> run_staging(
    core::ProcessRunner& runner, const std::filesystem::path& source,
    const std::filesystem::path& stage, const std::filesystem::path& home,
    EntryKind kind, Scope scope, std::string_view makeflags = {});

struct Receipt {
    std::string id;
    std::string name;
    std::string url;
    std::string host;
    std::string commit;
    std::string entry_point;
    Scope scope{Scope::User};
    std::int64_t installed_at{};
    std::vector<StagedFile> files;
    std::vector<std::pair<std::string, std::string>> backups;
    std::vector<std::string> launcher_ids;
    std::string pkgname;
};

/// in: receipt contains the complete planned installation metadata, files and backups.
/// out: succeeds only when the serialized receipt fits removal-planning limits.
/// err: receipt-limit rejects oversized counts or serialized bytes.
std::expected<void, ManualError> validate_receipt_limits(const Receipt& receipt);

/// in: paths are private absolute roots and receipt identifies an approved stage.
/// out: copies files into the user prefix and saves an atomic private receipt.
std::expected<Receipt, ManualError> install_user(
    core::ProcessRunner& runner, const std::filesystem::path& stage,
    const std::filesystem::path& home, const std::filesystem::path& state,
    Receipt receipt);

/// in: approved new stage and its receipt replace one matching user receipt.
/// out: removes the old installation and installs the new approved files.
std::expected<Receipt, ManualError> upgrade_user(
    core::ProcessRunner& runner, const std::filesystem::path& stage,
    const std::filesystem::path& home, const std::filesystem::path& state,
    const Receipt& previous, Receipt next);

struct RemovalResult {
    std::vector<std::string> modified_files;
};

/// in: receipt is a previously saved user-scope receipt.
/// out: removes matching files, restores backups and deletes the receipt.
std::expected<RemovalResult, ManualError> remove_user(
    core::ProcessRunner& runner, const std::filesystem::path& home,
    const std::filesystem::path& state, const Receipt& receipt);

/// in: url is an untrusted repository URL.
/// out: the system package name derived from the full repository path.
/// err: rejects invalid URLs and names pacman would refuse.
std::expected<std::string, ManualError> system_package_name(std::string_view url);

/// in: receipts are the recorded Git installs; installed holds installed package names.
/// out: true when pkgname is unused or already recorded as a system install of url.
bool system_package_available(std::string_view pkgname, std::string_view url,
                              std::span<const Receipt> receipts, const std::set<std::string>& installed);

/// in: receipt contains validated git metadata, optionally its recorded pkgname, and commit date is UTC.
/// out: fixed script-less PKGBUILD and package name.
std::expected<std::pair<std::string, std::string>, ManualError> render_pkgbuild(
    const Receipt& receipt, std::chrono::sys_seconds commit_date, std::string_view arch);

struct SystemRequest {
    std::vector<std::string> args;
    std::filesystem::path archive;
};

/// in: stage passed validation and receipt holds the approved repository metadata.
/// out: returns the local archive path for a privileged-helper upgrade request.
std::expected<std::filesystem::path, ManualError> build_system_package(
    core::ProcessRunner& runner, const std::filesystem::path& stage,
    const std::filesystem::path& home, const std::filesystem::path& job_dir,
    const Receipt& receipt, std::chrono::sys_seconds commit_date);

/// in: archive is the output of a validated local package build.
/// out: upgrade request for the privileged helper, without executing it.
std::expected<SystemRequest, ManualError> system_upgrade_request(const std::filesystem::path& archive);

/// in: receipt describes a system package installed by the privileged helper.
/// out: saves its private receipt including the validated package name.
std::expected<void, ManualError> record_system_install(
    const std::filesystem::path& state, const Receipt& receipt);

/// in: pkgname is the recorded validated package name.
/// out: removal request for the privileged helper, without executing it.
std::expected<SystemRequest, ManualError> system_remove_request(std::string_view pkgname);

struct LauncherEntry {
    std::string desktop_id;
    std::string name;
    std::string exec_display;
    std::filesystem::path path;
    std::string owner{"outside"};
};

class UnmanagedLauncherScanner {
public:
    /// in: owned holds absolute package and receipt paths; data dirs are absolute.
    /// out: only visible unowned application launchers.
    std::expected<std::vector<LauncherEntry>, ManualError> scan(
        const std::filesystem::path& data_home, const std::vector<std::filesystem::path>& data_dirs,
        const std::set<std::filesystem::path>& owned) const;
    /// in: state is the absolute XDG state root and runner invokes pacman-conf.
    /// out: scans XDG launchers excluding pacman file lists and local receipts.
    /// err: launcher-coverage-incomplete means the combined ownership-entry budget was exhausted.
    std::expected<std::vector<LauncherEntry>, ManualError> scan_system(
        core::ProcessRunner& runner, const std::filesystem::path& state) const;
};

}

#endif
