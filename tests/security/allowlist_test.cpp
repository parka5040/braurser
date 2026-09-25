#include "../../src/elevate/normalize.hpp"

#include <gtest/gtest.h>
#include <array>
#include <fstream>
#include <sstream>

namespace {
using braurser::elevate::normalize;

TEST(AllowlistFixtures, EveryCapturedParuAndYayInvocation) {
    constexpr std::array paru{
        'A','A','E','D','E','A','A','B','B','C','C','E','V','A','A','A','A','E','D','E','D','E','D','D'
    };
    constexpr std::array yay{
        'A','E','A','E','A','E','X','X','B','E','C','C','E','V','V','A','E','A','E','A','E','A','E','D','E','D','E','D','E','D','E'
    };
    for (auto fixture : {"paru", "yay"}) {
        std::ifstream file(std::string(BRAURSER_FIXTURES) + "/" + fixture + ".argv");
        ASSERT_TRUE(file.good());
        std::string line;
        size_t index = 0;
        while (std::getline(file, line)) {
            auto expected = fixture == std::string("paru") ? paru.at(index) : yay.at(index);
            auto pos = line.find("ARGV=SHIM ");
            ASSERT_NE(pos, std::string::npos) << line;
            std::istringstream stream(line.substr(pos + 10));
            std::vector<std::string> args;
            std::string arg;
            while (stream >> arg) args.push_back(arg);
            if (expected == 'V') {
                EXPECT_EQ(args, std::vector<std::string>{"-v"}) << line;
            } else {
                auto normalized = normalize(args);
                EXPECT_EQ(normalized.operation, expected == 'X' ? 0 : expected) << line << " " << normalized.reason;
            }
            ++index;
        }
        EXPECT_EQ(index, fixture == std::string("paru") ? paru.size() : yay.size());
    }
}

TEST(AllowlistNormalization, RejectsDangerousOptionsAndTargets) {
    constexpr std::array rejected{
        "--root", "--dbpath", "--config", "--cachedir", "--hookdir", "--gpgdir",
        "--logfile", "--overwrite", "--assume-installed", "--dbonly", "--sysroot",
        "--ignore", "--ignoregroup", "--print", "--arch", "--disable-download-timeout",
        "-d", "--nodeps", "-dd", "-w", "--downloadonly"
    };
    for (auto flag : rejected) {
        auto result = normalize(std::vector<std::string>{"pacman", "-S", "--needed", flag, "--", "tree"});
        EXPECT_FALSE(result) << flag;
    }
    for (auto target : {"--root", "", "Foo", "white space", ".hidden", "../escape", "repo//tree",
                        "BAD!/tree", "-tree", "https://host/archive", "extra/BAD"}) {
        auto result = normalize(std::vector<std::string>{"pacman", "-S", "--needed", "--", target});
        EXPECT_FALSE(result) << target;
    }
    EXPECT_FALSE(normalize(std::vector<std::string>{"pacman", "-S", "-y", "--"}));
    EXPECT_FALSE(normalize(std::vector<std::string>{"pacman", "-S", "--needed", "--", "tree", "--"}));
    EXPECT_FALSE(normalize(std::vector<std::string>{"pacman", "-S", "--needed", "--config", "/tmp/pacman.conf", "--", "tree"}));
    EXPECT_FALSE(normalize(std::vector<std::string>{"pacman", "-S", "--needed", "--color", "always", "--", "tree"}));
    EXPECT_FALSE(normalize(std::vector<std::string>{"pacman", "-S", "--needed", "--noprogressbar", "--noprogressbar", "--", "tree"}));
}

TEST(AllowlistNormalization, CommunityPolicyRejectsOfficialTransactionsAndOtherJobs) {
    constexpr auto job = "/home/builder/.cache/braurser/build/paru/job1";
    auto update = normalize(std::vector<std::string>{"pacman", "-S", "-y", "-u", "--"});
    ASSERT_TRUE(update);
    EXPECT_FALSE(braurser::elevate::allowed_for_job(update, job));
    auto remove = normalize(std::vector<std::string>{"pacman", "-R", "-n", "-s", "--", "tree"});
    ASSERT_TRUE(remove);
    EXPECT_FALSE(braurser::elevate::allowed_for_job(remove, job));
    auto own = normalize(std::vector<std::string>{"pacman", "-U", "--",
        std::string(job) + "/pkg/pkg-1-1-any.pkg.tar.zst"});
    ASSERT_TRUE(own);
    EXPECT_TRUE(braurser::elevate::allowed_for_job(own, job));
    auto other = normalize(std::vector<std::string>{"pacman", "-U", "--",
        "/home/builder/.cache/braurser/build/paru/job2/pkg/pkg-1-1-any.pkg.tar.zst"});
    ASSERT_TRUE(other);
    EXPECT_FALSE(braurser::elevate::allowed_for_job(other, job));
}

TEST(AllowlistNormalization, CanonicalizesObservedFlags) {
    auto result = normalize(std::vector<std::string>{"/usr/bin/pacman", "--color", "never", "-S",
        "--noconfirm", "--needed", "--noconfirm", "--config", "/etc/pacman.conf", "--", "extra/tree"});
    ASSERT_TRUE(result) << result.reason;
    EXPECT_EQ(result.args, (std::vector<std::string>{"--sync", "--needed", "--noconfirm", "--color=never", "--", "extra/tree"}));
}
}
