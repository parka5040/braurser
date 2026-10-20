#include <braurser/daemon/jobs/planner.hpp>

#include <glibmm/init.h>
#include <glibmm/main.h>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>

namespace {
namespace jobs = braurser::daemon::jobs;
namespace protocol = braurser::protocol;
using namespace std::chrono_literals;

class PlanExecutor : public ::testing::Test {
protected:
    void SetUp() override { Glib::init(); }
};

template<class Predicate>
bool await(Predicate condition, bool dispatch = true) {
    auto context = Glib::MainContext::get_default();
    const auto until = std::chrono::steady_clock::now() + 4s;
    while (!condition() && std::chrono::steady_clock::now() < until) {
        if (dispatch) context->iteration(false);
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

struct Gate {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    std::atomic<bool> observed{false};
    jobs::Result<jobs::Snapshot> run(const std::atomic<bool>& cancelled) {
        started.store(true);
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!release.load() && !cancelled.load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        observed.store(cancelled.load());
        return jobs::Snapshot{};
    }
};

jobs::Result<jobs::Snapshot> success(const std::atomic<bool>&) { return jobs::Snapshot{}; }

TEST_F(PlanExecutor, MainThreadFifoAndApprovalPriority) {
    Gate gate;
    std::vector<int> order;
    int completed{};
    const auto main = std::this_thread::get_id();
    jobs::ThreadPlanExecutor executor;
    auto done = [&](jobs::Result<jobs::Snapshot> result) {
        EXPECT_EQ(std::this_thread::get_id(), main);
        EXPECT_TRUE(result);
        ++completed;
    };
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) {
        order.push_back(0);
        return gate.run(cancel);
    }, done), 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    for (int index = 1; index <= 4; ++index) {
        const auto lane = index <= 2 ? jobs::PlanLane::Plan : jobs::PlanLane::Approval;
        ASSERT_NE(executor.submit(lane, [&, index](const auto&) {
            order.push_back(index);
            return jobs::Snapshot{};
        }, done), 0U);
    }
    gate.release.store(true);
    ASSERT_TRUE(await([&] { return completed == 5; }));
    EXPECT_EQ(order, (std::vector<int>{0, 3, 4, 1, 2}));
}

TEST_F(PlanExecutor, FullLanesRejectWithoutCallback) {
    Gate gate;
    int accepted{};
    int rejected{};
    jobs::ThreadPlanExecutor executor({1, 1, 3s});
    auto done = [&](auto) { ++accepted; };
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) { return gate.run(cancel); }, done), 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    for (const auto lane : {jobs::PlanLane::Plan, jobs::PlanLane::Approval}) {
        EXPECT_NE(executor.submit(lane, success, done), 0U);
        EXPECT_EQ(executor.submit(lane, success, [&](auto) { ++rejected; }), 0U);
    }
    EXPECT_EQ(executor.submit(jobs::PlanLane::Plan, {}, done), 0U);
    EXPECT_EQ(executor.submit(jobs::PlanLane::Plan, success, {}), 0U);
    gate.release.store(true);
    ASSERT_TRUE(await([&] { return accepted == 3; }));
    EXPECT_EQ(rejected, 0);
}

TEST_F(PlanExecutor, QueuedCancelIsDeferredAndNeverRuns) {
    Gate gate;
    bool called{};
    std::atomic<bool> ran{false};
    jobs::ThreadPlanExecutor executor;
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) { return gate.run(cancel); }, [](auto) {}), 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    const auto ticket = executor.submit(jobs::PlanLane::Approval, [&](const auto&) {
        ran.store(true);
        return jobs::Snapshot{};
    }, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kCancelled);
        called = true;
    });
    ASSERT_NE(ticket, 0U);
    executor.cancel(ticket);
    EXPECT_FALSE(called);
    ASSERT_TRUE(await([&] { return called; }));
    EXPECT_FALSE(ran.load());
    executor.cancel(ticket);
    executor.cancel(0);
}

