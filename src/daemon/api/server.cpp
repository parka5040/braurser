#include <braurser/daemon/api/server.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/protocol/dto.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace braurser::daemon::api {
namespace {
constexpr std::size_t max_queue = 4 * 1024 * 1024;
constexpr std::size_t max_pending = 64;

bool private_runtime(const std::filesystem::path& path) {
    if (!path.is_absolute() || path == "/" || path.lexically_normal() != path) return false;
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
           info.st_uid == getuid() && (info.st_mode & 0777) == 0700;
}

std::optional<protocol::RequestId> request_id(std::string_view bytes) {
    try {
        auto value = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (!value.is_object() || !value.contains("id")) return std::nullopt;
        const auto& id = value.at("id");
        if (id.is_string()) {
            auto text = id.get<std::string>();
            if (!text.empty() && text.size() <= 64 && protocol::valid_utf8(text)) return text;
        } else if (id.is_number_integer() &&
                   (!id.is_number_unsigned() || id.get<std::uint64_t>() <=
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
            return id.get<std::int64_t>();
        }
    } catch (const std::exception&) {}
    return std::nullopt;
}
}

bool PosixPeerCredentials::allowed(int fd, uid_t expected_uid) const {
    struct ucred credentials{};
    socklen_t size = sizeof(credentials);
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
           size == sizeof(credentials) && credentials.uid == expected_uid;
}

struct Server::Client {
    Client(Glib::RefPtr<Gio::Socket> value, ConnectionId identity)
        : socket(std::move(value)), id(identity) {}
    Glib::RefPtr<Gio::Socket> socket;
    ConnectionId id;
    protocol::FrameDecoder decoder;
    std::deque<std::string> output;
    std::size_t queued{};
    std::size_t sent{};
    std::size_t pending{};
    sigc::connection input;
    sigc::connection writable;
};

Server::Server(HandlerRegistry& registry, std::filesystem::path runtime,
               std::filesystem::path socket_path, const PeerCredentials* peers, DaemonLog* log)
    : registry_(registry), runtime_(std::move(runtime)),
      path_(socket_path.empty() ? runtime_ / "braurser" / "core.sock" : socket_path),
      custom_socket_(!socket_path.empty()),
      peers_(peers ? peers : &default_peers_), log_(log), loop_(Glib::MainLoop::create()) {}

Server::~Server() {
    stop();
    for (auto it = clients_.begin(); it != clients_.end();) {
        const int fd = it->first;
        ++it;
        close_client(fd);
    }
    if (lock_fd_ >= 0) close(lock_fd_);
}

std::expected<void, std::string> Server::start() {
    if (listener_ || lock_fd_ >= 0) return std::unexpected("server already started");
    if (!private_runtime(runtime_)) return std::unexpected("XDG runtime directory is not private");
    if (!path_.is_absolute() || path_.lexically_normal() != path_ || path_.filename().empty() ||
        path_.native().size() >= sizeof(sockaddr_un::sun_path) ||
        (!custom_socket_ && path_ != runtime_ / "braurser" / "core.sock"))
        return std::unexpected("invalid socket path");
    if (path_.parent_path() != runtime_) {
        if (custom_socket_ && !private_runtime(path_.parent_path()))
            return std::unexpected("test socket directory is not private");
        if (auto dir = core::ensure_private_dir(path_.parent_path()); !dir)
            return std::unexpected("cannot create private socket directory");
    }
    const auto lock_path = path_.parent_path() / "core.lock";
    struct stat info{};
    if (lstat(lock_path.c_str(), &info) == 0 &&
        (!S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 0777) != 0600))
        return std::unexpected("unsafe daemon lock file");
    lock_fd_ = open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_CREAT, 0600);
    if (lock_fd_ < 0 || fstat(lock_fd_, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0600 ||
        flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
        return std::unexpected("daemon lock unavailable");
    if (lstat(path_.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode) || info.st_uid != getuid())
            return std::unexpected("unsafe existing socket");
        const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (probe < 0) return std::unexpected("cannot probe existing socket");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.native().size() + 1);
        const int connected = connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        const int status = errno;
        close(probe);
        if (connected == 0 || status != ECONNREFUSED)
            return std::unexpected("existing socket is active or cannot be probed");
        struct stat current{};
        if (lstat(path_.c_str(), &current) != 0 || !S_ISSOCK(current.st_mode) ||
            current.st_uid != getuid() || current.st_dev != info.st_dev || current.st_ino != info.st_ino ||
            unlink(path_.c_str()) != 0)
            return std::unexpected("cannot remove stale socket");
    } else if (errno != ENOENT) {
        return std::unexpected("cannot inspect socket");
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return std::unexpected("cannot create listener: " + std::to_string(errno));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path_.c_str(), path_.native().size() + 1);
    const mode_t previous = umask(0177);
    const int bound = bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    umask(previous);
    if (bound != 0) {
        close(fd);
        return std::unexpected("cannot bind socket");
    }
    if (lstat(path_.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 0777) != 0600) {
        close(fd);
        return std::unexpected("socket permissions refused");
    }
    socket_dev_ = info.st_dev;
    socket_ino_ = info.st_ino;
    if (listen(fd, 64) != 0) {
        close(fd);
        finish();
        return std::unexpected("cannot listen on socket");
    }
    try {
        listener_ = Gio::Socket::create_from_fd(fd);
        accept_source_ = Glib::signal_io().connect(sigc::mem_fun(*this, &Server::accept_ready),
            listener_->get_fd(), Glib::IOCondition::IO_IN | Glib::IOCondition::IO_ERR | Glib::IOCondition::IO_HUP);
    } catch (const Glib::Error&) {
        close(fd);
        finish();
        return std::unexpected("cannot watch socket");
    }
    stopping_ = false;
    if (log_) log_->write("info", "socket", "listening", {{"path", path_.string()}});
    return {};
}

