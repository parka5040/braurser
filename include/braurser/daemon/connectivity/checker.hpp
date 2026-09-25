#ifndef BRAURSER_DAEMON_CONNECTIVITY_CHECKER_HPP
#define BRAURSER_DAEMON_CONNECTIVITY_CHECKER_HPP

#include <braurser/daemon/core/clock.hpp>
#include <braurser/daemon/core/http.hpp>
#include <chrono>
#include <optional>

namespace braurser::daemon::connectivity {

enum class Status { Online, Offline, Unknown };

class NetworkManagerStatus {
public:
    virtual ~NetworkManagerStatus() = default;
    /// out: online, offline, or unknown when NM is absent or reports unknown.
    virtual Status query() = 0;
};

class GioNetworkManagerStatus final : public NetworkManagerStatus {
public:
    Status query() override;
};

class ConnectivityChecker {
public:
    ConnectivityChecker(NetworkManagerStatus& nm, core::HttpClient& http, const core::Clock& clock);
    /// out: a 60-second cached status; unknown NM status is probed over HTTPS.
    Status check();

private:
    NetworkManagerStatus& nm_;
    core::HttpClient& http_;
    const core::Clock& clock_;
    std::optional<Status> cached_;
    std::chrono::system_clock::time_point checked_at_{};
};

}

#endif
