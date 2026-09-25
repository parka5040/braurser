#include <braurser/daemon/cache/store.hpp>

#include "internal.hpp"

#include <braurser/validate/validate.hpp>

#include <array>
#include <cerrno>
#include <climits>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::cache {
namespace {

void create_schema(const Database& db) {
    constexpr std::array schema{
        "CREATE TABLE sources(id TEXT PRIMARY KEY, kind TEXT NOT NULL, fingerprint TEXT NOT NULL, etag TEXT, last_modified TEXT, checked_at INTEGER NOT NULL)",
        "CREATE TABLE packages(id INTEGER PRIMARY KEY, source_kind TEXT NOT NULL CHECK(source_kind IN ('official','community')), repo TEXT NOT NULL, name TEXT NOT NULL, version TEXT NOT NULL, description TEXT NOT NULL, url TEXT NOT NULL, package_base TEXT NOT NULL, groups_json TEXT NOT NULL, provides_json TEXT NOT NULL, depends_json TEXT NOT NULL, optdepends_json TEXT NOT NULL, conflicts_json TEXT NOT NULL, replaces_json TEXT NOT NULL, download_size INTEGER NOT NULL, install_size INTEGER NOT NULL, build_date INTEGER NOT NULL, repo_rank INTEGER NOT NULL, UNIQUE(source_kind,repo,name))",
        "CREATE TABLE community(package_id INTEGER PRIMARY KEY REFERENCES packages(id), maintainer TEXT, submitter TEXT, votes INTEGER NOT NULL, popularity REAL NOT NULL, out_of_date INTEGER, first_submitted INTEGER NOT NULL, last_modified INTEGER NOT NULL, keywords_json TEXT NOT NULL, upstream_url TEXT)",
        "CREATE TABLE apps(package_id INTEGER PRIMARY KEY REFERENCES packages(id), appstream_id TEXT NOT NULL, display_name TEXT NOT NULL, summary TEXT NOT NULL, description_json TEXT NOT NULL, icon_path TEXT, categories_json TEXT NOT NULL, screenshots_json TEXT NOT NULL, launchable TEXT, homepage TEXT, license TEXT, developer TEXT)",
        "CREATE TABLE installed(name TEXT PRIMARY KEY, version TEXT NOT NULL, reason TEXT NOT NULL CHECK(reason IN ('explicit','dependency')), install_date INTEGER NOT NULL, size INTEGER NOT NULL, native INTEGER NOT NULL CHECK(native IN (0,1)), orphan INTEGER NOT NULL CHECK(orphan IN (0,1)), upgradable_to TEXT)",
        "CREATE TABLE \"groups\"(package_id INTEGER NOT NULL REFERENCES packages(id), name TEXT NOT NULL, PRIMARY KEY(package_id,name))",
        "CREATE VIRTUAL TABLE package_fts USING fts5(name, display_name, description, keywords, content='', tokenize='unicode61 remove_diacritics 2', prefix='2 3')",
        "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)",
        "CREATE TABLE package_extra(package_id INTEGER PRIMARY KEY REFERENCES packages(id), arch TEXT NOT NULL, packager TEXT NOT NULL, licenses_json TEXT NOT NULL, makedepends_json TEXT NOT NULL, checkdepends_json TEXT NOT NULL, comaintainers_json TEXT NOT NULL)",
        "CREATE TABLE installed_extra(name TEXT PRIMARY KEY REFERENCES installed(name), groups_json TEXT NOT NULL)",
        "CREATE INDEX packages_name ON packages(name,source_kind,repo_rank)",
        "CREATE INDEX packages_source_repo ON packages(source_kind,repo)",
        "CREATE INDEX groups_name ON \"groups\"(name)",
        "CREATE INDEX installed_reason ON installed(reason)",
        "PRAGMA user_version=2"
    };
    for (const char* sql : schema) db.execute(sql);
}

void insert_groups(Statement& statement, std::int64_t id, const std::vector<std::string>& groups) {
    for (const auto& group : groups) {
        statement.bind(1, id);
        statement.bind(2, group);
        statement.step();
        statement.reset();
    }
}

}

