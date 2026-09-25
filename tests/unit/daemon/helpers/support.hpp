#ifndef BRAURSER_TEST_DAEMON_HELPERS_SUPPORT_HPP
#define BRAURSER_TEST_DAEMON_HELPERS_SUPPORT_HPP

#include <braurser/daemon/helpers/aur_helper.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace braurser::daemon::helpers::test {

inline std::filesystem::path fixture(std::string_view name) {
    return std::filesystem::path(BRAURSER_SOURCE_DIR) / "tests/fixtures/helper-argv" / name;
}

inline std::string read_fixture(std::string_view name) {
    std::ifstream file(fixture(name), std::ios::binary);
    if (!file) throw std::runtime_error("fixture unavailable");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* variable = std::getenv("TMPDIR");
        const std::string base = variable != nullptr && *variable == '/' ? std::string(variable) : std::filesystem::temp_directory_path().string();
        std::string pattern = base + "/braurser-helper-XXXXXX";
        char* directory = mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("cannot make temporary directory");
        path_ = directory;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path script(std::string_view filename, std::string_view body) const {
        auto target = path_ / filename;
        std::ofstream output(target, std::ios::binary);
        if (!output) throw std::runtime_error("cannot create test helper");
        output << "#!/usr/bin/bash\n" << body;
        output.close();
        if (chmod(target.c_str(), 0700) != 0) throw std::runtime_error("cannot mark test helper executable");
        return target;
    }

private:
    std::filesystem::path path_;
};

inline HelperEnvironment environment(const std::filesystem::path& root) {
    return {.home = root / "home", .user = "tester", .runtime_dir = root / "runtime",
            .lang = "en_US.UTF-8", .display = ":1", .wayland_display = "wayland-0",
            .xauthority = root / "auth", .makeflags = "-j12"};
}

inline CommunityRequest request(const std::filesystem::path& root, std::string_view helper) {
    return {.name = *domain::PackageName::make("downgrade"),
            .job_dir = root / "home/.cache/braurser/build" / helper / "job-42",
            .shim = "/usr/lib/braurser/shim", .elevation = "sudo-askpass",
            .askpass = "/usr/lib/braurser/askpass", .home = root / "home"};
}

inline std::vector<std::string> installed_args(const CommunityRequest& request, std::string_view helper) {
    if (helper == "paru") {
        return {"--sudo", request.shim.string(), "--nosudoloop", "--skipreview", "--noprovides", "--clonedir",
                request.job_dir.string(), "--noconfirm", "-S", "--needed", "aur/downgrade"};
    }
    return {"--sudo", request.shim.string(), "--answerclean", "None", "--answerdiff", "None",
            "--answeredit", "None", "--builddir", request.job_dir.string(), "--noconfirm", "-S",
            "--needed", "aur/downgrade"};
}

inline core::ProcessResult result(int exit_code, std::string out = {}, std::string err = {}) {
    core::ProcessResult captured;
    captured.exit_code = exit_code;
    captured.out = std::move(out);
    captured.err = std::move(err);
    return captured;
}

inline std::string env_value(const core::ProcessSpec& spec, std::string_view key) {
    for (const auto& [name, value] : spec.env) if (name == key) return value;
    return {};
}

inline bool contains_env(const core::ProcessSpec& spec, std::string_view key) {
    for (const auto& [name, value] : spec.env) if (name == key) return true;
    return false;
}

