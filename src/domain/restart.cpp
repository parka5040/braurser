#include <braurser/domain/restart.hpp>
#include <algorithm>
#include <string_view>

namespace braurser::domain {
namespace {

int severity(RestartLevel level) {
    switch (level) {
    case RestartLevel::None: return 0;
    case RestartLevel::Unknown: return 1;
    case RestartLevel::RestartApp: return 2;
    case RestartLevel::RestartService: return 3;
    case RestartLevel::Logout: return 4;
    case RestartLevel::Reboot: return 5;
    }
    return 1;
}

bool glob(std::string_view pattern, std::string_view name) {
    if (pattern.empty() || pattern.size() > 255) return false;
    std::size_t p = 0, n = 0, star = std::string_view::npos, restart = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            restart = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++restart;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool matches(const RestartRule& rule, const PackageChange& change) {
    const bool name = std::any_of(rule.name_patterns.begin(), rule.name_patterns.end(), [&](const auto& pattern) {
        return glob(pattern, change.name.value());
    });
    if (name) return true;
    return std::any_of(rule.path_prefixes.begin(), rule.path_prefixes.end(), [&](const auto& prefix) {
        if (prefix.empty() || prefix.size() > 4096 || prefix[0] != '/') return false;
        return std::any_of(change.files.begin(), change.files.end(), [&](const auto& file) {
            return file.size() <= 4096 && std::string_view(file).starts_with(prefix);
        });
    });
}

}

RestartAdvice classify(const PackageChange& change, std::span<const RestartRule> rules) {
    RestartAdvice result{change.name, RestartLevel::Unknown, {}, {}};
    bool found = false;
    for (const auto& rule : rules) {
        if (rule.requires_running && !change.has_running_consumer) continue;
        if (!matches(rule, change)) continue;
        RestartLevel level = rule.level;
        if (rule.requires_running && level == RestartLevel::RestartService && !change.consumer_is_service)
            level = RestartLevel::RestartApp;
        if (!found || severity(level) > severity(result.level)) {
            result.level = level;
            result.rule_id = rule.id;
            result.reason_key = rule.reason_key;
            found = true;
        }
    }
    return result;
}

RestartLevel overall(std::span<const RestartAdvice> advices) {
    RestartLevel result = RestartLevel::Unknown;
    bool found = false;
    for (const auto& advice : advices) {
        if (!found || severity(advice.level) > severity(result)) result = advice.level;
        found = true;
    }
    return result;
}

}
