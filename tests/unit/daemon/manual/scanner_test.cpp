#include <braurser/daemon/manual/manual.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <unistd.h>

namespace {
using namespace braurser::daemon::manual;

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
