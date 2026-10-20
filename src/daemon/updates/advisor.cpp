#include <braurser/daemon/updates/updates.hpp>
#include <braurser/validate/validate.hpp>

#include <algorithm>

namespace braurser::daemon::updates {
namespace {
std::optional<std::vector<std::string>> installed_paths(std::string_view contents) {
    if (contents.size() > 8 * 1024 * 1024 || !contents.starts_with("%FILES%\n")) return {};
    std::vector<std::string> paths;
    for (std::size_t start = 8; start < contents.size();) {
        const auto end = contents.find('\n', start);
        if (end == contents.npos) return {};
        const auto path = contents.substr(start, end - start);
        start = end + 1;
        if (path.empty()) {
            if (start < contents.size() && !contents.substr(start).starts_with("%BACKUP%\n")) return {};
            break;
        }
        if (path.size() > 4095 || path.front() == '/' || path.find('\0') != path.npos ||
            !std::ranges::all_of(path, [](unsigned char c) { return c >= 0x20 && c != 0x7f; })) return {};
        for (std::size_t part_start = 0; part_start < path.size();) {
            const auto part_end = path.find('/', part_start);
            const auto part = path.substr(part_start, part_end == path.npos ? part_end : part_end - part_start);
            if (part.empty() || part == "." || part == "..") return {};
            if (part_end == path.npos) break;
            part_start = part_end + 1;
            if (part_start == path.size()) break;
        }
        if (path.ends_with('/')) continue;
        paths.push_back("/" + std::string(path));
        if (paths.size() > 100000) return {};
    }
    return paths;
}
}
std::expected<std::string, UpdateError> LocalDbFilesSource::files(
    std::string_view name, std::string_view version) const {
    if (!br_valid_pkgname(name.data(), name.size()) || version.empty() || version.size() > 128 ||
        name.size() + 1 + version.size() > 255 ||
        !std::ranges::all_of(version, [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == ':' || c == '-' || c == '_' || c == '+' || c == '~';
        })) return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid installed package"});
    auto result = reader_.files(db_path_, std::string(name) + "-" + std::string(version));
    if (!result) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "installed file list unavailable"});
    return *result;
}
RestartReport RestartAdvisor::advise(const std::vector<PackageUpdate>& updates,
    const std::vector<domain::RestartRule>& rules) const {
    RestartReport report;
    auto consumers = scanner_.scan();
    report.coverage_complete = consumers.coverage_complete;
    for (const auto& update : updates) {
        auto name = domain::PackageName::make(update.name);
        if (!name) { report.coverage_complete = false; continue; }
        auto contents = files_.files(update.name, update.from);
        auto paths = contents ? installed_paths(*contents) : std::nullopt;
        if (!paths) {
            report.coverage_complete = false;
            report.packages.push_back({*name, domain::RestartLevel::Unknown, {}, {}});
            continue;
        }
        domain::PackageChange change{*name, std::move(*paths), false, false};
        for (const auto& path : change.files) {
            const auto it = consumers.files.find(path);
            if (it == consumers.files.end()) continue;
            change.has_running_consumer = true;
            change.consumer_is_service |= it->second.service;
        }
        report.packages.push_back(domain::classify(change, rules));
    }
    std::ranges::sort(report.packages, [](const auto& a, const auto& b) {
        return a.name.value() < b.name.value();
    });
    report.overall = domain::overall(report.packages);
    return report;
}
}
