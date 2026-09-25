#include <braurser/protocol/rpc.hpp>
#include <braurser/protocol/dto.hpp>
#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/methods.hpp>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace braurser::protocol {
namespace {

bool fields(const nlohmann::json& j, std::initializer_list<std::string_view> names) {
    if (!j.is_object()) return false;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find(names.begin(), names.end(), it.key()) == names.end()) return false;
    }
    return true;
}

RequestId read_id(const nlohmann::json& value) {
    if (value.is_string()) {
        const auto id = value.get<std::string>();
        if (id.empty() || id.size() > 64 || !valid_utf8(id)) throw std::invalid_argument("invalid id");
        return id;
    }
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument("invalid id");
    return value.get<std::int64_t>();
}

nlohmann::json id_json(const RequestId& id) {
    return std::visit([](const auto& value) { return nlohmann::json(value); }, id);
}

bool error_code(int code) {
    return code == kParseError || code == kInvalidRequest || code == kMethodNotFound ||
           code == kInvalidParams || code == kInternalError ||
           (code <= kOffline && code >= kSystemUpdateRequired);
}

ErrorObject read_error(const nlohmann::json& value) {
    if (!fields(value, {"code", "message_key", "args"}) ||
        !value.contains("code") || !value.contains("message_key") || !value.contains("args") ||
        !value.at("code").is_number_integer() || value.at("code").is_number_unsigned() ||
        !value.at("message_key").is_string() ||
        !value.at("args").is_object()) throw std::invalid_argument("invalid error");
    const auto raw_code = value.at("code").get<std::int64_t>();
    if (raw_code < kParseError || raw_code > kOffline) throw std::invalid_argument("invalid code");
    const int code = static_cast<int>(raw_code);
    if (!error_code(code)) throw std::invalid_argument("invalid code");
    validate_wire(value);
    const std::string key = value.at("message_key").get<std::string>();
    if (!key.starts_with("error.")) throw std::invalid_argument("invalid error key");
    return {code, key, value.at("args")};
}

const MethodDescriptor* method(std::string_view name) {
    const auto it = std::find_if(kMethods.begin(), kMethods.end(), [name](const auto& entry) { return entry.name == name; });
    return it == kMethods.end() ? nullptr : &*it;
}

const NotificationDescriptor* notification(std::string_view name) {
    const auto it = std::find_if(kNotifications.begin(), kNotifications.end(), [name](const auto& entry) { return entry.name == name; });
    return it == kNotifications.end() ? nullptr : &*it;
}

}

