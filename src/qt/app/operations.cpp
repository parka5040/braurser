#include <braurser/qt/app.hpp>
#include <braurser/qt/internal.hpp>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QUrl>
#include <algorithm>

namespace braurser::qt {

void Window::updates_page(QVBoxLayout& box) {
    const auto& state = models_.updates.state();
    error(box, state.error);
    if (state.loading) { label(box, text("updates.checking")); }
    if (!state.report) { label(box, text("updates.never_checked")); return; }
    const auto& report = *state.report;
    label(box, text("updates.last_checked", {{"relative_time", copy_.date(report.checked_at)}}), "secondary");
    const auto count = report.official.size() + report.community.size() + report.manual.size();
    label(box, text("updates.count", {{"count", static_cast<std::int64_t>(count)}}));
    if (count == 0) {
        box.addWidget(new kit::EmptyState("braurser-success-symbolic", text("updates.current_title"), text("updates.current_body")));
    } else {
        auto* update = button(box, text("updates.all"), [this] { models_.updates.plan_system(); }, models_.updates.can_update() && state.options.has_value(), true);
        update->setObjectName("update-system"); box.setAlignment(update, Qt::AlignLeft);
    }
    for (std::size_t index = 0; index < report.news.size(); ++index) {
        const auto& news = report.news[index];
        auto* notice = new kit::Banner(kit::Severity::Warning, text("updates.news_title"), news.title);
        auto& content = *static_cast<QVBoxLayout*>(notice->layout());
        label(content, copy_.date(news.published), "secondary");
        auto* read = button(content, text("updates.news_read"), [this, url = news.url] { platform_.open_url(QUrl(kit::safe_text(url))); }, kit::allowed_link(news.url));
        content.setAlignment(read, Qt::AlignLeft);
        if (news.requires_manual_intervention) {
            check(content, text("updates.news_ack"), news.acknowledged, [this, index](bool value) { models_.updates.acknowledge_news(index, value); });
        }
        box.addWidget(notice);
    }
    if (!state.news_available) {
        auto* notice = new kit::Banner(kit::Severity::Warning, text("updates.news_title"), text("updates.news_unavailable"));
        auto& content = *static_cast<QVBoxLayout*>(notice->layout());
        check(content, text("updates.news_unavailable_ack"), state.missing_news_acknowledged,
              [this](bool value) { models_.updates.acknowledge_missing_news(value); });
        box.addWidget(notice);
    }
    for (const auto& group : state.groups) {
        if (group.items.empty()) { continue; }
        const auto& key = group.title.key;
        const auto* icon = key == "updates.restart" ? "braurser-restart-symbolic" :
            key == "updates.logout" ? "braurser-logout-symbolic" : key == "updates.apps" ? "braurser-app-restart-symbolic" :
            key == "updates.none" ? "braurser-success-symbolic" : "braurser-info-symbolic";
        auto* heading = new QHBoxLayout;
        heading->setSpacing(8);
        heading->addWidget(new kit::detail::IconLabel(icon, 24));
        auto* title = kit::detail::label(text("updates.group_count", {{"group", group.title.render(copy_)}, {"count", static_cast<std::int64_t>(group.items.size())}}));
        title->setProperty("brRole", "sectionTitle"); title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        heading->addWidget(title, 1);
        box.addLayout(heading);
        auto* content = new QFrame;
        content->setProperty("brRole", "card");
        auto& rows = *new QVBoxLayout(content);
        rows.setContentsMargins(16, 16, 16, 16); rows.setSpacing(12);
        for (const auto& index : group.items) {
            const std::array lists{&state.report->official, &state.report->community, &state.report->manual};
            const auto& item = lists[index.source]->at(index.item);
            if (rows.count() != 0) {
                auto* separator = new QFrame;
                separator->setFrameShape(QFrame::HLine); separator->setProperty("brRole", "separator");
                rows.addWidget(separator);
            }
            auto* card = new QWidget;
            card->setObjectName(kit::safe_text("update-row-" + item.package.name));
            auto* horizontal = new QHBoxLayout(card);
            horizontal->setContentsMargins(0, 0, 0, 0); horizontal->setSpacing(12);
            auto* details = column();
            auto& row = *static_cast<QVBoxLayout*>(details->layout());
            row.setContentsMargins(0, 0, 0, 0); row.setSpacing(4);
            horizontal->addWidget(details, 1);
            label(row, item.package.name, "rowTitle");
            label(row, text("updates.version_change", {{"old_version", item.from_version}, {"new_version", item.to_version}}), "secondary");
            box.parentWidget()->setAccessibleDescription(kit::safe_text(text("updates.count", {{"count", static_cast<std::int64_t>(count)}})));
            const auto badge = item.package.source.value == "community" ?
                kit::Source{kit::SourceKind::Community, text("source.community"), text("source.community_help")} : source(item.package);
            row.addWidget(new kit::SourceBadge(badge));
            if (item.restart.detail) { label(row, *item.restart.detail, "secondary"); }
            if (item.source.value == "community" || item.source.value == "git") {
                const auto blocker = models_.updates.item_blocker();
                if (blocker) { label(row, blocker->render(copy_), "secondary"); }
                const auto kind = item.source.value == "community" ? "community" : "manual";
                bool available = false;
                if (state.options) {
                    const auto& ids = item.source.value == "community" ? state.options->communityIndividualIds : state.options->manualIndividualIds;
                    available = std::ranges::find(ids, item.package.name) != ids.end();
                }
                auto* update = new kit::detail::ActionButton({text("updates.item_update"),
                    [this, kind, id = item.package.name] { if (std::string_view(kind) == "manual") { models_.git.upgrade(id); navigate("git"); } else { models_.updates.plan_item(kind, id); } },
                    models_.updates.can_update() && !blocker && available});
                update->setObjectName("update-item");
                horizontal->addWidget(update, 0, Qt::AlignVCenter);
            }
            rows.addWidget(card);
        }
        box.addWidget(content);
    }
    if (state.plan) { button(box, text("action.review"), [this] { const auto& value = models_.updates.state().plan; if (value && value->transactions.size() == 1) { models_.confirm.plan(value->transactions.front()); plan_pending_ = true; } }); }
}

void Window::request_plan(protocol::PlanParams params) {
    plan_pending_ = true; models_.confirm.request(std::move(params));
}
void Window::setup() {
    auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(kit::safe_text(text("first.setup")));
    auto* box = new QVBoxLayout(dialog);
    for (const auto* helper : {"yay", "paru"}) {
        button(*box, text(std::string("first.") + helper), [this, dialog, helper] {
            models_.first.helper(helper); plan_pending_ = true; models_.first.review(models_.confirm); dialog->accept();
        }, models_.first.state().setup_supported);
    }
    dialog->open();
}
void Window::show_confirm() {
    if (confirm_dialog_) { confirm_dialog_->raise(); return; }
    const auto& state = models_.confirm.state();
    if (!state.plan) { unavailable(); return; }
    auto* dialog = new QDialog(this);
    confirm_dialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(kit::safe_text(text("confirm.exact")));
    auto* root = new QVBoxLayout(dialog);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    auto* content = column();
    auto& box = *static_cast<QVBoxLayout*>(content->layout());
    scroll->setWidget(content); root->addWidget(scroll);
    error(box, state.error);
    const auto& plan = *state.plan;
    if (plan.kind.value == "remove") { label(box, text("confirm.remove_body")); label(box, text("confirm.files_unknown")); }
    const auto changes = [&](std::string_view key, const std::vector<protocol::PackageRef>& packages) {
        if (packages.empty()) { return; }
        label(box, text(key), "sectionTitle");
        for (const auto& item : packages) { label(box, item.name); box.addWidget(new kit::SourceBadge(source(item))); }
    };
    changes("confirm.also_installs", plan.installs); changes("confirm.also_updates", plan.upgrades); changes("confirm.also_removes", plan.removals);
    for (const auto& replacement : plan.replacements) { label(box, text("confirm.replacement", {{"old_package", replacement.old.name}, {"new_package", replacement.new_.name}})); }
    for (const auto& conflict : plan.conflicts) { label(box, conflict); }
    for (const auto& item : plan.protected_blockers) { label(box, text("confirm.protected", {{"package", item.name}})); }
    for (const auto& blocker : plan.blockers) { static_cast<void>(blocker); label(box, text("error.unsupported_title")); }
    label(box, text("confirm.download", {{"size", copy_.size(plan.download_bytes)}}));
    label(box, plan.install_delta_bytes ? text("confirm.storage_add", {{"size", copy_.size(plan.install_delta_bytes)}}) : text("confirm.storage_unknown"));
    if (state.busy) { label(box, text("confirm.queue_note")); }


    button(*root, text("action.cancel"), [dialog] { dialog->reject(); });
    auto* submit = button(*root, models_.confirm.action().render(copy_), [this, dialog] { models_.confirm.submit(); dialog->accept(); navigate("activity"); }, models_.confirm.can_confirm(), true);
    if (plan.requires_build_review) {
        if (state.pending_build_files == 0 && !state.build_files.empty() && state.build_files.size() == (plan.build_pins.empty() ? plan.build_review_refs.size() : plan.build_pins.size())) {
            for (const auto& build : state.build_files) {
                label(box, build.packageBase);
                for (const auto& file : build.files) { if (file.kind == "text") { label(box, file.name); auto* code = new QPlainTextEdit(kit::safe_code(file.text)); code->setReadOnly(true); box.addWidget(code); } }
                bool heading = false;
                for (const auto& file : build.files) {
                    if (file.kind != "binary") { continue; }
                    if (!heading) { label(box, text("detail.build_binary")); heading = true; }
                    label(box, text("detail.build_binary_file", {{"file", file.name}, {"size", copy_.size(file.size)}, {"sha256", file.sha256.substr(0, 12)}}));
                }
            }
            check(box, text("confirm.build_ack"), state.build_acknowledged, [this, submit](bool value) { models_.confirm.acknowledge_build(value); submit->setEnabled(models_.confirm.can_confirm()); });
        } else { label(box, text("detail.build_unavailable")); }
    }
    if (state.requires_dependents_ack) { check(box, text("confirm.remove_dependents_ack"), state.dependents_acknowledged, [this, submit](bool value) { models_.confirm.acknowledge_dependents(value); submit->setEnabled(models_.confirm.can_confirm()); }); }
    dialog->resize(std::min(width() - 24, 560), std::min(height() - 48, 640));
    dialog->open();
}

void Window::activity_page(QVBoxLayout& box) {
    const auto& state = models_.activity.state();
    error(box, state.error);
    error(box, state.auth_message);
    if (!state.job_id && state.queue.empty() && state.recent.empty()) {
        box.addWidget(new kit::EmptyState("braurser-activity-symbolic", text("activity.empty_title"), text("activity.empty_body")));
        return;
    }
    if (state.job_id) {
        label(box, *state.job_id);
        kit::ProgressState progress{state.phase.render(copy_), state.phase.render(copy_), state.fraction, {}, state.cancel_label.render(copy_), state.can_cancel};
        box.addWidget(new kit::ProgressPanel(progress, [this] { models_.activity.cancel(); }));
        if (state.progress && state.progress->current && state.progress->total && state.progress->unit.value == "bytes") {
            label(box, text("activity.download_amount", {{"downloaded", copy_.size(state.progress->current)}, {"total", copy_.size(state.progress->total)}}));
        }
        if (state.job && state.job->state.value == "failed") { label(box, text("activity.changes_unknown")); button(box, text("activity.refresh_state"), [this] { models_.installed.refresh(); }); }
    }
    auto* logs = column();
    auto& log_box = *static_cast<QVBoxLayout*>(logs->layout());
    auto* log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    log->setAccessibleName(kit::safe_text(text("activity.log")));
    QStringList lines;
    for (const auto& line : state.log) { lines.push_back(kit::safe_text(line)); }
    log->setPlainText(lines.join('\n'));
    log_box.addWidget(log);
    label(log_box, text("activity.log_warning"));
    button(log_box, text("activity.copy_log"), [log] { QApplication::clipboard()->setText(log->toPlainText()); });
    disclosure(box, *logs, text("activity.log"), copy_, models_.settings.state().values && models_.settings.state().values->show_log);
    if (state.question) {
        label(box, text("activity.needs_attention"), "sectionTitle");
        std::visit([&](const auto& question) {
            using T = std::decay_t<decltype(question)>;
            if constexpr (std::is_same_v<T, protocol::GitRedirectQuestion>) {
                label(box, text("git.redirect_body", {{"original_host", question.from_host}, {"destination_host", question.to_host}}));
                button(box, text("action.cancel"), [this] { models_.activity.answer("cancel"); });
                button(box, text("action.continue"), [this] { models_.activity.answer("continue"); });
            } else if constexpr (std::is_same_v<T, protocol::ProviderQuestion>) {
                for (const auto& option : question.options) { button(box, option.name, [this, id = option.name] { models_.activity.answer(id); }); }
            } else if constexpr (std::is_same_v<T, protocol::VariantChoices>) {
                for (const auto& option : question.options) { button(box, option.package.name, [this, id = option.package.name] { models_.activity.answer(id); }); }
            } else if constexpr (std::is_same_v<T, protocol::NewsAckQuestion>) {
                for (const auto& item : question.items) {
                    label(box, item.title);
                    button(box, text("updates.news_read"), [this, url = item.url] { platform_.open_url(QUrl(kit::safe_text(url))); });
                }
                label(box, text("updates.news_block"));
            } else { label(box, text("error.unsupported_title")); }
        }, state.question->choice.value);
    }
    if (!state.queue.empty()) {
        label(box, text("activity.waiting", {{"count", static_cast<std::int64_t>(state.queue.size())}}), "sectionTitle");
        for (const auto& item : state.queue) {
            label(box, item.jobId);
            if (item.state.value == "queued") { button(box, text("activity.remove_queue"), [this, id = item.jobId] { models_.activity.dequeue(id); }); }
            button(box, text("action.details"), [this, id = item.jobId] { models_.activity.attach(id); });
            if (item.state.value == "needs-attention" || item.state.value == "waiting-auth") { label(box, text("activity.auth_wait")); }
        }
    }
    if (!state.recent.empty()) {
        label(box, text("activity.recent"), "sectionTitle");
        for (const auto& item : state.recent) { button(box, item.jobId, [this, id = item.jobId] { models_.activity.attach(id); }); }
    }
}

}
