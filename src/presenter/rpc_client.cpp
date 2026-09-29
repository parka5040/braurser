#include <braurser/presenter/rpc_client.hpp>
#include <braurser/protocol/frame.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace braurser::presenter {
namespace {
using Clock = std::chrono::steady_clock;
RpcError failure(int code = protocol::kInternalError) {
    return {code, "error.core_title", nlohmann::json::object()};
}
class LocalConnector final : public SocketConnector {
public:
    explicit LocalConnector(std::string path) : path_(std::move(path)) {}
    std::expected<int, int> connect() override {
        struct stat metadata{};
        if (::lstat(path_.c_str(), &metadata) != 0) { return std::unexpected(errno); }
        if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != ::getuid()) { return std::unexpected(EACCES); }
        const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (socket < 0) { return std::unexpected(errno); }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            const int error = errno;
            ::close(socket);
            return std::unexpected(error);
        }
        return socket;
    }
private:
    std::string path_;
};
bool finished_job(const nlohmann::json& params) {
    const auto state = params.find("state");
    if (state == params.end() || !state->is_string()) { return false; }
    const auto& value = state->get_ref<const std::string&>();
    return value == "succeeded" || value == "failed" || value == "cancelled";
}
pid_t spawn_detached(const std::string& path, char* const* argv) {
    posix_spawn_file_actions_t actions{};
    if (::posix_spawn_file_actions_init(&actions) != 0) { return -1; }
    posix_spawnattr_t attributes{};
    if (::posix_spawnattr_init(&attributes) != 0) { ::posix_spawn_file_actions_destroy(&actions); return -1; }
    sigset_t empty{};
    sigemptyset(&empty);
    pid_t child = -1;
    const bool ready = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3) == 0 &&
                       ::posix_spawn_file_actions_addchdir_np(&actions, "/") == 0 &&
                       ::posix_spawnattr_setsigmask(&attributes, &empty) == 0 &&
                       ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK) == 0;
    if (!ready || ::posix_spawn(&child, path.c_str(), &actions, &attributes, argv, environ) != 0) { child = -1; }
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    return child;
}
bool owned_peer(int socket) {
    ucred peer{};
    socklen_t size = sizeof(peer);
    return ::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
           size == sizeof(peer) && peer.uid == ::getuid();
}
}

Subscription::Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
Subscription::~Subscription() { reset(); }
Subscription::Subscription(Subscription&& other) noexcept : cancel_(std::exchange(other.cancel_, {})) {}
Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) { reset(); cancel_ = std::exchange(other.cancel_, {}); }
    return *this;
}
void Subscription::reset() { if (cancel_) { auto cancel = std::exchange(cancel_, {}); cancel(); } }

