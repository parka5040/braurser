#ifndef BRAURSER_NOTIFIER_POLICY_HPP
#define BRAURSER_NOTIFIER_POLICY_HPP

#include <braurser/protocol/dto.hpp>
#include <chrono>
#include <optional>

namespace braurser::notifier {

std::optional<std::chrono::seconds> interval(const protocol::Settings& settings);

}

#endif
