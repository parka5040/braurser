#ifndef BRAURSER_DAEMON_CACHE_STORE_HPP
#define BRAURSER_DAEMON_CACHE_STORE_HPP

#include <braurser/daemon/catalog/records.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>

namespace braurser::daemon::cache {

class SnapshotBuilder final : public catalog::CatalogWriter {
public:
    /// in: cache_home is the absolute XDG cache directory.
    explicit SnapshotBuilder(std::filesystem::path cache_home);
    ~SnapshotBuilder() override;
    SnapshotBuilder(const SnapshotBuilder&) = delete;
    SnapshotBuilder& operator=(const SnapshotBuilder&) = delete;

    /// in: repo_order lists repositories in priority order.
    /// out: an isolated writable snapshot.
    std::expected<void, catalog::CatalogError> begin(const std::vector<std::string>& repo_order) override;
    /// in: package is a validated repository record.
    std::expected<void, catalog::CatalogError> add(const catalog::RepoPackage& package) override;
    /// in: package is a validated AUR record.
    std::expected<void, catalog::CatalogError> add(const catalog::AurPackage& package) override;
    /// in: app refers to an already-added package.
    std::expected<void, catalog::CatalogError> add(const catalog::AppMetadata& app) override;
    /// in: package is a validated installed record.
    std::expected<void, catalog::CatalogError> add(const catalog::InstalledPackage& package) override;
    /// in: source contains validated refresh state.
    std::expected<void, catalog::CatalogError> add(const catalog::SourceState& source) override;
    /// pre: begin succeeded; savepoints may not be nested.
    /// out: marks a rollback boundary within the current snapshot.
    std::expected<void, catalog::CatalogError> savepoint();
    /// pre: a savepoint is active.
    /// out: keeps records since the savepoint and removes the boundary.
    std::expected<void, catalog::CatalogError> release_savepoint();
    /// pre: a savepoint is active and no storage failure occurred.
    /// out: removes records since the boundary, leaving it active.
    std::expected<void, catalog::CatalogError> rollback_to_savepoint();
    /// out: validates and atomically replaces the active snapshot.
    std::expected<void, catalog::CatalogError> commit() override;
    /// out: discards the uncommitted snapshot.
    void abort() noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Store {
public:
    /// in: cache_home is the absolute XDG cache directory.
    explicit Store(std::filesystem::path cache_home);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// in: query contains bounded text, filters and a pagination cursor.
    /// out: matching packages with a snapshot-bound cursor.
    /// err: returns invalid-query, stale-cursor or storage errors.
    std::expected<catalog::SearchPage, catalog::CatalogError> search(const catalog::SearchQuery& query);
    /// in: id is a positive package identifier.
    /// out: full package details, or null when the id is absent.
    std::expected<std::optional<catalog::PackageDetails>, catalog::CatalogError> package(std::int64_t id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
