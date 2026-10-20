#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/connect.hpp>
#include <braurser/daemon/core/files.hpp>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <utility>
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
            if (!(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '+' || ch == '@')) return false;
        }
        start = end == path.npos ? path.size() : end + 1;
        if (end != path.npos && start == path.size()) return false;
    }
    return path.size() > 1;
}

struct ReplyState {
    GitReply reply;
    std::optional<ManualError> error;
    std::size_t headers{};
};

size_t body_cb(char* data, size_t size, size_t count, void* context) {
    auto& state = *static_cast<ReplyState*>(context);
    if (size && count > SIZE_MAX / size) return 0;
    const auto bytes = size * count;
    auto result = state.reply.body.append(std::string_view(data, bytes));
    if (!result) {
        state.error = std::move(result.error());
        return 0;
    }
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

GitReply::Body::~Body() { if (fd_ >= 0) close(fd_); }

GitReply::Body::Body(Body&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), size_(std::exchange(other.size_, 0)), limit_(other.limit_) {}

GitReply::Body& GitReply::Body::operator=(Body&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        size_ = std::exchange(other.size_, 0);
        limit_ = other.limit_;
    }
    return *this;
}

std::expected<GitReply::Body, ManualError> GitReply::Body::create(
    const std::filesystem::path& cache_root, std::size_t max_bytes) {
    if (!cache_root.is_absolute()) return std::unexpected(fail("git-spool"));
    const auto manual = cache_root / "braurser/manual";
    const auto directory = manual / "spool";
    for (const auto& part : {cache_root / "braurser", manual, directory}) {
        if (!core::ensure_private_dir(part)) return std::unexpected(fail("git-spool"));
    }
    Body result;
    result.limit_ = std::min(max_bytes, kMaxBytes);
    result.fd_ = open(directory.c_str(), O_RDWR | O_TMPFILE | O_CLOEXEC, 0600);
    if (result.fd_ < 0) {
        auto pattern = (directory / "response-XXXXXX").string();
        result.fd_ = mkostemp(pattern.data(), O_CLOEXEC);
        if (result.fd_ < 0) return std::unexpected(fail("git-spool"));
        int removed;
        do { removed = unlink(pattern.c_str()); } while (removed != 0 && errno == EINTR);
        if (removed != 0) return std::unexpected(fail("git-spool"));
    }
    return result;
}

std::expected<void, ManualError> GitReply::Body::append(std::string_view bytes) {
    if (bytes.size() > limit_ - size_) {
        *this = Body{};
        return std::unexpected(fail("git-size"));
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto amount = std::min(bytes.size() - offset, kReadChunk);
        const auto written = pwrite(fd_, bytes.data() + offset, amount, static_cast<off_t>(size_));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            *this = Body{};
            return std::unexpected(fail("git-spool-write"));
        }
        const auto count = static_cast<std::size_t>(written);
        offset += count;
        size_ += count;
    }
    return {};
}

std::expected<std::size_t, ManualError> GitReply::Body::read(
    std::span<char> buffer, std::size_t offset) const {
    if (offset > size_) return std::unexpected(fail("git-spool-read"));
    const auto amount = std::min({buffer.size(), size_ - offset, kReadChunk});
    std::size_t copied = 0;
    while (copied < amount) {
        const auto count = pread(fd_, buffer.data() + copied, amount - copied,
                                 static_cast<off_t>(offset + copied));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return std::unexpected(fail("git-spool-read"));
        copied += static_cast<std::size_t>(count);
    }
    return copied;
}

std::expected<GitReply, ManualError> GitHttp::request_bounded(
    std::string_view url, std::string_view body, bool post, std::size_t max_bytes) {
    auto result = request(url, body, post);
    if (result && result->body.size() > std::min(max_bytes, GitReply::Body::kMaxBytes))
        return std::unexpected(fail("git-size"));
    return result;
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
    return request_bounded(url, body, post, GitReply::Body::kMaxBytes);
}

std::expected<GitReply, ManualError> CurlGitHttp::request_bounded(
    std::string_view url, std::string_view body, bool post, std::size_t max_bytes) {
    if (cancel_ && cancel_->load()) return std::unexpected(fail("cancelled"));
    if (url.size() > 8192 || !url.starts_with("https://") ||
        (post && body.size() > 512ULL * 1024 * 1024)) return std::unexpected(fail("git-request"));
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!initialized) return std::unexpected(fail("curl-init"));
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) return std::unexpected(fail("curl-init"));
    auto cache = cache_root_;
    if (cache.empty()) {
        const auto* configured = std::getenv("XDG_CACHE_HOME");
        const auto* home = std::getenv("HOME");
        if (configured && *configured) cache = configured;
        else if (home && *home) cache = std::filesystem::path(home) / ".cache";
    }
    auto spool = GitReply::Body::create(cache, max_bytes);
    if (!spool) return std::unexpected(spool.error());
    ReplyState state;
    state.reply.body = std::move(*spool);
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
    if (cancel_) {
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0;
            });
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, cancel_);
    }
    curl_easy_setopt(handle.get(), CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(std::min(max_bytes, GitReply::Body::kMaxBytes)));
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
    if (status == CURLE_ABORTED_BY_CALLBACK) return std::unexpected(fail("cancelled"));
    if (state.error) return std::unexpected(std::move(*state.error));
    if (status != CURLE_OK)
        return std::unexpected(fail(status == CURLE_FILESIZE_EXCEEDED ? "git-size" : "git-connect"));
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &state.reply.status);
    return std::move(state.reply);
}

std::expected<GitReply::Body, ManualError> GuardedGitTransport::exchange(
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