inline void verify_specs(AURHelper& helper, const std::filesystem::path& root, std::string_view helper_name) {
    const auto install_request = request(root, helper_name);
    const auto install = helper.build_install_spec(install_request);
    ASSERT_TRUE(install.has_value()) << install.error().message;
    EXPECT_EQ(install->args, installed_args(install_request, helper_name));
    EXPECT_EQ(install->timeout, std::chrono::hours(4));
    EXPECT_EQ(install->max_stdout, 8U * 1024 * 1024);
    EXPECT_EQ(install->max_stderr, 8U * 1024 * 1024);
    EXPECT_EQ(install->cwd, std::nullopt);
    EXPECT_EQ(install->stdin_data, std::nullopt);
    EXPECT_EQ(env_value(*install, "PATH"), "/usr/bin");
    EXPECT_EQ(env_value(*install, "HOME"), (root / "home").string());
    EXPECT_EQ(env_value(*install, "USER"), "tester");
    EXPECT_EQ(env_value(*install, "LOGNAME"), "tester");
    EXPECT_EQ(env_value(*install, "LANG"), "en_US.UTF-8");
    EXPECT_EQ(env_value(*install, "TERM"), "dumb");
    EXPECT_EQ(env_value(*install, "NO_COLOR"), "1");
    EXPECT_EQ(env_value(*install, "XDG_RUNTIME_DIR"), (root / "runtime").string());
    EXPECT_EQ(env_value(*install, "DISPLAY"), ":1");
    EXPECT_EQ(env_value(*install, "WAYLAND_DISPLAY"), "wayland-0");
    EXPECT_EQ(env_value(*install, "XAUTHORITY"), (root / "auth").string());
    EXPECT_EQ(env_value(*install, "MAKEFLAGS"), "-j12");
    EXPECT_EQ(env_value(*install, "SUDO_ASKPASS"), "/usr/lib/braurser/askpass");
    EXPECT_EQ(env_value(*install, "BRAURSER_ELEVATION"), "sudo-askpass");
    EXPECT_EQ(env_value(*install, "BRAURSER_JOB_POLICY"), "aur-build");
    EXPECT_EQ(env_value(*install, "BRAURSER_JOB_DIR"), install_request.job_dir.string());
    EXPECT_FALSE(contains_env(*install, "LC_ALL"));
    EXPECT_EQ(install->env.size(), 16U);
    EXPECT_TRUE(std::filesystem::is_directory(install_request.job_dir));
    EXPECT_EQ(std::filesystem::status(install_request.job_dir).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_all);

    const auto updates = helper.build_updates_spec();
    EXPECT_EQ(updates.args, (std::vector<std::string>{"-Qua"}));
    EXPECT_EQ(updates.timeout, std::chrono::seconds(60));
    EXPECT_EQ(updates.env.size(), 12U);
    EXPECT_FALSE(contains_env(updates, "BRAURSER_JOB_DIR"));
    EXPECT_FALSE(contains_env(updates, "SUDO_ASKPASS"));

    const auto printed = helper.build_print_build_file_spec(install_request.name);
    EXPECT_EQ(printed.args, (std::vector<std::string>{"-Gp", "downgrade"}));
    EXPECT_EQ(printed.timeout, std::chrono::seconds(30));
    EXPECT_EQ(printed.max_stdout, 256U * 1024);
    EXPECT_EQ(printed.env.size(), 12U);
}

inline void verify_invalid_request(AURHelper& helper, const std::filesystem::path& root,
                                   std::string_view helper_name) {
    auto candidate = request(root, helper_name);
    candidate.job_dir = root / "outside/job-42";
    EXPECT_FALSE(helper.build_install_spec(candidate));
    candidate.job_dir = root / "home/.cache/braurser/build" /
                        (helper_name == "paru" ? "yay" : "paru") / "job-42";
    EXPECT_FALSE(helper.build_install_spec(candidate));
    candidate.job_dir = root / "home/.cache/braurser/build" / helper_name / "../job-42";
    EXPECT_FALSE(helper.build_install_spec(candidate));
    candidate = request(root, helper_name);
    candidate.elevation = "other";
    EXPECT_FALSE(helper.build_install_spec(candidate));
    candidate = request(root, helper_name);
    candidate.askpass.reset();
    EXPECT_FALSE(helper.build_install_spec(candidate));
    candidate = request(root, helper_name);
    candidate.elevation = "pkexec";
    candidate.askpass.reset();
    const auto permitted = helper.build_install_spec(candidate);
    ASSERT_TRUE(permitted.has_value());
    EXPECT_FALSE(contains_env(*permitted, "SUDO_ASKPASS"));
}

inline void verify_updates(AURHelper& helper, std::string_view helper_name) {
    const auto output = read_fixture("qua-output.txt");
    const auto start = output.find(std::string("=== ") + std::string(helper_name) + " query_aur_updates_pending:");
    ASSERT_NE(start, std::string::npos);
    const auto line_begin = output.find('\n', start) + 1;
    const auto line_end = output.find('\n', line_begin);
    const auto from_fixture = helper.parse_updates(result(0, output.substr(line_begin, line_end - line_begin)));
    ASSERT_TRUE(from_fixture.has_value());
    ASSERT_EQ(from_fixture->size(), 1U);
    EXPECT_EQ(from_fixture->front().name.value(), "downgrade");
    EXPECT_EQ(from_fixture->front().from, "12.0.0-1");
    EXPECT_EQ(from_fixture->front().to, "12.0.3-1");

    auto empty = helper.parse_updates(result(1));
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    EXPECT_FALSE(helper.parse_updates(result(1, "", "error: target not found")));
    EXPECT_FALSE(helper.parse_updates(result(2)));
    const auto versions = helper.parse_updates(result(0,
        "linux-git 1:6.8.r123.abcdef-1 -> 2:6.9.r456.abc123-2  \n"
        "downgrade 12.0.0-1 -> 12.0.3-1\t\n"));
    ASSERT_TRUE(versions.has_value());
    ASSERT_EQ(versions->size(), 2U);
    EXPECT_EQ((*versions)[0].from, "1:6.8.r123.abcdef-1");
    EXPECT_EQ((*versions)[0].to, "2:6.9.r456.abc123-2");
    const auto git = helper.parse_updates(result(0, "example-git r123.abcdef-1 -> r124.bcdef0-1  \n"));
    ASSERT_TRUE(git.has_value());
    EXPECT_EQ(git->front().name.value(), "example-git");
    EXPECT_EQ(git->front().from, "r123.abcdef-1");
    for (const auto bad : {"invalid", "bad 1.0 => 2.0", "bad 1 -> 2 extra", "bad 1 -> 2\n\n",
                           "Bad 1 -> 2", "bad 1 -> 2\nbad 1 -> 3"}) {
        EXPECT_FALSE(helper.parse_updates(result(0, bad))) << bad;
    }
    EXPECT_FALSE(helper.parse_updates(result(0, std::string("bad 1 -> 2\0", 11))));
}

