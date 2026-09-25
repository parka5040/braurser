#include "services.hpp"
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/cache/database.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/domain/variants.hpp>
#include <braurser/validate/validate.hpp>
#include <giomm/file.h>
#include <glibmm/main.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <span>
#include <cstdio>
#include <format>
#include <map>
#include <pwd.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::app {
namespace {
protocol::ErrorObject failure(int code = protocol::kInternalError) {
    return {code, "error.generic_title"};
}

protocol::PackageRef ref(const catalog::SearchHit& hit) {
    std::string source;
    switch (hit.source) {
    case domain::SourceKind::Official: source = "official"; break;
    case domain::SourceKind::Community: source = "community"; break;
    case domain::SourceKind::Git: source = "git"; break;
    case domain::SourceKind::Outside: source = "outside"; break;
    }
    return {{source}, hit.repo, hit.name};
}

protocol::PackageSummary summary(const catalog::PackageDetails& item) {
    const auto& hit = item.summary;
    protocol::PackageSummary result{ref(hit), hit.version, hit.description, hit.installed};
    result.installedVersion = hit.installed_version;
    if (item.installed) result.installedSize = item.installed->size;
    if (item.repo_record) result.downloadSize = item.repo_record->download_size;
    result.appName = hit.app_name;
    result.iconPath = hit.icon_path;
    if (hit.source == domain::SourceKind::Official) {
        result.alsoIn = item.also_in;
        result.shadowed = item.shadowed;
    }
    return result;
}

std::string iso_time(std::chrono::system_clock::time_point time) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(time));
}

bool compatible_catalog(const std::filesystem::path& path) {
    try {
        cache::Database db(path, SQLITE_OPEN_READONLY);
        auto version = db.prepare("PRAGMA user_version");
        return version.step() && version.integer(0) == 2;
    } catch (const std::exception&) { return false; }
}

std::chrono::system_clock::time_point file_age(const std::filesystem::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0077) != 0) return {};
    return std::chrono::system_clock::from_time_t(info.st_mtime);
}

protocol::WikiDocument convert_document(const wiki::Document& input, bool stale) {
    protocol::WikiDocument output;
    output.title = input.title;
    output.source_url = input.source_url;
    output.fetched_at = iso_time(input.fetched_at);
    output.stale = stale;
    output.attribution_key = "wiki.attribution";
    auto convert_inlines = [](const std::vector<wiki::Inline>& values) {
        std::vector<protocol::Inline> result;
        for (const auto& value : values) {
            if (const auto* text = std::get_if<wiki::Text>(&value))
                result.push_back({protocol::WikiText{text->s, text->emphasis, text->strong, text->code}});
            else {
                const auto& link = std::get<wiki::Link>(value);
                result.push_back({protocol::WikiLink{link.label, link.href}});
            }
        }
        return result;
    };
    for (const auto& block : input.blocks) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, wiki::Heading>) output.blocks.push_back({protocol::WikiHeading{static_cast<std::uint32_t>(value.level), value.text}});
            else if constexpr (std::is_same_v<T, wiki::Paragraph>) output.blocks.push_back({protocol::WikiParagraph{convert_inlines(value.inlines)}});
            else if constexpr (std::is_same_v<T, wiki::Code>) output.blocks.push_back({protocol::WikiCode{value.text}});
            else if constexpr (std::is_same_v<T, wiki::Note>) output.blocks.push_back({protocol::WikiNote{convert_inlines(value.inlines)}});
            else if constexpr (std::is_same_v<T, wiki::Warning>) output.blocks.push_back({protocol::WikiWarning{convert_inlines(value.inlines)}});
            else {
                protocol::WikiList list{value.ordered};
                for (const auto& item : value.items) list.items.push_back(convert_inlines(item));
                output.blocks.push_back({std::move(list)});
            }
        }, block);
    }
    for (const auto& section : input.sections)
        output.sections.push_back({static_cast<std::uint32_t>(section.level), section.title});
    return output;
}

protocol::NewsItem convert_news(const news::Item& item) {
    return {item.title, item.link, item.published, item.requires_manual_intervention, false};
}

protocol::RestartLevel restart_level(domain::RestartLevel level) {
    switch (level) {
    case domain::RestartLevel::None: return {"none"};
    case domain::RestartLevel::RestartApp: return {"restart-app"};
    case domain::RestartLevel::RestartService: return {"restart-service"};
    case domain::RestartLevel::Logout: return {"logout"};
    case domain::RestartLevel::Reboot: return {"reboot"};
    case domain::RestartLevel::Unknown: return {"unknown"};
    }
    return {"unknown"};
}

helpers::HelperEnvironment helper_environment(const core::XdgPaths& paths) {
    passwd user{};
    passwd* result = nullptr;
    std::array<char, 16384> buffer{};
    if (getpwuid_r(getuid(), &user, buffer.data(), buffer.size(), &result) != 0 || !result ||
        !result->pw_name || !result->pw_dir) throw std::runtime_error("local user unavailable");
    helpers::HelperEnvironment environment;
    environment.user = result->pw_name;
    environment.home = result->pw_dir;
    environment.runtime_dir = paths.runtime;
    return environment;
}

std::string distro_id() {
    auto file = core::read_bounded("/etc/os-release", 16 * 1024, true);
    if (!file) return "unknown";
    for (std::string_view remaining = *file; !remaining.empty();) {
        const auto end = remaining.find('\n');
        auto line = remaining.substr(0, end);
        if (line.starts_with("ID=")) {
            line.remove_prefix(3);
            if (line.size() >= 2 && line.front() == '"' && line.back() == '"')
                line = line.substr(1, line.size() - 2);
            if (!line.empty() && line.size() <= 64 &&
                std::ranges::all_of(line, [](unsigned char ch) {
                    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
                })) return std::string(line);
            return "unknown";
        }
        if (end == remaining.npos) break;
        remaining.remove_prefix(end + 1);
    }
    return "unknown";
}
}

