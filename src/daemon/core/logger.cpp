#include <braurser/daemon/core/logger.hpp>
#include <braurser/daemon/core/text.hpp>

namespace braurser::daemon::core {

RedactingRingLogger::RedactingRingLogger(std::size_t capacity, std::size_t max_entry_bytes)
    : capacity_(capacity), max_entry_bytes_(max_entry_bytes) {}

void RedactingRingLogger::log(std::string_view entry) {
    if (capacity_ == 0) return;
    auto clean = bounded_line(redact(strip_terminal_controls(entry)), max_entry_bytes_);
    std::lock_guard lock(mutex_);
    if (entries_.size() == capacity_) entries_.pop_front();
    entries_.push_back(std::move(clean));
}

std::vector<std::string> RedactingRingLogger::snapshot() const {
    std::lock_guard lock(mutex_);
    return {entries_.begin(), entries_.end()};
}

}