inline void verify_console(AURHelper& helper, std::string_view helper_name,
                           const std::vector<PhaseEvent>& expected, std::size_t expected_prompts) {
    const auto output = read_fixture("console-run3.log");
    const auto marker = std::string("=== ") + std::string(helper_name) + " aur_install_prefixed:";
    const auto start = output.find(marker);
    ASSERT_NE(start, std::string::npos);
    auto position = output.find('\n', start) + 1;
    std::vector<PhaseEvent> phases;
    std::size_t prompts = 0;
    while (position < output.size()) {
        const auto end = output.find('\n', position);
        const auto line = std::string_view(output).substr(position, end - position);
        if (line.starts_with("=== exit=")) break;
        if (line.find("[Y/n]") != std::string_view::npos) ++prompts;
        if (const auto event = helper.classify_line(line)) {
            EXPECT_NE(*event, PhaseEvent::UnexpectedPrompt) << line;
            if (phases.empty() || phases.back() != *event) phases.push_back(*event);
        }
        if (end == std::string::npos) break;
        position = end + 1;
    }
    EXPECT_EQ(prompts, expected_prompts);
    EXPECT_EQ(phases, expected);
    EXPECT_EQ(helper.classify_line(":: Proceed with installation? [Y/n]: "), std::nullopt);
    EXPECT_EQ(helper.classify_line(":: Proceed with installation? [Y/n] "), std::nullopt);
    EXPECT_EQ(helper.classify_line("Enter a number (default=1):"), PhaseEvent::UnexpectedPrompt);
    EXPECT_EQ(helper.classify_line("==> Packages to exclude: (eg: 1-3)"), PhaseEvent::UnexpectedPrompt);
    EXPECT_EQ(helper.classify_line("[y/N]"), PhaseEvent::UnexpectedPrompt);
    EXPECT_EQ(helper.classify_line("==> ERROR: build failed"), PhaseEvent::Error);
}

inline void verify_fake_execution(AURHelper& helper, const std::filesystem::path& root,
                                  std::string_view helper_name) {
    core::PosixProcessRunner runner;
    auto version = helper.version();
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(*version, helper_name == "paru" ? "2.1.0" : "13.0.1");
    auto capabilities = helper.capabilities();
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_EQ(*capabilities, (HelperCapabilities{true, true, true, true}));
    const auto install = helper.build_install_spec(request(root, helper_name));
    ASSERT_TRUE(install.has_value());
    auto executed = runner.run(*install);
    ASSERT_TRUE(executed.has_value());
    EXPECT_EQ(executed->exit_code, 0);
    std::string expected_args;
    for (const auto& arg : install->args) expected_args += arg + "\n";
    EXPECT_EQ(executed->out, expected_args);
    EXPECT_EQ(executed->err, "");
    const auto updates = runner.run(helper.build_updates_spec());
    ASSERT_TRUE(updates.has_value());
    auto parsed = helper.parse_updates(*updates);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->size(), 1U);
    EXPECT_EQ(parsed->front().name.value(), "downgrade");
    const auto printed = runner.run(helper.build_print_build_file_spec(request(root, helper_name).name));
    ASSERT_TRUE(printed.has_value());
    auto build_file = helper.parse_build_file(*printed);
    ASSERT_TRUE(build_file.has_value());
    EXPECT_EQ(*build_file, "pkgname=downgrade\n");
}

inline std::string script_body(std::string_view helper_name) {
    return "if [[ \"$1\" == '--version' ]]; then\n"
           "    printf '" + std::string(helper_name) +
           (helper_name == "paru" ? " v2.1.0 - libalpm v16.0.1\\n'\n" : " v13.0.1 - libalpm v16.0.1\\n'\n") +
           "elif [[ \"$1\" == '-Qua' ]]; then\n"
           "    printf 'downgrade 12.0.0-1 -> 12.0.3-1" +
           std::string(helper_name == "yay" ? " [1d5h]" : "") + "\\n'\n"
           "elif [[ \"$1\" == '-Gp' ]]; then\n"
           "    printf 'pkgname=downgrade\\n'\n"
           "else\n"
           "    printf '%s\\n' \"$@\"\n"
           "fi\n";
}

}

#endif
