#include <braurser/daemon/cache/database.hpp>
#include <braurser/daemon/cache/store.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using braurser::daemon::cache::Database;
using braurser::daemon::cache::SnapshotBuilder;
using braurser::daemon::cache::Store;
namespace catalog = braurser::daemon::catalog;

class Directory {
public:
    Directory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "braurser-cache-test-XXXXXX").string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        char* result = mkdtemp(bytes.data());
        if (!result) throw std::runtime_error("cannot create temporary test directory");
        path = result;
    }
    ~Directory() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

catalog::RepoPackage repo(std::string name, std::string repository = "core") {
    catalog::RepoPackage value;
    value.name = std::move(name);
    value.repo = std::move(repository);
    value.base = value.name;
    value.version = "1.2-3";
    value.description = "A text editor";
    value.url = "https://example.invalid/example";
    value.groups = {"editors"};
    value.licenses = {"MIT"};
    value.depends = {"glibc"};
    value.makedepends = {"cmake"};
    value.arch = "x86_64";
    value.packager = "Example";
    value.install_size = 42;
    return value;
}

void populate(SnapshotBuilder& writer) {
    const auto begun = writer.begin({"core", "extra"});
    ASSERT_TRUE(begun.has_value()) << begun.error().detail;
    auto nano = repo("nano");
    ASSERT_TRUE(writer.add(nano).has_value());
    auto abc = repo("abc", "extra");
    abc.description = "ABC search needle";
    ASSERT_TRUE(writer.add(abc).has_value());
    auto unavailable = repo("zzz-uninstalled", "testing");
    unavailable.description = "ABC helper";
    unavailable.groups.clear();
    ASSERT_TRUE(writer.add(unavailable).has_value());
    auto aur = catalog::AurPackage{};
    aur.name = "abc";
    aur.package_base = "abc";
    aur.version = "2";
    aur.description = "community needle";
    aur.keywords = {"uniqueaur"};
    ASSERT_TRUE(writer.add(aur).has_value());
    aur.name = "yay-helper";
    aur.keywords = {"helper"};
    aur.package_base = aur.name;
    aur.description = "helper";
    ASSERT_TRUE(writer.add(aur).has_value());
    catalog::AppMetadata app;
    app.pkgname = "nano";
    app.appstream_id = "org.example.nano";
    app.name = "Nano Editor";
    app.summary = "Editor";
    app.description_paragraphs = {"first", "second"};
    app.categories = {"Utility", "TextEditor"};
    app.screenshots = {{"https://example.invalid/one", "Caption"}};
    ASSERT_TRUE(writer.add(app).has_value());
    catalog::InstalledPackage installed;
    installed.name = "abc";
    installed.version = "1.0";
    installed.upgradable_to = "1.2";
    installed.native = true;
    installed.groups = {"local-group"};
    ASSERT_TRUE(writer.add(installed).has_value());
    installed.name = "nano";
    installed.reason = braurser::domain::InstallReason::Dependency;
    installed.orphan = true;
    installed.upgradable_to.reset();
    ASSERT_TRUE(writer.add(installed).has_value());
    installed.name = "yay-helper";
    installed.native = false;
    installed.orphan = false;
    ASSERT_TRUE(writer.add(installed).has_value());
    catalog::SourceState source{"core", "official", "fingerprint", {}, {}, 123};
    ASSERT_TRUE(writer.add(source).has_value());
    ASSERT_TRUE(writer.commit().has_value());
}

std::vector<std::string> names(Store& store, catalog::SearchFilters filters) {
    catalog::SearchQuery query;
    query.filters = std::move(filters);
    const auto result = store.search(query);
    if (!result) throw std::runtime_error(result.error().detail);
    std::vector<std::string> result_names;
    for (const auto& hit : result->hits) result_names.push_back(hit.name);
    return result_names;
}

