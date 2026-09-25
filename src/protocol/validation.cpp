#include <braurser/protocol/dto.hpp>
#include <braurser/protocol/frame.hpp>
#include <array>
#include <charconv>
#include <cctype>

namespace braurser::protocol {
namespace {

bool in(std::string_view value, std::initializer_list<std::string_view> choices) {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool ascii_digits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool utc_timestamp(std::string_view s) {
    if (s.size() < 20 || s.size() > 32 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
        s[13] != ':' || s[16] != ':' || s.back() != 'Z') return false;
    for (std::size_t i : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    if (s.size() > 20) {
        if (s[19] != '.' || s.size() == 21 || !ascii_digits(s.substr(20, s.size() - 21))) return false;
    }
    const auto num = [&s](std::size_t index) { return 10 * (s[index] - '0') + (s[index + 1] - '0'); };
    const int month = num(5);
    const int day = num(8);
    const int year = 1000 * (s[0] - '0') + 100 * (s[1] - '0') + 10 * (s[2] - '0') + (s[3] - '0');
    if (month < 1 || month > 12 || year < 1 || num(11) > 23 || num(14) > 59 || num(17) > 60) return false;
    constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int max_day = days[static_cast<std::size_t>(month - 1)] +
                        (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 1 : 0);
    return day >= 1 && day <= max_day;
}

bool https_url(std::string_view text) {
    if (!text.starts_with("https://") || text.size() > 2048) return false;
    const auto authority_end = text.find_first_of("/?#", 8);
    const auto authority = text.substr(8, authority_end == text.npos ? text.npos : authority_end - 8);
    const auto separator = authority.find(':');
    const auto host = authority.substr(0, separator);
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.' ||
        host.find('.') == host.npos || host.find("..") != host.npos ||
        !std::any_of(host.begin(), host.end(), [](unsigned char c) { return std::isalpha(c) != 0; })) return false;
    if (separator != authority.npos) {
        const auto port = authority.substr(separator + 1);
        if (port.empty() || port.size() > 5 || !ascii_digits(port)) return false;
        unsigned number = 0;
        for (char c : port) number = number * 10 + static_cast<unsigned>(c - '0');
        if (number == 0 || number > 65535) return false;
    }
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c <= 32 || c >= 127 || c == '\\') return false;
    }
    for (std::size_t start = 0; start < host.size();) {
        const auto end = host.find('.', start);
        const auto part = host.substr(start, end == host.npos ? host.npos : end - start);
        if (part.empty() || part.size() > 63 || part.front() == '-' || part.back() == '-') return false;
        for (char ch : part) {
            const auto c = static_cast<unsigned char>(ch);
            if (!(std::isalnum(c) || c == '-')) return false;
        }
        if (end == host.npos) break;
        start = end + 1;
    }
    return true;
}

bool safe_path(std::string_view text) {
    if (text.empty() || text.size() > 1024 || text.front() == '/' || text.find('\\') != text.npos || text.find('\0') != text.npos) return false;
    while (!text.empty()) {
        const auto slash = text.find('/');
        const auto part = text.substr(0, slash);
        if (part.empty() || part == "." || part == ".." || !br_valid_path_component(part.data(), part.size())) return false;
        if (slash == text.npos) break;
        text.remove_prefix(slash + 1);
    }
    return true;
}

bool package_target(std::string_view text) {
    std::size_t slash = 0;
    return text.size() <= 320 && br_split_target(text.data(), text.size(), &slash);
}

void validate_string(std::string_view s, std::string_view key, std::size_t& total) {
    const std::size_t cap = in(key, {"script_text", "text", "pkgbuild"}) ? 256 * 1024 :
                            in(key, {"description", "line"}) ? 16384 : 4096;
    if (s.size() > cap || s.size() > 512 * 1024 - total || !valid_utf8(s)) throw std::invalid_argument("invalid string");
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == 127 || (c < 32 && !(in(key, {"script_text", "description", "text", "pkgbuild", "line", "code", "s"}) &&
                                   (c == '\n' || c == '\t'))))
            throw std::invalid_argument("invalid control character");
    }
    total += s.size();
    if (in(key, {"package_base", "packageId", "alsoInstallPackageId", "variantId"}) &&
        !package_target(s)) throw std::invalid_argument("invalid package name");
    if (in(key, {"repo", "distro_id"}) && !br_valid_repo_name(s.data(), s.size())) throw std::invalid_argument("invalid repository");
    if (in(key, {"url", "href", "httpsUrl", "source_url", "sourceUrl", "upstream_url"}) &&
        !https_url(s)) throw std::invalid_argument("invalid URL");
    if (in(key, {"host", "from_host", "to_host"}) &&
        !https_url(std::string("https://") + std::string(s)))
        throw std::invalid_argument("invalid host");
    if (key == "owner_repo") {
        const auto slash = s.find('/');
        if (slash == s.npos || s.find('/', slash + 1) != s.npos ||
            !br_valid_repo_name(s.data(), slash) ||
            !br_valid_repo_name(s.data() + slash + 1, s.size() - slash - 1))
            throw std::invalid_argument("invalid repository path");
    }
    if (in(key, {"fetched_at", "fetchedAt", "published", "checked_at", "installed_at", "scanTime", "expires_at", "out_of_date_since", "first_submitted", "last_modified"}) &&
        !utc_timestamp(s)) throw std::invalid_argument("invalid timestamp");
    if (in(key, {"commit", "newCommit"}) &&
        (s.size() != 40 || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })))
        throw std::invalid_argument("invalid commit");
    if (in(key, {"scriptPath", "script_path"}) && !safe_path(s)) throw std::invalid_argument("invalid path");
    if (in(key, {"message_key", "reason_key", "attribution_key"}) &&
        (s.empty() || s.size() > 128 || !std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        }))) throw std::invalid_argument("invalid message key");
    if (in(key, {"cursor", "nextCursor", "next_cursor", "eventsCursor", "jobId", "planId", "approvalToken", "approval_token", "reportId", "previewId", "receiptId", "questionId", "choiceId", "questionToken", "question_id", "token", "plan_id", "report_id", "preview_id", "receipt_id"}) &&
        (s.empty() || s.size() > 128)) throw std::invalid_argument("invalid identifier");
    if (key == "officialAction" && s != "update-system") throw std::invalid_argument("invalid action");
}

