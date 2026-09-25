#include "../../../../src/launcher/selection.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {
namespace launcher = braurser::launcher;

TEST(LauncherSelection, EnvironmentSettingsAndDesktopTruthTable) {
    EXPECT_EQ(launcher::select_ui("qt", "gtk", "GNOME"), "qt");
    EXPECT_EQ(launcher::select_ui("gtk", "qt", "KDE"), "gtk");
    EXPECT_EQ(launcher::select_ui("", "qt", "GNOME"), "qt");
    EXPECT_EQ(launcher::select_ui("", "gtk", "LXQt"), "gtk");
    EXPECT_EQ(launcher::select_ui("", "auto", "gnome:KDE"), "qt");
    EXPECT_EQ(launcher::select_ui("", "auto", "lXqT:other"), "qt");
    EXPECT_EQ(launcher::select_ui("", "auto", "GNOME"), "gtk");
    EXPECT_EQ(launcher::select_ui("", "auto", "KDE-PLASMA"), "qt");
    EXPECT_FALSE(launcher::select_ui("invalid", "gtk", "KDE"));
    EXPECT_FALSE(launcher::select_ui("", "invalid", "KDE"));
}

TEST(LauncherSelection, MissingBinaryFallsBackOnlyToExecutableRegularFile) {
    auto pattern = (std::filesystem::temp_directory_path() / "braurser-launcher-XXXXXX").string();
    const auto* created = mkdtemp(pattern.data());
    ASSERT_NE(created, nullptr);
    const std::filesystem::path directory(created);
    EXPECT_FALSE(launcher::select_binary(directory, "gtk"));
    const auto gtk = directory / "braurser-gtk";
    const auto qt = directory / "braurser-qt";
    { std::ofstream file(qt); file << "binary"; }
    ASSERT_EQ(chmod(qt.c_str(), 0700), 0);
    const auto fallback = launcher::select_binary(directory, "gtk");
    ASSERT_TRUE(fallback);
    EXPECT_TRUE(fallback->fallback);
    EXPECT_EQ(fallback->binary, qt);
    ASSERT_EQ(symlink(qt.c_str(), gtk.c_str()), 0);
    EXPECT_TRUE(launcher::select_binary(directory, "gtk")->fallback);
    ASSERT_EQ(unlink(gtk.c_str()), 0);
    { std::ofstream file(gtk); file << "binary"; }
    ASSERT_EQ(chmod(gtk.c_str(), 0700), 0);
    const auto selected = launcher::select_binary(directory, "gtk");
    ASSERT_TRUE(selected);
    EXPECT_FALSE(selected->fallback);
    EXPECT_EQ(selected->binary, gtk);
    EXPECT_FALSE(launcher::select_binary(directory, "invalid"));
    std::filesystem::remove_all(directory);
}

TEST(LauncherDesktop, TemplatesPassDesktopFileValidate) {
    for (const auto* file : {BRAURSER_DESKTOP, BRAURSER_NOTIFIER_DESKTOP}) {
        pid_t child{};
        auto* command = const_cast<char*>("/usr/bin/desktop-file-validate");
        auto* argument = const_cast<char*>(file);
        char* arguments[]{command, argument, nullptr};
        ASSERT_EQ(posix_spawn(&child, command, nullptr, nullptr, arguments, environ), 0);
        int status{};
        ASSERT_EQ(waitpid(child, &status, 0), child);
        ASSERT_TRUE(WIFEXITED(status));
        EXPECT_EQ(WEXITSTATUS(status), 0) << file;
    }
}

}