TEST_F(PlanExecutor, RunningCancelIsObserved) {
    Gate gate;
    bool called{};
    jobs::ThreadPlanExecutor executor;
    const auto ticket = executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) { return gate.run(cancel); }, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kCancelled);
        called = true;
    });
    ASSERT_NE(ticket, 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    executor.cancel(ticket);
    EXPECT_FALSE(called);
    ASSERT_TRUE(await([&] { return called; }));
    EXPECT_TRUE(gate.observed.load());
}

TEST_F(PlanExecutor, DeadlineDeliversBusyBeforeLateResult) {
    Gate gate;
    std::atomic<bool> finished{false};
    int called{};
    jobs::ThreadPlanExecutor executor({2, 2, 100ms});
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) {
        gate.started.store(true);
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!gate.release.load() && std::chrono::steady_clock::now() < until) {
            if (cancel.load()) gate.observed.store(true);
            std::this_thread::sleep_for(1ms);
        }
        finished.store(true);
        return jobs::Snapshot{};
    }, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kBusy);
        EXPECT_FALSE(finished.load());
        ++called;
    }), 0U);
    ASSERT_TRUE(await([&] { return called == 1 && gate.observed.load(); }));
    gate.release.store(true);
    bool barrier{};
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, success, [&](auto result) {
        EXPECT_TRUE(result);
        barrier = true;
    }), 0U);
    ASSERT_TRUE(await([&] { return barrier; }));
    EXPECT_TRUE(finished.load());
    EXPECT_EQ(called, 1);
}

TEST_F(PlanExecutor, LateCompletionDeliversBusyExactlyOnce) {
    std::atomic<bool> finished{false};
    int called{};
    jobs::ThreadPlanExecutor executor({2, 2, 100ms});
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto&) {
        finished.store(true);
        return jobs::Snapshot{};
    }, [&](auto result) {
        ++called;
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kBusy);
    }), 0U);
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(finished.load());
    EXPECT_EQ(called, 0);
    ASSERT_TRUE(await([&] { return called == 1; }));
    executor.stop();
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_EQ(called, 1);
}

TEST_F(PlanExecutor, LateStartNeverRunsAndDeliversBusyExactlyOnce) {
    Gate gate;
    std::atomic<int> invocations{0};
    std::atomic<bool> barrier{false};
    int called{};
    jobs::ThreadPlanExecutor executor({3, 2, 100ms});
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) {
        return gate.run(cancel);
    }, [](auto) {}), 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto&) {
        invocations.fetch_add(1);
        return jobs::Snapshot{};
    }, [&](auto result) {
        ++called;
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kBusy);
    }), 0U);
    std::this_thread::sleep_for(300ms);
    gate.release.store(true);
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto&) {
        barrier.store(true);
        return jobs::Snapshot{};
    }, [](auto) {}), 0U);
    ASSERT_TRUE(await([&] { return barrier.load(); }, false));
    EXPECT_EQ(invocations.load(), 0);
    ASSERT_TRUE(await([&] { return called == 1; }));
    executor.stop();
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_EQ(called, 1);
}

TEST_F(PlanExecutor, ExplicitCancelWinsPastDeadlineExactlyOnce) {
    std::atomic<bool> finished{false};
    int called{};
    jobs::ThreadPlanExecutor executor({2, 2, 100ms});
    const auto ticket = executor.submit(jobs::PlanLane::Plan, [&](const auto&) {
        finished.store(true);
        return jobs::Snapshot{};
    }, [&](auto result) {
        ++called;
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kCancelled);
    });
    ASSERT_NE(ticket, 0U);
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(finished.load());
    executor.cancel(ticket);
    EXPECT_EQ(called, 0);
    ASSERT_TRUE(await([&] { return called == 1; }));
    executor.stop();
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_EQ(called, 1);
}

