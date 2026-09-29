#include <braurser/domain/protection.hpp>
#include <gtest/gtest.h>

namespace braurser::domain {
namespace {

std::vector<std::string> guard(std::vector<std::string> removals, std::vector<std::string> hold = {},
                               std::vector<std::string> kernels = {}) {
    return protected_removals(removals, hold, kernels);
}

}

TEST(Protection, EssentialPackagesAreAlwaysProtected) {
    EXPECT_EQ(guard({"tree", "sudo", "glibc", "nano"}), (std::vector<std::string>{"sudo", "glibc"}));
    EXPECT_EQ(guard({"base", "filesystem", "pacman", "systemd", "bash", "coreutils", "polkit"}).size(), 7U);
    EXPECT_TRUE(guard({"tree", "sudo-rs", "systemd-libs", "bash-completion"}).empty());
    EXPECT_TRUE(guard({}).empty());
}

TEST(Protection, HoldPackagesAreProtectedWithoutDuplicates) {
    EXPECT_EQ(guard({"tree", "firefox", "firefox", "pacman"}, {"firefox", "pacman"}),
              (std::vector<std::string>{"firefox", "pacman"}));
    EXPECT_TRUE(guard({"tree"}, {"firefox"}).empty());
}

TEST(Protection, LastInstalledKernelIsProtected) {
    EXPECT_EQ(guard({"linux", "tree"}, {}, {"linux"}), (std::vector<std::string>{"linux"}));
    EXPECT_TRUE(guard({"linux"}, {}, {"linux", "linux-lts"}).empty());
    EXPECT_EQ(guard({"linux-lts", "linux", "zfs"}, {}, {"linux", "linux-lts"}),
              (std::vector<std::string>{"linux-lts", "linux"}));
    EXPECT_TRUE(guard({"linux-headers"}, {}, {"linux"}).empty());
    EXPECT_TRUE(guard({"linux"}, {}, {}).empty());
}

TEST(Protection, KernelImageDetectionMatchesOnlyModuleVmlinuz) {
    EXPECT_TRUE(installs_kernel_image("%FILES%\nusr/\nusr/lib/\nusr/lib/modules/6.9.1-arch1-1/vmlinuz\n"));
    EXPECT_TRUE(installs_kernel_image("usr/lib/modules/7.2.6-zen2-1-zen/vmlinuz"));
    EXPECT_TRUE(installs_kernel_image("%FILES%\r\nusr/lib/modules/6.9/vmlinuz\r\n"));
    EXPECT_FALSE(installs_kernel_image("%FILES%\nusr/lib/modules/6.9/build/vmlinuz\n"));
    EXPECT_FALSE(installs_kernel_image("%FILES%\nusr/lib/modules//vmlinuz\n"));
    EXPECT_FALSE(installs_kernel_image("%FILES%\nusr/lib/modules/6.9/vmlinuz.old\n"));
    EXPECT_FALSE(installs_kernel_image("%FILES%\nboot/vmlinuz-linux\nusr/lib/modules/6.9/kernel/x.ko\n"));
    EXPECT_FALSE(installs_kernel_image(""));
}

}
