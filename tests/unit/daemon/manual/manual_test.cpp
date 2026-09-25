#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/process.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace {
using namespace braurser::daemon::manual;

class Temp {
public:
    Temp() {
        auto pattern = (std::filesystem::temp_directory_path() / "braurser-manual-XXXXXX").string();
        auto* directory = mkdtemp(pattern.data());
        if (directory) path = directory;
    }
    ~Temp() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

Receipt example_receipt() {
    Receipt receipt;
    receipt.id = "test-receipt";
    receipt.name = "Demo";
    receipt.url = "https://example.org/owner/demo.git";
    receipt.host = "example.org";
    receipt.commit = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    receipt.entry_point = "install.sh";
    return receipt;
}

TEST(ManualPackage, RendersFixedTemplate) {
    auto rendered = render_pkgbuild(example_receipt(), std::chrono::sys_seconds{std::chrono::seconds{0}}, "any");
    ASSERT_TRUE(rendered.has_value());
    EXPECT_EQ(rendered->first, "braurser-git-demo");
    std::ifstream file(std::filesystem::path(MANUAL_FIXTURES) / "PKGBUILD.golden");
    ASSERT_TRUE(file.is_open());
    const std::string golden(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    EXPECT_EQ(rendered->second, golden);
    auto request = system_remove_request("braurser-git-demo");
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->args, (std::vector<std::string>{"--remove", "--nosave", "--recursive", "--", "braurser-git-demo"}));
}

TEST(ManualPackage, RecordsSystemPackageWithoutRunningPacman) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto state = temp.path / "state";
    std::filesystem::create_directory(state);
    auto receipt = example_receipt();
    receipt.scope = Scope::System;
    receipt.pkgname = "braurser-git-demo";
    receipt.files.push_back({"bin/demo", std::string(64, 'a'), 0755});
    auto recorded = record_system_install(state, receipt);
    ASSERT_TRUE(recorded.has_value()) << recorded.error().code;
    std::ifstream file(state / "braurser/manual/receipts/test-receipt.json");
    ASSERT_TRUE(file.is_open());
    const std::string bytes(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    EXPECT_NE(bytes.find("\"pkgname\":\"braurser-git-demo\""), std::string::npos);
    EXPECT_NE(bytes.find("\"scope\":\"system\""), std::string::npos);
}

TEST(ManualStage, RejectsUnknownPathsAndPrivilegeBits) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto stage = temp.path / "stage";
    std::filesystem::create_directories(stage / "bin");
    std::ofstream(stage / "bin/demo") << "ok";
    ASSERT_TRUE(validate_stage(stage, Scope::User).has_value());
    std::ofstream(stage / "wrong") << "bad";
    auto unknown = validate_stage(stage, Scope::User);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().code, "stage-refused");
    std::filesystem::remove(stage / "wrong");
    ASSERT_EQ(chmod((stage / "bin/demo").c_str(), 04755), 0);
    auto privileged = validate_stage(stage, Scope::User);
    ASSERT_FALSE(privileged.has_value());
    EXPECT_EQ(privileged.error().code, "stage-refused");
}

TEST(ManualStage, RejectsSymlinksOutsideOrMissingFromTree) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto stage = temp.path / "stage";
    std::filesystem::create_directories(stage / "bin");
    std::ofstream(stage / "bin/demo") << "demo";
    for (auto target : {"/etc/passwd", "../../outside", "not-in-stage"}) {
        auto link = stage / "bin/link";
        std::filesystem::create_symlink(target, link);
        auto result = validate_stage(stage, Scope::User);
        ASSERT_FALSE(result.has_value()) << target;
        EXPECT_EQ(result.error().code, "stage-refused");
        std::filesystem::remove(link);
    }
}

TEST(ManualStage, RefusesMalformedDesktopAndEntityXml) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto stage = temp.path / "stage";
    std::filesystem::create_directories(stage / "bin");
    std::filesystem::create_directories(stage / "share/applications");
    std::filesystem::create_directories(stage / "share/mime/packages");
    std::ofstream(stage / "bin/demo") << "demo";
    std::ofstream(stage / "share/applications/bad.desktop") << "not a desktop entry\n";
    auto desktop = validate_stage(stage, Scope::User);
    ASSERT_FALSE(desktop.has_value());
    EXPECT_EQ(desktop.error().code, "stage-refused");
    std::filesystem::remove(stage / "share/applications/bad.desktop");
    std::ofstream(stage / "share/mime/packages/unsafe.xml") <<
        "<!DOCTYPE root [<!ENTITY local 'foo'>]><root>&local;</root>";
    auto xml = validate_stage(stage, Scope::User);
    ASSERT_FALSE(xml.has_value());
    EXPECT_EQ(xml.error().code, "stage-refused");
}

