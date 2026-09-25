#include <braurser/protocol/dto.hpp>
#include <array>
#include <cctype>
#include <limits>
#include <cmath>
#include <type_traits>

namespace braurser::protocol {
namespace {

template <class T>
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
    out = j.at(key).get<T>();
}

template <class T>
void read_field(const nlohmann::json& j, const char* key, std::optional<T>& out) {
    if (j.contains(key) && !j.at(key).is_null()) {
        T checked{};
        read_field(j, key, checked);
        out = std::move(checked);
    }
}

template <class T>
void write_field(nlohmann::json& j, const char* key, const T& value) {
    j[key] = value;
}

template <class T>
void write_field(nlohmann::json& j, const char* key, const std::optional<T>& value) {
    if (value) j[key] = *value;
}

void read_object(const nlohmann::json& j, std::initializer_list<std::string_view> keys) {
    if (!j.is_object()) throw std::invalid_argument("expected object");
    validate_wire(j);
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (std::find(keys.begin(), keys.end(), it.key()) == keys.end())
            throw std::invalid_argument("unknown field");
    }
}

}


void from_json(const nlohmann::json& j, PackageRef& out) {
    read_object(j, {"source","repo","name"});
    read_field(j, "source", out.source);
    read_field(j, "repo", out.repo);
    read_field(j, "name", out.name);
    if (!br_valid_pkgname(out.name.data(), out.name.size()) ||
        (out.repo && !br_valid_repo_name(out.repo->data(), out.repo->size())))
        throw std::invalid_argument("invalid package reference");
}

