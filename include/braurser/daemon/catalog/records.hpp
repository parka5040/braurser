#ifndef BRAURSER_DAEMON_CATALOG_RECORDS_HPP
#define BRAURSER_DAEMON_CATALOG_RECORDS_HPP

#include <braurser/domain/types.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace braurser::daemon::catalog {

struct CatalogError {
    std::string code;
    std::string detail;
};

struct RepoPackage {
    std::string repo;
    std::string name;
    std::string base;
    std::string version;
    std::string description;
    std::string url;
    std::string arch;
    std::string packager;
    std::uint64_t download_size{};
    std::uint64_t install_size{};
    std::int64_t build_date{};
    std::vector<std::string> licenses;
    std::vector<std::string> groups;
    std::vector<std::string> depends;
    std::vector<std::string> optdepends;
    std::vector<std::string> makedepends;
    std::vector<std::string> checkdepends;
    std::vector<std::string> provides;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
};

struct AurPackage {
    std::string name;
    std::string package_base;
    std::string version;
    std::string description;
    std::string url;
    std::optional<std::string> maintainer;
    std::optional<std::string> submitter;
    std::uint32_t votes{};
    double popularity{};
    std::optional<std::int64_t> out_of_date;
    std::int64_t first_submitted{};
    std::int64_t last_modified{};
    std::vector<std::string> licenses;
    std::vector<std::string> keywords;
    std::vector<std::string> groups;
    std::vector<std::string> depends;
    std::vector<std::string> makedepends;
    std::vector<std::string> checkdepends;
    std::vector<std::string> optdepends;
    std::vector<std::string> provides;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
    std::vector<std::string> comaintainers;
};

struct Screenshot {
    std::string url;
    std::string caption;
};

struct AppMetadata {
    std::string appstream_id;
    std::string pkgname;
    std::string name;
    std::string summary;
    std::vector<std::string> description_paragraphs;
    std::vector<std::string> categories;
    std::optional<std::string> icon_path;
    std::optional<std::string> launchable;
    std::optional<std::string> homepage;
    std::optional<std::string> license;
    std::optional<std::string> developer;
    std::vector<Screenshot> screenshots;
};

struct InstalledPackage {
    std::string name;
    std::string version;
    domain::InstallReason reason{domain::InstallReason::Explicit};
    std::int64_t install_date{};
    std::uint64_t size{};
    bool native{};
    bool orphan{};
    std::optional<std::string> upgradable_to;
    std::vector<std::string> groups;
};

struct SourceState {
    std::string id;
    std::string kind;
    std::string fingerprint;
    std::optional<std::string> etag;
    std::optional<std::string> last_modified;
    std::int64_t checked_at{};
};

class CatalogWriter {
public:
    virtual ~CatalogWriter() = default;

    /// in: repo_order lists configured repositories in pacman-conf order.
    /// out: an open snapshot ready for records, or an error.
    virtual std::expected<void, CatalogError> begin(const std::vector<std::string>& repo_order) = 0;

    /// pre: begin succeeded.
    /// in: one validated repository package.
    /// err: storage failure.
    virtual std::expected<void, CatalogError> add(const RepoPackage& package) = 0;

    /// pre: begin succeeded.
    /// in: one validated community package.
    /// err: storage failure.
    virtual std::expected<void, CatalogError> add(const AurPackage& package) = 0;

    /// pre: begin succeeded; the named package was added earlier.
    /// in: application metadata joined by pkgname.
    /// err: storage failure.
    virtual std::expected<void, CatalogError> add(const AppMetadata& app) = 0;

    /// pre: begin succeeded.
    /// in: one installed package with its computed query flags.
    /// err: storage failure.
    virtual std::expected<void, CatalogError> add(const InstalledPackage& package) = 0;

    /// pre: begin succeeded.
    /// in: refresh bookkeeping for one source.
    /// err: storage failure.
    virtual std::expected<void, CatalogError> add(const SourceState& source) = 0;

    /// pre: begin succeeded.
    /// out: the new snapshot atomically replaces the previous one.
    /// err: validation or storage failure; the previous snapshot stays in place.
    virtual std::expected<void, CatalogError> commit() = 0;

    /// out: discards an uncommitted snapshot.
    virtual void abort() noexcept = 0;
};

enum class SortOrder { Relevance, Name };

struct SearchFilters {
    bool installed{};
    bool not_installed{};
    bool explicit_only{};
    bool dependency_only{};
    bool orphans{};
    bool foreign{};
    bool native{};
    bool upgradable{};
    bool aur_only{};
    bool repo_only{};
    bool apps_only{};
    std::vector<std::string> repos;
    std::vector<std::string> groups;
    std::optional<std::string> category;
};

struct SearchQuery {
    std::string text;
    SearchFilters filters;
    SortOrder sort{SortOrder::Relevance};
    std::optional<std::string> cursor;
    std::uint32_t limit{50};
};

struct SearchHit {
    std::int64_t id{};
    domain::SourceKind source{domain::SourceKind::Official};
    std::optional<std::string> repo;
    std::string name;
    std::string version;
    std::string description;
    std::optional<std::string> app_name;
    std::optional<std::string> app_summary;
    std::optional<std::string> icon_path;
    bool installed{};
    std::optional<std::string> installed_version;
    std::optional<std::string> upgradable_to;
    std::vector<std::string> also_in;
    bool shadowed{};
};

struct SearchPage {
    std::vector<SearchHit> hits;
    std::uint64_t total{};
    std::optional<std::string> next_cursor;
    std::uint64_t generation{};
};

struct PackageDetails {
    SearchHit summary;
    std::optional<RepoPackage> repo_record;
    std::optional<AurPackage> community_record;
    std::optional<AppMetadata> app;
    std::optional<InstalledPackage> installed;
    std::vector<std::string> also_in;
    bool shadowed{};
};

}

#endif
