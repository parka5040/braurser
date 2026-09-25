#include <braurser/daemon/news/feed.hpp>
#include <braurser/daemon/core/files.hpp>

#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

namespace braurser::daemon::news {
namespace {

std::string fixture(std::string_view file) {
    const auto content = core::read_bounded(std::filesystem::path(FIXTURES_ROOT) / "news" / file, 1024 * 1024);
    EXPECT_TRUE(content.has_value());
    return content.value_or("");
}

struct FakeHttp final : core::HttpClient {
    std::vector<core::HttpResponse> replies;
    std::vector<core::HttpRequest> requests;
    std::expected<core::HttpResponse, core::HttpError> get(const core::HttpRequest& request) override {
        requests.push_back(request);
        if (requests.size() > replies.size()) return std::unexpected(core::HttpError{1, "offline"});
        return replies[requests.size() - 1];
    }
};

TEST(News, RealFixtureParsesArchItemsAndManualIntervention) {
    const auto result = parse_rss(fixture("arch-news.rss"));
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 10U);
    EXPECT_TRUE(result->front().requires_manual_intervention);
    EXPECT_EQ(result->front().published, "2026-09-22T09:09:27Z");
    EXPECT_NE(result->front().summary.find("Starting with package version"), std::string::npos);
    EXPECT_FALSE(result->at(2).requires_manual_intervention);
}

TEST(News, HostileFixtureRejectsLinksAndStripsScripts) {
    const auto result = parse_rss(fixture("hostile.rss"));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().published, "2026-09-22T09:09:27Z");
    EXPECT_EQ(result->front().summary.find("bad"), std::string::npos);
    EXPECT_TRUE(result->front().requires_manual_intervention);
    EXPECT_FALSE(parse_rss(fixture("xxe.rss")));
}

TEST(News, RepoGateOnlyOfficialArchRepositories) {
    EXPECT_TRUE(arch_news_applicable({"cachyos", "extra"}));
    EXPECT_FALSE(arch_news_applicable({"manjaro", "community", "artix"}));
    EXPECT_FALSE(arch_news_applicable({"Core"}));
}

TEST(News, ConditionalRequestRetainsLastValidFeed) {
    FakeHttp http;
    core::HttpResponse first_reply;
    first_reply.status = 200;
    first_reply.body = fixture("arch-news.rss");
    first_reply.etag = "\"abc\"";
    first_reply.last_modified = "Tue, 22 Sep 2026 09:09:27 GMT";
    core::HttpResponse second_reply;
    second_reply.status = 304;
    core::HttpResponse third_reply;
    third_reply.status = 200;
    third_reply.body = "invalid XML";
    http.replies = {first_reply, second_reply, third_reply};
    NewsFeed feed(http);
    auto first = feed.fetch();
    ASSERT_TRUE(first.has_value());
    auto second = feed.fetch();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->size(), first->size());
    EXPECT_EQ(http.requests[1].if_none_match, "\"abc\"");
    EXPECT_EQ(http.requests[1].if_modified_since, "Tue, 22 Sep 2026 09:09:27 GMT");
    EXPECT_EQ(http.requests[0].allowed_hosts, (std::vector<std::string>{"archlinux.org"}));
    EXPECT_EQ(http.requests[0].max_bytes, 1024U * 1024);
    EXPECT_FALSE(feed.fetch());
}

TEST(News, DISABLED_LiveArchRSS) {
    core::CurlHttpClient http;
    NewsFeed feed(http);
    auto result = feed.fetch();
    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->empty());
}

}
}
