#include <braurser/protocol/dto.hpp>
#include <array>
#include <cctype>
#include <limits>
#include <cmath>
#include <type_traits>

namespace braurser::protocol {
namespace {

struct ObjectKeys {
    std::initializer_list<std::string_view> envelope{};
    bool tagged = false;
};

template <class T>
struct IsVector : std::false_type {};

template <class T, class Allocator>
struct IsVector<std::vector<T, Allocator>> : std::true_type {};

template <class T>
void validate_value(const nlohmann::json& j) {
    if constexpr (std::is_same_v<T, std::string>) {
        if (!j.is_string()) throw std::invalid_argument("invalid string type");
    } else if constexpr (std::is_arithmetic_v<T>) {
        static_cast<void>(j.get<T>());
    } else if constexpr (std::is_same_v<T, nlohmann::json>) {
        static_cast<void>(j);
    } else if constexpr (IsVector<T>::value) {
        if (!j.is_array()) throw std::invalid_argument("invalid array type");
        for (const auto& item : j) validate_value<typename T::value_type>(item);
    } else {
        validate_json(j, TypeTag<T>{});
    }
}

template <bool Materialize, class T>
void read_field(const nlohmann::json& j, const char* key, T& out) {
    if (!j.contains(key)) throw std::invalid_argument("missing field");
    validate_wire(j.at(key), key);
    if constexpr (std::is_same_v<T, bool>) {
        if (!j.at(key).is_boolean()) throw std::invalid_argument("invalid boolean");
    } else if constexpr (std::is_unsigned_v<T>) {
        if (!j.at(key).is_number_integer() ||
            (!j.at(key).is_number_unsigned() && j.at(key).get<std::int64_t>() < 0) ||
            j.at(key).get<std::uint64_t>() > std::numeric_limits<T>::max())
            throw std::invalid_argument("invalid unsigned integer");
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!j.at(key).is_number() || !std::isfinite(j.at(key).get<double>()) ||
            j.at(key).get<double>() < 0) throw std::invalid_argument("invalid number");
    }
    if constexpr (Materialize) out = j.at(key).get<T>();
    else validate_value<T>(j.at(key));
}

template <bool Materialize, class T>
void read_field(const nlohmann::json& j, const char* key, std::optional<T>& out) {
    if (j.contains(key) && !j.at(key).is_null()) {
        T checked{};
        read_field<Materialize>(j, key, checked);
        if constexpr (Materialize) out = std::move(checked);
    }
}

bool present(const nlohmann::json& j, const char* key) {
    return j.contains(key) && !j.at(key).is_null();
}

const std::string& text(const nlohmann::json& j, const char* key) {
    return j.at(key).get_ref<const std::string&>();
}

template <class T>
void write_field(nlohmann::json& j, const char* key, const T& value) {
    j[key] = value;
}

template <class T>
void write_field(nlohmann::json& j, const char* key, const std::optional<T>& value) {
    if (value) j[key] = *value;
}

void read_object(const nlohmann::json& j, std::initializer_list<std::string_view> keys, ObjectKeys extra) {
    if (!j.is_object()) throw std::invalid_argument("expected object");
    std::array<std::string_view, 4> excluded{};
    if (extra.envelope.size() + static_cast<std::size_t>(extra.tagged) > excluded.size())
        throw std::invalid_argument("invalid object projection");
    std::copy(extra.envelope.begin(), extra.envelope.end(), excluded.begin());
    const auto count = extra.envelope.size() + static_cast<std::size_t>(extra.tagged);
    if (extra.tagged) excluded[extra.envelope.size()] = "kind";
    validate_wire(j, {}, std::span(excluded).first(count));
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end() &&
            std::find(extra.envelope.begin(), extra.envelope.end(), it.key()) == extra.envelope.end() &&
            !(extra.tagged && it.key() == "kind"))
            throw std::invalid_argument("unknown field");
    }
}

}


template <bool Materialize>
void read_json(const nlohmann::json& j, QuestionChoice& out, ObjectKeys extra);

#define BRAURSER_READ_JSON(type) \
template <bool Materialize> \
void read_json(const nlohmann::json& j, type& out, ObjectKeys extra); \
void from_json(const nlohmann::json& j, type& out) { read_json<true>(j, out, {}); } \
void validate_json(const nlohmann::json& j, TypeTag<type>) { type out{}; read_json<false>(j, out, {}); } \
template <bool Materialize> \
void read_json(const nlohmann::json& j, type& out, ObjectKeys extra)


BRAURSER_READ_JSON(PackageRef) {
    read_object(j, {"source","repo","name"}, extra);
    read_field<Materialize>(j, "source", out.source);
    read_field<Materialize>(j, "repo", out.repo);
    read_field<Materialize>(j, "name", out.name);
    if (!br_valid_pkgname(text(j, "name").data(), text(j, "name").size()) ||
        (present(j, "repo") && !br_valid_repo_name(text(j, "repo").data(), text(j, "repo").size())))
        throw std::invalid_argument("invalid package reference");
}

void to_json(nlohmann::json& j, const PackageRef& value) {
    j = nlohmann::json::object();
    write_field(j, "source", value.source);
    write_field(j, "repo", value.repo);
    write_field(j, "name", value.name);
    validate_json(j, TypeTag<PackageRef>{});
}

BRAURSER_READ_JSON(PackageSummary) {
    read_object(j, {"package","version","description","installed","installedVersion","installedSize","downloadSize","appName","iconPath","alsoIn","shadowed"}, extra);
    read_field<Materialize>(j, "package", out.package);
    read_field<Materialize>(j, "version", out.version);
    read_field<Materialize>(j, "description", out.description);
    read_field<Materialize>(j, "installed", out.installed);
    read_field<Materialize>(j, "installedVersion", out.installedVersion);
    read_field<Materialize>(j, "installedSize", out.installedSize);
    read_field<Materialize>(j, "downloadSize", out.downloadSize);
    read_field<Materialize>(j, "appName", out.appName);
    read_field<Materialize>(j, "iconPath", out.iconPath);
    read_field<Materialize>(j, "alsoIn", out.alsoIn);
    read_field<Materialize>(j, "shadowed", out.shadowed);
    if (present(j, "alsoIn")) {
        if (j.at("alsoIn").size() > 256) throw std::invalid_argument("too many repositories");
        for (const auto& item : j.at("alsoIn")) {
            const auto& repo = item.get_ref<const std::string&>();
            if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
        }
    }
}

void to_json(nlohmann::json& j, const PackageSummary& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "version", value.version);
    write_field(j, "description", value.description);
    write_field(j, "installed", value.installed);
    write_field(j, "installedVersion", value.installedVersion);
    write_field(j, "installedSize", value.installedSize);
    write_field(j, "downloadSize", value.downloadSize);
    write_field(j, "appName", value.appName);
    write_field(j, "iconPath", value.iconPath);
    write_field(j, "alsoIn", value.alsoIn);
    write_field(j, "shadowed", value.shadowed);
    validate_json(j, TypeTag<PackageSummary>{});
}

BRAURSER_READ_JSON(Screenshot) {
    read_object(j, {"url","caption"}, extra);
    read_field<Materialize>(j, "url", out.url);
    read_field<Materialize>(j, "caption", out.caption);
}

void to_json(nlohmann::json& j, const Screenshot& value) {
    j = nlohmann::json::object();
    write_field(j, "url", value.url);
    write_field(j, "caption", value.caption);
    validate_json(j, TypeTag<Screenshot>{});
}

BRAURSER_READ_JSON(AppMetadata) {
    read_object(j, {"appstream_id","name","summary","description_paragraphs","icon_path","screenshots","categories","launchables"}, extra);
    read_field<Materialize>(j, "appstream_id", out.appstream_id);
    read_field<Materialize>(j, "name", out.name);
    read_field<Materialize>(j, "summary", out.summary);
    read_field<Materialize>(j, "description_paragraphs", out.description_paragraphs);
    read_field<Materialize>(j, "icon_path", out.icon_path);
    read_field<Materialize>(j, "screenshots", out.screenshots);
    read_field<Materialize>(j, "categories", out.categories);
    read_field<Materialize>(j, "launchables", out.launchables);
}

void to_json(nlohmann::json& j, const AppMetadata& value) {
    j = nlohmann::json::object();
    write_field(j, "appstream_id", value.appstream_id);
    write_field(j, "name", value.name);
    write_field(j, "summary", value.summary);
    write_field(j, "description_paragraphs", value.description_paragraphs);
    write_field(j, "icon_path", value.icon_path);
    write_field(j, "screenshots", value.screenshots);
    write_field(j, "categories", value.categories);
    write_field(j, "launchables", value.launchables);
    validate_json(j, TypeTag<AppMetadata>{});
}

BRAURSER_READ_JSON(CommunityInfo) {
    read_object(j, {"maintainer","submitter","votes","popularity","out_of_date_since","first_submitted","last_modified","package_base","upstream_url"}, extra);
    read_field<Materialize>(j, "maintainer", out.maintainer);
    read_field<Materialize>(j, "submitter", out.submitter);
    read_field<Materialize>(j, "votes", out.votes);
    read_field<Materialize>(j, "popularity", out.popularity);
    read_field<Materialize>(j, "out_of_date_since", out.out_of_date_since);
    read_field<Materialize>(j, "first_submitted", out.first_submitted);
    read_field<Materialize>(j, "last_modified", out.last_modified);
    read_field<Materialize>(j, "package_base", out.package_base);
    read_field<Materialize>(j, "upstream_url", out.upstream_url);
}

void to_json(nlohmann::json& j, const CommunityInfo& value) {
    j = nlohmann::json::object();
    write_field(j, "maintainer", value.maintainer);
    write_field(j, "submitter", value.submitter);
    write_field(j, "votes", value.votes);
    write_field(j, "popularity", value.popularity);
    write_field(j, "out_of_date_since", value.out_of_date_since);
    write_field(j, "first_submitted", value.first_submitted);
    write_field(j, "last_modified", value.last_modified);
    write_field(j, "package_base", value.package_base);
    write_field(j, "upstream_url", value.upstream_url);
    validate_json(j, TypeTag<CommunityInfo>{});
}

BRAURSER_READ_JSON(PackageDetails) {
    read_object(j, {"package","version","description","dependencies","url","alsoIn","shadowed"}, extra);
    read_field<Materialize>(j, "package", out.package);
    read_field<Materialize>(j, "version", out.version);
    read_field<Materialize>(j, "description", out.description);
    read_field<Materialize>(j, "dependencies", out.dependencies);
    read_field<Materialize>(j, "url", out.url);
    read_field<Materialize>(j, "alsoIn", out.alsoIn);
    read_field<Materialize>(j, "shadowed", out.shadowed);
    if (present(j, "alsoIn")) {
        if (j.at("alsoIn").size() > 256) throw std::invalid_argument("too many repositories");
        for (const auto& item : j.at("alsoIn")) {
            const auto& repo = item.get_ref<const std::string&>();
            if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
        }
    }
}

