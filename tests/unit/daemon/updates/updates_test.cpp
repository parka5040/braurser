#include <braurser/daemon/updates/updates.hpp>
#include <braurser/daemon/core/files.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using namespace braurser::daemon;
namespace {
struct Runner final : core::ProcessRunner {
    core::ProcessResult reply{};
    core::ProcessSpec recorded;
    Runner() { reply.exit_code = 0; }
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec& spec) override {
        recorded = spec;
        return reply;
    }
};
struct Tree {
    std::filesystem::path path;
    Tree() {
        auto text = (std::filesystem::temp_directory_path() / "braurser-updates-XXXXXX").string();
        std::vector<char> buf(text.begin(), text.end());
        buf.push_back('\0');
        auto* created = mkdtemp(buf.data());
        if (!created) throw std::runtime_error("mkdtemp failed");
        path = created;
    }
    ~Tree() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
void write(const std::filesystem::path& path, std::string_view text) {
    if (!core::ensure_private_dir(path.parent_path()) || !core::atomic_write(path, text))
        throw std::runtime_error("test fixture write failed");
}
std::string fixture(const char* name) {
    auto read = core::read_bounded(std::filesystem::path(UPDATES_FIXTURES) / name, 1024 * 1024);
    if (!read) throw std::runtime_error("fixture missing");
    return *read;
}
}

TEST(Update, OfficialParsesIgnoredAndSorts) {
    Runner runner;
    runner.reply.out = fixture("official.txt");
    updates::OfficialUpdateChecker checker(runner);
    auto found = checker.check("/tmp/private-db");
    ASSERT_TRUE(found.has_value());
    ASSERT_EQ(found->size(), 2);
    EXPECT_EQ((*found)[0].name, "linux");
    EXPECT_TRUE((*found)[0].ignored);
    EXPECT_EQ((*found)[1].name, "zlib");
    EXPECT_EQ(runner.recorded.executable, "/usr/bin/pacman");
    EXPECT_EQ(runner.recorded.args, (std::vector<std::string>{"-Qu", "--dbpath", "/tmp/private-db"}));
}

TEST(Update, OfficialRejectsMalformedAndAcceptsExitOneEmpty) {
    Runner runner;
    runner.reply.out = fixture("malformed.txt");
    updates::OfficialUpdateChecker checker(runner);
    EXPECT_FALSE(checker.check("/tmp/private-db"));
    runner.reply = {};
    runner.reply.exit_code = 1;
    auto none = checker.check("/tmp/private-db");
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->empty());
    runner.reply.err = "failure";
    EXPECT_FALSE(checker.check("/tmp/private-db"));
    runner.reply = {};
    runner.reply.exit_code = 0;
    for (std::string_view text : {"linux 2 -> 1\n", "linux 1 -> 2\nlinux 1 -> 3\n",
                                  "../../evil 1 -> 2\n", "linux 1 -> 2 [ignored-more]\n"}) {
        runner.reply.out = text;
        EXPECT_FALSE(checker.check("/tmp/private-db"));
    }
}

TEST(Update, CommunityOnlyReturnsForeign) {
    Runner runner;
    runner.reply.out = fixture("community.txt");
    helpers::HelperEnvironment env{};
    env.home = "/home/test";
    env.user = "test";
    env.runtime_dir = "/tmp/test";
    helpers::ParuHelper helper(env, runner);
    updates::CommunityUpdateChecker checker(runner, helper);
    auto found = checker.check({"aur-app"});
    ASSERT_TRUE(found);
    ASSERT_EQ(found->size(), 1);
    EXPECT_EQ(found->front().name, "aur-app");
    EXPECT_EQ(runner.recorded.args, (std::vector<std::string>{"-Qua"}));
}

