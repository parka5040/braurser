#include <braurser/daemon/updates/updates.hpp>
#include <braurser/domain/vercmp.hpp>
#include <braurser/validate/validate.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <iterator>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::updates {
namespace {
UpdateError invalid(std::string detail) { return {UpdateErrorCode::InvalidOutput, std::move(detail)}; }
bool valid_version(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
        std::ranges::all_of(value, [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == ':' || c == '-' || c == '_' || c == '+' || c == '~';
        });
}
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        const auto start = line.find_first_not_of(" \t");
        if (start == line.npos) break;
        line.remove_prefix(start);
        const auto end = line.find_first_of(" \t");
        result.push_back(line.substr(0, end));
        if (end == line.npos) break;
        line.remove_prefix(end);
    }
    return result;
}
bool clean_path(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.native().size() > 4096 || path == "/") return false;
    for (const auto& part : path.relative_path()) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return std::ranges::all_of(path.native(), [](unsigned char c) { return c >= 0x20 && c != 0x7f; });
}

using Digest = std::pair<std::uint64_t, std::array<std::uint8_t, 32>>;
std::expected<Digest, UpdateError> digest(int dir, const std::string& name) {
    const int fd = openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "database open failed"});
    struct Closer { int fd; ~Closer() { close(fd); } } closer{fd};
    struct stat info {};
    constexpr std::uint64_t limit = 512ULL * 1024 * 1024;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > limit)
        return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "invalid database file"});
    br_sha256_ctx ctx{};
    br_sha256_init(&ctx);
    std::array<std::uint8_t, 16384> buffer{};
    std::uint64_t count = 0;
    while (true) {
        const auto n = read(fd, buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "database read failed"});
        if (n == 0) break;
        if (static_cast<std::uint64_t>(n) > limit - count)
            return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "database too large"});
        count += static_cast<std::uint64_t>(n);
        br_sha256_update(&ctx, buffer.data(), static_cast<std::size_t>(n));
    }
    if (count != static_cast<std::uint64_t>(info.st_size))
        return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "database changed during read"});
    Digest result{};
    result.first = count;
    br_sha256_final(&ctx, result.second.data());
    return result;
}
std::expected<std::map<std::string, Digest>, UpdateError> database_files(const std::filesystem::path& root) {
    if (!clean_path(root)) return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid database path"});
    const int fd = open((root / "sync").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "sync directory unavailable"});
    struct Closer { int fd; ~Closer() { close(fd); } } closer{fd};
    std::error_code ec;
    std::filesystem::directory_iterator it(root / "sync", ec);
    if (ec) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "sync listing unavailable"});
    std::map<std::string, Digest> result;
    std::size_t entries = 0;
    for (const auto end = std::filesystem::directory_iterator{}; it != end; it.increment(ec)) {
        if (ec) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "sync listing failed"});
        if (++entries > 256) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "too many database files"});
        const auto name = it->path().filename().string();
        if (!name.ends_with(".db")) continue;
        const auto repo = std::string_view(name).substr(0, name.size() - 3);
        if (!br_valid_repo_name(repo.data(), repo.size()))
            return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "invalid repository database name"});
        auto file = digest(fd, name);
        if (!file) return std::unexpected(file.error());
        result.emplace(name, *file);
    }
    if (ec) return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "sync listing failed"});
    return result;
}
}

std::expected<std::vector<PackageUpdate>, UpdateError> OfficialUpdateChecker::check(const std::filesystem::path& temp_db) const {
    if (!clean_path(temp_db)) return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid database path"});
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/pacman";
    spec.args = {"-Qu", "--dbpath", temp_db.string()};
    spec.env = {{"PATH", "/usr/bin"}, {"LC_ALL", "C"}};
    spec.timeout = std::chrono::seconds(60);
    spec.max_stdout = spec.max_stderr = 1024 * 1024;
    auto run = runner_.run(spec);
    if (!run || run->timed_out || run->signaled || run->out_truncated || run->err_truncated ||
        run->out.size() > spec.max_stdout || run->err.size() > spec.max_stderr ||
        (run->exit_code != 0 && !(run->exit_code == 1 && run->out.empty() && run->err.empty())))
        return std::unexpected(UpdateError{UpdateErrorCode::ProcessFailure, "official update query failed"});
    std::vector<PackageUpdate> result;
    std::set<std::string> seen;
    for (std::string_view remaining = run->out; !remaining.empty();) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        if (line.empty() || line.size() > 1024 || line.find('\0') != line.npos || line.find('\r') != line.npos)
            return std::unexpected(invalid("invalid official update line"));
        const auto fields = words(line);
        const bool ignored = fields.size() == 5 && fields[4] == "[ignored]";
        if ((fields.size() != 4 && !ignored) || fields[2] != "->" ||
            !br_valid_pkgname(fields[0].data(), fields[0].size()) ||
            !valid_version(fields[1]) || !valid_version(fields[3]) ||
            domain::vercmp(fields[1], fields[3]) >= 0 ||
            !seen.emplace(fields[0]).second)
            return std::unexpected(invalid("invalid official update record"));
        result.push_back({std::string(fields[0]), std::string(fields[1]), std::string(fields[3]), ignored});
        if (end == remaining.npos) break;
        remaining.remove_prefix(end + 1);
    }
    std::ranges::sort(result, {}, &PackageUpdate::name);
    return result;
}