void to_json(nlohmann::json& j, const PackageDetails& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "version", value.version);
    write_field(j, "description", value.description);
    write_field(j, "dependencies", value.dependencies);
    write_field(j, "url", value.url);
    write_field(j, "alsoIn", value.alsoIn);
    write_field(j, "shadowed", value.shadowed);
    validate_json(j, TypeTag<PackageDetails>{});
}

BRAURSER_READ_JSON(SearchFilters) {
    read_object(j, {"installed","not_installed","explicit","dependency","orphans","foreign","native","upgradable","repos","groups","aur_only","repo_only","apps_only","category"}, extra);
    if (j.contains("installed")) read_field<Materialize>(j, "installed", out.installed);
    if (j.contains("not_installed")) read_field<Materialize>(j, "not_installed", out.not_installed);
    if (j.contains("explicit")) read_field<Materialize>(j, "explicit", out.explicit_);
    if (j.contains("dependency")) read_field<Materialize>(j, "dependency", out.dependency);
    if (j.contains("orphans")) read_field<Materialize>(j, "orphans", out.orphans);
    if (j.contains("foreign")) read_field<Materialize>(j, "foreign", out.foreign);
    if (j.contains("native")) read_field<Materialize>(j, "native", out.native);
    if (j.contains("upgradable")) read_field<Materialize>(j, "upgradable", out.upgradable);
    if (j.contains("repos")) read_field<Materialize>(j, "repos", out.repos);
    if (j.contains("groups")) read_field<Materialize>(j, "groups", out.groups);
    if (j.contains("aur_only")) read_field<Materialize>(j, "aur_only", out.aur_only);
    if (j.contains("repo_only")) read_field<Materialize>(j, "repo_only", out.repo_only);
    if (j.contains("apps_only")) read_field<Materialize>(j, "apps_only", out.apps_only);
    read_field<Materialize>(j, "category", out.category);
    if (j.value("aur_only", false) && j.value("repo_only", false)) throw std::invalid_argument("conflicting source filters");
    if (j.contains("repos")) for (const auto& item : j.at("repos")) {
        const auto& repo = item.get_ref<const std::string&>();
        if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
    }
    if (j.contains("groups")) for (const auto& item : j.at("groups")) {
        const auto& group = item.get_ref<const std::string&>();
        if (!br_valid_pkgname(group.data(), group.size())) throw std::invalid_argument("invalid group");
    }
}

void to_json(nlohmann::json& j, const SearchFilters& value) {
    j = nlohmann::json::object();
    write_field(j, "installed", value.installed);
    write_field(j, "not_installed", value.not_installed);
    write_field(j, "explicit", value.explicit_);
    write_field(j, "dependency", value.dependency);
    write_field(j, "orphans", value.orphans);
    write_field(j, "foreign", value.foreign);
    write_field(j, "native", value.native);
    write_field(j, "upgradable", value.upgradable);
    write_field(j, "repos", value.repos);
    write_field(j, "groups", value.groups);
    write_field(j, "aur_only", value.aur_only);
    write_field(j, "repo_only", value.repo_only);
    write_field(j, "apps_only", value.apps_only);
    write_field(j, "category", value.category);
    validate_json(j, TypeTag<SearchFilters>{});
}

BRAURSER_READ_JSON(CatalogFilters) {
    read_object(j, {"installed", "explicit", "deps", "orphans", "foreign", "native", "upgradable",
                    "sourceRepo", "group", "aurOnly", "repoOnly", "appsOnly"}, extra);
    if (j.contains("installed")) read_field<Materialize>(j, "installed", out.installed);
    if (j.contains("explicit")) read_field<Materialize>(j, "explicit", out.explicit_);
    if (j.contains("deps")) read_field<Materialize>(j, "deps", out.deps);
    if (j.contains("orphans")) read_field<Materialize>(j, "orphans", out.orphans);
    if (j.contains("foreign")) read_field<Materialize>(j, "foreign", out.foreign);
    if (j.contains("native")) read_field<Materialize>(j, "native", out.native);
    if (j.contains("upgradable")) read_field<Materialize>(j, "upgradable", out.upgradable);
    read_field<Materialize>(j, "sourceRepo", out.sourceRepo);
    read_field<Materialize>(j, "group", out.group);
    if (j.contains("aurOnly")) read_field<Materialize>(j, "aurOnly", out.aurOnly);
    if (j.contains("repoOnly")) read_field<Materialize>(j, "repoOnly", out.repoOnly);
    if (j.contains("appsOnly")) read_field<Materialize>(j, "appsOnly", out.appsOnly);
    if (j.value("aurOnly", false) && j.value("repoOnly", false)) throw std::invalid_argument("conflicting source filters");
    if (present(j, "sourceRepo") && !br_valid_repo_name(text(j, "sourceRepo").data(), text(j, "sourceRepo").size()))
        throw std::invalid_argument("invalid repository");
    if (present(j, "group") && !br_valid_pkgname(text(j, "group").data(), text(j, "group").size()))
        throw std::invalid_argument("invalid group");
}

void to_json(nlohmann::json& j, const CatalogFilters& value) {
    j = nlohmann::json::object();
    write_field(j, "installed", value.installed);
    write_field(j, "explicit", value.explicit_);
    write_field(j, "deps", value.deps);
    write_field(j, "orphans", value.orphans);
    write_field(j, "foreign", value.foreign);
    write_field(j, "native", value.native);
    write_field(j, "upgradable", value.upgradable);
    write_field(j, "sourceRepo", value.sourceRepo);
    write_field(j, "group", value.group);
    write_field(j, "aurOnly", value.aurOnly);
    write_field(j, "repoOnly", value.repoOnly);
    write_field(j, "appsOnly", value.appsOnly);
    validate_json(j, TypeTag<CatalogFilters>{});
}

BRAURSER_READ_JSON(SearchQuery) {
    read_object(j, {"text","filters","sort","cursor","limit"}, extra);
    read_field<Materialize>(j, "text", out.text);
    read_field<Materialize>(j, "filters", out.filters);
    read_field<Materialize>(j, "sort", out.sort);
    read_field<Materialize>(j, "cursor", out.cursor);
    read_field<Materialize>(j, "limit", out.limit);
    if (j.at("limit").get<std::uint64_t>() == 0 || j.at("limit").get<std::uint64_t>() > 200 || text(j, "text").size() > 512)
        throw std::invalid_argument("invalid search bounds");
}

void to_json(nlohmann::json& j, const SearchQuery& value) {
    j = nlohmann::json::object();
    write_field(j, "text", value.text);
    write_field(j, "filters", value.filters);
    write_field(j, "sort", value.sort);
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    validate_json(j, TypeTag<SearchQuery>{});
}

BRAURSER_READ_JSON(SearchPage) {
    read_object(j, {"packages","next_cursor","stale"}, extra);
    read_field<Materialize>(j, "packages", out.packages);
    read_field<Materialize>(j, "next_cursor", out.next_cursor);
    read_field<Materialize>(j, "stale", out.stale);
}

void to_json(nlohmann::json& j, const SearchPage& value) {
    j = nlohmann::json::object();
    write_field(j, "packages", value.packages);
    write_field(j, "next_cursor", value.next_cursor);
    write_field(j, "stale", value.stale);
    validate_json(j, TypeTag<SearchPage>{});
}

BRAURSER_READ_JSON(VariantOption) {
    read_object(j, {"package","kind","development","recommended"}, extra);
    read_field<Materialize>(j, "package", out.package);
    read_field<Materialize>(j, "kind", out.kind);
    read_field<Materialize>(j, "development", out.development);
    read_field<Materialize>(j, "recommended", out.recommended);
}

void to_json(nlohmann::json& j, const VariantOption& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "kind", value.kind);
    write_field(j, "development", value.development);
    write_field(j, "recommended", value.recommended);
    validate_json(j, TypeTag<VariantOption>{});
}

BRAURSER_READ_JSON(VariantChoices) {
    read_object(j, {"options","urls_differ"}, extra);
    read_field<Materialize>(j, "options", out.options);
    read_field<Materialize>(j, "urls_differ", out.urls_differ);
}

void to_json(nlohmann::json& j, const VariantChoices& value) {
    j = nlohmann::json::object();
    write_field(j, "options", value.options);
    write_field(j, "urls_differ", value.urls_differ);
    validate_json(j, TypeTag<VariantChoices>{});
}

BRAURSER_READ_JSON(WikiText) {
    read_object(j, {"s","emphasis","strong","code"}, extra);
    read_field<Materialize>(j, "s", out.s);
    if (j.contains("emphasis")) read_field<Materialize>(j, "emphasis", out.emphasis);
    if (j.contains("strong")) read_field<Materialize>(j, "strong", out.strong);
    if (j.contains("code")) read_field<Materialize>(j, "code", out.code);
}

void to_json(nlohmann::json& j, const WikiText& value) {
    j = nlohmann::json::object();
    write_field(j, "s", value.s);
    write_field(j, "emphasis", value.emphasis);
    write_field(j, "strong", value.strong);
    write_field(j, "code", value.code);
    validate_json(j, TypeTag<WikiText>{});
}

BRAURSER_READ_JSON(WikiLink) {
    read_object(j, {"label","href"}, extra);
    read_field<Materialize>(j, "label", out.label);
    read_field<Materialize>(j, "href", out.href);
}

void to_json(nlohmann::json& j, const WikiLink& value) {
    j = nlohmann::json::object();
    write_field(j, "label", value.label);
    write_field(j, "href", value.href);
    validate_json(j, TypeTag<WikiLink>{});
}

BRAURSER_READ_JSON(WikiHeading) {
    read_object(j, {"level","text"}, extra);
    read_field<Materialize>(j, "level", out.level);
    read_field<Materialize>(j, "text", out.text);
    if (j.at("level").get<std::uint64_t>() < 2 || j.at("level").get<std::uint64_t>() > 4) throw std::invalid_argument("invalid heading level");
}

void to_json(nlohmann::json& j, const WikiHeading& value) {
    j = nlohmann::json::object();
    write_field(j, "level", value.level);
    write_field(j, "text", value.text);
    validate_json(j, TypeTag<WikiHeading>{});
}

BRAURSER_READ_JSON(WikiParagraph) {
    read_object(j, {"inlines"}, extra);
    read_field<Materialize>(j, "inlines", out.inlines);
}

void to_json(nlohmann::json& j, const WikiParagraph& value) {
    j = nlohmann::json::object();
    write_field(j, "inlines", value.inlines);
    validate_json(j, TypeTag<WikiParagraph>{});
}

BRAURSER_READ_JSON(WikiList) {
    read_object(j, {"ordered","items"}, extra);
    read_field<Materialize>(j, "ordered", out.ordered);
    read_field<Materialize>(j, "items", out.items);
}

void to_json(nlohmann::json& j, const WikiList& value) {
    j = nlohmann::json::object();
    write_field(j, "ordered", value.ordered);
    write_field(j, "items", value.items);
    validate_json(j, TypeTag<WikiList>{});
}

BRAURSER_READ_JSON(WikiCode) {
    read_object(j, {"text"}, extra);
    read_field<Materialize>(j, "text", out.text);
}

void to_json(nlohmann::json& j, const WikiCode& value) {
    j = nlohmann::json::object();
    write_field(j, "text", value.text);
    validate_json(j, TypeTag<WikiCode>{});
}

