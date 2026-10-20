#ifndef BRAURSER_DAEMON_API_SERVER_HPP
#define BRAURSER_DAEMON_API_SERVER_HPP

#include <braurser/daemon/api/registry.hpp>
#include <braurser/daemon/api/logger.hpp>
#include <braurser/protocol/frame.hpp>
#include <giomm/socket.h>
#include <glibmm/main.h>
#include <filesystem>
#include <expected>
#include <map>
#include <memory>
#include <deque>
#include <vector>
#include <sys/types.h>

namespace braurser::daemon::api {

class PeerCredentials {
public:
    virtual ~PeerCredentials() = default;
    /// in: fd is an accepted Unix-domain socket; expected_uid is this daemon's uid.
    /// out: true only for a peer with matching SO_PEERCRED uid.
    virtual bool allowed(int fd, uid_t expected_uid) const = 0;
};

class PosixPeerCredentials final : public PeerCredentials {
public:
    bool allowed(int fd, uid_t expected_uid) const override;
};

struct ServerOptions {
    unsigned first_request_timeout_ms{30000};
    unsigned frame_timeout_ms{30000};
};

class Server {
public:
    /// in: runtime is private; socket_path defaults to runtime/braurser/core.sock; options bound request and frame deadlines.
    Server(HandlerRegistry& registry, std::filesystem::path runtime,
           std::filesystem::path socket_path = {}, const PeerCredentials* peers = nullptr,
           DaemonLog* log = nullptr, ServerOptions options = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// out: creates the locked private socket or returns a safe startup error.
    std::expected<void, std::string> start();
    /// pre: start succeeded; runs the main event loop until shutdown.
    void run();
    /// out: closes the listener, drains queued responses and unlinks our socket.
    void stop();

    /// in: notification is a validated public protocol event.
    /// out: slow clients are disconnected when their queued output exceeds four MiB.
    void broadcast(const protocol::Notification& notification);
    /// in: connection identifies the exact authenticated recipient.
    void emit_to(ConnectionId connection, const protocol::Notification& notification);
    /// in: callback is invoked when an authenticated connection closes.
    void on_disconnect(std::function<void(ConnectionId)> callback);

private:
    struct Client;
    HandlerRegistry& registry_;
    std::filesystem::path runtime_;
    std::filesystem::path path_;
    bool custom_socket_{};
    PosixPeerCredentials default_peers_;
    const PeerCredentials* peers_;
    DaemonLog* log_;
    Glib::RefPtr<Glib::MainLoop> loop_;
    Glib::RefPtr<Gio::Socket> listener_;
    sigc::connection accept_source_;
    sigc::connection accept_retry_;
    sigc::connection shutdown_source_;
    std::map<int, std::shared_ptr<Client>> clients_;
    ConnectionId next_connection_{};
    std::vector<std::function<void(ConnectionId)>> disconnect_callbacks_;
    int lock_fd_{-1};
    dev_t socket_dev_{};
    ino_t socket_ino_{};
    bool stopping_{};
    ServerOptions options_;
    std::shared_ptr<int> lifetime_{std::make_shared<int>(0)};

    bool accept_ready(Glib::IOCondition condition);
    void watch_listener();
    bool client_ready(const std::shared_ptr<Client>& client, Glib::IOCondition condition);
    void close_client(int fd);
    void defer_close(const std::shared_ptr<Client>& client);
    void watch_frame_deadline(const std::shared_ptr<Client>& client);
    void queue_frame(const std::shared_ptr<Client>& client, std::shared_ptr<const std::string> frame);
    void queue(const std::shared_ptr<Client>& client, const protocol::Message& message);
    void process(const std::shared_ptr<Client>& client, std::string_view frame);
    void finish();
    std::vector<std::shared_ptr<Client>> connected() const;
    bool still_connected(const std::shared_ptr<Client>& client) const;
};

}

#endif