TEST(Update, CommunityAcceptsDevelMarkerAndSkipsNonNewer) {
    Runner runner;
    runner.reply.exit_code = 0;
    runner.reply.out = "gruvbox-plus-icon-theme-git 6.6.0.r202.g563e9ea-1 -> latest-commit\nopenai-codex-bin 0.156.1-1 -> 0.157.0-1\nstale-app 2.0-1 -> 1.0-1\n";
    helpers::HelperEnvironment env{};
    env.home = "/home/test";
    env.user = "test";
    env.runtime_dir = "/tmp/test";
    helpers::ParuHelper helper(env, runner);
    updates::CommunityUpdateChecker checker(runner, helper);
    auto found = checker.check({"gruvbox-plus-icon-theme-git", "openai-codex-bin", "stale-app"});
    ASSERT_TRUE(found);
    ASSERT_EQ(found->size(), 2);
    EXPECT_EQ(found->at(0).name, "gruvbox-plus-icon-theme-git");
    EXPECT_EQ(found->at(0).to, "latest-commit");
    EXPECT_EQ(found->at(1).name, "openai-codex-bin");
}

TEST(Update, TempDbUsesPrivateSnapshotAndRemovesIt) {
    Tree tree;
    write(tree.path / "real/local/demo-1/desc", "%NAME%\ndemo\n");
    write(tree.path / "real/sync/core.db", "original");
    Runner runner;
    std::filesystem::path db;
    {
        auto created = updates::TempSyncDb::create(runner, tree.path / "real", tree.path / "runtime", "/usr/bin/true");
        ASSERT_TRUE(created) << created.error().detail;
        db = created->path();
        ASSERT_TRUE(std::filesystem::is_symlink(db / "local"));
        auto copy = core::read_bounded(db / "sync/core.db", 100);
        ASSERT_TRUE(copy);
        EXPECT_EQ(*copy, "original");
        EXPECT_EQ(runner.recorded.executable, "/usr/bin/true");
        EXPECT_EQ(runner.recorded.args.back(), "--disable-sandbox");
        EXPECT_EQ(runner.recorded.timeout, std::chrono::minutes(5));
    }
    EXPECT_FALSE(std::filesystem::exists(db));
    auto absent = updates::TempSyncDb::create(runner, tree.path / "real", tree.path / "runtime", tree.path / "missing");
    ASSERT_FALSE(absent);
    EXPECT_EQ(absent.error().code, updates::UpdateErrorCode::Unavailable);
}

TEST(Update, TempDbSweepsStaleCopiesFromEarlierDaemons) {
    Tree tree;
    write(tree.path / "real/local/demo-1/desc", "%NAME%\ndemo\n");
    write(tree.path / "real/sync/core.db", "original");
    const auto root = tree.path / "runtime/braurser/checkdb";
    write(root / "db-999999999-4/sync/core.db", "stale");
    ASSERT_EQ(symlink((tree.path / "real/local").c_str(), (root / "db-999999999-4/local").c_str()), 0);
    const auto own = root / ("db-" + std::to_string(getpid()) + "-99999");
    write(own / "sync/core.db", "current");
    write(root / "db-x-1/keep", "x");
    write(root / "unrelated/keep", "x");
    Runner runner;
    {
        auto created = updates::TempSyncDb::create(runner, tree.path / "real", tree.path / "runtime", "/usr/bin/true");
        ASSERT_TRUE(created) << created.error().detail;
    }
    EXPECT_FALSE(std::filesystem::exists(root / "db-999999999-4"));
    EXPECT_TRUE(std::filesystem::exists(tree.path / "real/local/demo-1/desc"));
    EXPECT_TRUE(std::filesystem::exists(own / "sync/core.db"));
    EXPECT_TRUE(std::filesystem::exists(root / "db-x-1/keep"));
    EXPECT_TRUE(std::filesystem::exists(root / "unrelated/keep"));
}

TEST(Update, DISABLED_LiveTempDbCheck) {
    core::PosixProcessRunner real;
    auto checked = updates::TempSyncDb::create(real, "/var/lib/pacman", std::filesystem::temp_directory_path());
    ASSERT_TRUE(checked) << checked.error().detail;
    auto updates = updates::OfficialUpdateChecker(real).check(checked->path());
    ASSERT_TRUE(updates) << updates.error().detail;
}

TEST(Selection, SystemNeverSelectsSubset) {
    updates::UpdateReport report{{{"linux", "1", "2"}, {"glibc", "1", "2"}}, {}, {}};
    updates::UpdateSelectionPolicy policy;
    auto choice = policy.decide(report, {updates::SelectionKind::System, ""});
    ASSERT_TRUE(choice.allowed);
    ASSERT_EQ(choice.transactions.size(), 1);
    EXPECT_EQ(choice.transactions[0].args,
        (std::vector<std::string>{"--sync", "--refresh", "--sysupgrade"}));
}