struct SnapshotBuilder::Impl {
    explicit Impl(std::filesystem::path path) : cache_home(std::move(path)) {}
    std::filesystem::path cache_home;
    std::unique_ptr<internal::Directory> directory;
    std::string temp_name;
    std::map<std::string, std::int64_t> repo_ranks;
    bool failed{};
    std::unique_ptr<Database> db;
    std::unique_ptr<Transaction> transaction;
    std::unique_ptr<Statement> package_stmt;
    std::unique_ptr<Statement> extra_stmt;
    std::unique_ptr<Statement> groups_stmt;
    std::unique_ptr<Statement> community_stmt;
    std::unique_ptr<Statement> apps_stmt;
    std::unique_ptr<Statement> installed_stmt;
    std::unique_ptr<Statement> installed_extra_stmt;
    std::unique_ptr<Statement> sources_stmt;

    std::expected<void, catalog::CatalogError> reject(std::string code, std::string detail) {
        failed = true;
        return std::unexpected(internal::error(std::move(code), std::move(detail)));
    }
    void close() {
        sources_stmt.reset();
        installed_extra_stmt.reset();
        installed_stmt.reset();
        apps_stmt.reset();
        community_stmt.reset();
        groups_stmt.reset();
        extra_stmt.reset();
        package_stmt.reset();
        transaction.reset();
        db.reset();
    }
    void abort() noexcept {
        close();
        if (directory && !temp_name.empty()) unlinkat(directory->fd, temp_name.c_str(), 0);
        temp_name.clear();
        directory.reset();
        failed = false;
        repo_ranks.clear();
    }
    void initialize(const std::vector<std::string>& repo_order) {
        for (std::size_t index = 0; index < repo_order.size(); ++index)
            repo_ranks.emplace(repo_order[index], static_cast<std::int64_t>(index));
        directory = std::make_unique<internal::Directory>(cache_home, true);
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = "catalog.sqlite.new-" + std::to_string(internal::random_id());
            const int fd = openat(directory->fd, candidate.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd >= 0) {
                temp_name = candidate;
                if (fchmod(fd, 0600) != 0) {
                    const int code = errno;
                    ::close(fd);
                    throw std::runtime_error("cannot restrict snapshot file: " + std::to_string(code));
                }
                ::close(fd);
                break;
            }
            if (errno != EEXIST) throw std::runtime_error("cannot create snapshot file");
        }
        if (temp_name.empty()) throw std::runtime_error("snapshot filename collision limit exceeded");
        db = std::make_unique<Database>(directory->path / temp_name, SQLITE_OPEN_READWRITE);
        {
            auto mode = db->prepare("PRAGMA journal_mode=DELETE");
            if (!mode.step() || mode.text(0) != std::optional<std::string>("delete")) {
                throw std::runtime_error("snapshot requires DELETE journal mode");
            }
        }
        db->execute("PRAGMA synchronous=FULL");
        transaction = std::make_unique<Transaction>(*db);
        create_schema(*db);
        auto meta = db->prepare("INSERT INTO meta(key,value) VALUES(?1,?2)");
        meta.bind(1, "generation");
        meta.bind(2, std::to_string(internal::random_id()));
        meta.step();
        meta.reset();
        meta.bind(1, "repo_order");
        meta.bind(2, internal::json_array(repo_order));
        meta.step();
        package_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO packages(source_kind,repo,name,version,description,url,package_base,groups_json,provides_json,depends_json,optdepends_json,conflicts_json,replaces_json,download_size,install_size,build_date,repo_rank) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17)"));
        extra_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO package_extra VALUES(?1,?2,?3,?4,?5,?6,?7)"));
        groups_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT OR IGNORE INTO \"groups\"(package_id,name) VALUES(?1,?2)"));
        community_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO community VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)"));
        apps_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO apps VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)"));
        installed_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO installed VALUES(?1,?2,?3,?4,?5,?6,?7,?8)"));
        installed_extra_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO installed_extra VALUES(?1,?2)"));
        sources_stmt = std::make_unique<Statement>(db->prepare(
            "INSERT INTO sources VALUES(?1,?2,?3,?4,?5,?6)"));
    }
    std::int64_t insert_package(std::string_view kind, std::string_view repo, std::string_view name,
                                std::string_view version, std::string_view description, std::string_view url,
                                std::string_view base, const std::vector<std::string>& groups,
                                const std::vector<std::string>& provides, const std::vector<std::string>& depends,
                                const std::vector<std::string>& optdepends, const std::vector<std::string>& conflicts,
                                const std::vector<std::string>& replaces, std::uint64_t download_size,
                                std::uint64_t install_size, std::int64_t build_date, std::int64_t repo_rank) {
        auto& stmt = *package_stmt;
        stmt.bind(1, kind);
        stmt.bind(2, repo);
        stmt.bind(3, name);
        stmt.bind(4, version);
        stmt.bind(5, description);
        stmt.bind(6, url);
        stmt.bind(7, base);
        stmt.bind(8, internal::json_array(groups));
        stmt.bind(9, internal::json_array(provides));
        stmt.bind(10, internal::json_array(depends));
        stmt.bind(11, internal::json_array(optdepends));
        stmt.bind(12, internal::json_array(conflicts));
        stmt.bind(13, internal::json_array(replaces));
        stmt.bind(14, internal::checked_size(download_size));
        stmt.bind(15, internal::checked_size(install_size));
        stmt.bind(16, build_date);
        stmt.bind(17, repo_rank);
        stmt.step();
        stmt.reset();
        const auto id = db->last_insert_id();
        insert_groups(*groups_stmt, id, groups);
        return id;
    }
    void insert_extra(std::int64_t id, std::string_view arch, std::string_view packager,
                      const std::vector<std::string>& licenses, const std::vector<std::string>& makedepends,
                      const std::vector<std::string>& checkdepends, const std::vector<std::string>& comaintainers) {
        auto& stmt = *extra_stmt;
        stmt.bind(1, id);
        stmt.bind(2, arch);
        stmt.bind(3, packager);
        stmt.bind(4, internal::json_array(licenses));
        stmt.bind(5, internal::json_array(makedepends));
        stmt.bind(6, internal::json_array(checkdepends));
        stmt.bind(7, internal::json_array(comaintainers));
        stmt.step();
        stmt.reset();
    }
};

