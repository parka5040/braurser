#ifndef BRAURSER_DAEMON_WIKI_CLIENT_HPP
#define BRAURSER_DAEMON_WIKI_CLIENT_HPP

#include <braurser/daemon/core/clock.hpp>
#include <braurser/daemon/core/http.hpp>
#include <braurser/daemon/wiki/cache.hpp>
#include <braurser/daemon/wiki/document.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::wiki {

class ArticleResolver {
public:
    /// in: display name, package name and base name are untrusted.
    /// out: distinct, bounded candidates in deterministic priority order.
    static std::vector<std::string> candidates(std::string_view display_name,
                                                std::string_view package_name,
                                                std::string_view package_base);
    /// in: name is untrusted user-visible text.
    /// out: an encoded HTTPS ArchWiki search URL, or empty on invalid input.
    static std::string search_url(std::string_view name);
};

enum class LookupStatus { Found, NotFound, Error };
struct LookupResult {
    LookupStatus status{LookupStatus::NotFound};
    std::optional<Document> document;
    bool stale{false};
};

class WikiClient {
public:
    WikiClient(core::HttpClient& http, const core::Clock& clock, WikiCache* cache = nullptr);
    /// in: online is false when connectivity disallows fetches; package_name must be valid.
    /// out: returns only deterministic title, redirect, or exact package-link matches.
    LookupResult lookup(std::string_view display_name, std::string_view package_name,
                        std::string_view package_base, bool online = true);

private:
    core::HttpClient& http_;
    const core::Clock& clock_;
    WikiCache* cache_;
};

}

#endif