TEST(FTS5, SelfCheckAtOpen) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    const auto begun = writer.begin({});
    ASSERT_TRUE(begun.has_value()) << begun.error().detail;
    ASSERT_TRUE(writer.commit().has_value());
    Database db(directory.path / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
    EXPECT_NO_THROW(braurser::daemon::cache::Fts5SelfCheck::run(db));
    const auto deny = [](void*, int action, const char*, const char*, const char*, const char*) {
        return action == SQLITE_CREATE_VTABLE ? SQLITE_DENY : SQLITE_OK;
    };
    ASSERT_EQ(sqlite3_set_authorizer(db.handle(), deny, nullptr), SQLITE_OK);
    try {
        braurser::daemon::cache::Fts5SelfCheck::run(db);
        FAIL() << "FTS5 probe should fail when virtual tables are unavailable";
    } catch (const std::runtime_error& failure) {
        EXPECT_NE(std::string(failure.what()).find("FTS5 self-check failed"), std::string::npos);
    }
    ASSERT_EQ(sqlite3_set_authorizer(db.handle(), nullptr, nullptr), SQLITE_OK);
}

TEST(Cache, SchemaAndPrivateModes) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    populate(writer);
    const auto file = directory.path / "braurser/catalog.sqlite";
    struct stat info {};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    ASSERT_EQ(stat((directory.path / "braurser").c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
    Database db(file, SQLITE_OPEN_READONLY);
    auto version = db.prepare("PRAGMA user_version");
    ASSERT_TRUE(version.step());
    EXPECT_EQ(version.integer(0), 2);
    EXPECT_TRUE(db.integrity_check());
    auto foreign_keys = db.prepare("PRAGMA foreign_keys");
    ASSERT_TRUE(foreign_keys.step());
    EXPECT_EQ(foreign_keys.integer(0), 1);
    auto trusted_schema = db.prepare("PRAGMA trusted_schema");
    ASSERT_TRUE(trusted_schema.step());
    EXPECT_EQ(trusted_schema.integer(0), 0);
    auto timeout = db.prepare("PRAGMA busy_timeout");
    ASSERT_TRUE(timeout.step());
    EXPECT_EQ(timeout.integer(0), 2000);
    int defensive = 0;
    ASSERT_EQ(sqlite3_db_config(db.handle(), SQLITE_DBCONFIG_DEFENSIVE, -1, &defensive), SQLITE_OK);
    EXPECT_EQ(defensive, 1);
    for (const char* table : {"sources", "packages", "community", "apps", "installed", "groups", "package_fts", "meta"}) {
        auto lookup = db.prepare("SELECT count(*) FROM sqlite_master WHERE name=?1");
        lookup.bind(1, table);
        ASSERT_TRUE(lookup.step());
        EXPECT_EQ(lookup.integer(0), 1) << table;
    }
    EXPECT_FALSE(std::filesystem::exists(file.string() + "-wal"));
}

TEST(Cache, ShadowedOfficialRepositoriesRemainAddressable) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"core", "extra"}));
    auto preferred = repo("shared", "core");
    preferred.version = "2";
    auto shadowed = repo("shared", "extra");
    shadowed.version = "1";
    ASSERT_TRUE(writer.add(preferred));
    ASSERT_TRUE(writer.add(shadowed));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    catalog::SearchQuery query;
    query.text = "shared";
    auto default_page = store.search(query);
    ASSERT_TRUE(default_page) << default_page.error().detail;
    ASSERT_EQ(default_page->hits.size(), 1U);
    EXPECT_EQ(default_page->hits.front().repo, "core");
    EXPECT_EQ(default_page->hits.front().also_in, (std::vector<std::string>{"extra"}));
    EXPECT_FALSE(default_page->hits.front().shadowed);
    query.filters.repos = {"extra"};
    auto filtered = store.search(query);
    ASSERT_TRUE(filtered) << filtered.error().detail;
    ASSERT_EQ(filtered->hits.size(), 1U);
    EXPECT_EQ(filtered->hits.front().repo, "extra");
    EXPECT_EQ(filtered->hits.front().also_in, (std::vector<std::string>{"core"}));
    EXPECT_TRUE(filtered->hits.front().shadowed);
    auto detail = store.package(filtered->hits.front().id);
    ASSERT_TRUE(detail && detail->has_value());
    EXPECT_EQ((*detail)->also_in, (std::vector<std::string>{"core"}));
    EXPECT_TRUE((*detail)->shadowed);
}