void to_json(nlohmann::json& j, const PackageRef& value) {
    j = nlohmann::json::object();
    write_field(j, "source", value.source);
    write_field(j, "repo", value.repo);
    write_field(j, "name", value.name);
    PackageRef checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, PackageSummary& out) {
    read_object(j, {"package","version","description","installed","installedVersion","installedSize","downloadSize","appName","iconPath","alsoIn","shadowed"});
    read_field(j, "package", out.package);
    read_field(j, "version", out.version);
    read_field(j, "description", out.description);
    read_field(j, "installed", out.installed);
    read_field(j, "installedVersion", out.installedVersion);
    read_field(j, "installedSize", out.installedSize);
    read_field(j, "downloadSize", out.downloadSize);
    read_field(j, "appName", out.appName);
    read_field(j, "iconPath", out.iconPath);
    read_field(j, "alsoIn", out.alsoIn);
    read_field(j, "shadowed", out.shadowed);
    if (out.alsoIn) {
        if (out.alsoIn->size() > 256) throw std::invalid_argument("too many repositories");
        for (const auto& repo : *out.alsoIn)
            if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
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
    PackageSummary checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Screenshot& out) {
    read_object(j, {"url","caption"});
    read_field(j, "url", out.url);
    read_field(j, "caption", out.caption);
}

void to_json(nlohmann::json& j, const Screenshot& value) {
    j = nlohmann::json::object();
    write_field(j, "url", value.url);
    write_field(j, "caption", value.caption);
    Screenshot checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, AppMetadata& out) {
    read_object(j, {"appstream_id","name","summary","description_paragraphs","icon_path","screenshots","categories","launchables"});
    read_field(j, "appstream_id", out.appstream_id);
    read_field(j, "name", out.name);
    read_field(j, "summary", out.summary);
    read_field(j, "description_paragraphs", out.description_paragraphs);
    read_field(j, "icon_path", out.icon_path);
    read_field(j, "screenshots", out.screenshots);
    read_field(j, "categories", out.categories);
    read_field(j, "launchables", out.launchables);
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
    AppMetadata checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, CommunityInfo& out) {
    read_object(j, {"maintainer","submitter","votes","popularity","out_of_date_since","first_submitted","last_modified","package_base","upstream_url"});
    read_field(j, "maintainer", out.maintainer);
    read_field(j, "submitter", out.submitter);
    read_field(j, "votes", out.votes);
    read_field(j, "popularity", out.popularity);
    read_field(j, "out_of_date_since", out.out_of_date_since);
    read_field(j, "first_submitted", out.first_submitted);
    read_field(j, "last_modified", out.last_modified);
    read_field(j, "package_base", out.package_base);
    read_field(j, "upstream_url", out.upstream_url);
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
    CommunityInfo checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, PackageDetails& out) {
    read_object(j, {"package","version","description","dependencies","url","alsoIn","shadowed"});
    read_field(j, "package", out.package);
    read_field(j, "version", out.version);
    read_field(j, "description", out.description);
    read_field(j, "dependencies", out.dependencies);
    read_field(j, "url", out.url);
    read_field(j, "alsoIn", out.alsoIn);
    read_field(j, "shadowed", out.shadowed);
    if (out.alsoIn) {
        if (out.alsoIn->size() > 256) throw std::invalid_argument("too many repositories");
        for (const auto& repo : *out.alsoIn)
            if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
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
    PackageDetails checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SearchFilters& out) {
    read_object(j, {"installed","not_installed","explicit","dependency","orphans","foreign","native","upgradable","repos","groups","aur_only","repo_only","apps_only","category"});
    if (j.contains("installed")) read_field(j, "installed", out.installed);
    if (j.contains("not_installed")) read_field(j, "not_installed", out.not_installed);
    if (j.contains("explicit")) read_field(j, "explicit", out.explicit_);
    if (j.contains("dependency")) read_field(j, "dependency", out.dependency);
    if (j.contains("orphans")) read_field(j, "orphans", out.orphans);
    if (j.contains("foreign")) read_field(j, "foreign", out.foreign);
    if (j.contains("native")) read_field(j, "native", out.native);
    if (j.contains("upgradable")) read_field(j, "upgradable", out.upgradable);
    if (j.contains("repos")) read_field(j, "repos", out.repos);
    if (j.contains("groups")) read_field(j, "groups", out.groups);
    if (j.contains("aur_only")) read_field(j, "aur_only", out.aur_only);
    if (j.contains("repo_only")) read_field(j, "repo_only", out.repo_only);
    if (j.contains("apps_only")) read_field(j, "apps_only", out.apps_only);
    read_field(j, "category", out.category);
    if (out.aur_only && out.repo_only) throw std::invalid_argument("conflicting source filters");
    for (const auto& repo : out.repos) {
        if (!br_valid_repo_name(repo.data(), repo.size())) throw std::invalid_argument("invalid repository");
    }
    for (const auto& group : out.groups) {
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
    SearchFilters checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, CatalogFilters& out) {
    read_object(j, {"installed", "explicit", "deps", "orphans", "foreign", "native", "upgradable",
                    "sourceRepo", "group", "aurOnly", "repoOnly", "appsOnly"});
    if (j.contains("installed")) read_field(j, "installed", out.installed);
    if (j.contains("explicit")) read_field(j, "explicit", out.explicit_);
    if (j.contains("deps")) read_field(j, "deps", out.deps);
    if (j.contains("orphans")) read_field(j, "orphans", out.orphans);
    if (j.contains("foreign")) read_field(j, "foreign", out.foreign);
    if (j.contains("native")) read_field(j, "native", out.native);
    if (j.contains("upgradable")) read_field(j, "upgradable", out.upgradable);
    read_field(j, "sourceRepo", out.sourceRepo);
    read_field(j, "group", out.group);
    if (j.contains("aurOnly")) read_field(j, "aurOnly", out.aurOnly);
    if (j.contains("repoOnly")) read_field(j, "repoOnly", out.repoOnly);
    if (j.contains("appsOnly")) read_field(j, "appsOnly", out.appsOnly);
    if (out.aurOnly && out.repoOnly) throw std::invalid_argument("conflicting source filters");
    if (out.sourceRepo && !br_valid_repo_name(out.sourceRepo->data(), out.sourceRepo->size()))
        throw std::invalid_argument("invalid repository");
    if (out.group && !br_valid_pkgname(out.group->data(), out.group->size()))
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
    CatalogFilters checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SearchQuery& out) {
    read_object(j, {"text","filters","sort","cursor","limit"});
    read_field(j, "text", out.text);
    read_field(j, "filters", out.filters);
    read_field(j, "sort", out.sort);
    read_field(j, "cursor", out.cursor);
    read_field(j, "limit", out.limit);
    if (out.limit == 0 || out.limit > 200 || out.text.size() > 512)
        throw std::invalid_argument("invalid search bounds");
}

void to_json(nlohmann::json& j, const SearchQuery& value) {
    j = nlohmann::json::object();
    write_field(j, "text", value.text);
    write_field(j, "filters", value.filters);
    write_field(j, "sort", value.sort);
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    SearchQuery checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SearchPage& out) {
    read_object(j, {"packages","next_cursor","stale"});
    read_field(j, "packages", out.packages);
    read_field(j, "next_cursor", out.next_cursor);
    read_field(j, "stale", out.stale);
}

void to_json(nlohmann::json& j, const SearchPage& value) {
    j = nlohmann::json::object();
    write_field(j, "packages", value.packages);
    write_field(j, "next_cursor", value.next_cursor);
    write_field(j, "stale", value.stale);
    SearchPage checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, VariantOption& out) {
    read_object(j, {"package","kind","development","recommended"});
    read_field(j, "package", out.package);
    read_field(j, "kind", out.kind);
    read_field(j, "development", out.development);
    read_field(j, "recommended", out.recommended);
}

void to_json(nlohmann::json& j, const VariantOption& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "kind", value.kind);
    write_field(j, "development", value.development);
    write_field(j, "recommended", value.recommended);
    VariantOption checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, VariantChoices& out) {
    read_object(j, {"options","urls_differ"});
    read_field(j, "options", out.options);
    read_field(j, "urls_differ", out.urls_differ);
}

void to_json(nlohmann::json& j, const VariantChoices& value) {
    j = nlohmann::json::object();
    write_field(j, "options", value.options);
    write_field(j, "urls_differ", value.urls_differ);
    VariantChoices checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiText& out) {
    read_object(j, {"s","emphasis","strong","code"});
    read_field(j, "s", out.s);
    if (j.contains("emphasis")) read_field(j, "emphasis", out.emphasis);
    if (j.contains("strong")) read_field(j, "strong", out.strong);
    if (j.contains("code")) read_field(j, "code", out.code);
}

void to_json(nlohmann::json& j, const WikiText& value) {
    j = nlohmann::json::object();
    write_field(j, "s", value.s);
    write_field(j, "emphasis", value.emphasis);
    write_field(j, "strong", value.strong);
    write_field(j, "code", value.code);
    WikiText checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiLink& out) {
    read_object(j, {"label","href"});
    read_field(j, "label", out.label);
    read_field(j, "href", out.href);
}

void to_json(nlohmann::json& j, const WikiLink& value) {
    j = nlohmann::json::object();
    write_field(j, "label", value.label);
    write_field(j, "href", value.href);
    WikiLink checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiHeading& out) {
    read_object(j, {"level","text"});
    read_field(j, "level", out.level);
    read_field(j, "text", out.text);
    if (out.level < 2 || out.level > 4) throw std::invalid_argument("invalid heading level");
}

void to_json(nlohmann::json& j, const WikiHeading& value) {
    j = nlohmann::json::object();
    write_field(j, "level", value.level);
    write_field(j, "text", value.text);
    WikiHeading checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiParagraph& out) {
    read_object(j, {"inlines"});
    read_field(j, "inlines", out.inlines);
}

void to_json(nlohmann::json& j, const WikiParagraph& value) {
    j = nlohmann::json::object();
    write_field(j, "inlines", value.inlines);
    WikiParagraph checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiList& out) {
    read_object(j, {"ordered","items"});
    read_field(j, "ordered", out.ordered);
    read_field(j, "items", out.items);
}

void to_json(nlohmann::json& j, const WikiList& value) {
    j = nlohmann::json::object();
    write_field(j, "ordered", value.ordered);
    write_field(j, "items", value.items);
    WikiList checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiCode& out) {
    read_object(j, {"text"});
    read_field(j, "text", out.text);
}

void to_json(nlohmann::json& j, const WikiCode& value) {
    j = nlohmann::json::object();
    write_field(j, "text", value.text);
    WikiCode checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiNote& out) {
    read_object(j, {"inlines"});
    read_field(j, "inlines", out.inlines);
}

void to_json(nlohmann::json& j, const WikiNote& value) {
    j = nlohmann::json::object();
    write_field(j, "inlines", value.inlines);
    WikiNote checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiWarning& out) {
    read_object(j, {"inlines"});
    read_field(j, "inlines", out.inlines);
}

void to_json(nlohmann::json& j, const WikiWarning& value) {
    j = nlohmann::json::object();
    write_field(j, "inlines", value.inlines);
    WikiWarning checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiSection& out) {
    read_object(j, {"level","title"});
    read_field(j, "level", out.level);
    read_field(j, "title", out.title);
    if (out.level < 2 || out.level > 4) throw std::invalid_argument("invalid heading level");
}

void to_json(nlohmann::json& j, const WikiSection& value) {
    j = nlohmann::json::object();
    write_field(j, "level", value.level);
    write_field(j, "title", value.title);
    WikiSection checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiDocument& out) {
    read_object(j, {"title","source_url","fetched_at","stale","attribution_key","blocks","sections"});
    read_field(j, "title", out.title);
    read_field(j, "source_url", out.source_url);
    read_field(j, "fetched_at", out.fetched_at);
    read_field(j, "stale", out.stale);
    read_field(j, "attribution_key", out.attribution_key);
    read_field(j, "blocks", out.blocks);
    read_field(j, "sections", out.sections);
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
    WikiDocument checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, NewsItem& out) {
    read_object(j, {"title","url","published","requires_manual_intervention","acknowledged"});
    read_field(j, "title", out.title);
    read_field(j, "url", out.url);
    read_field(j, "published", out.published);
    read_field(j, "requires_manual_intervention", out.requires_manual_intervention);
    read_field(j, "acknowledged", out.acknowledged);
}

void to_json(nlohmann::json& j, const NewsItem& value) {
    j = nlohmann::json::object();
    write_field(j, "title", value.title);
    write_field(j, "url", value.url);
    write_field(j, "published", value.published);
    write_field(j, "requires_manual_intervention", value.requires_manual_intervention);
    write_field(j, "acknowledged", value.acknowledged);
    NewsItem checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, RestartAdviceDTO& out) {
    read_object(j, {"package","level","reason_key","detail"});
    read_field(j, "package", out.package);
    read_field(j, "level", out.level);
    read_field(j, "reason_key", out.reason_key);
    read_field(j, "detail", out.detail);
}

void to_json(nlohmann::json& j, const RestartAdviceDTO& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "level", value.level);
    write_field(j, "reason_key", value.reason_key);
    write_field(j, "detail", value.detail);
    RestartAdviceDTO checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdateItem& out) {
    read_object(j, {"package","from_version","to_version","source","restart"});
    read_field(j, "package", out.package);
    read_field(j, "from_version", out.from_version);
    read_field(j, "to_version", out.to_version);
    read_field(j, "source", out.source);
    read_field(j, "restart", out.restart);
}

void to_json(nlohmann::json& j, const UpdateItem& value) {
    j = nlohmann::json::object();
    write_field(j, "package", value.package);
    write_field(j, "from_version", value.from_version);
    write_field(j, "to_version", value.to_version);
    write_field(j, "source", value.source);
    write_field(j, "restart", value.restart);
    UpdateItem checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdateReport& out) {
    read_object(j, {"report_id","checked_at","official","community","manual","news","overall_restart"});
    read_field(j, "report_id", out.report_id);
    read_field(j, "checked_at", out.checked_at);
    read_field(j, "official", out.official);
    read_field(j, "community", out.community);
    read_field(j, "manual", out.manual);
    read_field(j, "news", out.news);
    read_field(j, "overall_restart", out.overall_restart);
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
    UpdateReport checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Replacement& out) {
    read_object(j, {"old","new"});
    read_field(j, "old", out.old);
    read_field(j, "new", out.new_);
}

void to_json(nlohmann::json& j, const Replacement& value) {
    j = nlohmann::json::object();
    write_field(j, "old", value.old);
    write_field(j, "new", value.new_);
    Replacement checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Blocker& out) {
    read_object(j, {"code","message_key","args"});
    read_field(j, "code", out.code);
    read_field(j, "message_key", out.message_key);
    read_field(j, "args", out.args);
}

void to_json(nlohmann::json& j, const Blocker& value) {
    j = nlohmann::json::object();
    write_field(j, "code", value.code);
    write_field(j, "message_key", value.message_key);
    write_field(j, "args", value.args);
    Blocker checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, TransactionPlan& out) {
    read_object(j, {"plan_id","revision","approval_token","kind","installs","upgrades","removals","replacements","conflicts","download_bytes","install_delta_bytes","restart","requires_build_review","build_review_refs","protected_blockers","blockers"});
    read_field(j, "plan_id", out.plan_id);
    read_field(j, "revision", out.revision);
    read_field(j, "approval_token", out.approval_token);
    read_field(j, "kind", out.kind);
    read_field(j, "installs", out.installs);
    read_field(j, "upgrades", out.upgrades);
    read_field(j, "removals", out.removals);
    read_field(j, "replacements", out.replacements);
    read_field(j, "conflicts", out.conflicts);
    read_field(j, "download_bytes", out.download_bytes);
    read_field(j, "install_delta_bytes", out.install_delta_bytes);
    read_field(j, "restart", out.restart);
    read_field(j, "requires_build_review", out.requires_build_review);
    read_field(j, "build_review_refs", out.build_review_refs);
    read_field(j, "protected_blockers", out.protected_blockers);
    read_field(j, "blockers", out.blockers);
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
    write_field(j, "protected_blockers", value.protected_blockers);
    write_field(j, "blockers", value.blockers);
    TransactionPlan checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Progress& out) {
    read_object(j, {"phase","current","total","unit"});
    read_field(j, "phase", out.phase);
    read_field(j, "current", out.current);
    read_field(j, "total", out.total);
    read_field(j, "unit", out.unit);
    if (out.current && out.total && *out.current > *out.total)
        throw std::invalid_argument("invalid progress");
}

void to_json(nlohmann::json& j, const Progress& value) {
    j = nlohmann::json::object();
    write_field(j, "phase", value.phase);
    write_field(j, "current", value.current);
    write_field(j, "total", value.total);
    write_field(j, "unit", value.unit);
    Progress checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ProviderQuestion& out) {
    read_object(j, {"options"});
    read_field(j, "options", out.options);
}

void to_json(nlohmann::json& j, const ProviderQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "options", value.options);
    ProviderQuestion checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, BuildReviewQuestion& out) {
    read_object(j, {"refs","pkgbuild"});
    read_field(j, "refs", out.refs);
    read_field(j, "pkgbuild", out.pkgbuild);
    if (out.pkgbuild && (out.pkgbuild->empty() || out.pkgbuild->size() > 256 * 1024))
        throw std::invalid_argument("invalid build file");
}