std::expected<std::vector<PackageUpdate>, UpdateError> CommunityUpdateChecker::check(
    const std::set<std::string>& foreign_names) const {
    for (const auto& name : foreign_names)
        if (!br_valid_pkgname(name.data(), name.size()))
            return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid foreign package"});
    auto spec = helper_.build_updates_spec();
    if (!clean_path(spec.executable) || spec.timeout > std::chrono::minutes(5) ||
        spec.max_stdout > 1024 * 1024 || spec.max_stderr > 1024 * 1024)
        return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid helper update specification"});
    auto run = runner_.run(spec);
    if (!run || run->out_truncated || run->err_truncated || run->out.size() > spec.max_stdout ||
        run->err.size() > spec.max_stderr)
        return std::unexpected(UpdateError{UpdateErrorCode::ProcessFailure, "helper update query failed"});
    auto parsed = helper_.parse_updates(*run);
    if (!parsed) return std::unexpected(UpdateError{UpdateErrorCode::InvalidOutput, "invalid helper update output"});
    std::vector<PackageUpdate> result;
    std::set<std::string> seen;
    for (const auto& update : *parsed) {
        const auto name = std::string(update.name.value());
        if (!valid_version(update.from) || !valid_version(update.to) || !seen.insert(name).second)
            return std::unexpected(invalid("invalid community update record"));
        const bool vcs_marker = update.to == "latest-commit";
        if (!vcs_marker && domain::vercmp(update.from, update.to) >= 0) continue;
        if (foreign_names.contains(name)) result.push_back({name, update.from, update.to, false});
    }
    std::ranges::sort(result, {}, &PackageUpdate::name);
    return result;
}

std::expected<bool, UpdateError> is_real_db_stale(const std::filesystem::path& temp_db,
    const std::filesystem::path& real_db, bool official_updates_found) {
    if (!clean_path(temp_db) || !clean_path(real_db))
        return std::unexpected(UpdateError{UpdateErrorCode::InvalidInput, "invalid database path"});
    if (official_updates_found) return true;
    auto temporary = database_files(temp_db);
    if (!temporary) return std::unexpected(temporary.error());
    auto real = database_files(real_db);
    if (!real) return std::unexpected(real.error());
    if (temporary->empty() || real->empty())
        return std::unexpected(UpdateError{UpdateErrorCode::FileFailure, "no repository databases"});
    return *temporary != *real;
}

SelectionDecision UpdateSelectionPolicy::decide(const UpdateReport& report, const UpdateSelection& selection,
    const std::set<std::string>& repo_dependencies) const {
    if (selection.kind == SelectionKind::System) {
        if (!selection.id.empty()) return {false, {}, {{"invalid-selection", "system selection takes no name"}}, {}};
        return {true, {{"pacman", {"--sync", "--refresh", "--sysupgrade"}}}, {}, {}};
    }
    if (selection.kind == SelectionKind::Community) {
        if (!br_valid_pkgname(selection.id.data(), selection.id.size()) ||
            std::ranges::none_of(report.community, [&](const auto& u) { return u.name == selection.id; }))
            return {false, {}, {{"not-found", selection.id}}, {}};
        std::set<std::string> pending;
        for (const auto& update : report.official) pending.insert(update.name);
        std::vector<std::string> intersection;
        std::set_intersection(repo_dependencies.begin(), repo_dependencies.end(), pending.begin(), pending.end(),
            std::back_inserter(intersection));
        if (!intersection.empty())
            return {false, {}, {{"system-update-required", intersection.front()}}, {}};
        return {true, {{"helper", {"install", "aur/" + selection.id}}}, {}, {}};
    }
    if (selection.kind == SelectionKind::Manual) {
        if (selection.id.empty() || selection.id.size() > 255 ||
            !std::ranges::all_of(selection.id, [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            }) || std::ranges::none_of(report.manual, [&](const auto& u) { return u.receipt_id == selection.id; }))
            return {false, {}, {{"not-found", selection.id}}, {}};
        return {true, {{"manual", {"upgrade", selection.id}}}, {}, {}};
    }
    return {false, {}, {{"invalid-selection", {}}}, {}};
}

SelectionDecision UpdateSelectionPolicy::install_official(std::string_view name, bool real_db_fresh) const {
    if (!br_valid_pkgname(name.data(), name.size()))
        return {false, {}, {{"invalid-package", {}}}, {}};
    std::vector<std::string> args{"--sync"};
    if (!real_db_fresh) args.insert(args.end(), {"--refresh", "--sysupgrade"});
    args.insert(args.end(), {"--needed", "--", std::string(name)});
    return {true, {{"pacman", std::move(args)}}, {}, real_db_fresh ? "" : "update-and-install"};
}

}
