#include <braurser/daemon/core/files.hpp>

#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::core {
namespace {

class Files : public testing::Test {
protected:
    void SetUp() override {
        const auto* variable = std::getenv("TMPDIR");
        const std::string temporary = variable != nullptr && *variable == '/' ? std::string(variable) : std::filesystem::temp_directory_path().string();
        std::string pattern = temporary + "/braurser-core-XXXXXX";
        auto* created = mkdtemp(pattern.data());
        ASSERT_NE(created, nullptr);
        root = created;
    }
    void TearDown() override {
        if (!root.empty()) std::filesystem::remove_all(root);
    }
    std::filesystem::path root;
};

TEST_F(Files, CreatesPrivateDirectoryAndRejectsSymlink) {
    const auto target = root / "a" / "b";
    ASSERT_TRUE(ensure_private_dir(target));
    struct stat info {};
    ASSERT_EQ(stat(target.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
    ASSERT_EQ(chmod(target.c_str(), 0777), 0);
    ASSERT_TRUE(ensure_private_dir(target));
    ASSERT_EQ(stat(target.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
    std::filesystem::create_directory_symlink(target, root / "link");
    EXPECT_FALSE(ensure_private_dir(root / "link"));
    EXPECT_FALSE(ensure_private_dir(root / "link" / "inner"));
}

TEST_F(Files, RefusesForeignWritableDirectory) {
    const std::filesystem::path parent{"/tmp"};
    struct stat info {};
    ASSERT_EQ(stat(parent.c_str(), &info), 0);
    if (info.st_uid == getuid() || (info.st_mode & 0022) == 0) GTEST_SKIP();
    EXPECT_FALSE(ensure_private_dir(parent));
}

TEST_F(Files, AtomicWriteSetsModeAndReplacesContents) {
    const auto path = root / "file";
    ASSERT_TRUE(atomic_write(path, "old"));
    ASSERT_TRUE(atomic_write(path, "new data"));
    auto contents = read_bounded(path, 16);
    ASSERT_TRUE(contents.has_value());
    EXPECT_EQ(*contents, "new data");
    struct stat info {};
    ASSERT_EQ(stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    EXPECT_FALSE(atomic_write(path, "bad", 0666));
    EXPECT_EQ(read_bounded(path, 16).value(), "new data");
}

TEST_F(Files, BoundedReadRejectsOverflowAndSymlink) {
    const auto path = root / "source";
    ASSERT_TRUE(atomic_write(path, "abcdef"));
    EXPECT_EQ(read_bounded(path, 6).value(), "abcdef");
    EXPECT_FALSE(read_bounded(path, 5));
    std::filesystem::create_symlink(path, root / "link");
    EXPECT_FALSE(read_bounded(root / "link", 100));
    EXPECT_EQ(read_bounded(root / "link", 100, true).value(), "abcdef");
}

TEST_F(Files, RemovesNestedTreeWithoutFollowingSymlinks) {
    const auto outside = root / "outside";
    ASSERT_TRUE(atomic_write(outside, "keep"));
    const auto nested = root / "sub" / "nested";
    ASSERT_TRUE(ensure_private_dir(nested));
    ASSERT_TRUE(atomic_write(nested / "data", "remove"));
    std::filesystem::create_symlink(outside, nested / "link");
    ASSERT_TRUE(remove_tree_confined(root, "sub"));
    EXPECT_FALSE(std::filesystem::exists(root / "sub"));
    EXPECT_EQ(read_bounded(outside, 10).value(), "keep");
}

TEST_F(Files, RefusesEscapeAndSymbolicRoot) {
    ASSERT_TRUE(ensure_private_dir(root / "sub"));
    EXPECT_FALSE(remove_tree_confined(root, "../outside"));
    EXPECT_FALSE(remove_tree_confined(root, root));
    EXPECT_FALSE(remove_tree_confined(root, root.parent_path() / "outside"));
    std::filesystem::create_directory_symlink(root, root / "link");
    EXPECT_FALSE(remove_tree_confined(root / "link", "sub"));
}

TEST_F(Files, RuntimeIsMandatory) {
    const auto* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string old = runtime ? runtime : "";
    unsetenv("XDG_RUNTIME_DIR");
    EXPECT_FALSE(resolve_xdg_paths());
    setenv("XDG_RUNTIME_DIR", "relative", 1);
    EXPECT_FALSE(resolve_xdg_paths());
    if (runtime) setenv("XDG_RUNTIME_DIR", old.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
}

}
}
