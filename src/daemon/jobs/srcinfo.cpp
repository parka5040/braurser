#include <braurser/daemon/jobs/srcinfo.hpp>
#include <braurser/domain/vercmp.hpp>
#include <braurser/protocol/dto.hpp>
#include <braurser/validate/validate.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace braurser::daemon::jobs {
namespace {
bool version_value(std::string_view value) {
    return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || std::string_view("._+~:-").find(static_cast<char>(ch)) != std::string_view::npos;
    });
}
bool word(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
}
using Fields = std::map<std::string, std::vector<std::string>, std::less<>>;
}

std::optional<Dependency> parse_dependency(std::string_view text) {
    if (text.empty() || text.size() > 512) return std::nullopt;
    const auto end = text.find_first_of("<>=");
    const auto name = text.substr(0, end);
    if (!br_valid_pkgname(name.data(), name.size())) return std::nullopt;
    Dependency result{std::string(name), {}, {}};
    if (end == text.npos) return result;
    auto remaining = text.substr(end);
    result.comparison = remaining.substr(0, 1);
    remaining.remove_prefix(1);
    if (result.comparison != "=" && remaining.starts_with('=')) {
        result.comparison += '=';
        remaining.remove_prefix(1);
    }
    if (!version_value(remaining)) return std::nullopt;
    result.version = remaining;
    return result;
}

std::optional<BuiltPackage> parse_pkginfo(std::string_view text) {
    if (text.empty() || text.size() > 256 * 1024 || !protocol::valid_utf8(text)) return std::nullopt;
    BuiltPackage result;
    std::size_t lines = 0;
    while (!text.empty()) {
        if (++lines > 4096) return std::nullopt;
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.size() > 4096 || std::ranges::any_of(line, [](unsigned char ch) { return ch < 32 || ch == 127; }))
            return std::nullopt;
        if (line.empty() || line.starts_with('#')) continue;
        const auto separator = line.find(" = ");
        if (separator == line.npos || !word(line.substr(0, separator))) return std::nullopt;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 3);
        if (key == "pkgname") {
            if (!result.name.empty() || !br_valid_pkgname(value.data(), value.size())) return std::nullopt;
            result.name = value;
        } else if (key == "pkgver") {
            if (!result.version.empty() || !version_value(value)) return std::nullopt;
            result.version = value;
        } else if (key == "arch") {
            if (!result.arch.empty() || !word(value)) return std::nullopt;
            result.arch = value;
        } else if (key == "depend") {
            if (result.depends.size() >= 256 || !parse_dependency(value)) return std::nullopt;
            result.depends.emplace_back(value);
        }
    }
    if (result.name.empty() || result.version.empty() || result.arch.empty()) return std::nullopt;
    return result;
}

bool satisfies_version(std::string_view version, const Dependency& dependency) {
    if (dependency.comparison.empty()) return true;
    if (version.empty()) return false;
    const auto order = domain::vercmp(version, dependency.version);
    return dependency.comparison == "=" ? order == 0 : dependency.comparison == "<" ? order < 0 :
        dependency.comparison == "<=" ? order <= 0 : dependency.comparison == ">" ? order > 0 :
        dependency.comparison == ">=" && order >= 0;
}

std::optional<SourceInfo> parse_srcinfo(std::string_view text, std::string_view architecture) {
    if (text.empty() || text.size() > 256 * 1024 || !word(architecture) || !protocol::valid_utf8(text)) return std::nullopt;
    SourceInfo result;
    Fields base;
    std::vector<std::pair<std::string, Fields>> sections;
    Fields* current = nullptr;
    std::size_t lines = 0;
    while (!text.empty()) {
        if (++lines > 4096) return std::nullopt;
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == text.npos ? std::string_view{} : text.substr(end + 1);
        if (line.size() > 4096 || std::ranges::any_of(line, [](unsigned char ch) {
            return ch == 127 || (ch < 32 && ch != '\t');
        })) return std::nullopt;
        if (line.empty() || line.starts_with('#')) continue;
        const bool indented = line.starts_with('\t');
        if (indented) line.remove_prefix(1);
        const auto separator = line.find(" = ");
        if (separator == line.npos) return std::nullopt;
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 3);
        if (!word(key) || value.find('\t') != value.npos) return std::nullopt;
        if (key == "pkgbase" || key == "pkgname") {
            if (indented || !br_valid_pkgname(value.data(), value.size())) return std::nullopt;
            if (key == "pkgbase") {
                if (!result.pkgbase.empty() || !sections.empty()) return std::nullopt;
                result.pkgbase = value;
                current = &base;
            } else {
                if (result.pkgbase.empty() || sections.size() >= 256 ||
                    std::ranges::any_of(sections, [&](const auto& item) { return item.first == value; })) return std::nullopt;
                sections.emplace_back(std::string(value), Fields{});
                current = &sections.back().second;
            }
            continue;
        }
        if (!indented || !current) return std::nullopt;
        const bool dependency = key == "depends" || key == "makedepends" || key == "checkdepends" || key == "provides" ||
            key.starts_with("depends_") || key.starts_with("makedepends_") || key.starts_with("checkdepends_") || key.starts_with("provides_");
        if (dependency) {
            if (!value.empty() && !parse_dependency(value)) return std::nullopt;
        } else if (key == "pkgver" || key == "pkgrel") {
            if (!version_value(value) || value.find_first_of("-:") != value.npos) return std::nullopt;
        } else if (key == "epoch") {
            if (value.empty() || value.size() > 10 || !std::ranges::all_of(value, [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) return std::nullopt;
        } else if (key == "arch") {
            if (!word(value)) return std::nullopt;
        } else continue;
        auto& values = (*current)[std::string(key)];
        if ((key == "pkgver" || key == "pkgrel" || key == "epoch") && !values.empty()) return std::nullopt;
        if (values.size() >= 256) return std::nullopt;
        if (!value.empty()) values.emplace_back(value);
    }
    if (result.pkgbase.empty() || sections.empty()) return std::nullopt;
    for (const auto& [name, fields] : sections) {
        auto inherited = [&](std::string_view key) -> std::vector<std::string> {
            const auto override = fields.find(key);
            if (override != fields.end()) return override->second;
            const auto original = base.find(key);
            return original == base.end() ? std::vector<std::string>{} : original->second;
        };
        auto scalar = [&](std::string_view key) {
            const auto values = inherited(key);
            return values.empty() ? std::string{} : values.front();
        };
        auto combined = [&](std::string_view key) {
            auto values = inherited(key);
            auto specific = inherited(std::string(key) + "_" + std::string(architecture));
            values.insert(values.end(), specific.begin(), specific.end());
            return values;
        };
        SourcePackage package;
        package.name = name;
        package.pkgver = scalar("pkgver");
        package.pkgrel = scalar("pkgrel");
        package.epoch = scalar("epoch");
        package.arch = inherited("arch");
        package.depends = combined("depends");
        package.provides = combined("provides");
        for (const auto& key : {std::string("makedepends"), "makedepends_" + std::string(architecture)}) {
            const auto found = base.find(key);
            if (found != base.end()) package.makedepends.insert(package.makedepends.end(), found->second.begin(), found->second.end());
        }
        if (package.pkgver.empty() || package.pkgrel.empty() || package.arch.empty() ||
            (std::ranges::find(package.arch, "any") == package.arch.end() &&
             std::ranges::find(package.arch, architecture) == package.arch.end())) return std::nullopt;
        result.packages.push_back(std::move(package));
    }
    return result;
}

}