std::expected<void, ProtocolError> EventSequenceTracker::observe(std::string_view job_id, std::uint64_t sequence) {
    if (job_id.empty() || job_id.size() > 128 || !valid_utf8(job_id) || sequence == 0 ||
        std::any_of(job_id.begin(), job_id.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_sequence"});
    const auto existing = last_.find(job_id);
    if (existing != last_.end()) {
        if (sequence <= existing->second)
            return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_sequence"});
        existing->second = sequence;
        return {};
    }
    if (last_.size() >= 1024)
        return std::unexpected(ProtocolError{kBusy, "error.busy"});
    last_.emplace(job_id, sequence);
    return {};
}

void EventSequenceTracker::forget(std::string_view job_id) {
    const auto existing = last_.find(job_id);
    if (existing != last_.end()) last_.erase(existing);
}

std::expected<Message, ProtocolError> parse_message(std::string_view text) {
    if (text.empty() || text.size() > kMaxFrameBytes || !valid_utf8(text))
        return std::unexpected(ProtocolError{kParseError, "error.invalid_json"});
    nlohmann::json j;
    try {
        std::vector<std::unordered_set<std::string>> object_keys;
        std::size_t nodes = 0;
        std::size_t strings = 0;
        j = nlohmann::json::parse(text.begin(), text.end(), [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value) {
            if (event == nlohmann::json::parse_event_t::object_start ||
                event == nlohmann::json::parse_event_t::array_start ||
                event == nlohmann::json::parse_event_t::value) {
                if (++nodes > 16384) throw std::invalid_argument("JSON node limit");
            }
            if ((event == nlohmann::json::parse_event_t::object_start ||
                 event == nlohmann::json::parse_event_t::array_start) && depth > 32)
                throw std::invalid_argument("nesting limit");
            if (event == nlohmann::json::parse_event_t::object_start) object_keys.emplace_back();
            if (event == nlohmann::json::parse_event_t::object_end) object_keys.pop_back();
            if (event == nlohmann::json::parse_event_t::key) {
                const auto& key = value.get_ref<const std::string&>();
                if (key.size() > 512 * 1024 - strings ||
                    !object_keys.back().insert(key).second)
                    throw std::invalid_argument("invalid JSON key");
                strings += key.size();
            } else if (event == nlohmann::json::parse_event_t::value && value.is_string()) {
                const auto size = value.get_ref<const std::string&>().size();
                if (size > 512 * 1024 - strings) throw std::invalid_argument("JSON string limit");
                strings += size;
            }
            return true;
        });
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(ProtocolError{kParseError, "error.invalid_json"});
    } catch (const std::invalid_argument&) {
        return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
    }
    try {
        validate_wire(j);
        if (!j.is_object() || !j.contains("jsonrpc") || j.at("jsonrpc") != "2.0")
            return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
        if (j.contains("method")) {
            if (!fields(j, {"jsonrpc", "id", "method", "params", "apiVersion"}) ||
                !j.at("method").is_string() || !j.contains("params") || !j.at("params").is_object())
                return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
            const auto name = j.at("method").get<std::string>();
            if (j.contains("id")) {
                if (!j.contains("apiVersion") || !j.at("apiVersion").is_number_unsigned() ||
                    j.at("apiVersion").get<std::uint64_t>() != kApiVersion)
                    return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
                RequestId id = read_id(j.at("id"));
                const auto* descriptor = method(name);
                if (!descriptor) return std::unexpected(ProtocolError{kMethodNotFound, "error.method_not_found"});
                if (!descriptor->validate_params(j.at("params")))
                    return std::unexpected(ProtocolError{kInvalidParams, "error.invalid_params"});
                return Message(Request{std::move(id), name, j.at("params")});
            }
            if (j.contains("apiVersion")) return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
            const auto* descriptor = notification(name);
            if (!descriptor) return std::unexpected(ProtocolError{kMethodNotFound, "error.method_not_found"});
            if (!descriptor->validate_params(j.at("params")))
                return std::unexpected(ProtocolError{kInvalidParams, "error.invalid_params"});
            return Message(Notification{name, j.at("params")});
        }
        if (!fields(j, {"jsonrpc", "id", "result", "error"}) || !j.contains("id") ||
            j.contains("result") == j.contains("error"))
            return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
        RequestId id = read_id(j.at("id"));
        if (j.contains("error")) return Message(Response{std::move(id), read_error(j.at("error"))});
        return Message(Response{std::move(id), j.at("result")});
    } catch (const nlohmann::json::exception&) {
        return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
    } catch (const std::invalid_argument&) {
        return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
    } catch (const std::out_of_range&) {
        return std::unexpected(ProtocolError{kInvalidRequest, "error.invalid_request"});
    }
}

std::string serialize_message(const Message& message) {
    nlohmann::json j = {{"jsonrpc", "2.0"}};
    std::visit([&j](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, Request>) {
            j["id"] = id_json(item.id);
            j["apiVersion"] = kApiVersion;
            j["method"] = item.method;
            j["params"] = item.params;
        } else if constexpr (std::is_same_v<T, Notification>) {
            j["method"] = item.method;
            j["params"] = item.params;
        } else {
            j["id"] = id_json(item.id);
            if (std::holds_alternative<ErrorObject>(item.outcome)) {
                const auto& e = std::get<ErrorObject>(item.outcome);
                j["error"] = {{"code", e.code}, {"message_key", e.message_key}, {"args", e.args}};
            } else {
                j["result"] = std::get<nlohmann::json>(item.outcome);
            }
        }
    }, message);
    const auto text = j.dump();
    if (!parse_message(text)) throw std::invalid_argument("invalid message");
    return text;
}

}