TEST(Cache, AtomicSwapPreservesOldReaderAndDetectsGeneration) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    const auto begun = writer.begin({});
    ASSERT_TRUE(begun.has_value()) << begun.error().detail;
    ASSERT_TRUE(writer.add(repo("first")).has_value());
    ASSERT_TRUE(writer.commit().has_value());
    Database old(directory.path / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
    Store store(directory.path);
    catalog::SearchQuery query;
    query.limit = 1;
    ASSERT_TRUE(store.search(query).has_value());
    const auto old_generation = store.search(query)->generation;
    const auto second_begin = writer.begin({});
    ASSERT_TRUE(second_begin.has_value()) << second_begin.error().detail;
    ASSERT_TRUE(writer.add(repo("second")).has_value());
    ASSERT_TRUE(writer.commit().has_value());
    auto old_row = old.prepare("SELECT name FROM packages");
    ASSERT_TRUE(old_row.step());
    EXPECT_EQ(old_row.text(0), "first");
    auto fresh = store.search(query);
    ASSERT_TRUE(fresh.has_value()) << fresh.error().detail;
    ASSERT_EQ(fresh->hits.size(), 1);
    EXPECT_EQ(fresh->hits[0].name, "second");
    EXPECT_NE(fresh->generation, old_generation);
}

TEST(Cache, BadIntegrityOrDestinationDoesNotReplaceSnapshot) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    const auto begun = writer.begin({});
    ASSERT_TRUE(begun.has_value()) << begun.error().detail;
    ASSERT_TRUE(writer.add(repo("kept")).has_value());
    ASSERT_TRUE(writer.commit().has_value());
    const auto file = directory.path / "braurser/catalog.sqlite";
    Database old(file, SQLITE_OPEN_READONLY);
    const auto second_begin = writer.begin({});
    ASSERT_TRUE(second_begin.has_value()) << second_begin.error().detail;
    ASSERT_TRUE(writer.add(repo("discarded")).has_value());
    ASSERT_EQ(chmod(file.c_str(), 0644), 0);
    const auto failure = writer.commit();
    ASSERT_FALSE(failure.has_value());
    EXPECT_EQ(failure.error().code, "storage");
    ASSERT_EQ(chmod(file.c_str(), 0600), 0);
    Store reader(directory.path);
    EXPECT_EQ(names(reader, {}), std::vector<std::string>{"kept"});
    const auto retry = writer.begin({});
    ASSERT_TRUE(retry.has_value()) << retry.error().detail;
    auto rejected = repo("valid");
    rejected.name = "invalid/name";
    EXPECT_EQ(writer.add(rejected).error().code, "invalid-record");
    ASSERT_TRUE(writer.commit());
    EXPECT_TRUE(names(reader, {}).empty());
    auto original = old.prepare("SELECT name FROM packages");
    ASSERT_TRUE(original.step());
    EXPECT_EQ(original.text(0), "kept");
    const auto corrupt = directory.path / "corrupt.sqlite";
    {
        const int fd = open(corrupt.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        ASSERT_GE(fd, 0);
        constexpr std::string_view junk = "not a sqlite database";
        ASSERT_EQ(write(fd, junk.data(), junk.size()), static_cast<ssize_t>(junk.size()));
        ASSERT_EQ(close(fd), 0);
    }
    try {
        Database bad(corrupt, SQLITE_OPEN_READONLY);
        EXPECT_FALSE(bad.integrity_check());
    } catch (const std::runtime_error&) {
        SUCCEED();
    }
    const auto truncated = directory.path / "truncated.sqlite";
    std::filesystem::copy_file(file, truncated);
    Database damaged(truncated, SQLITE_OPEN_READONLY);
    ASSERT_EQ(truncate(truncated.c_str(), 100), 0);
    sqlite3_db_release_memory(damaged.handle());
    try {
        EXPECT_FALSE(damaged.integrity_check());
    } catch (const std::runtime_error&) {
        SUCCEED();
    }
}

