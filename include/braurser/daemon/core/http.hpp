#ifndef BRAURSER_DAEMON_CORE_HTTP_HPP
#define BRAURSER_DAEMON_CORE_HTTP_HPP

#include <braurser/daemon/core/connect.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::core {

struct HttpRequest {
    std::string url;
    std::vector<std::string> allowed_hosts;
    std::optional<std::string> if_none_match;
    std::optional<std::string> if_modified_since;
    std::size_t max_bytes{1024 * 1024};
    std::chrono::milliseconds timeout{15000};
    int max_redirects{0};
    std::string user_agent{"braurser/0.1"};
    const ConnectGuard* guard{nullptr};
};

struct HttpResponse {
    long status{};
    std::string body;
    std::optional<std::string> etag;
    std::optional<std::string> last_modified;
    std::string content_type;
    bool not_modified{false};
    std::string final_url;
};

struct HttpError {
    int code{};
    std::string message;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    /// in: request permits only explicit HTTPS hosts and bounded responses.
    /// out: returns the response after validating every redirect.
    /// err: returns a validation or transport error.
    virtual std::expected<HttpResponse, HttpError> get(const HttpRequest& request) = 0;
};

struct HttpTransfer {
    HttpResponse response;
    std::optional<std::string> location;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    /// pre: url has passed URL and host validation.
    /// out: returns one response without following redirects.
    virtual std::expected<HttpTransfer, HttpError> perform(
        const HttpRequest& request, std::string_view url) = 0;
};

class CurlHttpClient final : public HttpClient {
public:
    CurlHttpClient();
    explicit CurlHttpClient(HttpTransport& transport);
    std::expected<HttpResponse, HttpError> get(const HttpRequest& request) override;

private:
    std::unique_ptr<HttpTransport> owned_transport_;
    HttpTransport* transport_;
};

}

#endif
