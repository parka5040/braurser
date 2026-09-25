#ifndef BRAURSER_DAEMON_API_SERVICES_HPP
#define BRAURSER_DAEMON_API_SERVICES_HPP

#include <braurser/daemon/api/registry.hpp>
#include <braurser/protocol/dto.hpp>
#include <braurser/protocol/rpc.hpp>
#include <expected>

namespace braurser::daemon::api {

template<class T>
using ServiceResult = std::expected<T, protocol::ErrorObject>;

class Services {
public:
    virtual ~Services() = default;
    virtual ServiceResult<protocol::HelloResult> hello(const protocol::HelloParams&) = 0;
    virtual ServiceResult<protocol::StatusResult> status(const protocol::StatusParams&) = 0;
    virtual ServiceResult<protocol::SearchResult> search(const protocol::SearchParams&) = 0;
    virtual ServiceResult<protocol::FacetsResult> facets(const protocol::FacetsParams&) { return std::unexpected(protocol::ErrorObject{protocol::kUnsupportedCapability, "error.generic_title"}); }
    virtual ServiceResult<protocol::BuildFilesResult> build_files(const protocol::BuildFilesParams&) { return std::unexpected(protocol::ErrorObject{protocol::kUnsupportedCapability, "error.generic_title"}); }
    virtual ServiceResult<protocol::GetResult> get(const protocol::GetParams&) = 0;
    virtual ServiceResult<protocol::VariantsResult> variants(const protocol::VariantsParams&) = 0;
    virtual ServiceResult<protocol::JobIdResult> refresh(const protocol::RefreshParams&) = 0;
    virtual ServiceResult<protocol::WikiGetResult> wiki(const protocol::WikiGetParams&) = 0;
    virtual ServiceResult<protocol::NewsListResult> news(const protocol::NewsListParams&) = 0;
    virtual ServiceResult<protocol::JobIdResult> check_updates(const protocol::UpdatesCheckParams&) = 0;
    virtual ServiceResult<protocol::UpdatesOptionsResult> update_options(const protocol::UpdatesOptionsParams&) = 0;
    virtual ServiceResult<protocol::JobIdResult> preview(const protocol::ManualPreviewParams&) = 0;
    virtual ServiceResult<protocol::ManualListResult> manual_list(const protocol::ManualListParams&) = 0;
    virtual ServiceResult<protocol::UnmanagedListResult> unmanaged(const protocol::UnmanagedListParams&) = 0;
    virtual ServiceResult<protocol::SettingsGetResult> settings_get(const protocol::SettingsGetParams&) = 0;
    virtual ServiceResult<protocol::SettingsSetResult> settings_set(const protocol::SettingsSetParams&) = 0;
    virtual ServiceResult<protocol::NotifierCheckResult> notifier_check(const protocol::NotifierCheckParams&) = 0;
    virtual ServiceResult<protocol::ClearCacheResult> clear_cache(const protocol::ClearCacheParams&) { return std::unexpected(protocol::ErrorObject{protocol::kUnsupportedCapability, "error.generic_title"}); }
    virtual ServiceResult<protocol::RestartStateResult> restart_state(const protocol::RestartStateParams&) { return std::unexpected(protocol::ErrorObject{protocol::kUnsupportedCapability, "error.generic_title"}); }
    virtual ServiceResult<protocol::EmptyResult> restart_acknowledge(const protocol::RestartAcknowledgeParams&) { return std::unexpected(protocol::ErrorObject{protocol::kUnsupportedCapability, "error.generic_title"}); }
    virtual ServiceResult<protocol::SetupStatusResult> setup_status(const protocol::SetupStatusParams&) { return std::unexpected(protocol::ErrorObject{protocol::kUnsupportedCapability, "error.generic_title"}); }
};

/// in: registry is extensible by jobs and launcher integrations; services remain alive while handlers run.
/// out: registers all read-only daemon endpoints and settings mutations for this phase.
void register_handlers(HandlerRegistry& registry, Services& services);

}

#endif