TEST(Cache, DetailRoundTrip) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    populate(writer);
    Store store(directory.path);
    catalog::SearchQuery query;
    auto page = store.search(query);
    ASSERT_TRUE(page.has_value()) << page.error().detail;
    ASSERT_EQ(page->total, 5);
    for (const auto& hit : page->hits) {
        auto detail = store.package(hit.id);
        ASSERT_TRUE(detail.has_value()) << detail.error().detail;
        ASSERT_TRUE(detail->has_value());
        EXPECT_EQ(detail->value().summary.id, hit.id);
        if (hit.name == "nano") {
            ASSERT_TRUE(detail->value().app.has_value());
            EXPECT_EQ(detail->value().app->screenshots.at(0).caption, "Caption");
            EXPECT_EQ(detail->value().repo_record->makedepends.at(0), "cmake");
            ASSERT_TRUE(detail->value().installed.has_value());
            EXPECT_EQ(detail->value().installed->groups, (std::vector<std::string>{"local-group"}));
        }
    }
    EXPECT_FALSE(store.package(99999)->has_value());
    EXPECT_EQ(store.package(0).error().code, "invalid-query");
}

TEST(Search, FilterDimensionsAndCombinations) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    populate(writer);
    Store store(directory.path);
    catalog::SearchFilters filters;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "abc", "nano", "yay-helper", "zzz-uninstalled"}));
    filters.installed = true;
    EXPECT_EQ(names(store, filters).size(), 4);
    filters.not_installed = true;
    EXPECT_TRUE(names(store, filters).empty());
    filters = {};
    filters.not_installed = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"zzz-uninstalled"}));
    filters = {};
    filters.explicit_only = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "abc"}));
    filters = {};
    filters.dependency_only = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"nano", "yay-helper"}));
    filters = {};
    filters.orphans = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"nano"}));
    filters = {};
    filters.foreign = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"yay-helper"}));
    filters = {};
    filters.native = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "abc", "nano"}));
    filters = {};
    filters.upgradable = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "abc"}));
    filters = {};
    filters.aur_only = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "yay-helper"}));
    filters = {};
    filters.repo_only = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "nano", "zzz-uninstalled"}));
    filters = {};
    filters.apps_only = true;
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"nano"}));
    filters = {};
    filters.repos = {"extra", "core"};
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "nano"}));
    filters = {};
    filters.groups = {"editors", "missing"};
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "nano"}));
    filters = {};
    filters.groups = {"local-group"};
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"abc", "abc", "nano", "yay-helper"}));
    filters = {};
    filters.category = "TextEditor";
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"nano"}));
    filters.native = true;
    filters.repo_only = true;
    filters.groups = {"editors"};
    EXPECT_EQ(names(store, filters), (std::vector<std::string>{"nano"}));
    filters.category = "Developer";
    EXPECT_TRUE(names(store, filters).empty());
    filters = {};
    filters.aur_only = true;
    filters.repo_only = true;
    EXPECT_TRUE(names(store, filters).empty());
}

