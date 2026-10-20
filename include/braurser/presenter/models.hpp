#ifndef BRAURSER_PRESENTER_MODELS_HPP
#define BRAURSER_PRESENTER_MODELS_HPP

#include <braurser/presenter/copy.hpp>
#include <braurser/presenter/rpc_client.hpp>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <span>

namespace braurser::presenter {

struct Text {
    std::string key;
    CopyArgs args;
    std::string render(const Copy& copy) const;
};
Text error_text(int code);

template<class State>
class Model {
public:
    Model() : lifetime_(std::make_shared<int>(0)), observers_(std::make_shared<Observers>()) {}
    virtual ~Model() = default;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;
    const State& state() const { return state_; }
    /// out: observes one immutable snapshot shared across callbacks; references last only for the call.
    Subscription observe(std::function<void(const State&)> callback) {
        if (!callback || observers_->callbacks.size() >= 128) { throw std::invalid_argument("invalid observer"); }
        const auto id = observers_->next++;
        observers_->callbacks.emplace(id, std::make_shared<std::function<void(const State&)>>(std::move(callback)));
        return Subscription([weak = std::weak_ptr<Observers>(observers_), id] {
            if (auto observers = weak.lock()) { observers->callbacks.erase(id); }
        });
    }
protected:
    void changed() {
        const auto observers = observers_;
        if (observers->callbacks.empty()) { return; }
        const auto snapshot = std::make_shared<const State>(state_);
        const auto callbacks = observers->callbacks;
        for (const auto& [id, callback] : callbacks) {
            if (observers->callbacks.contains(id)) { (*callback)(*snapshot); }
        }
    }
    template<class Params, class Callback>
    void call(Rpc& rpc, const Params& params, Callback callback) {
        rpc.call<Method<Params>>(params, [weak = std::weak_ptr<int>(lifetime_), callback = std::move(callback)](auto reply) mutable {
            if (!weak.expired()) { callback(std::move(reply)); }
        });
    }
    State state_;
    std::shared_ptr<int> lifetime_;
private:
    struct Observers {
        std::map<std::uint64_t, std::shared_ptr<std::function<void(const State&)>>> callbacks;
        std::uint64_t next = 1;
    };
    std::shared_ptr<Observers> observers_;
};

enum class Destination { Discover, Search, Updates, Installed, Settings, Activity, Details, Git };
enum class Viewport { Narrow, Compact, Wide, Large };
struct Location { Destination destination = Destination::Discover; std::string id; double scroll = 0; };
struct ShellState {
    Location location;
    std::vector<Location> history;
    Viewport viewport = Viewport::Wide;
    std::string theme = "system";
    std::string resolved_theme = "light";
    std::string connectivity = "unknown";
    bool connected = false;
    std::optional<std::string> active_job;
    std::size_t queue_count = 0;
    protocol::RestartStateResult restart;
    std::string catalog_state;
    std::optional<std::uint64_t> catalog_age;
    bool community_available = false;
};
class Shell final : public Model<ShellState> {
public:
    explicit Shell(Rpc& rpc);
    void navigate(Location location);
    void back();
    void width(unsigned width);
    void theme(std::string preference, std::optional<bool> desktop_dark);
    void refresh();
    void connected(bool value);
    void acknowledge_restart();
private:
    Rpc& rpc_;
};

std::string_view source_key(const protocol::PackageRef& package);
unsigned app_grid_columns(unsigned content_width);

struct Category { std::string id; Text label; std::string icon; std::vector<Category> children; };
struct DiscoverState {
    std::vector<Category> categories;
    std::vector<std::string> app_ids;
    std::vector<protocol::PackageSummary> apps;
    std::uint64_t collection_revision = 0;
    bool metadata_available = false;
    bool loading = false;
    bool cached = false;
    bool offline = false;
    std::optional<Text> notice;
};
class Discover final : public Model<DiscoverState> {
public:
    Discover();
    void collection(std::uint64_t revision, std::span<const std::string> shipped, std::span<const std::string> available,
                    std::span<const protocol::PackageSummary> metadata = {});
    void availability(bool metadata, bool cached, bool offline);
};

enum class Installation { Any, Installed, NotInstalled };
enum class AddedAs { Any, Explicit, Dependency };
enum class SourceFilter { All, Official, Community };
struct FilterSelection {
    Installation installation = Installation::Any;
    AddedAs added_as = AddedAs::Any;
    SourceFilter source = SourceFilter::All;
    bool orphans = false;
    bool updates = false;
    bool apps_only = true;
    std::vector<std::string> repositories;
    std::vector<std::string> groups;
    std::optional<std::string> category;
};
struct ActiveFilter { Text label; FilterSelection removed; };
std::vector<ActiveFilter> active_filters(const FilterSelection& selection);

struct FilterMapping {
    protocol::SearchFilters filters;
    bool not_installed_enabled = true;
    bool official_enabled = true;
    bool community_enabled = true;
    std::optional<Text> disabled_reason;
    std::optional<Text> foreign_note;
};
FilterMapping map_filters(const FilterSelection& selection);
Reply<protocol::SearchParams> search_params(const protocol::SearchQuery& query);
struct SearchState {
    protocol::SearchQuery query{"", {}, {"relevance"}, {}, 50};
    FilterSelection selection;
    protocol::FacetsResult facets;
    FilterMapping mapping;
    std::uint64_t generation = 0;
    std::vector<protocol::PackageSummary> results;
    std::optional<std::string> next_cursor;
    std::optional<std::uint64_t> total;
    std::optional<std::string> updates_checked_at;
    std::optional<Text> error;
    std::map<std::string, Text> source_errors;
    bool loading = false;
    bool stale = false;
    bool has_previous = false;
    bool has_next = false;
    std::uint64_t visible_first = 0;
    std::uint64_t visible_last = 0;
};
class Search final : public Model<SearchState> {
public:
    Search(Rpc& rpc, Timer& timer);
    ~Search() override;
    void query(std::string value);
    void filters(FilterSelection selection);
    void sort(std::string value);
    void submit();
    void next_page();
    /// out: re-fetches the page preceding the visible window, when available.
    void previous_page();
    void facets();
private:
    void fetch(std::size_t page, bool prepend = false);
    struct Page { std::optional<std::string> cursor; std::uint64_t start; std::size_t size; };
    std::vector<Page> pages_;
    std::size_t first_page_ = 0;
    std::size_t last_page_ = 0;
    void invalidate();
    Rpc& rpc_;
    Timer& timer_;
    std::optional<Timer::Token> timer_token_;
};

struct BuildFile { std::string path; std::string text; };
struct BuildDisclosure {
    std::string repository;
    std::string commit;
    std::vector<BuildFile> files;
    bool mutable_sources = false;
};
struct DetailsState {
    std::string id;
    std::optional<protocol::GetResult> package;
    std::optional<protocol::PackageSummary> summary;
    std::optional<protocol::CommunityInfo> community;
    std::optional<protocol::WikiGetResult> wiki;
    std::optional<protocol::VariantChoices> variants;
    std::vector<std::pair<Text, std::string>> facts;
    Text primary{"action.install", {}};
    Text build_disclosure{"detail.build_unavailable", {}};
    Text wiki_attribution{"wiki.attribution", {}};
    std::optional<BuildDisclosure> build;
    std::optional<Text> build_warning;
    bool update_available = false;
    std::optional<Text> error;
    std::optional<Text> wiki_error;
    bool loading = false;
    bool offline = false;
    bool can_open = false;
    bool can_remove = false;
};
class Details final : public Model<DetailsState> {
public:
    explicit Details(Rpc& rpc);
    void load(std::string id);
    /// out: releases inactive details and invalidates outstanding replies.
    void release();
    void summary(protocol::PackageSummary value);
    void load_build_files();
    void load_wiki(bool refresh);
    void variants();
    void community(std::optional<protocol::CommunityInfo> info);
    std::vector<std::pair<std::string, std::string>> community_rows(const Copy& copy) const;
    void build(BuildDisclosure disclosure);
    void updates(const protocol::UpdateReport& report);
    void offline(bool value);
private:
    Rpc& rpc_;
    std::uint64_t generation_ = 0;
};
struct VariantRow { protocol::VariantOption option; Text title; Text body; std::optional<Text> reason; };
struct VariantState {
    std::vector<VariantRow> options;
    std::optional<std::size_t> selected;
    std::optional<Text> note;
};
class VariantChooser final : public Model<VariantState> {
public:
    void choices(const protocol::VariantChoices& choices);
    void select(std::size_t index);
};

struct ConfirmState {
    std::optional<protocol::TransactionPlan> plan;
    std::vector<protocol::BuildFilesResult> build_files;
    std::size_t pending_build_files = 0;
    bool build_acknowledged = false;
    bool dependents_acknowledged = false;
    bool requires_dependents_ack = false;
    bool stale = false;
    bool busy = false;
    bool submitting = false;
    std::optional<std::string> job_id;
    std::optional<Text> error;
};
class Confirm final : public Model<ConfirmState> {
public:
    explicit Confirm(Rpc& rpc);
    void plan(protocol::TransactionPlan plan, bool requires_dependents_ack = false);
    void acknowledge_build(bool value);
    void acknowledge_dependents(bool value);
    void invalidate();
    void busy(bool value);
    bool can_confirm() const;
    Text action() const;
    void request(protocol::PlanParams params);
    void submit();
private:
    void fetch_omitted(protocol::BuildFilesParams request, protocol::BuildFilesResult files, std::uint64_t generation);
    void completed(Reply<protocol::JobIdResult> reply);
    std::optional<protocol::PlanParams> request_;
    std::uint64_t generation_ = 0;
    Rpc& rpc_;
};

struct ActivityState {
    std::optional<std::string> job_id;
    std::optional<protocol::JobsGetResult> job;
    std::optional<protocol::Progress> progress;
    std::optional<protocol::Question> question;
    std::deque<std::string> log;
    std::vector<protocol::JobSummary> queue;
    std::vector<protocol::JobSummary> recent;
    std::optional<Text> auth_message;
    std::optional<Text> error;
    std::optional<double> fraction;
    Text phase{"state.pending", {}};
    bool can_cancel = false;
    Text cancel_label{"activity.cannot_cancel", {}};
};
class Activity final : public Model<ActivityState> {
public:
    explicit Activity(Rpc& rpc, std::size_t log_bytes = 65536);
    void attach(std::string job_id);
    void refresh();
    void cancel_capability(bool allowed, bool after_step);
    void cancel();
    void answer(std::string choice);
    void dequeue(std::string job_id);
private:
    void progress(const protocol::Progress& progress);
    void log(std::string_view line);
    Rpc& rpc_;
    std::size_t log_limit_;
    std::size_t log_size_ = 0;
    std::uint64_t generation_ = 0;
    std::vector<Subscription> events_;
};

struct UpdateIndex { std::size_t source; std::size_t item; };
struct UpdateGroup { Text title; std::vector<UpdateIndex> items; bool expanded = false; };
struct UpdatesState {
    std::optional<protocol::UpdateReport> report;
    std::array<UpdateGroup, 5> groups;
    std::optional<protocol::UpdatesOptionsResult> options;
    std::optional<protocol::UpdatesPlanResult> plan;
    std::optional<Text> error;
    std::optional<std::string> check_job;
    bool loading = false;
    bool news_available = true;
    bool missing_news_acknowledged = false;
};
class Updates final : public Model<UpdatesState> {
public:
    explicit Updates(Rpc& rpc);
    void report(protocol::UpdateReport report);
    void check();
    void acknowledge_news(std::size_t index, bool acknowledged);
    void news_available(bool available);
    void acknowledge_missing_news(bool acknowledged);
    bool can_update() const;
    std::optional<Text> item_blocker() const;
    void plan_system(std::optional<std::string> also_install = {});
    void plan_item(std::string kind, std::string id);
private:
    void request_plan(protocol::PlanSelection selection, std::optional<std::string> also_install);
    Rpc& rpc_;
    std::uint64_t generation_ = 0;
};

enum class InstalledView { Apps, Components, Git, Outside };
struct InstalledState {
    InstalledView view = InstalledView::Apps;
    std::string query;
    std::vector<protocol::PackageSummary> packages;
    std::vector<protocol::ManualReceipt> receipts;
    std::vector<protocol::LauncherEntry> outside;
    std::optional<Text> error;
    bool loading = false;
};
class Installed final : public Model<InstalledState> {
public:
    explicit Installed(Rpc& rpc);
    void view(InstalledView view);
    /// out: releases inactive lists and invalidates outstanding replies.
    void release();
    void query(std::string query);
    void refresh();
private:
    Rpc& rpc_;
    std::uint64_t generation_ = 0;
};

/// pre: shell, details and installed outlive the returned subscription
/// out: releases inactive pages on navigation and reloads them on return
Subscription bind_page_retention(Shell& shell, Details& details, Installed& installed);

enum class GitStep { Url, Review, Scope, Progress };
struct GitState {
    GitStep step = GitStep::Url;
    std::string url;
    std::string script;
    std::optional<std::string> receipt_id;
    std::optional<protocol::ManualPreview> preview;
    std::optional<protocol::TransactionPlan> plan;
    std::optional<protocol::Question> redirect;
    std::optional<std::string> job_id;
    std::optional<protocol::ManualReceipt> result;
    std::optional<Text> error;
    std::string scope;
    bool acknowledged = false;
    bool loading = false;
    bool failed = false;
};
class GitWizard final : public Model<GitState> {
public:
    explicit GitWizard(Rpc& rpc);
    void upgrade(std::string receipt_id);
    void inspect(std::string url, std::string explicit_script, std::string scope);
    void preview(protocol::ManualPreview preview);
    void redirect(protocol::Question question);
    void confirm_redirect(bool proceed);
    void acknowledge(bool value);
    void next();
    void back();
    void scope(std::string value);
    void install();
    void result(std::optional<protocol::ManualReceipt> receipt);
    static bool valid_url(std::string_view value);
private:
    Rpc& rpc_;
    std::uint64_t generation_ = 0;
};

struct SettingsState {
    std::optional<protocol::Settings> values;
    std::optional<protocol::SettingsGetResult> legacy;
    std::set<std::string> available_helpers;
    std::string effective_helper;
    std::optional<std::uint64_t> cache_bytes;
    bool active_transaction = false;
    bool cache_confirmation = false;
    bool clearing_cache = false;
    bool saving = false;
    std::optional<Text> error;
};
class Settings final : public Model<SettingsState> {
public:
    explicit Settings(Rpc& rpc);
    void refresh();
    void apply(protocol::SettingsPatch patch);
    void request_clear_cache();
    void clear_cache(bool confirmed);
    void capabilities(std::set<std::string> helpers, std::string effective, bool busy);
    std::optional<Text> helper_disabled(std::string_view helper) const;
private:
    void send_next();
    Rpc& rpc_;
    std::deque<protocol::SettingsPatch> pending_;
};

enum class AuthAction { Install, Remove, Update, Setup, Git };
struct AuthBinding {
    std::string operation_id;
    std::string challenge_id;
    AuthAction action = AuthAction::Install;
    std::string app;
    std::vector<std::string> accounts;
    std::chrono::steady_clock::time_point expires;
    bool native_agent = false;
    bool trusted_local_fallback = false;
};
class CredentialChannel {
public:
    virtual ~CredentialChannel() = default;
    virtual bool submit(const AuthBinding& binding, std::string_view account, std::span<const char> password) = 0;
    virtual void cancel(const AuthBinding& binding) = 0;
};
struct AuthState {
    Text operation;
    std::string account;
    bool caps_lock = false;
    bool show_password = false;
    bool active = false;
    bool password_ui = false;
    std::optional<Text> message;
};
class Auth final : public Model<AuthState> {
public:
    /// pre: channel authenticates the local broker and supplies operation-bound challenges; it never stores the password span.
    Auth(CredentialChannel& channel, Timer& timer);
    ~Auth() override;
    void bind(AuthBinding binding);
    void account(std::string account);
    void password(std::span<const char> bytes);
    std::span<const char> buffer() const;
    void caps_lock(bool value);
    void show_password(bool value);
    void submit();
    void cancel();
    void expire();
private:
    void wipe() noexcept;
    CredentialChannel& channel_;
    Timer& timer_;
    std::optional<Timer::Token> expiry_;
    std::optional<AuthBinding> binding_;
    std::vector<char> password_;
    std::size_t password_length_ = 0;
};
struct EmptyState { Text title; Text body; std::optional<Text> action; };
EmptyState empty_search(std::string query);
EmptyState empty_installed(InstalledView view);
struct FirstRunState {
    Text title{"first.title", {}};
    Text body{"first.body", {}};
    bool helper_available = false;
    bool setup_supported = false;
    std::string preferred_helper = "yay";
    std::optional<protocol::SetupStatusResult> status;
    std::optional<Text> error;
};
class FirstRun final : public Model<FirstRunState> {
public:
    explicit FirstRun(Rpc& rpc);
    void refresh();
    void review(Confirm& confirm);
    void capabilities(bool helper_available, bool setup_supported);
    void helper(std::string helper);
private:
    Rpc& rpc_;
};

}

#endif
