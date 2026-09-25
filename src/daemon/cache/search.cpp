#include <braurser/daemon/cache/store.hpp>

#include "internal.hpp"

#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::cache {
namespace {

struct TextQuery {
    std::string match;
    std::string exact;
};

bool whitespace(std::uint32_t codepoint) {
    return (codepoint >= 9 && codepoint <= 13) || codepoint == 32 || codepoint == 0x85 ||
           codepoint == 0xa0 || codepoint == 0x1680 ||
           (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
           codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f || codepoint == 0x3000;
}

TextQuery parse_text(std::string_view text) {
    if (text.size() > 256) throw std::invalid_argument("search text exceeds 256 bytes");
    std::vector<std::string> terms;
    std::string term;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto byte = static_cast<unsigned char>(text[pos]);
        if (!byte) throw std::invalid_argument("search text contains NUL");
        std::uint32_t codepoint = 0;
        std::size_t width = 0;
        if (byte < 0x80) { codepoint = byte; width = 1; }
        else if (byte >= 0xc2 && byte <= 0xdf) { codepoint = byte & 0x1f; width = 2; }
        else if (byte >= 0xe0 && byte <= 0xef) { codepoint = byte & 0x0f; width = 3; }
        else if (byte >= 0xf0 && byte <= 0xf4) { codepoint = byte & 0x07; width = 4; }
        else throw std::invalid_argument("search text is not valid UTF-8");
        if (pos + width > text.size()) throw std::invalid_argument("search text is not valid UTF-8");
        for (std::size_t index = 1; index < width; ++index) {
            const auto continuation = static_cast<unsigned char>(text[pos + index]);
            if ((continuation & 0xc0) != 0x80) throw std::invalid_argument("search text is not valid UTF-8");
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if ((width == 3 && codepoint < 0x800) || (width == 4 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
            throw std::invalid_argument("search text is not valid UTF-8");
        }
        if (whitespace(codepoint)) {
            if (!term.empty()) {
                terms.push_back(std::move(term));
                term.clear();
                if (terms.size() > 16) throw std::invalid_argument("search text exceeds 16 terms");
            }
        } else {
            term.append(text.substr(pos, width));
        }
        pos += width;
    }
    if (!term.empty()) terms.push_back(std::move(term));
    if (terms.size() > 16) throw std::invalid_argument("search text exceeds 16 terms");
    TextQuery result;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i) result.match += ' ';
        result.match += '"';
        for (char ch : terms[i]) {
            if (ch == '"') result.match += '"';
            result.match += ch;
        }
        result.match += '"';
        if (i + 1 == terms.size()) result.match += '*';
        if (i) result.exact += ' ';
        result.exact += terms[i];
    }
    return result;
}

std::string encode_cursor(std::uint64_t generation, std::uint64_t offset) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::array<unsigned char, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[static_cast<std::size_t>(i)] = static_cast<unsigned char>(generation >> (56 - 8 * i));
        bytes[static_cast<std::size_t>(i + 8)] = static_cast<unsigned char>(offset >> (56 - 8 * i));
    }
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto first = static_cast<unsigned>(bytes[i]);
        const auto second = i + 1 < bytes.size() ? static_cast<unsigned>(bytes[i + 1]) : 0u;
        const auto third = i + 2 < bytes.size() ? static_cast<unsigned>(bytes[i + 2]) : 0u;
        result += alphabet[(first >> 2) & 63];
        result += alphabet[((first & 3) << 4) | (second >> 4)];
        if (i + 1 < bytes.size()) result += alphabet[((second & 15) << 2) | (third >> 6)];
        if (i + 2 < bytes.size()) result += alphabet[third & 63];
    }
    return result;
}

