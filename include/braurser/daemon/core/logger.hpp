#ifndef BRAURSER_DAEMON_CORE_LOGGER_HPP
#define BRAURSER_DAEMON_CORE_LOGGER_HPP

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::core {

class Logger {
public:
    virtual ~Logger() = default;
    /// in: entry may contain credentials or terminal control sequences.
    virtual void log(std::string_view entry) = 0;
};

class RedactingRingLogger final : public Logger {
public:
    explicit RedactingRingLogger(std::size_t capacity, std::size_t max_entry_bytes = 4096);
    void log(std::string_view entry) override;
    /// out: returns a chronological copy of the retained redacted entries.
    std::vector<std::string> snapshot() const;

private:
    std::size_t capacity_;
    std::size_t max_entry_bytes_;
    mutable std::mutex mutex_;
    std::deque<std::string> entries_;
};

}

#endif
