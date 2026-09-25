#include "support.hpp"
#include <tuple>

namespace braurser::daemon::helpers::test {

TEST(HelperParity, IdenticalSupportedCapabilities) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper paru(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    YayHelper yay(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    const auto paru_features = paru.capabilities();
    const auto yay_features = yay.capabilities();
    ASSERT_TRUE(paru_features.has_value());
    ASSERT_TRUE(yay_features.has_value());
    EXPECT_EQ(*paru_features, *yay_features);
    EXPECT_EQ(*paru_features, (HelperCapabilities{true, true, true, true}));
}

TEST(HelperParity, LocatorHonorsAutoAndExplicitPreference) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    HelperLocator locator(temp.path());
    EXPECT_FALSE(locator.locate("auto"));
    EXPECT_FALSE(locator.locate("unknown"));
    const auto yay_path = temp.script("yay", script_body("yay"));
    EXPECT_EQ(locator.locate("auto"), yay_path);
    EXPECT_FALSE(locator.locate("paru"));
    const auto paru_path = temp.script("paru", script_body("paru"));
    EXPECT_EQ(locator.locate("auto"), paru_path);
    EXPECT_EQ(locator.locate("yay"), yay_path);
    auto selected = locator.select("yay", environment(temp.path()), runner);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ((*selected)->name(), "yay");
    auto automatic = locator.select("auto", environment(temp.path()), runner);
    ASSERT_TRUE(automatic.has_value());
    EXPECT_EQ((*automatic)->name(), "paru");
}

TEST(HelperParity, EnvironmentRejectsUnsafeValuesAndDropsInvalidMakeflags) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    const auto binary = temp.script("paru", script_body("paru"));
    auto invalid = environment(temp.path());
    invalid.user = "test\nPATH=/usr/bin";
    EXPECT_THROW(ParuHelper(invalid, runner, binary), std::invalid_argument);
    invalid = environment(temp.path());
    invalid.lang = "en_US.UTF-8\nEVIL=1";
    EXPECT_THROW(YayHelper(invalid, runner, binary), std::invalid_argument);
    invalid = environment(temp.path());
    invalid.makeflags = "-j12 -f /etc/passwd";
    ParuHelper helper(invalid, runner, binary);
    auto spec = helper.build_updates_spec();
    EXPECT_FALSE(contains_env(spec, "MAKEFLAGS"));
}

TEST(HelperParity, ClassifiesAllCapturedConsoleLinesAndRecognizesOnlyKnownPrompts) {
    TemporaryDirectory temp;
    core::PosixProcessRunner runner;
    ParuHelper paru(environment(temp.path()), runner, temp.script("paru", script_body("paru")));
    YayHelper yay(environment(temp.path()), runner, temp.script("yay", script_body("yay")));
    for (const auto& [filename, expected_removal, expected_provider] :
         {std::tuple{"console-run2.log", 3U, 0U}, std::tuple{"console-run3.log", 0U, 1U}}) {
        const auto log = read_fixture(filename);
        std::size_t confirmed = 0;
        std::size_t excluded = 0;
        std::size_t removal = 0;
        std::size_t provider = 0;
        for (std::size_t position = 0; position < log.size();) {
            const auto end = log.find('\n', position);
            const auto line = std::string_view(log).substr(position, end - position);
            const bool is_confirmation = line.find("Proceed with installation? [Y/n]") != std::string_view::npos;
            const bool is_removal = line.find("Do you want to remove these packages? [Y/n]") != std::string_view::npos;
            const bool is_exclusion = line.find("==> Packages to exclude:") != std::string_view::npos;
            const bool is_provider = line.find("Enter a number (default=1):") != std::string_view::npos;
            for (AURHelper* helper : {static_cast<AURHelper*>(&paru), static_cast<AURHelper*>(&yay)}) {
                EXPECT_EQ(helper->classify_line(line) == PhaseEvent::UnexpectedPrompt,
                          is_removal || is_exclusion || is_provider) << filename << ": " << line;
            }
            confirmed += static_cast<std::size_t>(is_confirmation);
            excluded += static_cast<std::size_t>(is_exclusion);
            removal += static_cast<std::size_t>(is_removal);
            provider += static_cast<std::size_t>(is_provider);
            if (end == std::string::npos) break;
            position = end + 1;
        }
        EXPECT_GT(confirmed, 0U);
        EXPECT_EQ(excluded, 1U);
        EXPECT_EQ(removal, expected_removal);
        EXPECT_EQ(provider, expected_provider);
    }
}

}
