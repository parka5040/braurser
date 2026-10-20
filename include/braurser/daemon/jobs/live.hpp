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
    using Detail = std::function<api::ServiceResult<protocol::GetResult>(const protocol::GetParams&)>;
    /// in: report and preview are read only on the daemon thread; detail and runner must be safe on the planning thread.
    /// pre: when detail is empty, services.get is used and tasks must run on the daemon thread.
    LivePlanSource(api::Services& services, core::ProcessRunner& runner, Report report,
                   std::filesystem::path state = {}, std::filesystem::path cache = {},
                   PreviewLookup preview = {}, Detail detail = {});
    Result<Snapshot> snapshot(const protocol::PlanParams& selection) override;
    /// in: runs on the daemon thread; selection identifies the preview prepare copies.
    /// out: SHA-256 of the current update report and selected preview inputs.
    std::string inputs(const protocol::PlanParams& selection) override;
    /// in: runs on the daemon thread; captures the current update report and the selected preview.
    /// out: a task that uses only its own readers, the detail lookup and a runner that honours cancelled.
    PlanTask prepare(const protocol::PlanParams& selection) override;
private:
    struct Inputs {
        std::optional<protocol::UpdateReport> report;
        std::optional<manual::Preview> preview;
    };
    Result<Snapshot> compute(const protocol::PlanParams& selection, const Inputs& inputs,
                             const std::atomic<bool>& cancelled);
    api::Services& services_;
    core::ProcessRunner& runner_;
    Report report_;
    std::filesystem::path state_;
    std::filesystem::path cache_;
    PreviewLookup preview_;
    Detail detail_;
    catalog::LocalDbReader local_;
    updates::UpdateSelectionPolicy policy_;
};

}

#endif