RealServices::RealServices(core::XdgPaths paths, api::Broadcaster& broadcaster, api::DaemonLog* log)
    : paths_(std::move(paths)), broadcaster_(broadcaster), log_(log), config_(runner_), aur_(http_),
      aur_rpc_(http_), icons_(runner_), store_([this] {
          if (!core::ensure_private_dir(paths_.cache / "braurser"))
              throw std::runtime_error("private catalog cache unavailable");
          return paths_.cache;
      }()), wiki_cache_(paths_.cache, clock_),
      wiki_client_(http_, clock_, &wiki_cache_), connectivity_(nm_, http_, clock_), news_(http_),
      official_(runner_), elevation_(agent_probe_) {
    settings_ = {{"auto"}, {"auto"}, {"daily"}, true, false, true, false, {"system"}};
    auto config_dir = paths_.config / "braurser";
    if (core::ensure_private_dir(config_dir)) {
        const auto path = config_dir / "settings.json";
        std::optional<std::string> loaded;
        if (file_age(path) != std::chrono::system_clock::time_point{}) {
            if (auto contents = core::read_bounded(path, 8192)) loaded = std::move(*contents);
        }
        if (loaded) {
            try { settings_ = nlohmann::json::parse(*loaded).get<protocol::Settings>(); }
            catch (const std::exception&) { settings_ = {{"auto"}, {"auto"}, {"daily"}, true, false, true, false, {"system"}}; }
        }
    }
    auto configured = config_.read();
    if (configured) repos_ = std::move(*configured);
}

RealServices::~RealServices() {
    debounce_.disconnect();
    if (sync_monitor_) sync_monitor_->cancel();
    if (local_monitor_) local_monitor_->cancel();
    if (refresh_worker_.joinable()) refresh_worker_.join();
    if (updates_worker_.joinable()) updates_worker_.join();
    if (preview_worker_.joinable()) preview_worker_.join();
}

void RealServices::startup_refresh() {
    const auto age = file_age(paths_.cache / "braurser" / "catalog.sqlite");
    if (age == std::chrono::system_clock::time_point{} ||
        !compatible_catalog(paths_.cache / "braurser/catalog.sqlite") ||
        clock_.now() - age > std::chrono::hours(24))
        static_cast<void>(refresh({}));
}

void RealServices::start_monitors() {
    if (!repos_) return;
    auto schedule = [this](const auto&, const auto&, auto) {
        debounce_.disconnect();
        debounce_ = Glib::signal_timeout().connect([this] {
            static_cast<void>(refresh({}));
            return false;
        }, 2000);
    };
    try {
        sync_monitor_ = Gio::File::create_for_path((repos_->db_path / "sync").string())->monitor_directory();
        local_monitor_ = Gio::File::create_for_path((repos_->db_path / "local").string())->monitor_directory();
        sync_monitor_->signal_changed().connect(schedule);
        local_monitor_->signal_changed().connect(schedule);
    } catch (const Glib::Error&) {}
}

api::ServiceResult<protocol::HelloResult> RealServices::hello(const protocol::HelloParams& params) {
    if (params.apiVersion != protocol::kApiVersion) return std::unexpected(failure(protocol::kUnsupportedCapability));
    protocol::Capabilities capabilities;
    capabilities.helper = {"auto"};
    capabilities.elevation = {"unavailable"};
    capabilities.init = {"unknown"};
    std::error_code init_error;
    if (std::filesystem::is_directory("/run/systemd/system", init_error)) capabilities.init = {"systemd"};
    else if (std::filesystem::is_directory("/run/openrc", init_error)) capabilities.init = {"openrc"};
    capabilities.distro_id = distro_id();
    if (repos_) capabilities.repos = repos_->names;
    auto helper = helper_locator_.select(settings_.helper.value, helper_environment(paths_), runner_);
    if (log_) log_->write("info", "helper", "selected", {{"name", helper ? (*helper)->name() : "unavailable"}});
    if (helper) {
        capabilities.helper = {std::string((*helper)->name())};
        auto version = (*helper)->version();
        if (version) capabilities.helper_version = *version;
    }
    return protocol::HelloResult{"0.1.0", protocol::kApiVersion, capabilities.helper, capabilities,
                                 {protocol::UiPreference{"gtk"}, protocol::UiPreference{"qt"}}};
}

api::ServiceResult<protocol::StatusResult> RealServices::status(const protocol::StatusParams&) {
    protocol::StatusResult result;
    switch (connectivity_.check()) {
    case connectivity::Status::Online: result.connectivity = {"online"}; break;
    case connectivity::Status::Offline: result.connectivity = {"offline"}; break;
    case connectivity::Status::Unknown: result.connectivity = {"unknown"}; break;
    }
    const auto age = file_age(paths_.cache / "braurser" / "catalog.sqlite");
    if (age != std::chrono::system_clock::time_point{} && compatible_catalog(paths_.cache / "braurser/catalog.sqlite")) {
        result.catalogAge = static_cast<std::uint64_t>(std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::seconds>(clock_.now() - age).count()));
        result.catalogAgeSeconds = result.catalogAge;
    }
    result.catalogState = refresh_busy_ ? "refreshing" : !result.catalogAge ? "missing" :
        *result.catalogAge > 24 * 3600 ? "stale" : "ready";
    if (result.catalogAge) {
        try {
            cache::Database db(paths_.cache / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
            auto rows = db.prepare("SELECT id,kind,fingerprint,etag,last_modified FROM sources ORDER BY id");
            while (rows.step()) {
                protocol::CatalogSourceStatus source;
                source.id = rows.text(0).value_or("");
                source.status = rows.text(1).value_or("");
                source.reasonCode = rows.text(2);
                if (source.reasonCode && source.reasonCode->empty()) source.reasonCode.reset();
                source.packageCount = std::stoull(rows.text(3).value_or("0"));
                source.skippedCount = std::stoull(rows.text(4).value_or("0"));
                result.sources.push_back(std::move(source));
            }
        } catch (const std::exception&) { return std::unexpected(failure()); }
    }
    const auto selection = elevation_.select(elevation_environment());
    switch (selection.method) {
    case elevation::Method::Pkexec: result.elevationMethod = {"pkexec"}; break;
    case elevation::Method::SudoAskpass: result.elevationMethod = {"sudo-askpass"}; break;
    case elevation::Method::Unavailable: result.elevationMethod = {"unavailable"}; break;
    }
    if (log_) log_->write("info", "elevation", "selected", {{"method", result.elevationMethod.value}});
    return result;
}

