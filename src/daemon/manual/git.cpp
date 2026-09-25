#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/connect.hpp>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace braurser::daemon::manual {
namespace {

ManualError fail(std::string code, std::string detail = {}) {
    return {std::move(code), std::move(detail), {}, {}};
}

bool safe_host(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') return false;
    bool label_start = true;
    for (char ch : host) {
        if (ch == '.') {
            if (label_start) return false;
            label_start = true;
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            label_start = false;
        } else if (ch == '-' && !label_start) {
            label_start = false;
        } else return false;
    }
    return host.back() != '-';
}

bool valid_path(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.size() > 4096 ||
        path.find("..") != path.npos) return false;
    for (std::size_t start = 1; start < path.size();) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end == path.npos ? end : end - start);
        if (component.empty() || component == "." || component == ".." || component.size() > 255) return false;
        for (const char raw_ch : component) {
            const auto ch = static_cast<unsigned char>(raw_ch);
            if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')) return false;
        }
        start = end == path.npos ? path.size() : end + 1;
        if (end != path.npos && start == path.size()) return false;
    }
    return path.size() > 1;
}

struct ReplyState {
    GitReply reply;
    bool exceeded{};
    std::size_t headers{};
};

size_t body_cb(char* data, size_t size, size_t count, void* context) {
    auto& state = *static_cast<ReplyState*>(context);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    if (bytes > 512ULL * 1024 * 1024 - state.reply.body.size()) {
        state.exceeded = true;
        return 0;
    }
    state.reply.body.append(data, bytes);
    return bytes;
}

size_t header_cb(char* data, size_t size, size_t count, void* context) {
    auto& state = *static_cast<ReplyState*>(context);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    if (bytes > 65536 - state.headers) return 0;
    state.headers += bytes;
    std::string_view line(data, bytes);
    if (line.starts_with("HTTP/")) state.reply.location.clear();
    if (line.size() >= 9 && std::equal(line.begin(), line.begin() + 9, "Location:",
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b)); })) {
        line.remove_prefix(9);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
        if (line.size() > 8192 || line.find_first_of("\r\n") != line.npos || line.find('\0') != line.npos) return 0;
        state.reply.location = line;
    }
    return bytes;
}

curl_socket_t guarded_socket(void*, curlsocktype, curl_sockaddr* target) {
    static const core::PublicOnlyGuard guard;
    if (target->addrlen <= 0 || !guard.allow(reinterpret_cast<const sockaddr*>(&target->addr),
                                              static_cast<socklen_t>(target->addrlen))) return CURL_SOCKET_BAD;
    return socket(target->family, target->socktype | SOCK_CLOEXEC, target->protocol);
}

}

std::expected<GitUrl, ManualError> validate_git_url(std::string_view input) {
    if (!input.starts_with("https://") || input.size() > 8192 ||
        (input.find_first_of("\r\n\\?#%") != input.npos || input.find('\0') != input.npos)) return std::unexpected(fail("git-url"));
    auto slash = input.find('/', 8);
    if (slash == input.npos) return std::unexpected(fail("git-url"));
    const auto authority = input.substr(8, slash - 8);
    if (authority.empty() || authority.find('@') != authority.npos) return std::unexpected(fail("git-url"));
    std::string host;
    if (authority.front() == '[') {
        auto bracket = authority.find(']');
        if (bracket == authority.npos || (authority.size() != bracket + 1 && authority.substr(bracket + 1) != ":443"))
            return std::unexpected(fail("git-url"));
        in6_addr address{};
        std::string literal(authority.substr(1, bracket - 1));
        if (inet_pton(AF_INET6, literal.c_str(), &address) != 1) return std::unexpected(fail("git-url"));
        sockaddr_in6 socket_address{};
        socket_address.sin6_family = AF_INET6;
        socket_address.sin6_addr = address;
        if (!core::PublicOnlyGuard{}.allow(reinterpret_cast<sockaddr*>(&socket_address), sizeof(socket_address)))
            return std::unexpected(fail("git-address"));
        host = "[" + literal + "]";
    } else {
        auto colon = authority.find(':');
        if (colon != authority.npos && authority.substr(colon) != ":443") return std::unexpected(fail("git-port"));
        host = std::string(authority.substr(0, colon));
        if (!safe_host(host)) return std::unexpected(fail("git-host"));
        in_addr address{};
        if (inet_pton(AF_INET, host.c_str(), &address) == 1) {
            sockaddr_in socket_address{};
            socket_address.sin_family = AF_INET;
            socket_address.sin_addr = address;
            if (!core::PublicOnlyGuard{}.allow(reinterpret_cast<sockaddr*>(&socket_address), sizeof(socket_address)))
                return std::unexpected(fail("git-address"));
        } else if (std::all_of(host.begin(), host.end(), [](unsigned char ch) {
                       return std::isdigit(ch) || ch == '.';
                   })) return std::unexpected(fail("git-address"));
    }
    auto path = input.substr(slash);
    if (!valid_path(path)) return std::unexpected(fail("git-path"));
    if (path.ends_with(".git")) path.remove_suffix(4);
    if (path.size() <= 1) return std::unexpected(fail("git-path"));
    return GitUrl{std::string(input), std::move(host), std::string(path.substr(1))};
}

