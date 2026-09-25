#include <braurser/daemon/wiki/client.hpp>
#include <braurser/daemon/wiki/sanitizer.hpp>
#include <braurser/validate/validate.hpp>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <string>
#include <string_view>

namespace braurser::daemon::wiki {
namespace {

bool valid_title(std::string_view value) {
    return !value.empty() && value.size() <= 255 &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::string percent_encode(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const char raw_c : value) {
        const auto c = static_cast<unsigned char>(raw_c);
        if ((std::isalnum(c) && c < 128) || c == '-' || c == '_' || c == '.' || c == '~') result += static_cast<char>(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

std::string normalize(std::string_view value) {
    std::string result(value);
    for (auto& c : result) c = c == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

bool has_package_link(const Document& doc, std::string_view package) {
    const auto official = "https://archlinux.org/packages/?name=" + percent_encode(package);
    const auto aur = "https://aur.archlinux.org/packages/" + percent_encode(package);
    const auto matches = [&](const std::vector<Inline>& inlines) {
        return std::any_of(inlines.begin(), inlines.end(), [&](const Inline& inline_value) {
            const auto* link = std::get_if<Link>(&inline_value);
            return link && (link->href == official || link->href == aur || link->href == aur + "/");
        });
    };
    for (const auto& block : doc.blocks) {
        if (const auto* p = std::get_if<Paragraph>(&block); p && matches(p->inlines)) return true;
        if (const auto* n = std::get_if<Note>(&block); n && matches(n->inlines)) return true;
        if (const auto* w = std::get_if<Warning>(&block); w && matches(w->inlines)) return true;
        if (const auto* list = std::get_if<List>(&block))
            for (const auto& item : list->items) if (matches(item)) return true;
    }
    return false;
}

}

std::vector<std::string> ArticleResolver::candidates(std::string_view display_name,
                                                      std::string_view package_name,
                                                      std::string_view package_base) {
    std::vector<std::string> result;
    const auto add = [&](std::string_view candidate) {
        if (valid_title(candidate) &&
            std::find(result.begin(), result.end(), candidate) == result.end()) result.emplace_back(candidate);
    };
    add(display_name);
    if (!br_valid_pkgname(package_name.data(), package_name.size())) return result;
    add(package_name);
    if (br_valid_pkgname(package_base.data(), package_base.size())) {
        static constexpr std::array<std::string_view, 7> suffixes{
            "-bin", "-git", "-appimage", "-nightly", "-beta", "-stable", "-wayland"};
        std::string_view stripped = package_base;
        for (const auto suffix : suffixes) {
            if (package_base.ends_with(suffix) && package_base.size() > suffix.size()) {
                stripped = package_base.substr(0, package_base.size() - suffix.size());
                break;
            }
        }
        add(stripped);
    }
    return result;
}

std::string ArticleResolver::search_url(std::string_view name) {
    if (!valid_title(name)) return {};
    return "https://wiki.archlinux.org/index.php?search=" + percent_encode(name);
}

WikiClient::WikiClient(core::HttpClient& http, const core::Clock& clock, WikiCache* cache)
    : http_(http), clock_(clock), cache_(cache) {}

LookupResult WikiClient::lookup(std::string_view display_name, std::string_view package_name,
                                std::string_view package_base, bool online) {
    if (!br_valid_pkgname(package_name.data(), package_name.size())) return {LookupStatus::Error, {}, false};
    const auto names = ArticleResolver::candidates(display_name, package_name, package_base);
    bool failure = false;
    std::optional<Document> stale;
    for (const auto& candidate : names) {
        const auto cached = cache_ ? cache_->read(candidate, package_name) : std::nullopt;
        if (cached && cached->document && !cached->stale) return {LookupStatus::Found, cached->document, false};
        if (cached && cached->document && !online) return {LookupStatus::Found, cached->document, true};
        if (cached && cached->document && !stale) stale = cached->document;
        if (!online) continue;
        if (cached && !cached->document) continue;
        core::HttpRequest request;
        request.url = "https://wiki.archlinux.org/api.php?action=parse&page=" + percent_encode(candidate) +
                      "&redirects=1&section=0&prop=text%7Csections%7Cdisplaytitle&format=json&formatversion=2&maxlag=5";
        request.allowed_hosts = {"wiki.archlinux.org"};
        request.max_bytes = 2 * 1024 * 1024;
        request.timeout = std::chrono::seconds(10);
        request.user_agent = "braurser/0.1 (+https://wiki.archlinux.org/title/User_agent)";
        const auto response = http_.get(request);
        if (!response || response->status != 200 || response->body.size() > request.max_bytes) {
            failure = true;
            continue;
        }
        try {
            const auto data = nlohmann::json::parse(response->body);
            if (data.contains("error")) {
                if (data.at("error").at("code") == "missingtitle") {
                    if (cache_) cache_->write(candidate, std::nullopt, package_name);
                } else failure = true;
                continue;
            }
            const auto& page = data.at("parse");
            const auto title = page.at("title").get<std::string>();
            const auto html = page.at("text").get<std::string>();
            if (!valid_title(title) || html.size() > request.max_bytes) { failure = true; continue; }
            bool matched = normalize(title) == normalize(candidate);
            if (page.contains("redirects") && page.at("redirects").is_array()) {
                for (const auto& redirect : page.at("redirects")) {
                    if (normalize(redirect.at("from").get<std::string>()) == normalize(candidate)) matched = true;
                }
            }
            auto doc = sanitize(html);
            if (!matched) matched = has_package_link(doc, package_name);
            if (!matched) {
                if (cache_) cache_->write(candidate, std::nullopt, package_name);
                continue;
            }
            doc.title = title;
            doc.source_url = "https://wiki.archlinux.org/title/" + percent_encode(title);
            doc.fetched_at = clock_.now();
            if (cache_) cache_->write(candidate, doc, package_name);
            return {LookupStatus::Found, std::move(doc), false};
        } catch (const nlohmann::json::exception&) { failure = true; }
    }
    if (stale) return {LookupStatus::Found, std::move(stale), true};
    return {failure ? LookupStatus::Error : LookupStatus::NotFound, {}, false};
}

}
