#include <braurser/daemon/connectivity/checker.hpp>

#include <gtest/gtest.h>

namespace braurser::daemon::connectivity {
namespace {

struct FakeClock final : core::Clock {
    std::chrono::system_clock::time_point time{};
    std::chrono::system_clock::time_point now() const override { return time; }
};
struct FakeNm final : NetworkManagerStatus {
    Status status{Status::Unknown};
    int calls{0};
    Status query() override { ++calls; return status; }
};
struct FakeHttp final : core::HttpClient {
    core::HttpResponse response = [] {
        core::HttpResponse value;
        value.status = 200;
        value.body = "NetworkManager is online";
        return value;
    }();
    core::HttpRequest request;
    int calls{0};
    std::expected<core::HttpResponse, core::HttpError> get(const core::HttpRequest& value) override {
        request = value;
        ++calls;
        return response;
    }
};

TEST(Connectivity, KnownNmStatusAvoidsProbeAndCaches60Seconds) {
    FakeNm nm;
    nm.status = Status::Offline;
    FakeHttp http;
    FakeClock clock;
    ConnectivityChecker checker(nm, http, clock);
    EXPECT_EQ(checker.check(), Status::Offline);
    nm.status = Status::Online;
    clock.time += std::chrono::seconds(59);
    EXPECT_EQ(checker.check(), Status::Offline);
    EXPECT_EQ(nm.calls, 1);
    clock.time += std::chrono::seconds(1);
    EXPECT_EQ(checker.check(), Status::Online);
    EXPECT_EQ(http.calls, 0);
}

TEST(Connectivity, UnknownUsesStrictBoundedHttpsProbe) {
    FakeNm nm;
    FakeHttp http;
    FakeClock clock;
    ConnectivityChecker checker(nm, http, clock);
    EXPECT_EQ(checker.check(), Status::Online);
    EXPECT_EQ(http.calls, 1);
    EXPECT_EQ(http.request.url, "https://ping.archlinux.org/nm-check.txt");
    EXPECT_EQ(http.request.allowed_hosts, (std::vector<std::string>{"ping.archlinux.org"}));
    EXPECT_EQ(http.request.timeout, std::chrono::seconds(3));
    EXPECT_EQ(http.request.max_bytes, 64U);
    clock.time += std::chrono::seconds(60);
    http.response.body += "\n";
    EXPECT_EQ(checker.check(), Status::Online);
    clock.time += std::chrono::seconds(60);
    http.response.body += "\n";
    EXPECT_EQ(checker.check(), Status::Offline);
}

TEST(Connectivity, SystemBusAbsenceIsHandled) {
    GioNetworkManagerStatus nm;
    const auto value = nm.query();
    EXPECT_TRUE(value == Status::Unknown || value == Status::Online || value == Status::Offline);
}

TEST(Connectivity, DISABLED_LivePing) {
    FakeNm nm;
    core::CurlHttpClient http;
    FakeClock clock;
    ConnectivityChecker checker(nm, http, clock);
    EXPECT_EQ(checker.check(), Status::Online);
}

}
}