BRAURSER_READ_JSON(WikiNote) {
    read_object(j, {"inlines"}, extra);
    read_field<Materialize>(j, "inlines", out.inlines);
}

void to_json(nlohmann::json& j, const WikiNote& value) {
    j = nlohmann::json::object();
    write_field(j, "inlines", value.inlines);
    validate_json(j, TypeTag<WikiNote>{});
}

BRAURSER_READ_JSON(WikiWarning) {
    read_object(j, {"inlines"}, extra);
    read_field<Materialize>(j, "inlines", out.inlines);
}

void to_json(nlohmann::json& j, const WikiWarning& value) {
    j = nlohmann::json::object();
    write_field(j, "inlines", value.inlines);
    validate_json(j, TypeTag<WikiWarning>{});
}

BRAURSER_READ_JSON(WikiSection) {
    read_object(j, {"level","title"}, extra);
    read_field<Materialize>(j, "level", out.level);
    read_field<Materialize>(j, "title", out.title);
    if (j.at("level").get<std::uint64_t>() < 2 || j.at("level").get<std::uint64_t>() > 4) throw std::invalid_argument("invalid heading level");
}

void to_json(nlohmann::json& j, const WikiSection& value) {
    j = nlohmann::json::object();
    write_field(j, "level", value.level);
    write_field(j, "title", value.title);
    validate_json(j, TypeTag<WikiSection>{});
}

BRAURSER_READ_JSON(WikiDocument) {
    read_object(j, {"title","source_url","fetched_at","stale","attribution_key","blocks","sections"}, extra);
    read_field<Materialize>(j, "title", out.title);
    read_field<Materialize>(j, "source_url", out.source_url);
    read_field<Materialize>(j, "fetched_at", out.fetched_at);
    read_field<Materialize>(j, "stale", out.stale);
    read_field<Materialize>(j, "attribution_key", out.attribution_key);
    read_field<Materialize>(j, "blocks", out.blocks);
    read_field<Materialize>(j, "sections", out.sections);
}

void to_json(nlohmann::json& j, const WikiDocument& value) {
    j = nlohmann::json::object();
    write_field(j, "title", value.title);
    write_field(j, "source_url", value.source_url);
    write_field(j, "fetched_at", value.fetched_at);
    write_field(j, "stale", value.stale);
    write_field(j, "attribution_key", value.attribution_key);
    write_field(j, "blocks", value.blocks);
    write_field(j, "sections", value.sections);
    validate_json(j, TypeTag<WikiDocument>{});
}

BRAURSER_READ_JSON(NewsItem) {
    read_object(j, {"title","url","published","requires_manual_intervention","acknowledged"}, extra);
    read_field<Materialize>(j, "title", out.title);
    read_field<Materialize>(j, "url", out.url);
    read_field<Materialize>(j, "published", out.published);
    read_field<Materialize>(j, "requires_manual_intervention", out.requires_manual_intervention);
    read_field<Materialize>(j, "acknowledged", out.acknowledged);
}

void to_json(nlohmann::json& j, const NewsItem& value) {
    j = nlohmann::json::object();
    write_field(j, "title", value.title);
    write_field(j, "url", value.url);
    write_field(j, "published", value.published);
    write_field(j, "requires_manual_intervention", value.requires_manual_intervention);
    write_field(j, "acknowledged", value.acknowledged);
    validate_json(j, TypeTag<NewsItem>{});
}

BRAURSER_READ_JSON(RestartAdviceDTO) {
    read_object(j, {"package","level","reason_key","detail"}, extra);
    read_field<Materialize>(j, "package", out.package);
    read_field<Materialize>(j, "level", out.level);
    read_field<Materialize>(j, "reason_key", out.reason_key);
    read_field<Materialize>(j, "detail", out.detail);
}

void to_json(nlohmann::json& j, const RestartAdviceDTO& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "level", value.level);
    write_field(j, "reason_key", value.reason_key);
    write_field(j, "detail", value.detail);
    validate_json(j, TypeTag<RestartAdviceDTO>{});
}

BRAURSER_READ_JSON(UpdateItem) {
    read_object(j, {"package","from_version","to_version","source","restart"}, extra);
    read_field<Materialize>(j, "package", out.package);
    read_field<Materialize>(j, "from_version", out.from_version);
    read_field<Materialize>(j, "to_version", out.to_version);
    read_field<Materialize>(j, "source", out.source);
    read_field<Materialize>(j, "restart", out.restart);
}

void to_json(nlohmann::json& j, const UpdateItem& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "from_version", value.from_version);
    write_field(j, "to_version", value.to_version);
    write_field(j, "source", value.source);
    write_field(j, "restart", value.restart);
    validate_json(j, TypeTag<UpdateItem>{});
}

BRAURSER_READ_JSON(UpdateReport) {
    read_object(j, {"report_id","checked_at","official","community","manual","news","overall_restart"}, extra);
    read_field<Materialize>(j, "report_id", out.report_id);
    read_field<Materialize>(j, "checked_at", out.checked_at);
    read_field<Materialize>(j, "official", out.official);
    read_field<Materialize>(j, "community", out.community);
    read_field<Materialize>(j, "manual", out.manual);
    read_field<Materialize>(j, "news", out.news);
    read_field<Materialize>(j, "overall_restart", out.overall_restart);
}

void to_json(nlohmann::json& j, const UpdateReport& value) {
    j = nlohmann::json::object();
    write_field(j, "report_id", value.report_id);
    write_field(j, "checked_at", value.checked_at);
    write_field(j, "official", value.official);
    write_field(j, "community", value.community);
    write_field(j, "manual", value.manual);
    write_field(j, "news", value.news);
    write_field(j, "overall_restart", value.overall_restart);
    validate_json(j, TypeTag<UpdateReport>{});
}

BRAURSER_READ_JSON(Replacement) {
    read_object(j, {"old","new"}, extra);
    read_field<Materialize>(j, "old", out.old);
    read_field<Materialize>(j, "new", out.new_);
}

void to_json(nlohmann::json& j, const Replacement& value) {
    j = nlohmann::json::object();
    write_field(j, "old", value.old);
    write_field(j, "new", value.new_);
    validate_json(j, TypeTag<Replacement>{});
}

BRAURSER_READ_JSON(Blocker) {
    read_object(j, {"code","message_key","args"}, extra);
    read_field<Materialize>(j, "code", out.code);
    read_field<Materialize>(j, "message_key", out.message_key);
    read_field<Materialize>(j, "args", out.args);
}

void to_json(nlohmann::json& j, const Blocker& value) {
    j = nlohmann::json::object();
    write_field(j, "code", value.code);
    write_field(j, "message_key", value.message_key);
    write_field(j, "args", value.args);
    validate_json(j, TypeTag<Blocker>{});
}

BRAURSER_READ_JSON(BuildPin) {
    read_object(j, {"pkgbase", "pkgnames", "commit"}, extra);
    read_field<Materialize>(j, "pkgbase", out.pkgbase);
    read_field<Materialize>(j, "pkgnames", out.pkgnames);
    read_field<Materialize>(j, "commit", out.commit);
    const auto& pkgbase = text(j, "pkgbase");
    const auto& commit = text(j, "commit");
    const auto& names = j.at("pkgnames");
    if (!br_valid_pkgname(pkgbase.data(), pkgbase.size()) || names.empty() || names.size() > 256 ||
        (commit.size() != 40 && commit.size() != 64) ||
        !std::all_of(commit.begin(), commit.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) throw std::invalid_argument("invalid build pin");
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& name = names[index].get_ref<const std::string&>();
        if (!br_valid_pkgname(name.data(), name.size())) throw std::invalid_argument("invalid build package");
        for (std::size_t other = 0; other < index; ++other)
            if (names[other] == names[index]) throw std::invalid_argument("invalid build package");
    }
}

void to_json(nlohmann::json& j, const BuildPin& value) {
    j = {{"pkgbase", value.pkgbase}, {"pkgnames", value.pkgnames}, {"commit", value.commit}};
    validate_json(j, TypeTag<BuildPin>{});
}

BRAURSER_READ_JSON(TransactionPlan) {
    read_object(j, {"plan_id","revision","approval_token","kind","installs","upgrades","removals","replacements","conflicts","download_bytes","install_delta_bytes","restart","requires_build_review","build_review_refs","build_pins","protected_blockers","blockers"}, extra);
    read_field<Materialize>(j, "plan_id", out.plan_id);
    read_field<Materialize>(j, "revision", out.revision);
    read_field<Materialize>(j, "approval_token", out.approval_token);
    read_field<Materialize>(j, "kind", out.kind);
    read_field<Materialize>(j, "installs", out.installs);
    read_field<Materialize>(j, "upgrades", out.upgrades);
    read_field<Materialize>(j, "removals", out.removals);
    read_field<Materialize>(j, "replacements", out.replacements);
    read_field<Materialize>(j, "conflicts", out.conflicts);
    read_field<Materialize>(j, "download_bytes", out.download_bytes);
    read_field<Materialize>(j, "install_delta_bytes", out.install_delta_bytes);
    read_field<Materialize>(j, "restart", out.restart);
    read_field<Materialize>(j, "requires_build_review", out.requires_build_review);
    read_field<Materialize>(j, "build_review_refs", out.build_review_refs);
    read_field<Materialize>(j, "build_pins", out.build_pins);
    if (j.at("build_pins").size() > 16) throw std::invalid_argument("too many build pins");
    for (std::size_t index = 0; index < j.at("build_pins").size(); ++index)
        for (std::size_t other = 0; other < index; ++other)
            if (j.at("build_pins")[index].at("pkgbase") == j.at("build_pins")[other].at("pkgbase"))
                throw std::invalid_argument("duplicate build pin");
    read_field<Materialize>(j, "protected_blockers", out.protected_blockers);
    read_field<Materialize>(j, "blockers", out.blockers);
}

void to_json(nlohmann::json& j, const TransactionPlan& value) {
    j = nlohmann::json::object();
    write_field(j, "plan_id", value.plan_id);
    write_field(j, "revision", value.revision);
    write_field(j, "approval_token", value.approval_token);
    write_field(j, "kind", value.kind);
    write_field(j, "installs", value.installs);
    write_field(j, "upgrades", value.upgrades);
    write_field(j, "removals", value.removals);
    write_field(j, "replacements", value.replacements);
    write_field(j, "conflicts", value.conflicts);
    write_field(j, "download_bytes", value.download_bytes);
    write_field(j, "install_delta_bytes", value.install_delta_bytes);
    write_field(j, "restart", value.restart);
    write_field(j, "requires_build_review", value.requires_build_review);
    write_field(j, "build_review_refs", value.build_review_refs);
    write_field(j, "build_pins", value.build_pins);
    write_field(j, "protected_blockers", value.protected_blockers);
    write_field(j, "blockers", value.blockers);
    validate_json(j, TypeTag<TransactionPlan>{});
}

