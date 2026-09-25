#include "support.hpp"

namespace braurser::daemon::helpers::test {

TEST(Paru, ExactSpecsAndEnvironment) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper helper(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    verify_specs(helper, temp.path(), "paru");
}

TEST(Paru, ConfinesJobAndElevation) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper helper(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    verify_invalid_request(helper, temp.path(), "paru");
}

TEST(Paru, ParsesCapturedAndSyntheticUpdates) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper helper(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    verify_updates(helper, "paru");
    EXPECT_FALSE(helper.parse_updates(result(0, "downgrade 1 -> 2 [1d5h]")));
}

TEST(Paru, CapturedConsolePhaseSequence) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper helper(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    verify_console(helper, "paru", {PhaseEvent::Resolving, PhaseEvent::Downloading,
                                     PhaseEvent::Installing, PhaseEvent::Finishing,
                                     PhaseEvent::Building, PhaseEvent::Downloading,
                                     PhaseEvent::Building, PhaseEvent::Downloading,
                                     PhaseEvent::Building, PhaseEvent::Installing,
                                     PhaseEvent::Finishing}, 3);
    const auto other = read_fixture("console-run3.log");
    const auto section = other.find("=== paru aur_install_provided_name:");
    ASSERT_NE(section, std::string::npos);
    const auto prompt = other.find("Enter a number (default=1):", section);
    ASSERT_NE(prompt, std::string::npos);
    EXPECT_EQ(helper.classify_line(std::string_view(other).substr(prompt, other.find('\n', prompt) - prompt)),
              PhaseEvent::UnexpectedPrompt);
}

TEST(Paru, RunnerExecutesFakeHelperSpecs) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper helper(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    verify_fake_execution(helper, temp.path(), "paru");
}

TEST(Paru, RejectsInvalidBuildFileAndVersion) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper helper(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    EXPECT_FALSE(helper.parse_build_file(result(0, std::string(256 * 1024 + 1, 'a'))));
    EXPECT_FALSE(helper.parse_build_file(result(0, std::string("a\0b", 3))));
    EXPECT_FALSE(helper.parse_build_file(result(1, "pkgname=a")));
    ParuHelper unsupported(environment(temp.path()), runner,
                            temp.script("unsupported", "printf 'paru v1.0.0 - libalpm v16.0.1\\n'\n"));
    auto features = unsupported.capabilities();
    ASSERT_FALSE(features.has_value());
    EXPECT_EQ(features.error().code, HelperErrorCode::UnsupportedVersion);
}

}
