#ifndef BRAURSER_DAEMON_CORE_CLOCK_HPP
#define BRAURSER_DAEMON_CORE_CLOCK_HPP

#include <chrono>

namespace braurser::daemon::core {

class Clock {
public:
    virtual ~Clock() = default;
    /// out: returns current wall-clock time.
    virtual std::chrono::system_clock::time_point now() const = 0;
};

class SystemClock final : public Clock {
public:
    std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }
};

}

#endif
