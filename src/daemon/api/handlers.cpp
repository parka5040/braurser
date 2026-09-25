#include <braurser/daemon/api/services.hpp>

namespace braurser::daemon::api {
namespace {
template<class Method, class Result>
void bind(HandlerRegistry& registry, Services& services,
          ServiceResult<Result> (Services::*operation)(const typename Method::Params&)) {
    registry.register_handler<Method>([&services, operation](const auto& params, Reply<Result> reply) {
        auto result = (services.*operation)(params);
        if (result) reply.ok(*result);
        else reply.error(result.error());
    });
}
}

void register_handlers(HandlerRegistry& registry, Services& services) {
    bind<HelloParamsMethod>(registry, services, &Services::hello);
    bind<StatusParamsMethod>(registry, services, &Services::status);
    bind<SearchParamsMethod>(registry, services, &Services::search);
    bind<FacetsParamsMethod>(registry, services, &Services::facets);
    bind<BuildFilesParamsMethod>(registry, services, &Services::build_files);
    bind<GetParamsMethod>(registry, services, &Services::get);
    bind<VariantsParamsMethod>(registry, services, &Services::variants);
    bind<RefreshParamsMethod>(registry, services, &Services::refresh);
    bind<WikiGetParamsMethod>(registry, services, &Services::wiki);
    bind<NewsListParamsMethod>(registry, services, &Services::news);
    bind<UpdatesCheckParamsMethod>(registry, services, &Services::check_updates);
    bind<UpdatesOptionsParamsMethod>(registry, services, &Services::update_options);
    bind<ManualPreviewParamsMethod>(registry, services, &Services::preview);
    bind<ManualListParamsMethod>(registry, services, &Services::manual_list);
    bind<UnmanagedListParamsMethod>(registry, services, &Services::unmanaged);
    bind<SettingsGetParamsMethod>(registry, services, &Services::settings_get);
    bind<SettingsSetParamsMethod>(registry, services, &Services::settings_set);
    bind<NotifierCheckParamsMethod>(registry, services, &Services::notifier_check);
    bind<ClearCacheParamsMethod>(registry, services, &Services::clear_cache);
    bind<RestartStateParamsMethod>(registry, services, &Services::restart_state);
    bind<RestartAcknowledgeParamsMethod>(registry, services, &Services::restart_acknowledge);
    bind<SetupStatusParamsMethod>(registry, services, &Services::setup_status);
#define BRAURSER_UNSUPPORTED(method) \
    registry.register_handler<method>([](const auto&, auto reply) { \
        reply.error({protocol::kUnsupportedCapability, "error.generic_title"}); \
    });
    BRAURSER_UNSUPPORTED(PlanParamsMethod)
    BRAURSER_UNSUPPORTED(InstallParamsMethod)
    BRAURSER_UNSUPPORTED(RemoveParamsMethod)
    BRAURSER_UNSUPPORTED(UpgradeSystemParamsMethod)
    BRAURSER_UNSUPPORTED(UpgradeCommunityParamsMethod)
    BRAURSER_UNSUPPORTED(ManualInstallParamsMethod)
    BRAURSER_UNSUPPORTED(ManualUpgradeParamsMethod)
    BRAURSER_UNSUPPORTED(ManualRemoveParamsMethod)
    BRAURSER_UNSUPPORTED(SetupHelperParamsMethod)
    BRAURSER_UNSUPPORTED(JobsDequeueParamsMethod)
#undef BRAURSER_UNSUPPORTED
}

}
