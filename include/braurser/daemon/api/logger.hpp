#ifndef BRAURSER_DAEMON_API_LOGGER_HPP
#define BRAURSER_DAEMON_API_LOGGER_HPP

#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace braurser::daemon::api {

class DaemonLog {
public:
    /// in: state_home is an absolute private XDG state directory.
    explicit DaemonLog(const std::filesystem::path& state_home);

    /// in: values are untrusted and redacted before writing; keys are fixed diagnostic labels.
    /// out: writes one bounded structured line to a private rotating log.
    void write(std::string_view level, std::string_view component, std::string_view event,
               std::initializer_list<std::pair<std::string_view, std::string_view>> values = {});

private:
    std::filesystem::path directory_;
    std::mutex mutex_;
};

}

#endif
