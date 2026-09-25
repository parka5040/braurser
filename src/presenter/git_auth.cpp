#include <braurser/presenter/models.hpp>
#include <algorithm>
#include <cstring>

namespace braurser::presenter {
GitWizard::GitWizard(Rpc& rpc) : rpc_(rpc) {}
void GitWizard::upgrade(std::string receipt_id) {
    if (receipt_id.empty() || receipt_id.size() > 128 || !protocol::valid_utf8(receipt_id)) { throw std::invalid_argument("invalid receipt identifier"); }
    ++generation_; state_ = {}; state_.receipt_id = std::move(receipt_id); changed();
}
bool GitWizard::valid_url(std::string_view value) {
    if (value.size() > 4096 || !value.starts_with("https://")) { return false; }
    value.remove_prefix(8);
    const auto slash = value.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == value.size()) { return false; }
    const auto host = value.substr(0, slash);
    if (host.size() > 253 || host.front() == '.' || host.back() == '.' || host.find("..") != std::string_view::npos) { return false; }
    if (std::ranges::any_of(host, [](unsigned char c) { return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'); })) { return false; }
    for (const auto c : value.substr(slash + 1)) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '/')) { return false; }
    }
    return value.find("/../") == std::string_view::npos && !value.ends_with("/..") && value.find("//") == std::string_view::npos;
}
void GitWizard::inspect(std::string url, std::string explicit_script, std::string scope_value) {
    if (state_.step != GitStep::Url || state_.loading) { return; }
    if (!valid_url(url)) { state_.error = Text{url.find('@') != std::string::npos ? "git.credentials_rejected" : "git.url_invalid", {}}; changed(); return; }
    if (explicit_script.empty() || explicit_script.size() > 4096 || explicit_script.front() == '/' || explicit_script.find("..") != std::string::npos ||
        explicit_script.find_first_of("\r\n\t\\") != std::string::npos || explicit_script.find('\0') != std::string::npos || !protocol::valid_utf8(explicit_script) ||
        (scope_value != "user" && scope_value != "system")) {
        state_.error = Text{"git.select_script", {}}; changed(); return;
    }
    ++generation_; state_.url = std::move(url); state_.script = std::move(explicit_script); state_.scope = std::move(scope_value);
    state_.preview.reset(); state_.redirect.reset(); state_.acknowledged = false; state_.error.reset(); state_.loading = true;
    call(rpc_, protocol::ManualPreviewParams{state_.url, state_.script, {state_.scope}}, [this, generation = generation_](Reply<protocol::JobIdResult> reply) {
        if (generation != generation_) { return; }
        if (reply) { state_.job_id = reply->jobId; }
        else { state_.loading = false; state_.error = error_text(reply.error().code); }
        changed();
    }); changed();
}
void GitWizard::preview(protocol::ManualPreview value) {
    if (!state_.job_id || state_.step == GitStep::Progress || state_.redirect) { return; }
    try {
        protocol::validate_wire(nlohmann::json(value));
        if (value.script_text.empty() || value.script_text.size() > 65536 || !protocol::valid_utf8(value.script_text) ||
            value.script_text.find('\0') != std::string::npos || value.script_path != state_.script || value.scope_options.empty() ||
            (value.commit.size() != 40 && value.commit.size() != 64) ||
            value.commit.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) { throw std::invalid_argument("invalid preview"); }
    } catch (const std::exception&) {
        state_.loading = false; state_.error = Text{value.script_text.size() > 65536 ? "git.script_too_large" : "git.unsupported", {}};
        state_.preview.reset(); state_.acknowledged = false; changed(); return;
    }
    state_.preview = std::move(value); state_.loading = false; state_.step = GitStep::Review; state_.acknowledged = false; state_.error.reset(); changed();
}
void GitWizard::redirect(protocol::Question question) {
    if (!state_.job_id || !std::holds_alternative<protocol::GitRedirectQuestion>(question.choice.value) || state_.step != GitStep::Url) { return; }
    state_.redirect = std::move(question); state_.loading = false; changed();
}
void GitWizard::confirm_redirect(bool proceed) {
    if (!state_.redirect || !state_.job_id || state_.loading) { return; }
    const auto question = *state_.redirect;
    state_.loading = true;
    call(rpc_, protocol::JobsAnswerParams{*state_.job_id, question.question_id, proceed ? "continue" : "cancel", question.token}, [this, proceed, generation = generation_](Reply<protocol::AcceptedResult> reply) {
        if (generation != generation_) { return; }
        if (reply && reply->accepted) {
            state_.redirect.reset(); state_.loading = proceed;
            if (!proceed) { state_.job_id.reset(); }
        } else { state_.loading = false; state_.error = reply ? Text{"git.fetch_failed", {}} : error_text(reply.error().code); }
        changed();
    }); changed();
}
void GitWizard::acknowledge(bool value) { if (state_.step == GitStep::Review && state_.preview) { state_.acknowledged = value; changed(); } }
void GitWizard::next() {
    if (state_.step != GitStep::Review || !state_.preview || !state_.acknowledged) { return; }
    state_.step = GitStep::Scope;
    const auto& options = state_.preview->scope_options;
    if (std::ranges::none_of(options, [this](const auto& option) { return option.value == state_.scope; })) { state_.scope.clear(); }
    if (!state_.scope.empty()) { scope(state_.scope); }
    changed();
}
void GitWizard::back() {
    if (state_.loading || state_.step == GitStep::Progress) { return; }
    if (state_.step == GitStep::Scope) { state_.step = GitStep::Review; }
    else if (state_.step == GitStep::Review) { state_.step = GitStep::Url; state_.preview.reset(); state_.acknowledged = false; ++generation_; }
    changed();
}
void GitWizard::scope(std::string value) {
    if (state_.step != GitStep::Scope || !state_.preview) { return; }
    if (std::ranges::none_of(state_.preview->scope_options, [&value](const auto& option) { return option.value == value; })) {
        state_.error = Text{"git.scope_unavailable", {}};
    } else { state_.scope = std::move(value); state_.error.reset(); }
    state_.plan.reset();
    if (state_.error) { changed(); return; }
    state_.loading = true;
    protocol::PlanParams params;
    params.kind = state_.receipt_id ? "manual-upgrade" : "manual-install"; params.receiptId = state_.receipt_id; params.previewId = state_.preview->preview_id; params.scope = protocol::ReceiptScope{state_.scope};
    call(rpc_, params, [this, generation = ++generation_](Reply<protocol::TransactionPlan> reply) {
        if (generation != generation_) { return; }
        state_.loading = false;
        if (reply) { state_.plan = *reply; } else { state_.error = error_text(reply.error().code); }
        changed();
    });
    changed();
}
void GitWizard::install() {
    if (state_.step != GitStep::Scope || !state_.preview || !state_.plan || !state_.acknowledged || state_.scope.empty() || state_.loading ||
        !state_.plan->blockers.empty() || !state_.plan->protected_blockers.empty() || !state_.plan->conflicts.empty() || state_.plan->approval_token.empty()) { return; }
    state_.loading = true;
    const auto completed = [this, generation = generation_](Reply<protocol::JobIdResult> reply) {
        if (generation != generation_) { return; }
        state_.loading = false;
        if (reply) { state_.job_id = reply->jobId; state_.step = GitStep::Progress; }
        else {
            state_.error = error_text(reply.error().code);
            if (reply.error().code == protocol::kStalePlan) { state_.error.reset(); scope(state_.scope); state_.error = Text{"confirm.plan_changed", {}}; }
        }
        changed();
    };
    if (state_.receipt_id) { call(rpc_, protocol::ManualUpgradeParams{{state_.plan->plan_id, state_.plan->approval_token}}, completed); }
    else { call(rpc_, protocol::ManualInstallParams{{state_.plan->plan_id, state_.plan->approval_token}}, completed); }
    changed();
}
void GitWizard::result(std::optional<protocol::ManualReceipt> receipt) {
    if (state_.step != GitStep::Progress) { return; }
    state_.result = std::move(receipt); state_.failed = !state_.result.has_value(); changed();
}
Auth::Auth(CredentialChannel& channel, Timer& timer) : channel_(channel), timer_(timer), password_(4096, '\0') {}
Auth::~Auth() {
    wipe();
    if (expiry_) { timer_.cancel(*expiry_); }
    if (binding_ && state_.active) { try { channel_.cancel(*binding_); } catch (...) {} }
}
void Auth::wipe() noexcept { ::explicit_bzero(password_.data(), password_.size()); password_length_ = 0; }
void Auth::bind(AuthBinding binding) {
    wipe();
    if (binding_ && state_.active) { try { channel_.cancel(*binding_); } catch (...) {} }
    state_ = {}; binding_.reset();
    if (expiry_) { timer_.cancel(*expiry_); expiry_.reset(); }
    if (binding.operation_id.empty() || binding.operation_id.size() > 128 || binding.challenge_id.empty() || binding.challenge_id.size() > 128 ||
        binding.accounts.empty() || binding.accounts.size() > 32 || binding.expires <= std::chrono::steady_clock::now() ||
        binding.expires > std::chrono::steady_clock::now() + std::chrono::minutes(10) || binding.app.size() > 4096 || !protocol::valid_utf8(binding.app)) {
        state_.message = Text{"auth.unavailable_title", {}}; changed(); return;
    }
    for (const auto& account_value : binding.accounts) {
        if (account_value.empty() || account_value.size() > 256 || !protocol::valid_utf8(account_value) || account_value.find('\0') != std::string::npos) {
            state_.message = Text{"auth.unavailable_title", {}}; changed(); return;
        }
    }
    switch (binding.action) {
    case AuthAction::Install: state_.operation = {"auth.install", {{"app", binding.app}}}; break;
    case AuthAction::Remove: state_.operation = {"auth.remove", {{"app", binding.app}}}; break;
    case AuthAction::Update: state_.operation = {"auth.update", {}}; break;
    case AuthAction::Setup: state_.operation = {"auth.setup", {}}; break;
    case AuthAction::Git: state_.operation = {"auth.git", {}}; break;
    default: state_.message = Text{"auth.unavailable_title", {}}; changed(); return;
    }
    state_.account = binding.accounts.front();
    state_.password_ui = !binding.native_agent && binding.trusted_local_fallback;
    state_.active = binding.native_agent || state_.password_ui;
    if (binding.native_agent) { state_.message = Text{"auth.native_wait", {}}; }
    else if (!binding.trusted_local_fallback) { state_.message = Text{"auth.unavailable_title", {}}; }
    binding_ = std::move(binding);
    if (state_.active) {
        expiry_ = timer_.after(std::max(std::chrono::milliseconds(1), std::chrono::ceil<std::chrono::milliseconds>(binding_->expires - std::chrono::steady_clock::now())),
            [this, weak = std::weak_ptr<int>(lifetime_)] {
                if (weak.expired()) { return; }
                expiry_.reset(); expire();
            });
    }
    changed();
}
void Auth::account(std::string value) {
    wipe();
    if (!binding_ || std::ranges::find(binding_->accounts, value) == binding_->accounts.end()) { throw std::invalid_argument("account not allowed"); }
    state_.account = std::move(value); changed();
}
void Auth::password(std::span<const char> bytes) {
    wipe();
    if (!state_.active || !state_.password_ui || !binding_) { return; }
    if (binding_->expires <= std::chrono::steady_clock::now()) { expire(); return; }
    if (bytes.size() > password_.size() || std::ranges::find(bytes, '\0') != bytes.end()) { throw std::length_error("invalid password length"); }
    std::ranges::copy(bytes, password_.begin()); password_length_ = bytes.size();
}
std::span<const char> Auth::buffer() const { return password_; }
void Auth::caps_lock(bool value) { state_.caps_lock = value; changed(); }
void Auth::show_password(bool value) { state_.show_password = value && state_.password_ui && state_.active; changed(); }
void Auth::submit() {
    if (!binding_ || !state_.active || !state_.password_ui) { wipe(); return; }
    if (binding_->expires <= std::chrono::steady_clock::now()) { expire(); return; }
    if (expiry_) { timer_.cancel(*expiry_); expiry_.reset(); }
    bool accepted = false;
    try { accepted = channel_.submit(*binding_, state_.account, std::span(password_.data(), password_length_)); }
    catch (...) { wipe(); state_.active = false; state_.show_password = false; state_.message = Text{"auth.failed", {}}; changed(); return; }
    wipe(); state_.active = false; state_.show_password = false;
    state_.message = Text{accepted ? "auth.checking" : "auth.failed", {}}; changed();
}
void Auth::cancel() {
    wipe();
    if (expiry_) { timer_.cancel(*expiry_); expiry_.reset(); }
    state_.active = false; state_.show_password = false; state_.message = Text{"auth.canceled", {}};
    if (binding_) { try { channel_.cancel(*binding_); } catch (...) {} }
    changed();
}
void Auth::expire() {
    wipe();
    if (expiry_) { timer_.cancel(*expiry_); expiry_.reset(); }
    state_.active = false; state_.show_password = false; state_.message = Text{"auth.timeout", {}};
    if (binding_) { try { channel_.cancel(*binding_); } catch (...) {} }
    changed();
}

}
