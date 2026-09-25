#include "internal.hpp"

#include <braurser/daemon/core/files.hpp>
#include <braurser/validate/validate.hpp>

#include <array>
#include <cmath>
#include <cerrno>
#include <climits>
#include <limits>
#include <stdexcept>
#include <utility>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::cache::internal {
namespace {

bool bounded(std::string_view value, std::size_t max, bool required = false) {
    return value.size() <= max && value.find('\0') == std::string_view::npos &&
           (!required || !value.empty());
}

bool bounded_optional(const std::optional<std::string>& value, std::size_t max) {
    return !value || bounded(*value, max);
}

bool bounded_list(const std::vector<std::string>& values, std::size_t max) {
    if (values.size() > 20000) return false;
    std::size_t total = 0;
    for (const auto& value : values) {
        if (!bounded(value, max) || total > 1048576 - value.size()) return false;
        total += value.size();
    }
    return true;
}

bool package_name(std::string_view name) { return br_valid_pkgname(name.data(), name.size()); }

std::string quote(std::string_view text) {
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (const char raw_byte : text) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += static_cast<char>(byte);
        } else if (byte < 0x20) {
            result += "\\u00";
            result += digits[byte >> 4];
            result += digits[byte & 15];
        } else {
            result += static_cast<char>(byte);
        }
    }
    result += '"';
    return result;
}

}

Directory::Directory(std::filesystem::path cache_home, bool create)
    : path(std::move(cache_home) / "braurser") {
    if (!path.is_absolute() || path.native().find('\0') != std::string::npos ||
        path.lexically_normal() != path) {
        throw std::runtime_error("cache directory path must be absolute and normalized");
    }
    if (create) {
        auto result = core::ensure_private_dir(path);
        if (!result) throw std::runtime_error("cache directory: " + result.error().message);
    }
    fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open private cache directory");
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0777) != 0700) {
        close(fd);
        fd = -1;
        throw std::runtime_error("cache directory owner or mode refused");
    }
}
Directory::~Directory() { if (fd >= 0) close(fd); }

std::uint64_t random_id() {
    std::uint64_t result = 0;
    auto* bytes = reinterpret_cast<unsigned char*>(&result);
    std::size_t offset = 0;
    while (offset < sizeof(result)) {
        const auto count = getrandom(bytes + offset, sizeof(result) - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("secure random source unavailable");
        offset += static_cast<std::size_t>(count);
    }
    return result & static_cast<std::uint64_t>(INT64_MAX);
}

std::string json_array(const std::vector<std::string>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += quote(value);
    }
    return result + ']';
}
std::string json_screenshots(const std::vector<catalog::Screenshot>& values) {
    std::string result = "[";
    for (const auto& value : values) {
        if (result.size() > 1) result += ',';
        result += "{\"url\":" + quote(value.url) + ",\"caption\":" + quote(value.caption) + '}';
    }
    return result + ']';
}
std::vector<std::string> read_array(const Database& db, std::string_view json) {
    auto statement = db.prepare("SELECT value FROM json_each(?1) WHERE type='text' ORDER BY key");
    statement.bind(1, json);
    std::vector<std::string> result;
    while (statement.step()) result.push_back(statement.text(0).value_or(""));
    return result;
}
std::vector<catalog::Screenshot> read_screenshots(const Database& db, std::string_view json) {
    auto statement = db.prepare("SELECT json_extract(value,'$.url'), json_extract(value,'$.caption') "
                                "FROM json_each(?1) ORDER BY key");
    statement.bind(1, json);
    std::vector<catalog::Screenshot> result;
    while (statement.step()) result.push_back({statement.text(0).value_or(""), statement.text(1).value_or("")});
    return result;
}
void bind_optional(Statement& stmt, int index, const std::optional<std::string>& value) {
    if (value) stmt.bind(index, *value);
    else stmt.bind_null(index);
}
void bind_optional(Statement& stmt, int index, const std::optional<std::int64_t>& value) {
    if (value) stmt.bind(index, *value);
    else stmt.bind_null(index);
}
std::int64_t checked_size(std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(INT64_MAX)) throw std::runtime_error("package size exceeds SQLite integer range");
    return static_cast<std::int64_t>(size);
}
catalog::CatalogError error(std::string code, std::string detail) {
    return {std::move(code), std::move(detail)};
}

bool valid(const catalog::RepoPackage& record) {
    return br_valid_repo_name(record.repo.data(), record.repo.size()) &&
           package_name(record.name) && package_name(record.base) &&
           bounded(record.version, 256, true) && bounded(record.description, 65536) &&
           bounded(record.url, 8192) && bounded(record.arch, 128) && bounded(record.packager, 512) &&
           bounded_list(record.licenses, 512) && bounded_list(record.groups, 255) &&
           bounded_list(record.depends, 8192) && bounded_list(record.optdepends, 8192) &&
           bounded_list(record.makedepends, 8192) && bounded_list(record.checkdepends, 8192) &&
           bounded_list(record.provides, 8192) && bounded_list(record.conflicts, 8192) &&
           bounded_list(record.replaces, 8192);
}

bool valid(const catalog::AurPackage& record) {
    return package_name(record.name) && package_name(record.package_base) &&
           bounded(record.version, 256, true) && bounded(record.description, 65536) &&
           bounded(record.url, 8192) && bounded_optional(record.maintainer, 512) &&
           bounded_optional(record.submitter, 512) && std::isfinite(record.popularity) &&
           bounded_list(record.licenses, 512) && bounded_list(record.keywords, 512) &&
           bounded_list(record.groups, 255) && bounded_list(record.depends, 8192) &&
           bounded_list(record.optdepends, 8192) && bounded_list(record.makedepends, 8192) &&
           bounded_list(record.checkdepends, 8192) && bounded_list(record.provides, 8192) &&
           bounded_list(record.conflicts, 8192) && bounded_list(record.replaces, 8192) &&
           bounded_list(record.comaintainers, 512);
}

bool valid(const catalog::AppMetadata& record) {
    if (!package_name(record.pkgname) || !bounded(record.appstream_id, 512, true) ||
        !bounded(record.name, 4096) || !bounded(record.summary, 8192) ||
        !bounded_list(record.description_paragraphs, 65536) ||
        !bounded_list(record.categories, 512) || !bounded_optional(record.icon_path, 8192) ||
        !bounded_optional(record.launchable, 8192) || !bounded_optional(record.homepage, 8192) ||
        !bounded_optional(record.license, 512) || !bounded_optional(record.developer, 4096) ||
        record.screenshots.size() > 256) return false;
    for (const auto& image : record.screenshots) {
        if (!bounded(image.url, 8192, true) || !bounded(image.caption, 8192)) return false;
    }
    return true;
}

bool valid(const catalog::InstalledPackage& record) {
    return package_name(record.name) && bounded(record.version, 256, true) &&
           (record.reason == domain::InstallReason::Explicit ||
            record.reason == domain::InstallReason::Dependency) &&
           bounded_optional(record.upgradable_to, 256) && bounded_list(record.groups, 255);
}

bool valid(const catalog::SourceState& record) {
    return bounded(record.id, 256, true) && bounded(record.kind, 64, true) &&
           bounded(record.fingerprint, 8192) && bounded_optional(record.etag, 8192) &&
           bounded_optional(record.last_modified, 512);
}

}
