#ifndef BRAURSER_DAEMON_CATALOG_SOURCES_HPP
#define BRAURSER_DAEMON_CATALOG_SOURCES_HPP

#include <braurser/daemon/catalog/records.hpp>
#include <braurser/daemon/core/http.hpp>
#include <braurser/daemon/core/process.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::catalog {

struct Repositories {
    std::vector<std::string> names;
    std::filesystem::path db_path;
};

class PacmanConfig {
public:
    explicit PacmanConfig(core::ProcessRunner& runner) : runner_(runner) {}
    /// out: configured repositories in order and absolute database path.
    /// err: rejects invalid command output and failures.
    std::expected<Repositories, CatalogError> read() const;
    /// out: configured HoldPkg names, empty when unset.
    /// err: rejects invalid names, invalid command output and failures.
    std::expected<std::vector<std::string>, CatalogError> hold_packages() const;
private:
    core::ProcessRunner& runner_;
};

struct RecordCounts {
    std::size_t packages{};
    std::size_t rejected{};
};

using RepoSink = std::function<void(RepoPackage&&)>;
using AurSink = std::function<void(AurPackage&&)>;
using AppSink = std::function<void(AppMetadata&&)>;

struct SyncResult {
    std::vector<RepoPackage> packages;
    std::size_t rejected{};
};

/// in: desc is one bounded pacman desc record and repo is validated.
/// out: a package or an error for an invalid record.
std::expected<RepoPackage, CatalogError> parse_repo_desc(std::string_view desc, std::string_view repo);

class SyncDbReader {
public:
    /// in: archive and repo are bounded; sink consumes each valid record immediately.
    /// out: emitted and rejected counts; errors may follow emitted records.
    std::expected<RecordCounts, CatalogError> read_bytes(std::span<const std::byte> archive, std::string_view repo, const RepoSink& sink) const;
    /// in: db_path is absolute; sink consumes each valid record immediately.
    /// out: emitted and rejected counts; errors may follow emitted records.
    std::expected<RecordCounts, CatalogError> read(const std::filesystem::path& db_path, std::string_view repo, const RepoSink& sink) const;
    /// in: archive contains a bounded compressed pacman database.
    /// out: valid records and a rejected-record count.
    std::expected<SyncResult, CatalogError> read_bytes(std::span<const std::byte> archive, std::string_view repo) const;
};

struct LocalRecord {
    InstalledPackage package;
    std::vector<std::string> depends;
    std::vector<std::string> optdepends;
    std::vector<std::string> provides;
    std::vector<std::string> validation;
};

class LocalDbReader {
public:
    /// in: db_path points to the absolute pacman database root.
    /// out: bounded installed records; invalid or symbolic records are rejected.
    std::expected<std::vector<LocalRecord>, CatalogError> read(const std::filesystem::path& db_path) const;
    /// in: package_dir is a single validated local database directory name.
    /// out: bounded contents of its files record, without following symlinks.
    std::expected<std::string, CatalogError> files(const std::filesystem::path& db_path, std::string_view package_dir) const;
};

struct QuerySets {
    std::vector<std::string> installed;
    std::vector<std::string> explicit_packages;
    std::vector<std::string> dependencies;
    std::vector<std::string> orphans;
    std::vector<std::string> foreign;
    std::vector<std::string> native;
    std::vector<std::string> upgradable;
    std::vector<InstalledPackage> packages;
};

class OfficialIndex {
public:
    /// in: bounded package name and version; call finish before subsequent lookups.
    /// err: oversized strings or arena throw length_error.
    void add(std::string_view name, std::string_view version);
    /// out: sorted index, keeping the first insertion of each name.
    void finish();
    /// pre: finish succeeded and no later add occurred.
    /// out: version view valid until the next mutation, or null if absent.
    std::optional<std::string_view> version(std::string_view name) const;
    /// pre: finish succeeded.
    /// out: unique package count.
    std::size_t size() const noexcept { return entries_.size(); }
private:
    struct Entry { std::uint32_t offset; std::uint16_t name_length; std::uint16_t version_length; };
    std::string arena_;
    std::vector<Entry> entries_;
};

