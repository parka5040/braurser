#include <braurser/daemon/core/http.hpp>

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace braurser::daemon::core {
namespace {

class FakeTransport final : public HttpTransport {
public:
    std::vector<std::string> visited;
    std::vector<HttpTransfer> replies;
    std::expected<HttpTransfer, HttpError> perform(const HttpRequest&, std::string_view url) override {
        visited.emplace_back(url);
        if (visited.size() > replies.size()) return std::unexpected(HttpError{2, "missing fake response"});
        return replies[visited.size() - 1];
    }
};

HttpTransfer reply(long status, std::optional<std::string> location = std::nullopt) {
    HttpTransfer result;
    result.response.status = status;
    result.location = std::move(location);
    return result;
}

HttpRequest request(std::string url = "https://wiki.archlinux.org/api.php") {
    HttpRequest result;
    result.url = std::move(url);
    result.allowed_hosts = {"wiki.archlinux.org"};
    return result;
}

TEST(Http, RequiresAllowedHttpsHostWithoutCredentials) {
    const std::vector<std::string> denied{
        "http://wiki.archlinux.org", "file:///etc/passwd", "ftp://wiki.archlinux.org/",
        "https://other.example/api", "https://wiki.archlinux.org.evil.example/api",
        "https://user:pass@wiki.archlinux.org/api", "https://user@wiki.archlinux.org/api",
        "https://wiki.archlinux.org@evil.example/api", "https://wiki.archlinux.org\r\nInjected: yes/"
    };
    for (const auto& url : denied) {
        SCOPED_TRACE(url);
        FakeTransport transport;
        CurlHttpClient client(transport);
        EXPECT_FALSE(client.get(request(url)));
        EXPECT_TRUE(transport.visited.empty());
    }
}

TEST(Http, RejectsInvalidAllowlistAndHeaders) {
    FakeTransport transport;
    CurlHttpClient client(transport);
    auto input = request();
    input.allowed_hosts = {};
    EXPECT_FALSE(client.get(input));
    input.allowed_hosts = {"Wiki.Archlinux.Org"};
    EXPECT_FALSE(client.get(input));
    input.allowed_hosts = {"wiki.archlinux.org"};
    input.if_none_match = "ok\r\nInjected: yes";
    EXPECT_FALSE(client.get(input));
    input.if_none_match.reset();
    input.max_redirects = -1;
    EXPECT_FALSE(client.get(input));
    EXPECT_TRUE(transport.visited.empty());
}

TEST(Http, ValidatesEveryRedirectBeforeTransport) {
    FakeTransport transport;
    transport.replies = {reply(302, "https://evil.example/")};
    CurlHttpClient client(transport);
    auto input = request();
    input.max_redirects = 2;
    EXPECT_FALSE(client.get(input));
    ASSERT_EQ(transport.visited.size(), 1U);
    EXPECT_EQ(transport.visited[0], "https://wiki.archlinux.org/api.php");
}

TEST(Http, PermitsSameHostRelativeRedirectAnd304) {
    FakeTransport transport;
    auto not_modified = reply(304);
    not_modified.response.etag = "test";
    transport.replies = {reply(302, "/next"), not_modified};
    CurlHttpClient client(transport);
    auto input = request();
    input.max_redirects = 1;
    auto result = client.get(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->not_modified);
    EXPECT_EQ(result->etag, "test");
    EXPECT_EQ(result->final_url, "https://wiki.archlinux.org/next");
    ASSERT_EQ(transport.visited.size(), 2U);
}

TEST(Http, RefusesHttpDowngradeAndRedirectLimit) {
    FakeTransport transport;
    transport.replies = {reply(301, "http://wiki.archlinux.org/bad")};
    CurlHttpClient client(transport);
    auto input = request();
    input.max_redirects = 1;
    EXPECT_FALSE(client.get(input));
    input.max_redirects = 0;
    EXPECT_FALSE(client.get(input));
}

TEST(Http, RejectsOversizedFakeResponse) {
    FakeTransport transport;
    auto large = reply(200);
    large.response.body = "too large";
    transport.replies = {large};
    CurlHttpClient client(transport);
    auto input = request();
    input.max_bytes = 1;
    EXPECT_FALSE(client.get(input));
}

TEST(Http, DefaultGuardRefusesLoopbackUnlessExplicitlyAllowed) {
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener < 0) GTEST_SKIP() << "network sockets denied by sandbox";
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 4) != 0) {
        close(listener);
        GTEST_SKIP() << "loopback listening denied by sandbox";
    }
    socklen_t length = sizeof(address);
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    auto input = request("https://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/");
    input.allowed_hosts = {"127.0.0.1"};
    input.timeout = std::chrono::milliseconds(100);
    CurlHttpClient client;
    EXPECT_FALSE(client.get(input));
    pollfd entry{listener, POLLIN, 0};
    EXPECT_EQ(poll(&entry, 1, 0), 0);
    input.allow_private_addresses = true;
    EXPECT_FALSE(client.get(input));
    EXPECT_EQ(poll(&entry, 1, 0), 1);
    const int accepted = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (accepted >= 0) close(accepted);
    close(listener);
}

TEST(Http, DISABLED_LiveArchWiki) {
    CurlHttpClient client;
    auto input = request("https://wiki.archlinux.org/api.php?action=query&meta=siteinfo&format=json");
    auto result = client.get(input);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, 200);
    EXPECT_NE(result->body.find("query"), std::string::npos);
}

}
}
