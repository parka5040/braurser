#include <braurser/daemon/core/clock.hpp>
#include <braurser/daemon/core/logger.hpp>
#include <braurser/daemon/core/text.hpp>

#include <gtest/gtest.h>
#include <chrono>
#include <string>

namespace braurser::daemon::core {
namespace {

class FakeClock final : public Clock {
public:
    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::time_point(std::chrono::seconds(42));
    }
};

TEST(Text, RemovesRealHelperEscapeSequences) {
    const std::string input = "\x1b[1m\x1b[31m -> \x1b[0m\x1b[0minvalid option";
    EXPECT_EQ(strip_terminal_controls(input), " -> invalid option");
    EXPECT_EQ(strip_terminal_controls("a\x1b]0;private\a" "b\x1b]title\x1b\\c\x01\t\n"), "abc\t\n");
}

TEST(Text, RedactsCredentialFieldsAndUrlUserinfo) {
    const auto sanitized = redact("password=hunter2 PASSWD: trustno1 token=abc authorization:Bearer secret=xyz "
                                  "https://user:password@example.org/path?x=1");
    EXPECT_EQ(sanitized, "password=[REDACTED] PASSWD: [REDACTED] token=[REDACTED] "
                         "authorization:[REDACTED] secret=[REDACTED] "
                         "https://[REDACTED]@example.org/path?x=1");
}

TEST(Text, TruncatesOnUtf8Boundary) {
    EXPECT_EQ(bounded_line("abcdef", 3), "abc…");
    EXPECT_EQ(bounded_line("aéz", 2), "a…");
    EXPECT_EQ(bounded_line("abc", 3), "abc");
}

TEST(Text, LoggerRedactsBeforeSavingAndEvictsOldest) {
    RedactingRingLogger logger(2);
    logger.log("one");
    logger.log("\x1b[31mtoken=123\x1b[0m");
    logger.log("password=xyz");
    EXPECT_EQ(logger.snapshot(), (std::vector<std::string>{"token=[REDACTED]", "password=[REDACTED]"}));
}

TEST(Text, ClockCanBeInjected) {
    FakeClock clock;
    const Clock& source = clock;
    EXPECT_EQ(source.now(), std::chrono::system_clock::time_point(std::chrono::seconds(42)));
    SystemClock system;
    EXPECT_GE(system.now(), std::chrono::system_clock::time_point{});
}

}
}