void to_json(nlohmann::json& j, const BuildReviewQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "refs", value.refs);
    write_field(j, "pkgbuild", value.pkgbuild);
    BuildReviewQuestion checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ConflictQuestion& out) {
    read_object(j, {"a","b"});
    read_field(j, "a", out.a);
    read_field(j, "b", out.b);
}

void to_json(nlohmann::json& j, const ConflictQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "a", value.a);
    write_field(j, "b", value.b);
    ConflictQuestion checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, NewsAckQuestion& out) {
    read_object(j, {"items"});
    read_field(j, "items", out.items);
}

void to_json(nlohmann::json& j, const NewsAckQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "items", value.items);
    NewsAckQuestion checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, GitRedirectQuestion& out) {
    read_object(j, {"from_host","to_host"});
    read_field(j, "from_host", out.from_host);
    read_field(j, "to_host", out.to_host);
}

void to_json(nlohmann::json& j, const GitRedirectQuestion& value) {
    j = nlohmann::json::object();
    write_field(j, "from_host", value.from_host);
    write_field(j, "to_host", value.to_host);
    GitRedirectQuestion checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Question& out) {
    if (!j.is_object()) throw std::invalid_argument("invalid question");
    validate_wire(j);
    read_field(j, "question_id", out.question_id);
    read_field(j, "token", out.token);
    read_field(j, "expires_at", out.expires_at);
    auto payload = j;
    payload.erase("question_id");
    payload.erase("token");
    payload.erase("expires_at");
    out.choice = payload.get<QuestionChoice>();
}

