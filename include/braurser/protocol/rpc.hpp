#ifndef BRAURSER_PROTOCOL_RPC_HPP
#define BRAURSER_PROTOCOL_RPC_HPP

#include <braurser/protocol/error.hpp>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace braurser::protocol {

inline constexpr std::uint32_t kApiVersion = 1;
using RequestId = std::variant<std::int64_t, std::string>;

struct Request {
    RequestId id;
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

struct ErrorObject {
    int code;
    std::string message_key;
    nlohmann::json args = nlohmann::json::object();
};

struct Response {
    RequestId id;
    std::variant<nlohmann::json, ErrorObject> outcome;
};

struct Notification {
    std::string method;
    nlohmann::json params = nlohmann::json::object();
};

using Message = std::variant<Request, Response, Notification>;

class EventSequenceTracker {
public:
    /// in: job_id identifies the job; sequence is its next positive event sequence.
    /// out: success only when sequence is greater than the last observed value for this job.
    /// err: invalid identifiers, stale sequences or an exhausted bounded tracker fail closed.
    std::expected<void, ProtocolError> observe(std::string_view job_id, std::uint64_t sequence);

    /// in: job_id identifies a completed job.
    /// out: stored sequence state for that job is discarded.
    void forget(std::string_view job_id);

private:
    std::map<std::string, std::uint64_t, std::less<>> last_;
};

/// in: text is a bounded UTF-8 JSON-RPC 2.0 message.
/// out: a validated envelope or a safe protocol error.
std::expected<Message, ProtocolError> parse_message(std::string_view text);

/// in: message contains a well-formed request, response or notification.
/// out: bounded UTF-8 JSON text.
/// err: std::invalid_argument if the message violates the wire contract.
std::string serialize_message(const Message& message);

}

#endif