TEST(Search, FtsQuotesOperatorsAndUnicodeWhitespace) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    populate(writer);
    Store store(directory.path);
    catalog::SearchQuery query;
    for (const std::string text : {"\"", "NEAR", "name:nano", "^", "*", "(nano)", "nano\" OR name:abc", "NEAR(nano abc)", "\"\"\"", "nano\xc2\xa0" "editor"}) {
        query.text = text;
        const auto result = store.search(query);
        ASSERT_TRUE(result.has_value()) << "FTS text: " << text << "; error: " << result.error().detail;
    }
    query.text = "nano\xc2\xa0" "editor";
    auto match = store.search(query);
    ASSERT_TRUE(match.has_value());
    ASSERT_EQ(match->hits.size(), 1);
    query.text = "nano";
    match = store.search(query);
    ASSERT_TRUE(match.has_value());
    ASSERT_EQ(match->hits.size(), 1);
    EXPECT_EQ(match->hits[0].name, "nano");
    query.text = "uniqueaur";
    match = store.search(query);
    ASSERT_TRUE(match.has_value());
    ASSERT_EQ(match->hits.size(), 1);
    EXPECT_EQ(match->hits[0].source, braurser::domain::SourceKind::Community);
    query.text = std::string("nano\0abc", 8);
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.text = std::string("\xc0\xaf", 2);
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.text = std::string(257, 'a');
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.text = "a a a a a a a a a a a a a a a a a";
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.text.clear();
    query.filters.category = std::string("Utility\0Other", 13);
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.filters.category.reset();
    query.filters.repos.assign(65, "core");
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.filters.repos.clear();
    query.filters.groups = {std::string(257, 'x')};
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
}

TEST(Search, StableSortPaginationAndCursorStaleness) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    populate(writer);
    Store store(directory.path);
    catalog::SearchQuery query;
    query.sort = catalog::SortOrder::Name;
    query.limit = 1;
    std::vector<catalog::SearchHit> hits;
    std::optional<std::string> cursor;
    for (unsigned i = 0; i < 5; ++i) {
        query.cursor = cursor;
        auto page = store.search(query);
        ASSERT_TRUE(page.has_value()) << page.error().detail;
        EXPECT_EQ(page->total, 5);
        ASSERT_EQ(page->hits.size(), 1);
        hits.push_back(page->hits.front());
        cursor = page->next_cursor;
    }
    ASSERT_FALSE(cursor.has_value());
    EXPECT_EQ(hits[0].name, "abc");
    EXPECT_EQ(hits[0].source, braurser::domain::SourceKind::Official);
    EXPECT_EQ(hits[1].source, braurser::domain::SourceKind::Community);
    EXPECT_LT(hits[0].id, hits[1].id);
    query.cursor = "not-a-cursor";
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.cursor.reset();
    query.text = "abc";
    query.sort = catalog::SortOrder::Relevance;
    auto exact = store.search(query);
    ASSERT_TRUE(exact.has_value()) << exact.error().detail;
    EXPECT_EQ(exact->total, 3);
    ASSERT_EQ(exact->hits.size(), 1);
    EXPECT_EQ(exact->hits[0].name, "abc");
    ASSERT_TRUE(exact->next_cursor.has_value());
    auto saved = *exact->next_cursor;
    const auto begun = writer.begin({});
    ASSERT_TRUE(begun.has_value()) << begun.error().detail;
    ASSERT_TRUE(writer.add(repo("new-one")).has_value());
    ASSERT_TRUE(writer.commit().has_value());
    query.cursor = saved;
    auto stale = store.search(query);
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().code, "stale-cursor");
    query.cursor.reset();
    query.limit = 0;
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
    query.limit = 201;
    EXPECT_EQ(store.search(query).error().code, "invalid-query");
}