SnapshotBuilder::SnapshotBuilder(std::filesystem::path cache_home)
    : impl_(std::make_unique<Impl>(std::move(cache_home))) {}
SnapshotBuilder::~SnapshotBuilder() { abort(); }
std::expected<void, catalog::CatalogError> SnapshotBuilder::begin(const std::vector<std::string>& repo_order) {
    if (impl_->db) return std::unexpected(internal::error("state", "snapshot already open"));
    if (repo_order.size() > 256) return std::unexpected(internal::error("invalid-record", "too many repositories"));
    for (const auto& repo : repo_order) {
        if (!br_valid_repo_name(repo.data(), repo.size())) {
            return std::unexpected(internal::error("invalid-record", "invalid repository name"));
        }
    }
    try {
        impl_->initialize(repo_order);
        return {};
    } catch (const std::exception& e) {
        impl_->abort();
        return std::unexpected(internal::error("storage", e.what()));
    }
}
std::expected<void, catalog::CatalogError> SnapshotBuilder::add(const catalog::RepoPackage& package) {
    if (!impl_->db) return std::unexpected(internal::error("state", "begin required"));
    if (!internal::valid(package)) return impl_->reject("invalid-record", "invalid repository package");
    try {
        const auto id = impl_->insert_package("official", package.repo, package.name, package.version,
                                               package.description, package.url, package.base, package.groups,
                                               package.provides, package.depends, package.optdepends,
                                               package.conflicts, package.replaces, package.download_size,
                                               package.install_size, package.build_date,
                                               impl_->repo_ranks.contains(package.repo) ? impl_->repo_ranks.at(package.repo) : 256);
        impl_->insert_extra(id, package.arch, package.packager, package.licenses, package.makedepends,
                            package.checkdepends, {});
        return {};
    } catch (const std::exception& e) {
        return impl_->reject("storage", e.what());
    }
}
std::expected<void, catalog::CatalogError> SnapshotBuilder::add(const catalog::AurPackage& package) {
    if (!impl_->db) return std::unexpected(internal::error("state", "begin required"));
    if (!internal::valid(package)) return impl_->reject("invalid-record", "invalid community package");
    try {
        const auto id = impl_->insert_package("community", "", package.name, package.version,
                                               package.description, package.url, package.package_base,
                                               package.groups, package.provides, package.depends,
                                               package.optdepends, package.conflicts, package.replaces, 0, 0, 0, 257);
        impl_->insert_extra(id, "", "", package.licenses, package.makedepends,
                            package.checkdepends, package.comaintainers);
        auto& stmt = *impl_->community_stmt;
        stmt.bind(1, id);
        internal::bind_optional(stmt, 2, package.maintainer);
        internal::bind_optional(stmt, 3, package.submitter);
        stmt.bind(4, static_cast<std::int64_t>(package.votes));
        stmt.bind(5, package.popularity);
        internal::bind_optional(stmt, 6, package.out_of_date);
        stmt.bind(7, package.first_submitted);
        stmt.bind(8, package.last_modified);
        stmt.bind(9, internal::json_array(package.keywords));
        internal::bind_optional(stmt, 10, package.url.empty() ? std::nullopt : std::optional(package.url));
        stmt.step();
        stmt.reset();
        return {};
    } catch (const std::exception& e) {
        return impl_->reject("storage", e.what());
    }
}
std::expected<void, catalog::CatalogError> SnapshotBuilder::add(const catalog::AppMetadata& app) {
    if (!impl_->db) return std::unexpected(internal::error("state", "begin required"));
    if (!internal::valid(app)) return impl_->reject("invalid-record", "invalid app metadata");
    try {
        auto lookup = impl_->db->prepare("SELECT id FROM packages WHERE name=?1 ORDER BY source_kind='community',repo_rank,id LIMIT 1");
        lookup.bind(1, app.pkgname);
        if (!lookup.step()) return impl_->reject("invalid-record", "app references missing package");
        auto& stmt = *impl_->apps_stmt;
        stmt.bind(1, lookup.integer(0));
        stmt.bind(2, app.appstream_id);
        stmt.bind(3, app.name);
        stmt.bind(4, app.summary);
        stmt.bind(5, internal::json_array(app.description_paragraphs));
        internal::bind_optional(stmt, 6, app.icon_path);
        stmt.bind(7, internal::json_array(app.categories));
        stmt.bind(8, internal::json_screenshots(app.screenshots));
        internal::bind_optional(stmt, 9, app.launchable);
        internal::bind_optional(stmt, 10, app.homepage);
        internal::bind_optional(stmt, 11, app.license);
        internal::bind_optional(stmt, 12, app.developer);
        stmt.step();
        stmt.reset();
        return {};
    } catch (const std::exception& e) {
        return impl_->reject("storage", e.what());
    }
}
std::expected<void, catalog::CatalogError> SnapshotBuilder::add(const catalog::InstalledPackage& package) {
    if (!impl_->db) return std::unexpected(internal::error("state", "begin required"));
    if (!internal::valid(package)) return impl_->reject("invalid-record", "invalid installed package");
    try {
        auto& stmt = *impl_->installed_stmt;
        stmt.bind(1, package.name);
        stmt.bind(2, package.version);
        stmt.bind(3, package.reason == domain::InstallReason::Explicit ? "explicit" : "dependency");
        stmt.bind(4, package.install_date);
        stmt.bind(5, internal::checked_size(package.size));
        stmt.bind(6, static_cast<std::int64_t>(package.native));
        stmt.bind(7, static_cast<std::int64_t>(package.orphan));
        internal::bind_optional(stmt, 8, package.upgradable_to);
        stmt.step();
        stmt.reset();
        auto& extra = *impl_->installed_extra_stmt;
        extra.bind(1, package.name);
        extra.bind(2, internal::json_array(package.groups));
        extra.step();
        extra.reset();
        auto lookup = impl_->db->prepare("SELECT id FROM packages WHERE name=?1");
        lookup.bind(1, package.name);
        while (lookup.step()) insert_groups(*impl_->groups_stmt, lookup.integer(0), package.groups);
        return {};
    } catch (const std::exception& e) {
        return impl_->reject("storage", e.what());
    }
}
std::expected<void, catalog::CatalogError> SnapshotBuilder::add(const catalog::SourceState& source) {
    if (!impl_->db) return std::unexpected(internal::error("state", "begin required"));
    if (!internal::valid(source)) return impl_->reject("invalid-record", "invalid source state");
    try {
        auto& stmt = *impl_->sources_stmt;
        stmt.bind(1, source.id);
        stmt.bind(2, source.kind);
        stmt.bind(3, source.fingerprint);
        internal::bind_optional(stmt, 4, source.etag);
        internal::bind_optional(stmt, 5, source.last_modified);
        stmt.bind(6, source.checked_at);
        stmt.step();
        stmt.reset();
        return {};
    } catch (const std::exception& e) {
        return impl_->reject("storage", e.what());
    }
}
std::expected<void, catalog::CatalogError> SnapshotBuilder::commit() {
    if (!impl_->db) return std::unexpected(internal::error("state", "begin required"));
    if (impl_->failed) {
        impl_->abort();
        return std::unexpected(internal::error("invalid-record", "snapshot contains rejected records"));
    }
    try {
        impl_->db->execute(
            "INSERT INTO package_fts(rowid,name,display_name,description,keywords) "
            "SELECT p.id,p.name,COALESCE(a.display_name,''),p.description,"
            "COALESCE((SELECT group_concat(value,' ') FROM json_each(c.keywords_json)),'') "
            "FROM packages p LEFT JOIN apps a ON a.package_id=p.id LEFT JOIN community c ON c.package_id=p.id");
        impl_->transaction->commit();
        impl_->db->execute("ANALYZE");
        if (!impl_->db->integrity_check()) throw std::runtime_error("snapshot integrity check failed");
        impl_->close();
        const int fd = openat(impl_->directory->fd, impl_->temp_name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot reopen snapshot for sync");
        const int sync_result = fsync(fd);
        const int sync_error = errno;
        close(fd);
        if (sync_result != 0) throw std::runtime_error("snapshot sync failed: " + std::to_string(sync_error));
        struct stat existing {};
        if (fstatat(impl_->directory->fd, "catalog.sqlite", &existing, AT_SYMLINK_NOFOLLOW) == 0) {
            if (!S_ISREG(existing.st_mode) || existing.st_uid != getuid() || (existing.st_mode & 0777) != 0600) {
                throw std::runtime_error("existing snapshot type, owner or mode refused");
            }
        } else if (errno != ENOENT) {
            throw std::runtime_error("cannot inspect existing snapshot");
        }
        if (renameat(impl_->directory->fd, impl_->temp_name.c_str(), impl_->directory->fd, "catalog.sqlite") != 0) {
            throw std::runtime_error("cannot replace snapshot");
        }
        impl_->temp_name.clear();
        if (fsync(impl_->directory->fd) != 0) throw std::runtime_error("cannot sync snapshot directory");
        impl_->directory.reset();
        return {};
    } catch (const std::exception& e) {
        impl_->abort();
        return std::unexpected(internal::error("storage", e.what()));
    }
}
void SnapshotBuilder::abort() noexcept { impl_->abort(); }

}