void to_json(nlohmann::json& j, const Question& value) {
    j = value.choice;
    write_field(j, "question_id", value.question_id);
    write_field(j, "token", value.token);
    write_field(j, "expires_at", value.expires_at);
    Question checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Settings& out) {
    read_object(j, {"ui","helper","check_frequency","notifications","autostart","review_builds","show_log","theme"});
    read_field(j, "ui", out.ui);
    read_field(j, "helper", out.helper);
    read_field(j, "check_frequency", out.check_frequency);
    read_field(j, "notifications", out.notifications);
    read_field(j, "autostart", out.autostart);
    read_field(j, "review_builds", out.review_builds);
    read_field(j, "show_log", out.show_log);
    read_field(j, "theme", out.theme);
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
    Settings checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SettingsPatch& out) {
    read_object(j, {"ui", "helper", "check_frequency", "notifications", "autostart", "review_builds", "show_log", "theme"});
    read_field(j, "ui", out.ui);
    read_field(j, "helper", out.helper);
    read_field(j, "check_frequency", out.check_frequency);
    read_field(j, "notifications", out.notifications);
    read_field(j, "autostart", out.autostart);
    read_field(j, "review_builds", out.review_builds);
    read_field(j, "show_log", out.show_log);
    read_field(j, "theme", out.theme);
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
    SettingsPatch checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ManualPreview& out) {
    read_object(j, {"preview_id","host","owner_repo","commit","script_path","script_text","scope_options","external_refs","staging_supported"});
    read_field(j, "preview_id", out.preview_id);
    read_field(j, "host", out.host);
    read_field(j, "owner_repo", out.owner_repo);
    read_field(j, "commit", out.commit);
    read_field(j, "script_path", out.script_path);
    read_field(j, "script_text", out.script_text);
    read_field(j, "scope_options", out.scope_options);
    read_field(j, "external_refs", out.external_refs);
    read_field(j, "staging_supported", out.staging_supported);
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
    ManualPreview checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ManualReceipt& out) {
    read_object(j, {"receipt_id","name","scope","commit","installed_at","launcher_ids","removable"});
    read_field(j, "receipt_id", out.receipt_id);
    read_field(j, "name", out.name);
    read_field(j, "scope", out.scope);
    read_field(j, "commit", out.commit);
    read_field(j, "installed_at", out.installed_at);
    read_field(j, "launcher_ids", out.launcher_ids);
    read_field(j, "removable", out.removable);
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
    ManualReceipt checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, LauncherEntry& out) {
    read_object(j, {"desktop_id","name","exec_display","path","owner"});
    read_field(j, "desktop_id", out.desktop_id);
    read_field(j, "name", out.name);
    read_field(j, "exec_display", out.exec_display);
    read_field(j, "path", out.path);
    read_field(j, "owner", out.owner);
}

void to_json(nlohmann::json& j, const LauncherEntry& value) {
    j = nlohmann::json::object();
    write_field(j, "desktop_id", value.desktop_id);
    write_field(j, "name", value.name);
    write_field(j, "exec_display", value.exec_display);
    write_field(j, "path", value.path);
    write_field(j, "owner", value.owner);
    LauncherEntry checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, Capabilities& out) {
    read_object(j, {"helper","helper_version","elevation","init","distro_id","repos"});
    read_field(j, "helper", out.helper);
    read_field(j, "helper_version", out.helper_version);
    read_field(j, "elevation", out.elevation);
    read_field(j, "init", out.init);
    read_field(j, "distro_id", out.distro_id);
    read_field(j, "repos", out.repos);
    for (const auto& repo : out.repos) {
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
    Capabilities checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SystemStatus& out) {
    read_object(j, {"connectivity","catalog_age","active_job","elevation_method"});
    read_field(j, "connectivity", out.connectivity);
    read_field(j, "catalog_age", out.catalog_age);
    read_field(j, "active_job", out.active_job);
    read_field(j, "elevation_method", out.elevation_method);
}

void to_json(nlohmann::json& j, const SystemStatus& value) {
    j = nlohmann::json::object();
    write_field(j, "connectivity", value.connectivity);
    write_field(j, "catalog_age", value.catalog_age);
    write_field(j, "active_job", value.active_job);
    write_field(j, "elevation_method", value.elevation_method);
    SystemStatus checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, HelloParams& out) {
    read_object(j, {"apiVersion"});
    read_field(j, "apiVersion", out.apiVersion);
    if (out.apiVersion != 1) throw std::invalid_argument("invalid API version");
}

void to_json(nlohmann::json& j, const HelloParams& value) {
    j = nlohmann::json::object();
    write_field(j, "apiVersion", value.apiVersion);
    HelloParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, HelloResult& out) {
    read_object(j, {"serverVersion","apiVersion","helper","capabilities","uiBackends"});
    read_field(j, "serverVersion", out.serverVersion);
    read_field(j, "apiVersion", out.apiVersion);
    read_field(j, "helper", out.helper);
    read_field(j, "capabilities", out.capabilities);
    read_field(j, "uiBackends", out.uiBackends);
    if (out.apiVersion != 1) throw std::invalid_argument("invalid API version");
}

void to_json(nlohmann::json& j, const HelloResult& value) {
    j = nlohmann::json::object();
    write_field(j, "serverVersion", value.serverVersion);
    write_field(j, "apiVersion", value.apiVersion);
    write_field(j, "helper", value.helper);
    write_field(j, "capabilities", value.capabilities);
    write_field(j, "uiBackends", value.uiBackends);
    HelloResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, StatusParams& out) {
    static_cast<void>(out);
    read_object(j, {});
}

void to_json(nlohmann::json& j, const StatusParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    StatusParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, CatalogSourceStatus& out) {
    read_object(j, {"id","status","reasonCode","packageCount","skippedCount"});
    read_field(j, "id", out.id);
    read_field(j, "status", out.status);
    read_field(j, "reasonCode", out.reasonCode);
    read_field(j, "packageCount", out.packageCount);
    read_field(j, "skippedCount", out.skippedCount);
    if (out.status != "ok" && out.status != "unavailable" && out.status != "failed")
        throw std::invalid_argument("invalid source status");
}

void to_json(nlohmann::json& j, const CatalogSourceStatus& value) {
    j = {{"id", value.id}, {"status", value.status}, {"packageCount", value.packageCount}, {"skippedCount", value.skippedCount}};
    write_field(j, "reasonCode", value.reasonCode);
    CatalogSourceStatus checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, StatusResult& out) {
    read_object(j, {"connectivity","catalogAge","activeJob","elevationMethod","catalogState","catalogAgeSeconds","sources"});
    read_field(j, "connectivity", out.connectivity);
    read_field(j, "catalogAge", out.catalogAge);
    read_field(j, "activeJob", out.activeJob);
    read_field(j, "elevationMethod", out.elevationMethod);
    read_field(j, "catalogState", out.catalogState);
    read_field(j, "catalogAgeSeconds", out.catalogAgeSeconds);
    read_field(j, "sources", out.sources);
    if (out.catalogState != "missing" && out.catalogState != "refreshing" &&
        out.catalogState != "ready" && out.catalogState != "stale")
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
    StatusResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SearchParams& out) {
    read_object(j, {"text","filters","cursor","limit","sort","category"});
    read_field(j, "text", out.text);
    read_field(j, "filters", out.filters);
    read_field(j, "cursor", out.cursor);
    read_field(j, "limit", out.limit);
    if (j.contains("sort")) read_field(j, "sort", out.sort);
    read_field(j, "category", out.category);
    if (out.limit == 0 || out.limit > 200 || out.text.size() > 256 ||
        (out.category && (out.category->empty() || out.category->size() > 256)))
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
    SearchParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SearchResult& out) {
    read_object(j, {"packages","nextCursor","stale","catalogState"});
    read_field(j, "packages", out.packages);
    read_field(j, "nextCursor", out.nextCursor);
    read_field(j, "stale", out.stale);
    read_field(j, "catalogState", out.catalogState);
    if (out.catalogState && *out.catalogState != "refreshing" && *out.catalogState != "ready" && *out.catalogState != "stale")
        throw std::invalid_argument("invalid catalog state");
}

void to_json(nlohmann::json& j, const SearchResult& value) {
    j = nlohmann::json::object();
    write_field(j, "packages", value.packages);
    write_field(j, "nextCursor", value.nextCursor);
    write_field(j, "stale", value.stale);
    write_field(j, "catalogState", value.catalogState);
    SearchResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, GetParams& out) {
    read_object(j, {"packageId"});
    read_field(j, "packageId", out.packageId);
}

void to_json(nlohmann::json& j, const GetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "packageId", value.packageId);
    GetParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, GetResult& out) {
    read_object(j, {"details","installedState","source","appMetadata"});
    read_field(j, "details", out.details);
    read_field(j, "installedState", out.installedState);
    read_field(j, "source", out.source);
    read_field(j, "appMetadata", out.appMetadata);
}

