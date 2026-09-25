#ifndef BRAURSER_DAEMON_API_BROADCASTER_HPP
#define BRAURSER_DAEMON_API_BROADCASTER_HPP

#include <braurser/daemon/api/registry.hpp>
#include <braurser/daemon/api/server.hpp>
#include <optional>
#include <type_traits>

namespace braurser::daemon::api {

class Broadcaster {
public:
    explicit Broadcaster(Server& server) : server_(server) {}

    /// in: payload is a typed nonprivate notification.
    template<class Notification>
        requires (!std::is_same_v<Notification, JobQuestionNotificationEvent>)
    void emit(const typename Notification::Payload& payload) {
        server_.broadcast(protocol::Notification{std::string(Notification::wire_name), nlohmann::json(payload)});
    }

    /// in: payload is private and owner identifies its authenticated connection.
    template<class Notification>
    void emit_to(ConnectionId owner, const typename Notification::Payload& payload) {
        server_.emit_to(owner, protocol::Notification{std::string(Notification::wire_name), nlohmann::json(payload)});
    }

private:
    Server& server_;
};

}

#endif