void Server::run() { loop_->run(); }

bool Server::accept_ready(Glib::IOCondition condition) {
    if (stopping_ || ((condition & (Glib::IOCondition::IO_ERR | Glib::IOCondition::IO_HUP)) != Glib::IOCondition(0))) return false;
    while (true) {
        const int fd = accept4(listener_->get_fd(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        if (!peers_->allowed(fd, getuid())) { close(fd); continue; }
        try {
            if (next_connection_ == std::numeric_limits<ConnectionId>::max()) { close(fd); continue; }
            auto socket = Gio::Socket::create_from_fd(fd);
            auto client = std::make_shared<Client>(std::move(socket), ++next_connection_);
            clients_.emplace(fd, client);
            client->input = Glib::signal_io().connect(
                [this, client](Glib::IOCondition flags) { return client_ready(client, flags); },
                fd, Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR);
        } catch (const Glib::Error&) { close(fd); }
    }
}

void Server::close_client(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) return;
    const auto id = it->second->id;
    it->second->input.disconnect();
    it->second->writable.disconnect();
    clients_.erase(it);
    for (const auto& callback : disconnect_callbacks_) callback(id);
    if (stopping_ && clients_.empty()) finish();
}

void Server::queue(const std::shared_ptr<Client>& client, const protocol::Message& message) {
    const auto active = clients_.find(client->socket->get_fd());
    if (active == clients_.end() || active->second != client) return;
    try {
        auto frame = protocol::encode_frame(protocol::serialize_message(message));
        if (frame.size() > max_queue - client->queued) {
            close_client(client->socket->get_fd());
            return;
        }
        client->queued += frame.size();
        client->output.push_back(std::move(frame));
        if (!client->writable.connected()) {
            client->writable = Glib::signal_io().connect(
                [this, client](Glib::IOCondition flags) { return client_ready(client, flags); },
                client->socket->get_fd(), Glib::IOCondition::IO_OUT | Glib::IOCondition::IO_HUP | Glib::IOCondition::IO_ERR);
        }
    } catch (const std::exception&) { close_client(client->socket->get_fd()); }
}

bool Server::client_ready(const std::shared_ptr<Client>& client, Glib::IOCondition condition) {
    const int fd = client->socket->get_fd();
    if ((condition & (Glib::IOCondition::IO_ERR | Glib::IOCondition::IO_HUP)) != Glib::IOCondition(0)) { close_client(fd); return false; }
    if ((condition & Glib::IOCondition::IO_IN) != Glib::IOCondition(0)) {
        std::array<std::byte, 8192> buffer{};
        while (true) {
            const auto size = recv(fd, buffer.data(), buffer.size(), 0);
            if (size == 0) { close_client(fd); return false; }
            if (size < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                close_client(fd); return false;
            }
            client->decoder.feed(std::span(buffer.data(), static_cast<std::size_t>(size)));
            while (true) {
                auto next = client->decoder.next();
                if (!next) { close_client(fd); return false; }
                if (!next->has_value()) break;
                process(client, **next);
                const auto active = clients_.find(fd);
                if (active == clients_.end() || active->second != client) return false;
            }
        }
    }
    if ((condition & Glib::IOCondition::IO_OUT) != Glib::IOCondition(0)) {
        while (!client->output.empty()) {
            auto& frame = client->output.front();
            const auto count = send(fd, frame.data() + client->sent, frame.size() - client->sent, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count <= 0) { close_client(fd); return false; }
            client->sent += static_cast<std::size_t>(count);
            client->queued -= static_cast<std::size_t>(count);
            if (client->sent == frame.size()) { client->output.pop_front(); client->sent = 0; }
        }
        if (client->output.empty()) {
            client->writable.disconnect();
            if (stopping_ && client->pending == 0) { close_client(fd); return false; }
        }
    }
    const auto active = clients_.find(fd);
    return active != clients_.end() && active->second == client;
}

void Server::process(const std::shared_ptr<Client>& client, std::string_view frame) {
    const auto parsed = protocol::parse_message(frame);
    if (!parsed || !std::holds_alternative<protocol::Request>(*parsed)) {
        auto id = request_id(frame);
        if (id) {
            const int code = parsed ? protocol::kInvalidRequest : parsed.error().code;
            if (log_) {
                try { log_->write("warn", "handler", "error", {{"code", std::to_string(code)}}); }
                catch (const std::exception&) { std::fputs("braurserd: daemon log write failed\n", stderr); }
            }
            queue(client, protocol::Response{*id, protocol::ErrorObject{code, "error.generic_title"}});
        } else close_client(client->socket->get_fd());
        return;
    }
    if (client->pending >= max_pending) {
        queue(client, protocol::Response{std::get<protocol::Request>(*parsed).id,
            protocol::ErrorObject{protocol::kBusy, "error.generic_title"}});
        return;
    }
    ++client->pending;
    auto request = std::get<protocol::Request>(*parsed);
    std::weak_ptr<Client> weak = client;
    registry_.dispatch(request, {client->id, getuid()}, [this, weak, id = std::move(request.id), method = request.method](auto outcome) {
        if (log_ && std::holds_alternative<protocol::ErrorObject>(outcome)) {
            try {
                log_->write("warn", "handler", "error", {{"code", std::to_string(std::get<protocol::ErrorObject>(outcome).code)}, {"method", method}});
            } catch (const std::exception&) { std::fputs("braurserd: daemon log write failed\n", stderr); }
        }
        auto current = weak.lock();
        if (!current || current->pending == 0) return;
        const auto active = clients_.find(current->socket->get_fd());
        if (active == clients_.end() || active->second != current) return;
        --current->pending;
        queue(current, protocol::Response{id, std::move(outcome)});
        if (stopping_ && current->pending == 0 && current->output.empty()) close_client(current->socket->get_fd());
    });
}

void Server::broadcast(const protocol::Notification& notification) {
    if (notification.method == JobQuestionNotificationEvent::wire_name) return;
    for (auto it = clients_.begin(); it != clients_.end();) {
        auto client = it->second;
        ++it;
        queue(client, notification);
    }
}

void Server::emit_to(ConnectionId connection, const protocol::Notification& notification) {
    if (!connection) return;
    for (auto it = clients_.begin(); it != clients_.end(); ++it) {
        if (it->second->id == connection) { queue(it->second, notification); return; }
    }
}

void Server::on_disconnect(std::function<void(ConnectionId)> callback) {
    disconnect_callbacks_.push_back(std::move(callback));
}

void Server::finish() {
    if (listener_) { accept_source_.disconnect(); listener_.reset(); }
    struct stat info{};
    if (socket_ino_ && lstat(path_.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) &&
        info.st_uid == getuid() && info.st_dev == socket_dev_ && info.st_ino == socket_ino_)
        unlink(path_.c_str());
    socket_ino_ = 0;
    if (loop_->is_running()) loop_->quit();
}

void Server::stop() {
    if (stopping_) return;
    stopping_ = true;
    accept_source_.disconnect();
    listener_.reset();
    for (auto it = clients_.begin(); it != clients_.end();) {
        auto client = it->second;
        ++it;
        client->input.disconnect();
        if (client->pending == 0 && client->output.empty()) close_client(client->socket->get_fd());
    }
    if (clients_.empty()) finish();
    else shutdown_source_ = Glib::signal_timeout().connect([this] {
        for (auto it = clients_.begin(); it != clients_.end();) {
            int fd = it->first; ++it; close_client(fd);
        }
        finish(); return false;
    }, 3000);
}

}