struct RpcClient::Impl : std::enable_shared_from_this<RpcClient::Impl> {
    struct Pending {
        std::int64_t id;
        std::string method;
        std::string frame;
        std::size_t offset = 0;
        Clock::time_point deadline;
        Callback callback;
    };
    struct Listener {
        std::string method;
        std::function<void(const nlohmann::json&)> callback;
        std::shared_ptr<std::atomic_bool> alive;
    };
    Dispatcher& dispatcher;
    RpcOptions options;
    std::shared_ptr<SocketConnector> connector;
    std::mutex mutex;
    std::deque<Pending> pending;
    std::map<std::uint64_t, Listener> listeners;
    std::int64_t next_id = 1;
    std::uint64_t next_listener = 1;
    std::size_t bytes = 0;
    std::jthread worker;
    int socket = -1;
    pid_t child = -1;
    bool launched = false;
    protocol::FrameDecoder decoder;
    protocol::EventSequenceTracker sequences;
    Impl(Dispatcher& dispatch, RpcOptions configuration, std::shared_ptr<SocketConnector> connect)
        : dispatcher(dispatch), options(std::move(configuration)), connector(std::move(connect)) {}
    void deliver(Callback callback, Reply<nlohmann::json> reply) {
        dispatcher.post([callback = std::move(callback), reply = std::move(reply)]() mutable { callback(std::move(reply)); });
    }
    void publish(std::string_view method, const nlohmann::json& params) {
        std::vector<Listener> targets;
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, listener] : listeners) {
                static_cast<void>(id);
                if (listener.method == method) { targets.push_back(listener); }
            }
        }
        for (const auto& listener : targets) {
            dispatcher.post([listener, params] { if (listener.alive->load()) { listener.callback(params); } });
        }
    }
    void disconnect() {
        if (socket < 0) { return; }
        ::close(socket);
        socket = -1;
        decoder = {};
        sequences = {};
        std::vector<Callback> failed;
        {
            std::lock_guard lock(mutex);
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->offset != 0) {
                    bytes -= it->frame.size();
                    failed.push_back(std::move(it->callback));
                    it = pending.erase(it);
                } else { ++it; }
            }
        }
        for (auto& callback : failed) { deliver(std::move(callback), std::unexpected(failure())); }
        publish("", false);
    }
    void expire() {
        std::vector<Callback> expired;
        bool partial = false;
        {
            std::lock_guard lock(mutex);
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->deadline <= Clock::now()) {
                    partial = partial || (it->offset != 0 && it->offset != it->frame.size());
                    bytes -= it->frame.size();
                    expired.push_back(std::move(it->callback));
                    it = pending.erase(it);
                } else { ++it; }
            }
        }
        for (auto& callback : expired) { deliver(std::move(callback), std::unexpected(failure())); }
        if (partial) { disconnect(); }
    }
    bool receive(std::string_view text) {
        auto message = protocol::parse_message(text);
        if (!message) { return false; }
        if (const auto* response = std::get_if<protocol::Response>(&*message)) {
            const auto* id = std::get_if<std::int64_t>(&response->id);
            if (!id) { return false; }
            Callback callback;
            std::string method;
            {
                std::lock_guard lock(mutex);
                auto it = std::ranges::find(pending, *id, &Pending::id);
                if (it == pending.end()) { return true; }
                if (it->offset != it->frame.size()) { return false; }
                callback = std::move(it->callback);
                method = it->method;
                bytes -= it->frame.size();
                pending.erase(it);
            }
            if (const auto* error = std::get_if<protocol::ErrorObject>(&response->outcome)) {
                deliver(std::move(callback), std::unexpected(*error));
            } else {
                const auto& result = std::get<nlohmann::json>(response->outcome);
                const auto descriptor = std::ranges::find(protocol::kMethods, method, &protocol::MethodDescriptor::name);
                if (descriptor == protocol::kMethods.end() || !descriptor->validate_result(result)) {
                    deliver(std::move(callback), std::unexpected(failure(protocol::kInvalidRequest)));
                    return false;
                }
                deliver(std::move(callback), result);
            }
            return true;
        }
        if (const auto* event = std::get_if<protocol::Notification>(&*message)) {
            const auto descriptor = std::ranges::find(protocol::kNotifications, event->method, &protocol::NotificationDescriptor::name);
            if (descriptor == protocol::kNotifications.end() || !descriptor->validate_params(event->params)) { return false; }
            if (event->params.contains("jobId") && event->params.contains("sequence")) {
                const auto job = event->params.at("jobId").get<std::string>();
                if (!sequences.observe(job, event->params.at("sequence").get<std::uint64_t>())) { return false; }
                publish(event->method, event->params);
                if (event->method == Event<protocol::JobStateNotification>::name && finished_job(event->params)) { sequences.forget(job); }
                return true;
            }
            publish(event->method, event->params);
            return true;
        }
        return false;
    }
    void run(std::stop_token stop) {
        auto backoff = options.initial_backoff;
        auto retry = Clock::now();
        while (!stop.stop_requested()) {
            if (child > 0 && ::waitpid(child, nullptr, WNOHANG) == child) { child = -1; }
            expire();
            if (socket < 0 && Clock::now() >= retry) {
                auto connection = connector->connect();
                if (connection && owned_peer(*connection)) {
                    socket = *connection;
                    const auto flags = ::fcntl(socket, F_GETFL);
                    if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0 || ::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0) {
                        ::close(socket); socket = -1;
                    } else {
                        backoff = options.initial_backoff;
                        publish("", true);
                    }
                } else {
                    if (connection) { ::close(*connection); }
                    if (!connection && options.launch_daemon && !launched &&
                        (connection.error() == ENOENT || connection.error() == ECONNREFUSED)) {
                        launched = true;
                        std::array<char*, 3> argv{options.daemon_path.data(), const_cast<char*>("--background"), nullptr};
                        child = spawn_detached(options.daemon_path, argv.data());
                    }
                }
                retry = Clock::now() + backoff;
                backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
            }
            if (socket < 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            bool writing = false;
            {
                std::lock_guard lock(mutex);
                writing = std::ranges::any_of(pending, [](const Pending& value) { return value.offset < value.frame.size(); });
            }
            pollfd descriptor{socket, static_cast<short>(POLLIN | (writing ? POLLOUT : 0)), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            if (ready < 0 && errno == EINTR) { continue; }
            if (ready < 0 || (descriptor.revents & (POLLERR | POLLNVAL)) != 0) { disconnect(); continue; }
            if ((descriptor.revents & POLLOUT) != 0) {
                bool failed = false;
                {
                    std::lock_guard lock(mutex);
                    for (auto& item : pending) {
                        if (item.offset == item.frame.size()) { continue; }
                        const auto sent = ::send(socket, item.frame.data() + item.offset, item.frame.size() - item.offset, MSG_NOSIGNAL);
                        if (sent > 0) { item.offset += static_cast<std::size_t>(sent); }
                        else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { failed = true; }
                        if (failed || item.offset != item.frame.size()) { break; }
                    }
                }
                if (failed) { disconnect(); continue; }
            }
            if ((descriptor.revents & (POLLIN | POLLHUP)) != 0) {
                std::array<std::byte, 8192> input{};
                const auto count = ::recv(socket, input.data(), input.size(), 0);
                if (count <= 0) {
                    if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { disconnect(); }
                    continue;
                }
                decoder.feed(std::span(input.data(), static_cast<std::size_t>(count)));
                while (true) {
                    auto frame = decoder.next();
                    if (!frame) { disconnect(); break; }
                    if (!*frame) { break; }
                    if (!receive(**frame)) { disconnect(); break; }
                }
            }
        }
        disconnect();
    }
    Subscription add(std::string_view method, std::function<void(const nlohmann::json&)> callback) {
        auto alive = std::make_shared<std::atomic_bool>(true);
        std::lock_guard lock(mutex);
        if (listeners.size() >= 256) { throw std::length_error("too many subscriptions"); }
        const auto id = next_listener++;
        listeners.emplace(id, Listener{std::string(method), std::move(callback), alive});
        return Subscription([weak = weak_from_this(), id, alive] {
            alive->store(false);
            if (auto owner = weak.lock()) { std::lock_guard guard(owner->mutex); owner->listeners.erase(id); }
        });
    }
};