BRAURSER_READ_JSON(Progress) {
    read_object(j, {"phase","current","total","unit"}, extra);
    read_field<Materialize>(j, "phase", out.phase);
    read_field<Materialize>(j, "current", out.current);
    read_field<Materialize>(j, "total", out.total);
    read_field<Materialize>(j, "unit", out.unit);
    if (present(j, "current") && present(j, "total") &&
        j.at("current").get<std::uint64_t>() > j.at("total").get<std::uint64_t>())
        throw std::invalid_argument("invalid progress");
}

void to_json(nlohmann::json& j, const Progress& value) {
    j = nlohmann::json::object();
    write_field(j, "phase", value.phase);
    write_field(j, "current", value.current);
    write_field(j, "total", value.total);
    write_field(j, "unit", value.unit);
    validate_json(j, TypeTag<Progress>{});
}

BRAURSER_READ_JSON(ProviderQuestion) {
    read_object(j, {"options"}, extra);
    read_field<Materialize>(j, "options", out.options);
}

void to_json(nlohmann::json& j, const ProviderQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "options", value.options);
    validate_json(j, TypeTag<ProviderQuestion>{});
}

BRAURSER_READ_JSON(BuildReviewQuestion) {
    read_object(j, {"refs","pkgbuild"}, extra);
    read_field<Materialize>(j, "refs", out.refs);
    read_field<Materialize>(j, "pkgbuild", out.pkgbuild);
    if (present(j, "pkgbuild") && (text(j, "pkgbuild").empty() || text(j, "pkgbuild").size() > 256 * 1024))
        throw std::invalid_argument("invalid build file");
}

void to_json(nlohmann::json& j, const BuildReviewQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "refs", value.refs);
    write_field(j, "pkgbuild", value.pkgbuild);
    validate_json(j, TypeTag<BuildReviewQuestion>{});
}

BRAURSER_READ_JSON(ConflictQuestion) {
    read_object(j, {"a","b"}, extra);
    read_field<Materialize>(j, "a", out.a);
    read_field<Materialize>(j, "b", out.b);
}

void to_json(nlohmann::json& j, const ConflictQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "a", value.a);
    write_field(j, "b", value.b);
    validate_json(j, TypeTag<ConflictQuestion>{});
}

BRAURSER_READ_JSON(NewsAckQuestion) {
    read_object(j, {"items"}, extra);
    read_field<Materialize>(j, "items", out.items);
}

void to_json(nlohmann::json& j, const NewsAckQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "items", value.items);
    validate_json(j, TypeTag<NewsAckQuestion>{});
}

BRAURSER_READ_JSON(GitRedirectQuestion) {
    read_object(j, {"from_host","to_host"}, extra);
    read_field<Materialize>(j, "from_host", out.from_host);
    read_field<Materialize>(j, "to_host", out.to_host);
}

void to_json(nlohmann::json& j, const GitRedirectQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "from_host", value.from_host);
    write_field(j, "to_host", value.to_host);
    validate_json(j, TypeTag<GitRedirectQuestion>{});
}

BRAURSER_READ_JSON(Question) {
    if (!j.is_object()) throw std::invalid_argument("invalid question");
    validate_wire(j);
    read_field<Materialize>(j, "question_id", out.question_id);
    read_field<Materialize>(j, "token", out.token);
    read_field<Materialize>(j, "expires_at", out.expires_at);
    static_cast<void>(extra);
    read_json<Materialize>(j, out.choice, {{"question_id", "token", "expires_at"}});
}

void to_json(nlohmann::json& j, const Question& value) {
    j = value.choice;
    write_field(j, "question_id", value.question_id);
    write_field(j, "token", value.token);
    write_field(j, "expires_at", value.expires_at);
    validate_json(j, TypeTag<Question>{});
}

BRAURSER_READ_JSON(Settings) {
    read_object(j, {"ui","helper","check_frequency","notifications","autostart","review_builds","show_log","theme"}, extra);
    read_field<Materialize>(j, "ui", out.ui);
    read_field<Materialize>(j, "helper", out.helper);
    read_field<Materialize>(j, "check_frequency", out.check_frequency);
    read_field<Materialize>(j, "notifications", out.notifications);
    read_field<Materialize>(j, "autostart", out.autostart);
    read_field<Materialize>(j, "review_builds", out.review_builds);
    read_field<Materialize>(j, "show_log", out.show_log);
    read_field<Materialize>(j, "theme", out.theme);
}

void to_json(nlohmann::json& j, const Settings& value) {
    j = nlohmann::json::object();
    write_field(j, "ui", value.ui);
    write_field(j, "helper", value.helper);
    write_field(j, "check_frequency", value.check_frequency);
    write_field(j, "notifications", value.notifications);
    write_field(j, "autostart", value.autostart);
    write_field(j, "review_builds", value.review_builds);
    write_field(j, "show_log", value.show_log);
    write_field(j, "theme", value.theme);
    validate_json(j, TypeTag<Settings>{});
}

BRAURSER_READ_JSON(SettingsPatch) {
    read_object(j, {"ui", "helper", "check_frequency", "notifications", "autostart", "review_builds", "show_log", "theme"}, extra);
    read_field<Materialize>(j, "ui", out.ui);
    read_field<Materialize>(j, "helper", out.helper);
    read_field<Materialize>(j, "check_frequency", out.check_frequency);
    read_field<Materialize>(j, "notifications", out.notifications);
    read_field<Materialize>(j, "autostart", out.autostart);
    read_field<Materialize>(j, "review_builds", out.review_builds);
    read_field<Materialize>(j, "show_log", out.show_log);
    read_field<Materialize>(j, "theme", out.theme);
}

void to_json(nlohmann::json& j, const SettingsPatch& value) {
    j = nlohmann::json::object();
    write_field(j, "ui", value.ui);
    write_field(j, "helper", value.helper);
    write_field(j, "check_frequency", value.check_frequency);
    write_field(j, "notifications", value.notifications);
    write_field(j, "autostart", value.autostart);
    write_field(j, "review_builds", value.review_builds);
    write_field(j, "show_log", value.show_log);
    write_field(j, "theme", value.theme);
    validate_json(j, TypeTag<SettingsPatch>{});
}

BRAURSER_READ_JSON(ManualPreview) {
    read_object(j, {"preview_id","host","owner_repo","commit","script_path","script_text","scope_options","external_refs","staging_supported"}, extra);
    read_field<Materialize>(j, "preview_id", out.preview_id);
    read_field<Materialize>(j, "host", out.host);
    read_field<Materialize>(j, "owner_repo", out.owner_repo);
    read_field<Materialize>(j, "commit", out.commit);
    read_field<Materialize>(j, "script_path", out.script_path);
    read_field<Materialize>(j, "script_text", out.script_text);
    read_field<Materialize>(j, "scope_options", out.scope_options);
    read_field<Materialize>(j, "external_refs", out.external_refs);
    read_field<Materialize>(j, "staging_supported", out.staging_supported);
}

void to_json(nlohmann::json& j, const ManualPreview& value) {
    j = nlohmann::json::object();
    write_field(j, "preview_id", value.preview_id);
    write_field(j, "host", value.host);
    write_field(j, "owner_repo", value.owner_repo);
    write_field(j, "commit", value.commit);
    write_field(j, "script_path", value.script_path);
    write_field(j, "script_text", value.script_text);
    write_field(j, "scope_options", value.scope_options);
    write_field(j, "external_refs", value.external_refs);
    write_field(j, "staging_supported", value.staging_supported);
    validate_json(j, TypeTag<ManualPreview>{});
}

BRAURSER_READ_JSON(ManualReceipt) {
    read_object(j, {"receipt_id","name","scope","commit","installed_at","launcher_ids","removable"}, extra);
    read_field<Materialize>(j, "receipt_id", out.receipt_id);
    read_field<Materialize>(j, "name", out.name);
    read_field<Materialize>(j, "scope", out.scope);
    read_field<Materialize>(j, "commit", out.commit);
    read_field<Materialize>(j, "installed_at", out.installed_at);
    read_field<Materialize>(j, "launcher_ids", out.launcher_ids);
    read_field<Materialize>(j, "removable", out.removable);
}

void to_json(nlohmann::json& j, const ManualReceipt& value) {
    j = nlohmann::json::object();
    write_field(j, "receipt_id", value.receipt_id);
    write_field(j, "name", value.name);
    write_field(j, "scope", value.scope);
    write_field(j, "commit", value.commit);
    write_field(j, "installed_at", value.installed_at);
    write_field(j, "launcher_ids", value.launcher_ids);
    write_field(j, "removable", value.removable);
    validate_json(j, TypeTag<ManualReceipt>{});
}

BRAURSER_READ_JSON(LauncherEntry) {
    read_object(j, {"desktop_id","name","exec_display","path","owner"}, extra);
    read_field<Materialize>(j, "desktop_id", out.desktop_id);
    read_field<Materialize>(j, "name", out.name);
    read_field<Materialize>(j, "exec_display", out.exec_display);
    read_field<Materialize>(j, "path", out.path);
    read_field<Materialize>(j, "owner", out.owner);
}

void to_json(nlohmann::json& j, const LauncherEntry& value) {
    j = nlohmann::json::object();
    write_field(j, "desktop_id", value.desktop_id);
    write_field(j, "name", value.name);
    write_field(j, "exec_display", value.exec_display);
    write_field(j, "path", value.path);
    write_field(j, "owner", value.owner);
    validate_json(j, TypeTag<LauncherEntry>{});
}

BRAURSER_READ_JSON(Capabilities) {
    read_object(j, {"helper","helper_version","elevation","init","distro_id","repos"}, extra);
    read_field<Materialize>(j, "helper", out.helper);
    read_field<Materialize>(j, "helper_version", out.helper_version);
    read_field<Materialize>(j, "elevation", out.elevation);
    read_field<Materialize>(j, "init", out.init);
    read_field<Materialize>(j, "distro_id", out.distro_id);
    read_field<Materialize>(j, "repos", out.repos);
    if (j.contains("repos")) for (const auto& item : j.at("repos")) {
        const auto& repo = item.get_ref<const std::string&>();
        if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
    }
}

void to_json(nlohmann::json& j, const Capabilities& value) {
    j = nlohmann::json::object();
    write_field(j, "helper", value.helper);
    write_field(j, "helper_version", value.helper_version);
    write_field(j, "elevation", value.elevation);
    write_field(j, "init", value.init);
    write_field(j, "distro_id", value.distro_id);
    write_field(j, "repos", value.repos);
    validate_json(j, TypeTag<Capabilities>{});
}

BRAURSER_READ_JSON(SystemStatus) {
    read_object(j, {"connectivity","catalog_age","active_job","elevation_method"}, extra);
    read_field<Materialize>(j, "connectivity", out.connectivity);
    read_field<Materialize>(j, "catalog_age", out.catalog_age);
    read_field<Materialize>(j, "active_job", out.active_job);
    read_field<Materialize>(j, "elevation_method", out.elevation_method);
}

void to_json(nlohmann::json& j, const SystemStatus& value) {
    j = nlohmann::json::object();
    write_field(j, "connectivity", value.connectivity);
    write_field(j, "catalog_age", value.catalog_age);
    write_field(j, "active_job", value.active_job);
    write_field(j, "elevation_method", value.elevation_method);
    validate_json(j, TypeTag<SystemStatus>{});
}

