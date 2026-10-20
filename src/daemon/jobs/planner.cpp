#include <braurser/daemon/jobs/planner.hpp>

#include <glibmm/dispatcher.h>
#include <glibmm/main.h>
#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace braurser::daemon::jobs {

struct ThreadPlanExecutor::Impl {
    struct Item {
        Ticket id{};
        PlanTask task;
        Done done;
        std::atomic<bool> cancelled{false};
        std::chrono::steady_clock::time_point expires;
        bool cancel_requested{};
        bool expired{};
        bool delivered{};
        sigc::connection deadline;
    };

    explicit Impl(PlanExecutorOptions value) : options(value) {
        dispatch = dispatcher.connect([this] { drain(); });
        worker = std::jthread([this] { run(); });
    }

    PlanExecutorOptions options;
    std::map<Ticket, std::shared_ptr<Item>> items;
    Glib::Dispatcher dispatcher;
    sigc::connection dispatch;
    Ticket next{};
    bool stopped{};
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::shared_ptr<Item>> approvals;
    std::deque<std::shared_ptr<Item>> plans;
    std::deque<std::pair<std::shared_ptr<Item>, Result<Snapshot>>> finished;
    bool stopping{};
    std::jthread worker;

    void run() {
        for (;;) {
            std::shared_ptr<Item> item;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return stopping || !approvals.empty() || !plans.empty(); });
                if (stopping) return;
                auto& queue = approvals.empty() ? plans : approvals;
                item = std::move(queue.front());
                queue.pop_front();
            }
            auto result = [&]() -> Result<Snapshot> {
                if (item->cancelled.load())
                    return std::unexpected(Error{protocol::kCancelled, "error.generic_title"});
                if (std::chrono::steady_clock::now() >= item->expires) {
                    item->cancelled.store(true);
                    return std::unexpected(Error{protocol::kBusy, "error.generic_title"});
                }
                try { return item->task(item->cancelled); }
                catch (...) { return std::unexpected(Error{protocol::kUntrustedInput, "error.generic_title"}); }
            }();
            item->task = nullptr;
            {
                std::lock_guard lock(mutex);
                finished.emplace_back(std::move(item), std::move(result));
            }
            dispatcher.emit();
        }
    }

    void drain() {
        decltype(finished) completed;
        {
            std::lock_guard lock(mutex);
            completed.swap(finished);
        }
        for (auto& [item, result] : completed) {
            if (stopped || item->delivered) continue;
            item->delivered = true;
            item->deadline.disconnect();
            items.erase(item->id);
            auto done = std::move(item->done);
            item->expired = std::chrono::steady_clock::now() >= item->expires;
            if (item->cancel_requested)
                done(std::unexpected(Error{protocol::kCancelled, "error.generic_title"}));
            else if (item->expired) {
                item->cancelled.store(true);
                done(std::unexpected(Error{protocol::kBusy, "error.generic_title"}));
            } else done(std::move(result));
        }
    }

    Ticket submit(PlanLane lane, PlanTask task, Done done) {
        if (stopped || !task || !done) return 0;
        std::lock_guard lock(mutex);
        auto& queue = lane == PlanLane::Approval ? approvals : plans;
        const auto maximum = lane == PlanLane::Approval ? options.max_approvals : options.max_plans;
        if (queue.size() >= maximum) return 0;
        auto item = std::make_shared<Item>();
        item->id = ++next;
        item->task = std::move(task);
        item->done = std::move(done);
        item->expires = std::chrono::steady_clock::now() + options.deadline;
        items.emplace(item->id, item);
        const auto delay = static_cast<unsigned int>(std::clamp<std::chrono::milliseconds::rep>(
            options.deadline.count(), 0, std::numeric_limits<unsigned int>::max()));
        item->deadline = Glib::signal_timeout().connect([this, weak = std::weak_ptr<Item>(item)] {
            auto current = weak.lock();
            if (!current || stopped || current->delivered) return false;
            current->expired = true;
            current->cancelled.store(true);
            current->delivered = true;
            items.erase(current->id);
            auto callback = std::move(current->done);
            callback(std::unexpected(Error{current->cancel_requested ? protocol::kCancelled : protocol::kBusy,
                                           "error.generic_title"}));
            return false;
        }, delay);
        queue.push_back(item);
        ready.notify_one();
        return item->id;
    }

    void cancel(Ticket ticket) {
        const auto found = items.find(ticket);
        if (found == items.end() || found->second->delivered) return;
        auto item = found->second;
        item->cancel_requested = true;
        item->cancelled.store(true);
        {
            std::lock_guard lock(mutex);
            for (auto* queue : {&approvals, &plans}) {
                const auto queued = std::find(queue->begin(), queue->end(), item);
                if (queued == queue->end()) continue;
                queue->erase(queued);
                finished.emplace_back(item, std::unexpected(Error{protocol::kCancelled, "error.generic_title"}));
                break;
            }
        }
        dispatcher.emit();
    }

    void stop() {
        if (stopped) return;
        stopped = true;
        for (auto& [id, item] : items) {
            static_cast<void>(id);
            item->cancel_requested = true;
            item->cancelled.store(true);
            item->deadline.disconnect();
        }
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        worker.request_stop();
        ready.notify_one();
        if (worker.joinable()) worker.join();
        approvals.clear();
        plans.clear();
        finished.clear();
        items.clear();
        dispatch.disconnect();
    }
};

ThreadPlanExecutor::ThreadPlanExecutor(PlanExecutorOptions options) : impl_(std::make_unique<Impl>(options)) {}
ThreadPlanExecutor::~ThreadPlanExecutor() { stop(); }
PlanExecutor::Ticket ThreadPlanExecutor::submit(PlanLane lane, PlanTask task, Done done) {
    return impl_->submit(lane, std::move(task), std::move(done));
}
void ThreadPlanExecutor::cancel(Ticket ticket) { impl_->cancel(ticket); }
void ThreadPlanExecutor::stop() { impl_->stop(); }

}
