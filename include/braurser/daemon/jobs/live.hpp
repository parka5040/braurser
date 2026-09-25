#ifndef BRAURSER_DAEMON_JOBS_LIVE_HPP
#define BRAURSER_DAEMON_JOBS_LIVE_HPP

#include <braurser/daemon/jobs/jobs.hpp>
#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/helpers/aur_helper.hpp>
#include <braurser/daemon/updates/updates.hpp>
#include <braurser/daemon/manual/manual.hpp>

namespace braurser::daemon::jobs {

/// in: state is the private XDG state root and id identifies an existing user receipt.
/// out: a read-only removal plan bound to the complete saved receipt.
Result<Snapshot> user_removal_snapshot(const std::filesystem::path& state, std::string_view id);

class LivePlanSource final : public PlanSource {
public:
    using Report = std::function<std::optional<protocol::UpdateReport>()>;
    using PreviewLookup = std::function<std::optional<manual::Preview>(std::string_view)>;
    LivePlanSource(api::Services& services, core::ProcessRunner& runner, Report report,
                   std::filesystem::path state = {}, std::filesystem::path cache = {},
                   PreviewLookup preview = {});
    Result<Snapshot> snapshot(const protocol::PlanParams& selection) override;
private:
    api::Services& services_;
    core::ProcessRunner& runner_;
    Report report_;
    std::filesystem::path state_;
    std::filesystem::path cache_;
    PreviewLookup preview_;
    manual::CurlGitHttp git_http_;
    catalog::PacmanConfig config_;
    catalog::LocalDbReader local_;
    helpers::HelperLocator locator_;
    updates::UpdateSelectionPolicy policy_;
};

}

#endif
