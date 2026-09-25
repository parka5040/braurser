#include "../../src/pacman-helper/request.hpp"
#include "../../src/elevate/auth.hpp"

#include <gtest/gtest.h>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
br_request parse(std::initializer_list<const char*> args, bool expected) {
    std::vector<const char*> raw(args);
    br_request request{};
    EXPECT_EQ(br_parse_request(static_cast<int>(raw.size()), raw.data(), &request), expected) << raw.front();
    return request;
}

TEST(PacmanHelperParser, AcceptsExactlySixCanonicalOperations) {
    EXPECT_EQ(parse({"--sync", "--needed", "--asdeps", "--noconfirm", "--", "extra/tree"}, true).operation, 'A');
    EXPECT_EQ(parse({"--sync", "--refresh", "--sysupgrade", "--noconfirm"}, true).operation, 'B');
    EXPECT_EQ(parse({"--sync", "--refresh", "--sysupgrade", "--needed", "--", "tree"}, true).operation, 'B');
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
