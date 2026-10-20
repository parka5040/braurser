#include <braurser/daemon/core/process.hpp>

#include <gtest/gtest.h>
#include <chrono>
#include <cerrno>
#include <thread>
#include <csignal>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace braurser::daemon::core {
namespace {

ProcessSpec spec(std::string executable) {
    ProcessSpec result;
    result.executable = std::move(executable);
    return result;
}

TEST(Process, ExitCodes) {
    PosixProcessRunner runner;
    auto good = runner.run(spec("/usr/bin/true"));
    auto bad = runner.run(spec("/usr/bin/false"));
    ASSERT_TRUE(good.has_value());
    ASSERT_TRUE(bad.has_value());
    EXPECT_EQ(good->exit_code, 0);
    EXPECT_EQ(bad->exit_code, 1);
}

TEST(Process, OutputAndArgumentVector) {
    PosixProcessRunner runner;
    auto input = spec("/usr/bin/printf");
    input.args = {"%s", "a; not-a-shell-command"};
    auto result = runner.run(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->out, "a; not-a-shell-command");
    EXPECT_TRUE(result->err.empty());
}

TEST(Process, ReplacesEnvironment) {
    PosixProcessRunner runner;
    ASSERT_EQ(setenv("BRAURSER_PARENT_SECRET", "must-not-appear", 1), 0);
    auto input = spec("/usr/bin/env");
    input.env = {{"BRAURSER_CHILD", "yes"}};
    auto result = runner.run(input);
    unsetenv("BRAURSER_PARENT_SECRET");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->out, "BRAURSER_CHILD=yes\n");
}

TEST(Process, TimeoutTerminatesGroupAndReaps) {
    PosixProcessRunner runner;
    auto input = spec("/usr/bin/sleep");
    input.args = {"10"};
    input.timeout = std::chrono::milliseconds(35);
    const auto start = std::chrono::steady_clock::now();
    auto result = runner.run(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->timed_out);
    EXPECT_TRUE(result->signaled);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
}

TEST(Process, PreCancelledDoesNotSpawn) {
    PosixProcessRunner runner;
    std::atomic<bool> cancelled{true};
    auto input = spec("/usr/bin/touch");
    const auto marker = std::filesystem::temp_directory_path() /
        ("braurser-cancelled-process-" + std::to_string(getpid()));
    ASSERT_FALSE(std::filesystem::exists(marker));
    input.args = {marker.string()};
    input.cancel = &cancelled;
    auto result = runner.run(input);
    EXPECT_FALSE(std::filesystem::exists(marker));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ECANCELED);
    EXPECT_EQ(result.error().message, "cancelled");
}

TEST(Process, RunningCancelTerminatesAndReaps) {
    PosixProcessRunner runner;
    std::atomic<bool> cancelled{false};
    auto input = spec("/usr/bin/sleep");
    input.args = {"30"};
    input.timeout = std::chrono::seconds(40);
    input.cancel = &cancelled;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancelled.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    auto result = runner.run(input);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->timed_out);
    EXPECT_TRUE(result->signaled);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
}

TEST(Process, SendsStdin) {
    PosixProcessRunner runner;
    auto input = spec("/usr/bin/cat");
    input.stdin_data = "hello stdin\n";
    auto result = runner.run(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->out, "hello stdin\n");
}

TEST(Process, StopsGroupPastCaptureCap) {
    PosixProcessRunner runner;
    auto input = spec("/usr/bin/yes");
    input.timeout = std::chrono::seconds(30);
    input.max_stdout = 19;
    const auto start = std::chrono::steady_clock::now();
    auto result = runner.run(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->timed_out);
    EXPECT_TRUE(result->signaled);
    EXPECT_EQ(result->term_signal, SIGKILL);
    EXPECT_TRUE(result->out_truncated);
    EXPECT_EQ(result->out.size(), 19U);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
}

TEST(Process, FloodingWritersCannotOutliveTheCap) {
    PosixProcessRunner runner;
    auto input = spec("/usr/bin/bash");
    input.args = {"-c", "yes & yes & yes & wait"};
    input.env = {{"PATH", "/usr/bin"}};
    input.timeout = std::chrono::seconds(30);
    input.max_stdout = 64 * 1024;
    const auto start = std::chrono::steady_clock::now();
    auto result = runner.run(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->out_truncated);
    EXPECT_FALSE(result->timed_out);
    EXPECT_EQ(result->out.size(), 64U * 1024U);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
}

TEST(Process, RejectsBadExecutableAndEnvironment) {
    PosixProcessRunner runner;
    EXPECT_FALSE(runner.run(spec("true")));
    EXPECT_FALSE(runner.run(spec("/no/such/executable")));
    auto input = spec("/usr/bin/true");
    input.env = {{"X=Y", "z"}};
    EXPECT_FALSE(runner.run(input));
}

TEST(Process, ClosesParentDescriptors) {
    PosixProcessRunner runner;
    const int extra = open("/dev/null", O_RDONLY);
    ASSERT_GE(extra, 0);
    auto input = spec("/usr/bin/ls");
    input.args = {"-1", "/proc/self/fd"};
    auto result = runner.run(input);
    close(extra);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exit_code, 0);
    EXPECT_EQ(result->out, "0\n1\n2\n3\n");
}

TEST(Process, ClosedChildStdinDoesNotSignalParent) {
    PosixProcessRunner runner;
    auto input = spec("/usr/bin/true");
    input.stdin_data = std::string(1 << 20, 'x');
    auto result = runner.run(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exit_code, 0);
}

}
}
