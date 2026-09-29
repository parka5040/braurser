#include <braurser/daemon/catalog/sources.hpp>

#include <algorithm>
#include <unordered_map>

namespace braurser::daemon::catalog {
namespace {

bool utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch == 0) return false;
        if (ch < 0x80) { ++i; continue; }
        const int count = ch >= 0xF0 && ch <= 0xF4 ? 4 :
                          ch >= 0xE0 && ch <= 0xEF ? 3 :
                          ch >= 0xC2 && ch <= 0xDF ? 2 : 0;
        if (!count || i + static_cast<std::size_t>(count) > text.size()) return false;
        const auto next = static_cast<unsigned char>(text[i + 1]);
        if (next < 0x80 || next > 0xBF || (ch == 0xE0 && next < 0xA0) ||
            (ch == 0xED && next >= 0xA0) || (ch == 0xF0 && next < 0x90) ||
            (ch == 0xF4 && next >= 0x90)) return false;
        for (int n = 2; n < count; ++n) {
            const auto cont = static_cast<unsigned char>(text[i + static_cast<std::size_t>(n)]);
            if (cont < 0x80 || cont > 0xBF) return false;
        }
        i += static_cast<std::size_t>(count);
    }
    return true;
}

std::expected<std::string, CatalogError> decode(std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') { result += value[i]; continue; }
        if (++i == value.size()) return std::unexpected(CatalogError{"desktop-escape", {}});
        switch (value[i]) {
        case 's': result += ' '; break;
        case 'n': result += '\n'; break;
        case 't': result += '\t'; break;
        case 'r': result += '\r'; break;
        case '\\': result += '\\'; break;
        case ';': result += ';'; break;
        default: return std::unexpected(CatalogError{"desktop-escape", {}});
        }
    }
    return result;
}

std::vector<std::string> split(std::string_view raw) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= raw.size(); ++index) {
        if (index < raw.size() && raw[index] == '\\') { ++index; continue; }
        if (index != raw.size() && raw[index] != ';') continue;
        if (index > start) {
            auto item = decode(raw.substr(start, index - start));
            if (item) result.push_back(std::move(*item));
        }
        start = index + 1;
    }
    return result;
}

}

std::expected<DesktopEntry, CatalogError> parse_desktop_entry(std::string_view text,
                                                                std::string_view locale) {
    if (text.size() > 65536 || !utf8(text)) return std::unexpected(CatalogError{"desktop-input", {}});
    std::unordered_map<std::string, std::string> fields;
    std::unordered_map<std::string, std::string> raw_lists;
    bool in_group = false;
    bool seen_group = false;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find('\n', start);
        if (end == text.npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        start = end == text.size() ? text.size() : end + 1;
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            if (line.back() != ']') return std::unexpected(CatalogError{"desktop-group", {}});
            in_group = line == "[Desktop Entry]";
            if (in_group) {
                if (seen_group) return std::unexpected(CatalogError{"desktop-duplicate", {}});
                seen_group = true;
            }
            continue;
        }
        if (!in_group) continue;
        const auto eq = line.find('=');
        if (eq == line.npos || eq == 0) return std::unexpected(CatalogError{"desktop-key", {}});
        const auto key = line.substr(0, eq);
        if (std::ranges::any_of(key, [](char ch) {
                return !(ch >= 'A' && ch <= 'Z') && !(ch >= 'a' && ch <= 'z') &&
                       !(ch >= '0' && ch <= '9') && ch != '-' && ch != '[' && ch != ']' && ch != '@' && ch != '_';
            })) return std::unexpected(CatalogError{"desktop-key", {}});
        auto raw = line.substr(eq + 1);
        auto value = decode(raw);
        if (!value) return std::unexpected(value.error());
        if (key == "Categories" || key == "MimeType") raw_lists.emplace(key, raw);
        if (!fields.emplace(key, std::move(*value)).second)
            return std::unexpected(CatalogError{"desktop-duplicate", {}});
    }
    if (!seen_group) return std::unexpected(CatalogError{"desktop-group", {}});
    DesktopEntry result;
    auto get = [&](std::string_view key) {
        auto it = fields.find(std::string(key));
        return it == fields.end() ? std::string{} : it->second;
    };
    std::string base(locale);
    if (const auto cut = base.find_first_of(".@"); cut != std::string::npos) base.resize(cut);
    if (base.size() > 32) return std::unexpected(CatalogError{"desktop-locale", {}});
    auto localized = [&](std::string_view key) {
        if (!base.empty()) {
            auto exact = get(std::string(key) + "[" + base + "]");
            if (!exact.empty()) return exact;
            auto underscore = base.find('_');
            if (underscore != base.npos) {
                auto language = get(std::string(key) + "[" + base.substr(0, underscore) + "]");
                if (!language.empty()) return language;
            }
        }
        return get(key);
    };
    result.type = get("Type");
    if (result.type != "Application" && result.type != "Link" && result.type != "Directory")
        return std::unexpected(CatalogError{"desktop-type", {}});
    result.name = localized("Name");
    if (result.name.empty()) return std::unexpected(CatalogError{"desktop-name", {}});
    result.generic_name = localized("GenericName");
    result.comment = localized("Comment");
    result.exec = get("Exec");
    result.try_exec = get("TryExec");
    result.icon = get("Icon");
    auto boolean = [&](const char* key, bool& dest) -> bool {
        const auto value = get(key);
        if (value.empty() || value == "false") return true;
        if (value == "true") { dest = true; return true; }
        return false;
    };
    if (!boolean("NoDisplay", result.no_display) || !boolean("Hidden", result.hidden))
        return std::unexpected(CatalogError{"desktop-boolean", {}});
    result.categories = split(raw_lists["Categories"]);
    result.mime_types = split(raw_lists["MimeType"]);
    result.startup_wm_class = get("StartupWMClass");
    return result;
}

}
