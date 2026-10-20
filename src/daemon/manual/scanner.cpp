#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/text.hpp>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <set>

namespace braurser::daemon::manual {
namespace {

constexpr std::size_t kMaxOwnershipEntries = 2000000;

ManualError fail(std::string code) { return {std::move(code), {}, {}, {}}; }

std::string display_exec(std::string_view text) {
    std::string result;
    for (std::size_t pos = 0; pos < text.size() && result.size() < 1024; ++pos) {
        if (text[pos] == '%') {
            if (pos + 1 < text.size() && text[pos + 1] == '%') result += '%';
            if (pos + 1 < text.size()) ++pos;
        } else result += text[pos];
    }
    return core::bounded_line(core::strip_terminal_controls(result), 256);
}

bool launcher_path(const std::filesystem::path& path, const std::vector<std::filesystem::path>& roots) {
    if (path.extension() != ".desktop") return false;
    for (const auto& root : roots) {
        const auto prefix = (root / "applications").string() + "/";
        if (path.native().starts_with(prefix)) return true;
    }
    return false;
}

std::expected<std::set<std::filesystem::path>, ManualError> owned_files(
    const std::filesystem::path& db, const std::vector<std::filesystem::path>& roots, std::size_t& seen) {
    std::set<std::filesystem::path> owned;
    catalog::LocalDbReader reader;
    auto records = reader.read(db);
    if (!records) return std::unexpected(fail("launcher-local-db"));
    for (const auto& record : *records) {
        auto files = reader.files(db, record.package.name + "-" + record.package.version);
        if (!files || files->size() > 8 * 1024 * 1024)
            return std::unexpected(fail("launcher-local-files"));
        bool in_files = false;
        for (std::size_t start = 0; start < files->size();) {
            auto end = files->find('\n', start);
            if (end == files->npos) end = files->size();
            auto line = std::string_view(*files).substr(start, end - start);
            if (line == "%FILES%") in_files = true;
            else if (!line.empty() && line.front() == '%') in_files = false;
            else if (in_files && !line.empty()) {
                if (++seen > kMaxOwnershipEntries) return std::unexpected(fail("launcher-coverage-incomplete"));
                if (line.front() != '/' && line.find("..") == line.npos &&
                    line.find('\0') == line.npos && line.size() <= 4096) {
                    const auto path = std::filesystem::path("/") / line;
                    if (launcher_path(path, roots)) owned.insert(path);
                }
            }
            start = end + 1;
        }
    }
    return owned;
}

}

std::expected<std::vector<LauncherEntry>, ManualError> UnmanagedLauncherScanner::scan(
    const std::filesystem::path& data_home, const std::vector<std::filesystem::path>& data_dirs,
    const std::set<std::filesystem::path>& owned) const {
    if (!data_home.is_absolute()) return std::unexpected(fail("launcher-path"));
    std::vector<LauncherEntry> result;
    std::set<std::string> seen;
    std::vector<std::filesystem::path> roots{data_home};
    roots.insert(roots.end(), data_dirs.begin(), data_dirs.end());
    if (roots.size() > 128) return std::unexpected(fail("launcher-limit"));
    std::size_t entries = 0;
    for (const auto& root : roots) {
        if (!root.is_absolute() || root.lexically_normal() != root) return std::unexpected(fail("launcher-path"));
        const auto applications = root / "applications";
        std::error_code ec;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(applications, ec)))
            return std::unexpected(fail("launcher-directory"));
        ec.clear();
        if (!std::filesystem::is_directory(applications, ec)) continue;
        std::filesystem::recursive_directory_iterator iterator(applications,
            std::filesystem::directory_options::none, ec);
        if (ec) return std::unexpected(fail("launcher-open"));
        for (; iterator != std::filesystem::recursive_directory_iterator{}; iterator.increment(ec)) {
            if (ec) return std::unexpected(fail("launcher-read"));
            const auto& item = *iterator;
            if (++entries > 100000 || iterator.depth() > 16)
                return std::unexpected(fail("launcher-limit"));
            if (item.is_directory(ec)) continue;
            if (ec) return std::unexpected(fail("launcher-stat"));
            if (item.is_symlink(ec) || !item.is_regular_file(ec) || ec ||
                item.path().extension() != ".desktop" || owned.contains(item.path())) continue;
            auto relative = item.path().lexically_relative(applications).generic_string();
            if (relative.size() > 4096 || relative.find("..") != relative.npos) continue;
            std::replace(relative.begin(), relative.end(), '/', '-');
            if (seen.contains(relative)) continue;
            auto text = core::read_bounded(item.path(), 65536);
            if (!text) continue;
            auto parsed = catalog::parse_desktop_entry(*text, "C.UTF-8");
            if (!parsed || parsed->type != "Application" || parsed->hidden || parsed->no_display) continue;
            seen.insert(relative);
            result.push_back({relative, parsed->name, display_exec(parsed->exec), item.path(), "outside"});
        }
        if (ec) return std::unexpected(fail("launcher-read"));
    }
    return result;
}

