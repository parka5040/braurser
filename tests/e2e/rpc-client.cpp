#include <braurser/protocol/frame.hpp>
#include <braurser/protocol/rpc.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <poll.h>
#include <map>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class Connection {
public:
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection() { if (fd_ >= 0) close(fd_); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool send_request(const std::string& id, const std::string& method, const nlohmann::json& params) {
        const auto frame = braurser::protocol::encode_frame(braurser::protocol::serialize_message(
            braurser::protocol::Request{id, method, params}));
        std::size_t sent{};
        while (sent < frame.size()) {
            const auto count = send(fd_, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    void replay(nlohmann::json value) { backlog_.push_back(std::move(value)); }

    std::optional<nlohmann::json> next(Clock::time_point deadline) {
        if (!backlog_.empty()) {
            auto value = std::move(backlog_.front());
            backlog_.pop_front();
            return value;
        }
        while (true) {
            if (!pending_.empty()) {
                auto value = std::move(pending_.front());
                pending_.pop_front();
                return value;
            }
            const auto now = Clock::now();
            if (now >= deadline) return std::nullopt;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            pollfd ready{fd_, POLLIN, 0};
            const auto waited = poll(&ready, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
            if (waited < 0 && errno == EINTR) continue;
            if (waited <= 0) return std::nullopt;
            std::array<std::byte, 8192> bytes{};
            const auto count = recv(fd_, bytes.data(), bytes.size(), 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return std::nullopt;
            decoder_.feed(std::span(bytes.data(), static_cast<std::size_t>(count)));
            while (true) {
                auto frame = decoder_.next();
                if (!frame) return std::nullopt;
                if (!frame->has_value()) break;
                auto value = nlohmann::json::parse(**frame, nullptr, false);
                if (!value.is_object()) return std::nullopt;
                std::cout << value.dump() << std::endl;
                pending_.push_back(std::move(value));
            }
        }
    }

private:
    int fd_;
    braurser::protocol::FrameDecoder decoder_;
    std::deque<nlohmann::json> pending_;
    std::deque<nlohmann::json> backlog_;
};

std::optional<std::string> terminal_job(const nlohmann::json& item) {
    if (item.value("method", std::string{}) != "job.state") return std::nullopt;
    const auto& params = item.value("params", nlohmann::json::object());
    if (!params.is_object()) return std::nullopt;
    const auto state = params.value("state", std::string{});
    if (state != "succeeded" && state != "failed" && state != "cancelled") return std::nullopt;
    return params.value("jobId", std::string{});
}

std::optional<nlohmann::json> await_response(Connection& connection, const std::string& id,
                                             Clock::time_point deadline, std::map<std::string, std::string>& finished) {
    std::vector<nlohmann::json> deferred;
    while (auto value = connection.next(deadline)) {
        if (value->contains("id") && value->at("id") == id) {
            for (auto& item : deferred) connection.replay(std::move(item));
            return value;
        }
        if (auto job = terminal_job(*value)) finished[*job] = value->at("params").value("state", std::string{});
        else deferred.push_back(std::move(*value));
    }
    return std::nullopt;
}

int await_job(Connection& connection, const nlohmann::json& response, Clock::time_point deadline,
              std::map<std::string, std::string>& finished) {
    if (!response.contains("result")) return 1;
    const auto& result = response.at("result");
    if (!result.is_object() || !result.contains("jobId") || !result.at("jobId").is_string()) return 0;
    const auto job = result.at("jobId").get<std::string>();
    if (auto known = finished.find(job); known != finished.end()) return known->second == "succeeded" ? 0 : 3;
    const char* acknowledge = std::getenv("BRAURSER_E2E_ACK_REVIEWS");
    const bool answer_reviews = acknowledge != nullptr && std::string_view(acknowledge) == "1";
    int answers = 0;
    std::optional<Clock::time_point> attention;
    while (true) {
        const auto limit = attention ? std::min(deadline, *attention) : deadline;
        auto value = connection.next(limit);
        if (!value) {
            if (attention && Clock::now() >= *attention) {
                std::cerr << "rpc-client: job " << job << " needs attention without a question\n";
                return 3;
            }
            break;
        }
        if (terminal_job(*value) == job)
            return value->at("params").value("state", std::string{}) == "succeeded" ? 0 : 3;
        if (value->value("method", std::string{}) == "job.state") {
            const auto& state = value->value("params", nlohmann::json::object());
            if (state.value("jobId", std::string{}) == job)
                attention = state.value("state", std::string{}) == "needs-attention"
                    ? std::optional<Clock::time_point>(Clock::now() + std::chrono::seconds(10)) : std::nullopt;
            continue;
        }
        if (value->value("method", std::string{}) != "job.question") continue;
        if (value->value("params", nlohmann::json::object()).value("jobId", std::string{}) == job) attention.reset();
        if (!answer_reviews) continue;
        const auto& params = value->value("params", nlohmann::json::object());
        const auto& question = params.value("question", nlohmann::json::object());
        if (params.value("jobId", std::string{}) != job || question.value("kind", std::string{}) != "build_review") continue;
        const nlohmann::json answer{{"jobId", job}, {"questionId", question.value("question_id", std::string{})},
                                    {"choiceId", "ack"}, {"questionToken", question.value("token", std::string{})}};
        if (!connection.send_request("rpc-client-answer-" + std::to_string(++answers), "jobs.answer", answer)) return 1;
    }
    std::cerr << "rpc-client: timed out or disconnected before job " << job << " finished\n";
    return 1;
}

std::optional<int> parse_seconds(const char* text) {
    int seconds = 0;
    const std::string_view input(text);
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), seconds);
    if (error != std::errc{} || end != input.data() + input.size() || seconds < 1 || seconds > 3600)
        return std::nullopt;
    return seconds;
}

int usage() {
    std::cerr << "usage: braurser-rpc-client SOCKET METHOD JSON_PARAMS [TIMEOUT_SECONDS]\n"
                 "       braurser-rpc-client SOCKET --plan PLAN_JSON_PARAMS ACTION_METHOD [TIMEOUT_SECONDS]\n";
    return 2;
}

bool waits_for_job(const std::string& method) {
    return method.starts_with("jobs.") || method.starts_with("manual.") || method == "catalog.refresh" ||
           method == "updates.check";
}

}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) return usage();
    const std::filesystem::path socket_path(argv[1]);
    if (!socket_path.is_absolute() || socket_path.native().size() >= sizeof(sockaddr_un::sun_path)) return 2;
    const bool plan_mode = std::string_view(argv[2]) == "--plan";
    if (plan_mode ? argc < 5 : argc > 5) return usage();
    const auto params = nlohmann::json::parse(argv[3], nullptr, false);
    if (!params.is_object()) return 2;
    const int timeout_index = plan_mode ? 5 : 4;
    int seconds = 60;
    if (argc > timeout_index) {
        const auto parsed = parse_seconds(argv[timeout_index]);
        if (!parsed) return 2;
        seconds = *parsed;
    }
    const auto deadline = Clock::now() + std::chrono::seconds(seconds);
    try {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return 1;
        Connection connection(fd);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socket_path.c_str(), socket_path.native().size() + 1);
        if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return 1;
        std::map<std::string, std::string> finished;
        if (!plan_mode) {
            const std::string method(argv[2]);
            if (!connection.send_request("rpc-client-1", method, params)) return 1;
            const auto response = await_response(connection, "rpc-client-1", deadline, finished);
            if (!response || response->contains("error")) return 1;
            return waits_for_job(method) ? await_job(connection, *response, deadline, finished) : 0;
        }
        if (!connection.send_request("rpc-client-plan", "jobs.plan", params)) return 1;
        const auto planned = await_response(connection, "rpc-client-plan", deadline, finished);
        if (!planned || !planned->contains("result")) return 1;
        const auto& plan = planned->at("result");
        const nlohmann::json approval{{"planId", plan.value("plan_id", std::string{})},
                                      {"approvalToken", plan.value("approval_token", std::string{})}};
        if (!connection.send_request("rpc-client-action", argv[4], approval)) return 1;
        const auto response = await_response(connection, "rpc-client-action", deadline, finished);
        if (!response || response->contains("error")) return 1;
        return await_job(connection, *response, deadline, finished);
    } catch (const std::exception& error) {
        std::cerr << "rpc-client: " << error.what() << '\n';
        return 2;
    }
}