BRAURSER_READ_JSON(HelloParams) {
    read_object(j, {"apiVersion"}, extra);
    read_field<Materialize>(j, "apiVersion", out.apiVersion);
    if (j.at("apiVersion").get<std::uint64_t>() != 1) throw std::invalid_argument("invalid API version");
}

void to_json(nlohmann::json& j, const HelloParams& value) {
    j = nlohmann::json::object();
    write_field(j, "apiVersion", value.apiVersion);
    validate_json(j, TypeTag<HelloParams>{});
}

BRAURSER_READ_JSON(HelloResult) {
    read_object(j, {"serverVersion","apiVersion","helper","capabilities","uiBackends"}, extra);
    read_field<Materialize>(j, "serverVersion", out.serverVersion);
    read_field<Materialize>(j, "apiVersion", out.apiVersion);
    read_field<Materialize>(j, "helper", out.helper);
    read_field<Materialize>(j, "capabilities", out.capabilities);
    read_field<Materialize>(j, "uiBackends", out.uiBackends);
    if (j.at("apiVersion").get<std::uint64_t>() != 1) throw std::invalid_argument("invalid API version");
}

void to_json(nlohmann::json& j, const HelloResult& value) {
    j = nlohmann::json::object();
    write_field(j, "serverVersion", value.serverVersion);
    write_field(j, "apiVersion", value.apiVersion);
    write_field(j, "helper", value.helper);
    write_field(j, "capabilities", value.capabilities);
    write_field(j, "uiBackends", value.uiBackends);
    validate_json(j, TypeTag<HelloResult>{});
}

BRAURSER_READ_JSON(StatusParams) {
    static_cast<void>(out);
    read_object(j, {}, extra);
}

void to_json(nlohmann::json& j, const StatusParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    validate_json(j, TypeTag<StatusParams>{});
}

BRAURSER_READ_JSON(CatalogSourceStatus) {
    read_object(j, {"id","status","reasonCode","packageCount","skippedCount"}, extra);
    read_field<Materialize>(j, "id", out.id);
    read_field<Materialize>(j, "status", out.status);
    read_field<Materialize>(j, "reasonCode", out.reasonCode);
    read_field<Materialize>(j, "packageCount", out.packageCount);
    read_field<Materialize>(j, "skippedCount", out.skippedCount);
    if (text(j, "status") != "ok" && text(j, "status") != "unavailable" && text(j, "status") != "failed")
        throw std::invalid_argument("invalid source status");
}

void to_json(nlohmann::json& j, const CatalogSourceStatus& value) {
    j = {{"id", value.id}, {"status", value.status}, {"packageCount", value.packageCount}, {"skippedCount", value.skippedCount}};
    write_field(j, "reasonCode", value.reasonCode);
    validate_json(j, TypeTag<CatalogSourceStatus>{});
}

BRAURSER_READ_JSON(StatusResult) {
    read_object(j, {"connectivity","catalogAge","activeJob","elevationMethod","catalogState","catalogAgeSeconds","sources"}, extra);
    read_field<Materialize>(j, "connectivity", out.connectivity);
    read_field<Materialize>(j, "catalogAge", out.catalogAge);
    read_field<Materialize>(j, "activeJob", out.activeJob);
    read_field<Materialize>(j, "elevationMethod", out.elevationMethod);
    read_field<Materialize>(j, "catalogState", out.catalogState);
    read_field<Materialize>(j, "catalogAgeSeconds", out.catalogAgeSeconds);
    read_field<Materialize>(j, "sources", out.sources);
    if (text(j, "catalogState") != "missing" && text(j, "catalogState") != "refreshing" &&
        text(j, "catalogState") != "ready" && text(j, "catalogState") != "stale")
        throw std::invalid_argument("invalid catalog state");
}

void to_json(nlohmann::json& j, const StatusResult& value) {
    j = nlohmann::json::object();
    write_field(j, "connectivity", value.connectivity);
    write_field(j, "catalogAge", value.catalogAge);
    write_field(j, "activeJob", value.activeJob);
    write_field(j, "elevationMethod", value.elevationMethod);
    write_field(j, "catalogState", value.catalogState);
    write_field(j, "catalogAgeSeconds", value.catalogAgeSeconds);
    write_field(j, "sources", value.sources);
    validate_json(j, TypeTag<StatusResult>{});
}

BRAURSER_READ_JSON(SearchParams) {
    read_object(j, {"text","filters","cursor","limit","sort","category"}, extra);
    read_field<Materialize>(j, "text", out.text);
    read_field<Materialize>(j, "filters", out.filters);
    read_field<Materialize>(j, "cursor", out.cursor);
    read_field<Materialize>(j, "limit", out.limit);
    if (j.contains("sort")) read_field<Materialize>(j, "sort", out.sort);
    read_field<Materialize>(j, "category", out.category);
    if (j.at("limit").get<std::uint64_t>() == 0 || j.at("limit").get<std::uint64_t>() > 200 || text(j, "text").size() > 256 ||
        (present(j, "category") && (text(j, "category").empty() || text(j, "category").size() > 256)))
        throw std::invalid_argument("invalid search bounds");
}

void to_json(nlohmann::json& j, const SearchParams& value) {
    j = nlohmann::json::object();
    write_field(j, "text", value.text);
    write_field(j, "filters", value.filters);
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    write_field(j, "sort", value.sort);
    write_field(j, "category", value.category);
    validate_json(j, TypeTag<SearchParams>{});
}

BRAURSER_READ_JSON(SearchResult) {
    read_object(j, {"packages","nextCursor","stale","catalogState"}, extra);
    read_field<Materialize>(j, "packages", out.packages);
    read_field<Materialize>(j, "nextCursor", out.nextCursor);
    read_field<Materialize>(j, "stale", out.stale);
    read_field<Materialize>(j, "catalogState", out.catalogState);
    if (present(j, "catalogState") && text(j, "catalogState") != "refreshing" && text(j, "catalogState") != "ready" && text(j, "catalogState") != "stale")
        throw std::invalid_argument("invalid catalog state");
}

void to_json(nlohmann::json& j, const SearchResult& value) {
    j = nlohmann::json::object();
    write_field(j, "packages", value.packages);
    write_field(j, "nextCursor", value.nextCursor);
    write_field(j, "stale", value.stale);
    write_field(j, "catalogState", value.catalogState);
    validate_json(j, TypeTag<SearchResult>{});
}

BRAURSER_READ_JSON(GetParams) {
    read_object(j, {"packageId"}, extra);
    read_field<Materialize>(j, "packageId", out.packageId);
}

void to_json(nlohmann::json& j, const GetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "packageId", value.packageId);
    validate_json(j, TypeTag<GetParams>{});
}

BRAURSER_READ_JSON(GetResult) {
    read_object(j, {"details","installedState","source","appMetadata"}, extra);
    read_field<Materialize>(j, "details", out.details);
    read_field<Materialize>(j, "installedState", out.installedState);
    read_field<Materialize>(j, "source", out.source);
    read_field<Materialize>(j, "appMetadata", out.appMetadata);
}

void to_json(nlohmann::json& j, const GetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "details", value.details);
    write_field(j, "installedState", value.installedState);
    write_field(j, "source", value.source);
    write_field(j, "appMetadata", value.appMetadata);
    validate_json(j, TypeTag<GetResult>{});
}

BRAURSER_READ_JSON(VariantsParams) {
    read_object(j, {"packageId"}, extra);
    read_field<Materialize>(j, "packageId", out.packageId);
}

void to_json(nlohmann::json& j, const VariantsParams& value) {
    j = nlohmann::json::object();
    write_field(j, "packageId", value.packageId);
    validate_json(j, TypeTag<VariantsParams>{});
}

BRAURSER_READ_JSON(VariantsResult) {
    read_object(j, {"choices","questionRequired"}, extra);
    read_field<Materialize>(j, "choices", out.choices);
    read_field<Materialize>(j, "questionRequired", out.questionRequired);
}

void to_json(nlohmann::json& j, const VariantsResult& value) {
    j = nlohmann::json::object();
    write_field(j, "choices", value.choices);
    write_field(j, "questionRequired", value.questionRequired);
    validate_json(j, TypeTag<VariantsResult>{});
}

BRAURSER_READ_JSON(FacetsParams) { static_cast<void>(out); read_object(j, {}, extra); }
void to_json(nlohmann::json& j, const FacetsParams&) { j = nlohmann::json::object(); }

BRAURSER_READ_JSON(FacetCount) {
    read_object(j, {"name","packageCount"}, extra);
    read_field<Materialize>(j, "name", out.name);
    read_field<Materialize>(j, "packageCount", out.packageCount);
}
void to_json(nlohmann::json& j, const FacetCount& value) {
    j = {{"name", value.name}, {"packageCount", value.packageCount}};
    validate_json(j, TypeTag<FacetCount>{});
}
BRAURSER_READ_JSON(CategoryCount) {
    read_object(j, {"id","packageCount"}, extra);
    read_field<Materialize>(j, "id", out.id);
    read_field<Materialize>(j, "packageCount", out.packageCount);
}
void to_json(nlohmann::json& j, const CategoryCount& value) {
    j = {{"id", value.id}, {"packageCount", value.packageCount}};
    validate_json(j, TypeTag<CategoryCount>{});
}
BRAURSER_READ_JSON(FacetsResult) {
    read_object(j, {"repos","groups","categories"}, extra);
    read_field<Materialize>(j, "repos", out.repos);
    read_field<Materialize>(j, "groups", out.groups);
    read_field<Materialize>(j, "categories", out.categories);
}
void to_json(nlohmann::json& j, const FacetsResult& value) {
    j = {{"repos", value.repos}, {"groups", value.groups}, {"categories", value.categories}};
    validate_json(j, TypeTag<FacetsResult>{});
}
namespace {
bool build_path(std::string_view path) {
    if (path.empty() || path.size() > 1024 || path.back() == '/' || path.find('\\') != path.npos) return false;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part == "." || part == ".." || !br_valid_path_component(part.data(), part.size())) return false;
        if (slash == path.npos) break;
        path.remove_prefix(slash + 1);
    }
    return true;
}
}