std::pair<std::uint64_t, std::uint64_t> decode_cursor(std::string_view cursor) {
    if (cursor.size() != 22) throw std::invalid_argument("invalid search cursor");
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::array<unsigned char, 16> bytes{};
    unsigned buffer = 0;
    unsigned bits = 0;
    std::size_t count = 0;
    for (char ch : cursor) {
        const auto index = alphabet.find(ch);
        if (index == std::string_view::npos) throw std::invalid_argument("invalid search cursor");
        buffer = (buffer << 6) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (count >= bytes.size()) throw std::invalid_argument("invalid search cursor");
            bytes[count++] = static_cast<unsigned char>(buffer >> bits);
            buffer &= (1u << bits) - 1u;
        }
    }
    if (count != bytes.size() || bits != 4 || buffer != 0) throw std::invalid_argument("invalid search cursor");
    std::uint64_t generation = 0;
    std::uint64_t offset = 0;
    for (int i = 0; i < 8; ++i) {
        generation = (generation << 8) | bytes[static_cast<std::size_t>(i)];
        offset = (offset << 8) | bytes[static_cast<std::size_t>(i + 8)];
    }
    if (offset > static_cast<std::uint64_t>(INT64_MAX)) throw std::invalid_argument("search offset out of range");
    return {generation, offset};
}

void validate_filters(const catalog::SearchFilters& filters) {
    auto validate = [](std::string_view text) {
        if (text.empty() || text.size() > 256 || text.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("invalid search filter value");
        }
        parse_text(text);
    };
    if (filters.repos.size() > 64 || filters.groups.size() > 64) {
        throw std::invalid_argument("too many search filter values");
    }
    for (const auto& repo : filters.repos) validate(repo);
    for (const auto& group : filters.groups) validate(group);
    if (filters.category) validate(*filters.category);
}

struct Selection {
    std::string from = " FROM packages p LEFT JOIN apps a ON a.package_id=p.id "
                       "LEFT JOIN installed i ON i.name=p.name ";
    std::string where = " WHERE 1=1";
    std::vector<std::string> parameters;
    void condition(std::string_view fragment) { where += fragment; }
    void bind(std::string_view column, std::string value) {
        where += " AND ";
        where += column;
        where += "=?" + std::to_string(parameters.size() + 1);
        parameters.push_back(std::move(value));
    }
    void array(std::string_view column, const std::vector<std::string>& values) {
        if (values.empty()) return;
        where += " AND ";
        where += column;
        where += " IN (SELECT value FROM json_each(?" + std::to_string(parameters.size() + 1) + "))";
        parameters.push_back(internal::json_array(values));
    }
    void apply(const catalog::SearchFilters& f) {
        if (f.installed || f.explicit_only || f.dependency_only || f.orphans || f.foreign || f.native || f.upgradable) {
            condition(" AND i.name IS NOT NULL");
        }
        if (f.not_installed) condition(" AND i.name IS NULL");
        if (f.explicit_only) bind("i.reason", "explicit");
        if (f.dependency_only) bind("i.reason", "dependency");
        if (f.orphans) condition(" AND i.orphan=1");
        if (f.foreign) condition(" AND i.native=0");
        if (f.native) condition(" AND i.native=1");
        if (f.upgradable) condition(" AND i.upgradable_to IS NOT NULL");
        if (f.aur_only) bind("p.source_kind", "community");
        if (f.repo_only) bind("p.source_kind", "official");
        if (f.apps_only) condition(" AND a.package_id IS NOT NULL");
        array("p.repo", f.repos);
        if (f.repos.empty()) condition(" AND (p.source_kind!='official' OR NOT EXISTS ("
            "SELECT 1 FROM packages earlier WHERE earlier.source_kind='official' AND earlier.name=p.name "
            "AND (earlier.repo_rank<p.repo_rank OR (earlier.repo_rank=p.repo_rank AND earlier.id<p.id))))");
        if (!f.groups.empty()) {
            condition(" AND EXISTS (SELECT 1 FROM \"groups\" g WHERE g.package_id=p.id");
            array("g.name", f.groups);
            where += ')';
        }
        if (f.category) {
            where += " AND EXISTS (SELECT 1 FROM json_each(a.categories_json) category WHERE category.value=?" +
                     std::to_string(parameters.size() + 1) + ')';
            parameters.push_back(*f.category);
        }
    }
    void bind_all(Statement& stmt) const {
        for (std::size_t i = 0; i < parameters.size(); ++i) stmt.bind(static_cast<int>(i + 1), parameters[i]);
    }
};