void to_json(nlohmann::json& j, const GetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "details", value.details);
    write_field(j, "installedState", value.installedState);
    write_field(j, "source", value.source);
    write_field(j, "appMetadata", value.appMetadata);
    GetResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, VariantsParams& out) {
    read_object(j, {"packageId"});
    read_field(j, "packageId", out.packageId);
}

void to_json(nlohmann::json& j, const VariantsParams& value) {
    j = nlohmann::json::object();
    write_field(j, "packageId", value.packageId);
    VariantsParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, VariantsResult& out) {
    read_object(j, {"choices","questionRequired"});
    read_field(j, "choices", out.choices);
    read_field(j, "questionRequired", out.questionRequired);
}

void to_json(nlohmann::json& j, const VariantsResult& value) {
    j = nlohmann::json::object();
    write_field(j, "choices", value.choices);
    write_field(j, "questionRequired", value.questionRequired);
    VariantsResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, FacetsParams&) { read_object(j, {}); }
void to_json(nlohmann::json& j, const FacetsParams&) { j = nlohmann::json::object(); }

void from_json(const nlohmann::json& j, FacetCount& out) {
    read_object(j, {"name","packageCount"});
    read_field(j, "name", out.name);
    read_field(j, "packageCount", out.packageCount);
}
void to_json(nlohmann::json& j, const FacetCount& value) {
    j = {{"name", value.name}, {"packageCount", value.packageCount}};
    FacetCount checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, CategoryCount& out) {
    read_object(j, {"id","packageCount"});
    read_field(j, "id", out.id);
    read_field(j, "packageCount", out.packageCount);
}
void to_json(nlohmann::json& j, const CategoryCount& value) {
    j = {{"id", value.id}, {"packageCount", value.packageCount}};
    CategoryCount checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, FacetsResult& out) {
    read_object(j, {"repos","groups","categories"});
    read_field(j, "repos", out.repos);
    read_field(j, "groups", out.groups);
    read_field(j, "categories", out.categories);
}
void to_json(nlohmann::json& j, const FacetsResult& value) {
    j = {{"repos", value.repos}, {"groups", value.groups}, {"categories", value.categories}};
    FacetsResult checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, BuildFilesParams& out) {
    read_object(j, {"packageId"});
    read_field(j, "packageId", out.packageId);
}
void to_json(nlohmann::json& j, const BuildFilesParams& value) {
    j = {{"packageId", value.packageId}};
    BuildFilesParams checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, BuildFile& out) {
    read_object(j, {"name","text"});
    read_field(j, "name", out.name);
    read_field(j, "text", out.text);
}
void to_json(nlohmann::json& j, const BuildFile& value) {
    j = {{"name", value.name}, {"text", value.text}};
    BuildFile checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, BuildFilesResult& out) {
    read_object(j, {"packageBase","files","truncated"});
    read_field(j, "packageBase", out.packageBase);
    read_field(j, "files", out.files);
    read_field(j, "truncated", out.truncated);
}
void to_json(nlohmann::json& j, const BuildFilesResult& value) {
    j = {{"packageBase", value.packageBase}, {"files", value.files}, {"truncated", value.truncated}};
    BuildFilesResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, RefreshParams& out) {
    read_object(j, {"sources"});
    read_field(j, "sources", out.sources);
}

void to_json(nlohmann::json& j, const RefreshParams& value) {
    j = nlohmann::json::object();
    write_field(j, "sources", value.sources);
    RefreshParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobIdResult& out) {
    read_object(j, {"jobId"});
    read_field(j, "jobId", out.jobId);
}

void to_json(nlohmann::json& j, const JobIdResult& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    JobIdResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiGetParams& out) {
    read_object(j, {"packageId","allowRefresh"});
    read_field(j, "packageId", out.packageId);
    read_field(j, "allowRefresh", out.allowRefresh);
}

void to_json(nlohmann::json& j, const WikiGetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "packageId", value.packageId);
    write_field(j, "allowRefresh", value.allowRefresh);
    WikiGetParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, WikiGetResult& out) {
    read_object(j, {"document","sourceUrl","attribution","fetchedAt","stale"});
    read_field(j, "document", out.document);
    read_field(j, "sourceUrl", out.sourceUrl);
    read_field(j, "attribution", out.attribution);
    read_field(j, "fetchedAt", out.fetchedAt);
    read_field(j, "stale", out.stale);
}

void to_json(nlohmann::json& j, const WikiGetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "document", value.document);
    write_field(j, "sourceUrl", value.sourceUrl);
    write_field(j, "attribution", value.attribution);
    write_field(j, "fetchedAt", value.fetchedAt);
    write_field(j, "stale", value.stale);
    WikiGetResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, NewsListParams& out) {
    read_object(j, {"cursor","limit"});
    read_field(j, "cursor", out.cursor);
    read_field(j, "limit", out.limit);
    if (out.limit == 0 || out.limit > 200) throw std::invalid_argument("invalid limit");
}

void to_json(nlohmann::json& j, const NewsListParams& value) {
    j = nlohmann::json::object();
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    NewsListParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, NewsListResult& out) {
    read_object(j, {"items","nextCursor","stale"});
    read_field(j, "items", out.items);
    read_field(j, "nextCursor", out.nextCursor);
    read_field(j, "stale", out.stale);
}

void to_json(nlohmann::json& j, const NewsListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "items", value.items);
    write_field(j, "nextCursor", value.nextCursor);
    write_field(j, "stale", value.stale);
    NewsListResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdatesCheckParams& out) {
    read_object(j, {"refresh"});
    read_field(j, "refresh", out.refresh);
}

void to_json(nlohmann::json& j, const UpdatesCheckParams& value) {
    j = nlohmann::json::object();
    write_field(j, "refresh", value.refresh);
    UpdatesCheckParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdatesOptionsParams& out) {
    read_object(j, {"reportId"});
    read_field(j, "reportId", out.reportId);
}

