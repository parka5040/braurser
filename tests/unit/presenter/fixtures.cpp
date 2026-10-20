#include "fixtures.hpp"
#include <algorithm>
#include <cctype>

namespace braurser::presenter::fixtures {
namespace {
constexpr auto timestamp = "2026-09-25T12:00:00Z";

Data make_data() {
    Data result;
    const protocol::PackageRef krita{{"official"}, "extra", "krita"};
    const protocol::PackageRef binary{{"community"}, {}, "drawing-bin"};
    const protocol::PackageRef development{{"community"}, {}, "drawing-git"};
    result.packages = {{krita, "5.2.9", "Create digital paintings and illustrations.", false},
        {binary, "2.0", "A community-supported drawing application.", true},
        {development, "2.1.r42", "The latest development version of Drawing.", false}};
    result.packages[0].appName = "Krita";
    result.packages[1].installedVersion = "1.9";
    result.packages[1].installedSize = 42000000;
    result.packages[1].downloadSize = 12000000;
    const protocol::AppMetadata metadata{"org.kde.krita", "Krita", result.packages.front().description,
        {"Krita is a painting application for artists."}, {}, {}, {"Graphics"}, {"org.kde.krita.desktop"}};
    result.official = {{krita, "5.2.9", metadata.summary, {"qt6-base"}, "https://krita.org"}, "not-installed", {"official"}, metadata};
    result.community = {{binary, "2.0", result.packages[1].description, {"qt6-base"}, "https://example.org/drawing"}, "installed", {"community"}, {}};
    result.community_info = {"drawing-maintainer", "drawing-contributor", 42, 1.25, "2026-09-20T12:00:00Z",
        "2024-01-01T12:00:00Z", "2026-09-18T12:00:00Z", "drawing-bin", "https://example.org/drawing"};
    result.variants = {{{{binary, {"bin"}, false, true}, {development, {"git"}, true, false}}, false}, true};
    result.receipt = {"fixture-git", "Canvas", {"user"}, std::string(40, 'a'), timestamp, {"canvas.desktop"}, false};
    result.preview = {"fixture-preview", "example.org", "team/canvas", result.receipt.commit, "install.sh",
        "printf '%s\\n' 'Installing Canvas'\n", {{"user"}, {"system"}}, {"https://example.org/canvas/source"}, false};
    result.outside = {"local-notes.desktop", "Local notes", {}, {}, {"outside"}};
    protocol::WikiDocument document{"Krita", "https://wiki.archlinux.org/title/Krita", timestamp, false, "wiki.attribution", {}, {}};
    document.blocks = {{protocol::WikiHeading{2, "Krita"}},
        {protocol::WikiParagraph{{{protocol::WikiText{"Krita is a digital painting application. ", false, false, false}},
            {protocol::WikiLink{"ArchWiki", "https://wiki.archlinux.org/title/Krita"}}}}},
        {protocol::WikiHeading{3, "Installation"}},
        {protocol::WikiNote{{{protocol::WikiText{"Distribution instructions may differ on Artix.", false, false, false}}}}},
        {protocol::WikiWarning{{{protocol::WikiText{"Save your work before updating.", false, true, false}}}}},
        {protocol::WikiCode{"pacman -S krita"}}};
    document.sections = {{2, "Krita"}, {3, "Installation"}};
    result.wiki = {document, document.source_url, document.attribution_key, document.fetched_at, false};
    auto& report = result.updates;
    report.report_id = "fixture-report";
    report.checked_at = timestamp;
    report.overall_restart = {"reboot"};
    const auto item = [](std::string name, std::string source, std::string level) {
        protocol::PackageRef package{{source}, source == "official" ? std::optional<std::string>("extra") : std::nullopt, std::move(name)};
        return protocol::UpdateItem{package, "1.0", "2.0", {source}, {package, {std::move(level)}, "updates.reason_unknown", {}}};
    };
    report.official = {item("linux", "official", "reboot"), item("plasma-workspace", "official", "logout"),
        item("krita", "official", "restart-app"), item("libexample", "official", "none")};
    report.community = {item(binary.name, "community", "unknown")};
    report.manual = {item(result.receipt.receipt_id, "git", "unknown")};
    report.news = {{"Review distribution update instructions", "https://archlinux.org/news/", timestamp, true, false}};
    result.question = {"fixture-provider", "fixture-token", "2099-09-25T12:00:00Z", {protocol::ProviderQuestion{{binary, development}}}};
    result.running = {{"running"}, {}, protocol::Progress{{"downloading"}, 48, 100, {"items"}}, {}, {},
        {{1, "job.question", nlohmann::json(protocol::JobQuestionNotification{"fixture-active", 1, result.question})}}, {}};
    return result;
}

std::string lowercase(std::string value) {
    for (auto& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return value;
}
}

const Data& data() {
    static const auto value = make_data();
    return value;
}

FakeServer::FakeServer(Dispatcher& dispatcher, Mode mode) : dispatcher_(dispatcher), mode_(mode) {}

FakeServer::~FakeServer() {
    alive_.reset();
    for (const auto& [id, listener] : listeners_) { static_cast<void>(id); listener->active = false; }
}

protocol::UpdateReport FakeServer::update_report() { return data().updates; }

Reply<nlohmann::json> FakeServer::response(std::string_view method, const nlohmann::json& params) {
    if (mode_ == Mode::Error) { return std::unexpected(RpcError{protocol::kOffline, "offline.operation_block", nlohmann::json::object()}); }
    if (const auto found = overrides_.find(std::string(method)); found != overrides_.end()) { return found->second; }
    const auto& fixture = data();
    const bool empty = mode_ == Mode::Empty;
    const bool offline = mode_ == Mode::Offline;
    if (method == "system.hello") {
        return nlohmann::json(protocol::HelloResult{"fixture", protocol::kApiVersion, {"paru"},
            {{"paru"}, "2.1", {"sudo-askpass"}, {"unknown"}, "arch", {"extra"}}, {{"gtk"}, {"qt"}}});
    }
    if (method == "system.status") { return nlohmann::json(protocol::StatusResult{{offline ? "offline" : "online"}, 60, {}, {"sudo-askpass"}, "ready", 60, {{"community", "ok", {}, 2, 0}}}); }
    if (method == "catalog.facets") { return nlohmann::json(protocol::FacetsResult{{{"extra", 1}}, {{"graphics", 3}}, {{"Graphics", 3}}}); }
    if (method == "catalog.buildFiles") { return nlohmann::json(protocol::BuildFilesResult{"drawing-bin", {{"PKGBUILD", "pkgname=drawing-bin\npkgver=2.0\n", "text", 31, std::string(64, 'a')}}, false}); }
    if (method == "restart.state") { return nlohmann::json(restart_); }
    if (method == "restart.acknowledge") { restart_ = {}; return nlohmann::json(protocol::EmptyResult{}); }
    if (method == "setup.status") { return nlohmann::json(protocol::SetupStatusResult{{true, "2.1"}, {false, {}}, {"paru"}, {"sudo-askpass"}, {}}); }
    if (method == "settings.clearCache") { return nlohmann::json(protocol::ClearCacheResult{12000000}); }
    if (method == "jobs.dequeue") { dequeued_ = true; return nlohmann::json(protocol::JobsDequeueResult{true}); }
    if (method == "jobs.plan") {
        const auto request = params.get<protocol::PlanParams>();
        protocol::TransactionPlan plan;
        plan.plan_id = "fixture-plan-" + std::to_string(++revision_); plan.revision = revision_;
        plan.approval_token = "fixture-approval-" + std::to_string(revision_); plan.kind = {request.kind};
        const auto package = request.packageId.value_or(request.receiptId.value_or(request.helper ? request.helper->value : "canvas"));
        const protocol::PackageRef ref{{request.kind.starts_with("manual") ? "git" : package == "krita" ? "official" : "community"}, package == "krita" ? std::optional<std::string>("extra") : std::nullopt, package};
        if (request.kind == "remove" || request.kind == "manual-remove") { plan.removals = {ref}; }
        else if (request.kind == "update-system") { for (const auto& item : fixture.updates.official) { plan.upgrades.push_back(item.package); } }
        else if (request.kind == "update-community" || request.kind == "manual-upgrade") { plan.upgrades = {ref}; }
        else { plan.installs = {ref}; }
        plan.download_bytes = 12000000; plan.install_delta_bytes = 42000000;
        plans_[plan.plan_id] = plan;
        return nlohmann::json(plan);
    }
    if (method == "jobs.install" || method == "jobs.remove" || method == "jobs.upgradeSystem" || method == "jobs.upgradeCommunity" ||
        method == "manual.install" || method == "manual.upgrade" || method == "manual.remove" || method == "setup.helper") {
        const auto request = params.get<protocol::InstallParams>();
        const auto found = plans_.find(request.planId);
        if (found == plans_.end() || found->second.approval_token != request.approvalToken) { return std::unexpected(RpcError{protocol::kStalePlan, "confirm.plan_changed", nlohmann::json::object()}); }
        plans_.erase(found);
        return nlohmann::json(protocol::JobIdResult{method == "manual.install" ? "fixture-install" : "fixture-active"});
    }
    if (method == "catalog.search") {
        const auto query = params.get<protocol::SearchParams>();
        auto packages = empty ? std::vector<protocol::PackageSummary>{} : fixture.packages;
        const auto text = lowercase(query.text);
        std::erase_if(packages, [&](auto& package) {

            return (query.filters.installed && !package.installed) || (query.category && *query.category != "Graphics") ||
                (query.filters.sourceRepo && package.package.repo != query.filters.sourceRepo) || (!text.empty() && lowercase(package.package.name + " " + package.description).find(text) == std::string::npos) ||
                (query.filters.repoOnly && package.package.source.value != "official") ||
                (query.filters.aurOnly && package.package.source.value != "community");
        });
        if (query.sort.value == "name") { std::ranges::sort(packages, {}, [](const auto& package) { return package.package.name; }); }
        return nlohmann::json(protocol::SearchResult{packages, {}, offline});
    }
    if (method == "catalog.get") {
        const auto id = params.get<protocol::GetParams>().packageId;
        if (id == fixture.official.details.package.name) { return nlohmann::json(fixture.official); }
        if (id == "drawing-bin" || id == "drawing-git") {
            auto package = fixture.community;
            package.details.package.name = id;
            return nlohmann::json(package);
        }
    }
    if (method == "catalog.variants") { return nlohmann::json(fixture.variants); }
    if (method == "wiki.get") {
        auto wiki = fixture.wiki;
        wiki.stale = offline;
        wiki.document.stale = offline;
        return nlohmann::json(wiki);
    }
    if (method == "updates.options") { return nlohmann::json(protocol::UpdatesOptionsResult{"update-system", {"drawing-bin"}, {"fixture-git"}}); }
    if (method == "updates.check") { return nlohmann::json(protocol::JobIdResult{"fixture-check"}); }
    if (method == "jobs.list") {
        auto jobs = protocol::JobsListResult{{{"fixture-active", {"running"}}, {"fixture-queued", {"queued"}}, {"fixture-recent", {"succeeded"}}}, {}};
        if (dequeued_) { std::erase_if(jobs.jobs, [](const auto& job) { return job.jobId == "fixture-queued"; }); }
        if (empty) { jobs.jobs.clear(); }
        return nlohmann::json(jobs);
    }
    if (method == "jobs.get") {
        const auto id = params.get<protocol::JobsGetParams>().jobId;
        if (id == "fixture-active") { return nlohmann::json(fixture.running); }
        nlohmann::json result;
        if (id == "fixture-check") {
            auto report = update_report();
            if (empty) { report.official.clear(); report.community.clear(); report.manual.clear(); report.news.clear(); report.overall_restart = {"none"}; }
            result = report;
        } else if (id == "fixture-preview") { result = fixture.preview; }
        else if (id == "fixture-install") { result = fixture.receipt; }
        else { return std::unexpected(RpcError{protocol::kUnsupportedCapability, "error.unsupported_title", nlohmann::json::object()}); }
        return nlohmann::json(protocol::JobsGetResult{{"succeeded"}, {}, {}, result, {}, {}, {}});
    }
    if (method == "manual.preview") { return nlohmann::json(protocol::JobIdResult{"fixture-preview"}); }
    if (method == "manual.list") { return nlohmann::json(protocol::ManualListResult{empty ? std::vector<protocol::ManualReceipt>{} : std::vector{fixture.receipt}}); }
    if (method == "unmanaged.list") { return nlohmann::json(protocol::UnmanagedListResult{empty ? std::vector<protocol::LauncherEntry>{} : std::vector{fixture.outside}, timestamp}); }
    if (method == "settings.get") { return nlohmann::json(protocol::SettingsGetResult{settings_.ui, settings_.notifications, settings_.check_frequency, {"sudo-askpass"}}); }
    if (method == "settings.set") {
        const auto patch = params.get<protocol::SettingsSetParams>().changedFields;
        if (patch.helper) { settings_.helper = *patch.helper; }
        if (patch.check_frequency) { settings_.check_frequency = *patch.check_frequency; }
        if (patch.notifications) { settings_.notifications = *patch.notifications; }
        if (patch.autostart) { settings_.autostart = *patch.autostart; }
        if (patch.review_builds) { settings_.review_builds = *patch.review_builds; }
        if (patch.show_log) { settings_.show_log = *patch.show_log; }
        if (patch.theme) { settings_.theme = *patch.theme; }
        return nlohmann::json(protocol::SettingsSetResult{settings_});
    }
    return std::unexpected(RpcError{protocol::kUnsupportedCapability, "error.unsupported_title", nlohmann::json::object()});
}

void FakeServer::request(std::string_view method, nlohmann::json params, Callback callback, std::chrono::milliseconds) {
    if (requests.size() >= 1024) { requests.erase(requests.begin()); }
    requests.emplace_back(std::string(method), params);
    if (mode_ == Mode::Loading) { return; }
    auto result = response(method, params);
    const bool question = result && method == "jobs.get" && params.value("jobId", "") == "fixture-active";
    dispatcher_.post([callback = std::move(callback), result = std::move(result)]() mutable { callback(std::move(result)); });
    if (question) { notify(protocol::JobQuestionNotification{"fixture-active", 1, data().question}); }
}

Subscription FakeServer::subscribe(std::string_view method, std::function<void(const nlohmann::json&)> callback) {
    const auto id = next_++;
    auto listener = std::make_shared<Listener>(std::string(method), std::move(callback));
    listeners_.emplace(id, listener);
    return Subscription([this, id, listener, weak = std::weak_ptr<int>(alive_)] {
        listener->active = false;
        if (!weak.expired()) { listeners_.erase(id); }
    });
}

void FakeServer::emit_event(std::string_view method, nlohmann::json value) {
    const auto listeners = listeners_;
    for (const auto& [id, listener] : listeners) {
        static_cast<void>(id);
        if (listener->method == method) {
            dispatcher_.post([listener, value] { if (listener->active) { listener->callback(value); } });
        }
    }
}

void FakeServer::reply(std::string method, nlohmann::json value) { overrides_[std::move(method)] = std::move(value); }

}