TEST(ManualInstall, RestoresPreexistingFileOnRemoval) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto home = temp.path / "home";
    auto state = temp.path / "state";
    auto stage = temp.path / "stage";
    std::filesystem::create_directories(home / ".local/bin");
    std::filesystem::create_directories(stage / "bin");
    std::filesystem::create_directory(state);
    std::ofstream(home / ".local/bin/demo") << "original";
    std::ofstream(stage / "bin/demo") << "installed";
    braurser::daemon::core::PosixProcessRunner runner;
    auto installed = install_user(runner, stage, home, state, example_receipt());
    ASSERT_TRUE(installed.has_value()) << installed.error().code;
    std::ifstream input(home / ".local/bin/demo");
    std::string content;
    input >> content;
    EXPECT_EQ(content, "installed");
    auto removed = remove_user(runner, home, state, *installed);
    ASSERT_TRUE(removed.has_value()) << removed.error().code;
    EXPECT_TRUE(removed->modified_files.empty());
    std::ifstream restored(home / ".local/bin/demo");
    restored >> content;
    EXPECT_EQ(content, "original");
}

TEST(ManualRemove, ReportsModifiedFilesWithoutDeletingThem) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto home = temp.path / "home";
    auto state = temp.path / "state";
    auto stage = temp.path / "stage";
    std::filesystem::create_directories(home / ".local/bin");
    std::filesystem::create_directories(stage / "bin");
    std::filesystem::create_directory(state);
    std::ofstream(stage / "bin/demo") << "installed";
    braurser::daemon::core::PosixProcessRunner runner;
    auto installed = install_user(runner, stage, home, state, example_receipt());
    ASSERT_TRUE(installed.has_value()) << installed.error().code;
    std::ofstream(home / ".local/bin/demo") << "locally changed";
    auto removed = remove_user(runner, home, state, *installed);
    ASSERT_TRUE(removed.has_value()) << removed.error().code;
    EXPECT_EQ(removed->modified_files, (std::vector<std::string>{"bin/demo"}));
    EXPECT_TRUE(std::filesystem::exists(home / ".local/bin/demo"));
    EXPECT_TRUE(std::filesystem::exists(state / "braurser/manual/receipts/test-receipt.json"));
}

TEST(ManualUpgrade, ReplacesFilesAndPreservesOriginalBackup) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto home = temp.path / "home";
    auto state = temp.path / "state";
    auto stage = temp.path / "stage";
    std::filesystem::create_directories(home / ".local/bin");
    std::filesystem::create_directories(stage / "bin");
    std::filesystem::create_directory(state);
    std::ofstream(home / ".local/bin/demo") << "original";
    std::ofstream(stage / "bin/demo") << "old";
    braurser::daemon::core::PosixProcessRunner runner;
    auto old = install_user(runner, stage, home, state, example_receipt());
    ASSERT_TRUE(old.has_value()) << old.error().code;
    std::ofstream(stage / "bin/demo") << "new";
    auto next = example_receipt();
    next.commit = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    auto upgraded = upgrade_user(runner, stage, home, state, *old, next);
    ASSERT_TRUE(upgraded.has_value()) << upgraded.error().code;
    std::ifstream upgraded_file(home / ".local/bin/demo");
    std::string value;
    upgraded_file >> value;
    EXPECT_EQ(value, "new");
    auto removed = remove_user(runner, home, state, *upgraded);
    ASSERT_TRUE(removed.has_value()) << removed.error().code;
    std::ifstream restored(home / ".local/bin/demo");
    restored >> value;
    EXPECT_EQ(value, "original");
}