void to_json(nlohmann::json& j, const UpdatesOptionsParams& value) {
    j = nlohmann::json::object();
    write_field(j, "reportId", value.reportId);
    UpdatesOptionsParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdatesOptionsResult& out) {
    read_object(j, {"officialAction","communityIndividualIds","manualIndividualIds"});
    read_field(j, "officialAction", out.officialAction);
    read_field(j, "communityIndividualIds", out.communityIndividualIds);
    read_field(j, "manualIndividualIds", out.manualIndividualIds);
    if (out.officialAction != "update-system") throw std::invalid_argument("invalid action");
}

void to_json(nlohmann::json& j, const UpdatesOptionsResult& value) {
    j = nlohmann::json::object();
    write_field(j, "officialAction", value.officialAction);
    write_field(j, "communityIndividualIds", value.communityIndividualIds);
    write_field(j, "manualIndividualIds", value.manualIndividualIds);
    UpdatesOptionsResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, PlanSelection& out) {
    read_object(j, {"kind","id"});
    read_field(j, "kind", out.kind);
    read_field(j, "id", out.id);
}

void to_json(nlohmann::json& j, const PlanSelection& value) {
    j = nlohmann::json::object();
    write_field(j, "kind", value.kind);
    write_field(j, "id", value.id);
    PlanSelection checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdatesPlanParams& out) {
    read_object(j, {"reportId","selection","alsoInstallPackageId"});
    read_field(j, "reportId", out.reportId);
    read_field(j, "selection", out.selection);
    read_field(j, "alsoInstallPackageId", out.alsoInstallPackageId);
}

void to_json(nlohmann::json& j, const UpdatesPlanParams& value) {
    j = nlohmann::json::object();
    write_field(j, "reportId", value.reportId);
    write_field(j, "selection", value.selection);
    write_field(j, "alsoInstallPackageId", value.alsoInstallPackageId);
    UpdatesPlanParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdatesPlanResult& out) {
    read_object(j, {"planId","transactions","restartAdvice","requiresApproval","blockers"});
    read_field(j, "planId", out.planId);
    read_field(j, "transactions", out.transactions);
    read_field(j, "restartAdvice", out.restartAdvice);
    read_field(j, "requiresApproval", out.requiresApproval);
    read_field(j, "blockers", out.blockers);
}

void to_json(nlohmann::json& j, const UpdatesPlanResult& value) {
    j = nlohmann::json::object();
    write_field(j, "planId", value.planId);
    write_field(j, "transactions", value.transactions);
    write_field(j, "restartAdvice", value.restartAdvice);
    write_field(j, "requiresApproval", value.requiresApproval);
    write_field(j, "blockers", value.blockers);
    UpdatesPlanResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, PlanParams& out) {
    read_object(j, {"kind","packageId","variantId","previewId","receiptId","scope","helper"});
    read_field(j, "kind", out.kind);
    read_field(j, "packageId", out.packageId);
    read_field(j, "variantId", out.variantId);
    read_field(j, "previewId", out.previewId);
    read_field(j, "receiptId", out.receiptId);
    read_field(j, "scope", out.scope);
    read_field(j, "helper", out.helper);
    const bool package = out.kind == "install" || out.kind == "remove" || out.kind == "update-community" || out.kind == "update-and-install";
    const bool manual = out.kind == "manual-install" || out.kind == "manual-upgrade" || out.kind == "manual-remove";
    if (!package && !manual && out.kind != "update-system" && out.kind != "setup-helper")
        throw std::invalid_argument("invalid plan kind");
    if ((package && !out.packageId) || ((out.kind == "manual-install" || out.kind == "manual-upgrade") && !out.previewId) ||
        ((out.kind == "manual-upgrade" || out.kind == "manual-remove") && !out.receiptId) ||
        (out.kind == "setup-helper" && (!out.helper || out.helper->value == "auto")) ||
        (!package && out.packageId) || (out.kind != "install" && out.variantId) ||
        (!manual && (out.previewId || out.receiptId || out.scope)) || (out.kind != "setup-helper" && out.helper))
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
    PlanParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, InstallParams& out) {
    read_object(j, {"planId","approvalToken"});
    read_field(j, "planId", out.planId);
    read_field(j, "approvalToken", out.approvalToken);
}

void to_json(nlohmann::json& j, const InstallParams& value) {
    j = {{"planId", value.planId}, {"approvalToken", value.approvalToken}};
    InstallParams checked;
    from_json(j, checked);
}

#define BRAURSER_APPROVAL_JSON(type) \
void from_json(const nlohmann::json& j, type& out) { from_json(j, static_cast<InstallParams&>(out)); } \
void to_json(nlohmann::json& j, const type& value) { to_json(j, static_cast<const InstallParams&>(value)); }
BRAURSER_APPROVAL_JSON(RemoveParams)
BRAURSER_APPROVAL_JSON(UpgradeSystemParams)
BRAURSER_APPROVAL_JSON(UpgradeCommunityParams)
BRAURSER_APPROVAL_JSON(ManualInstallParams)
BRAURSER_APPROVAL_JSON(ManualUpgradeParams)
BRAURSER_APPROVAL_JSON(ManualRemoveParams)
BRAURSER_APPROVAL_JSON(SetupHelperParams)
#undef BRAURSER_APPROVAL_JSON

void from_json(const nlohmann::json& j, JobEvent& out) {
    read_object(j, {"sequence","kind","payload"});
    read_field(j, "sequence", out.sequence);
    if (out.sequence == 0) throw std::invalid_argument("invalid event sequence");
    read_field(j, "kind", out.kind);
    read_field(j, "payload", out.payload);
}

void to_json(nlohmann::json& j, const JobEvent& value) {
    j = nlohmann::json::object();
    write_field(j, "sequence", value.sequence);
    write_field(j, "kind", value.kind);
    write_field(j, "payload", value.payload);
    JobEvent checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsGetParams& out) {
    read_object(j, {"jobId","eventsCursor"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "eventsCursor", out.eventsCursor);
}

void to_json(nlohmann::json& j, const JobsGetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "eventsCursor", value.eventsCursor);
    JobsGetParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsGetResult& out) {
    read_object(j, {"state","plan","progress","result","error","events","nextCursor"});
    read_field(j, "state", out.state);
    read_field(j, "plan", out.plan);
    read_field(j, "progress", out.progress);
    read_field(j, "result", out.result);
    read_field(j, "error", out.error);
    read_field(j, "events", out.events);
    read_field(j, "nextCursor", out.nextCursor);
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
    JobsGetResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsListParams& out) {
    read_object(j, {"cursor","limit"});
    read_field(j, "cursor", out.cursor);
    read_field(j, "limit", out.limit);
    if (out.limit == 0 || out.limit > 200) throw std::invalid_argument("invalid limit");
}

void to_json(nlohmann::json& j, const JobsListParams& value) {
    j = nlohmann::json::object();
    write_field(j, "cursor", value.cursor);
    write_field(j, "limit", value.limit);
    JobsListParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobSummary& out) {
    read_object(j, {"jobId","state"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "state", out.state);
}

void to_json(nlohmann::json& j, const JobSummary& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "state", value.state);
    JobSummary checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsListResult& out) {
    read_object(j, {"jobs","nextCursor"});
    read_field(j, "jobs", out.jobs);
    read_field(j, "nextCursor", out.nextCursor);
}