api::ServiceResult<protocol::SearchResult> RealServices::search(const protocol::SearchParams& params) {
    catalog::SearchQuery query;
    query.text = params.text;
    query.limit = params.limit;
    query.cursor = params.cursor;
    query.sort = params.sort.value == "name" ? catalog::SortOrder::Name : catalog::SortOrder::Relevance;
    query.filters.category = params.category;
    query.filters.installed = params.filters.installed;
    query.filters.explicit_only = params.filters.explicit_;
    query.filters.dependency_only = params.filters.deps;
    query.filters.orphans = params.filters.orphans;
    query.filters.foreign = params.filters.foreign;
    query.filters.native = params.filters.native;
    query.filters.upgradable = params.filters.upgradable;
    query.filters.aur_only = params.filters.aurOnly;
    query.filters.repo_only = params.filters.repoOnly;
    query.filters.apps_only = params.filters.appsOnly;
    if (params.filters.sourceRepo) query.filters.repos.push_back(*params.filters.sourceRepo);
    if (params.filters.group) query.filters.groups.push_back(*params.filters.group);
    protocol::SearchResult result;
    const bool compatible = compatible_catalog(paths_.cache / "braurser/catalog.sqlite");
    if (!compatible) {
        result.catalogState = "refreshing";
        return result;
    }
    auto page = store_.search(query);
    if (!page) return std::unexpected(failure(page.error().code == "invalid-query" ? protocol::kInvalidParams : protocol::kInternalError));
    for (const auto& hit : page->hits) {
        auto detail = store_.package(hit.id);
        if (!detail || !*detail) return std::unexpected(failure());
        result.packages.push_back(summary(**detail));
    }
    result.nextCursor = page->next_cursor;
    result.stale = refresh_busy_.load();
    result.catalogState = result.stale ? "refreshing" : "ready";
    return result;
}

api::ServiceResult<protocol::FacetsResult> RealServices::facets(const protocol::FacetsParams&) {
    if (!compatible_catalog(paths_.cache / "braurser/catalog.sqlite"))
        return std::unexpected(failure(protocol::kBusy));
    try {
        cache::Database db(paths_.cache / "braurser/catalog.sqlite", SQLITE_OPEN_READONLY);
        protocol::FacetsResult result;
        auto repos = db.prepare("SELECT repo,count(*) FROM packages WHERE source_kind='official' GROUP BY repo ORDER BY repo LIMIT 256");
        while (repos.step()) result.repos.push_back({repos.text(0).value_or(""), static_cast<std::uint64_t>(repos.integer(1))});
        auto groups = db.prepare("SELECT name,count(*) FROM \"groups\" GROUP BY name ORDER BY name LIMIT 256");
        while (groups.step()) result.groups.push_back({groups.text(0).value_or(""), static_cast<std::uint64_t>(groups.integer(1))});
        auto categories = db.prepare("SELECT category.value,count(DISTINCT a.package_id) FROM apps a, json_each(a.categories_json) category WHERE category.type='text' GROUP BY category.value ORDER BY category.value LIMIT 256");
        while (categories.step()) result.categories.push_back({categories.text(0).value_or(""), static_cast<std::uint64_t>(categories.integer(1))});
        return result;
    } catch (const std::exception&) { return std::unexpected(failure()); }
}

api::ServiceResult<protocol::BuildFilesResult> RealServices::build_files(const protocol::BuildFilesParams& params) {
    auto item = package(params.packageId);
    if (!item || !item->community_record) return std::unexpected(failure(protocol::kInvalidParams));
    const auto name = domain::PackageName::make(item->summary.name);
    if (!name) return std::unexpected(failure(protocol::kInvalidParams));
    auto helper = helper_locator_.select(settings_.helper.value, helper_environment(paths_), runner_);
    if (!helper) return std::unexpected(failure(protocol::kUnsupportedCapability));
    if (log_) log_->write("info", "helper", "selected", {{"name", (*helper)->name()}});
    auto spec = (*helper)->build_print_build_file_spec(*name);
    spec.timeout = std::chrono::seconds(30);
    spec.max_stdout = 256 * 1024;
    spec.max_stderr = 64 * 1024;
    auto output = runner_.run(spec);
    if (!output) return std::unexpected(failure());
    auto text = (*helper)->parse_build_file(*output);
    if (!text || !protocol::valid_utf8(*text)) return std::unexpected(failure(protocol::kUntrustedInput));
    protocol::BuildFilesResult result;
    result.packageBase = item->community_record->package_base;
    result.files.push_back({"PKGBUILD", *text});
    return result;
}

std::optional<catalog::PackageDetails> RealServices::package(std::string_view id) {
    std::size_t slash = 0;
    if (!br_split_target(id.data(), id.size(), &slash)) return std::nullopt;
    const std::optional<std::string_view> repo = slash < id.size() ?
        std::optional(id.substr(0, slash)) : std::nullopt;
    const bool community = repo && (*repo == "aur" || *repo == "community");
    const auto name = repo ? id.substr(slash + 1) : id;
    catalog::SearchQuery query{std::string(name), {}, catalog::SortOrder::Relevance, {}, 200};
    if (repo && !community) query.filters.repos.push_back(std::string(*repo));
    auto page = store_.search(query);
    if (!page) return std::nullopt;
    for (const auto& hit : page->hits) {
        if (hit.name != name || (community && hit.source != domain::SourceKind::Community) ||
            (repo && !community && (!hit.repo || *hit.repo != *repo))) continue;
        auto detail = store_.package(hit.id);
        if (detail && *detail) return std::move(**detail);
    }
    return std::nullopt;
}

api::ServiceResult<protocol::GetResult> RealServices::get(const protocol::GetParams& params) {
    auto item = package(params.packageId);
    if (!item) return std::unexpected(failure(protocol::kInvalidParams));
    protocol::GetResult result;
    const auto& url = item->repo_record ? item->repo_record->url : item->community_record->url;
    std::optional<std::string> safe_url;
    try {
        protocol::validate_wire(nlohmann::json{{"url", url}});
        safe_url = url;
    } catch (const std::invalid_argument&) {}
    result.details = {ref(item->summary), item->summary.version, item->summary.description,
                      item->repo_record ? item->repo_record->depends : item->community_record->depends, safe_url};
    if (item->repo_record) {
        result.details.alsoIn = item->also_in;
        result.details.shadowed = item->shadowed;
    }
    result.installedState = item->installed ? "installed" : "not-installed";
    result.source = result.details.package.source;
    if (item->app) {
        const auto& app = *item->app;
        protocol::AppMetadata metadata;
        metadata.appstream_id = app.appstream_id;
        metadata.name = app.name;
        metadata.summary = app.summary;
        metadata.description_paragraphs = app.description_paragraphs;
        metadata.icon_path = app.icon_path;
        if (app.icon_path && std::filesystem::path(*app.icon_path).extension() == ".jxl") {
            auto converted = icons_.convert(*app.icon_path, paths_.cache);
            metadata.icon_path = converted ? std::optional(converted->string()) : std::nullopt;
        }
        metadata.categories = app.categories;
        if (app.launchable) metadata.launchables.push_back(*app.launchable);
        for (const auto& shot : app.screenshots) metadata.screenshots.push_back({shot.url, shot.caption});
        result.appMetadata = std::move(metadata);
    }
    return result;
}