TEST(Cache, InvalidRecordsAndMissingAppDoNotPoisonSnapshot) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"core"}));
    ASSERT_TRUE(writer.add(repo("kept")));
    auto invalid_repo = repo("bad/name");
    EXPECT_EQ(writer.add(invalid_repo).error().code, "invalid-record");
    catalog::AurPackage invalid_aur;
    EXPECT_EQ(writer.add(invalid_aur).error().code, "invalid-record");
    catalog::InstalledPackage invalid_installed;
    EXPECT_EQ(writer.add(invalid_installed).error().code, "invalid-record");
    catalog::SourceState invalid_source;
    EXPECT_EQ(writer.add(invalid_source).error().code, "invalid-record");
    catalog::AppMetadata app;
    EXPECT_EQ(writer.add(app).error().code, "invalid-record");
    app.pkgname = "missing";
    app.appstream_id = "org.example.missing";
    EXPECT_EQ(writer.add(app).error().code, "invalid-record");
    app.pkgname = "kept";
    ASSERT_TRUE(writer.add(app));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    EXPECT_EQ(names(store, {}), (std::vector<std::string>{"kept"}));
    Database db(directory.path / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
    auto rows = db.prepare("SELECT count(*) FROM apps");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.integer(0), 1);
}

TEST(Cache, StorageFailureStillPoisonsSnapshot) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"core"}));
    ASSERT_TRUE(writer.add(repo("kept")));
    ASSERT_TRUE(writer.commit());
    ASSERT_TRUE(writer.begin({"core"}));
    ASSERT_TRUE(writer.add(repo("duplicate")));
    EXPECT_EQ(writer.add(repo("duplicate")).error().code, "storage");
    EXPECT_EQ(writer.commit().error().code, "storage");
    Store store(directory.path);
    EXPECT_EQ(names(store, {}), (std::vector<std::string>{"kept"}));
}

TEST(Cache, SavepointRollsBackOnlyPartialSourceAndCanBeReused) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    EXPECT_FALSE(writer.savepoint());
    ASSERT_TRUE(writer.begin({"core"}));
    ASSERT_TRUE(writer.add(repo("official")));
    ASSERT_TRUE(writer.savepoint());
    catalog::AurPackage community;
    community.name = community.package_base = "partial";
    community.version = "1";
    ASSERT_TRUE(writer.add(community));
    ASSERT_TRUE(writer.rollback_to_savepoint());
    community.name = community.package_base = "cached";
    ASSERT_TRUE(writer.add(community));
    ASSERT_TRUE(writer.release_savepoint());
    ASSERT_TRUE(writer.savepoint());
    ASSERT_TRUE(writer.add(repo("discarded")));
    ASSERT_TRUE(writer.rollback_to_savepoint());
    ASSERT_TRUE(writer.release_savepoint());
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    EXPECT_EQ(names(store, {}), (std::vector<std::string>{"cached", "official"}));
}

TEST(Cache, ReadConnectionsUseBoundedSharedPageCacheAndHitsCarrySizes) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"core"}));
    auto package = repo("sized");
    package.download_size = 123;
    ASSERT_TRUE(writer.add(package));
    catalog::InstalledPackage installed;
    installed.name = "sized";
    installed.version = "1";
    installed.size = 456;
    ASSERT_TRUE(writer.add(installed));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    auto page = store.search({});
    ASSERT_TRUE(page);
    ASSERT_EQ(page->hits.size(), 1);
    EXPECT_EQ(page->hits.front().download_size, 123);
    EXPECT_EQ(page->hits.front().installed_size, 456);
    Database db(directory.path / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
    for (const auto& [sql, value] : std::array<std::pair<const char*, std::int64_t>, 3>{{
        {"PRAGMA cache_size", -2048}, {"PRAGMA mmap_size", 67108864}, {"PRAGMA temp_store", 1}}}) {
        auto pragma = db.prepare(sql);
        ASSERT_TRUE(pragma.step());
        EXPECT_EQ(pragma.integer(0), value) << sql;
    }
}

