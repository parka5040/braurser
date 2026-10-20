#include "../../src/pacman-helper/request.hpp"
#include "../../src/elevate/auth.hpp"

#include <gtest/gtest.h>
#include <array>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr char transaction[] = "--braurser-transaction=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr char digest[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

br_request parse(std::initializer_list<const char*> args, bool expected) {
    std::vector<const char*> raw(args);
    br_request request{};
    EXPECT_EQ(br_parse_request(static_cast<int>(raw.size()), raw.data(), &request), expected) << raw.front();
    return request;
}

TEST(PacmanHelperParser, AcceptsExactlySixCanonicalOperations) {
    EXPECT_EQ(parse({"--sync", "--needed", "--asdeps", "--noconfirm", "--", "extra/tree"}, true).operation, 'A');
    EXPECT_EQ(parse({"--sync", "--refresh", "--sysupgrade", "--noconfirm", transaction}, true).operation, 'B');
    EXPECT_EQ(parse({"--sync", "--refresh", "--sysupgrade", "--needed", transaction, "--", "tree"}, true).operation, 'B');
    EXPECT_EQ(parse({"--remove", "--nosave", "--recursive", "--", "tree"}, true).operation, 'C');
    EXPECT_EQ(parse({"--upgrade", "--needed", "--color=never", "--", "/home/u/a.pkg.tar.zst"}, true).operation, 'D');
    EXPECT_EQ(parse({"--database", "--asexplicit", "--noprogressbar", "--", "tree"}, true).operation, 'E');
    EXPECT_EQ(parse({"--auth-check"}, true).operation, 'F');
}

TEST(PacmanHelperParser, RejectsUnknownFlagsMalformedOrderAndTargets) {
    for (auto flag : {"--root", "--dbpath", "--config", "--cachedir", "--hookdir", "--gpgdir",
                      "--logfile", "--overwrite", "--assume-installed", "--dbonly", "--sysroot",
                      "--ignore", "--ignoregroup", "--print", "--arch", "--disable-download-timeout",
                      "-d", "--nodeps", "-dd", "-w", "--downloadonly"}) {
        parse({"--sync", "--needed", flag, "--", "tree"}, false);
    }
    for (auto target : {"", "-n", "Foo", "space name", ".hidden", "repo//name",
                        "BAD!/name", "https://host/pkg", "../escape"}) {
        parse({"--sync", "--needed", "--", target}, false);
    }
    parse({"--sync", "--refresh", "--"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--needed"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--", "tree"}, false);
    parse({"--sync", "--needed", "--noconfirm", "--noconfirm", "--", "tree"}, false);
    parse({"--sync", "--needed", "--", "tree", "--"}, false);
    parse({"--sync", "--needed", "--", "tree", "--root"}, false);
    parse({"--remove", "--nosave", "--recursive", "--", "extra/tree"}, false);
    parse({"--upgrade", "--", "relative.pkg.tar.zst"}, false);
    parse({"--upgrade", "--", "/home/u/a\n2026-01-01T00:00:00Z uid=0.pkg.tar.zst"}, false);
    parse({"--upgrade", "--", "/home/u/a\x1b[2K.pkg.tar.zst"}, false);
    parse({"--upgrade", "--", "/home/u/a\x7f.pkg.tar.zst"}, false);
    parse({"--auth-check", "extra"}, false);
    std::vector<const char*> too_many{"--upgrade", "--"};
    for (int i = 0; i < 65; ++i) too_many.push_back("/a.pkg.tar.zst");
    br_request request{};
    EXPECT_FALSE(br_parse_request(static_cast<int>(too_many.size()), too_many.data(), &request));
    std::vector<const char*> many{"--sync", "--needed", "--"};
    for (int i = 0; i < 512; ++i) many.push_back("tree");
    EXPECT_TRUE(br_parse_request(static_cast<int>(many.size()), many.data(), &request));
    many.push_back("tree");
    EXPECT_FALSE(br_parse_request(static_cast<int>(many.size()), many.data(), &request));
}