api::ServiceResult<protocol::VariantsResult> RealServices::variants(const protocol::VariantsParams& params) {
    auto item = package(params.packageId);
    if (!item) return std::unexpected(failure(protocol::kInvalidParams));
    auto name = domain::PackageName::make(item->summary.name);
    if (!name) return std::unexpected(failure(protocol::kInvalidParams));
    std::string base = item->summary.name;
    constexpr std::array<std::string_view, 7> suffixes{"-bin", "-git", "-appimage", "-nightly", "-beta", "-stable", "-wayland"};
    for (const auto suffix : suffixes) if (base.ends_with(suffix)) { base.resize(base.size() - suffix.size()); break; }
    std::vector<domain::VariantCandidate> candidates;
    std::map<std::string, std::optional<std::string>> candidate_repos;
    std::set<std::string> visited;
    auto add_exact = [&](const std::string& candidate_name) {
        auto page = store_.search({candidate_name, {}, catalog::SortOrder::Relevance, {}, 50});
        if (!page) return;
        for (const auto& hit : page->hits) {
            if (hit.name != candidate_name || !visited.insert(hit.name).second) continue;
            auto valid = domain::PackageName::make(hit.name);
            if (!valid) continue;
            std::string url;
            auto detail = store_.package(hit.id);
            if (detail && *detail) url = (*detail)->repo_record ? (*detail)->repo_record->url : (*detail)->community_record->url;
            candidate_repos.emplace(hit.name, hit.repo);
            candidates.push_back({*valid, hit.source, url});
        }
    };
    add_exact(base);
    for (auto suffix : suffixes) add_exact(base + std::string(suffix));
    protocol::VariantsResult result;
    auto family = domain::resolve_family(*name, candidates);
    if (!family) return result;
    result.choices.urls_differ = family->urls_differ;
    for (std::size_t index = 0; index < family->options.size(); ++index) {
        const auto& option = family->options[index];
        const auto kind = [kind = option.kind]() -> std::string {
            switch (kind) {
            case domain::VariantKind::Standard: return "standard";
            case domain::VariantKind::Bin: return "bin";
            case domain::VariantKind::Git: return "git";
            case domain::VariantKind::AppImage: return "appimage";
            case domain::VariantKind::Nightly: return "nightly";
            case domain::VariantKind::Beta: return "beta";
            case domain::VariantKind::Stable: return "stable";
            case domain::VariantKind::Wayland: return "wayland";
            }
            return "standard";
        }();
        const auto candidate_name = std::string(option.candidate.name.value());
        protocol::PackageRef package_ref{{option.candidate.source == domain::SourceKind::Official ? "official" : "community"},
                                         candidate_repos.at(candidate_name), candidate_name};
        result.choices.options.push_back({package_ref, {kind}, option.development,
                                         family->recommended && *family->recommended == index});
    }
    result.questionRequired = !family->recommended.has_value();
    return result;
}