TEST(Cache, LargeProvidesAndOneMebibyteBoundary) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"core"}));
    auto large = repo("texlive-latexextra");
    for (int i = 0; i < 5000; ++i) large.provides.push_back("texlive-feature-" + std::to_string(i));
    ASSERT_TRUE(writer.add(large));
    auto boundary = repo("boundary");
    boundary.provides.assign(128, std::string(8192, 'x'));
    ASSERT_TRUE(writer.add(boundary));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    auto page = store.search({});
    ASSERT_TRUE(page) << page.error().detail;
    ASSERT_EQ(page->total, 2);
    for (const auto& hit : page->hits) {
        auto details = store.package(hit.id);
        ASSERT_TRUE(details) << details.error().detail;
        ASSERT_TRUE(details->has_value());
        EXPECT_EQ(details->value().repo_record->provides.size(), hit.name == "boundary" ? 128 : 5000);
    }
    ASSERT_TRUE(writer.begin({"core"}));
    boundary.provides.push_back("x");
    auto rejected = writer.add(boundary);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "invalid-record");
    ASSERT_TRUE(writer.commit());
    EXPECT_EQ(store.search({})->total, 0);
}

TEST(Search, PacmanRepoPriorityAndExplicitShadowedRepo) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"system", "world", "galaxy", "extra"}));
    auto later = repo("firefox", "extra");
    later.version = "200";
    ASSERT_TRUE(writer.add(later));
    auto first = repo("firefox", "galaxy");
    first.version = "100";
    ASSERT_TRUE(writer.add(first));
    ASSERT_TRUE(writer.add(repo("firefox-debugadapter", "extra")));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    catalog::SearchQuery unfiltered_query;
    unfiltered_query.text = "firefox";
    auto unfiltered = store.search(unfiltered_query);
    ASSERT_TRUE(unfiltered) << unfiltered.error().detail;
    ASSERT_EQ(unfiltered->total, 2);
    EXPECT_EQ(unfiltered->hits.front().repo, "galaxy");
    EXPECT_EQ(unfiltered->hits.front().version, "100");
    auto details = store.package(unfiltered->hits.front().id);
    ASSERT_TRUE(details);
    ASSERT_TRUE(details->has_value());
    EXPECT_EQ(details->value().repo_record->repo, "galaxy");
    catalog::SearchQuery query;
    query.filters.repos = {"extra"};
    query.text = "firefox";
    auto filtered = store.search(query);
    ASSERT_TRUE(filtered) << filtered.error().detail;
    ASSERT_EQ(filtered->total, 2);
    EXPECT_EQ(filtered->hits.front().repo, "extra");
    EXPECT_EQ(filtered->hits.front().version, "200");
    query.filters.repos = {"galaxy", "extra"};
    filtered = store.search(query);
    ASSERT_TRUE(filtered);
    EXPECT_EQ(filtered->total, 3);
    Database db(directory.path / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
    auto ranks = db.prepare("SELECT repo,repo_rank FROM packages WHERE name='firefox' ORDER BY repo_rank");
    ASSERT_TRUE(ranks.step());
    EXPECT_EQ(ranks.text(0), "galaxy");
    EXPECT_EQ(ranks.integer(1), 2);
    ASSERT_TRUE(ranks.step());
    EXPECT_EQ(ranks.text(0), "extra");
    EXPECT_EQ(ranks.integer(1), 3);
}

TEST(Search, ExactPrefixThenFullTextRelevance) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"galaxy", "extra"}));
    auto exact = repo("firefox");
    exact.repo = "galaxy";
    exact.description = "browser";
    ASSERT_TRUE(writer.add(exact));
    auto prefix = repo("firefox-debugadapter", "extra");
    prefix.description = "debug tool";
    ASSERT_TRUE(writer.add(prefix));
    prefix.name = "firefox_remove_ctrl_q";
    prefix.base = prefix.name;
    ASSERT_TRUE(writer.add(prefix));
    auto description_only = repo("browser-helper");
    description_only.description = "firefox firefox firefox firefox firefox firefox";
    ASSERT_TRUE(writer.add(description_only));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    catalog::SearchQuery query;
    query.text = "firefox";
    auto page = store.search(query);
    ASSERT_TRUE(page) << page.error().detail;
    ASSERT_EQ(page->hits.size(), 4);
    EXPECT_EQ(page->hits[0].name, "firefox");
    EXPECT_EQ(page->hits[1].name, "firefox-debugadapter");
    EXPECT_EQ(page->hits[2].name, "firefox_remove_ctrl_q");
    EXPECT_EQ(page->hits[3].name, "browser-helper");
}