TEST(Selection, BlocksCommunityWhenResolvedRepoDependencyIsPending) {
    updates::UpdateReport report{{{"libx", "1", "2"}}, {{"app", "1", "2"}}, {}};
    updates::UpdateSelectionPolicy policy;
    auto blocked = policy.decide(report, {updates::SelectionKind::Community, "app"}, {"libx"});
    EXPECT_FALSE(blocked.allowed);
    ASSERT_EQ(blocked.blockers.size(), 1);
    EXPECT_EQ(blocked.blockers[0].code, "system-update-required");
    auto allowed = policy.decide(report, {updates::SelectionKind::Community, "app"}, {"liby"});
    ASSERT_TRUE(allowed.allowed);
    EXPECT_EQ(allowed.transactions[0].args, (std::vector<std::string>{"install", "aur/app"}));
}

TEST(Selection, InstallRefreshesWholeSystemWhenStale) {
    updates::UpdateSelectionPolicy policy;
    auto stale = policy.install_official("demo", false);
    ASSERT_TRUE(stale.allowed);
    EXPECT_EQ(stale.reason, "update-and-install");
    EXPECT_EQ(stale.transactions[0].args,
        (std::vector<std::string>{"--sync", "--refresh", "--sysupgrade", "--needed", "--", "demo"}));
    auto fresh = policy.install_official("demo", true);
    EXPECT_EQ(fresh.transactions[0].args, (std::vector<std::string>{"--sync", "--needed", "--", "demo"}));
    EXPECT_FALSE(policy.install_official("--sysupgrade", true).allowed);
}

TEST(Selection, ManualReceiptMustExist) {
    updates::UpdateReport report{{}, {}, {{"receipt-2"}}};
    updates::UpdateSelectionPolicy policy;
    EXPECT_FALSE(policy.decide(report, {updates::SelectionKind::Manual, "receipt-1"}).allowed);
    auto chosen = policy.decide(report, {updates::SelectionKind::Manual, "receipt-2"});
    ASSERT_TRUE(chosen.allowed);
    EXPECT_EQ(chosen.transactions[0].args, (std::vector<std::string>{"upgrade", "receipt-2"}));
}

TEST(Freshness, ChecksUpdatePresenceAndDatabaseDigest) {
    Tree tree;
    write(tree.path / "real/sync/core.db", "same");
    write(tree.path / "temp/sync/core.db", "same");
    auto same = updates::is_real_db_stale(tree.path / "temp", tree.path / "real", false);
    ASSERT_TRUE(same);
    EXPECT_FALSE(*same);
    write(tree.path / "temp/sync/core.db", "diff");
    auto changed = updates::is_real_db_stale(tree.path / "temp", tree.path / "real", false);
    ASSERT_TRUE(changed);
    EXPECT_TRUE(*changed);
    EXPECT_EQ(updates::is_real_db_stale(tree.path / "temp", tree.path / "real", true).value(), true);
    write(tree.path / "temp/sync/extra.db", "extra");
    EXPECT_TRUE(*updates::is_real_db_stale(tree.path / "temp", tree.path / "real", false));
    std::filesystem::remove(tree.path / "temp/sync/core.db");
    std::filesystem::create_symlink(tree.path / "real/sync/core.db", tree.path / "temp/sync/core.db");
    EXPECT_FALSE(updates::is_real_db_stale(tree.path / "temp", tree.path / "real", false));
}

TEST(Restart, RuleLoaderAcceptsShippedAndRejectsInvalid) {
    updates::RestartRuleLoader loader;
    auto shipped = loader.load(RESTART_RULES);
    ASSERT_TRUE(shipped) << shipped.error().detail;
    EXPECT_GE(shipped->size(), 8);
    Tree tree;
    for (const auto& bad : {"{", R"({"version":1,"rules":[{"id":"x","level":"explode","names":["x"],"paths":[],"requires_running":false,"reason":"restart.reason.x"}]})",
        R"({"version":1,"rules":[{"id":"x","level":"reboot","names":["x"],"paths":[],"requires_running":false,"reason":"untrusted"}]})"}) {
        write(tree.path / "rules.json", bad);
        EXPECT_FALSE(loader.load(tree.path / "rules.json"));
    }
    write(tree.path / "rules.json", std::string(256 * 1024 + 1, 'x'));
    EXPECT_FALSE(loader.load(tree.path / "rules.json"));
}