void inspect(const nlohmann::json& j, std::string_view key, unsigned depth, std::size_t& total, std::size_t& nodes) {
    if ((depth > 32 && j.is_structured()) || ++nodes > 16384)
        throw std::invalid_argument("JSON limit exceeded");
    if (j.is_discarded() || j.is_binary()) throw std::invalid_argument("invalid JSON value");
    if (j.is_string()) {
        validate_string(j.get_ref<const std::string&>(), key, total);
    } else if (j.is_array()) {
        if (j.size() > 256) throw std::invalid_argument("array limit exceeded");
        for (const auto& value : j) inspect(value, key == "external_refs" ? "url" : "", depth + 1, total, nodes);
    } else if (j.is_object()) {
        if (j.size() > 256) throw std::invalid_argument("object limit exceeded");
        for (auto it = j.begin(); it != j.end(); ++it) {
            validate_string(it.key(), "", total);
            inspect(it.value(), it.key(), depth + 1, total, nodes);
        }
    }
}

}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) { ++i; continue; }
        int following = 0;
        std::uint32_t cp = 0;
        if (byte >= 0xc2 && byte <= 0xdf) { following = 1; cp = byte & 0x1fU; }
        else if (byte >= 0xe0 && byte <= 0xef) { following = 2; cp = byte & 0x0fU; }
        else if (byte >= 0xf0 && byte <= 0xf4) { following = 3; cp = byte & 0x07U; }
        else return false;
        if (i + static_cast<std::size_t>(following) >= text.size()) return false;
        for (int j = 1; j <= following; ++j) {
            const auto next = static_cast<unsigned char>(text[i + static_cast<std::size_t>(j)]);
            if ((next & 0xc0U) != 0x80U) return false;
            cp = (cp << 6U) | (next & 0x3fU);
        }
        if ((following == 1 && cp < 0x80) || (following == 2 && cp < 0x800) ||
            (following == 3 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
        i += static_cast<std::size_t>(following + 1);
    }
    return true;
}

void validate_wire(const nlohmann::json& value, std::string_view key) {
    std::size_t total = 0;
    std::size_t nodes = 0;
    inspect(value, key, 0, total, nodes);
}

}
