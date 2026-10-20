#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/domain/vercmp.hpp>
#include <braurser/validate/validate.hpp>

#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <system_error>

namespace braurser::daemon::catalog {
namespace {

CatalogError error(std::string code) { return {std::move(code), {}}; }

using Fields = std::vector<std::pair<std::string_view, std::string_view>>;

std::expected<std::string, CatalogError> pacman_conf(core::ProcessRunner& runner, std::vector<std::string> args) {
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/pacman-conf";
    spec.args = std::move(args);
    spec.env = {{"LC_ALL", "C"}};
    spec.timeout = std::chrono::seconds(10);
    spec.max_stdout = spec.max_stderr = 64 * 1024;
    auto result = runner.run(spec);
    if (!result || result->exit_code != 0 || result->signaled || result->timed_out ||
        result->out_truncated || result->err_truncated) return std::unexpected(error("pacman-conf"));
    return result->out;
}

std::expected<void, CatalogError> parse_fields(std::string_view desc, Fields& fields) {
    if (desc.size() > 256 * 1024 || desc.find('\0') != desc.npos) return std::unexpected(error("desc-size"));
    fields.clear();
    std::string_view key;
    std::unordered_map<std::string_view, std::size_t> counts;
    for (std::size_t start = 0; start < desc.size();) {
        auto end = desc.find('\n', start);
        if (end == desc.npos) end = desc.size();
        auto line = desc.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        start = end == desc.size() ? desc.size() : end + 1;
        if (line.empty()) { key = {}; continue; }
        if (line.size() >= 3 && line.front() == '%' && line.back() == '%') {
            key = line.substr(1, line.size() - 2);
            if (key.find('%') != std::string::npos) key = {};
        } else if (!key.empty()) {
            if (++counts[key] > 20000) return std::unexpected(error("desc-values"));
            fields.emplace_back(key, line);
        }
    }
    return {};
}

std::string_view first(const Fields& fields, std::string_view key) {
    for (const auto& [name, value] : fields) if (name == key) return value;
    return {};
}

std::vector<std::string> many(const Fields& fields, std::string_view key) {
    std::vector<std::string> result;
    for (const auto& [name, value] : fields) if (name == key) result.emplace_back(value);
    return result;
}

template<class T>
T number(const Fields& fields, const char* key) {
    auto text = first(fields, key);
    T result{};
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    return ec == std::errc{} && end == text.data() + text.size() ? result : T{};
}

bool valid_name(std::string_view name) {
    return br_valid_pkgname(name.data(), name.size());
}

std::expected<LocalRecord, CatalogError> parse_local(std::string_view desc, Fields& fields) {
    auto parsed = parse_fields(desc, fields);
    if (!parsed) return std::unexpected(parsed.error());
    LocalRecord result;
    result.package.name = first(fields, "NAME");
    result.package.version = first(fields, "VERSION");
    if (!valid_name(result.package.name) || result.package.version.empty()) return std::unexpected(error("local-record"));
    const auto reason = first(fields, "REASON");
    if (reason == "1") result.package.reason = domain::InstallReason::Dependency;
    else if (!reason.empty() && reason != "0") return std::unexpected(error("local-reason"));
    result.package.install_date = number<std::int64_t>(fields, "INSTALLDATE");
    result.package.size = number<std::uint64_t>(fields, "SIZE");
    result.package.groups = many(fields, "GROUPS");
    result.depends = many(fields, "DEPENDS");
    result.optdepends = many(fields, "OPTDEPENDS");
    result.provides = many(fields, "PROVIDES");
    result.validation = many(fields, "VALIDATION");
    return result;
}

std::string_view dependency_name(std::string_view text) {
    auto end = text.find_first_of("<>=:");
    return text.substr(0, end);
}

bool satisfies(std::string_view dependency, std::string_view offer, std::string_view version) {
    const auto name = dependency_name(dependency);
    if (name != dependency_name(offer)) return false;
    const auto op = dependency.find_first_of("<>=");
    if (op == dependency.npos) return true;
    auto constraint = dependency.substr(op);
    const auto stop = constraint.find(": ");
    if (stop != constraint.npos) constraint = constraint.substr(0, stop);
    const auto value = constraint.find_first_not_of("<>=");
    if (value == constraint.npos) return false;
    const auto signs = constraint.substr(0, value);
    const auto target = constraint.substr(value);
    if (target.empty()) return false;
    if (offer != name) {
        const auto equals = offer.find('=');
        if (equals == offer.npos || equals + 1 == offer.size()) return false;
        version = offer.substr(equals + 1);
    }
    const int cmp = domain::vercmp(version, target);
    return (signs == "=" && cmp == 0) || (signs == ">" && cmp > 0) ||
           (signs == "<" && cmp < 0) || (signs == ">=" && cmp >= 0) ||
           (signs == "<=" && cmp <= 0);
}

}

std::expected<Repositories, CatalogError> PacmanConfig::read() const {
    auto names = pacman_conf(runner_, {"--repo-list"});
    if (!names) return std::unexpected(names.error());
    auto db = pacman_conf(runner_, {"DBPath"});
    if (!db) return std::unexpected(db.error());
    Repositories result;
    std::set<std::string> seen;
    for (std::size_t start = 0; start < names->size();) {
        auto end = names->find('\n', start);
        if (end == names->npos) end = names->size();
        auto name = names->substr(start, end - start);
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (!br_valid_repo_name(name.data(), name.size()) || !seen.insert(name).second)
            return std::unexpected(error("repo-name"));
        result.names.push_back(std::move(name));
        start = end == names->size() ? names->size() : end + 1;
    }
    if (db->ends_with('\n')) db->pop_back();
    if (db->ends_with('\r')) db->pop_back();
    result.db_path = *db;
    if (db->find('\0') != db->npos || !result.db_path.is_absolute() ||
        result.db_path.lexically_normal() != result.db_path || db->find('\n') != db->npos)
        return std::unexpected(error("db-path"));
    if (!result.db_path.has_filename() && result.db_path.has_parent_path() && result.db_path != result.db_path.root_path())
        result.db_path = result.db_path.parent_path();
    return result;
}

std::expected<std::vector<std::string>, CatalogError> PacmanConfig::hold_packages() const {
    auto output = pacman_conf(runner_, {"HoldPkg"});
    if (!output) return std::unexpected(output.error());
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (std::size_t start = 0; start < output->size();) {
        auto end = output->find('\n', start);
        if (end == output->npos) end = output->size();
        auto name = output->substr(start, end - start);
        start = end == output->size() ? output->size() : end + 1;
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (!br_valid_pkgname(name.data(), name.size()) || result.size() >= 256)
            return std::unexpected(error("hold-name"));
        if (seen.insert(name).second) result.push_back(std::move(name));
    }
    return result;
}

namespace {

std::expected<RepoPackage, CatalogError> parse_repo_fields(std::string_view desc, std::string_view repo, Fields& fields) {
    if (!br_valid_repo_name(repo.data(), repo.size())) return std::unexpected(error("repo-name"));
    auto parsed = parse_fields(desc, fields);
    if (!parsed) return std::unexpected(parsed.error());
    RepoPackage result;
    result.repo = repo;
    result.name = first(fields, "NAME");
    result.version = first(fields, "VERSION");
    if (!valid_name(result.name) || result.version.empty()) return std::unexpected(error("repo-record"));
    result.base = first(fields, "BASE");
    result.description = first(fields, "DESC");
    result.url = first(fields, "URL");
    result.arch = first(fields, "ARCH");
    result.packager = first(fields, "PACKAGER");
    result.download_size = number<std::uint64_t>(fields, "CSIZE");
    result.install_size = number<std::uint64_t>(fields, "ISIZE");
    result.build_date = number<std::int64_t>(fields, "BUILDDATE");
    result.licenses = many(fields, "LICENSE");
    result.groups = many(fields, "GROUPS");
    result.depends = many(fields, "DEPENDS");
    result.optdepends = many(fields, "OPTDEPENDS");
    result.makedepends = many(fields, "MAKEDEPENDS");
    result.checkdepends = many(fields, "CHECKDEPENDS");
    result.provides = many(fields, "PROVIDES");
    result.conflicts = many(fields, "CONFLICTS");
    result.replaces = many(fields, "REPLACES");
    return result;
}

}

std::expected<RepoPackage, CatalogError> parse_repo_desc(std::string_view desc, std::string_view repo) {
    Fields fields;
    return parse_repo_fields(desc, repo, fields);
}

std::expected<RecordCounts, CatalogError> SyncDbReader::read_bytes(std::span<const std::byte> bytes,
                                                                   std::string_view repo, const RepoSink& sink) const {
    if (!br_valid_repo_name(repo.data(), repo.size()) || bytes.empty() || bytes.size() > 768ULL * 1024 * 1024)
        return std::unexpected(error("sync-input"));
    using Archive = std::unique_ptr<archive, decltype(&archive_read_free)>;
    Archive handle(archive_read_new(), archive_read_free);
    if (!handle) return std::unexpected(error("archive-init"));
    archive_read_support_format_tar(handle.get());
    archive_read_support_filter_gzip(handle.get());
    archive_read_support_filter_zstd(handle.get());
    archive_read_support_filter_xz(handle.get());
    if (archive_read_open_memory(handle.get(), bytes.data(), bytes.size()) != ARCHIVE_OK)
        return std::unexpected(error("archive-open"));
    RecordCounts result;
    Fields fields;
    archive_entry* entry = nullptr;
    std::uint64_t expanded = 0;
    std::size_t entries = 0;
    while (true) {
        const int status = archive_read_next_header(handle.get(), &entry);
        if (status == ARCHIVE_EOF) break;
        if (status != ARCHIVE_OK || ++entries > 250000) return std::unexpected(error("archive-entries"));
        const char* raw = archive_entry_pathname(entry);
        std::string_view path = raw ? std::string_view(raw) : std::string_view{};
        const bool desc = path.ends_with("/desc") && path.find('/') == path.size() - 5 &&
                          br_valid_path_component(path.data(), path.size() - 5) &&
                          archive_entry_filetype(entry) == AE_IFREG;
        const auto size = archive_entry_size(entry);
        if (size < 0 || static_cast<std::uint64_t>(size) > 768ULL * 1024 * 1024 - expanded ||
            (desc && size > 256 * 1024)) return std::unexpected(error("archive-size"));
        std::string content;
        std::array<char, 8192> buffer{};
        while (true) {
            const auto got = archive_read_data(handle.get(), buffer.data(), buffer.size());
            if (got < 0) return std::unexpected(error("archive-data"));
            if (got == 0) break;
            if (static_cast<std::uint64_t>(got) > 768ULL * 1024 * 1024 - expanded)
                return std::unexpected(error("archive-size"));
            expanded += static_cast<std::uint64_t>(got);
            if (desc) {
                if (static_cast<std::size_t>(got) > 256 * 1024 - content.size())
                    return std::unexpected(error("desc-size"));
                content.append(buffer.data(), static_cast<std::size_t>(got));
            }
        }
        if (desc) {
            auto parsed = parse_repo_fields(content, repo, fields);
            if (parsed) { sink(std::move(*parsed)); ++result.packages; }
            else ++result.rejected;
        }
    }
    return result;
}

std::expected<RecordCounts, CatalogError> SyncDbReader::read(const std::filesystem::path& db_path,
                                                             std::string_view repo, const RepoSink& sink) const {
    if (!db_path.is_absolute() || !br_valid_repo_name(repo.data(), repo.size()))
        return std::unexpected(error("sync-path"));
    auto bytes = core::read_bounded(db_path / "sync" / (std::string(repo) + ".db"), 768ULL * 1024 * 1024);
    if (!bytes) return std::unexpected(error("sync-read"));
    return read_bytes(std::as_bytes(std::span(*bytes)), repo, sink);
}

std::expected<SyncResult, CatalogError> SyncDbReader::read_bytes(std::span<const std::byte> bytes, std::string_view repo) const {
    SyncResult result;
    auto counts = read_bytes(bytes, repo, [&](RepoPackage&& package) { result.packages.push_back(std::move(package)); });
    if (!counts) return std::unexpected(counts.error());
    result.rejected = counts->rejected;
    return result;
}

std::expected<std::vector<LocalRecord>, CatalogError> LocalDbReader::read(const std::filesystem::path& db_path) const {
    if (!db_path.is_absolute()) return std::unexpected(error("local-path"));
    const auto root = db_path / "local";
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root, ec)) || ec)
        return std::unexpected(error("local-root"));
    std::filesystem::directory_iterator it(root, ec);
    if (ec) return std::unexpected(error("local-open"));
    std::vector<LocalRecord> result;
    Fields fields;
    std::size_t entries = 0;
    std::uint64_t expanded = 0;
    for (const auto& item : it) {
        if (++entries > 250000) return std::unexpected(error("local-entries"));
        const auto name = item.path().filename().string();
        if (!br_valid_path_component(name.data(), name.size()) ||
            !item.is_directory(ec) || item.is_symlink(ec) || ec) { ec.clear(); continue; }
        auto desc = core::read_bounded(item.path() / "desc", 256 * 1024);
        if (!desc) continue;
        if (desc->size() > 768ULL * 1024 * 1024 - expanded) return std::unexpected(error("local-size"));
        expanded += desc->size();
        auto record = parse_local(*desc, fields);
        if (record && name == record->package.name + "-" + record->package.version)
            result.push_back(std::move(*record));
    }
    return result;
}