bool RealServices::build_snapshot(bool forced) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    auto diagnostic = [this](std::string_view source, std::string_view event, std::string_view code,
                             std::size_t count, std::size_t skipped, Clock::time_point start) {
        if (!log_) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        log_->write(event == "failed" ? "warn" : "info", "catalog", event,
                    {{"source", source}, {"code", code}, {"count", std::to_string(count)},
                     {"skipped", std::to_string(skipped)}, {"elapsed_ms", std::to_string(elapsed)}});
    };
    auto configured = config_.read();
    if (!configured) {
        diagnostic("official", "failed", configured.error().code, 0, 0, started);
        return false;
    }
    std::vector<catalog::RepoPackage> packages;
    std::map<std::string, protocol::CatalogSourceStatus> sources;
    for (const auto& repo : configured->names) {
        const auto since = Clock::now();
        diagnostic(repo, "start", "ok", 0, 0, since);
        auto records = sync_.read(configured->db_path, repo);
        if (!records) {
            diagnostic(repo, "failed", records.error().code, 0, 0, since);
            return false;
        }
        sources.emplace("repo/" + repo, protocol::CatalogSourceStatus{"repo/" + repo, "ok",
                        records->rejected ? std::optional<std::string>("invalid-record") : std::nullopt,
                        records->packages.size(), records->rejected});
        diagnostic(repo, "finish", "ok", records->packages.size(), records->rejected, since);
        packages.insert(packages.end(), std::make_move_iterator(records->packages.begin()),
                        std::make_move_iterator(records->packages.end()));
    }
    auto local = local_.read(configured->db_path);
    if (!local) {
        diagnostic("installed", "failed", local.error().code, 0, 0, started);
        return false;
    }
    auto installed = query_.compute(*local, packages);
    sources.emplace("installed", protocol::CatalogSourceStatus{"installed", "ok", {}, installed.packages.size(), 0});
    diagnostic("installed", "finish", "ok", installed.packages.size(), 0, started);
    std::vector<catalog::AurPackage> community;
    bool aur_available = false;
    std::string aur_reason = "dump-unavailable";
    std::string aur_status = "unavailable";
    const auto aur_start = Clock::now();
    diagnostic("community", "start", "ok", 0, 0, aur_start);
    const auto dump_path = paths_.cache / "braurser/downloads/packages-meta-ext-v1.json.gz";
    const auto dump_age = file_age(dump_path);
    if (forced || dump_age == std::chrono::system_clock::time_point{} ||
        clock_.now() - dump_age >= std::chrono::hours(24)) {
        std::expected<std::vector<catalog::AurPackage>, catalog::CatalogError> fetched =
            std::unexpected(catalog::CatalogError{"aur-exception", {}});
        try { fetched = aur_.fetch(paths_.cache); } catch (const std::exception&) {}
        if (fetched) { community = std::move(*fetched); aur_available = true; }
        else {
            aur_reason = fetched.error().code;
            if (aur_reason != "aur-http" && aur_reason != "aur-status") aur_status = "failed";
            diagnostic("community", "failed", aur_reason, 0, 0, aur_start);
        }
    }
    if (!aur_available && dump_age != std::chrono::system_clock::time_point{}) {
        const auto cached = core::read_bounded(dump_path, 64ULL * 1024 * 1024);
        if (cached) {
            std::expected<std::vector<catalog::AurPackage>, catalog::CatalogError> parsed =
                std::unexpected(catalog::CatalogError{"aur-parse-exception", {}});
            try { parsed = catalog::AurDump{}.parse(std::as_bytes(std::span(*cached))); }
            catch (const std::exception&) {}
            if (parsed) { community = std::move(*parsed); aur_available = true; }
            else {
                aur_status = "failed";
                aur_reason = parsed.error().code;
                diagnostic("community", "failed", aur_reason, 0, 0, aur_start);
            }
        } else diagnostic("community", "failed", "cache-read", 0, 0, aur_start);
    }
    sources.emplace("community", protocol::CatalogSourceStatus{"community", aur_available ? "ok" : aur_status,
                    aur_available ? std::nullopt : std::optional(aur_reason), community.size(), 0});
    diagnostic("community", "finish", aur_available ? "ok" : aur_reason, community.size(), 0, aur_start);
    const auto app_start = Clock::now();
    diagnostic("apps", "start", "ok", 0, 0, app_start);
    std::expected<std::vector<catalog::AppMetadata>, catalog::CatalogError> discovered =
        std::unexpected(catalog::CatalogError{"appstream-exception", {}});
    try { discovered = appstream_.discover(); } catch (const std::exception&) {}
    std::vector<catalog::AppMetadata> apps;
    if (discovered) apps = std::move(*discovered);
    else diagnostic("apps", "failed", discovered.error().code, 0, 0, app_start);
    const bool apps_available = discovered && !apps.empty();
    const auto app_reason = discovered ? "appstream-unavailable" : discovered.error().code;
    sources.emplace("apps", protocol::CatalogSourceStatus{"apps", apps_available ? "ok" : discovered ? "unavailable" : "failed",
                    apps_available ? std::nullopt : std::optional<std::string>(app_reason), 0, 0});
    std::set<std::string> names;
    for (const auto& record : packages) names.insert(record.name);
    for (const auto& record : community) names.insert(record.name);
    std::set<std::string> app_names;
    std::erase_if(apps, [&](const auto& record) {
        return !names.contains(record.pkgname) || !app_names.insert(record.pkgname).second;
    });
    sources.at("apps").packageCount = apps.size();
    diagnostic("apps", "finish", apps_available ? "ok" : app_reason, apps.size(), 0, app_start);
    cache::SnapshotBuilder builder(paths_.cache);
    std::size_t retries = 0;
    while (true) {
        if (auto begun = builder.begin(configured->names); !begun) {
            diagnostic("snapshot", "failed", begun.error().code, 0, retries, started);
            return false;
        }
        bool retry = false;
        auto add_records = [&](auto& records, auto source_for) {
            for (auto it = records.begin(); it != records.end(); ++it) {
                if (auto added = builder.add(*it); !added) {
                    auto& source = sources.at(source_for(*it));
                    diagnostic(source.id, "failed", added.error().code, source.packageCount, source.skippedCount, started);
                    if (added.error().code != "invalid-record")
                        throw std::runtime_error("catalog storage failure");
                    source.reasonCode = added.error().code;
                    --source.packageCount;
                    ++source.skippedCount;
                    records.erase(it);
                    retry = true;
                    return;
                }
            }
        };
        add_records(packages, [](const auto& record) { return "repo/" + record.repo; });
        if (!retry) add_records(community, [](const auto&) { return std::string("community"); });
        if (!retry) add_records(installed.packages, [](const auto&) { return std::string("installed"); });
        if (!retry) add_records(apps, [](const auto&) { return std::string("apps"); });
        if (retry) {
            builder.abort();
            if (++retries > 1024) {
                diagnostic("snapshot", "failed", "rejection-limit", 0, retries, started);
                return false;
            }
            continue;
        }
        for (const auto& [id, source] : sources) {
            catalog::SourceState state{id, source.status, source.reasonCode.value_or(""),
                                       std::to_string(source.packageCount), std::to_string(source.skippedCount),
                                       std::chrono::duration_cast<std::chrono::seconds>(clock_.now().time_since_epoch()).count()};
            if (auto recorded = builder.add(state); !recorded) {
                diagnostic(id, "failed", recorded.error().code, source.packageCount, source.skippedCount, started);
                return false;
            }
        }
        if (auto committed = builder.commit(); !committed) {
            diagnostic("snapshot", "failed", committed.error().code, 0, retries, started);
            return false;
        }
        for (const auto& [id, source] : sources)
            diagnostic(id, "finish", source.reasonCode.value_or("ok"), source.packageCount,
                       source.skippedCount, started);
        diagnostic("snapshot", "finish", "ok", packages.size() + community.size(), retries, started);
        return true;
    }
}

elevation::Environment RealServices::elevation_environment() const {
    elevation::Environment environment;
    environment.uid = getuid();
    environment.askpass = BRAURSER_LIBEXEC_DIR "/braurser-askpass-gtk";
    if (const char* session = std::getenv("XDG_SESSION_ID")) environment.session_id = session;
    return environment;
}

api::ServiceResult<protocol::JobIdResult> RealServices::refresh(const protocol::RefreshParams& params) {
    if (refresh_busy_.exchange(true)) return std::unexpected(failure(protocol::kBusy));
    if (log_) log_->write("info", "catalog", "refresh_start");
    bool forced = !params.sources.empty();
    const auto id = "refresh-" + std::to_string(++next_job_);
    if (refresh_worker_.joinable()) refresh_worker_.join();
    refresh_worker_ = std::jthread([this, forced, id] {
        bool success = false;
        try { success = build_snapshot(forced); }
        catch (const std::exception&) {
            if (log_) log_->write("error", "catalog", "refresh_exception", {{"code", "exception"}});
            else std::fputs("braurserd: catalog refresh exception\n", stderr);
        }
        if (log_) log_->write(success ? "info" : "error", "catalog", "refresh_finish",
                               {{"code", success ? "ok" : "failed"}});
        Glib::MainContext::get_default()->invoke([this, success, id] {
            refresh_busy_ = false;
            if (success) broadcaster_.emit<api::CatalogChangedNotificationEvent>({++revision_});
            broadcaster_.emit<api::JobStateNotificationEvent>({id, 1, protocol::JobState{success ? "succeeded" : "failed"}});
            return false;
        });
    });
    return protocol::JobIdResult{id};
}