BRAURSER_READ_JSON(BuildFilesParams) {
    read_object(j, {"packageId", "commit", "path"}, extra);
    read_field<Materialize>(j, "packageId", out.packageId);
    if constexpr (Materialize) { out.commit.reset(); out.path.reset(); }
    read_field<Materialize>(j, "commit", out.commit);
    read_field<Materialize>(j, "path", out.path);
    if (present(j, "path") && (!present(j, "commit") || !build_path(text(j, "path"))))
        throw std::invalid_argument("invalid build path");
    if (present(j, "commit") && ((text(j, "commit").size() != 40 && text(j, "commit").size() != 64) ||
        !std::all_of(text(j, "commit").begin(), text(j, "commit").end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }))) throw std::invalid_argument("invalid build commit");
}
void to_json(nlohmann::json& j, const BuildFilesParams& value) {
    j = {{"packageId", value.packageId}};
    write_field(j, "commit", value.commit);
    write_field(j, "path", value.path);
    validate_json(j, TypeTag<BuildFilesParams>{});
}
BRAURSER_READ_JSON(BuildFile) {
    read_object(j, {"name","text","kind","size","sha256"}, extra);
    read_field<Materialize>(j, "name", out.name);
    read_field<Materialize>(j, "text", out.text);
    read_field<Materialize>(j, "kind", out.kind);
    read_field<Materialize>(j, "size", out.size);
    read_field<Materialize>(j, "sha256", out.sha256);
    const auto& kind = text(j, "kind");
    const auto size = j.at("size").get<std::uint64_t>();
    const auto& digest = text(j, "sha256");
    if (!build_path(text(j, "name")) || (kind != "text" && kind != "binary" && kind != "omitted") ||
        size > 4 * 1024 * 1024 || digest.size() != 64 ||
        !std::all_of(digest.begin(), digest.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        }) || (kind == "text" ? text(j, "text").size() != size : !text(j, "text").empty()) ||
        (kind != "binary" && size > 448 * 1024)) throw std::invalid_argument("invalid build file");
}
void to_json(nlohmann::json& j, const BuildFile& value) {
    j = {{"name", value.name}, {"text", value.text}, {"kind", value.kind}, {"size", value.size}, {"sha256", value.sha256}};
    validate_json(j, TypeTag<BuildFile>{});
}
BRAURSER_READ_JSON(BuildFilesResult) {
    read_object(j, {"packageBase","files","truncated"}, extra);
    read_field<Materialize>(j, "packageBase", out.packageBase);
    read_field<Materialize>(j, "files", out.files);
    read_field<Materialize>(j, "truncated", out.truncated);
    const auto& base = text(j, "packageBase");
    if (!br_valid_pkgname(base.data(), base.size())) throw std::invalid_argument("invalid build base");
    const auto& files = j.at("files");
    for (auto file = files.begin(); file != files.end(); ++file) {
        for (auto previous = files.begin(); previous != file; ++previous) {
            if (text(*file, "name") == text(*previous, "name")) throw std::invalid_argument("duplicate build file");
        }
    }
}
void to_json(nlohmann::json& j, const BuildFilesResult& value) {
    j = {{"packageBase", value.packageBase}, {"files", value.files}, {"truncated", value.truncated}};
    validate_json(j, TypeTag<BuildFilesResult>{});
}

BRAURSER_READ_JSON(RefreshParams) {
    read_object(j, {"sources"}, extra);
    read_field<Materialize>(j, "sources", out.sources);
}

void to_json(nlohmann::json& j, const RefreshParams& value) {
    j = nlohmann::json::object();
    write_field(j, "sources", value.sources);
    validate_json(j, TypeTag<RefreshParams>{});
}

BRAURSER_READ_JSON(JobIdResult) {
    read_object(j, {"jobId"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
}

void to_json(nlohmann::json& j, const JobIdResult& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    validate_json(j, TypeTag<JobIdResult>{});
}

BRAURSER_READ_JSON(WikiGetParams) {
    read_object(j, {"packageId","allowRefresh"}, extra);
    read_field<Materialize>(j, "packageId", out.packageId);
    read_field<Materialize>(j, "allowRefresh", out.allowRefresh);
}

void to_json(nlohmann::json& j, const WikiGetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "packageId", value.packageId);
    write_field(j, "allowRefresh", value.allowRefresh);
    validate_json(j, TypeTag<WikiGetParams>{});
}

BRAURSER_READ_JSON(WikiGetResult) {
    read_object(j, {"document","sourceUrl","attribution","fetchedAt","stale"}, extra);
    read_field<Materialize>(j, "document", out.document);
    read_field<Materialize>(j, "sourceUrl", out.sourceUrl);
    read_field<Materialize>(j, "attribution", out.attribution);
    read_field<Materialize>(j, "fetchedAt", out.fetchedAt);
    read_field<Materialize>(j, "stale", out.stale);
}

void to_json(nlohmann::json& j, const WikiGetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "document", value.document);
    write_field(j, "sourceUrl", value.sourceUrl);
    write_field(j, "attribution", value.attribution);
    write_field(j, "fetchedAt", value.fetchedAt);
    write_field(j, "stale", value.stale);
    validate_json(j, TypeTag<WikiGetResult>{});
}

BRAURSER_READ_JSON(NewsListParams) {
    read_object(j, {"cursor","limit"}, extra);
    read_field<Materialize>(j, "cursor", out.cursor);
    read_field<Materialize>(j, "limit", out.limit);
    if (j.at("limit").get<std::uint64_t>() == 0 || j.at("limit").get<std::uint64_t>() > 200) throw std::invalid_argument("invalid limit");
}

void to_json(nlohmann::json& j, const NewsListParams& value) {
    j = nlohmann::json::object();
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    validate_json(j, TypeTag<NewsListParams>{});
}

BRAURSER_READ_JSON(NewsListResult) {
    read_object(j, {"items","nextCursor","stale"}, extra);
    read_field<Materialize>(j, "items", out.items);
    read_field<Materialize>(j, "nextCursor", out.nextCursor);
    read_field<Materialize>(j, "stale", out.stale);
}

void to_json(nlohmann::json& j, const NewsListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "items", value.items);
    write_field(j, "nextCursor", value.nextCursor);
    write_field(j, "stale", value.stale);
    validate_json(j, TypeTag<NewsListResult>{});
}

BRAURSER_READ_JSON(UpdatesCheckParams) {
    read_object(j, {"refresh"}, extra);
    read_field<Materialize>(j, "refresh", out.refresh);
}

void to_json(nlohmann::json& j, const UpdatesCheckParams& value) {
    j = nlohmann::json::object();
    write_field(j, "refresh", value.refresh);
    validate_json(j, TypeTag<UpdatesCheckParams>{});
}

BRAURSER_READ_JSON(UpdatesOptionsParams) {
    read_object(j, {"reportId"}, extra);
    read_field<Materialize>(j, "reportId", out.reportId);
}

void to_json(nlohmann::json& j, const UpdatesOptionsParams& value) {
    j = nlohmann::json::object();
    write_field(j, "reportId", value.reportId);
    validate_json(j, TypeTag<UpdatesOptionsParams>{});
}

BRAURSER_READ_JSON(UpdatesOptionsResult) {
    read_object(j, {"officialAction","communityIndividualIds","manualIndividualIds"}, extra);
    read_field<Materialize>(j, "officialAction", out.officialAction);
    read_field<Materialize>(j, "communityIndividualIds", out.communityIndividualIds);
    read_field<Materialize>(j, "manualIndividualIds", out.manualIndividualIds);
    if (text(j, "officialAction") != "update-system") throw std::invalid_argument("invalid action");
}

void to_json(nlohmann::json& j, const UpdatesOptionsResult& value) {
    j = nlohmann::json::object();
    write_field(j, "officialAction", value.officialAction);
    write_field(j, "communityIndividualIds", value.communityIndividualIds);
    write_field(j, "manualIndividualIds", value.manualIndividualIds);
    validate_json(j, TypeTag<UpdatesOptionsResult>{});
}

BRAURSER_READ_JSON(PlanSelection) {
    read_object(j, {"kind","id"}, extra);
    read_field<Materialize>(j, "kind", out.kind);
    read_field<Materialize>(j, "id", out.id);
}

void to_json(nlohmann::json& j, const PlanSelection& value) {
    j = nlohmann::json::object();
    write_field(j, "kind", value.kind);
    write_field(j, "id", value.id);
    validate_json(j, TypeTag<PlanSelection>{});
}

BRAURSER_READ_JSON(UpdatesPlanParams) {
    read_object(j, {"reportId","selection","alsoInstallPackageId"}, extra);
    read_field<Materialize>(j, "reportId", out.reportId);
    read_field<Materialize>(j, "selection", out.selection);
    read_field<Materialize>(j, "alsoInstallPackageId", out.alsoInstallPackageId);
}

void to_json(nlohmann::json& j, const UpdatesPlanParams& value) {
    j = nlohmann::json::object();
    write_field(j, "reportId", value.reportId);
    write_field(j, "selection", value.selection);
    write_field(j, "alsoInstallPackageId", value.alsoInstallPackageId);
    validate_json(j, TypeTag<UpdatesPlanParams>{});
}

BRAURSER_READ_JSON(UpdatesPlanResult) {
    read_object(j, {"planId","transactions","restartAdvice","requiresApproval","blockers"}, extra);
    read_field<Materialize>(j, "planId", out.planId);
    read_field<Materialize>(j, "transactions", out.transactions);
    read_field<Materialize>(j, "restartAdvice", out.restartAdvice);
    read_field<Materialize>(j, "requiresApproval", out.requiresApproval);
    read_field<Materialize>(j, "blockers", out.blockers);
}

void to_json(nlohmann::json& j, const UpdatesPlanResult& value) {
    j = nlohmann::json::object();
    write_field(j, "planId", value.planId);
    write_field(j, "transactions", value.transactions);
    write_field(j, "restartAdvice", value.restartAdvice);
    write_field(j, "requiresApproval", value.requiresApproval);
    write_field(j, "blockers", value.blockers);
    validate_json(j, TypeTag<UpdatesPlanResult>{});
}

BRAURSER_READ_JSON(PlanParams) {
    read_object(j, {"kind","packageId","variantId","previewId","receiptId","scope","helper"}, extra);
    read_field<Materialize>(j, "kind", out.kind);
    read_field<Materialize>(j, "packageId", out.packageId);
    read_field<Materialize>(j, "variantId", out.variantId);
    read_field<Materialize>(j, "previewId", out.previewId);
    read_field<Materialize>(j, "receiptId", out.receiptId);
    read_field<Materialize>(j, "scope", out.scope);
    read_field<Materialize>(j, "helper", out.helper);
    const bool package = text(j, "kind") == "install" || text(j, "kind") == "remove" || text(j, "kind") == "update-community" || text(j, "kind") == "update-and-install";
    const bool manual = text(j, "kind") == "manual-install" || text(j, "kind") == "manual-upgrade" || text(j, "kind") == "manual-remove";
    if (!package && !manual && text(j, "kind") != "update-system" && text(j, "kind") != "setup-helper")
        throw std::invalid_argument("invalid plan kind");
    if ((package && !present(j, "packageId")) || ((text(j, "kind") == "manual-install" || text(j, "kind") == "manual-upgrade") && !present(j, "previewId")) ||
        ((text(j, "kind") == "manual-upgrade" || text(j, "kind") == "manual-remove") && !present(j, "receiptId")) ||
        (text(j, "kind") == "setup-helper" && (!present(j, "helper") || text(j, "helper") == "auto")) ||
        (!package && present(j, "packageId")) || (text(j, "kind") != "install" && present(j, "variantId")) ||
        (!manual && (present(j, "previewId") || present(j, "receiptId") || present(j, "scope"))) || (text(j, "kind") != "setup-helper" && present(j, "helper")))
        throw std::invalid_argument("invalid plan selection");
}

