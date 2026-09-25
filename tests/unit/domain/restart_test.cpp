#include <braurser/domain/restart.hpp>
#include <gtest/gtest.h>
#include <array>

namespace braurser::domain {
namespace {

PackageChange change(std::string_view name, std::vector<std::string> files = {},
                     bool running = false, bool service = false) {
    return {*PackageName::make(name), std::move(files), running, service};
}

RestartRule rule(std::string id, RestartLevel level, std::vector<std::string> names,
                 std::vector<std::string> paths = {}, bool running = false) {
    return {id, level, std::move(names), std::move(paths), running, "restart.reason." + id};
}

}

TEST(Restart, HighestSeverityWinsRegardlessOfOrder) {
    const std::array rules{rule("reboot", RestartLevel::Reboot, {"linux", "linux-*"}),
                           rule("none", RestartLevel::None, {"*-docs"}),
                           rule("logout", RestartLevel::Logout, {"linux"})};
    const auto advice = classify(change("linux"), rules);
    EXPECT_EQ(advice.level, RestartLevel::Reboot);
    EXPECT_EQ(advice.rule_id, "reboot");
    EXPECT_EQ(advice.reason_key, "restart.reason.reboot");
    EXPECT_EQ(classify(change("font-docs"), rules).level, RestartLevel::None);
    EXPECT_EQ(classify(change("unmatched"), rules).level, RestartLevel::Unknown);
}

TEST(Restart, GlobAnchoringAndQuestionMark) {
    const std::array rules{rule("glob", RestartLevel::Logout, {"x?z", "a*b"})};
    EXPECT_EQ(classify(change("xyz"), rules).level, RestartLevel::Logout);
    EXPECT_EQ(classify(change("ab"), rules).level, RestartLevel::Logout);
    EXPECT_EQ(classify(change("axxxxxb"), rules).level, RestartLevel::Logout);
    EXPECT_EQ(classify(change("wxyz"), rules).level, RestartLevel::Unknown);
    EXPECT_EQ(classify(change("az"), rules).level, RestartLevel::Unknown);
    EXPECT_EQ(classify(change("abz"), rules).level, RestartLevel::Unknown);
}

TEST(Restart, FilePrefixAndRunningConsumer) {
    const std::array rules{rule("service", RestartLevel::RestartService, {}, {"/etc/init.d/"}, true),
                           rule("app", RestartLevel::RestartApp, {}, {"/usr/share/applications/"}, true)};
    EXPECT_EQ(classify(change("foo", {"/etc/init.d/foo"}), rules).level, RestartLevel::Unknown);
    EXPECT_EQ(classify(change("foo", {"/etc/init.d/foo"}, true), rules).level, RestartLevel::RestartApp);
    EXPECT_EQ(classify(change("foo", {"/etc/init.d/foo"}, true, true), rules).level,
              RestartLevel::RestartService);
    EXPECT_EQ(classify(change("foo", {"/etc/init.dish/foo"}, true, true), rules).level,
              RestartLevel::Unknown);
    EXPECT_EQ(classify(change("foo", {"/usr/share/applications/foo.desktop"}, true), rules).level,
              RestartLevel::RestartApp);
}

TEST(Restart, OverallKeepsKnownHighSeverity) {
    const std::array rules{rule("none", RestartLevel::None, {"font"}),
                           rule("logout", RestartLevel::Logout, {"mesa"})};
    const std::array advices{classify(change("font"), rules), classify(change("mesa"), rules),
                             classify(change("unknown"), rules)};
    EXPECT_EQ(overall(advices), RestartLevel::Logout);
    const std::array only_none{classify(change("font"), rules)};
    EXPECT_EQ(overall(only_none), RestartLevel::None);
    const std::array with_unknown{classify(change("font"), rules), classify(change("unknown"), rules)};
    EXPECT_EQ(overall(with_unknown), RestartLevel::Unknown);
    EXPECT_EQ(overall({}), RestartLevel::Unknown);
}

}
