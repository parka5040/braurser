#ifndef BRAURSER_DAEMON_JOBS_REGISTER_HPP
#define BRAURSER_DAEMON_JOBS_REGISTER_HPP

#include <braurser/daemon/api/broadcaster.hpp>
#include <braurser/daemon/api/services.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/jobs/live.hpp>
#include <functional>

namespace braurser::daemon::jobs {

/// in: server supplies authenticated connection identities and disconnect notifications; detail is safe off the daemon thread.
/// out: registers jobs handlers that plan on a background thread and returns an idempotent stop hook that starts no further job and joins job and planning workers.
/// pre: server outlives every planning completion.
std::function<void()> register_handlers(api::HandlerRegistry& registry, api::Services& services,
                       const core::XdgPaths& paths, api::Broadcaster& broadcaster,
                       LivePlanSource::Report report, api::Server& server,
                       LivePlanSource::PreviewLookup preview = {}, LivePlanSource::Detail detail = {});

}

#endif