std::expected<std::string, CatalogError> LocalDbReader::files(const std::filesystem::path& db_path,
                                                                 std::string_view package_dir) const {
    if (!db_path.is_absolute() || !br_valid_path_component(package_dir.data(), package_dir.size()))
        return std::unexpected(error("local-path"));
    auto bytes = core::read_bounded(db_path / "local" / std::string(package_dir) / "files", 8 * 1024 * 1024);
    if (!bytes) return std::unexpected(error("local-files"));
    return std::move(*bytes);
}

void OfficialIndex::add(std::string_view name, std::string_view version_text) {
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    if (name.size() > std::numeric_limits<std::uint16_t>::max() ||
        version_text.size() > std::numeric_limits<std::uint16_t>::max() ||
        arena_.size() > limit || name.size() + version_text.size() + 2 > limit - arena_.size())
        throw std::length_error("official index exceeds bounds");
    entries_.push_back({static_cast<std::uint32_t>(arena_.size()), static_cast<std::uint16_t>(name.size()),
                        static_cast<std::uint16_t>(version_text.size())});
    arena_.append(name); arena_ += '\0';
    arena_.append(version_text); arena_ += '\0';
}

void OfficialIndex::finish() {
    auto name = [this](const Entry& entry) { return std::string_view(arena_).substr(entry.offset, entry.name_length); };
    std::ranges::sort(entries_, [&](const Entry& left, const Entry& right) {
        const auto comparison = name(left).compare(name(right));
        return comparison < 0 || (comparison == 0 && left.offset < right.offset);
    });
    const auto duplicates = std::ranges::unique(entries_, [&](const Entry& left, const Entry& right) {
        return name(left) == name(right);
    });
    entries_.erase(duplicates.begin(), duplicates.end());
}

