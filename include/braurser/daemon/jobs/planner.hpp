#ifndef BRAURSER_DAEMON_JOBS_PLANNER_HPP
#define BRAURSER_DAEMON_JOBS_PLANNER_HPP

#include <braurser/daemon/jobs/jobs.hpp>
#include <chrono>
#include <cstddef>
#include <memory>

namespace braurser::daemon::jobs {

struct PlanExecutorOptions {
    std::size_t max_plans{32};
    std::size_t max_approvals{320};
    std::chrono::milliseconds deadline{std::chrono::seconds(180)};
};

class ThreadPlanExecutor final : public PlanExecutor {
public:
    /// pre: constructed and used on the thread that runs the default GLib main context.
    /// in: options bound each lane's queue and the time from submit to delivery.
    /// out: one planning thread; results are delivered through the main loop.
    explicit ThreadPlanExecutor(PlanExecutorOptions options = {});
    ~ThreadPlanExecutor() override;
    ThreadPlanExecutor(const ThreadPlanExecutor&) = delete;
    ThreadPlanExecutor& operator=(const ThreadPlanExecutor&) = delete;
    /// out: a nonzero ticket; done runs on the main loop with kCancelled for explicit cancellation, else kBusy at or after the deadline, else the result.
    /// err: returns 0 without calling done when stopped or the lane is full.
    Ticket submit(PlanLane lane, PlanTask task, Done done) override;
    /// out: a queued task is dropped and a running one sees its cancel flag; done later receives kCancelled.
    void cancel(Ticket ticket) override;
    /// out: idempotent; cancels every task, joins the planning thread and drops undelivered results.
    void stop() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