api::ServiceResult<protocol::WikiGetResult> RealServices::wiki(const protocol::WikiGetParams& params) {
    auto item = package(params.packageId);
    if (!item) return std::unexpected(failure(protocol::kInvalidParams));
    const std::string base = item->repo_record ? item->repo_record->base : item->community_record->package_base;
    const auto result = wiki_client_.lookup(item->app ? item->app->name : "", item->summary.name,
                                            base, params.allowRefresh && connectivity_.check() != connectivity::Status::Offline);
    if (!result.document) return std::unexpected(failure(result.status == wiki::LookupStatus::Error ? protocol::kOffline : protocol::kInvalidParams));
    auto document = convert_document(*result.document, result.stale);
    return protocol::WikiGetResult{document, document.source_url, std::string(wiki::attribution_text),
                                   document.fetched_at, result.stale};
}

api::ServiceResult<protocol::NewsListResult> RealServices::news(const protocol::NewsListParams& params) {
    protocol::NewsListResult result;
    if (!repos_ || !news::arch_news_applicable(repos_->names)) return result;
    auto items = news_.fetch();
    if (!items) return std::unexpected(failure(protocol::kOffline));
    std::size_t start{};
    if (params.cursor) {
        const auto [end, error] = std::from_chars(params.cursor->data(), params.cursor->data() + params.cursor->size(), start);
        if (error != std::errc{} || end != params.cursor->data() + params.cursor->size())
            return std::unexpected(failure(protocol::kInvalidParams));
    }
    if (start > items->size()) return std::unexpected(failure(protocol::kInvalidParams));
    for (std::size_t i = start; i < items->size() && i - start < params.limit; ++i)
        result.items.push_back(convert_news((*items)[i]));
    if (start + result.items.size() < items->size()) result.nextCursor = std::to_string(start + result.items.size());
    return result;
}

void RealServices::run_updates(std::string job, bool request_refresh, catalog::Repositories repos, std::string helper_preference) {
    std::vector<updates::PackageUpdate> official_updates;
    std::vector<updates::PackageUpdate> community_updates;
    std::expected<std::vector<updates::PackageUpdate>, updates::UpdateError> checked =
        std::unexpected(updates::UpdateError{updates::UpdateErrorCode::Unavailable, {}});
    if (request_refresh) {
        auto temp = updates::TempSyncDb::create(runner_, repos.db_path, paths_.runtime);
        if (temp) checked = official_.check(temp->path());
        else if (log_) log_->write("error", "updates", "tempdb_failed", {{"code", std::to_string(static_cast<int>(temp.error().code))}});
    } else checked = official_.check(repos.db_path);
    if (!checked) {
        { if (log_) log_->write("error", "updates", "check_failed", {{"stage", "official-check"}}); Glib::MainContext::get_default()->invoke([this, job] { updates_busy_ = false; broadcaster_.emit<api::JobStateNotificationEvent>({job, 1, protocol::JobState{"failed"}}); return false; }); }
        return;
    }
    official_updates = std::move(*checked);
    const auto helper = helper_locator_.select(helper_preference,
        helper_environment(paths_), runner_);
    if (helper) {
        auto local = local_.read(repos.db_path);
        if (!local) {
            { if (log_) log_->write("error", "updates", "check_failed", {{"stage", "local-db"}}); Glib::MainContext::get_default()->invoke([this, job] { updates_busy_ = false; broadcaster_.emit<api::JobStateNotificationEvent>({job, 1, protocol::JobState{"failed"}}); return false; }); }
            return;
        }
        std::vector<catalog::RepoPackage> repository_packages;
        for (const auto& repo : repos.names) {
            auto records = sync_.read(repos.db_path, repo);
            if (!records) {
                { if (log_) log_->write("error", "updates", "check_failed", {{"stage", "sync-db"}}); Glib::MainContext::get_default()->invoke([this, job] { updates_busy_ = false; broadcaster_.emit<api::JobStateNotificationEvent>({job, 1, protocol::JobState{"failed"}}); return false; }); }
                return;
            }
            repository_packages.insert(repository_packages.end(),
                std::make_move_iterator(records->packages.begin()),
                std::make_move_iterator(records->packages.end()));
        }
        auto installed = query_.compute(*local, repository_packages);
        std::set<std::string> foreign(installed.foreign.begin(), installed.foreign.end());
        updates::CommunityUpdateChecker checker(runner_, **helper);
        auto community = checker.check(foreign);
        if (!community) {
            { if (log_) log_->write("error", "updates", "check_failed", {{"stage", "community-check"}}); Glib::MainContext::get_default()->invoke([this, job] { updates_busy_ = false; broadcaster_.emit<api::JobStateNotificationEvent>({job, 1, protocol::JobState{"failed"}}); return false; }); }
            return;
        }
        community_updates = std::move(*community);
    }
    std::optional<updates::RestartReport> advice;
    if (!official_updates.empty()) {
        auto rules = updates::RestartRuleLoader{}.load(BRAURSER_DATA_DIR "/restart-rules.json");
        if (rules) {
            updates::LocalDbFilesSource files(local_, repos.db_path);
            updates::RestartAdvisor advisor(files, consumer_scanner_);
            advice = advisor.advise(official_updates, *rules);
        }
    }
    Glib::MainContext::get_default()->invoke([this, job, official = std::move(official_updates),
                                                community = std::move(community_updates), advice = std::move(advice)] {
        protocol::UpdateReport next;
        next.report_id = "report-" + std::to_string(++next_job_);
        next.checked_at = iso_time(clock_.now());
        next.overall_restart = advice ? restart_level(advice->overall) : protocol::RestartLevel{"unknown"};
        auto restart_for = [&](const protocol::PackageRef& package_ref) {
            protocol::RestartAdviceDTO fallback{package_ref, {"unknown"}, "restart.unknown", {}};
            if (!advice) return fallback;
            auto match = std::ranges::find_if(advice->packages, [&](const auto& item) {
                return item.name.value() == package_ref.name;
            });
            if (match == advice->packages.end()) return fallback;
            return protocol::RestartAdviceDTO{package_ref, restart_level(match->level),
                match->reason_key.empty() ? fallback.reason_key : match->reason_key, {}};
        };
        for (const auto& update : official) {
            protocol::PackageRef package_ref{{"official"}, {}, update.name};
            next.official.push_back({package_ref, update.from, update.to, {"official"}, restart_for(package_ref)});
        }
        for (const auto& update : community) {
            protocol::PackageRef package_ref{{"community"}, {}, update.name};
            next.community.push_back({package_ref, update.from, update.to, {"community"}, restart_for(package_ref)});
        }
        const std::uint64_t count = next.official.size() + next.community.size() + next.manual.size();
        const std::string report_id = next.report_id;
        report_ = std::move(next);
        last_check_ = clock_.now();
        updates_busy_ = false;
        if (count > 0) broadcaster_.emit<api::UpdatesAvailableNotificationEvent>({report_id, count});
        broadcaster_.emit<api::JobStateNotificationEvent>({job, 1, protocol::JobState{"succeeded"}});
        return false;
    });
}

