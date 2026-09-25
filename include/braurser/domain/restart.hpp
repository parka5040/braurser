#ifndef BRAURSER_DOMAIN_RESTART_HPP
#define BRAURSER_DOMAIN_RESTART_HPP

#include <braurser/domain/types.hpp>
#include <span>
#include <string>
#include <vector>

namespace braurser::domain {

enum class RestartLevel { None, RestartApp, RestartService, Logout, Reboot, Unknown };

struct RestartRule {
    std::string id;
    RestartLevel level;
    std::vector<std::string> name_patterns;
    std::vector<std::string> path_prefixes;
    bool requires_running;
    std::string reason_key;
};

struct PackageChange {
    PackageName name;
    std::vector<std::string> files;
    bool has_running_consumer;
    bool consumer_is_service;
};

struct RestartAdvice {
    PackageName name;
    RestartLevel level;
    std::string rule_id;
    std::string reason_key;
};

/// in: change and rules contain package and matching rules.
/// out: highest severity match or Unknown if no rule matches.
RestartAdvice classify(const PackageChange& change, std::span<const RestartRule> rules);

/// in: advices contains zero or more classification results.
/// out: highest severity advice, or Unknown when empty.
RestartLevel overall(std::span<const RestartAdvice> advices);

}

#endif
