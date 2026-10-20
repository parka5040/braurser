#ifndef BRAURSER_PRESENTER_RPC_CLIENT_HPP
#define BRAURSER_PRESENTER_RPC_CLIENT_HPP

#include <braurser/protocol/methods.hpp>
#include <braurser/protocol/rpc.hpp>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>

namespace braurser::presenter {

class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    virtual void post(std::function<void()> callback) = 0;
};

class Timer {
public:
    using Token = std::uint64_t;
    virtual ~Timer() = default;
    virtual Token after(std::chrono::milliseconds delay, std::function<void()> callback) = 0;
    virtual void cancel(Token token) = 0;
};

class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel);
    ~Subscription();
    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    void reset();
private:
    std::function<void()> cancel_;
};

template<class Params> struct Method;
#define BRAURSER_PRESENTER_METHOD(name_value, params, result) \
    template<> struct Method<protocol::params> { \
        using Params = protocol::params; \
        using Result = protocol::result; \
        static constexpr std::string_view name = name_value; \
    };
BRAURSER_PROTOCOL_METHODS(BRAURSER_PRESENTER_METHOD)
#undef BRAURSER_PRESENTER_METHOD

template<class Params> struct Event;
#define BRAURSER_PRESENTER_EVENT(name_value, params) \
    template<> struct Event<protocol::params> { \
        using Params = protocol::params; \
        static constexpr std::string_view name = name_value; \
    };
BRAURSER_PROTOCOL_NOTIFICATIONS(BRAURSER_PRESENTER_EVENT)
#undef BRAURSER_PRESENTER_EVENT

using RpcError = protocol::ErrorObject;
template<class T> using Reply = std::expected<T, RpcError>;

inline constexpr std::chrono::seconds kPlanTimeout{200};

template<class Params>
inline constexpr std::chrono::seconds kRequestTimeout =
    std::is_same_v<Params, protocol::PlanParams> ||
    std::is_same_v<Params, protocol::UpdatesPlanParams> ||
    std::is_same_v<Params, protocol::InstallParams> ||
    std::is_same_v<Params, protocol::RemoveParams> ||
    std::is_same_v<Params, protocol::UpgradeSystemParams> ||
    std::is_same_v<Params, protocol::UpgradeCommunityParams> ||
    std::is_same_v<Params, protocol::ManualInstallParams> ||
    std::is_same_v<Params, protocol::ManualUpgradeParams> ||
    std::is_same_v<Params, protocol::ManualRemoveParams> ||
    std::is_same_v<Params, protocol::SetupHelperParams> ? kPlanTimeout : std::chrono::seconds(30);

class Rpc {
public:
    using Callback = std::function<void(Reply<nlohmann::json>)>;
    virtual ~Rpc() = default;
    virtual void request(std::string_view method, nlohmann::json params, Callback callback,
                         std::chrono::milliseconds timeout) = 0;
    virtual Subscription subscribe(std::string_view method, std::function<void(const nlohmann::json&)> callback) = 0;
    template<class Descriptor>
    void call(const typename Descriptor::Params& params,
              std::function<void(Reply<typename Descriptor::Result>)> callback,
              std::chrono::milliseconds timeout = kRequestTimeout<typename Descriptor::Params>) {
        request(Descriptor::name, nlohmann::json(params), [callback = std::move(callback)](Reply<nlohmann::json> reply) {
            if (!reply) { callback(std::unexpected(reply.error())); return; }
            Reply<typename Descriptor::Result> result = std::unexpected(RpcError{protocol::kInvalidRequest, "error.core_title", nlohmann::json::object()});
            try { result = reply->template get<typename Descriptor::Result>(); }
            catch (const std::exception&) {}
            callback(std::move(result));
        }, timeout);
    }
    template<class Params>
    Subscription listen(std::function<void(const Params&)> callback) {
        return subscribe(Event<Params>::name, [callback = std::move(callback)](const nlohmann::json& json) {
            callback(json.template get<Params>());
        });
    }
};

class SocketConnector {
public:
    virtual ~SocketConnector() = default;
    virtual std::expected<int, int> connect() = 0;
};

struct RpcOptions {
    std::string socket_path;
    std::string daemon_path;
    std::size_t queue_limit = 128;
    std::size_t outbound_bytes_limit = 4 * 1024 * 1024;
    std::chrono::milliseconds initial_backoff{50};
    bool launch_daemon = true;
};

class RpcClient final : public Rpc {
public:
    /// pre: dispatcher and an injected connector outlive this client; post queues onto the UI thread.
    explicit RpcClient(Dispatcher& dispatcher, RpcOptions options = {}, std::shared_ptr<SocketConnector> connector = {});
    ~RpcClient() override;
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    void request(std::string_view method, nlohmann::json params, Callback callback,
                 std::chrono::milliseconds timeout = std::chrono::seconds(30)) override;
    Subscription subscribe(std::string_view method, std::function<void(const nlohmann::json&)> callback) override;
    Subscription connection_changed(std::function<void(bool)> callback);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}

#endif