TEST(ManualInstall, InstallsAndRemovesRelativeSymlinks) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto home = temp.path / "home";
    auto state = temp.path / "state";
    auto stage = temp.path / "stage";
    std::filesystem::create_directory(home);
    std::filesystem::create_directory(state);
    std::filesystem::create_directories(stage / "bin");
    std::ofstream(stage / "bin/demo") << "demo";
    std::filesystem::create_symlink("demo", stage / "bin/alias");
    braurser::daemon::core::PosixProcessRunner runner;
    auto installed = install_user(runner, stage, home, state, example_receipt());
    ASSERT_TRUE(installed.has_value()) << installed.error().code;
    EXPECT_EQ(std::filesystem::read_symlink(home / ".local/bin/alias"), "demo");
    auto removed = remove_user(runner, home, state, *installed);
    ASSERT_TRUE(removed.has_value()) << removed.error().code;
    EXPECT_FALSE(std::filesystem::exists(home / ".local/bin/alias"));
}

TEST(ManualStaging, RunsScriptWithoutHostWrites) {
    if (!std::filesystem::exists("/usr/bin/bwrap")) GTEST_SKIP();
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto src = temp.path / "src";
    auto stage = temp.path / "stage";
    auto home = temp.path / "home";
    std::filesystem::create_directory(src);
    std::filesystem::create_directory(stage);
    std::filesystem::create_directory(home);
    std::ofstream(src / "install.sh") << "mkdir -p \"$PREFIX/bin\"\nprintf demo > \"$PREFIX/bin/demo\"\n";
    braurser::daemon::core::PosixProcessRunner runner;
    auto output = run_staging(runner, src, stage, home, EntryKind::Script, Scope::User);
    ASSERT_TRUE(output.has_value()) << output.error().code << " " << output.error().detail;
    EXPECT_TRUE(std::filesystem::is_regular_file(stage / "bin/demo"));
}

TEST(ManualStaging, RefusesHostWrite) {
    if (!std::filesystem::exists("/usr/bin/bwrap")) GTEST_SKIP();
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto src = temp.path / "src";
    auto stage = temp.path / "stage";
    auto home = temp.path / "home";
    std::filesystem::create_directory(src);
    std::filesystem::create_directory(stage);
    std::filesystem::create_directory(home);
    std::ofstream(src / "install.sh") << "printf bad > /usr/bin/braurser-sandbox-test\n";
    braurser::daemon::core::PosixProcessRunner runner;
    auto output = run_staging(runner, src, stage, home, EntryKind::Script, Scope::User);
    ASSERT_FALSE(output.has_value());
    EXPECT_EQ(output.error().code, "staging-failed");
    EXPECT_FALSE(std::filesystem::exists("/usr/bin/braurser-sandbox-test"));
}

TEST(ManualStaging, RejectsSetuidOutput) {
    if (!std::filesystem::exists("/usr/bin/bwrap")) GTEST_SKIP();
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto src = temp.path / "src";
    auto stage = temp.path / "stage";
    auto home = temp.path / "home";
    std::filesystem::create_directory(src);
    std::filesystem::create_directory(stage);
    std::filesystem::create_directory(home);
    std::ofstream(src / "install.sh") <<
        "mkdir -p \"$PREFIX/bin\"\nprintf demo > \"$PREFIX/bin/demo\"\n"
        "chmod 4755 \"$PREFIX/bin/demo\"\n";
    braurser::daemon::core::PosixProcessRunner runner;
    auto output = run_staging(runner, src, stage, home, EntryKind::Script, Scope::User);
    ASSERT_FALSE(output.has_value());
    EXPECT_EQ(output.error().code, "stage-refused");
}

TEST(ManualPackage, DISABLED_MakepkgBuild) {
    Temp temp;
    ASSERT_FALSE(temp.path.empty());
    auto stage = temp.path / "stage";
    auto home = temp.path / "home";
    std::filesystem::create_directories(stage / "bin");
    std::filesystem::create_directory(home);
    std::ofstream(stage / "bin/demo") << "demo";
    braurser::daemon::core::PosixProcessRunner runner;
    auto built = build_system_package(runner, stage, home, home / ".cache/braurser/build/manual/testjob",
        example_receipt(), std::chrono::sys_seconds{std::chrono::seconds{0}});
    ASSERT_TRUE(built.has_value()) << built.error().code << " " << built.error().detail;
}

}
