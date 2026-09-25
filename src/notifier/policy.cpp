#include "policy.hpp"

namespace braurser::notifier {

std::optional<std::chrono::seconds> interval(const protocol::Settings& settings) {
    if (!settings.autostart || !settings.notifications) return std::nullopt;
    if (settings.check_frequency.value == "daily") return std::chrono::hours(24);
    if (settings.check_frequency.value == "weekly") return std::chrono::hours(24 * 7);
    return std::nullopt;
}

}