void to_json(nlohmann::json& j, const PlanParams& value) {
    j = {{"kind", value.kind}};
    write_field(j, "packageId", value.packageId);
    write_field(j, "variantId", value.variantId);
    write_field(j, "previewId", value.previewId);
    write_field(j, "receiptId", value.receiptId);
    write_field(j, "scope", value.scope);
    write_field(j, "helper", value.helper);
    validate_json(j, TypeTag<PlanParams>{});
}

BRAURSER_READ_JSON(InstallParams) {
    read_object(j, {"planId","approvalToken"}, extra);
    read_field<Materialize>(j, "planId", out.planId);
    read_field<Materialize>(j, "approvalToken", out.approvalToken);
}

void to_json(nlohmann::json& j, const InstallParams& value) {
    j = {{"planId", value.planId}, {"approvalToken", value.approvalToken}};
    validate_json(j, TypeTag<InstallParams>{});
}

#define BRAURSER_APPROVAL_JSON(type) \
BRAURSER_READ_JSON(type) { read_json<Materialize>(j, static_cast<InstallParams&>(out), extra); } \
void to_json(nlohmann::json& j, const type& value) { to_json(j, static_cast<const InstallParams&>(value)); }
BRAURSER_APPROVAL_JSON(RemoveParams)
BRAURSER_APPROVAL_JSON(UpgradeSystemParams)
BRAURSER_APPROVAL_JSON(UpgradeCommunityParams)
BRAURSER_APPROVAL_JSON(ManualInstallParams)
BRAURSER_APPROVAL_JSON(ManualUpgradeParams)
BRAURSER_APPROVAL_JSON(ManualRemoveParams)
BRAURSER_APPROVAL_JSON(SetupHelperParams)
#undef BRAURSER_APPROVAL_JSON

BRAURSER_READ_JSON(JobEvent) {
    read_object(j, {"sequence","kind","payload"}, extra);
    read_field<Materialize>(j, "sequence", out.sequence);
    if (j.at("sequence").get<std::uint64_t>() == 0) throw std::invalid_argument("invalid event sequence");
    read_field<Materialize>(j, "kind", out.kind);
    read_field<Materialize>(j, "payload", out.payload);
}

void to_json(nlohmann::json& j, const JobEvent& value) {
    j = nlohmann::json::object();
    write_field(j, "sequence", value.sequence);
    write_field(j, "kind", value.kind);
    write_field(j, "payload", value.payload);
    validate_json(j, TypeTag<JobEvent>{});
}

BRAURSER_READ_JSON(JobsGetParams) {
    read_object(j, {"jobId","eventsCursor"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "eventsCursor", out.eventsCursor);
}

void to_json(nlohmann::json& j, const JobsGetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "eventsCursor", value.eventsCursor);
    validate_json(j, TypeTag<JobsGetParams>{});
}

BRAURSER_READ_JSON(JobsGetResult) {
    read_object(j, {"state","plan","progress","result","error","events","nextCursor"}, extra);
    read_field<Materialize>(j, "state", out.state);
    read_field<Materialize>(j, "plan", out.plan);
    read_field<Materialize>(j, "progress", out.progress);
    read_field<Materialize>(j, "result", out.result);
    read_field<Materialize>(j, "error", out.error);
    read_field<Materialize>(j, "events", out.events);
    read_field<Materialize>(j, "nextCursor", out.nextCursor);
}

void to_json(nlohmann::json& j, const JobsGetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "state", value.state);
    write_field(j, "plan", value.plan);
    write_field(j, "progress", value.progress);
    write_field(j, "result", value.result);
    write_field(j, "error", value.error);
    write_field(j, "events", value.events);
    write_field(j, "nextCursor", value.nextCursor);
    validate_json(j, TypeTag<JobsGetResult>{});
}

BRAURSER_READ_JSON(JobsListParams) {
    read_object(j, {"cursor","limit"}, extra);
    read_field<Materialize>(j, "cursor", out.cursor);
    read_field<Materialize>(j, "limit", out.limit);
    if (j.at("limit").get<std::uint64_t>() == 0 || j.at("limit").get<std::uint64_t>() > 200) throw std::invalid_argument("invalid limit");
}

void to_json(nlohmann::json& j, const JobsListParams& value) {
    j = nlohmann::json::object();
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    validate_json(j, TypeTag<JobsListParams>{});
}

BRAURSER_READ_JSON(JobSummary) {
    read_object(j, {"jobId","state"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "state", out.state);
}

void to_json(nlohmann::json& j, const JobSummary& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "state", value.state);
    validate_json(j, TypeTag<JobSummary>{});
}

BRAURSER_READ_JSON(JobsListResult) {
    read_object(j, {"jobs","nextCursor"}, extra);
    read_field<Materialize>(j, "jobs", out.jobs);
    read_field<Materialize>(j, "nextCursor", out.nextCursor);
}

void to_json(nlohmann::json& j, const JobsListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "jobs", value.jobs);
    write_field(j, "nextCursor", value.nextCursor);
    validate_json(j, TypeTag<JobsListResult>{});
}

BRAURSER_READ_JSON(JobsCancelParams) {
    read_object(j, {"jobId"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
}

void to_json(nlohmann::json& j, const JobsCancelParams& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    validate_json(j, TypeTag<JobsCancelParams>{});
}

BRAURSER_READ_JSON(JobsCancelResult) {
    read_object(j, {"accepted","reason"}, extra);
    read_field<Materialize>(j, "accepted", out.accepted);
    read_field<Materialize>(j, "reason", out.reason);
}

void to_json(nlohmann::json& j, const JobsCancelResult& value) {
    j = nlohmann::json::object();
    write_field(j, "accepted", value.accepted);
    write_field(j, "reason", value.reason);
    validate_json(j, TypeTag<JobsCancelResult>{});
}

BRAURSER_READ_JSON(JobsAnswerParams) {
    read_object(j, {"jobId","questionId","choiceId","questionToken"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "questionId", out.questionId);
    read_field<Materialize>(j, "choiceId", out.choiceId);
    read_field<Materialize>(j, "questionToken", out.questionToken);
}

void to_json(nlohmann::json& j, const JobsAnswerParams& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "questionId", value.questionId);
    write_field(j, "choiceId", value.choiceId);
    write_field(j, "questionToken", value.questionToken);
    validate_json(j, TypeTag<JobsAnswerParams>{});
}

BRAURSER_READ_JSON(AcceptedResult) {
    read_object(j, {"accepted"}, extra);
    read_field<Materialize>(j, "accepted", out.accepted);
}

void to_json(nlohmann::json& j, const AcceptedResult& value) {
    j = nlohmann::json::object();
    write_field(j, "accepted", value.accepted);
    validate_json(j, TypeTag<AcceptedResult>{});
}

BRAURSER_READ_JSON(ManualPreviewParams) {
    read_object(j, {"httpsUrl","scriptPath","scope"}, extra);
    read_field<Materialize>(j, "httpsUrl", out.httpsUrl);
    read_field<Materialize>(j, "scriptPath", out.scriptPath);
    read_field<Materialize>(j, "scope", out.scope);
}

void to_json(nlohmann::json& j, const ManualPreviewParams& value) {
    j = nlohmann::json::object();
    write_field(j, "httpsUrl", value.httpsUrl);
    write_field(j, "scriptPath", value.scriptPath);
    write_field(j, "scope", value.scope);
    validate_json(j, TypeTag<ManualPreviewParams>{});
}

BRAURSER_READ_JSON(ManualListParams) {
    static_cast<void>(out);
    read_object(j, {}, extra);
}

void to_json(nlohmann::json& j, const ManualListParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    validate_json(j, TypeTag<ManualListParams>{});
}

BRAURSER_READ_JSON(ManualListResult) {
    read_object(j, {"receipts"}, extra);
    read_field<Materialize>(j, "receipts", out.receipts);
}

void to_json(nlohmann::json& j, const ManualListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "receipts", value.receipts);
    validate_json(j, TypeTag<ManualListResult>{});
}

BRAURSER_READ_JSON(JobsDequeueParams) {
    read_object(j, {"jobId"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
}
void to_json(nlohmann::json& j, const JobsDequeueParams& value) {
    j = {{"jobId", value.jobId}};
    validate_json(j, TypeTag<JobsDequeueParams>{});
}
BRAURSER_READ_JSON(JobsDequeueResult) {
    read_object(j, {"removed"}, extra);
    read_field<Materialize>(j, "removed", out.removed);
}
void to_json(nlohmann::json& j, const JobsDequeueResult& value) {
    j = {{"removed", value.removed}};
}

BRAURSER_READ_JSON(UnmanagedListParams) {
    static_cast<void>(out);
    read_object(j, {}, extra);
}

void to_json(nlohmann::json& j, const UnmanagedListParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    validate_json(j, TypeTag<UnmanagedListParams>{});
}

BRAURSER_READ_JSON(UnmanagedListResult) {
    read_object(j, {"entries","scanTime"}, extra);
    read_field<Materialize>(j, "entries", out.entries);
    read_field<Materialize>(j, "scanTime", out.scanTime);
}

void to_json(nlohmann::json& j, const UnmanagedListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "entries", value.entries);
    write_field(j, "scanTime", value.scanTime);
    validate_json(j, TypeTag<UnmanagedListResult>{});
}

BRAURSER_READ_JSON(SettingsGetParams) {
    static_cast<void>(out);
    read_object(j, {}, extra);
}

void to_json(nlohmann::json& j, const SettingsGetParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    validate_json(j, TypeTag<SettingsGetParams>{});
}

BRAURSER_READ_JSON(SettingsGetResult) {
    read_object(j, {"uiPreference","notifications","refreshPolicy","elevationPreference"}, extra);
    read_field<Materialize>(j, "uiPreference", out.uiPreference);
    read_field<Materialize>(j, "notifications", out.notifications);
    read_field<Materialize>(j, "refreshPolicy", out.refreshPolicy);
    read_field<Materialize>(j, "elevationPreference", out.elevationPreference);
}

void to_json(nlohmann::json& j, const SettingsGetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "uiPreference", value.uiPreference);
    write_field(j, "notifications", value.notifications);
    write_field(j, "refreshPolicy", value.refreshPolicy);
    write_field(j, "elevationPreference", value.elevationPreference);
    validate_json(j, TypeTag<SettingsGetResult>{});
}

BRAURSER_READ_JSON(SettingsSetParams) {
    read_object(j, {"changedFields"}, extra);
    read_field<Materialize>(j, "changedFields", out.changedFields);
}

void to_json(nlohmann::json& j, const SettingsSetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "changedFields", value.changedFields);
    validate_json(j, TypeTag<SettingsSetParams>{});
}

BRAURSER_READ_JSON(SettingsSetResult) {
    read_object(j, {"settings"}, extra);
    read_field<Materialize>(j, "settings", out.settings);
}

void to_json(nlohmann::json& j, const SettingsSetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "settings", value.settings);
    validate_json(j, TypeTag<SettingsSetResult>{});
}