catalog::SearchHit hit_from(Statement& stmt) {
    catalog::SearchHit hit;
    hit.id = stmt.integer(0);
    const auto source = stmt.text(1).value_or("");
    if (source == "official") hit.source = domain::SourceKind::Official;
    else if (source == "community") hit.source = domain::SourceKind::Community;
    else throw std::runtime_error("unknown package source");
    if (hit.source == domain::SourceKind::Official) hit.repo = stmt.text(2);
    hit.name = stmt.text(3).value_or("");
    hit.version = stmt.text(4).value_or("");
    hit.description = stmt.text(5).value_or("");
    hit.app_name = stmt.text(6);
    hit.app_summary = stmt.text(7);
    hit.icon_path = stmt.text(8);
    hit.installed = !stmt.is_null(9);
    hit.installed_version = stmt.text(9);
    hit.upgradable_to = stmt.text(10);
    return hit;
}

void shadow_repos(const Database& db, catalog::SearchHit& hit) {
    if (hit.source != domain::SourceKind::Official) return;
    auto repos = db.prepare("SELECT id,repo FROM packages WHERE source_kind='official' AND name=?1 ORDER BY repo_rank,id LIMIT 256");
    repos.bind(1, hit.name);
    while (repos.step()) {
        if (repos.integer(0) == hit.id) hit.shadowed = !hit.also_in.empty();
        else hit.also_in.push_back(repos.text(1).value_or(""));
    }
}

constexpr std::string_view hit_columns =
    "p.id,p.source_kind,p.repo,p.name,p.version,p.description,a.display_name,a.summary,a.icon_path,i.version,i.upgradable_to";

}

struct Store::Impl {
    explicit Impl(std::filesystem::path cache_home) : directory(std::move(cache_home), false) {}
    internal::Directory directory;
    std::unique_ptr<Database> db;
    dev_t device{};
    ino_t inode{};
    std::uint64_t generation{};

    void refresh() {
        auto valid = [](const struct stat& info) {
            return S_ISREG(info.st_mode) && info.st_uid == getuid() && (info.st_mode & 0777) == 0600;
        };
        struct stat info {};
        if (fstatat(directory.fd, "catalog.sqlite", &info, AT_SYMLINK_NOFOLLOW) != 0 || !valid(info)) {
            throw std::runtime_error("catalog snapshot missing or has unsafe owner or mode");
        }
        if (db && info.st_dev == device && info.st_ino == inode) return;
        for (unsigned attempt = 0; attempt < 4; ++attempt) {
            if (!valid(info)) throw std::runtime_error("catalog snapshot owner or mode refused");
            auto next = std::make_unique<Database>(directory.path / "catalog.sqlite", SQLITE_OPEN_READONLY);
            auto version = next->prepare("PRAGMA user_version");
            if (!version.step() || version.integer(0) != 2) throw std::runtime_error("unsupported catalog schema version");
            auto meta = next->prepare("SELECT value FROM meta WHERE key=?1");
            meta.bind(1, "generation");
            if (!meta.step()) throw std::runtime_error("catalog generation is missing");
            const auto value = meta.text(0).value_or("");
            std::size_t end = 0;
            const auto parsed = std::stoull(value, &end);
            if (end != value.size() || parsed > static_cast<std::uint64_t>(INT64_MAX)) {
                throw std::runtime_error("invalid catalog generation");
            }
            struct stat again {};
            if (fstatat(directory.fd, "catalog.sqlite", &again, AT_SYMLINK_NOFOLLOW) != 0) {
                throw std::runtime_error("catalog snapshot disappeared");
            }
            if (again.st_dev != info.st_dev || again.st_ino != info.st_ino) {
                info = again;
                continue;
            }
            db = std::move(next);
            device = info.st_dev;
            inode = info.st_ino;
            generation = parsed;
            return;
        }
        throw std::runtime_error("catalog changed repeatedly while opening");
    }
};

