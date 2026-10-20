#include <braurser/daemon/manual/manual.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <optional>
#include <set>
#include <unistd.h>

namespace {
using namespace braurser::daemon::manual;

class Environment {
public:
    Environment(const char* name, const std::filesystem::path& value) : name_(name) {
        if (const auto* old = std::getenv(name)) previous_ = old;
        setenv(name, value.c_str(), 1);
    }
    ~Environment() {
        if (previous_) setenv(name_.c_str(), previous_->c_str(), 1);
        else unsetenv(name_.c_str());
    }
private:
    std::string name_;
    std::optional<std::string> previous_;
};

class Fixture {
public:
    Fixture() {
        auto pattern = (std::filesystem::temp_directory_path() / "braurser-launcher-XXXXXX").string();
        if (const auto* raw = mkdtemp(pattern.data())) path = raw;
    }
    ~Fixture() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

class ConfigRunner final : public braurser::daemon::core::ProcessRunner {
public:
    explicit ConfigRunner(std::filesystem::path db) : db_(std::move(db)) {}
    std::expected<braurser::daemon::core::ProcessResult, braurser::daemon::core::ProcessError> run(
        const braurser::daemon::core::ProcessSpec& spec) override {
        braurser::daemon::core::ProcessResult result;
        result.exit_code = 0;
        result.out = spec.args == std::vector<std::string>{"DBPath"} ? db_.string() + "\n" : "core\n";
        return result;
    }
private:
    std::filesystem::path db_;
};

void package(const std::filesystem::path& db, std::string_view name, std::string_view files) {
    const auto dir = db / "local" / (std::string(name) + "-1");
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "desc") << "%NAME%\n" << name << "\n\n%VERSION%\n1\n\n";
    std::ofstream(dir / "files") << "%FILES%\n" << files << "\n";
}

TEST(LauncherScan, SystemOwnershipFiltersRelevantPackagesAndReceipts) {
    Fixture fixture;
    ASSERT_FALSE(fixture.path.empty());
    const auto home = fixture.path / "home";
    const auto data = home / ".local/share";
    const auto shared = fixture.path / "shared";
    const auto state = fixture.path / "state";
    std::filesystem::create_directories(data / "applications");
    std::filesystem::create_directories(shared / "applications");
    std::filesystem::create_directories(state / "braurser/manual/receipts");
    const std::string desktop = "[Desktop Entry]\nType=Application\nName=Demo\nExec=demo\n";
    std::ofstream(data / "applications/receipt.desktop") << desktop;
    std::ofstream(shared / "applications/package.desktop") << desktop;
    std::ofstream(data / "applications/visible.desktop") << desktop;
    package(fixture.path / "db", "demo", "usr/lib/unrelated.so\n" +
        (shared / "applications/package.desktop").relative_path().string() + "\n");
    std::ofstream(state / "braurser/manual/receipts/demo.json") <<
        R"({"scope":"user","files":[{"path":"bin/unrelated"},{"path":"share/applications/receipt.desktop"}]})";
    Environment home_env("HOME", home);
    Environment data_env("XDG_DATA_HOME", data);
    Environment dirs_env("XDG_DATA_DIRS", shared);
    ConfigRunner runner(fixture.path / "db");
    const auto result = UnmanagedLauncherScanner{}.scan_system(runner, state);
    ASSERT_TRUE(result) << result.error().code;
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().desktop_id, "visible.desktop");
}

TEST(LauncherScan, CombinedPackageAndReceiptBudgetFailsClosed) {
    Fixture fixture;
    ASSERT_FALSE(fixture.path.empty());
    const auto home = fixture.path / "home";
    const auto state = fixture.path / "state";
    std::filesystem::create_directories(home / ".local/share/applications");
    std::filesystem::create_directories(state / "braurser/manual/receipts");
    std::string files;
    files.reserve(2000000);
    for (std::size_t index = 0; index < 1000000; ++index) files += "a\n";
    package(fixture.path / "db", "first", files);
    package(fixture.path / "db", "second", files);
    std::ofstream(state / "braurser/manual/receipts/demo.json") <<
        R"({"scope":"user","files":[{"path":"share/applications/demo.desktop"}]})";
    Environment home_env("HOME", home);
    Environment data_env("XDG_DATA_HOME", home / ".local/share");
    Environment dirs_env("XDG_DATA_DIRS", fixture.path / "shared");
    ConfigRunner runner(fixture.path / "db");
    const auto result = UnmanagedLauncherScanner{}.scan_system(runner, state);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "launcher-coverage-incomplete");
}

TEST(LauncherScan, FiltersPackagedHiddenAndInvalidLaunchers) {
    auto pattern = (std::filesystem::temp_directory_path() / "braurser-launcher-XXXXXX").string();
    auto* raw = mkdtemp(pattern.data());
    ASSERT_NE(raw, nullptr);
    const std::filesystem::path root(raw);
    const auto local = root / "local/applications";
    const auto shared = root / "shared/applications";
    std::filesystem::create_directories(local);
    std::filesystem::create_directories(shared);
    std::ofstream(local / "visible.desktop") << "[Desktop Entry]\nType=Application\nName=Visible\nExec=demo %f --name=ok\n";
    std::ofstream(local / "hidden.desktop") << "[Desktop Entry]\nType=Application\nName=Hidden\nNoDisplay=true\n";
    std::ofstream(local / "packaged.desktop") << "[Desktop Entry]\nType=Application\nName=Packaged\n";
    std::ofstream(shared / "visible.desktop") << "[Desktop Entry]\nType=Application\nName=Other\n";
    std::ofstream(shared / "link.desktop") << "[Desktop Entry]\nType=Link\nName=Not an app\n";
    UnmanagedLauncherScanner scanner;
    auto entries = scanner.scan(root / "local", {root / "shared"}, {local / "packaged.desktop"});
    ASSERT_TRUE(entries.has_value()) << entries.error().code;
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(entries->front().desktop_id, "visible.desktop");
    EXPECT_EQ(entries->front().name, "Visible");
    EXPECT_EQ(entries->front().exec_display, "demo  --name=ok");
    EXPECT_EQ(entries->front().owner, "outside");
    std::filesystem::remove_all(root);
}

}