BRAURSER_READ_JSON(ClearCacheParams) { static_cast<void>(out); read_object(j, {}, extra); }
void to_json(nlohmann::json& j, const ClearCacheParams&) { j = nlohmann::json::object(); }
BRAURSER_READ_JSON(ClearCacheResult) {
    read_object(j, {"freedBytes"}, extra);
    read_field<Materialize>(j, "freedBytes", out.freedBytes);
}
void to_json(nlohmann::json& j, const ClearCacheResult& value) { j = {{"freedBytes", value.freedBytes}}; }
BRAURSER_READ_JSON(RestartStateParams) { static_cast<void>(out); read_object(j, {}, extra); }
void to_json(nlohmann::json& j, const RestartStateParams&) { j = nlohmann::json::object(); }
BRAURSER_READ_JSON(RestartAcknowledgeParams) { static_cast<void>(out); read_object(j, {}, extra); }
void to_json(nlohmann::json& j, const RestartAcknowledgeParams&) { j = nlohmann::json::object(); }
BRAURSER_READ_JSON(RestartStateResult) {
    read_object(j, {"level","reason_keys","since"}, extra);
    read_field<Materialize>(j, "level", out.level);
    read_field<Materialize>(j, "reason_keys", out.reason_keys);
    read_field<Materialize>(j, "since", out.since);
    if (j.at("reason_keys").size() > 64) throw std::invalid_argument("too many restart reasons");
    for (const auto& key : j.at("reason_keys")) validate_wire(key, "reason_key");
    if (present(j, "since")) validate_wire(j.at("since"), "since");
}
void to_json(nlohmann::json& j, const RestartStateResult& value) {
    j = {{"level", value.level}, {"reason_keys", value.reason_keys}};
    write_field(j, "since", value.since);
    validate_json(j, TypeTag<RestartStateResult>{});
}
BRAURSER_READ_JSON(EmptyResult) { static_cast<void>(out); read_object(j, {}, extra); }
void to_json(nlohmann::json& j, const EmptyResult&) { j = nlohmann::json::object(); }
BRAURSER_READ_JSON(HelperStatus) {
    read_object(j, {"installed","version"}, extra);
    read_field<Materialize>(j, "installed", out.installed);
    read_field<Materialize>(j, "version", out.version);
    if (!j.at("installed").get<bool>() && present(j, "version")) throw std::invalid_argument("invalid helper status");
}
void to_json(nlohmann::json& j, const HelperStatus& value) {
    j = {{"installed", value.installed}};
    write_field(j, "version", value.version);
    validate_json(j, TypeTag<HelperStatus>{});
}
BRAURSER_READ_JSON(SetupStatusParams) { static_cast<void>(out); read_object(j, {}, extra); }
void to_json(nlohmann::json& j, const SetupStatusParams&) { j = nlohmann::json::object(); }
BRAURSER_READ_JSON(SetupStatusResult) {
    read_object(j, {"paru","yay","chosen","elevation","notes"}, extra);
    read_field<Materialize>(j, "paru", out.paru);
    read_field<Materialize>(j, "yay", out.yay);
    read_field<Materialize>(j, "chosen", out.chosen);
    read_field<Materialize>(j, "elevation", out.elevation);
    read_field<Materialize>(j, "notes", out.notes);
}
void to_json(nlohmann::json& j, const SetupStatusResult& value) {
    j = {{"paru", value.paru}, {"yay", value.yay}, {"chosen", value.chosen},
         {"elevation", value.elevation}, {"notes", value.notes}};
    validate_json(j, TypeTag<SetupStatusResult>{});
}

BRAURSER_READ_JSON(NotifierCheckParams) {
    static_cast<void>(out);
    read_object(j, {}, extra);
}

void to_json(nlohmann::json& j, const NotifierCheckParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    validate_json(j, TypeTag<NotifierCheckParams>{});
}

BRAURSER_READ_JSON(NotifierCheckResult) {
    read_object(j, {"updateCount","notificationSent","reason"}, extra);
    read_field<Materialize>(j, "updateCount", out.updateCount);
    read_field<Materialize>(j, "notificationSent", out.notificationSent);
    read_field<Materialize>(j, "reason", out.reason);
}

void to_json(nlohmann::json& j, const NotifierCheckResult& value) {
    j = nlohmann::json::object();
    write_field(j, "updateCount", value.updateCount);
    write_field(j, "notificationSent", value.notificationSent);
    write_field(j, "reason", value.reason);
    validate_json(j, TypeTag<NotifierCheckResult>{});
}

BRAURSER_READ_JSON(JobProgressNotification) {
    read_object(j, {"jobId", "sequence", "phase", "current", "total", "unit"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "sequence", out.sequence);
    if (j.at("sequence").get<std::uint64_t>() == 0) throw std::invalid_argument("invalid event sequence");
    read_json<Materialize>(j, out.progress, {{"jobId", "sequence"}});
}

void to_json(nlohmann::json& j, const JobProgressNotification& value) {
    j = value.progress;
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    validate_json(j, TypeTag<JobProgressNotification>{});
}

BRAURSER_READ_JSON(JobLogNotification) {
    read_object(j, {"jobId","sequence","line"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "sequence", out.sequence);
    if (j.at("sequence").get<std::uint64_t>() == 0) throw std::invalid_argument("invalid event sequence");
    read_field<Materialize>(j, "line", out.line);
    if (text(j, "line").size() > 4096) throw std::invalid_argument("log line too long");
}

void to_json(nlohmann::json& j, const JobLogNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "line", value.line);
    validate_json(j, TypeTag<JobLogNotification>{});
}

BRAURSER_READ_JSON(JobQuestionNotification) {
    read_object(j, {"jobId","sequence","question"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "sequence", out.sequence);
    if (j.at("sequence").get<std::uint64_t>() == 0) throw std::invalid_argument("invalid event sequence");
    read_field<Materialize>(j, "question", out.question);
}

void to_json(nlohmann::json& j, const JobQuestionNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "question", value.question);
    validate_json(j, TypeTag<JobQuestionNotification>{});
}

BRAURSER_READ_JSON(JobAuthStateNotification) {
    read_object(j, {"jobId","sequence","state"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "sequence", out.sequence);
    if (j.at("sequence").get<std::uint64_t>() == 0) throw std::invalid_argument("invalid event sequence");
    read_field<Materialize>(j, "state", out.state);
}

void to_json(nlohmann::json& j, const JobAuthStateNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "state", value.state);
    validate_json(j, TypeTag<JobAuthStateNotification>{});
}

BRAURSER_READ_JSON(JobStateNotification) {
    read_object(j, {"jobId","sequence","state"}, extra);
    read_field<Materialize>(j, "jobId", out.jobId);
    read_field<Materialize>(j, "sequence", out.sequence);
    if (j.at("sequence").get<std::uint64_t>() == 0) throw std::invalid_argument("invalid event sequence");
    read_field<Materialize>(j, "state", out.state);
}

void to_json(nlohmann::json& j, const JobStateNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "state", value.state);
    validate_json(j, TypeTag<JobStateNotification>{});
}

BRAURSER_READ_JSON(CatalogChangedNotification) {
    read_object(j, {"revision"}, extra);
    read_field<Materialize>(j, "revision", out.revision);
}

void to_json(nlohmann::json& j, const CatalogChangedNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "revision", value.revision);
    validate_json(j, TypeTag<CatalogChangedNotification>{});
}

BRAURSER_READ_JSON(UpdatesAvailableNotification) {
    read_object(j, {"reportId","count"}, extra);
    read_field<Materialize>(j, "reportId", out.reportId);
    read_field<Materialize>(j, "count", out.count);
}

void to_json(nlohmann::json& j, const UpdatesAvailableNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "reportId", value.reportId);
    write_field(j, "count", value.count);
    validate_json(j, TypeTag<UpdatesAvailableNotification>{});
}

namespace {

template <bool Materialize, typename Variant>
void read_variant(const nlohmann::json& j, Variant& out, ObjectKeys extra,
                  std::initializer_list<std::pair<std::string_view, void (*)(const nlohmann::json&, Variant&, ObjectKeys)>> cases) {
    if (!j.is_object() || !j.contains("kind") || !j.at("kind").is_string()) throw std::invalid_argument("invalid tagged union");
    const auto& kind = text(j, "kind");
    for (auto [tag, parser] : cases) {
        if (tag == kind) {
            extra.tagged = true;
            parser(j, out, extra);
            return;
        }
    }
    throw std::invalid_argument("unknown tagged union kind");
}

template <bool Materialize, typename T, typename Variant>
void parse_case(const nlohmann::json& j, Variant& out, ObjectKeys extra) {
    T value{};
    read_json<Materialize>(j, value, extra);
    if constexpr (Materialize) out.value = std::move(value);
}

template <typename Variant>
void write_variant(nlohmann::json& j, const Variant& out, std::initializer_list<std::pair<std::string_view, std::size_t>> tags) {
    std::visit([&j](const auto& item) { j = item; }, out.value);
    for (auto [tag, index] : tags) {
        if (index == out.value.index()) { j["kind"] = tag; return; }
    }
    throw std::invalid_argument("invalid tagged union");
}

}


BRAURSER_READ_JSON(Inline) {
    read_variant<Materialize>(j, out, extra, {
        {"text", &parse_case<Materialize, WikiText, Inline>},
        {"link", &parse_case<Materialize, WikiLink, Inline>},
    });
}
void to_json(nlohmann::json& j, const Inline& out) {
    write_variant(j, out, {
        {"text", 0},
        {"link", 1},
    });
}

BRAURSER_READ_JSON(WikiBlock) {
    read_variant<Materialize>(j, out, extra, {
        {"heading", &parse_case<Materialize, WikiHeading, WikiBlock>},
        {"paragraph", &parse_case<Materialize, WikiParagraph, WikiBlock>},
        {"list", &parse_case<Materialize, WikiList, WikiBlock>},
        {"code", &parse_case<Materialize, WikiCode, WikiBlock>},
        {"note", &parse_case<Materialize, WikiNote, WikiBlock>},
        {"warning", &parse_case<Materialize, WikiWarning, WikiBlock>},
    });
}
void to_json(nlohmann::json& j, const WikiBlock& out) {
    write_variant(j, out, {
        {"heading", 0},
        {"paragraph", 1},
        {"list", 2},
        {"code", 3},
        {"note", 4},
        {"warning", 5},
    });
}

BRAURSER_READ_JSON(QuestionChoice) {
    read_variant<Materialize>(j, out, extra, {
        {"variant", &parse_case<Materialize, VariantChoices, QuestionChoice>},
        {"provider", &parse_case<Materialize, ProviderQuestion, QuestionChoice>},
        {"build_review", &parse_case<Materialize, BuildReviewQuestion, QuestionChoice>},
        {"conflict", &parse_case<Materialize, ConflictQuestion, QuestionChoice>},
        {"news_ack", &parse_case<Materialize, NewsAckQuestion, QuestionChoice>},
        {"git_redirect", &parse_case<Materialize, GitRedirectQuestion, QuestionChoice>},
    });
}
void to_json(nlohmann::json& j, const QuestionChoice& out) {
    write_variant(j, out, {
        {"variant", 0},
        {"provider", 1},
        {"build_review", 2},
        {"conflict", 3},
        {"news_ack", 4},
        {"git_redirect", 5},
    });
}

#undef BRAURSER_READ_JSON

}
