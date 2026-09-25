#ifndef BRAURSER_DAEMON_NEWS_FEED_HPP
#define BRAURSER_DAEMON_NEWS_FEED_HPP

#include <braurser/daemon/core/http.hpp>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::news {

struct Item {
    std::string title;
    std::string link;
    std::string published;
    std::string summary;
    bool requires_manual_intervention{false};
};

/// in: repos contains configured repository names.
/// out: true only when an official Arch repository is configured.
bool arch_news_applicable(const std::vector<std::string>& repos);

/// in: xml contains at most one MiB of untrusted RSS XML.
/// out: verified items with HTTPS Arch links and sanitized summaries.
/// err: an invalid feed produces no items.
std::expected<std::vector<Item>, std::string> parse_rss(std::string_view xml);

class NewsFeed {
public:
    explicit NewsFeed(core::HttpClient& http);
    /// out: fetches Arch RSS with conditional headers and retains the last valid feed on 304.
    /// err: HTTP or XML errors do not replace the last valid feed.
    std::expected<std::vector<Item>, std::string> fetch();

private:
    core::HttpClient& http_;
    std::optional<std::string> etag_;
    std::optional<std::string> last_modified_;
    std::optional<std::vector<Item>> saved_;
};

}

#endif
