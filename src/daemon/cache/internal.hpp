#ifndef BRAURSER_DAEMON_CACHE_INTERNAL_HPP
#define BRAURSER_DAEMON_CACHE_INTERNAL_HPP

#include <braurser/daemon/cache/database.hpp>
#include <braurser/daemon/catalog/records.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::daemon::cache::internal {

struct Directory {
    int fd{-1};
    std::filesystem::path path;
    Directory(std::filesystem::path cache_home, bool create);
    ~Directory();
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
};

std::uint64_t random_id();
std::string json_array(const std::vector<std::string>& values);
std::string json_screenshots(const std::vector<catalog::Screenshot>& values);
std::vector<std::string> read_array(const Database& db, std::string_view json);
std::vector<catalog::Screenshot> read_screenshots(const Database& db, std::string_view json);
void bind_optional(Statement& stmt, int index, const std::optional<std::string>& value);
void bind_optional(Statement& stmt, int index, const std::optional<std::int64_t>& value);
std::int64_t checked_size(std::uint64_t size);
catalog::CatalogError error(std::string code, std::string detail);
bool valid(const catalog::RepoPackage& record);
bool valid(const catalog::AurPackage& record);
bool valid(const catalog::AppMetadata& record);
bool valid(const catalog::InstalledPackage& record);
bool valid(const catalog::SourceState& record);

}

#endif
