#include "support.hpp"

namespace braurser::daemon::helpers::test {

TEST(Yay, ExactSpecsAndEnvironment) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    YayHelper helper(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    verify_specs(helper, temp.path(), "yay");
}

TEST(Yay, ConfinesJobAndElevation) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    YayHelper helper(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    verify_invalid_request(helper, temp.path(), "yay");
}

TEST(Yay, ParsesCapturedAndSyntheticUpdates) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    YayHelper helper(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    verify_updates(helper, "yay");
    auto aged = helper.parse_updates(result(0, "downgrade 1:12.0-1 -> 2:12.0-2 [1d5h]   \n"));
    ASSERT_TRUE(aged.has_value());
    EXPECT_EQ(aged->front().to, "2:12.0-2");
    EXPECT_FALSE(helper.parse_updates(result(0, "downgrade 1 -> 2 [!malformed]")));
}

TEST(Yay, CapturedConsolePhaseSequence) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    YayHelper helper(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    verify_console(helper, "yay", {PhaseEvent::Resolving, PhaseEvent::Building,
                                    PhaseEvent::Downloading, PhaseEvent::Installing,
                                    PhaseEvent::Finishing, PhaseEvent::Building,
                                    PhaseEvent::Downloading, PhaseEvent::Building,
                                    PhaseEvent::Installing, PhaseEvent::Finishing}, 2);
    EXPECT_EQ(helper.classify_line("\x1b[1m==> ERROR: failed\x1b[0m"), PhaseEvent::Error);
}

TEST(Yay, RunnerExecutesFakeHelperSpecs) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    YayHelper helper(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    verify_fake_execution(helper, temp.path(), "yay");
}

TEST(Yay, RejectsMissingOrSymlinkedBuildDir) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    YayHelper helper(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    const auto candidate = request(temp.path(), "yay");
    auto created = helper.build_install_spec(candidate);
    ASSERT_TRUE(created.has_value());
    std::filesystem::remove(candidate.job_dir);
    std::filesystem::create_directory_symlink(temp.path(), candidate.job_dir);
    EXPECT_FALSE(helper.build_install_spec(candidate));
}

}