std::optional<std::string_view> OfficialIndex::version(std::string_view name) const {
    const auto found = std::ranges::lower_bound(entries_, name, {}, [this](const Entry& entry) {
        return std::string_view(arena_).substr(entry.offset, entry.name_length);
    });
    if (found == entries_.end() || std::string_view(arena_).substr(found->offset, found->name_length) != name) return std::nullopt;
    return std::string_view(arena_).substr(static_cast<std::size_t>(found->offset) + found->name_length + 1, found->version_length);
}

QuerySets LocalQuery::compute(std::span<const LocalRecord> local, const OfficialIndex& official) const {
    QuerySets result;
    struct Offer { const LocalRecord* record; std::string_view text; };
    std::unordered_map<std::string_view, std::vector<Offer>> offers;
    for (const auto& record : local) {
        offers[record.package.name].push_back({&record, record.package.name});
        for (const auto& offer : record.provides) offers[dependency_name(offer)].push_back({&record, offer});
    }
    std::unordered_set<std::string_view> required;
    for (const auto& consumer : local) {
        for (const auto* list : {&consumer.depends, &consumer.optdepends}) {
            for (const auto& dep : *list) {
                const auto found = offers.find(dependency_name(dep));
                if (found == offers.end()) continue;
                for (const auto& offer : found->second)
                    if (satisfies(dep, offer.text, offer.record->package.version)) required.insert(offer.record->package.name);
            }
        }
    }
    for (const auto& record : local) {
        auto package = record.package;
        auto found = official.version(package.name);
        package.native = found.has_value();
        package.orphan = package.reason == domain::InstallReason::Dependency &&
                         !required.contains(package.name);
        if (package.native && domain::vercmp(package.version, *found) < 0)
            package.upgradable_to = *found;
        result.installed.push_back(package.name);
        if (package.reason == domain::InstallReason::Explicit) result.explicit_packages.push_back(package.name);
        else result.dependencies.push_back(package.name);
        if (package.orphan) result.orphans.push_back(package.name);
        if (package.native) result.native.push_back(package.name);
        else result.foreign.push_back(package.name);
        if (package.upgradable_to) result.upgradable.push_back(package.name);
        result.packages.push_back(std::move(package));
    }
    for (auto* group : {&result.installed, &result.explicit_packages, &result.dependencies,
                        &result.orphans, &result.foreign, &result.native, &result.upgradable})
        std::ranges::sort(*group);
    return result;
}

}
