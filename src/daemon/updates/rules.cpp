#include <braurser/daemon/updates/updates.hpp>
#include <braurser/daemon/core/files.hpp>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace braurser::daemon::updates {
namespace {
using Json = nlohmann::json;
UpdateError invalid() { return {UpdateErrorCode::InvalidOutput, "invalid restart rules"}; }
bool token(std::string_view value, std::size_t max) {
    return !value.empty() && value.size() <= max &&
        std::ranges::all_of(value, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        });
}
std::expected<std::vector<std::string>, UpdateError> patterns(const Json& object, const char* key, bool paths) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() > 200)
        return std::unexpected(invalid());
    std::vector<std::string> result;
    for (const auto& item : object.at(key)) {
        if (!item.is_string()) return std::unexpected(invalid());
        const auto text = item.get<std::string>();
        if (text.empty() || text.size() > (paths ? 4096U : 255U) ||
            (paths && (text.front() != '/' || text.find("..") != std::string::npos)) ||
            !std::ranges::all_of(text, [paths](unsigned char c) {
                return c >= 0x21 && c <= 0x7e && (paths || c != '/');
            })) return std::unexpected(invalid());
        result.push_back(text);
    }
    return result;
}
}
std::expected<std::vector<domain::RestartRule>, UpdateError> RestartRuleLoader::load(
    const std::filesystem::path& path) const {
    auto contents = core::read_bounded(path, 256 * 1024);
    if (!contents) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "cannot read restart rules"});
    auto json = Json::parse(*contents, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.size() != 2 ||
        !json.contains("version") || !json.at("version").is_number_integer() ||
        json.at("version") != 1 || !json.contains("rules") ||
        !json.at("rules").is_array() || json.at("rules").size() > 500)
        return std::unexpected(invalid());
    std::vector<domain::RestartRule> rules;
    std::set<std::string> seen;
    for (const auto& item : json.at("rules")) {
        if (!item.is_object() || item.size() != 6 || !item.contains("id") || !item.at("id").is_string() ||
            !item.contains("level") || !item.at("level").is_string() ||
            !item.contains("requires_running") || !item.at("requires_running").is_boolean() ||
            !item.contains("reason") || !item.at("reason").is_string()) return std::unexpected(invalid());
        const auto id = item.at("id").get<std::string>();
        const auto level = item.at("level").get<std::string>();
        const auto reason = item.at("reason").get<std::string>();
        if (!token(id, 128) || !seen.insert(id).second ||
            !reason.starts_with("restart.reason.") || !token(reason.substr(15), 128))
            return std::unexpected(invalid());
        domain::RestartLevel parsed;
        if (level == "none") parsed = domain::RestartLevel::None;
        else if (level == "restart-app") parsed = domain::RestartLevel::RestartApp;
        else if (level == "restart-service") parsed = domain::RestartLevel::RestartService;
        else if (level == "logout") parsed = domain::RestartLevel::Logout;
        else if (level == "reboot") parsed = domain::RestartLevel::Reboot;
        else return std::unexpected(invalid());
        auto names = patterns(item, "names", false);
        if (!names) return std::unexpected(names.error());
        auto paths = patterns(item, "paths", true);
        if (!paths || names->size() + paths->size() > 200 || (names->empty() && paths->empty()))
            return std::unexpected(invalid());
        rules.push_back({id, parsed, std::move(*names), std::move(*paths),
            item.at("requires_running").get<bool>(), reason});
    }
    return rules;
}
}
