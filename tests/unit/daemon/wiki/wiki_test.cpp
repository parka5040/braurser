#include <braurser/daemon/wiki/client.hpp>
#include <braurser/daemon/core/files.hpp>

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::wiki {
namespace {

struct TestClock final : core::Clock {
    std::chrono::system_clock::time_point current{std::chrono::sys_days(std::chrono::year(2026)/9/25)};
    std::chrono::system_clock::time_point now() const override { return current; }
};

struct FakeHttp final : core::HttpClient {
    std::vector<core::HttpRequest> requests;
    std::vector<core::HttpResponse> replies;
    std::expected<core::HttpResponse, core::HttpError> get(const core::HttpRequest& request) override {
        requests.push_back(request);
        if (requests.size() > replies.size()) return std::unexpected(core::HttpError{1, "offline"});
        return replies[requests.size() - 1];
    }
};

core::HttpResponse fixture(std::string_view file) {
    const auto content = core::read_bounded(std::filesystem::path(FIXTURES_ROOT) / "wiki" / file, 2 * 1024 * 1024);
    EXPECT_TRUE(content.has_value());
    core::HttpResponse response;
    response.status = 200;
    response.body = content.value_or("");
    return response;
}

TEST(Wiki, CandidateOrderAndSuffix) {
    EXPECT_EQ(ArticleResolver::candidates("Media Player", "vlc-git", "vlc-git"),
              (std::vector<std::string>{"Media Player", "vlc-git", "vlc"}));
    EXPECT_EQ(ArticleResolver::candidates("pacman", "pacman", "pacman"),
              (std::vector<std::string>{"pacman"}));
    EXPECT_TRUE(ArticleResolver::search_url("\r\n").empty());
    EXPECT_EQ(ArticleResolver::search_url("Firefox Privacy"),
              "https://wiki.archlinux.org/index.php?search=Firefox%20Privacy");
}

TEST(Wiki, MissingTitleThenRedirectDeliberatelyMatches) {
    FakeHttp http;
    TestClock clock;
    http.replies = {fixture("missingtitle.json"), fixture("vlc-redirect.section0.json")};
    WikiClient client(http, clock);
    auto result = client.lookup("Does Not Exist", "vlc", "vlc");
    ASSERT_EQ(result.status, LookupStatus::Found);
    ASSERT_TRUE(result.document);
    EXPECT_EQ(result.document->title, "VLC media player");
    ASSERT_EQ(http.requests.size(), 2U);
    EXPECT_EQ(http.requests[0].allowed_hosts, (std::vector<std::string>{"wiki.archlinux.org"}));
    EXPECT_EQ(http.requests[0].max_bytes, 2U * 1024 * 1024);
    EXPECT_EQ(http.requests[0].timeout, std::chrono::seconds(10));
    EXPECT_NE(http.requests[0].url.find("maxlag=5"), std::string::npos);
    EXPECT_NE(http.requests[0].url.find("prop=text%7Csections%7Cdisplaytitle"), std::string::npos);
}

TEST(Wiki, UnrelatedTitleIsNotAutoPicked) {
    FakeHttp http;
    TestClock clock;
    http.replies = {fixture("vlc-redirect.section0.json"), fixture("missingtitle.json")};
    WikiClient client(http, clock);
    EXPECT_EQ(client.lookup("not-vlc", "not-vlc", "not-vlc").status, LookupStatus::NotFound);
}

TEST(Wiki, FullPagePackageLinkMatchesUnrelatedTitle) {
    FakeHttp http;
    TestClock clock;
    http.replies = {fixture("pacman.full.json")};
    WikiClient client(http, clock);
    auto result = client.lookup("Unrelated application", "pacman", "pacman");
    ASSERT_TRUE(result.document.has_value());
    EXPECT_EQ(result.document->title, "Pacman");
}

TEST(Wiki, CacheReturnsStaleWhenOfflineAndNegativeExpires) {
    TestClock clock;
    const auto root = std::filesystem::temp_directory_path() /
                      ("braurser-wiki-" + std::to_string(getpid()) + "-" + std::to_string(clock.now().time_since_epoch().count()));
    ASSERT_FALSE(std::filesystem::exists(root));
    WikiCache cache(root, clock);
    FakeHttp http;
    http.replies = {fixture("firefox.section0.json")};
    WikiClient client(http, clock, &cache);
    auto first = client.lookup("Firefox", "firefox", "firefox");
    ASSERT_TRUE(first.document.has_value());
    const auto cache_dir = root / "braurser" / "wiki";
    struct stat info{};
    ASSERT_EQ(::stat(cache_dir.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0700);
    for (const auto& file : std::filesystem::directory_iterator(cache_dir)) {
        ASSERT_EQ(::stat(file.path().c_str(), &info), 0);
        EXPECT_EQ(info.st_mode & 0777, 0600);
    }
    auto second = client.lookup("Firefox", "firefox", "firefox");
    ASSERT_EQ(second.status, LookupStatus::Found);
    EXPECT_EQ(http.requests.size(), 1U);
    clock.current += std::chrono::hours(24 * 8);
    auto offline = client.lookup("Firefox", "firefox", "firefox", false);
    EXPECT_EQ(offline.status, LookupStatus::Found);
    EXPECT_TRUE(offline.stale);
    const auto failed_fetch = client.lookup("Firefox", "firefox", "firefox", true);
    EXPECT_EQ(failed_fetch.status, LookupStatus::Found);
    EXPECT_TRUE(failed_fetch.stale);
    EXPECT_FALSE(cache.read("Firefox", "other-package"));
    EXPECT_TRUE(cache.write("missing", std::nullopt));
    EXPECT_TRUE(cache.read("missing"));
    clock.current += std::chrono::hours(25);
    EXPECT_FALSE(cache.read("missing"));
    std::filesystem::remove_all(root);
}

TEST(Wiki, DISABLED_LiveArchWiki) {
    core::CurlHttpClient http;
    TestClock clock;
    WikiClient client(http, clock);
    const auto result = client.lookup("Firefox", "firefox", "firefox");
    ASSERT_EQ(result.status, LookupStatus::Found);
}

}
}