RpcClient::RpcClient(Dispatcher& dispatcher, RpcOptions options, std::shared_ptr<SocketConnector> connector) {
    if (options.queue_limit == 0 || options.queue_limit > 4096 || options.outbound_bytes_limit == 0 ||
        options.outbound_bytes_limit > 64 * 1024 * 1024 || options.initial_backoff.count() <= 0 || options.initial_backoff > std::chrono::seconds(5)) {
        throw std::invalid_argument("invalid RPC bounds");
    }
    if (options.socket_path.empty() && !connector) {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] != '/') { throw std::invalid_argument("absolute runtime directory required"); }
        options.socket_path = std::string(runtime) + "/braurser/core.sock";
    }
    if (!connector) {
        if (options.socket_path.empty() || options.socket_path.front() != '/' || options.socket_path.size() >= sizeof(sockaddr_un::sun_path) ||
            options.socket_path.find('\0') != std::string::npos) { throw std::invalid_argument("invalid socket path"); }
        connector = std::make_shared<LocalConnector>(options.socket_path);
    }
    if (options.daemon_path.empty()) { options.daemon_path = BRAURSER_DAEMON_PATH; }
    if (options.daemon_path.front() != '/' || options.daemon_path.size() > 4096 || options.daemon_path.find('\0') != std::string::npos) {
        throw std::invalid_argument("invalid daemon path");
    }
    impl_ = std::make_shared<Impl>(dispatcher, std::move(options), std::move(connector));
    impl_->worker = std::jthread([owner = impl_.get()](std::stop_token stop) { owner->run(stop); });
}
RpcClient::~RpcClient() {
    impl_->worker.request_stop();
    impl_->worker.join();
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, listener] : impl_->listeners) { static_cast<void>(id); listener.alive->store(false); }
}
void RpcClient::request(std::string_view method, nlohmann::json params, Callback callback, std::chrono::milliseconds timeout) {
    if (!callback) { throw std::invalid_argument("callback required"); }
    const auto descriptor = std::ranges::find(protocol::kMethods, method, &protocol::MethodDescriptor::name);
    if (descriptor == protocol::kMethods.end() || !descriptor->validate_params(params) || timeout.count() <= 0 || timeout > std::chrono::minutes(10)) {
        impl_->deliver(std::move(callback), std::unexpected(failure(protocol::kInvalidParams))); return;
    }
    bool full = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->next_id == INT64_MAX) { full = true; }
        else {
            const auto id = impl_->next_id++;
            auto frame = protocol::encode_frame(protocol::serialize_message(protocol::Request{id, std::string(method), std::move(params)}));
            full = impl_->pending.size() >= impl_->options.queue_limit || frame.size() > impl_->options.outbound_bytes_limit - impl_->bytes;
            if (!full) {
                impl_->bytes += frame.size();
                impl_->pending.push_back({id, std::string(method), std::move(frame), 0, Clock::now() + timeout, std::move(callback)});
            }
        }
    }
    if (full) { impl_->deliver(std::move(callback), std::unexpected(failure(protocol::kBusy))); }
}
Subscription RpcClient::subscribe(std::string_view method, std::function<void(const nlohmann::json&)> callback) {
    if (!callback || std::ranges::find(protocol::kNotifications, method, &protocol::NotificationDescriptor::name) == protocol::kNotifications.end()) {
        throw std::invalid_argument("unknown notification");
    }
    return impl_->add(method, std::move(callback));
}
Subscription RpcClient::connection_changed(std::function<void(bool)> callback) {
    if (!callback) { throw std::invalid_argument("callback required"); }
    return impl_->add("", [callback = std::move(callback)](const nlohmann::json& value) { callback(value.get<bool>()); });
}

}
