#ifndef BRAURSER_PROTOCOL_METHODS_HPP
#define BRAURSER_PROTOCOL_METHODS_HPP

#include <braurser/protocol/dto.hpp>
#include <array>
#include <string_view>

namespace braurser::protocol {

/// in: value is an untrusted DTO payload.
/// out: true only when the shared DTO rules accept it without materializing it.
template <class T>
bool accepts(const nlohmann::json& value) {
    try {
        validate_json(value, TypeTag<T>{});
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

struct MethodDescriptor {
    std::string_view name;
    std::string_view params_type;
    std::string_view result_type;
    bool (*validate_params)(const nlohmann::json&);
    bool (*validate_result)(const nlohmann::json&);
};

#define BRAURSER_PROTOCOL_METHODS(X) \
    X("system.hello", HelloParams, HelloResult) \
    X("system.status", StatusParams, StatusResult) \
    X("catalog.search", SearchParams, SearchResult) \
    X("catalog.facets", FacetsParams, FacetsResult) \
    X("catalog.buildFiles", BuildFilesParams, BuildFilesResult) \
    X("catalog.get", GetParams, GetResult) \
    X("catalog.variants", VariantsParams, VariantsResult) \
    X("catalog.refresh", RefreshParams, JobIdResult) \
    X("wiki.get", WikiGetParams, WikiGetResult) \
    X("news.list", NewsListParams, NewsListResult) \
    X("updates.check", UpdatesCheckParams, JobIdResult) \
    X("updates.options", UpdatesOptionsParams, UpdatesOptionsResult) \
    X("updates.plan", UpdatesPlanParams, UpdatesPlanResult) \
    X("jobs.plan", PlanParams, TransactionPlan) \
    X("jobs.install", InstallParams, JobIdResult) \
    X("jobs.remove", RemoveParams, JobIdResult) \
    X("jobs.upgradeSystem", UpgradeSystemParams, JobIdResult) \
    X("jobs.upgradeCommunity", UpgradeCommunityParams, JobIdResult) \
    X("jobs.get", JobsGetParams, JobsGetResult) \
    X("jobs.list", JobsListParams, JobsListResult) \
    X("jobs.cancel", JobsCancelParams, JobsCancelResult) \
    X("jobs.dequeue", JobsDequeueParams, JobsDequeueResult) \
    X("jobs.answer", JobsAnswerParams, AcceptedResult) \
    X("manual.preview", ManualPreviewParams, JobIdResult) \
    X("manual.install", ManualInstallParams, JobIdResult) \
    X("manual.upgrade", ManualUpgradeParams, JobIdResult) \
    X("manual.list", ManualListParams, ManualListResult) \
    X("manual.remove", ManualRemoveParams, JobIdResult) \
    X("unmanaged.list", UnmanagedListParams, UnmanagedListResult) \
    X("settings.get", SettingsGetParams, SettingsGetResult) \
    X("settings.set", SettingsSetParams, SettingsSetResult) \
    X("settings.clearCache", ClearCacheParams, ClearCacheResult) \
    X("restart.state", RestartStateParams, RestartStateResult) \
    X("restart.acknowledge", RestartAcknowledgeParams, EmptyResult) \
    X("setup.status", SetupStatusParams, SetupStatusResult) \
    X("setup.helper", SetupHelperParams, JobIdResult) \
    X("notifier.check", NotifierCheckParams, NotifierCheckResult)

#define BRAURSER_PROTOCOL_NOTIFICATIONS(X) \
    X("job.progress", JobProgressNotification) \
    X("job.log", JobLogNotification) \
    X("job.question", JobQuestionNotification) \
    X("job.authState", JobAuthStateNotification) \
    X("job.state", JobStateNotification) \
    X("catalog.changed", CatalogChangedNotification) \
    X("updates.available", UpdatesAvailableNotification)

#define BRAURSER_METHOD_ENTRY(name, params, result) MethodDescriptor{name, #params, #result, &accepts<params>, &accepts<result>},
inline constexpr auto kMethods = std::to_array<MethodDescriptor>({BRAURSER_PROTOCOL_METHODS(BRAURSER_METHOD_ENTRY)});
#undef BRAURSER_METHOD_ENTRY

struct NotificationDescriptor {
    std::string_view name;
    std::string_view params_type;
    bool (*validate_params)(const nlohmann::json&);
};

#define BRAURSER_NOTIFICATION_ENTRY(name, params) NotificationDescriptor{name, #params, &accepts<params>},
inline constexpr auto kNotifications = std::to_array<NotificationDescriptor>({BRAURSER_PROTOCOL_NOTIFICATIONS(BRAURSER_NOTIFICATION_ENTRY)});
#undef BRAURSER_NOTIFICATION_ENTRY

}

#endif
