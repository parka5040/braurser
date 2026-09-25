#include <braurser/daemon/catalog/sources.hpp>

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace braurser::daemon::catalog {
namespace {

TEST(LocalQueryCompute, VersionedProvidesAndRepositoryPriority) {
    LocalRecord provider;
    provider.package.name = "provider";
    provider.package.version = "1-1";
    provider.package.reason = domain::InstallReason::Dependency;
    provider.provides = {"virtual=2"};
    LocalRecord consumer;
    consumer.package.name = "consumer";
    consumer.package.version = "1-1";
    consumer.depends = {"virtual>=2"};
    LocalRecord unused;
    unused.package.name = "unused";
    unused.package.version = "1-1";
    unused.package.reason = domain::InstallReason::Dependency;
    RepoPackage first;
    first.name = "provider";
    first.version = "1-1";
    RepoPackage second = first;
    second.version = "3-1";
    const std::vector<LocalRecord> local{provider, consumer, unused};
    const std::vector<RepoPackage> repos{first, second};
    auto sets = LocalQuery{}.compute(local, repos);
    EXPECT_EQ(sets.orphans, (std::vector<std::string>{"unused"}));
    EXPECT_EQ(sets.native, (std::vector<std::string>{"provider"}));
    EXPECT_EQ(sets.foreign, (std::vector<std::string>{"consumer", "unused"}));
    EXPECT_TRUE(sets.upgradable.empty());
}

TEST(LocalQueryParity, MatchesPacmanQuerySetsOnHost) {
    if (!std::filesystem::exists("/usr/bin/pacman") || !std::filesystem::exists("/usr/bin/pacman-conf"))
        GTEST_SKIP() << "pacman is unavailable";
    core::PosixProcessRunner runner;
    auto configured = PacmanConfig(runner).read();
    ASSERT_TRUE(configured) << configured.error().code;
    auto local = LocalDbReader{}.read(configured->db_path);
    ASSERT_TRUE(local) << local.error().code;
    std::vector<RepoPackage> packages;
    for (const auto& repo : configured->names) {
        auto read = SyncDbReader{}.read(configured->db_path, repo);
        ASSERT_TRUE(read) << read.error().code;
        packages.insert(packages.end(), std::make_move_iterator(read->packages.begin()),
                        std::make_move_iterator(read->packages.end()));
    }
    const auto sets = LocalQuery{}.compute(*local, packages);
    const std::array<std::pair<std::string, const std::vector<std::string>*>, 6> tests{{
        {"-Qq", &sets.installed}, {"-Qqe", &sets.explicit_packages},
        {"-Qqd", &sets.dependencies}, {"-Qqdt", &sets.orphans},
        {"-Qqm", &sets.foreign}, {"-Qqn", &sets.native}
    }};
    for (const auto& [flag, expected] : tests) {
        core::ProcessSpec spec;
        spec.executable = "/usr/bin/pacman";
        spec.args = {flag};
        spec.env = {{"LC_ALL", "C"}};
        spec.timeout = std::chrono::seconds(30);
        spec.max_stdout = 16 * 1024 * 1024;
        spec.max_stderr = 64 * 1024;
        auto output = runner.run(spec);
        ASSERT_TRUE(output);
        ASSERT_EQ(output->exit_code, 0) << flag;
        std::vector<std::string> actual;
        for (std::size_t start = 0; start < output->out.size();) {
            auto end = output->out.find('\n', start);
            if (end == output->out.npos) end = output->out.size();
            if (end > start) actual.push_back(output->out.substr(start, end - start));
            start = end == output->out.size() ? output->out.size() : end + 1;
        }
        std::ranges::sort(actual);
        EXPECT_EQ(*expected, actual) << flag;
    }
}

}
}