std::expected<std::vector<LauncherEntry>, ManualError> UnmanagedLauncherScanner::scan_system(
    core::ProcessRunner& runner, const std::filesystem::path& state) const {
    const auto* home = std::getenv("HOME");
    if (!home || !std::filesystem::path(home).is_absolute() || !state.is_absolute())
        return std::unexpected(fail("launcher-home"));
    const auto* xdg_home = std::getenv("XDG_DATA_HOME");
    std::filesystem::path data_home = xdg_home && *xdg_home ? xdg_home : std::filesystem::path(home) / ".local/share";
    const auto* data_dirs = std::getenv("XDG_DATA_DIRS");
    std::string_view paths = data_dirs && *data_dirs ? data_dirs : "/usr/local/share:/usr/share";
    std::vector<std::filesystem::path> dirs;
    for (std::size_t start = 0; start < paths.size();) {
        auto end = paths.find(':', start);
        if (end == paths.npos) end = paths.size();
        dirs.emplace_back(paths.substr(start, end - start));
        start = end + 1;
    }
    std::vector<std::filesystem::path> roots{data_home};
    roots.insert(roots.end(), dirs.begin(), dirs.end());
    if (roots.size() > 128) return std::unexpected(fail("launcher-limit"));
    for (const auto& root : roots)
        if (!root.is_absolute() || root.lexically_normal() != root) return std::unexpected(fail("launcher-path"));
    std::size_t seen = 0;
    catalog::PacmanConfig config(runner);
    auto repositories = config.read();
    if (!repositories) return std::unexpected(fail("launcher-pacman"));
    auto owned = owned_files(repositories->db_path, roots, seen);
    if (!owned) return std::unexpected(owned.error());
    auto receipt_dir = state / "braurser/manual/receipts";
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(receipt_dir, ec)))
        return std::unexpected(fail("launcher-receipts"));
    ec.clear();
    if (std::filesystem::is_directory(receipt_dir, ec)) {
        std::size_t count = 0;
        for (const auto& item : std::filesystem::directory_iterator(receipt_dir)) {
            if (++count > 50000) return std::unexpected(fail("launcher-receipts"));
            if (item.path().extension() != ".json" || item.is_symlink()) continue;
            auto data = core::read_bounded(item.path(), 8 * 1024 * 1024);
            if (!data || !nlohmann::json::accept(*data)) return std::unexpected(fail("launcher-receipt"));
            auto record = nlohmann::json::parse(*data);
            if (!record.is_object() || !record.contains("files") || !record["files"].is_array() ||
                !record.contains("scope") || !record["scope"].is_string() ||
                (record["scope"] != "user" && record["scope"] != "system"))
                return std::unexpected(fail("launcher-receipt"));
            auto prefix = record["scope"] == "system" ? std::filesystem::path("/usr/local") :
                std::filesystem::path(home) / ".local";
            for (const auto& file : record["files"]) {
                if (++seen > kMaxOwnershipEntries) return std::unexpected(fail("launcher-coverage-incomplete"));
                if (!file.is_object() || !file.contains("path") || !file["path"].is_string())
                    return std::unexpected(fail("launcher-receipt"));
                auto relative = std::filesystem::path(file["path"].get<std::string>());
                if (relative.is_absolute() || relative.empty() || relative.string().find("..") != std::string::npos)
                    return std::unexpected(fail("launcher-receipt"));
                const auto path = prefix / relative;
                if (launcher_path(path, roots)) owned->insert(path);
            }
        }
    }
    return scan(data_home, dirs, *owned);
}

}