TEST_F(PlanExecutor, ExplicitCancelWinsAfterWorkerExpiry) {
    Gate gate;
    std::atomic<int> invocations{0};
    std::atomic<bool> barrier{false};
    int called{};
    jobs::ThreadPlanExecutor executor({3, 2, 100ms});
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) {
        return gate.run(cancel);
    }, [](auto) {}), 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    const auto ticket = executor.submit(jobs::PlanLane::Plan, [&](const auto&) {
        invocations.fetch_add(1);
        return jobs::Snapshot{};
    }, [&](auto result) {
        ++called;
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kCancelled);
    });
    ASSERT_NE(ticket, 0U);
    std::this_thread::sleep_for(300ms);
    gate.release.store(true);
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto&) {
        barrier.store(true);
        return jobs::Snapshot{};
    }, [](auto) {}), 0U);
    ASSERT_TRUE(await([&] { return barrier.load(); }, false));
    EXPECT_EQ(invocations.load(), 0);
    executor.cancel(ticket);
    ASSERT_TRUE(await([&] { return called == 1; }));
    executor.stop();
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_EQ(called, 1);
}

TEST_F(PlanExecutor, ExplicitCancelWinsWhenDeadlineTimerRunsFirst) {
    Gate gate;
    int called{};
    jobs::ThreadPlanExecutor executor({2, 2, 100ms});
    const auto ticket = executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) {
        gate.started.store(true);
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!gate.release.load() && std::chrono::steady_clock::now() < until) {
            if (cancel.load()) gate.observed.store(true);
            std::this_thread::sleep_for(1ms);
        }
        return jobs::Snapshot{};
    }, [&](auto result) {
        ++called;
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kCancelled);
    });
    ASSERT_NE(ticket, 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    std::this_thread::sleep_for(300ms);
    executor.cancel(ticket);
    ASSERT_TRUE(await([&] { return called == 1; }));
    gate.release.store(true);
    executor.stop();
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_EQ(called, 1);
}

TEST_F(PlanExecutor, StopJoinsDropsCallbacksAndIsIdempotent) {
    Gate gate;
    int called{};
    jobs::ThreadPlanExecutor executor;
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) { return gate.run(cancel); }, [&](auto) { ++called; }), 0U);
    ASSERT_TRUE(await([&] { return gate.started.load(); }));
    ASSERT_NE(executor.submit(jobs::PlanLane::Approval, success, [&](auto) { ++called; }), 0U);
    executor.stop();
    EXPECT_TRUE(gate.observed.load());
    EXPECT_EQ(called, 0);
    EXPECT_EQ(executor.submit(jobs::PlanLane::Plan, success, [&](auto) { ++called; }), 0U);
    executor.stop();
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_EQ(called, 0);
}

TEST_F(PlanExecutor, DestructorCancelsInFlight) {
    Gate gate;
    bool called{};
    {
        jobs::ThreadPlanExecutor executor;
        ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [&](const auto& cancel) { return gate.run(cancel); }, [&](auto) { called = true; }), 0U);
        ASSERT_TRUE(await([&] { return gate.started.load(); }));
    }
    EXPECT_TRUE(gate.observed.load());
    Glib::MainContext::get_default()->iteration(false);
    EXPECT_FALSE(called);
}

TEST_F(PlanExecutor, ExceptionsBecomeUntrustedInput) {
    bool called{};
    jobs::ThreadPlanExecutor executor;
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, [](const auto&) -> jobs::Result<jobs::Snapshot> {
        throw 7;
    }, [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, protocol::kUntrustedInput);
        called = true;
    }), 0U);
    ASSERT_TRUE(await([&] { return called; }));
}

TEST_F(PlanExecutor, DoneCanSubmitReentrantly) {
    int called{};
    jobs::ThreadPlanExecutor executor;
    ASSERT_NE(executor.submit(jobs::PlanLane::Plan, success, [&](auto result) {
        EXPECT_TRUE(result);
        ++called;
        EXPECT_NE(executor.submit(jobs::PlanLane::Approval, success, [&](auto nested) {
            EXPECT_TRUE(nested);
            ++called;
        }), 0U);
    }), 0U);
    ASSERT_TRUE(await([&] { return called == 2; }));
}

}
