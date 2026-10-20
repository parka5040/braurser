#include <braurser/daemon/core/http.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <limits>
#include <curl/curl.h>
#include <mutex>
#include <sys/socket.h>
#include <unistd.h>

namespace braurser::daemon::core {
namespace {

std::once_flag curl_once;
CURLcode curl_start_status = CURLE_OK;

void initialize_curl() {
    curl_start_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}

std::optional<std::string> part(CURLU* url, CURLUPart name) {
    char* raw = nullptr;
    if (curl_url_get(url, name, &raw, 0) != CURLUE_OK) return std::nullopt;
    std::string value(raw);
    curl_free(raw);
    return value;
}

bool valid_header(std::string_view value) {
    return value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos &&
           value.find('\0') == std::string_view::npos;
}

std::expected<std::string, HttpError> validate_url(
    std::string_view input, const std::vector<std::string>& allowed_hosts,
    std::optional<std::string_view> base = std::nullopt) {
    if (input.empty() || input.size() > 8192 || input.find('\0') != std::string_view::npos ||
        !valid_header(input)) {
        return std::unexpected(HttpError{1, "invalid URL"});
    }
    CURLU* url = curl_url();
    if (!url) return std::unexpected(HttpError{2, "URL allocation failed"});
    auto cleanup = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(url, curl_url_cleanup);
    if (base && curl_url_set(url, CURLUPART_URL, std::string(*base).c_str(), 0) != CURLUE_OK) {
        return std::unexpected(HttpError{1, "invalid base URL"});
    }
    if (curl_url_set(url, CURLUPART_URL, std::string(input).c_str(), 0) != CURLUE_OK) {
        return std::unexpected(HttpError{1, "invalid URL"});
    }
    const auto scheme = part(url, CURLUPART_SCHEME);
    const auto host = part(url, CURLUPART_HOST);
    if (!scheme || *scheme != "https" || !host ||
        std::find(allowed_hosts.begin(), allowed_hosts.end(), *host) == allowed_hosts.end() ||
        part(url, CURLUPART_USER) || part(url, CURLUPART_PASSWORD) ||
        part(url, CURLUPART_OPTIONS)) {
        return std::unexpected(HttpError{1, "URL scheme, host or credentials refused"});
    }
    const auto normalized = part(url, CURLUPART_URL);
    if (!normalized || normalized->size() > 8192) {
        return std::unexpected(HttpError{1, "invalid URL"});
    }
    return *normalized;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                               value.back() == '\r' || value.back() == '\n')) value.remove_suffix(1);
    return value;
}

bool matches(std::string_view key, std::string_view target) {
    return key.size() == target.size() &&
           std::equal(key.begin(), key.end(), target.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

struct ResponseState {
    HttpTransfer transfer;
    std::size_t limit;
    bool exceeded{false};
};

std::size_t write_body(char* data, std::size_t size, std::size_t count, void* user) {
    auto* state = static_cast<ResponseState*>(user);
    if (size != 0 && count > SIZE_MAX / size) return 0;
    const auto total = size * count;
    if (total > state->limit - state->transfer.response.body.size()) {
        state->exceeded = true;
        return 0;
    }
    state->transfer.response.body.append(data, total);
    return total;
}

std::size_t capture_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto* state = static_cast<ResponseState*>(user);
    if (size != 0 && count > SIZE_MAX / size) return 0;
    const auto total = size * count;
    if (total > 16384) return 0;
    const std::string_view line(data, total);
    if (line.starts_with("HTTP/")) {
        state->transfer.response.etag.reset();
        state->transfer.response.last_modified.reset();
        state->transfer.response.content_type.clear();
        state->transfer.location.reset();
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return total;
    const auto key = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1));
    if (value.size() > 8192 || !valid_header(value)) return 0;
    if (matches(key, "etag")) state->transfer.response.etag = value;
    if (matches(key, "last-modified")) state->transfer.response.last_modified = value;
    if (matches(key, "content-type")) state->transfer.response.content_type = value;
    if (matches(key, "location")) state->transfer.location = value;
    return total;
}

curl_socket_t open_guarded(void* data, curlsocktype, struct curl_sockaddr* address) {
    const auto* guard = static_cast<const ConnectGuard*>(data);
    if (address->addrlen <= 0 ||
        !guard->allow(reinterpret_cast<const sockaddr*>(&address->addr),
                      static_cast<socklen_t>(address->addrlen))) return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
}

