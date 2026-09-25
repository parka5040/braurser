#include <braurser/daemon/core/text.hpp>

#include <algorithm>
#include <array>
#include <cctype>

namespace braurser::daemon::core {

std::string strip_terminal_controls(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == 0x1b) {
            ++i;
            if (i == text.size()) break;
            if (text[i] == '[') {
                ++i;
                while (i < text.size()) {
                    const auto next = static_cast<unsigned char>(text[i++]);
                    if (next >= 0x40 && next <= 0x7e) break;
                }
            } else if (text[i] == ']') {
                ++i;
                while (i < text.size()) {
                    if (text[i++] == '\a') break;
                    if (text[i - 1] == '\x1b' && i < text.size() && text[i] == '\\') {
                        ++i;
                        break;
                    }
                }
            } else {
                ++i;
            }
        } else {
            ++i;
            if ((byte >= 0x20 && byte != 0x7f) || byte == '\n' || byte == '\t') {
                result.push_back(static_cast<char>(byte));
            }
        }
    }
    return result;
}

std::string redact(std::string_view text) {
    static constexpr std::array<std::string_view, 5> keys{
        "authorization", "password", "passwd", "secret", "token"};
    std::string output;
    output.reserve(text.size());
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        if (text.substr(cursor, 3) == "://") {
            const auto start = cursor + 3;
            const auto end = text.find_first_of("/ \t\r\n?#", start);
            const auto authority_end = end == std::string_view::npos ? text.size() : end;
            const auto at = text.find('@', start);
            if (at != std::string_view::npos && at < authority_end) {
                output += "://[REDACTED]@";
                cursor = at + 1;
                continue;
            }
        }
        bool masked = false;
        for (const auto key : keys) {
            if (cursor + key.size() >= text.size() ||
                (cursor > 0 && (std::isalnum(static_cast<unsigned char>(text[cursor - 1])) ||
                                text[cursor - 1] == '_'))) continue;
            bool equal = true;
            for (std::size_t i = 0; i < key.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(text[cursor + i])) != key[i]) {
                    equal = false;
                    break;
                }
            }
            if (!equal || (text[cursor + key.size()] != ':' && text[cursor + key.size()] != '=')) continue;
            output.append(text.substr(cursor, key.size() + 1));
            cursor += key.size() + 1;
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t')) {
                output.push_back(text[cursor++]);
            }
            if (cursor < text.size() && text[cursor] != '\r' && text[cursor] != '\n') {
                output += "[REDACTED]";
                while (cursor < text.size() && !std::isspace(static_cast<unsigned char>(text[cursor])) &&
                       text[cursor] != '&' && text[cursor] != ';') ++cursor;
            }
            masked = true;
            break;
        }
        if (!masked) output.push_back(text[cursor++]);
    }
    return output;
}

std::string bounded_line(std::string_view text, std::size_t max) {
    if (text.size() <= max) return std::string(text);
    auto size = max;
    while (size > 0 && size < text.size() &&
           (static_cast<unsigned char>(text[size]) & 0xc0U) == 0x80) --size;
    return std::string(text.substr(0, size)) + "…";
}

}