TEST(Search, InstalledBreaksEqualRelevanceTies) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"extra"}));
    auto first = repo("firefox-aaa", "extra");
    first.description = "web browser";
    auto second = repo("firefox-zzz", "extra");
    second.description = first.description;
    ASSERT_TRUE(writer.add(first));
    ASSERT_TRUE(writer.add(second));
    catalog::InstalledPackage installed;
    installed.name = second.name;
    installed.version = second.version;
    ASSERT_TRUE(writer.add(installed));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    catalog::SearchQuery query;
    query.text = "firefox";
    auto page = store.search(query);
    ASSERT_TRUE(page) << page.error().detail;
    ASSERT_EQ(page->hits.size(), 2);
    EXPECT_EQ(page->hits[0].name, "firefox-zzz");
    EXPECT_TRUE(page->hits[0].installed);
    EXPECT_EQ(page->hits[1].name, "firefox-aaa");
}

TEST(Search, EmptyCommunityDescriptionLosesEqualRelevanceTie) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({}));
    catalog::AurPackage empty;
    empty.name = "firefox-aaa";
    empty.package_base = empty.name;
    empty.version = "1";
    ASSERT_TRUE(writer.add(empty));
    auto described = empty;
    described.name = "firefox-zzz";
    described.package_base = described.name;
    described.description = "!";
    ASSERT_TRUE(writer.add(described));
    ASSERT_TRUE(writer.commit());
    Store store(directory.path);
    catalog::SearchQuery query;
    query.text = "firefox";
    auto page = store.search(query);
    ASSERT_TRUE(page) << page.error().detail;
    ASSERT_EQ(page->hits.size(), 2);
    EXPECT_EQ(page->hits[0].name, "firefox-zzz");
    EXPECT_EQ(page->hits[1].name, "firefox-aaa");
}

TEST(Cache, SourceStatesPersistSkippedAndUnavailable) {
    Directory directory;
    SnapshotBuilder writer(directory.path);
    ASSERT_TRUE(writer.begin({"galaxy"}));
    ASSERT_TRUE(writer.add(catalog::SourceState{"repo/galaxy", "ok", "invalid-record", "10", "2", 123}));
    ASSERT_TRUE(writer.add(catalog::SourceState{"community", "unavailable", "dump-unavailable", "0", "0", 123}));
    ASSERT_TRUE(writer.add(catalog::SourceState{"apps", "unavailable", "appstream-unavailable", "0", "0", 123}));
    ASSERT_TRUE(writer.add(catalog::SourceState{"broken", "failed", "appstream-xml", "0", "0", 123}));
    ASSERT_TRUE(writer.commit());
    Database db(directory.path / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
    auto rows = db.prepare("SELECT id,kind,fingerprint,etag,last_modified FROM sources ORDER BY id");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.text(0), "apps");
    EXPECT_EQ(rows.text(1), "unavailable");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.text(0), "broken");
    EXPECT_EQ(rows.text(1), "failed");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.text(0), "community");
    EXPECT_EQ(rows.text(1), "unavailable");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.text(0), "repo/galaxy");
    EXPECT_EQ(rows.text(1), "ok");
    EXPECT_EQ(rows.text(2), "invalid-record");
    EXPECT_EQ(rows.text(3), "10");
    EXPECT_EQ(rows.text(4), "2");
}

}
