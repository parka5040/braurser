#include <braurser/presenter/models.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace braurser::presenter {
namespace {
void bounded(std::string_view value, std::size_t limit = 4096) {
    if (value.size() > limit || !protocol::valid_utf8(value) || value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("invalid presenter text");
    }
}
RpcError unsupported() { return {protocol::kUnsupportedCapability, "error.unsupported_body", nlohmann::json::object()}; }
std::size_t impact(std::string_view level) {
    if (level == "reboot") { return 0; }
    if (level == "logout") { return 1; }
    if (level == "restart-app" || level == "restart-service") { return 2; }
    if (level == "none") { return 3; }
    return 4;
}
}
std::string Text::render(const Copy& copy) const { return copy.text(key, args); }
Text error_text(int code) {
    switch (code) {
    case protocol::kOffline: return {"offline.operation_block", {}};
    case protocol::kUnsupportedCapability: return {"error.unsupported_title", {}};
    case protocol::kAuthorizationUnavailable: return {"auth.unavailable_title", {}};
    case protocol::kBusy: return {"error.lock_title", {}};
    case protocol::kStalePlan: return {"confirm.plan_changed", {}};
    case protocol::kUnsafeTransaction: return {"error.generic_title", {}};
    case protocol::kExpiredQuestion: return {"auth.timeout", {}};
    case protocol::kCancelled: return {"activity.canceled", {}};
    case protocol::kUntrustedInput: return {"error.integrity_title", {}};
    case protocol::kSystemUpdateRequired: return {"updates.system_first", {}};
    default: return {"error.generic_title", {}};
    }
}
Shell::Shell(Rpc& rpc) : rpc_(rpc) {}
void Shell::navigate(Location location) {
    bounded(location.id);
    if (!std::isfinite(location.scroll) || location.scroll < 0) { throw std::invalid_argument("invalid scroll position"); }
    if (state_.history.size() == 128) { state_.history.erase(state_.history.begin()); }
    state_.history.push_back(state_.location);
    state_.location = std::move(location);
    changed();
}
void Shell::back() {
    if (state_.history.empty()) { return; }
    state_.location = state_.history.back(); state_.history.pop_back(); changed();
}
void Shell::width(unsigned value) {
    state_.viewport = value < 480 ? Viewport::Narrow : value < 840 ? Viewport::Compact : value < 1080 ? Viewport::Wide : Viewport::Large;
    changed();
}
void Shell::theme(std::string preference, std::optional<bool> desktop_dark) {
    if (preference != "system" && preference != "light" && preference != "dark") { throw std::invalid_argument("invalid theme"); }
    state_.theme = preference;
    state_.resolved_theme = preference == "system" ? (desktop_dark.value_or(false) ? "dark" : "light") : preference;
    changed();
}
void Shell::connected(bool value) { state_.connected = value; changed(); }
void Shell::refresh() {
    call(rpc_, protocol::StatusParams{}, [this](Reply<protocol::StatusResult> reply) {
        if (reply) { state_.connectivity = reply->connectivity.value; state_.active_job = reply->activeJob; state_.connected = true;
            state_.catalog_state = reply->catalogState; state_.catalog_age = reply->catalogAgeSeconds;
            state_.community_available = std::ranges::any_of(reply->sources, [](const auto& source) { return source.id == "community" && source.status != "unavailable" && source.status != "failed"; });
        }
        else { state_.connected = false; }
        changed();
    });
    call(rpc_, protocol::JobsListParams{{}, 100}, [this](Reply<protocol::JobsListResult> reply) {
        if (reply) {
            state_.queue_count = static_cast<std::size_t>(std::ranges::count_if(reply->jobs, [](const auto& job) { return job.state.value == "queued" || job.state.value == "needs-attention"; }));
            changed();
        }
    });
    call(rpc_, protocol::RestartStateParams{}, [this](Reply<protocol::RestartStateResult> reply) {
        if (reply) { state_.restart = *reply; changed(); }
    });
}
void Shell::acknowledge_restart() {
    call(rpc_, protocol::RestartAcknowledgeParams{}, [this](Reply<protocol::EmptyResult> reply) {
        if (reply) { state_.restart = {}; changed(); }
    });
}
std::string_view source_key(const protocol::PackageRef& package) {
    const auto& source = package.source.value;
    if (source == "official" || source == "community" || source == "git") { return source; }
    if (source == "outside" || source == "foreign") { return "foreign"; }
    throw std::invalid_argument("unknown package source");
}
unsigned app_grid_columns(unsigned content_width) {
    return content_width >= 1200 ? 4U : content_width >= 900 ? 3U : content_width >= 600 ? 2U : 1U;
}
Discover::Discover() {
    for (const auto* id : {"AudioVideo", "Development", "Education", "Game", "Graphics", "Network", "Office", "Science", "Settings", "System", "Utility"}) {
        state_.categories.push_back({id, {std::string("category.") + id, {}}, "braurser-package-symbolic", {}});
    }
}
void Discover::collection(std::uint64_t revision, std::span<const std::string> shipped, std::span<const std::string> available,
                          std::span<const protocol::PackageSummary> metadata) {
    if (shipped.size() > 4096 || available.size() > 100000 || metadata.size() > 100000) { throw std::length_error("collection exceeds limit"); }
    std::map<std::string, const protocol::PackageSummary*> packages;
    for (const auto& package : metadata) {
        protocol::validate_wire(nlohmann::json(package));
        packages.emplace(package.package.name, &package);
    }
    state_.apps.clear();
    std::set<std::string> current(available.begin(), available.end());
    state_.app_ids.clear();
    std::set<std::string> seen;
    for (const auto& id : shipped) {
        bounded(id, 512);
        if (current.contains(id) && seen.insert(id).second) {
            state_.app_ids.push_back(id);
            if (const auto found = packages.find(id); found != packages.end()) { state_.apps.push_back(*found->second); }
        }
    }
    state_.collection_revision = revision;
    changed();
}
void Discover::availability(bool metadata, bool cached, bool offline) {
    state_.metadata_available = metadata; state_.cached = cached; state_.offline = offline;
    state_.notice = offline ? std::optional<Text>({cached ? "offline.local_ready" : "offline.empty", {}}) : std::nullopt;
    changed();
}
std::vector<ActiveFilter> active_filters(const FilterSelection& selection) {
    static_cast<void>(map_filters(selection));
    std::vector<ActiveFilter> result;
    const auto add = [&](Text label, auto clear) {
        auto removed = selection; clear(removed); result.push_back({std::move(label), std::move(removed)});
    };
    if (selection.category) { add({"category." + *selection.category, {}}, [](auto& value) { value.category.reset(); }); }
    if (selection.installation != Installation::Any) {
        add({selection.installation == Installation::Installed ? "filter.installed" : "filter.not_installed", {}}, [](auto& value) { value.installation = Installation::Any; });
    }
    if (selection.added_as != AddedAs::Any) { add({selection.added_as == AddedAs::Explicit ? "filter.explicit" : "filter.dependency", {}}, [](auto& value) { value.added_as = AddedAs::Any; }); }
    if (selection.source != SourceFilter::All) { add({selection.source == SourceFilter::Official ? "filter.official" : "filter.community", {}}, [](auto& value) { value.source = SourceFilter::All; }); }
    if (selection.orphans) { add({"filter.orphans", {}}, [](auto& value) { value.orphans = false; }); }
    if (selection.updates) { add({"filter.updates", {}}, [](auto& value) { value.updates = false; }); }
    if (selection.apps_only) { add({"filter.apps_only", {}}, [](auto& value) { value.apps_only = false; }); }
    for (const auto& repository : selection.repositories) {
        add({"filter.category_chip", {{"category", repository}}}, [&repository](auto& value) { std::erase(value.repositories, repository); });
    }
    for (const auto& group : selection.groups) {
        add({"filter.category_chip", {{"category", group}}}, [&group](auto& value) { std::erase(value.groups, group); });
    }
    return result;
}
FilterMapping map_filters(const FilterSelection& selection) {
    if ((selection.installation != Installation::Any && selection.installation != Installation::Installed && selection.installation != Installation::NotInstalled) ||
        (selection.added_as != AddedAs::Any && selection.added_as != AddedAs::Explicit && selection.added_as != AddedAs::Dependency) ||
        (selection.source != SourceFilter::All && selection.source != SourceFilter::Official && selection.source != SourceFilter::Community)) {
        throw std::invalid_argument("unknown filter selection");
    }
    FilterMapping result;
    auto& filter = result.filters;
    const bool implied = selection.added_as != AddedAs::Any || selection.orphans;
    result.not_installed_enabled = !implied;
    if (implied) { result.disabled_reason = Text{"filter.requires_installed", {}}; }
    filter.installed = implied || selection.installation == Installation::Installed;
    filter.not_installed = !implied && selection.installation == Installation::NotInstalled;
    filter.explicit_ = selection.added_as == AddedAs::Explicit;
    filter.dependency = selection.added_as == AddedAs::Dependency;
    filter.orphans = selection.orphans;
    filter.upgradable = selection.updates;
    filter.apps_only = selection.apps_only;
    filter.repos = selection.repositories;
    filter.groups = selection.groups;
    filter.category = selection.category;
    if (filter.repos.size() > 64 || filter.groups.size() > 64) { throw std::length_error("too many filters"); }
    for (const auto& repo : filter.repos) { if (!br_valid_repo_name(repo.data(), repo.size())) { throw std::invalid_argument("invalid repository"); } }
    for (const auto& group : filter.groups) { bounded(group, 255); }
    if (filter.category) { bounded(*filter.category, 128); }
    if (selection.source == SourceFilter::Official) {
        if (filter.installed) { filter.native = true; } else { filter.repo_only = true; }
    } else if (selection.source == SourceFilter::Community) {
        if (filter.installed) { filter.foreign = true; result.foreign_note = Text{"filter.foreign_note", {}}; }
        else { filter.aur_only = true; }
    }
    result.community_enabled = filter.repos.empty();
    if (!result.community_enabled) { result.disabled_reason = Text{"error.unsupported_body", {}}; }
    return result;
}
Reply<protocol::SearchParams> search_params(const protocol::SearchQuery& query) {
    const auto& filter = query.filters;
    if (filter.not_installed || filter.repos.size() > 1 || filter.groups.size() > 1 || (query.sort.value != "relevance" && query.sort.value != "name") ||
        (filter.native && filter.foreign) || (filter.aur_only && filter.repo_only) ||
        (filter.explicit_ && filter.dependency) || (filter.aur_only && !filter.repos.empty()) || (filter.foreign && !filter.repos.empty())) {
        return std::unexpected(unsupported());
    }
    protocol::CatalogFilters wire;
    wire.installed = filter.installed; wire.explicit_ = filter.explicit_; wire.deps = filter.dependency;
    wire.orphans = filter.orphans; wire.foreign = filter.foreign; wire.native = filter.native;
    wire.upgradable = filter.upgradable; wire.aurOnly = filter.aur_only; wire.repoOnly = filter.repo_only; wire.appsOnly = filter.apps_only;
    if (!filter.repos.empty()) { wire.sourceRepo = filter.repos.front(); }
    if (!filter.groups.empty()) { wire.group = filter.groups.front(); }
    return protocol::SearchParams{query.text, wire, query.cursor, query.limit, query.sort, filter.category};
}
Search::Search(Rpc& rpc, Timer& timer) : rpc_(rpc), timer_(timer) {
    state_.mapping = map_filters(state_.selection); state_.query.filters = state_.mapping.filters;
}
Search::~Search() { if (timer_token_) { timer_.cancel(*timer_token_); } }
void Search::invalidate() {
    if (timer_token_) { timer_.cancel(*timer_token_); timer_token_.reset(); }
    ++state_.generation; state_.query.cursor.reset(); state_.next_cursor.reset(); state_.total.reset(); state_.loading = false;
    pages_.clear(); first_page_ = 0; last_page_ = 0;
    state_.has_previous = false; state_.has_next = false;
    state_.visible_first = 0; state_.visible_last = 0; state_.results.clear();
}
void Search::query(std::string value) {
    bounded(value);
    invalidate(); state_.query.text = std::move(value); state_.error.reset();
    timer_token_ = timer_.after(std::chrono::milliseconds(250), [this, weak = std::weak_ptr<int>(lifetime_)] {
        if (weak.expired()) { return; }
        timer_token_.reset(); submit();
    });
    changed();
}
void Search::filters(FilterSelection selection) {
    auto mapping = map_filters(selection);
    invalidate(); state_.selection = std::move(selection); state_.mapping = std::move(mapping);
    state_.query.filters = state_.mapping.filters; fetch(0);
}
void Search::sort(std::string value) {
    if (value != "relevance" && value != "name") { throw std::invalid_argument("invalid sort"); }
    invalidate(); state_.query.sort.value = std::move(value); fetch(0);
}
void Search::facets() {
    call(rpc_, protocol::FacetsParams{}, [this](Reply<protocol::FacetsResult> reply) {
        if (reply) { state_.facets = *reply; } else { state_.error = error_text(reply.error().code); }
        changed();
    });
}
void Search::submit() { invalidate(); fetch(0); }
void Search::next_page() {
    if (state_.loading || !state_.has_next) { return; }
    fetch(last_page_ + 1);
}
void Search::previous_page() {
    if (state_.loading || !state_.has_previous) { return; }
    fetch(first_page_ - 1, true);
}
void Search::fetch(std::size_t page, bool prepend) {
    auto query = state_.query;
    query.cursor = page < pages_.size() ? pages_[page].cursor : state_.next_cursor;
    if (page == 0 && pages_.empty()) { query.cursor.reset(); }
    const auto params = search_params(query);
    if (!params) { state_.error = error_text(params.error().code); changed(); return; }
    state_.loading = true; state_.error.reset();
    const auto generation = state_.generation;
    call(rpc_, *params, [this, generation, page, prepend, cursor = query.cursor](Reply<protocol::SearchResult> reply) {
        if (generation != state_.generation) { return; }
        state_.loading = false;
        if (!reply) { state_.error = error_text(reply.error().code); }
        else if (reply->packages.size() > 200) { state_.error = Text{"error.generic_title", {}}; }
        else {
            const auto start = page < pages_.size() ? pages_[page].start : page == 0 ? 0 : pages_[page - 1].start + pages_[page - 1].size;
            if (page == pages_.size()) { pages_.push_back({cursor, start, reply->packages.size()}); }
            else { pages_[page].size = reply->packages.size(); }
            if (prepend) {
                state_.results.insert(state_.results.begin(), std::make_move_iterator(reply->packages.begin()), std::make_move_iterator(reply->packages.end()));
                first_page_ = page;
                while (state_.results.size() > 200 && last_page_ > first_page_) {
                    state_.results.resize(state_.results.size() - pages_[last_page_].size); --last_page_;
                }
            } else {
                if (page == 0) { state_.results.clear(); first_page_ = 0; }
                state_.results.insert(state_.results.end(), std::make_move_iterator(reply->packages.begin()), std::make_move_iterator(reply->packages.end()));
                last_page_ = page;
                while (state_.results.size() > 200 && first_page_ < last_page_) {
                    state_.results.erase(state_.results.begin(), state_.results.begin() + static_cast<std::ptrdiff_t>(pages_[first_page_].size)); ++first_page_;
                }
                state_.next_cursor = std::move(reply->nextCursor);
                if (!state_.next_cursor) { state_.total = start + pages_[page].size; }
            }
            if (pages_.size() > 1024) {
                if (first_page_ == 0) {
                    state_.results.erase(state_.results.begin(), state_.results.begin() + static_cast<std::ptrdiff_t>(pages_.front().size));
                    ++first_page_;
                }
                pages_.erase(pages_.begin()); --first_page_; --last_page_;
            }
            state_.stale = reply->stale;
            state_.has_previous = first_page_ > 0;
            state_.has_next = last_page_ + 1 < pages_.size() || state_.next_cursor.has_value();
            state_.visible_first = state_.results.empty() ? 0 : pages_[first_page_].start + 1;
            state_.visible_last = pages_[last_page_].start + pages_[last_page_].size;
        }
        changed();
    });
    changed();
}
Details::Details(Rpc& rpc) : rpc_(rpc) {}
void Details::release() { ++generation_; state_ = {}; changed(); }
void Details::load(std::string id) {
    bounded(id, 512); ++generation_; state_ = {}; state_.id = std::move(id); state_.loading = true;
    call(rpc_, protocol::GetParams{state_.id}, [this, generation = generation_](Reply<protocol::GetResult> reply) {
        if (generation != generation_) { return; }
        state_.loading = false;
        if (!reply) { state_.error = error_text(reply.error().code); }
        else {
            state_.package = *reply;
            if (reply->source.value == "community") { load_build_files(); }
            state_.can_remove = reply->installedState == "installed";
            state_.can_open = state_.can_remove && reply->appMetadata && !reply->appMetadata->launchables.empty();
            state_.primary = {state_.can_open ? "action.open" : state_.can_remove ? "action.remove" : "action.install", {}};
            state_.facts = {{{"detail.available_version", {}}, reply->details.version}, {{"detail.package", {}}, reply->details.package.name}};
        }
        changed();
    });
    changed();
}
void Details::summary(protocol::PackageSummary value) {
    if (value.package.name != state_.id) { return; }
    state_.summary = std::move(value); changed();
}
void Details::load_build_files() {
    call(rpc_, protocol::BuildFilesParams{state_.id}, [this, generation = generation_](Reply<protocol::BuildFilesResult> reply) {
        if (generation != generation_) { return; }
        if (reply && !reply->files.empty()) {
            BuildDisclosure disclosure; disclosure.repository = reply->packageBase;
            for (const auto& file : reply->files) { disclosure.files.push_back({file.name, file.text}); }
            state_.build = std::move(disclosure);
            state_.build_disclosure = {"detail.build_review", {}};
            if (reply->truncated) { state_.build_warning = Text{"detail.build_truncated", {}}; }
        } else { state_.build_disclosure = {"detail.build_unavailable", {}}; }
        changed();
    });
}
void Details::load_wiki(bool refresh) {
    if (state_.id.empty()) { return; }
    call(rpc_, protocol::WikiGetParams{state_.id, refresh}, [this, generation = generation_](Reply<protocol::WikiGetResult> reply) {
        if (generation != generation_) { return; }
        if (reply) { state_.wiki = *reply; state_.wiki_error.reset(); }
        else { state_.wiki_error = Text{"wiki.error", {}}; }
        changed();
    });
}
void Details::variants() {
    if (state_.id.empty()) { return; }
    call(rpc_, protocol::VariantsParams{state_.id}, [this, generation = generation_](Reply<protocol::VariantsResult> reply) {
        if (generation != generation_) { return; }
        if (reply) { state_.variants = reply->choices; } else { state_.error = error_text(reply.error().code); }
        changed();
    });
}
void Details::community(std::optional<protocol::CommunityInfo> info) {
    if (info) { protocol::validate_wire(nlohmann::json(*info)); }
    state_.community = std::move(info); changed();
}
std::vector<std::pair<std::string, std::string>> Details::community_rows(const Copy& copy) const {
    if (!state_.community) { return {}; }
    const auto& info = *state_.community;
    return {
        {copy.text("community.maintainer"), info.maintainer.value_or(copy.text("community.no_maintainer"))},
        {copy.text("community.updated"), copy.date(info.last_modified)},
        {copy.text("community.votes"), copy.number(static_cast<double>(info.votes))},
        {copy.text("community.popularity"), copy.number(info.popularity)},
        {copy.text(info.out_of_date_since ? "community.outdated" : "community.not_flagged"), copy.text("community.context")}
    };
}
void Details::build(BuildDisclosure disclosure) {
    bounded(disclosure.repository); bounded(disclosure.commit, 128);
    if (disclosure.repository.empty() || disclosure.commit.empty() || disclosure.files.empty() || disclosure.files.size() > 32) {
        throw std::invalid_argument("invalid build disclosure");
    }
    std::size_t bytes = 0;
    for (const auto& file : disclosure.files) {
        bounded(file.path); bounded(file.text, 65536); bytes += file.text.size();
    }
    if (bytes > 1024 * 1024) { throw std::length_error("build review exceeds limit"); }
    state_.build_disclosure = {"detail.build_identity", {{"repository", disclosure.repository}, {"commit", disclosure.commit}}};
    state_.build_warning = disclosure.mutable_sources ? std::optional<Text>({"detail.build_mutable", {}}) : std::nullopt;
    state_.build = std::move(disclosure); changed();
}
void Details::updates(const protocol::UpdateReport& report) {
    if (!state_.package) { return; }
    const auto& package = state_.package->details.package;
    const auto matches = [&package](const auto& item) {
        return item.package.name == package.name && item.package.source == package.source && item.package.repo == package.repo;
    };
    state_.update_available = state_.can_remove && package.source.value == "community" && std::ranges::any_of(report.community, matches);
    state_.primary = {state_.update_available ? "updates.item_update" : state_.can_open ? "action.open" : state_.can_remove ? "action.remove" : "action.install", {}};
    changed();
}
void Details::offline(bool value) { state_.offline = value; changed(); }
void VariantChooser::choices(const protocol::VariantChoices& choices) {
    if (choices.options.size() > 128) { throw std::length_error("too many variants"); }
    VariantState next;
    if (choices.urls_differ) { next.note = Text{"variant.different_sites", {}}; }
    std::size_t recommendations = 0;
    for (const auto& option : choices.options) {
        std::string kind = option.kind.value;
        if (kind == "standard") { kind = option.package.source.value == "official" ? "official" : "source"; }
        if (kind == "bin") { kind = "binary"; }
        if (kind != "official" && kind != "source" && kind != "binary" && kind != "git" && kind != "appimage" && kind != "nightly" && kind != "beta" && kind != "stable" && kind != "wayland") {
            throw std::invalid_argument("unknown variant");
        }
        VariantRow row{option, {"variant." + kind, {}}, {"variant." + kind + "_body", {}}, {}};
        if (option.recommended) {
            ++recommendations;
            next.selected = next.options.size();
            const auto reason = option.package.source.value == "official" ? "official" : kind == "binary" ? "binary" : kind == "appimage" ? "appimage" : "source";
            row.reason = Text{std::string("variant.reason_") + reason, {}};
        }
        next.options.push_back(std::move(row));
    }
    if (recommendations > 1) { throw std::invalid_argument("ambiguous recommendation"); }
    state_ = std::move(next); changed();
}
void VariantChooser::select(std::size_t index) {
    if (index >= state_.options.size()) { throw std::out_of_range("variant index"); }
    state_.selected = index; changed();
}
Confirm::Confirm(Rpc& rpc) : rpc_(rpc) {}
void Confirm::plan(protocol::TransactionPlan value, bool dependents) {
    protocol::validate_wire(nlohmann::json(value));
    {
        protocol::PlanParams params; params.kind = value.kind.value;
        if (params.kind == "install" && !value.installs.empty()) { params.packageId = value.installs.front().name; }
        if (params.kind == "remove" && !value.removals.empty()) { params.packageId = value.removals.front().name; }
        if (params.kind == "update-community" && !value.upgrades.empty()) { params.packageId = value.upgrades.front().name; }
        if (params.kind == "update-and-install" && !value.installs.empty()) { params.packageId = value.installs.front().name; }
        if (params.kind == "manual-upgrade" && !value.upgrades.empty()) { params.receiptId = value.upgrades.front().name; }
        request_ = params;
    }
    state_.plan = std::move(value); state_.build_acknowledged = false; state_.dependents_acknowledged = false;
    state_.requires_dependents_ack = dependents; state_.stale = false; state_.error.reset(); state_.submitting = false; state_.job_id.reset();
    std::vector<protocol::BuildFilesParams> reviews;
    if (state_.plan->requires_build_review) {
        for (const auto& pin : state_.plan->build_pins) { reviews.push_back({pin.pkgbase, pin.commit}); }
        if (reviews.empty()) {
            for (const auto& ref : state_.plan->build_review_refs) { reviews.push_back({ref.name, {}}); }
        }
    }
    state_.build_files.clear(); state_.pending_build_files = reviews.size();
    const auto generation = ++generation_;
    for (const auto& request : reviews) {
        call(rpc_, request, [this, generation, request](Reply<protocol::BuildFilesResult> reply) {
            if (generation != generation_) { return; }
            if (reply && !reply->files.empty() && !reply->truncated &&
                (!request.commit || reply->packageBase == request.packageId)) { fetch_omitted(request, std::move(*reply), generation); }
            else { --state_.pending_build_files; state_.error = Text{"detail.build_unavailable", {}}; changed(); }
        });
    }
    changed();
}
void Confirm::fetch_omitted(protocol::BuildFilesParams request, protocol::BuildFilesResult files, std::uint64_t generation) {
    if (generation != generation_) { return; }
    const auto omitted = std::ranges::find_if(files.files, [](const auto& file) { return file.kind == "omitted"; });
    if (omitted == files.files.end()) {
        --state_.pending_build_files;
        state_.build_files.push_back(std::move(files));
        changed();
        return;
    }
    if (!request.commit) {
        --state_.pending_build_files;
        state_.error = Text{"detail.build_unavailable", {}};
        changed();
        return;
    }
    const auto index = static_cast<std::size_t>(omitted - files.files.begin());
    request.path = omitted->name;
    call(rpc_, request, [this, generation, request, index, files = std::move(files)](Reply<protocol::BuildFilesResult> reply) mutable {
        if (generation != generation_) { return; }
        const auto& expected = files.files[index];
        if (!reply || reply->truncated || reply->packageBase != files.packageBase || reply->files.size() != 1 ||
            reply->files.front().kind != "text" || reply->files.front().name != expected.name ||
            reply->files.front().size != expected.size || reply->files.front().sha256 != expected.sha256) {
            --state_.pending_build_files;
            state_.error = Text{"detail.build_unavailable", {}};
            changed();
            return;
        }
        files.files[index] = std::move(reply->files.front());
        fetch_omitted(request, std::move(files), generation);
    });
}
void Confirm::acknowledge_build(bool value) {
    state_.build_acknowledged = value && state_.pending_build_files == 0 && state_.plan &&
        !state_.build_files.empty() && state_.build_files.size() ==
        (state_.plan->build_pins.empty() ? state_.plan->build_review_refs.size() : state_.plan->build_pins.size());
    changed();
}
void Confirm::acknowledge_dependents(bool value) { state_.dependents_acknowledged = value; changed(); }
void Confirm::invalidate() { state_.stale = true; state_.build_acknowledged = false; state_.dependents_acknowledged = false; state_.error = Text{"confirm.plan_changed", {}}; changed(); }
void Confirm::busy(bool value) { state_.busy = value; changed(); }
bool Confirm::can_confirm() const {
    return state_.plan && !state_.plan->approval_token.empty() && !state_.submitting && !state_.stale && state_.plan->blockers.empty() && state_.plan->protected_blockers.empty() && state_.plan->conflicts.empty() &&
        (!state_.plan->requires_build_review || (state_.build_acknowledged && state_.pending_build_files == 0 && !state_.build_files.empty() && state_.build_files.size() == (state_.plan->build_pins.empty() ? state_.plan->build_review_refs.size() : state_.plan->build_pins.size()))) && (!state_.requires_dependents_ack || state_.dependents_acknowledged);
}
Text Confirm::action() const {
    if (state_.busy) { return {"confirm.add_queue", {}}; }
    if (state_.plan) {
        if (state_.plan->kind.value == "remove" || state_.plan->kind.value == "manual-remove") { return {"action.remove", {}}; }
        if (state_.plan->kind.value == "upgrade-system" || state_.plan->kind.value == "update-system") { return {"updates.all", {}}; }
        if (state_.plan->kind.value == "upgrade-community" || state_.plan->kind.value == "update-community" || state_.plan->kind.value == "manual-upgrade") { return {"updates.item_update", {}}; }
    }
    return {"action.install", {}};
}
void Confirm::request(protocol::PlanParams params) {
    protocol::validate_wire(nlohmann::json(params));
    request_ = params;
    state_.plan.reset(); state_.job_id.reset(); state_.submitting = true;
    state_.build_acknowledged = false; state_.dependents_acknowledged = false;
    call(rpc_, params, [this, generation = ++generation_](Reply<protocol::TransactionPlan> reply) {
        if (generation != generation_) { return; }
        state_.submitting = false;
        if (reply) { const auto notice = state_.error; const auto saved_request = request_; plan(*reply); request_ = saved_request; if (notice && notice->key == "confirm.plan_changed") { state_.error = notice; changed(); } } else { state_.error = reply.error().code == protocol::kUntrustedInput && reply.error().message_key == "detail.build_too_large" ? Text{"detail.build_too_large", {}} : error_text(reply.error().code); changed(); }
    });
    changed();
}
void Confirm::completed(Reply<protocol::JobIdResult> reply) {
    state_.submitting = false;
    if (reply) { state_.job_id = reply->jobId; state_.stale = true; }
    else {
        state_.error = error_text(reply.error().code);
        if (reply.error().code == protocol::kStalePlan) {
            invalidate();
            if (request_) { request(*request_); }
            state_.error = Text{"confirm.plan_changed", {}};
        }
    }
    changed();
}
void Confirm::submit() {
    if (!can_confirm()) { return; }
    const auto& kind = state_.plan->kind.value;
    const protocol::InstallParams params{state_.plan->plan_id, state_.plan->approval_token};
    state_.submitting = true;
    const auto done = [this](auto reply) { completed(std::move(reply)); };
    if (kind == "install" || kind == "update-and-install") { call(rpc_, params, done); }
    else if (kind == "remove") { call(rpc_, protocol::RemoveParams{params}, done); }
    else if (kind == "update-system" || kind == "upgrade-system") { call(rpc_, protocol::UpgradeSystemParams{params}, done); }
    else if (kind == "update-community" || kind == "upgrade-community") { call(rpc_, protocol::UpgradeCommunityParams{params}, done); }
    else if (kind == "manual-install") { call(rpc_, protocol::ManualInstallParams{params}, done); }
    else if (kind == "manual-upgrade") { call(rpc_, protocol::ManualUpgradeParams{params}, done); }
    else if (kind == "manual-remove") { call(rpc_, protocol::ManualRemoveParams{params}, done); }
    else if (kind == "setup-helper") { call(rpc_, protocol::SetupHelperParams{params}, done); }
    else { state_.submitting = false; state_.error = error_text(protocol::kUnsupportedCapability); }
    changed();
}
Updates::Updates(Rpc& rpc) : rpc_(rpc) {
    const std::array<const char*, 5> keys{"updates.restart", "updates.logout", "updates.apps", "updates.none", "updates.unknown"};
    for (std::size_t index = 0; index < keys.size(); ++index) { state_.groups[index].title = {keys[index], {}}; state_.groups[index].expanded = index == 4; }
}
void Updates::report(protocol::UpdateReport value) {
    protocol::validate_wire(nlohmann::json(value));
    ++generation_; state_.options.reset(); state_.plan.reset(); state_.error.reset(); state_.missing_news_acknowledged = false; state_.loading = false;
    state_.report = std::move(value);
    for (auto& group : state_.groups) { group.items.clear(); }
    const std::array lists{&state_.report->official, &state_.report->community, &state_.report->manual};
    for (std::size_t source = 0; source < lists.size(); ++source) {
        for (std::size_t index = 0; index < lists[source]->size(); ++index) {
            state_.groups[impact((*lists[source])[index].restart.level.value)].items.push_back({source, index});
        }
    }
    call(rpc_, protocol::UpdatesOptionsParams{state_.report->report_id}, [this, generation = generation_](Reply<protocol::UpdatesOptionsResult> reply) {
        if (generation != generation_) { return; }
        if (reply) { state_.options = *reply; } else { state_.error = error_text(reply.error().code); }
        changed();
    });
    changed();
}
void Updates::check() {
    if (state_.loading) { return; }
    state_.loading = true;
    call(rpc_, protocol::UpdatesCheckParams{true}, [this](Reply<protocol::JobIdResult> reply) {
        state_.loading = false;
        if (reply) { state_.check_job = reply->jobId; } else { state_.error = error_text(reply.error().code); }
        changed();
    }); changed();
}
void Updates::acknowledge_news(std::size_t index, bool acknowledged) {
    if (!state_.report || index >= state_.report->news.size()) { throw std::out_of_range("news index"); }
    state_.report->news[index].acknowledged = acknowledged; changed();
}
void Updates::news_available(bool available) { state_.news_available = available; state_.missing_news_acknowledged = false; changed(); }
void Updates::acknowledge_missing_news(bool acknowledged) { state_.missing_news_acknowledged = acknowledged; changed(); }
bool Updates::can_update() const {
    return state_.report && !state_.loading && (state_.news_available || state_.missing_news_acknowledged) &&
        std::ranges::none_of(state_.report->news, [](const auto& item) { return item.requires_manual_intervention && !item.acknowledged; });
}
std::optional<Text> Updates::item_blocker() const {
    if (state_.report && !state_.report->official.empty()) { return Text{"updates.system_first", {}}; }
    return {};
}
void Updates::plan_system(std::optional<std::string> also_install) {
    if (!state_.options || state_.options->officialAction.empty()) { return; }
    request_plan({{"system"}, {}}, std::move(also_install));
}
void Updates::plan_item(std::string kind, std::string id) {
    if (!state_.options) { return; }
    if (auto blocker = item_blocker()) { state_.error = blocker; changed(); return; }
    const auto* ids = kind == "community" ? &state_.options->communityIndividualIds : kind == "manual" ? &state_.options->manualIndividualIds : nullptr;
    if (!ids || std::ranges::find(*ids, id) == ids->end()) { return; }
    if (kind == "manual") { state_.error = Text{"git.review", {}}; changed(); return; }
    request_plan({{std::move(kind)}, std::move(id)}, {});
}
void Updates::request_plan(protocol::PlanSelection selection, std::optional<std::string> also_install) {
    if (!can_update()) { state_.error = Text{"updates.news_block", {}}; changed(); return; }
    state_.loading = true; state_.plan.reset();
    protocol::PlanParams params;
    params.kind = selection.kind.value == "system" ? (also_install ? "update-and-install" : "update-system") : selection.kind.value == "manual" ? "manual-upgrade" : "update-community";
    if (selection.kind.value == "manual") { params.receiptId = selection.id; }
    else { params.packageId = also_install ? also_install : selection.id; }
    call(rpc_, params, [this, generation = generation_](Reply<protocol::TransactionPlan> reply) {
        if (generation != generation_) { return; }
        state_.loading = false;
        if (reply) { state_.plan = protocol::UpdatesPlanResult{reply->plan_id, {*reply}, reply->restart, true, reply->blockers}; }
        else { state_.error = error_text(reply.error().code); }
        changed();
    }); changed();
}
Subscription bind_page_retention(Shell& shell, Details& details, Installed& installed) {
    return shell.observe([&details, &installed, previous = shell.state().location](const auto& state) mutable {
        const auto destination = state.location.destination;
        if (previous.destination == destination && previous.id == state.location.id) { return; }
        const auto leaving = previous.destination;
        previous = state.location;
        if (leaving == Destination::Details) { details.release(); }
        if (leaving == Destination::Installed) { installed.release(); }
        if (destination == Destination::Details && details.state().id != state.location.id) {
            details.load(state.location.id); details.load_wiki(false);
        }
        if (destination == Destination::Installed) { installed.refresh(); }
    });
}
Installed::Installed(Rpc& rpc) : rpc_(rpc) {}
void Installed::release() {
    ++generation_;
    const auto view = state_.view; auto query = std::move(state_.query);
    state_ = {}; state_.view = view; state_.query = std::move(query); changed();
}
void Installed::view(InstalledView value) { release(); state_.view = value; refresh(); }
void Installed::query(std::string value) { bounded(value); state_.query = std::move(value); refresh(); }
void Installed::refresh() {
    const auto generation = ++generation_; state_.loading = true; state_.error.reset();
    if (state_.view == InstalledView::Git) {
        call(rpc_, protocol::ManualListParams{}, [this, generation](Reply<protocol::ManualListResult> reply) {
            if (generation != generation_) { return; }
            state_.loading = false;
            if (reply) { state_.receipts = reply->receipts; } else { state_.error = error_text(reply.error().code); }
            changed();
        });
    } else if (state_.view == InstalledView::Outside) {
        call(rpc_, protocol::UnmanagedListParams{}, [this, generation](Reply<protocol::UnmanagedListResult> reply) {
            if (generation != generation_) { return; }
            state_.loading = false;
            if (reply) { state_.outside = reply->entries; } else { state_.error = error_text(reply.error().code); }
            changed();
        });
    } else if (state_.view == InstalledView::Components) {
        state_.loading = false; state_.error = Text{"error.unsupported_body", {}};
    } else {
        protocol::CatalogFilters filter; filter.installed = true; filter.appsOnly = true;
        call(rpc_, protocol::SearchParams{state_.query, filter, {}, 100}, [this, generation](Reply<protocol::SearchResult> reply) {
            if (generation != generation_) { return; }
            state_.loading = false;
            if (reply) { state_.packages = reply->packages; } else { state_.error = error_text(reply.error().code); }
            changed();
        });
    }
    changed();
}
Settings::Settings(Rpc& rpc) : rpc_(rpc) {}
void Settings::refresh() {
    call(rpc_, protocol::SettingsGetParams{}, [this](Reply<protocol::SettingsGetResult> reply) {
        if (reply) { state_.legacy = *reply; } else { state_.error = error_text(reply.error().code); }
        changed();
    });
}
void Settings::request_clear_cache() {
    if (state_.active_transaction || state_.clearing_cache) { return; }
    state_.cache_confirmation = true; changed();
}
void Settings::clear_cache(bool confirmed) {
    if (!state_.cache_confirmation) { return; }
    state_.cache_confirmation = false;
    if (!confirmed || state_.active_transaction || state_.clearing_cache) { changed(); return; }
    state_.clearing_cache = true;
    call(rpc_, protocol::ClearCacheParams{}, [this](Reply<protocol::ClearCacheResult> reply) {
        state_.clearing_cache = false;
        if (reply) { state_.cache_bytes = 0; state_.error.reset(); }
        else { state_.error = error_text(reply.error().code); }
        changed();
    });
    changed();
}
void Settings::capabilities(std::set<std::string> helpers, std::string effective, bool busy) {
    for (const auto& helper : helpers) { if (helper != "paru" && helper != "yay") { throw std::invalid_argument("invalid helper"); } }
    state_.available_helpers = std::move(helpers); state_.effective_helper = std::move(effective); state_.active_transaction = busy; changed();
}
std::optional<Text> Settings::helper_disabled(std::string_view helper) const {
    if (state_.active_transaction) { return Text{"settings.helper_busy", {}}; }
    if (helper != "auto" && !state_.available_helpers.contains(std::string(helper))) { return Text{"settings.helper_missing", {}}; }
    return {};
}
void Settings::apply(protocol::SettingsPatch patch) {
    protocol::validate_wire(nlohmann::json(patch));
    if (patch.helper) { if (auto reason = helper_disabled(patch.helper->value)) { state_.error = reason; changed(); return; } }
    if (pending_.size() >= 64) { state_.error = Text{"settings.save_failed", {}}; changed(); return; }
    pending_.push_back(std::move(patch)); if (!state_.saving) { send_next(); }
}
void Settings::send_next() {
    if (pending_.empty()) { state_.saving = false; changed(); return; }
    state_.saving = true;
    auto patch = std::move(pending_.front()); pending_.pop_front();
    call(rpc_, protocol::SettingsSetParams{std::move(patch)}, [this](Reply<protocol::SettingsSetResult> reply) {
        if (reply) { state_.values = reply->settings; state_.error.reset(); }
        else { state_.error = Text{"settings.save_failed", {}}; }
        send_next();
    }); changed();
}
EmptyState empty_search(std::string query) { bounded(query); return {{"search.empty_title", {{"query", std::move(query)}}}, {"search.empty_body", {}}, Text{"filter.clear", {}}}; }
EmptyState empty_installed(InstalledView view) {
    const auto key = view == InstalledView::Apps ? "apps" : view == InstalledView::Components ? "components" : view == InstalledView::Git ? "git" : "outside";
    return {{std::string("installed.empty_") + key, {}}, {std::string("installed.empty_") + key + "_body", {}}, {}};
}
FirstRun::FirstRun(Rpc& rpc) : rpc_(rpc) {}
void FirstRun::refresh() {
    call(rpc_, protocol::SetupStatusParams{}, [this](Reply<protocol::SetupStatusResult> reply) {
        if (reply) {
            state_.status = *reply;
            capabilities(reply->paru.installed || reply->yay.installed, reply->elevation.value != "unavailable");
        } else { state_.error = error_text(reply.error().code); changed(); }
    });
}
void FirstRun::review(Confirm& confirm) {
    if (!state_.setup_supported) { return; }
    protocol::PlanParams params; params.kind = "setup-helper"; params.helper = protocol::HelperPreference{state_.preferred_helper};
    confirm.request(params);
}
void FirstRun::capabilities(bool helper_available, bool setup_supported) { state_.helper_available = helper_available; state_.setup_supported = setup_supported; changed(); }
void FirstRun::helper(std::string helper) {
    if (helper != "paru" && helper != "yay") { throw std::invalid_argument("invalid helper"); }
    state_.preferred_helper = std::move(helper); changed();
}

}