std::expected<GitReply, ManualError> CurlGitHttp::request(
    std::string_view url, std::string_view body, bool post) {
    if (url.size() > 8192 || !url.starts_with("https://") ||
        (post && body.size() > 512ULL * 1024 * 1024)) return std::unexpected(fail("git-request"));
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!initialized) return std::unexpected(fail("curl-init"));
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return std::unexpected(fail("curl-init"));
    ReplyState state;
    const std::string target(url);
    curl_slist* raw = curl_slist_append(nullptr, post ?
        "Content-Type: application/x-git-upload-pack-request" :
        "Accept: application/x-git-upload-pack-advertisement");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw, curl_slist_free_all);
    if (!headers) return std::unexpected(fail("curl-headers"));
    if (post) {
        raw = curl_slist_append(headers.get(), "Accept: application/x-git-upload-pack-result");
        if (!raw) return std::unexpected(fail("curl-headers"));
        headers.release();
        headers.reset(raw);
    }
    curl_easy_setopt(handle.get(), CURLOPT_URL, target.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(handle.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(handle.get(), CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(512ULL * 1024 * 1024));
    curl_easy_setopt(handle.get(), CURLOPT_OPENSOCKETFUNCTION, guarded_socket);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, body_cb);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &state);
    if (post) {
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    const auto status = curl_easy_perform(handle.get());
    if (status != CURLE_OK) return std::unexpected(fail(state.exceeded ? "git-size" : "git-connect"));
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &state.reply.status);
    return std::move(state.reply);
}

std::expected<std::string, ManualError> GuardedGitTransport::exchange(
    std::string_view url, std::string_view payload) {
    auto parsed = validate_git_url(url);
    if (!parsed) return std::unexpected(parsed.error());
    const bool post = !payload.empty();
    const auto suffix = post ? "/git-upload-pack" : "/info/refs?service=git-upload-pack";
    for (int hop = 0; hop < 5; ++hop) {
        auto reply = http_.request(parsed->url + suffix, payload, post);
        if (!reply) return std::unexpected(reply.error());
        if (reply->status == 301 || reply->status == 302 || reply->status == 303 ||
            reply->status == 307 || reply->status == 308) {
            auto redirected = reply->location;
            if (!redirected.ends_with(suffix)) return std::unexpected(fail("git-redirect"));
            redirected.resize(redirected.size() - std::strlen(suffix));
            auto destination = validate_git_url(redirected);
            if (!destination) return std::unexpected(destination.error());
            if (destination->host != parsed->host)
                return std::unexpected(ManualError{"RedirectRequired", {}, parsed->host, destination->host});
            parsed = std::move(destination);
            continue;
        }
        if (reply->status != 200 || reply->body.size() > 512ULL * 1024 * 1024)
            return std::unexpected(fail("git-response"));
        return std::move(reply->body);
    }
    return std::unexpected(fail("git-redirect-limit"));
}

}