Store::Store(std::filesystem::path cache_home) : impl_(std::make_unique<Impl>(std::move(cache_home))) {}
Store::~Store() = default;
std::expected<catalog::SearchPage, catalog::CatalogError> Store::search(const catalog::SearchQuery& query) {
    if (query.limit == 0 || query.limit > 200) {
        return std::unexpected(internal::error("invalid-query", "search limit must be 1..200"));
    }
    try {
        const auto text = parse_text(query.text);
        validate_filters(query.filters);
        impl_->refresh();
        std::uint64_t offset = 0;
        if (query.cursor) {
            auto [generation, index] = decode_cursor(*query.cursor);
            if (generation != impl_->generation) {
                return std::unexpected(internal::error("stale-cursor", "catalog generation changed"));
            }
            offset = index;
        }
        Selection select;
        if (!text.match.empty()) {
            select.from += " JOIN package_fts ON package_fts.rowid=p.id";
            select.where += " AND package_fts MATCH ?1";
            select.parameters.push_back(text.match);
        }
        select.apply(query.filters);
        auto count = impl_->db->prepare(("SELECT count(*)" + select.from + select.where).c_str());
        select.bind_all(count);
        if (!count.step()) throw std::runtime_error("search count unavailable");
        const auto total = static_cast<std::uint64_t>(count.integer(0));
        std::string sql = "SELECT " + std::string(hit_columns) + select.from + select.where;
        if (query.sort == catalog::SortOrder::Relevance && !text.match.empty()) {
            sql += " ORDER BY CASE WHEN p.name=?" + std::to_string(select.parameters.size() + 1) +
                   " COLLATE NOCASE THEN 0 WHEN substr(p.name,1,length(?" +
                   std::to_string(select.parameters.size() + 1) + ")) COLLATE NOCASE=?" +
                   std::to_string(select.parameters.size() + 1) + " THEN 1 ELSE 2 END, "
                   "bm25(package_fts,10.0,8.0,1.0,2.0), "
                   "(p.source_kind='community' AND p.description=''), i.name IS NULL, "
                   "p.name COLLATE NOCASE, p.source_kind='community', p.repo_rank, p.id";
        } else if (query.sort == catalog::SortOrder::Name || query.sort == catalog::SortOrder::Relevance) {
            sql += " ORDER BY p.name COLLATE NOCASE, p.source_kind='community', p.repo_rank, p.id";
        } else {
            return std::unexpected(internal::error("invalid-query", "unknown search sort order"));
        }
        const int extra = query.sort == catalog::SortOrder::Relevance && !text.match.empty() ? 1 : 0;
        sql += " LIMIT ?" + std::to_string(select.parameters.size() + static_cast<std::size_t>(extra) + 1) +
               " OFFSET ?" + std::to_string(select.parameters.size() + static_cast<std::size_t>(extra) + 2);
        auto rows = impl_->db->prepare(sql.c_str());
        select.bind_all(rows);
        if (extra) rows.bind(static_cast<int>(select.parameters.size() + 1), text.exact);
        rows.bind(static_cast<int>(select.parameters.size() + static_cast<std::size_t>(extra) + 1),
                  static_cast<std::int64_t>(query.limit));
        rows.bind(static_cast<int>(select.parameters.size() + static_cast<std::size_t>(extra) + 2),
                  static_cast<std::int64_t>(offset));
        catalog::SearchPage page;
        page.total = total;
        page.generation = impl_->generation;
        while (rows.step()) {
            auto hit = hit_from(rows);
            shadow_repos(*impl_->db, hit);
            page.hits.push_back(std::move(hit));
        }
        if (offset <= total && page.hits.size() < total - offset) {
            page.next_cursor = encode_cursor(impl_->generation, offset + page.hits.size());
        }
        return page;
    } catch (const std::invalid_argument& e) {
        return std::unexpected(internal::error("invalid-query", e.what()));
    } catch (const std::exception& e) {
        return std::unexpected(internal::error("storage", e.what()));
    }
}
std::expected<std::optional<catalog::PackageDetails>, catalog::CatalogError> Store::package(std::int64_t id) {
    if (id <= 0) return std::unexpected(internal::error("invalid-query", "package id must be positive"));
    try {
        impl_->refresh();
        auto row = impl_->db->prepare(
            "SELECT p.id,p.source_kind,p.repo,p.name,p.version,p.description,a.display_name,a.summary,a.icon_path,i.version,i.upgradable_to,"
            "p.url,p.package_base,p.groups_json,p.provides_json,p.depends_json,p.optdepends_json,p.conflicts_json,p.replaces_json,"
            "p.download_size,p.install_size,p.build_date,x.arch,x.packager,x.licenses_json,x.makedepends_json,x.checkdepends_json,x.comaintainers_json,"
            "c.maintainer,c.submitter,c.votes,c.popularity,c.out_of_date,c.first_submitted,c.last_modified,c.keywords_json,"
            "a.appstream_id,a.description_json,a.categories_json,a.screenshots_json,a.launchable,a.homepage,a.license,a.developer,"
            "i.reason,i.install_date,i.size,i.native,i.orphan,ix.groups_json "
            "FROM packages p LEFT JOIN package_extra x ON x.package_id=p.id "
            "LEFT JOIN community c ON c.package_id=p.id LEFT JOIN apps a ON a.package_id=p.id "
            "LEFT JOIN installed i ON i.name=p.name "
            "LEFT JOIN installed_extra ix ON ix.name=p.name WHERE p.id=?1");
        row.bind(1, id);
        if (!row.step()) return std::optional<catalog::PackageDetails>{};
        catalog::PackageDetails details;
        details.summary = hit_from(row);
        shadow_repos(*impl_->db, details.summary);
        details.also_in = details.summary.also_in;
        details.shadowed = details.summary.shadowed;
        auto array = [&](int index) {
            return internal::read_array(*impl_->db, row.text(index).value_or("[]"));
        };
        if (details.summary.source == domain::SourceKind::Official) {
            catalog::RepoPackage record;
            record.repo = details.summary.repo.value_or("");
            record.name = details.summary.name;
            record.version = details.summary.version;
            record.description = details.summary.description;
            record.url = row.text(11).value_or("");
            record.base = row.text(12).value_or("");
            record.groups = array(13);
            record.provides = array(14);
            record.depends = array(15);
            record.optdepends = array(16);
            record.conflicts = array(17);
            record.replaces = array(18);
            record.download_size = static_cast<std::uint64_t>(row.integer(19));
            record.install_size = static_cast<std::uint64_t>(row.integer(20));
            record.build_date = row.integer(21);
            record.arch = row.text(22).value_or("");
            record.packager = row.text(23).value_or("");
            record.licenses = array(24);
            record.makedepends = array(25);
            record.checkdepends = array(26);
            details.repo_record = std::move(record);
        } else {
            catalog::AurPackage record;
            record.name = details.summary.name;
            record.version = details.summary.version;
            record.description = details.summary.description;
            record.url = row.text(11).value_or("");
            record.package_base = row.text(12).value_or("");
            record.groups = array(13);
            record.provides = array(14);
            record.depends = array(15);
            record.optdepends = array(16);
            record.conflicts = array(17);
            record.replaces = array(18);
            record.licenses = array(24);
            record.makedepends = array(25);
            record.checkdepends = array(26);
            record.comaintainers = array(27);
            record.maintainer = row.text(28);
            record.submitter = row.text(29);
            record.votes = static_cast<std::uint32_t>(row.integer(30));
            record.popularity = row.real(31);
            if (!row.is_null(32)) record.out_of_date = row.integer(32);
            record.first_submitted = row.integer(33);
            record.last_modified = row.integer(34);
            record.keywords = array(35);
            details.community_record = std::move(record);
        }
        if (!row.is_null(36)) {
            catalog::AppMetadata app;
            app.pkgname = details.summary.name;
            app.appstream_id = row.text(36).value_or("");
            app.name = details.summary.app_name.value_or("");
            app.summary = details.summary.app_summary.value_or("");
            app.description_paragraphs = array(37);
            app.categories = array(38);
            app.screenshots = internal::read_screenshots(*impl_->db, row.text(39).value_or("[]"));
            app.icon_path = details.summary.icon_path;
            app.launchable = row.text(40);
            app.homepage = row.text(41);
            app.license = row.text(42);
            app.developer = row.text(43);
            details.app = std::move(app);
        }
        if (details.summary.installed) {
            catalog::InstalledPackage installed;
            installed.name = details.summary.name;
            installed.version = details.summary.installed_version.value_or("");
            const auto reason = row.text(44).value_or("");
            if (reason != "explicit" && reason != "dependency") throw std::runtime_error("unknown install reason");
            installed.reason = reason == "explicit" ? domain::InstallReason::Explicit : domain::InstallReason::Dependency;
            installed.install_date = row.integer(45);
            installed.size = static_cast<std::uint64_t>(row.integer(46));
            installed.native = row.integer(47) == 1;
            installed.orphan = row.integer(48) == 1;
            installed.upgradable_to = details.summary.upgradable_to;
            installed.groups = array(49);
            details.installed = std::move(installed);
        }
        return std::optional<catalog::PackageDetails>(std::move(details));
    } catch (const std::exception& e) {
        return std::unexpected(internal::error("storage", e.what()));
    }
}

}
