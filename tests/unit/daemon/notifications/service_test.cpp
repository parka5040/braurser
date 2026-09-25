#include <braurser/daemon/notifications/service.hpp>
#include <braurser/protocol/dto.hpp>
#include "../../../../src/notifier/policy.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace notifications = braurser::daemon::notifications;
namespace protocol = braurser::protocol;
namespace notifier = braurser::notifier;

struct PrivateDirectory {
    PrivateDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "braurser-notifier-XXXXXX").string();
        if (auto* created = mkdtemp(pattern.data())) path = created;
    }
    ~PrivateDirectory() { if (!path.empty()) std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

struct FakeTransport final : notifications::NotificationTransport {
    bool accept = true;
    std::vector<notifications::Payload> sent;
    bool send(const notifications::Payload& payload) override {
        if (!accept) return false;
        sent.push_back(payload);
        return true;
    }
};

protocol::UpdateReport report(std::string name) {
    protocol::UpdateReport result;
    protocol::UpdateItem item;
    item.package = {{"official"}, {}, std::move(name)};
    result.official.push_back(std::move(item));
    return result;
}

TEST(NotifierPolicy, AutostartNotificationAndFrequencyOptIn) {
    protocol::Settings settings{{"auto"}, {"auto"}, {"daily"}, true, false, true, false, {"system"}};
    EXPECT_FALSE(notifier::interval(settings));
    settings.autostart = true;
    EXPECT_EQ(notifier::interval(settings), std::chrono::hours(24));
    settings.check_frequency = {"weekly"};
    EXPECT_EQ(notifier::interval(settings), std::chrono::hours(24 * 7));
    settings.check_frequency = {"never"};
    EXPECT_FALSE(notifier::interval(settings));
    settings.check_frequency = {"daily"};
    settings.notifications = false;
    EXPECT_FALSE(notifier::interval(settings));
}

TEST(NotifierThrottle, DeduplicatesSetsWithinDayAndResetsNextDay) {
    PrivateDirectory directory;
    ASSERT_FALSE(directory.path.empty());
    FakeTransport transport;
    notifications::NotificationService service(directory.path, transport);
    const auto today = std::chrono::sys_days(std::chrono::year{2026}/9/25);
    EXPECT_TRUE(service.notify(report("first"), today));
    notifications::NotificationService restored(directory.path, transport);
    EXPECT_FALSE(restored.notify(report("first"), today + std::chrono::hours(1)));
    EXPECT_FALSE(service.notify(report("first"), today + std::chrono::hours(2)));
    EXPECT_TRUE(service.notify(report("second"), today + std::chrono::hours(3)));
    EXPECT_FALSE(service.notify(report("first"), today + std::chrono::hours(4)));
    auto pair = report("first");
    pair.official.push_back(report("second").official.front());
    EXPECT_TRUE(service.notify(pair, today + std::chrono::hours(5)));
    std::ranges::reverse(pair.official);
    EXPECT_FALSE(service.notify(pair, today + std::chrono::hours(6)));
    EXPECT_TRUE(service.notify(report("first"), today + std::chrono::hours(24)));
    EXPECT_EQ(transport.sent.size(), 4U);
    struct stat info{};
    const auto path = directory.path / "braurser/notifications.json";
    ASSERT_EQ(lstat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600);
    ASSERT_EQ(lstat(path.parent_path().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
}

TEST(NotifierThrottle, AbsenceAndInvalidStateFailClosed) {
    PrivateDirectory directory;
    ASSERT_FALSE(directory.path.empty());
    FakeTransport transport;
    notifications::NotificationService service(directory.path, transport);
    const auto today = std::chrono::sys_days(std::chrono::year{2026}/9/25);
    transport.accept = false;
    EXPECT_FALSE(service.notify(report("first"), today));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "braurser/notifications.json"));
    transport.accept = true;
    EXPECT_TRUE(service.notify(report("first"), today));
    const auto path = directory.path / "braurser/notifications.json";
    ASSERT_EQ(chmod(path.c_str(), 0644), 0);
    EXPECT_FALSE(service.notify(report("second"), today));
    EXPECT_EQ(transport.sent.size(), 1U);
}

TEST(NotifierPayload, OnlyTrustedPluralCopyIsSent) {
    auto single = notifications::NotificationService::payload(1);
    auto multiple = notifications::NotificationService::payload(2);
    EXPECT_EQ(single.app_name, "braurser");
    EXPECT_EQ(single.icon, "braurser");
    EXPECT_EQ(single.actions.front(), "review");
    EXPECT_NE(single.body.find("1 update"), std::string::npos);
    EXPECT_NE(multiple.body.find("2 updates"), std::string::npos);
    const auto value = single.summary + single.body + multiple.summary + multiple.body;
    for (const auto* secret : {"password", "https:", "http:", "command output", "sensitive-package"})
        EXPECT_EQ(value.find(secret), std::string::npos);
    PrivateDirectory directory;
    ASSERT_FALSE(directory.path.empty());
    FakeTransport transport;
    notifications::NotificationService service(directory.path, transport);
    EXPECT_TRUE(service.notify(report("PASSWORD=topsecret https://example.invalid command output")));
    ASSERT_EQ(transport.sent.size(), 1U);
    const auto sent = transport.sent.front().summary + transport.sent.front().body;
    for (const auto* secret : {"topsecret", "https://", "example.invalid", "command output"})
        EXPECT_EQ(sent.find(secret), std::string::npos);
}

}
