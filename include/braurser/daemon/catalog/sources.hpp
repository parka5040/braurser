#ifndef BRAURSER_DAEMON_CATALOG_SOURCES_HPP
#define BRAURSER_DAEMON_CATALOG_SOURCES_HPP

#include <braurser/daemon/catalog/records.hpp>
#include <braurser/daemon/core/http.hpp>
#include <braurser/daemon/core/process.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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
private:
    core::ProcessRunner& runner_;
};

struct SyncResult {
    std::vector<RepoPackage> packages;
    std::size_t rejected{};
};

/// in: desc is one bounded pacman desc record and repo is validated.
/// out: a package or an error for an invalid record.
std::expected<RepoPackage, CatalogError> parse_repo_desc(std::string_view desc, std::string_view repo);

class SyncDbReader {
public:
    /// in: archive contains a bounded compressed pacman database.
    /// out: valid records and a rejected-record count.
    std::expected<SyncResult, CatalogError> read_bytes(std::span<const std::byte> archive, std::string_view repo) const;
    /// in: db_path is the absolute pacman database root.
    /// out: reads the named sync database.
    std::expected<SyncResult, CatalogError> read(const std::filesystem::path& db_path, std::string_view repo) const;
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

class LocalQuery {
public:
    /// in: repos are ordered as returned by pacman-conf.
    /// out: installed-package query sets and computed flags.
    QuerySets compute(std::span<const LocalRecord> local, std::span<const RepoPackage> repos) const;
};

class AurDump {
public:
    /// in: bytes contain the compressed AUR metadata dump.
    /// out: SAX-decoded validated packages, without constructing the full document.
    std::expected<std::vector<AurPackage>, CatalogError> parse(std::span<const std::byte> bytes) const;
};

class AurDumpFetcher {
public:
    explicit AurDumpFetcher(core::HttpClient& http) : http_(http) {}
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
    /// in: path is a compressed AppStream collection.
    /// out: metadata and all cached icon sizes for supported components.
    std::expected<std::vector<AppStreamRecord>, CatalogError> read_records(const std::filesystem::path& path) const;
    /// in: path is a compressed AppStream collection.
    /// out: bounded application metadata for supported component types.
    std::expected<std::vector<AppMetadata>, CatalogError> read(const std::filesystem::path& path) const;
    /// out: metadata from discovered system AppStream collections.
    std::expected<std::vector<AppMetadata>, CatalogError> discover() const;
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
