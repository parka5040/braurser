#include <braurser/presenter/models.hpp>
#include <braurser/daemon/core/text.hpp>
#include <algorithm>

namespace braurser::presenter {
Activity::Activity(Rpc& rpc, std::size_t log_bytes) : rpc_(rpc), log_limit_(log_bytes) {
    if (log_limit_ < 64 || log_limit_ > 1024 * 1024) { throw std::invalid_argument("invalid log bound"); }
    events_.push_back(rpc_.listen<protocol::JobProgressNotification>([this](const auto& event) {
        if (state_.job_id != event.jobId) { return; }
        progress(event.progress); changed();
    }));
    events_.push_back(rpc_.listen<protocol::JobLogNotification>([this](const auto& event) {
        if (state_.job_id != event.jobId) { return; }
        log(event.line); changed();
    }));
    events_.push_back(rpc_.listen<protocol::JobQuestionNotification>([this](const auto& event) {
        if (state_.job_id != event.jobId) { return; }
        state_.question = event.question; changed();
    }));
    events_.push_back(rpc_.listen<protocol::JobAuthStateNotification>([this](const auto& event) {
        if (state_.job_id != event.jobId) { return; }
        const auto& value = event.state.value;
        if (value == "required") { state_.auth_message = Text{"activity.auth_wait", {}}; }
        else if (value == "prompting") { state_.auth_message = Text{"auth.native_wait", {}}; }
        else if (value == "cancelled") { state_.auth_message = Text{"auth.canceled", {}}; }
        else if (value == "unavailable") { state_.auth_message = Text{"auth.unavailable_title", {}}; }
        else if (value == "failed") { state_.auth_message = Text{"auth.failed", {}}; }
        else { state_.auth_message.reset(); }
        changed();
    }));
    events_.push_back(rpc_.listen<protocol::JobStateNotification>([this](const auto& event) {
        if (state_.job_id != event.jobId) { return; }
        if (state_.job) { state_.job->state = event.state; }
        if (event.state.value == "succeeded" || event.state.value == "failed" || event.state.value == "cancelled") {
            state_.can_cancel = false; state_.question.reset(); refresh();
        }
        changed();
    }));
}
void Activity::attach(std::string job_id) {
    if (job_id.empty() || job_id.size() > 128) { throw std::invalid_argument("invalid job identifier"); }
    ++generation_; state_ = {}; state_.job_id = std::move(job_id); log_size_ = 0; refresh();
}
void Activity::refresh() {
    if (state_.job_id) {
        call(rpc_, protocol::JobsGetParams{*state_.job_id, {}}, [this, generation = generation_](Reply<protocol::JobsGetResult> reply) {
            if (generation != generation_) { return; }
            if (reply) { state_.job = *reply; if (reply->progress) { progress(*reply->progress); } }
            else { state_.error = error_text(reply.error().code); }
            changed();
        });
    }
    call(rpc_, protocol::JobsListParams{{}, 100}, [this](Reply<protocol::JobsListResult> reply) {
        if (reply) {
            state_.queue.clear(); state_.recent.clear();
            for (const auto& item : reply->jobs) {
                if (item.state.value == "queued" || item.state.value == "needs-attention" || item.state.value == "waiting-auth") { state_.queue.push_back(item); }
                else if (item.state.value == "succeeded" || item.state.value == "failed" || item.state.value == "cancelled") { state_.recent.push_back(item); }
            }
        } else { state_.error = error_text(reply.error().code); }
        changed();
    });
}
void Activity::progress(const protocol::Progress& value) {
    state_.progress = value; state_.fraction.reset();
    const auto& phase = value.phase.value;
    state_.phase = {phase == "downloading" ? "activity.downloading" : phase == "building" ? "activity.building" :
                    phase == "installing" ? "activity.installing_phase" : phase == "removing" ? "activity.removing_phase" :
                    phase == "checking" ? "updates.checking" : "activity.finishing", {}};
    if (phase != "building" && value.current && value.total && *value.total != 0 && *value.current <= *value.total) {
        state_.fraction = static_cast<double>(*value.current) / static_cast<double>(*value.total);
    }
}
void Activity::log(std::string_view line) {
    if (line.size() > 65536 || !protocol::valid_utf8(line)) { return; }
    auto clean = daemon::core::strip_terminal_controls(line);
    auto probe = clean;
    std::erase(probe, '"');
    std::erase(probe, '\'');
    if (daemon::core::redact(probe) != probe) { clean = "[REDACTED]"; }
    clean = daemon::core::bounded_line(clean, log_limit_ - 3);
    while (!state_.log.empty() && (log_size_ + clean.size() > log_limit_ || state_.log.size() >= 1024)) {
        log_size_ -= state_.log.front().size(); state_.log.pop_front();
    }
    log_size_ += clean.size(); state_.log.push_back(std::move(clean));
}
void Activity::cancel_capability(bool allowed, bool after_step) {
    state_.can_cancel = allowed;
    state_.cancel_label = {allowed ? (after_step ? "activity.cancel_after" : "action.cancel") : "activity.cannot_cancel", {}};
    changed();
}
void Activity::cancel() {
    if (!state_.job_id || !state_.can_cancel) { return; }
    state_.can_cancel = false;
    call(rpc_, protocol::JobsCancelParams{*state_.job_id}, [this, generation = generation_](Reply<protocol::JobsCancelResult> reply) {
        if (generation != generation_) { return; }
        state_.cancel_label = {reply && reply->accepted ? "activity.cancel_pending" : "activity.cannot_cancel", {}};
        if (!reply) { state_.error = error_text(reply.error().code); }
        changed();
    }); changed();
}
void Activity::dequeue(std::string job_id) {
    if (std::ranges::none_of(state_.queue, [&](const auto& item) { return item.jobId == job_id && item.state.value == "queued"; })) { return; }
    call(rpc_, protocol::JobsDequeueParams{std::move(job_id)}, [this](Reply<protocol::JobsDequeueResult> reply) {
        if (reply && reply->removed) { refresh(); }
        else { state_.error = reply ? Text{"activity.needs_attention", {}} : error_text(reply.error().code); changed(); }
    });
}
void Activity::answer(std::string choice) {
    if (!state_.job_id || !state_.question || choice.empty() || choice.size() > 512) { return; }
    const auto question = *state_.question;
    state_.question.reset();
    call(rpc_, protocol::JobsAnswerParams{*state_.job_id, question.question_id, std::move(choice), question.token}, [this, generation = generation_](Reply<protocol::AcceptedResult> reply) {
        if (generation != generation_) { return; }
        if (!reply) { state_.error = error_text(reply.error().code); }
        else if (!reply->accepted) { state_.error = Text{"activity.needs_attention", {}}; }
        changed();
    }); changed();
}

}
