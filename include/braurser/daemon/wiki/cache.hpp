#ifndef BRAURSER_DAEMON_WIKI_CACHE_HPP
#define BRAURSER_DAEMON_WIKI_CACHE_HPP

#include <braurser/daemon/core/clock.hpp>
#include <braurser/daemon/wiki/document.hpp>
#include <filesystem>
#include <optional>
#include <string_view>

namespace braurser::daemon::wiki {

struct CachedArticle {
    std::optional<Document> document;
    bool stale{false};
};

class WikiCache {
public:
    /// in: cache_root is an absolute XDG cache directory.
    WikiCache(std::filesystem::path cache_root, const core::Clock& clock);
    /// in: title is a bounded candidate title.
    /// out: a verified cache entry, or none if unavailable or corrupt.
    std::optional<CachedArticle> read(std::string_view title, std::string_view package_name = {}) const;
    /// in: title identifies a successful lookup or a negative lookup for package_name.
    /// out: writes a private atomic cache file.
    bool write(std::string_view title, const std::optional<Document>& document,
               std::string_view package_name = {}) const;

private:
    std::filesystem::path root_;
    const core::Clock& clock_;
};

}

#endif