TEST(PacmanHelperParser, RequiresOneCanonicalTransactionDigestForSystemUpdates) {
    auto request = parse({"--sync", "--refresh", "--sysupgrade", transaction}, true);
    EXPECT_STREQ(request.transaction, digest);
    parse({"--sync", "--refresh", "--sysupgrade", "--color=never", "--noprogressbar", transaction}, true);
    parse({"--sync", "--refresh", "--sysupgrade", transaction, "--", "tree"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--needed", transaction, "--", "extra/tree"}, true);
    parse({"--sync", "--refresh", "--sysupgrade"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--noconfirm"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", transaction, transaction}, false);
    parse({"--sync", "--refresh", "--sysupgrade", transaction, "--noconfirm"}, false);
    parse({"--sync", "--refresh", transaction, "--sysupgrade"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--needed", transaction}, false);
    parse({"--sync", "--refresh", "--sysupgrade", transaction, "--"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--needed", "--", transaction, "tree"}, false);
    parse({"--sync", "--needed", transaction, "--", "tree"}, false);
    parse({"--sync", "--refresh", "--sysupgrade", "--noconfirm", transaction, "--", "tree"}, false);
    for (const std::string& value : {std::string{}, std::string(63, 'a'), std::string(65, 'a'),
                                    std::string(64, 'A'), std::string(64, 'g'), std::string(64, '0') + "\n"}) {
        const auto flag = "--braurser-transaction=" + value;
        parse({"--sync", "--refresh", "--sysupgrade", flag.c_str()}, false);
    }
    for (size_t i = 0; i < 64; ++i) {
        std::string value = digest;
        value[i] = 'F';
        const auto flag = "--braurser-transaction=" + value;
        parse({"--sync", "--refresh", "--sysupgrade", flag.c_str()}, false);
    }
}

TEST(PacmanHelperTransaction, FinalAndPrintCommandsAreExact) {
    auto request = parse({"--sync", "--refresh", "--sysupgrade", "--noconfirm", "--needed",
                          "--color=never", "--noprogressbar", transaction, "--", "extra/tree", "vim"}, true);
    std::array<const char*, 532> argv{};
    auto count = br_build_upgrade_argv(&request, "/stage/db", argv.data(), argv.size());
    ASSERT_GT(count, 0u);
    std::vector<std::string> final(argv.begin(), argv.begin() + static_cast<std::ptrdiff_t>(count));
    EXPECT_EQ(final, (std::vector<std::string>{"pacman", "--sync", "--sysupgrade", "--noconfirm",
        "--dbpath", "/stage/db", "--color=never", "--noprogressbar", "--needed", "--", "extra/tree", "vim"}));
    EXPECT_EQ(argv[count], nullptr);
    for (const auto& arg : final) {
        EXPECT_NE(arg, "--refresh");
        EXPECT_EQ(arg.find("--braurser-transaction"), std::string::npos);
    }
    count = br_build_print_argv(&request, "/stage/db", argv.data(), argv.size());
    ASSERT_GT(count, 0u);
    EXPECT_EQ((std::vector<std::string>(argv.begin(), argv.begin() + static_cast<std::ptrdiff_t>(count))),
        (std::vector<std::string>{"pacman", "--sync", "--sysupgrade", "--print", "--print-format",
        "%r/%n %v", "--dbpath", "/stage/db", "--color=never", "--needed", "--", "extra/tree", "vim"}));
    EXPECT_EQ(argv[count], nullptr);
    EXPECT_EQ(br_build_upgrade_argv(&request, "/stage/db", argv.data(), 3), 0u);
    EXPECT_EQ(br_build_print_argv(&request, "/stage/db", argv.data(), 3), 0u);
    request = parse({"--sync", "--refresh", "--sysupgrade", transaction}, true);
    count = br_build_upgrade_argv(&request, "/stage/db", argv.data(), argv.size());
    EXPECT_EQ((std::vector<std::string>(argv.begin(), argv.begin() + static_cast<std::ptrdiff_t>(count))),
        (std::vector<std::string>{"pacman", "--sync", "--sysupgrade", "--noconfirm", "--dbpath", "/stage/db"}));
    count = br_build_print_argv(&request, "/stage/db", argv.data(), argv.size());
    EXPECT_EQ((std::vector<std::string>(argv.begin(), argv.begin() + static_cast<std::ptrdiff_t>(count))),
        (std::vector<std::string>{"pacman", "--sync", "--sysupgrade", "--print", "--print-format",
        "%r/%n %v", "--dbpath", "/stage/db", "--color=never"}));
}

TEST(PacmanHelperTransaction, FinalCommandBoundsIncludeDatabasePathAndTerminator) {
    br_request request{};
    request.operation = 'B';
    request.needed = 1;
    request.color = 1;
    request.progress = 1;
    request.count = 512;
    for (auto& target : request.targets) target = "tree";
    std::array<const char*, 523> argv{};
    EXPECT_EQ(br_build_upgrade_argv(&request, "/stage/db", argv.data(), argv.size() - 1), 0u);
    EXPECT_EQ(br_build_upgrade_argv(&request, "/stage/db", argv.data(), argv.size()), 522u);
    EXPECT_EQ(argv.back(), nullptr);
    for (const char* path : std::initializer_list<const char*>{nullptr, "", "relative", "/var/../lib"})
        EXPECT_EQ(br_build_upgrade_argv(&request, path, argv.data(), argv.size()), 0u);
}

TEST(PacmanHelperTransaction, ClassifiesSyncEntriesAndSelectsOnlyMissingDatabaseSignatures) {
    for (const char* name : {"core.db", "extra.db", "custom-repo.db", "core.db.sig"})
        EXPECT_EQ(br_sync_entry_action(name, S_IFREG | 0644, 1), BR_SYNC_OVERWRITE) << name;
    for (const char* name : {"core.db", "core.files", "core.files.sig", "notes", ".metadata",
                             "BAD!.db", "BAD!.db.sig", "core.db.sig.bak", ".db.sig"}) {
        EXPECT_EQ(br_sync_entry_action(name, S_IFREG | 0644, 0), BR_SYNC_COPY) << name;
        if (std::string_view(name) != "core.db") {
            EXPECT_EQ(br_sync_entry_action(name, S_IFREG | 0644, 1), BR_SYNC_COPY) << name;
        }
    }
    for (const char* name : {"core.db.sig", "extra.db.sig", "removed-repo.db.sig"}) {
        EXPECT_EQ(br_sync_entry_action(name, S_IFREG | 0644, 0), BR_SYNC_DELETE) << name;
        EXPECT_EQ(br_sync_entry_action(name, S_IFREG | 0644, 1), BR_SYNC_OVERWRITE) << name;
    }
    EXPECT_EQ(br_sync_entry_action(".", S_IFDIR | 0755, 0), BR_SYNC_SKIP);
    EXPECT_EQ(br_sync_entry_action("..", S_IFDIR | 0755, 0), BR_SYNC_SKIP);
    for (const mode_t mode : std::array<mode_t, 4>{S_IFDIR, S_IFLNK, S_IFIFO, S_IFSOCK})
        EXPECT_EQ(br_sync_entry_action("core.db", mode, 1), BR_SYNC_INVALID);
    for (const char* name : std::initializer_list<const char*>{nullptr, "", "../core.db", "sync/core.db"})
        EXPECT_EQ(br_sync_entry_action(name, S_IFREG, 1), BR_SYNC_INVALID);
    EXPECT_EQ(br_sync_entry_action(std::string(256, 'a').c_str(), S_IFREG, 1), BR_SYNC_INVALID);
    EXPECT_EQ(br_sync_entry_action("core.db", S_IFREG, -1), BR_SYNC_INVALID);
}

TEST(PacmanHelperTransaction, EncodesAndComparesEveryDigestByte) {
    uint8_t raw[32]{};
    for (size_t i = 0; i < 32; ++i) raw[i] = static_cast<uint8_t>(i);
    char hex[65]{};
    br_transaction_hex(raw, hex);
    EXPECT_STREQ(hex, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    EXPECT_TRUE(br_transaction_equal(hex, hex));
    for (size_t i = 0; i < 65; ++i) {
        char other[65]{};
        std::memcpy(other, hex, sizeof(other));
        other[i] = other[i] == '0' ? '1' : '0';
        EXPECT_FALSE(br_transaction_equal(hex, other)) << i;
        EXPECT_FALSE(br_transaction_equal(other, hex)) << i;
    }
    char invalid[65]{};
    EXPECT_FALSE(br_transaction_equal(invalid, invalid));
    std::memset(invalid, 'A', 64);
    EXPECT_FALSE(br_transaction_equal(invalid, invalid));
    std::memset(invalid, 'g', 64);
    EXPECT_FALSE(br_transaction_equal(invalid, invalid));
}

TEST(PacmanHelperTransaction, RejectsUnsafeDatabasePaths) {
    EXPECT_TRUE(br_valid_dbpath("/var/lib/pacman"));
    EXPECT_TRUE(br_valid_dbpath("/"));
    EXPECT_TRUE(br_valid_dbpath("/var/db..name"));
    for (const auto* path : {"", "relative", "/..", "/../var", "/var/../lib", "/var/..",
                             "/var/db\n", "/var/db\r", "/var/db\x1b", "/var/db\x7f"})
        EXPECT_FALSE(br_valid_dbpath(path)) << path;
    EXPECT_FALSE(br_valid_dbpath(std::string(4096, '/').c_str()));
}

TEST(PacmanHelperArchive, AggregateBudgetRejectsDuplicateSourcesAndAmplification) {
    br_archive_batch batch{};
    EXPECT_TRUE(br_add_archive(&batch, 1, 10, UINT64_C(4294967296)));
    EXPECT_FALSE(br_add_archive(&batch, 1, 10, 1));
    EXPECT_TRUE(br_add_archive(&batch, 2, 10, UINT64_C(4294967296)));
    EXPECT_EQ(batch.total, UINT64_C(8589934592));
    EXPECT_FALSE(br_add_archive(&batch, 1, 11, 1));
    EXPECT_EQ(batch.count, 2u);
    br_archive_batch oversized{};
    EXPECT_FALSE(br_add_archive(&oversized, 1, 1, UINT64_C(4294967297)));
    EXPECT_FALSE(br_add_archive(&oversized, 1, 1, UINT64_MAX));
    for (ino_t i = 0; i < 64; ++i) EXPECT_TRUE(br_add_archive(&oversized, 1, i, 0));
    EXPECT_FALSE(br_add_archive(&oversized, 1, 64, 0));
}

TEST(PacmanHelperArchive, FreeSpaceIncludesReserveWithoutMultiplicationOverflow) {
    EXPECT_TRUE(br_stage_space(UINT64_C(8589934592), UINT64_C(2359296), 4096));
    EXPECT_FALSE(br_stage_space(UINT64_C(8589934592), UINT64_C(2359295), 4096));
    EXPECT_FALSE(br_stage_space(0, UINT64_MAX, 0));
    EXPECT_FALSE(br_stage_space(UINT64_MAX, UINT64_MAX, UINT64_MAX));
    EXPECT_TRUE(br_stage_space(UINT64_C(8589934592), UINT64_MAX, UINT64_MAX));
    EXPECT_FALSE(br_stage_space(1, UINT64_C(262144), 4096));
    EXPECT_TRUE(br_stage_space(1, UINT64_C(262145), 4096));
}

TEST(PacmanHelperIdentity, RejectsMissingRootAndConflicts) {
    uid_t result = 0;
    EXPECT_FALSE(br_caller_uid(nullptr, nullptr, &result));
    EXPECT_FALSE(br_caller_uid("0", nullptr, &result));
    EXPECT_FALSE(br_caller_uid(nullptr, "0", &result));
    EXPECT_FALSE(br_caller_uid("12", "13", &result));
    EXPECT_FALSE(br_caller_uid("-1", nullptr, &result));
    EXPECT_FALSE(br_caller_uid("999999999999999999999", nullptr, &result));
    EXPECT_TRUE(br_caller_uid("123", "123", &result));
    EXPECT_EQ(result, 123u);
    EXPECT_TRUE(br_caller_uid(nullptr, "234", &result));
    EXPECT_EQ(result, 234u);
}

class TempTree {
public:
    TempTree() {
        auto template_path = (std::filesystem::temp_directory_path() / "pacman-security-XXXXXX").string();
        std::vector<char> buffer(template_path.begin(), template_path.end());
        buffer.push_back(0);
        if (mkdtemp(buffer.data())) root = buffer.data();
    }
    ~TempTree() { if (!root.empty()) std::filesystem::remove_all(root); }
    std::filesystem::path root;
};

struct DatabaseStats {
    size_t calls = 0;
    size_t reject_at = 0;
    mode_t mode = S_IFDIR | 0755;
    uid_t owner = 0;
    int failure = 0;
};

int database_stat(int, struct stat* metadata, void* context) {
    auto& injected = *static_cast<DatabaseStats*>(context);
    *metadata = {};
    metadata->st_mode = S_IFDIR | 0755;
    metadata->st_uid = 0;
    if (++injected.calls == injected.reject_at) {
        metadata->st_mode = injected.mode;
        metadata->st_uid = injected.owner;
        return injected.failure;
    }
    return 0;
}

TEST(PacmanHelperDatabase, TrustPolicyRejectsWritableForeignAndNonDirectoryComponents) {
    struct stat metadata{};
    metadata.st_mode = S_IFDIR | 0755;
    EXPECT_TRUE(br_trusted_directory(&metadata));
    metadata.st_mode = S_IFDIR | 0700;
    EXPECT_TRUE(br_trusted_directory(&metadata));
    metadata.st_mode = S_IFDIR | 0755;
    metadata.st_uid = 1000;
    EXPECT_FALSE(br_trusted_directory(&metadata));
    metadata.st_uid = 0;
    for (const mode_t mode : std::array<mode_t, 7>{S_IFDIR | 0775, S_IFDIR | 0757, S_IFDIR | 01777,
                                                  S_IFREG | 0755, S_IFLNK | 0755, S_IFIFO | 0600, 0}) {
        metadata.st_mode = mode;
        EXPECT_FALSE(br_trusted_directory(&metadata)) << mode;
    }
    EXPECT_FALSE(br_trusted_directory(nullptr));
}

TEST(PacmanHelperDatabase, TrustWalkInspectsRootAndEveryAncestorUsingInjectedMetadata) {
    TempTree tree;
    ASSERT_FALSE(tree.root.empty());
    const auto database = tree.root / "parent/database";
    std::filesystem::create_directories(database / "local");
    std::filesystem::create_directories(database / "sync");
    DatabaseStats stats;
    const br_database_provider provider{database_stat, &stats};
    const auto path = database.string();
    int fd = br_open_database_path(path.c_str(), &provider);
    ASSERT_GE(fd, 0);
    const size_t ancestors = stats.calls;
    EXPECT_GE(ancestors, 4u);
    for (const char* name : {"local", "sync"}) {
        int child = br_open_trusted_directory(fd, name, &provider);
        ASSERT_GE(child, 0);
        close(child);
    }
    close(fd);
    for (size_t component = 1; component <= ancestors; ++component) {
        for (const mode_t mode : std::array<mode_t, 4>{S_IFDIR | 0775, S_IFDIR | 0757,
                                                       S_IFDIR | 01777, S_IFLNK | 0755}) {
            stats = {};
            stats.reject_at = component;
            stats.mode = mode;
            EXPECT_EQ(br_open_database_path(path.c_str(), &provider), -1) << component << " " << mode;
            EXPECT_EQ(stats.calls, component);
        }
        stats = {};
        stats.reject_at = component;
        stats.owner = 1000;
        EXPECT_EQ(br_open_database_path(path.c_str(), &provider), -1) << component;
        stats = {};
        stats.reject_at = component;
        stats.failure = -1;
        EXPECT_EQ(br_open_database_path(path.c_str(), &provider), -1) << component;
    }
    for (const char* name : {"local", "sync"}) {
        stats = {};
        fd = br_open_database_path(path.c_str(), &provider);
        ASSERT_GE(fd, 0);
        stats.reject_at = stats.calls + 1;
        stats.owner = 1000;
        EXPECT_EQ(br_open_trusted_directory(fd, name, &provider), -1) << name;
        close(fd);
    }
    stats = {};
    const auto alias = tree.root / "alias";
    std::filesystem::create_directory_symlink(database, alias);
    EXPECT_EQ(br_open_database_path(alias.c_str(), &provider), -1);
    EXPECT_EQ(br_open_database_path((alias / "local").c_str(), &provider), -1);
    std::filesystem::remove(database / "sync");
    std::filesystem::create_directory_symlink(database / "local", database / "sync");
    fd = br_open_database_path(path.c_str(), &provider);
    ASSERT_GE(fd, 0);
    EXPECT_EQ(br_open_trusted_directory(fd, "sync", &provider), -1);
    close(fd);
    EXPECT_EQ(br_open_database_path("relative", &provider), -1);
    const br_database_provider invalid{nullptr, nullptr};
    EXPECT_EQ(br_open_database_path(path.c_str(), &invalid), -1);
}

TEST(PacmanHelperCleanup, NonArchiveFailuresWith65And512TargetsNeverUnlinkUnstagedPaths) {
    TempTree tree;
    ASSERT_FALSE(tree.root.empty());
    const auto protected_file = tree.root / "must-survive";
    std::ofstream(protected_file) << "not staged";
    for (const size_t count : {65u, 512u}) {
        for (const char operation : {'A', 'B', 'C', 'E'}) {
            std::vector<const char*> args;
            if (operation == 'A') args = {"--sync", "--needed", "--"};
            if (operation == 'B') args = {"--sync", "--refresh", "--sysupgrade", "--needed", transaction, "--"};
            if (operation == 'C') args = {"--remove", "--nosave", "--recursive", "--"};
            if (operation == 'E') args = {"--database", "--asdeps", "--"};
            args.insert(args.end(), count, "tree");
            br_request request{};
            ASSERT_TRUE(br_parse_request(static_cast<int>(args.size()), args.data(), &request));
            ASSERT_EQ(request.count, count);
            ASSERT_EQ(request.operation, operation);
            char copies[64][4096]{};
            const auto path = protected_file.string();
            std::memcpy(copies[0], path.c_str(), path.size() + 1);
            size_t staged = 0;
            br_clean_archives(copies, staged);
            EXPECT_TRUE(std::filesystem::exists(protected_file)) << operation << " " << count;
        }
    }
}

TEST(PacmanHelperCleanup, RemovesOnlySuccessfullyStagedPaths) {
    TempTree tree;
    ASSERT_FALSE(tree.root.empty());
    char copies[64][4096]{};
    for (size_t i = 0; i < 2; ++i) {
        const auto path = tree.root / std::to_string(i);
        std::ofstream(path) << "archive";
        const auto text = path.string();
        std::memcpy(copies[i], text.c_str(), text.size() + 1);
    }
    br_clean_archives(copies, 1);
    EXPECT_FALSE(std::filesystem::exists(tree.root / "0"));
    EXPECT_TRUE(std::filesystem::exists(tree.root / "1"));
    br_clean_archives(copies, 512);
    EXPECT_FALSE(std::filesystem::exists(tree.root / "1"));
}

int directory_open(int parent, const char* name, void*) {
    return openat(parent, name, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
}
int directory_inspect(int fd, uid_t caller, void*) {
    struct stat st{};
    if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) return 0;
    if (st.st_uid != caller && st.st_uid != 0 && st.st_uid != 65534) return 0;
    if ((st.st_mode & 0022) && !(st.st_mode & S_ISVTX && (st.st_uid == 0 || st.st_uid == 65534))) return 0;
    return 1;
}
int file_open(int parent, const char* name, void*) {
    return openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
}
int file_inspect(int fd, uid_t caller, void*) {
    struct stat st{};
    return !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == caller &&
           st.st_size >= 0 && static_cast<uint64_t>(st.st_size) <= UINT64_C(4294967296);
}
int foreign_file(int, uid_t, void*) { return 0; }
TEST(PacmanHelperArchive, RestrictsLocationOwnershipTypeAndPermissions) {
    const br_archive_provider provider{directory_open, directory_inspect, file_open, file_inspect, nullptr};
    TempTree t;
    ASSERT_FALSE(t.root.empty());
    auto base = t.root / ".cache/braurser/build/paru/job/pkg";
    std::filesystem::create_directories(base);
    auto file = base / "pkg-1-1-x86_64.pkg.tar.zst";
    std::ofstream(file) << "archive";
    const auto path = file.string();
    const auto home = t.root.string();
    EXPECT_TRUE(br_validate_archive_path(path.c_str(), home.c_str(), getuid(), &provider));
    const br_archive_provider foreign{directory_open, directory_inspect, file_open, foreign_file, nullptr};
    EXPECT_FALSE(br_validate_archive_path(path.c_str(), home.c_str(), getuid(), &foreign));
    EXPECT_FALSE(br_validate_archive_path(path.c_str(), home.c_str(), getuid() + 1, &provider));
    chmod(base.c_str(), 0770);
    EXPECT_FALSE(br_validate_archive_path(path.c_str(), home.c_str(), getuid(), &provider));
    chmod(base.c_str(), 0700);
    auto alias = base / "alias-1-1-x86_64.pkg.tar.zst";
    std::filesystem::create_symlink(file, alias);
    EXPECT_FALSE(br_validate_archive_path(alias.c_str(), home.c_str(), getuid(), &provider));
    auto wrong = t.root / ".cache/braurser/build/other/job/pkg/pkg-1-1-x86_64.pkg.tar.zst";
    EXPECT_FALSE(br_validate_archive_path(wrong.c_str(), home.c_str(), getuid(), &provider));
    auto linked = t.root / ".cache/braurser/build/paru/job/linked";
    std::filesystem::create_symlink(base, linked);
    auto linked_file = linked / "pkg-1-1-x86_64.pkg.tar.zst";
    EXPECT_FALSE(br_validate_archive_path(linked_file.c_str(), home.c_str(), getuid(), &provider));
    auto escape = t.root / ".cache/braurser/build/paru/../pkg/pkg/a.pkg.tar.zst";
    EXPECT_FALSE(br_validate_archive_path(escape.c_str(), home.c_str(), getuid(), &provider));
    auto outside = t.root / "elsewhere.pkg.tar.zst";
    EXPECT_FALSE(br_validate_archive_path(outside.c_str(), home.c_str(), getuid(), &provider));
    auto oversize = base / "huge.pkg.tar.zst";
    int fd = open(oversize.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, static_cast<off_t>(UINT64_C(4294967297))), 0);
    close(fd);
    EXPECT_FALSE(br_validate_archive_path(oversize.c_str(), home.c_str(), getuid(), &provider));
    std::filesystem::remove(file);
    std::filesystem::create_directory(file);
    EXPECT_FALSE(br_validate_archive_path(path.c_str(), home.c_str(), getuid(), &provider));
}

void on_term(int) { _exit(77); }
TEST(PacmanHelperAuth, FailedProbeCreatesMarkerAndSignalsProcessGroup) {
    TempTree t;
    ASSERT_FALSE(t.root.empty());
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (setpgid(0, 0)) _exit(90);
        struct sigaction action{};
        action.sa_handler = on_term;
        sigaction(SIGTERM, &action, nullptr);
        _exit(braurser::elevate::auth_check(FAILED_METHOD_PATH, {"--auth-check"}, t.root.string()) + 91);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 77);
    struct stat st{};
    ASSERT_EQ(stat((t.root / "auth-failed").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600);
}
TEST(PacmanHelperAuth, SuccessfulProbeDoesNotCreateMarker) {
    TempTree t;
    EXPECT_EQ(braurser::elevate::auth_check("/usr/bin/true", {"--auth-check"}, t.root.string()), 0);
    EXPECT_FALSE(std::filesystem::exists(t.root / "auth-failed"));
}
}