api::ServiceResult<protocol::JobIdResult> RealServices::check_updates(const protocol::UpdatesCheckParams& params) {
    if (!repos_) return std::unexpected(failure(protocol::kUnsupportedCapability));
    if (updates_busy_.exchange(true)) return std::unexpected(failure(protocol::kBusy));
    const auto id = "check-" + std::to_string(++next_job_);
    if (updates_worker_.joinable()) updates_worker_.join();
    updates_worker_ = std::jthread([this, id, refresh = params.refresh,
                                    repos = *repos_, preference = settings_.helper.value] {
        try { run_updates(id, refresh, repos, preference); } catch (const std::exception&) {
            Glib::MainContext::get_default()->invoke([this, id] {
                updates_busy_ = false;
                broadcaster_.emit<api::JobStateNotificationEvent>({id, 1, protocol::JobState{"failed"}});
                return false;
            });
        }
    });
    return protocol::JobIdResult{id};
}

api::ServiceResult<protocol::UpdatesOptionsResult> RealServices::update_options(const protocol::UpdatesOptionsParams& params) {
    if (!report_ || report_->report_id != params.reportId) return std::unexpected(failure(protocol::kStalePlan));
    protocol::UpdatesOptionsResult options;
    options.officialAction = "update-system";
    for (const auto& item : report_->community) options.communityIndividualIds.push_back(item.package.name);
    for (const auto& item : report_->manual) options.manualIndividualIds.push_back(item.package.name);
    return options;
}

std::optional<manual::Preview> RealServices::preview_result(std::string_view id) const {
    if (last_preview_ && last_preview_->preview_id == id) return last_preview_;
    return std::nullopt;
}

std::optional<protocol::UpdateReport> RealServices::latest_report() const { return report_; }

api::ServiceResult<protocol::JobIdResult> RealServices::preview(const protocol::ManualPreviewParams& params) {
    if (!manual::validate_git_url(params.httpsUrl) ||
        (params.scriptPath != "install.sh" && params.scriptPath != "Makefile" && !params.scriptPath.empty()))
        return std::unexpected(failure(protocol::kUntrustedInput));
    if (preview_busy_.exchange(true)) return std::unexpected(failure(protocol::kBusy));
    if (preview_worker_.joinable()) preview_worker_.join();
    const auto id = "preview-" + std::to_string(++next_job_);
    const auto selected = params.scriptPath.empty() ? std::nullopt :
        std::optional(params.scriptPath == "Makefile" ? manual::EntryKind::MakeInstall : manual::EntryKind::Script);
    preview_worker_ = std::jthread([this, id, url = params.httpsUrl, selected] {
        std::optional<manual::Preview> result;
        try {
            auto fetched = manual::fetch_preview(git_http_, url, paths_.cache, id, "HEAD", selected);
            if (fetched) result = std::move(*fetched);
        } catch (const std::exception&) {}
        Glib::MainContext::get_default()->invoke([this, id, result = std::move(result)]() mutable {
            const bool success = result.has_value();
            if (success) last_preview_ = std::move(result);
            preview_busy_ = false;
            broadcaster_.emit<api::JobStateNotificationEvent>({id, 1, {success ? "succeeded" : "failed"}});
            return false;
        });
    });
    return protocol::JobIdResult{id};
}

api::ServiceResult<protocol::ManualListResult> RealServices::manual_list(const protocol::ManualListParams&) {
    protocol::ManualListResult result;
    const auto directory = paths_.state / "braurser/manual/receipts";
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return result;
    struct stat directory_info{};
    if (lstat(directory.c_str(), &directory_info) != 0 || !S_ISDIR(directory_info.st_mode) ||
        directory_info.st_uid != getuid() || (directory_info.st_mode & 0777) != 0700)
        return std::unexpected(failure());
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error || ++count > 10000) return std::unexpected(failure());
        if (entry.path().extension() != ".json") continue;
        auto contents = core::read_bounded(entry.path(), 1024 * 1024);
        if (!contents) continue;
        auto json = nlohmann::json::parse(*contents, nullptr, false);
        if (!json.is_object() || !json.contains("id") || !json.contains("name") || !json.contains("commit") ||
            !json.contains("scope") || !json.contains("installed_at")) continue;
        try {
            protocol::ManualReceipt receipt;
            receipt.receipt_id = json.at("id").get<std::string>();
            receipt.name = json.at("name").get<std::string>();
            receipt.commit = json.at("commit").get<std::string>();
            receipt.scope = {json.at("scope").get<std::string>()};
            receipt.installed_at = iso_time(std::chrono::system_clock::from_time_t(json.at("installed_at").get<std::time_t>()));
            receipt.removable = true;
            if (json.contains("launcher_ids")) receipt.launcher_ids = json.at("launcher_ids").get<std::vector<std::string>>();
            nlohmann::json(receipt).get<protocol::ManualReceipt>();
            result.receipts.push_back(std::move(receipt));
        } catch (const std::exception&) {}
    }
    return result;
}