class LocalQuery {
public:
    /// in: official is finalized in configured repository priority order.
    /// out: installed-package query sets and computed flags.
    QuerySets compute(std::span<const LocalRecord> local, const OfficialIndex& official) const;
};

class AurDump {
public:
    /// in: bytes contain a bounded compressed dump; sink consumes validated records.
    /// out: emitted and rejected counts; errors may follow emitted records.
    std::expected<RecordCounts, CatalogError> parse(std::span<const std::byte> bytes, const AurSink& sink) const;
    /// in: bytes contain the compressed AUR metadata dump.
    /// out: SAX-decoded validated packages, without constructing the full document.
    std::expected<std::vector<AurPackage>, CatalogError> parse(std::span<const std::byte> bytes) const;
};

class AurDumpFetcher {
public:
    explicit AurDumpFetcher(core::HttpClient& http) : http_(http) {}
    /// in: cache_root is absolute; sink consumes validated records immediately.
    /// out: emitted and rejected counts; failed refresh preserves the previous dump.
    /// err: errors may follow emitted records; caller must roll them back before fallback.
    std::expected<RecordCounts, CatalogError> fetch(const std::filesystem::path& cache_root, const AurSink& sink) const;
    /// in: cache_root is the absolute XDG cache root.
    /// out: validated current dump; on failed refresh the previous dump remains.
    std::expected<std::vector<AurPackage>, CatalogError> fetch(const std::filesystem::path& cache_root) const;
private:
    core::HttpClient& http_;
};

class AurRpcClient {
public:
    explicit AurRpcClient(core::HttpClient& http, std::size_t daily_budget = 3000);
    /// in: names are validated package names.
    /// out: records returned by batches of at most 100.
    std::expected<std::vector<AurPackage>, CatalogError> info(std::span<const std::string> names);
private:
    core::HttpClient& http_;
    std::size_t daily_budget_;
    std::size_t used_{};
    std::int64_t day_{};
    std::mutex mutex_;
};

struct CachedIcon {
    std::filesystem::path path;
    std::uint32_t width{};
    std::uint32_t height{};
};

struct AppStreamRecord {
    AppMetadata metadata;
    std::vector<CachedIcon> icons;
};

class AppStreamReader {
public:
    /// in: path is a compressed collection; sink consumes one component at a time.
    /// out: bounded component count; errors may follow emitted records.
    std::expected<std::size_t, CatalogError> read_records(const std::filesystem::path& path, const std::function<void(AppStreamRecord&&)>& sink, std::size_t component_limit = 300000) const;
    /// in: sink consumes discovered application metadata immediately.
    /// out: aggregate component count bounded across all collections.
    std::expected<std::size_t, CatalogError> discover(const AppSink& sink) const;
    /// in: path is a compressed AppStream collection.
    /// out: metadata and all cached icon sizes for supported components.
    std::expected<std::vector<AppStreamRecord>, CatalogError> read_records(const std::filesystem::path& path) const;
    /// in: path is a compressed AppStream collection.
    /// out: bounded application metadata for supported component types.
    std::expected<std::vector<AppMetadata>, CatalogError> read(const std::filesystem::path& path) const;
};

class IconConverter {
public:
    explicit IconConverter(core::ProcessRunner& runner) : runner_(runner) {}
    /// in: input is a regular PNG or JXL of at most 2 MiB; cache_root is absolute.
    /// out: path to a private validated PNG in the icon cache.
    std::expected<std::filesystem::path, CatalogError> convert(const std::filesystem::path& input,
                                                               const std::filesystem::path& cache_root) const;
private:
    core::ProcessRunner& runner_;
};

struct DesktopEntry {
    std::string type;
    std::string name;
    std::string generic_name;
    std::string comment;
    std::string exec;
    std::string try_exec;
    std::string icon;
    bool no_display{};
    bool hidden{};
    std::vector<std::string> categories;
    std::vector<std::string> mime_types;
    std::string startup_wm_class;
};

/// in: text contains at most 64 KiB of valid UTF-8 desktop entry data.
/// out: parsed Desktop Entry group with base-locale or untranslated names.
std::expected<DesktopEntry, CatalogError> parse_desktop_entry(std::string_view text, std::string_view locale = {});

}

#endif
