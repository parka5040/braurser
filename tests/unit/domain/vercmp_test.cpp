#include <braurser/domain/vercmp.hpp>
#include <gtest/gtest.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

namespace braurser::domain {
namespace {

struct Row { std::string_view a; std::string_view b; int expected; };

constexpr std::array rows{
    Row{"1.0a", "1.0", -1}, Row{"1.0alpha", "1.0", -1}, Row{"1.0.1", "1.0", 1},
    Row{"1.0", "1.0-1", 0}, Row{"1:1.0", "2.0", 1}, Row{"1..0", "1.0", 1},
    Row{"1_0", "1.0", 0}, Row{"1+0", "1.0", 0}, Row{"010", "10", 0},
    Row{"a", "b", -1}, Row{"1.0~rc1", "1.0", 1}, Row{"r123.abc", "1.2", -1},
    Row{"1.0a", "1.0.1", -1}, Row{"1.0", "1.0.a", -1}, Row{"2.0-1", "2.0-2", -1},
    Row{"", "", 0}, Row{"1", "1", 0}, Row{"a", "a", 0},
    Row{"0", "00", 0}, Row{"0001", "1", 0}, Row{"0000", "0", 0},
    Row{"1", "2", -1}, Row{"2", "1", 1}, Row{"10", "9", 1},
    Row{"2.0", "10.0", -1}, Row{"1.2", "1.10", -1}, Row{"1.10", "1.2", 1},
    Row{"a", "z", -1}, Row{"abc", "abd", -1}, Row{"z", "a", 1},
    Row{"abc2", "abc10", -1}, Row{"abc01", "abc1", 0}, Row{"12a", "12b", -1},
    Row{"1:1", "0:9", 1}, Row{"2:0", "1:999", 1}, Row{"0:1", "1", 0},
    Row{"01:1", "1:1", 0}, Row{"10:1", "2:999", 1}, Row{"1:1", "2:0", -1},
    Row{"1.0-9", "1.0-10", -1}, Row{"1.0-10", "1.0-9", 1}, Row{"1.0-01", "1.0-1", 0},
    Row{"1.0-1", "1.0-2", -1}, Row{"1.0-2", "1.0-1", 1}, Row{"1.0-3", "1.0", 0},
    Row{"1.0", "1.0-3", 0}, Row{"1.0-1", "1.1", -1}, Row{"1.2-8", "1.1-9", 1},
    Row{"1.0-2-1", "1.0-2-2", -1}, Row{"1.2.3", "1.2.4", -1}, Row{"3.2.1", "3.2.0", 1},
    Row{"1.0", "1.0a", 1}, Row{"1.0", "1.0.1", -1}, Row{"1.0.a", "1.0", 1},
    Row{"1..0", "1.0", 1}, Row{"1.0", "1..0", -1}, Row{"1_0", "1+0", 0},
    Row{"1.0~rc1", "1.0", 1}, Row{"1.0~rc1", "1.0~rc2", -1},
    Row{"r123.abc1234", "r124.abc1234", -1}, Row{"r999.abc", "r1000.abc", -1},
    Row{"1.0-rc1", "1.0-rc2", -1}, Row{"1.0-2", "1.0-02", 0},
    Row{"1.0+1", "1.0.1", 0}, Row{"1-1", "1-2", -1},
    Row{"1:2-1", "0:999-99", 1}, Row{"2:1-1", "2:1-2", -1},
    Row{"1:1-1", "1:2-1", -1}, Row{"123", "123", 0},
    Row{"1.2.3.4", "1.2.3.5", -1}, Row{"1.2.3.10", "1.2.3.9", 1}
};

int tool_vercmp(const std::string& a, const std::string& b) {
    int fds[2];
    if (pipe(fds) != 0) return 100;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(fds[0]); close(fds[1]); return 100;
    }
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    std::array<char*, 4> argv{const_cast<char*>("/usr/bin/vercmp"), const_cast<char*>(a.c_str()),
                              const_cast<char*>(b.c_str()), nullptr};
    pid_t pid = 0;
    const int err = posix_spawn(&pid, "/usr/bin/vercmp", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (err != 0) { close(fds[0]); return 100; }
    char result[32]{};
    const ssize_t count = read(fds[0], result, sizeof(result) - 1);
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || count < 1)
        return 100;
    if (result[0] == '-') return -1;
    if (result[0] == '0') return 0;
    if (result[0] == '1') return 1;
    return 100;
}

}

TEST(Vercmp, HandTable) {
    static_assert(rows.size() >= 60);
    for (const auto& row : rows) {
        EXPECT_EQ(vercmp(row.a, row.b), row.expected) << row.a << " / " << row.b;
        EXPECT_EQ(vercmp(row.b, row.a), -row.expected) << row.b << " / " << row.a;
    }
}

TEST(Vercmp, PacmanParity) {
    if (access("/usr/bin/vercmp", X_OK) != 0) GTEST_SKIP() << "/usr/bin/vercmp unavailable";
    for (const auto& row : rows) {
        ASSERT_EQ(vercmp(row.a, row.b), tool_vercmp(std::string(row.a), std::string(row.b)))
            << row.a << " / " << row.b;
        ASSERT_EQ(vercmp(row.b, row.a), tool_vercmp(std::string(row.b), std::string(row.a)))
            << row.b << " / " << row.a;
    }
    std::uint32_t seed = 0x132acf45U;
    constexpr std::string_view alphabet = "0123456789.abc-_+:~";
    const auto version = [&]() {
        seed = seed * 1664525U + 1013904223U;
        const auto length = static_cast<std::size_t>(seed % 24U);
        std::string result;
        for (std::size_t i = 0; i < length; ++i) {
            seed = seed * 1664525U + 1013904223U;
            result += alphabet[seed % alphabet.size()];
        }
        return result;
    };
    for (int i = 0; i < 2000; ++i) {
        const std::string a = version(), b = version();
        ASSERT_EQ(vercmp(a, b), tool_vercmp(a, b)) << "pair " << i << ": " << a << " / " << b;
    }
}

}
