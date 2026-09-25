#include <braurser/daemon/news/feed.hpp>
#include <braurser/daemon/wiki/sanitizer.hpp>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace braurser::daemon::news {
namespace {

std::string xml_text(xmlNode* node) {
    auto* content = xmlNodeGetContent(node);
    if (!content) return {};
    std::string value(reinterpret_cast<const char*>(content));
    xmlFree(content);
    return value;
}

std::string child_text(xmlNode* node, const char* name) {
    for (auto* child = node->children; child; child = child->next)
        if (child->type == XML_ELEMENT_NODE && xmlStrEqual(child->name, BAD_CAST name)) return xml_text(child);
    return {};
}

std::string rfc3339(std::string_view input) {
    if (input.size() > 128) return {};
    std::string date(input);
    char weekday[4]{}, month[4]{}, zone[8]{};
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    int end = 0;
    if (std::sscanf(date.c_str(), "%3[a-zA-Z], %d %3[a-zA-Z] %d %d:%d:%d %7s%n",
                    weekday, &day, month, &year, &hour, &minute, &second, zone, &end) != 8 ||
        end != static_cast<int>(date.size())) return {};
    constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto found = std::find(months.begin(), months.end(), std::string_view(month));
    if (found == months.end() || year < 1970 || year > 9999 || hour > 23 || hour < 0 ||
        minute > 59 || minute < 0 || second > 59 || second < 0) return {};
    int offset = 0;
    constexpr std::array<std::pair<std::string_view, int>, 11> named_zones{{
        {"GMT", 0}, {"UT", 0}, {"UTC", 0}, {"EST", -300}, {"EDT", -240},
        {"CST", -360}, {"CDT", -300}, {"MST", -420}, {"MDT", -360},
        {"PST", -480}, {"PDT", -420}
    }};
    const auto named = std::find_if(named_zones.begin(), named_zones.end(), [&](const auto& entry) {
        return entry.first == zone;
    });
    if (named != named_zones.end()) offset = named->second;
    else {
        if (std::char_traits<char>::length(zone) != 5 || (zone[0] != '+' && zone[0] != '-') ||
            !std::all_of(zone + 1, zone + 5, [](unsigned char c) { return std::isdigit(c); })) return {};
        const int hours = (zone[1] - '0') * 10 + zone[2] - '0';
        const int minutes = (zone[3] - '0') * 10 + zone[4] - '0';
        if (hours > 23 || minutes > 59) return {};
        offset = (hours * 60 + minutes) * (zone[0] == '+' ? 1 : -1);
    }
    using namespace std::chrono;
    const year_month_day ymd{std::chrono::year(year), std::chrono::month(static_cast<unsigned>(found - months.begin() + 1)),
                             std::chrono::day(static_cast<unsigned>(day))};
    if (!ymd.ok()) return {};
    const auto time = sys_days(ymd) + hours(hour) + minutes(minute) + seconds(second) - minutes(offset);
    const auto utc_day = floor<days>(time);
    const year_month_day utc_date(utc_day);
    const auto tod = hh_mm_ss(time - utc_day);
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02lld:%02lld:%02lldZ",
                  static_cast<int>(utc_date.year()), static_cast<unsigned>(utc_date.month()),
                  static_cast<unsigned>(utc_date.day()), static_cast<long long>(tod.hours().count()),
                  static_cast<long long>(tod.minutes().count()), static_cast<long long>(tod.seconds().count()));
    return buffer;
}

bool valid_link(std::string_view link) {
    if (!link.starts_with("https://archlinux.org/") || link.size() > 8192) return false;
    return std::none_of(link.begin(), link.end(), [](unsigned char c) { return c <= 32 || c == 127; });
}

std::string normalize_title(std::string_view raw) {
    std::string result;
    bool space = false;
    for (const char raw_c : raw) {
        const auto c = static_cast<unsigned char>(raw_c);
        if (c == 127) continue;
        if (c < 33) { space = true; continue; }
        if (space && !result.empty()) result += ' ';
        space = false;
        result += static_cast<char>(c);
        if (result.size() > wiki::max_block_text) return {};
    }
    return result;
}

std::string summary(std::string_view html) {
    const auto doc = wiki::sanitize(html);
    for (const auto& block : doc.blocks)
        if (const auto* para = std::get_if<wiki::Paragraph>(&block)) return wiki::inline_text(para->inlines);
    return {};
}

}

bool arch_news_applicable(const std::vector<std::string>& repos) {
    return std::any_of(repos.begin(), repos.end(), [](const auto& repo) {
        return repo == "core" || repo == "extra" || repo == "multilib";
    });
}

std::expected<std::vector<Item>, std::string> parse_rss(std::string_view xml) {
    if (xml.empty() || xml.size() > 1024 * 1024 || xml.find('\0') != std::string_view::npos)
        return std::unexpected("RSS size or bytes refused");
    const int options = XML_PARSE_NONET | XML_PARSE_NO_XXE | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
    auto parsed = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>(
        xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr, options), xmlFreeDoc);
    if (!parsed || parsed->intSubset || parsed->extSubset)
        return std::unexpected("invalid RSS XML");
    auto* root = xmlDocGetRootElement(parsed.get());
    if (!root || !xmlStrEqual(root->name, BAD_CAST "rss")) return std::unexpected("not RSS");
    xmlNode* channel = nullptr;
    for (auto* node = root->children; node; node = node->next)
        if (node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST "channel")) { channel = node; break; }
    if (!channel) return std::unexpected("RSS channel missing");
    std::vector<Item> result;
    for (auto* node = channel->children; node && result.size() < 100; node = node->next) {
        if (node->type != XML_ELEMENT_NODE || !xmlStrEqual(node->name, BAD_CAST "item")) continue;
        auto title = normalize_title(child_text(node, "title"));
        auto link = child_text(node, "link");
        auto published = rfc3339(child_text(node, "pubDate"));
        auto description = child_text(node, "description");
        if (title.empty() || title.size() > wiki::max_block_text || !valid_link(link) || published.empty() ||
            description.size() > 2 * 1024 * 1024) continue;
        auto lower = title;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        result.push_back(Item{std::move(title), std::move(link), std::move(published), summary(description),
                              lower.find("manual intervention") != std::string::npos});
    }
    return result;
}

NewsFeed::NewsFeed(core::HttpClient& http) : http_(http) {}

std::expected<std::vector<Item>, std::string> NewsFeed::fetch() {
    core::HttpRequest request;
    request.url = "https://archlinux.org/feeds/news/";
    request.allowed_hosts = {"archlinux.org"};
    request.max_bytes = 1024 * 1024;
    request.timeout = std::chrono::seconds(10);
    request.if_none_match = etag_;
    request.if_modified_since = last_modified_;
    const auto response = http_.get(request);
    if (!response) return std::unexpected("news transport failed");
    if (response->status == 304) {
        if (saved_) return *saved_;
        return std::unexpected("news cache missing");
    }
    if (response->status != 200 || response->body.size() > request.max_bytes)
        return std::unexpected("news response refused");
    auto items = parse_rss(response->body);
    if (!items) return items;
    saved_ = *items;
    etag_ = response->etag;
    last_modified_ = response->last_modified;
    return items;
}

}