class CurlTransport final : public HttpTransport {
public:
    std::expected<HttpTransfer, HttpError> perform(
        const HttpRequest& request, std::string_view url) override {
        std::call_once(curl_once, initialize_curl);
        if (curl_start_status != CURLE_OK) return std::unexpected(HttpError{2, "curl initialization failed"});
        auto handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>(curl_easy_init(), curl_easy_cleanup);
        if (!handle) return std::unexpected(HttpError{2, "curl handle allocation failed"});
        ResponseState state{{}, request.max_bytes};
        const std::string target(url);
        curl_slist* raw_headers = nullptr;
        if (request.if_none_match) {
            raw_headers = curl_slist_append(raw_headers, ("If-None-Match: " + *request.if_none_match).c_str());
        }
        if (request.if_modified_since) {
            raw_headers = curl_slist_append(raw_headers, ("If-Modified-Since: " + *request.if_modified_since).c_str());
        }
        auto headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>(raw_headers, curl_slist_free_all);
        const long timeout = static_cast<long>(request.timeout.count());
        curl_easy_setopt(handle.get(), CURLOPT_URL, target.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, request.user_agent.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, timeout);
        curl_easy_setopt(handle.get(), CURLOPT_MAXFILESIZE_LARGE,
                         static_cast<curl_off_t>(request.max_bytes));
        curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
        curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*");
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, capture_header);
        curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &state);
        const PublicOnlyGuard default_guard;
        const ConnectGuard* guard = request.guard;
        if (!guard && !request.allow_private_addresses) guard = &default_guard;
        if (guard) {
            curl_easy_setopt(handle.get(), CURLOPT_OPENSOCKETFUNCTION, open_guarded);
            curl_easy_setopt(handle.get(), CURLOPT_OPENSOCKETDATA, guard);
        }
        const auto status = curl_easy_perform(handle.get());
        if (status != CURLE_OK) {
            return std::unexpected(HttpError{static_cast<int>(status),
                                              state.exceeded ? "response exceeds limit" : "HTTP transport failed"});
        }
        curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &state.transfer.response.status);
        state.transfer.response.not_modified = state.transfer.response.status == 304;
        return std::move(state.transfer);
    }
};

}

CurlHttpClient::CurlHttpClient()
    : owned_transport_(std::make_unique<CurlTransport>()), transport_(owned_transport_.get()) {}

CurlHttpClient::CurlHttpClient(HttpTransport& transport) : transport_(&transport) {}

std::expected<HttpResponse, HttpError> CurlHttpClient::get(const HttpRequest& request) {
    if (request.allowed_hosts.empty() || request.max_redirects < 0 || request.max_redirects > 10 ||
        request.timeout.count() <= 0 || request.timeout.count() > LONG_MAX ||
        request.max_bytes > static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max()) ||
        request.user_agent.empty() || request.user_agent.size() > 256 ||
        !valid_header(request.user_agent) ||
        (request.if_none_match && (!valid_header(*request.if_none_match) || request.if_none_match->size() > 8192)) ||
        (request.if_modified_since && (!valid_header(*request.if_modified_since) ||
                                      request.if_modified_since->size() > 8192))) {
        return std::unexpected(HttpError{1, "invalid HTTP request"});
    }
    for (const auto& host : request.allowed_hosts) {
        if (host.empty() || host.size() > 253 ||
            !std::all_of(host.begin(), host.end(), [](unsigned char c) {
                return std::islower(c) || std::isdigit(c) || c == '-' || c == '.' || c == ':' || c == '[' || c == ']';
            })) {
            return std::unexpected(HttpError{1, "invalid allowed host"});
        }
    }
    auto current = validate_url(request.url, request.allowed_hosts);
    if (!current) return std::unexpected(current.error());
    for (int redirect = 0;; ++redirect) {
        auto transfer = transport_->perform(request, *current);
        if (!transfer) return std::unexpected(transfer.error());
        if (transfer->response.body.size() > request.max_bytes) {
            return std::unexpected(HttpError{1, "response exceeds limit"});
        }
        const auto code = transfer->response.status;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            if (!transfer->location || redirect >= request.max_redirects) {
                return std::unexpected(HttpError{1, "redirect refused"});
            }
            auto next = validate_url(*transfer->location, request.allowed_hosts, *current);
            if (!next) return std::unexpected(next.error());
            current = std::move(next);
            continue;
        }
        transfer->response.not_modified = code == 304;
        transfer->response.final_url = *current;
        return std::move(transfer->response);
    }
}

}
