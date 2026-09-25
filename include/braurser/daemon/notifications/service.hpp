#ifndef BRAURSER_DAEMON_NOTIFICATIONS_SERVICE_HPP
#define BRAURSER_DAEMON_NOTIFICATIONS_SERVICE_HPP

#include <braurser/daemon/api/registry.hpp>
#include <braurser/protocol/dto.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace braurser::daemon::app { class RealServices; }

namespace braurser::daemon::notifications {

struct Payload {
    std::string app_name;
    std::string icon;
    std::string summary;
    std::string body;
    std::vector<std::string> actions;
};

class NotificationTransport {
public:
    virtual ~NotificationTransport() = default;
    virtual bool send(const Payload&) = 0;
};

class DesktopTransport final : public NotificationTransport {
public:
    explicit DesktopTransport(std::filesystem::path launcher);
    ~DesktopTransport() override;
    bool send(const Payload&) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class NotificationService {
public:
    NotificationService(std::filesystem::path state, NotificationTransport& transport);
    /// in: report holds only update identifiers; now is the notification day.
    /// out: sends once per changed update set per day, returning true only when sent.
    bool notify(const protocol::UpdateReport& report,
                std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
    /// in: count is the number of updates and never external copy.
    /// out: returns only fixed translated copy and its numeric plural.
    static Payload payload(std::uint64_t count);
private:
    std::filesystem::path state_;
    NotificationTransport& transport_;
};

/// in: services remains alive while the registry handles requests.
/// out: binds the notifier endpoint to the desktop service.
void register_service(api::HandlerRegistry& registry, app::RealServices& services,
                      const std::filesystem::path& state, const std::filesystem::path& launcher);

}

#endif