api::ServiceResult<protocol::UnmanagedListResult> RealServices::unmanaged(const protocol::UnmanagedListParams&) {
    auto entries = launcher_scanner_.scan_system(runner_, paths_.state);
    if (!entries) return std::unexpected(failure());
    protocol::UnmanagedListResult result;
    result.scanTime = iso_time(clock_.now());
    for (const auto& item : *entries)
        result.entries.push_back({item.desktop_id, item.name, item.exec_display, item.path.string(), {item.owner}});
    return result;
}

api::ServiceResult<protocol::SettingsGetResult> RealServices::settings_get(const protocol::SettingsGetParams&) {
    return protocol::SettingsGetResult{settings_.ui, settings_.notifications, settings_.check_frequency, {"unavailable"}};
}

api::ServiceResult<protocol::SettingsSetResult> RealServices::settings_set(const protocol::SettingsSetParams& params) {
    auto next = settings_;
    const auto& patch = params.changedFields;
    if (patch.ui) next.ui = *patch.ui;
    if (patch.helper) next.helper = *patch.helper;
    if (patch.check_frequency) next.check_frequency = *patch.check_frequency;
    if (patch.notifications) next.notifications = *patch.notifications;
    if (patch.autostart) next.autostart = *patch.autostart;
    if (patch.review_builds) next.review_builds = *patch.review_builds;
    if (patch.show_log) next.show_log = *patch.show_log;
    if (patch.theme) next.theme = *patch.theme;
    const auto bytes = nlohmann::json(next).dump();
    if (bytes.size() > 8192 || !core::ensure_private_dir(paths_.config / "braurser") ||
        !core::atomic_write(paths_.config / "braurser/settings.json", bytes, 0600))
        return std::unexpected(failure());
    settings_ = next;
    return protocol::SettingsSetResult{settings_};
}

api::ServiceResult<protocol::ClearCacheResult> RealServices::clear_cache(const protocol::ClearCacheParams&) {
    if (refresh_busy_ || preview_busy_) return std::unexpected(failure(protocol::kBusy));
    const auto root = paths_.cache / "braurser";
    std::uint64_t freed = 0;
    for (const auto& name : {"downloads", "icons", "wiki"}) {
        const auto subtree = root / name;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(subtree, error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error || !std::filesystem::is_directory(status)) return std::unexpected(failure());
        std::size_t entries = 0;
        for (std::filesystem::recursive_directory_iterator it(subtree, error), end; it != end && !error; it.increment(error)) {
            if (++entries > 100000) return std::unexpected(failure(protocol::kBusy));
            const auto entry_status = it->symlink_status(error);
            if (error) break;
            if (std::filesystem::is_regular_file(entry_status)) {
                const auto size = it->file_size(error);
                if (error) break;
                freed = size > UINT64_MAX - freed ? UINT64_MAX : freed + size;
            }
        }
        if (error || !core::remove_tree_confined(root, name)) return std::unexpected(failure());
    }
    return protocol::ClearCacheResult{freed};
}

api::ServiceResult<protocol::RestartStateResult> RealServices::restart_state(const protocol::RestartStateParams&) {
    const auto path = paths_.state / "braurser/restart.json";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) return std::unexpected(failure());
        return protocol::RestartStateResult{};
    }
    if (file_age(path) == std::chrono::system_clock::time_point{}) return std::unexpected(failure());
    auto bytes = core::read_bounded(path, 8192);
    if (!bytes) return std::unexpected(failure());
    try { return nlohmann::json::parse(*bytes).get<protocol::RestartStateResult>(); }
    catch (const std::exception&) { return std::unexpected(failure()); }
}

api::ServiceResult<protocol::EmptyResult> RealServices::restart_acknowledge(const protocol::RestartAcknowledgeParams&) {
    const auto current = restart_state({});
    if (!current) return std::unexpected(current.error());
    if (!core::ensure_private_dir(paths_.state / "braurser") ||
        !core::atomic_write(paths_.state / "braurser/restart.json",
                            nlohmann::json(protocol::RestartStateResult{}).dump(), 0600))
        return std::unexpected(failure());
    return protocol::EmptyResult{};
}

api::ServiceResult<protocol::SetupStatusResult> RealServices::setup_status(const protocol::SetupStatusParams&) {
    protocol::SetupStatusResult result;
    for (const auto& preference : {"paru", "yay"}) {
        auto helper = helper_locator_.select(preference, helper_environment(paths_), runner_);
        auto& target = std::string_view(preference) == "paru" ? result.paru : result.yay;
        target.installed = helper.has_value();
        if (helper) {
            auto version = (*helper)->version();
            if (version) target.version = *version;
            else result.notes.push_back(std::string(preference) + "-version-unavailable");
        }
    }
    if (settings_.helper.value == "paru" && result.paru.installed) result.chosen = {"paru"};
    else if (settings_.helper.value == "yay" && result.yay.installed) result.chosen = {"yay"};
    else if (result.paru.installed) result.chosen = {"paru"};
    else if (result.yay.installed) result.chosen = {"yay"};
    else result.notes.push_back("helper-unavailable");
    switch (elevation_.select(elevation_environment()).method) {
    case elevation::Method::Pkexec: result.elevation = {"pkexec"}; break;
    case elevation::Method::SudoAskpass: result.elevation = {"sudo-askpass"}; break;
    case elevation::Method::Unavailable: result.elevation = {"unavailable"}; result.notes.push_back("elevation-unavailable"); break;
    }
    if (log_) {
        log_->write("info", "helper", "selected", {{"name", result.chosen.value}});
        log_->write("info", "elevation", "selected", {{"method", result.elevation.value}});
    }
    return result;
}

api::ServiceResult<protocol::NotifierCheckResult> RealServices::notifier_check(const protocol::NotifierCheckParams&) {
    protocol::NotifierCheckResult result;
    result.notificationSent = false;
    if (settings_.check_frequency.value == "never") { result.reason = "disabled"; return result; }
    const auto interval = settings_.check_frequency.value == "weekly" ? std::chrono::hours(24 * 7) : std::chrono::hours(24);
    if (last_check_ == std::chrono::system_clock::time_point{} || clock_.now() - last_check_ >= interval)
        static_cast<void>(check_updates({false}));
    if (report_) result.updateCount = report_->official.size() + report_->community.size() + report_->manual.size();
    else result.reason = "pending";
    return result;
}

}
