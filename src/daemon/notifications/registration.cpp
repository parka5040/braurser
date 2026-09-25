#include <braurser/daemon/notifications/service.hpp>
#include "../app/services.hpp"

namespace braurser::daemon::notifications {

void register_service(api::HandlerRegistry& registry, app::RealServices& services,
                      const std::filesystem::path& state, const std::filesystem::path& launcher) {
    auto transport = std::make_shared<DesktopTransport>(launcher);
    auto notifier = std::make_shared<NotificationService>(state, *transport);
    registry.register_handler<api::NotifierCheckParamsMethod>([&services, transport, notifier](
        const protocol::NotifierCheckParams& params, api::Reply<protocol::NotifierCheckResult> reply) {
        auto result = services.notifier_check(params);
        if (!result) { reply.error(result.error()); return; }
        if (result->updateCount > 0) {
            const auto settings = services.settings_get({});
            if (settings && settings->notifications) {
                const auto report = services.latest_report();
                if (report) result->notificationSent = notifier->notify(*report);
            }
        }
        reply.ok(*result);
    });
}

}
