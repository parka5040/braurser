#ifndef BRAURSER_PROTOCOL_ERROR_HPP
#define BRAURSER_PROTOCOL_ERROR_HPP

#include <string>

namespace braurser::protocol {

inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kOffline = -32001;
inline constexpr int kUnsupportedCapability = -32002;
inline constexpr int kAuthorizationUnavailable = -32003;
inline constexpr int kBusy = -32004;
inline constexpr int kStalePlan = -32005;
inline constexpr int kUnsafeTransaction = -32006;
inline constexpr int kExpiredQuestion = -32007;
inline constexpr int kCancelled = -32008;
inline constexpr int kUntrustedInput = -32009;
inline constexpr int kSystemUpdateRequired = -32010;

struct ProtocolError {
    int code;
    std::string message_key;
};

}

#endif