namespace {
void process(const std::filesystem::path& root, int pid, int parent, int session, int tty,
    std::string_view env, std::string_view exe, std::string_view maps) {
    const auto dir = root / std::to_string(pid);
    write(dir / "status", "Name:\titem\nUid:\t" + std::to_string(getuid()) + "\t" +
        std::to_string(getuid()) + "\nPPid:\t" + std::to_string(parent) + "\n");
    write(dir / "comm", "item\n");
    write(dir / "stat", std::to_string(pid) + " (item) S " + std::to_string(parent) + " 2 " +
        std::to_string(session) + " " + std::to_string(tty) + " 0\n");
    write(dir / "environ", env);
    write(dir / "maps", maps);
    std::filesystem::create_symlink(exe, dir / "exe");
}
}

TEST(Scanner, IdentifiesServicesAppsDeletedMapsAndIncompleteCoverage) {
    Tree tree;
    process(tree.path, 10, 1, 10, 0, "", "/usr/bin/daemon (deleted)",
        "10-20 r-xp 0000 08:01 1 /usr/lib/libdaemon.so (deleted)\n");
    process(tree.path, 11, 1, 11, 1, "", "/usr/bin/application",
        "10-20 r-xp 0000 08:01 2 /usr/lib/libapp.so\n");
    process(tree.path, 12, 1, 12, 0, "XDG_SESSION_ID=42", "/usr/bin/session-app", "");
    process(tree.path, 13, 10, 13, 0, "", "/usr/bin/service-child", "");
    process(tree.path, 14, 11, 11, 0, "", "/usr/bin/app-child", "");
    updates::RunningConsumerScanner scanner(tree.path);
    auto scan = scanner.scan();
    ASSERT_TRUE(scan.coverage_complete);
    ASSERT_EQ(scan.files.at("/usr/lib/libdaemon.so").size(), 1);
    EXPECT_TRUE(scan.files.at("/usr/lib/libdaemon.so").begin()->service);
    EXPECT_FALSE(scan.files.at("/usr/lib/libapp.so").begin()->service);
    EXPECT_FALSE(scan.files.at("/usr/bin/session-app").begin()->service);
    EXPECT_TRUE(scan.files.contains("/usr/bin/daemon"));
    EXPECT_TRUE(scan.files.at("/usr/bin/service-child").begin()->service);
    EXPECT_FALSE(scan.files.at("/usr/bin/app-child").begin()->service);
    ASSERT_TRUE(core::ensure_private_dir(tree.path / "15"));
    scan = scanner.scan();
    EXPECT_FALSE(scan.coverage_complete);
    EXPECT_EQ(scan.unreadable_processes, 1);
}

namespace {
struct FakeFiles : updates::InstalledFilesSource {
    std::expected<std::string, updates::UpdateError> files(std::string_view name, std::string_view) const override {
        if (name == "demo") return "%FILES%\nusr/bin/demo\n\n";
        return std::unexpected(updates::UpdateError{updates::UpdateErrorCode::FileFailure, "missing"});
    }
};
struct FakeScanner : updates::ConsumerScanner {
    updates::ConsumerScan scan() const override {
        return {{{"/usr/bin/demo", {{100, "demo", true}}}}, 0, true};
    }
};
}
TEST(Restart, AdvisorUsesInstalledFilesAndRunningService) {
    FakeFiles files;
    FakeScanner scanner;
    updates::RestartAdvisor advisor(files, scanner);
    braurser::domain::RestartRule rule{"demo-service", braurser::domain::RestartLevel::RestartService,
        {"demo"}, {}, true, "restart.reason.demo"};
    auto report = advisor.advise({{"missing", "1", "2"}, {"demo", "1", "2"}}, {rule});
    ASSERT_EQ(report.packages.size(), 2);
    EXPECT_EQ(report.packages[0].level, braurser::domain::RestartLevel::RestartService);
    EXPECT_EQ(report.packages[1].level, braurser::domain::RestartLevel::Unknown);
    EXPECT_EQ(report.overall, braurser::domain::RestartLevel::RestartService);
    EXPECT_FALSE(report.coverage_complete);
}