void to_json(nlohmann::json& j, const JobsListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "jobs", value.jobs);
    write_field(j, "nextCursor", value.nextCursor);
    JobsListResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsCancelParams& out) {
    read_object(j, {"jobId"});
    read_field(j, "jobId", out.jobId);
}

void to_json(nlohmann::json& j, const JobsCancelParams& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    JobsCancelParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsCancelResult& out) {
    read_object(j, {"accepted","reason"});
    read_field(j, "accepted", out.accepted);
    read_field(j, "reason", out.reason);
}

void to_json(nlohmann::json& j, const JobsCancelResult& value) {
    j = nlohmann::json::object();
    write_field(j, "accepted", value.accepted);
    write_field(j, "reason", value.reason);
    JobsCancelResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsAnswerParams& out) {
    read_object(j, {"jobId","questionId","choiceId","questionToken"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "questionId", out.questionId);
    read_field(j, "choiceId", out.choiceId);
    read_field(j, "questionToken", out.questionToken);
}

void to_json(nlohmann::json& j, const JobsAnswerParams& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "questionId", value.questionId);
    write_field(j, "choiceId", value.choiceId);
    write_field(j, "questionToken", value.questionToken);
    JobsAnswerParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, AcceptedResult& out) {
    read_object(j, {"accepted"});
    read_field(j, "accepted", out.accepted);
}

void to_json(nlohmann::json& j, const AcceptedResult& value) {
    j = nlohmann::json::object();
    write_field(j, "accepted", value.accepted);
    AcceptedResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ManualPreviewParams& out) {
    read_object(j, {"httpsUrl","scriptPath","scope"});
    read_field(j, "httpsUrl", out.httpsUrl);
    read_field(j, "scriptPath", out.scriptPath);
    read_field(j, "scope", out.scope);
}

void to_json(nlohmann::json& j, const ManualPreviewParams& value) {
    j = nlohmann::json::object();
    write_field(j, "httpsUrl", value.httpsUrl);
    write_field(j, "scriptPath", value.scriptPath);
    write_field(j, "scope", value.scope);
    ManualPreviewParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ManualListParams& out) {
    static_cast<void>(out);
    read_object(j, {});
}

void to_json(nlohmann::json& j, const ManualListParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    ManualListParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ManualListResult& out) {
    read_object(j, {"receipts"});
    read_field(j, "receipts", out.receipts);
}

void to_json(nlohmann::json& j, const ManualListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "receipts", value.receipts);
    ManualListResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobsDequeueParams& out) {
    read_object(j, {"jobId"});
    read_field(j, "jobId", out.jobId);
}
void to_json(nlohmann::json& j, const JobsDequeueParams& value) {
    j = {{"jobId", value.jobId}};
    JobsDequeueParams checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, JobsDequeueResult& out) {
    read_object(j, {"removed"});
    read_field(j, "removed", out.removed);
}
void to_json(nlohmann::json& j, const JobsDequeueResult& value) {
    j = {{"removed", value.removed}};
}

void from_json(const nlohmann::json& j, UnmanagedListParams& out) {
    static_cast<void>(out);
    read_object(j, {});
}

void to_json(nlohmann::json& j, const UnmanagedListParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    UnmanagedListParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UnmanagedListResult& out) {
    read_object(j, {"entries","scanTime"});
    read_field(j, "entries", out.entries);
    read_field(j, "scanTime", out.scanTime);
}

void to_json(nlohmann::json& j, const UnmanagedListResult& value) {
    j = nlohmann::json::object();
    write_field(j, "entries", value.entries);
    write_field(j, "scanTime", value.scanTime);
    UnmanagedListResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SettingsGetParams& out) {
    static_cast<void>(out);
    read_object(j, {});
}

void to_json(nlohmann::json& j, const SettingsGetParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    SettingsGetParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SettingsGetResult& out) {
    read_object(j, {"uiPreference","notifications","refreshPolicy","elevationPreference"});
    read_field(j, "uiPreference", out.uiPreference);
    read_field(j, "notifications", out.notifications);
    read_field(j, "refreshPolicy", out.refreshPolicy);
    read_field(j, "elevationPreference", out.elevationPreference);
}

void to_json(nlohmann::json& j, const SettingsGetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "uiPreference", value.uiPreference);
    write_field(j, "notifications", value.notifications);
    write_field(j, "refreshPolicy", value.refreshPolicy);
    write_field(j, "elevationPreference", value.elevationPreference);
    SettingsGetResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SettingsSetParams& out) {
    read_object(j, {"changedFields"});
    read_field(j, "changedFields", out.changedFields);
}

void to_json(nlohmann::json& j, const SettingsSetParams& value) {
    j = nlohmann::json::object();
    write_field(j, "changedFields", value.changedFields);
    SettingsSetParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, SettingsSetResult& out) {
    read_object(j, {"settings"});
    read_field(j, "settings", out.settings);
}

void to_json(nlohmann::json& j, const SettingsSetResult& value) {
    j = nlohmann::json::object();
    write_field(j, "settings", value.settings);
    SettingsSetResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, ClearCacheParams&) { read_object(j, {}); }
void to_json(nlohmann::json& j, const ClearCacheParams&) { j = nlohmann::json::object(); }
void from_json(const nlohmann::json& j, ClearCacheResult& out) {
    read_object(j, {"freedBytes"});
    read_field(j, "freedBytes", out.freedBytes);
}
void to_json(nlohmann::json& j, const ClearCacheResult& value) { j = {{"freedBytes", value.freedBytes}}; }
void from_json(const nlohmann::json& j, RestartStateParams&) { read_object(j, {}); }
void to_json(nlohmann::json& j, const RestartStateParams&) { j = nlohmann::json::object(); }
void from_json(const nlohmann::json& j, RestartAcknowledgeParams&) { read_object(j, {}); }
void to_json(nlohmann::json& j, const RestartAcknowledgeParams&) { j = nlohmann::json::object(); }
void from_json(const nlohmann::json& j, RestartStateResult& out) {
    read_object(j, {"level","reason_keys","since"});
    read_field(j, "level", out.level);
    read_field(j, "reason_keys", out.reason_keys);
    read_field(j, "since", out.since);
    if (out.reason_keys.size() > 64) throw std::invalid_argument("too many restart reasons");
    for (const auto& key : out.reason_keys) validate_wire(nlohmann::json{{"reason_key", key}});
    if (out.since) validate_wire(nlohmann::json{{"since", *out.since}});
}
void to_json(nlohmann::json& j, const RestartStateResult& value) {
    j = {{"level", value.level}, {"reason_keys", value.reason_keys}};
    write_field(j, "since", value.since);
    RestartStateResult checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, EmptyResult&) { read_object(j, {}); }
