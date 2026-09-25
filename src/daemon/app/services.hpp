#ifndef BRAURSER_DAEMON_APP_SERVICES_HPP
#define BRAURSER_DAEMON_APP_SERVICES_HPP

#include <braurser/daemon/api/broadcaster.hpp>
#include <braurser/daemon/api/logger.hpp>
#include <braurser/daemon/api/services.hpp>
#include <braurser/daemon/cache/store.hpp>
#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/connectivity/checker.hpp>
#include <braurser/daemon/core/clock.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/news/feed.hpp>
#include <braurser/daemon/updates/updates.hpp>
#include <braurser/daemon/wiki/client.hpp>
#include <braurser/daemon/elevation/selector.hpp>
#include <giomm/filemonitor.h>
#include <atomic>
#include <thread>

namespace braurser::daemon::app {

class RealServices final : public api::Services {
public:
    explicit RealServices(core::XdgPaths paths, api::Broadcaster& broadcaster, api::DaemonLog* log = nullptr);
    ~RealServices() override;
    void start_monitors();
    void startup_refresh();
    std::optional<manual::Preview> preview_result(std::string_view id) const;
    std::optional<protocol::UpdateReport> latest_report() const;

    api::ServiceResult<protocol::HelloResult> hello(const protocol::HelloParams&) override;
    api::ServiceResult<protocol::StatusResult> status(const protocol::StatusParams&) override;
    api::ServiceResult<protocol::SearchResult> search(const protocol::SearchParams&) override;
    api::ServiceResult<protocol::FacetsResult> facets(const protocol::FacetsParams&) override;
    api::ServiceResult<protocol::BuildFilesResult> build_files(const protocol::BuildFilesParams&) override;
    api::ServiceResult<protocol::GetResult> get(const protocol::GetParams&) override;
    api::ServiceResult<protocol::VariantsResult> variants(const protocol::VariantsParams&) override;
    api::ServiceResult<protocol::JobIdResult> refresh(const protocol::RefreshParams&) override;
    api::ServiceResult<protocol::WikiGetResult> wiki(const protocol::WikiGetParams&) override;
    api::ServiceResult<protocol::NewsListResult> news(const protocol::NewsListParams&) override;
    api::ServiceResult<protocol::JobIdResult> check_updates(const protocol::UpdatesCheckParams&) override;
    api::ServiceResult<protocol::UpdatesOptionsResult> update_options(const protocol::UpdatesOptionsParams&) override;
    api::ServiceResult<protocol::JobIdResult> preview(const protocol::ManualPreviewParams&) override;
    api::ServiceResult<protocol::ManualListResult> manual_list(const protocol::ManualListParams&) override;
    api::ServiceResult<protocol::UnmanagedListResult> unmanaged(const protocol::UnmanagedListParams&) override;
    api::ServiceResult<protocol::SettingsGetResult> settings_get(const protocol::SettingsGetParams&) override;
    api::ServiceResult<protocol::SettingsSetResult> settings_set(const protocol::SettingsSetParams&) override;
    api::ServiceResult<protocol::NotifierCheckResult> notifier_check(const protocol::NotifierCheckParams&) override;
    api::ServiceResult<protocol::ClearCacheResult> clear_cache(const protocol::ClearCacheParams&) override;
    api::ServiceResult<protocol::RestartStateResult> restart_state(const protocol::RestartStateParams&) override;
    api::ServiceResult<protocol::EmptyResult> restart_acknowledge(const protocol::RestartAcknowledgeParams&) override;
    api::ServiceResult<protocol::SetupStatusResult> setup_status(const protocol::SetupStatusParams&) override;

private:
    core::XdgPaths paths_;
    api::Broadcaster& broadcaster_;
    api::DaemonLog* log_;
    core::SystemClock clock_;
    core::PosixProcessRunner runner_;
    core::CurlHttpClient http_;
    catalog::PacmanConfig config_;
    catalog::SyncDbReader sync_;
    catalog::LocalDbReader local_;
    catalog::LocalQuery query_;
    catalog::AurDumpFetcher aur_;
    catalog::AurRpcClient aur_rpc_;
    catalog::AppStreamReader appstream_;
    catalog::IconConverter icons_;
    cache::Store store_;
    wiki::WikiCache wiki_cache_;
    wiki::WikiClient wiki_client_;
    connectivity::GioNetworkManagerStatus nm_;
    connectivity::ConnectivityChecker connectivity_;
    news::NewsFeed news_;
    updates::OfficialUpdateChecker official_;
    updates::RunningConsumerScanner consumer_scanner_;
    manual::CurlGitHttp git_http_;
    manual::UnmanagedLauncherScanner launcher_scanner_;
    helpers::HelperLocator helper_locator_;
    elevation::ProcAgentProbe agent_probe_;
    elevation::ElevationSelector elevation_;
    protocol::Settings settings_;
    std::optional<protocol::UpdateReport> report_;
    std::optional<catalog::Repositories> repos_;
    std::jthread refresh_worker_;
    std::jthread updates_worker_;
    std::jthread preview_worker_;
    Glib::RefPtr<Gio::FileMonitor> sync_monitor_;
    Glib::RefPtr<Gio::FileMonitor> local_monitor_;
    sigc::connection debounce_;
    std::atomic<bool> refresh_busy_{false};
    std::atomic<bool> updates_busy_{false};
    std::atomic<bool> preview_busy_{false};
    std::optional<manual::Preview> last_preview_;
    std::uint64_t revision_{};
    std::uint64_t next_job_{};
    std::chrono::system_clock::time_point last_check_{};

    bool build_snapshot(bool forced);
    elevation::Environment elevation_environment() const;
    void run_updates(std::string job, bool refresh, catalog::Repositories repos, std::string helper_preference);
    std::optional<catalog::PackageDetails> package(std::string_view id);
};

}

#endif
