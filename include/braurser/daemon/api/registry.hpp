#ifndef BRAURSER_DAEMON_API_REGISTRY_HPP
#define BRAURSER_DAEMON_API_REGISTRY_HPP

#include <braurser/protocol/methods.hpp>
#include <braurser/protocol/rpc.hpp>
#include <functional>
#include <cstdint>
#include <sys/types.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace braurser::daemon::api {

using ConnectionId = std::uint64_t;
struct RequestContext {
    ConnectionId connection{};
    uid_t uid{};
};

#define BRAURSER_API_METHOD_TAG(name, params, result) \
    struct params##Method { using Params = protocol::params; using Result = protocol::result; \
        static constexpr std::string_view wire_name = name; };
BRAURSER_PROTOCOL_METHODS(BRAURSER_API_METHOD_TAG)
#undef BRAURSER_API_METHOD_TAG

#define BRAURSER_API_NOTIFICATION_TAG(name, params) \
    struct params##Event { using Payload = protocol::params; static constexpr std::string_view wire_name = name; };
BRAURSER_PROTOCOL_NOTIFICATIONS(BRAURSER_API_NOTIFICATION_TAG)
#undef BRAURSER_API_NOTIFICATION_TAG

template<class Result>
class Reply {
public:
    using Sender = std::function<void(std::variant<nlohmann::json, protocol::ErrorObject>)>;
    explicit Reply(Sender sender) : state_(std::make_shared<State>(std::move(sender))) {}

    /// in: value is a valid typed method result.
    /// out: sends at most one response.
    void ok(const Result& value) const { finish(nlohmann::json(value)); }
    /// in: error has a protocol code and a safe message key.
    /// out: sends at most one response.
    void error(protocol::ErrorObject value) const { finish(std::move(value)); }

private:
    struct State { explicit State(Sender sender) : send(std::move(sender)) {} Sender send; bool done{}; };
    std::shared_ptr<State> state_;
    void finish(std::variant<nlohmann::json, protocol::ErrorObject> outcome) const {
        if (state_->done) return;
        state_->done = true;
        state_->send(std::move(outcome));
    }
};

class HandlerRegistry {
public:
    using Sender = Reply<protocol::HelloResult>::Sender;

    /// in: handler accepts typed method params and a reply that may be retained for asynchronous completion.
    template<class Method, class Handler>
    void register_handler(Handler&& handler) {
        handlers_.insert_or_assign(std::string(Method::wire_name),
            [fn = std::forward<Handler>(handler)]
            (const nlohmann::json& params, RequestContext context, Sender sender) {
                auto reply = Reply<typename Method::Result>(std::move(sender));
                if constexpr (std::is_invocable_v<Handler, const typename Method::Params&, RequestContext,
                                                  Reply<typename Method::Result>>)
                    fn(params.get<typename Method::Params>(), context, std::move(reply));
                else fn(params.get<typename Method::Params>(), std::move(reply));
            });
    }

    /// in: request is an untrusted bounded RPC envelope; sender delivers a single outcome.
    /// out: registered handlers receive typed parameters; failures receive safe protocol errors.
    void dispatch(const protocol::Request& request, Sender sender) const;
    /// in: context identifies the authenticated connection and peer uid.
    void dispatch(const protocol::Request& request, RequestContext context, Sender sender) const;

private:
    using Handler = std::function<void(const nlohmann::json&, RequestContext, Sender)>;
    std::unordered_map<std::string, Handler> handlers_;
};

}

#endif