void to_json(nlohmann::json& j, const EmptyResult&) { j = nlohmann::json::object(); }
void from_json(const nlohmann::json& j, HelperStatus& out) {
    read_object(j, {"installed","version"});
    read_field(j, "installed", out.installed);
    read_field(j, "version", out.version);
    if (!out.installed && out.version) throw std::invalid_argument("invalid helper status");
}
void to_json(nlohmann::json& j, const HelperStatus& value) {
    j = {{"installed", value.installed}};
    write_field(j, "version", value.version);
    HelperStatus checked;
    from_json(j, checked);
}
void from_json(const nlohmann::json& j, SetupStatusParams&) { read_object(j, {}); }
void to_json(nlohmann::json& j, const SetupStatusParams&) { j = nlohmann::json::object(); }
void from_json(const nlohmann::json& j, SetupStatusResult& out) {
    read_object(j, {"paru","yay","chosen","elevation","notes"});
    read_field(j, "paru", out.paru);
    read_field(j, "yay", out.yay);
    read_field(j, "chosen", out.chosen);
    read_field(j, "elevation", out.elevation);
    read_field(j, "notes", out.notes);
}
void to_json(nlohmann::json& j, const SetupStatusResult& value) {
    j = {{"paru", value.paru}, {"yay", value.yay}, {"chosen", value.chosen},
         {"elevation", value.elevation}, {"notes", value.notes}};
    SetupStatusResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, NotifierCheckParams& out) {
    static_cast<void>(out);
    read_object(j, {});
}

void to_json(nlohmann::json& j, const NotifierCheckParams& value) {
    static_cast<void>(value);
    j = nlohmann::json::object();
    NotifierCheckParams checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, NotifierCheckResult& out) {
    read_object(j, {"updateCount","notificationSent","reason"});
    read_field(j, "updateCount", out.updateCount);
    read_field(j, "notificationSent", out.notificationSent);
    read_field(j, "reason", out.reason);
}

void to_json(nlohmann::json& j, const NotifierCheckResult& value) {
    j = nlohmann::json::object();
    write_field(j, "updateCount", value.updateCount);
    write_field(j, "notificationSent", value.notificationSent);
    write_field(j, "reason", value.reason);
    NotifierCheckResult checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobProgressNotification& out) {
    read_object(j, {"jobId", "sequence", "phase", "current", "total", "unit"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "sequence", out.sequence);
    if (out.sequence == 0) throw std::invalid_argument("invalid event sequence");
    auto progress = j;
    progress.erase("jobId");
    progress.erase("sequence");
    out.progress = progress.get<Progress>();
}

void to_json(nlohmann::json& j, const JobProgressNotification& value) {
    j = value.progress;
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    JobProgressNotification checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobLogNotification& out) {
    read_object(j, {"jobId","sequence","line"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "sequence", out.sequence);
    if (out.sequence == 0) throw std::invalid_argument("invalid event sequence");
    read_field(j, "line", out.line);
    if (out.line.size() > 4096) throw std::invalid_argument("log line too long");
}

void to_json(nlohmann::json& j, const JobLogNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "line", value.line);
    JobLogNotification checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobQuestionNotification& out) {
    read_object(j, {"jobId","sequence","question"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "sequence", out.sequence);
    if (out.sequence == 0) throw std::invalid_argument("invalid event sequence");
    read_field(j, "question", out.question);
}

void to_json(nlohmann::json& j, const JobQuestionNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "question", value.question);
    JobQuestionNotification checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobAuthStateNotification& out) {
    read_object(j, {"jobId","sequence","state"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "sequence", out.sequence);
    if (out.sequence == 0) throw std::invalid_argument("invalid event sequence");
    read_field(j, "state", out.state);
}

void to_json(nlohmann::json& j, const JobAuthStateNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "state", value.state);
    JobAuthStateNotification checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, JobStateNotification& out) {
    read_object(j, {"jobId","sequence","state"});
    read_field(j, "jobId", out.jobId);
    read_field(j, "sequence", out.sequence);
    if (out.sequence == 0) throw std::invalid_argument("invalid event sequence");
    read_field(j, "state", out.state);
}

void to_json(nlohmann::json& j, const JobStateNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "jobId", value.jobId);
    write_field(j, "sequence", value.sequence);
    write_field(j, "state", value.state);
    JobStateNotification checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, CatalogChangedNotification& out) {
    read_object(j, {"revision"});
    read_field(j, "revision", out.revision);
}

void to_json(nlohmann::json& j, const CatalogChangedNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "revision", value.revision);
    CatalogChangedNotification checked;
    from_json(j, checked);
}

void from_json(const nlohmann::json& j, UpdatesAvailableNotification& out) {
    read_object(j, {"reportId","count"});
    read_field(j, "reportId", out.reportId);
    read_field(j, "count", out.count);
}

void to_json(nlohmann::json& j, const UpdatesAvailableNotification& value) {
    j = nlohmann::json::object();
    write_field(j, "reportId", value.reportId);
    write_field(j, "count", value.count);
    UpdatesAvailableNotification checked;
    from_json(j, checked);
}

namespace {

template <typename Variant>
void read_variant(const nlohmann::json& j, Variant& out, std::initializer_list<std::pair<std::string_view, void (*)(const nlohmann::json&, Variant&)>> cases) {
    if (!j.is_object() || !j.contains("kind") || !j.at("kind").is_string()) throw std::invalid_argument("invalid tagged union");
    const auto kind = j.at("kind").get<std::string>();
    for (auto [tag, parser] : cases) {
        if (tag == kind) {
            auto payload = j;
            payload.erase("kind");
            parser(payload, out);
            return;
        }
    }
    throw std::invalid_argument("unknown tagged union kind");
}

template <typename T, typename Variant>
void parse_case(const nlohmann::json& j, Variant& out) { out.value = j.get<T>(); }

template <typename Variant>
void write_variant(nlohmann::json& j, const Variant& out, std::initializer_list<std::pair<std::string_view, std::size_t>> tags) {
    std::visit([&j](const auto& item) { j = item; }, out.value);
    for (auto [tag, index] : tags) {
        if (index == out.value.index()) { j["kind"] = tag; return; }
    }
    throw std::invalid_argument("invalid tagged union");
}

}


void from_json(const nlohmann::json& j, Inline& out) {
    read_variant(j, out, {
        {"text", &parse_case<WikiText, Inline>},
        {"link", &parse_case<WikiLink, Inline>},
    });
}
void to_json(nlohmann::json& j, const Inline& out) {
    write_variant(j, out, {
        {"text", 0},
        {"link", 1},
    });
}

void from_json(const nlohmann::json& j, WikiBlock& out) {
    read_variant(j, out, {
        {"heading", &parse_case<WikiHeading, WikiBlock>},
        {"paragraph", &parse_case<WikiParagraph, WikiBlock>},
        {"list", &parse_case<WikiList, WikiBlock>},
        {"code", &parse_case<WikiCode, WikiBlock>},
        {"note", &parse_case<WikiNote, WikiBlock>},
        {"warning", &parse_case<WikiWarning, WikiBlock>},
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

void from_json(const nlohmann::json& j, QuestionChoice& out) {
    read_variant(j, out, {
        {"variant", &parse_case<VariantChoices, QuestionChoice>},
        {"provider", &parse_case<ProviderQuestion, QuestionChoice>},
        {"build_review", &parse_case<BuildReviewQuestion, QuestionChoice>},
        {"conflict", &parse_case<ConflictQuestion, QuestionChoice>},
        {"news_ack", &parse_case<NewsAckQuestion, QuestionChoice>},
        {"git_redirect", &parse_case<GitRedirectQuestion, QuestionChoice>},
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

}
