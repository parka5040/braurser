#include <braurser/qt/app.hpp>
#include <QButtonGroup>
#include <QDialog>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QStandardItemModel>
#include <algorithm>

namespace braurser::qt {

void Window::installed_page(QVBoxLayout& box) {
    const auto& state = models_.installed.state();
    button(box, text("discover.git"), [this] { navigate("git"); });
    combo(box, text("installed.view"), {{"0", text("installed.apps")}, {"1", text("installed.components")},
        {"2", text("installed.git")}, {"3", text("installed.outside")}}, std::to_string(static_cast<int>(state.view)),
        [this](std::string value) { models_.installed.view(static_cast<presenter::InstalledView>(std::stoi(value))); });
    auto* search = entry(box, text("search.installed_placeholder"), state.query, "installed-query");
    connect(search, &QLineEdit::textEdited, this, [this](const QString& value) { models_.installed.query(value.toStdString()); });
    error(box, state.error);
    if (state.loading) { label(box, text("state.loading")); }
    bool empty = true;
    if (state.view == presenter::InstalledView::Apps || state.view == presenter::InstalledView::Components) {
        for (const auto& item : state.packages) { package(box, item); }
        empty = state.packages.empty();
    } else if (state.view == presenter::InstalledView::Git) {
        for (const auto& receipt : state.receipts) {
            if (!state.query.empty() && !kit::safe_text(receipt.name).contains(kit::safe_text(state.query), Qt::CaseInsensitive)) { continue; }
            empty = false;
            label(box, receipt.name, "sectionTitle");
            label(box, text(receipt.scope.value == "user" ? "git.user" : "git.system"));
            label(box, receipt.commit);
            if (!receipt.launcher_ids.empty()) {
                button(box, text("action.open"), [this, id = receipt.launcher_ids.front(), name = receipt.name] { launch(id, name); });
            }
            if (receipt.removable) { button(box, text("action.remove"), [this, id = receipt.receipt_id] { protocol::PlanParams params; params.kind = "manual-remove"; params.receiptId = id; request_plan(params); }); }
            else { label(box, text("confirm.removal_unavailable")); label(box, text("confirm.git_untracked_removal")); }
        }
    } else {
        label(box, text("installed.outside_help"));
        for (const auto& item : state.outside) {
            if (!state.query.empty() && !kit::safe_text(item.name).contains(kit::safe_text(state.query), Qt::CaseInsensitive)) { continue; }
            empty = false;
            label(box, item.name, "sectionTitle");
            label(box, text("installed.outside_unmanaged"));
            label(box, text("installed.outside_notice"));
            button(box, text("action.open"), [this, id = item.desktop_id, name = item.name] { launch(id, name); });
        }
    }
    if (empty && !state.loading && !state.error) {
        const auto value = presenter::empty_installed(state.view);
        box.addWidget(new kit::EmptyState("braurser-installed-symbolic", value.title.render(copy_), value.body.render(copy_)));
    }
}

void Window::settings_page(QVBoxLayout& box) {
    const auto& state = models_.settings.state();
    error(box, state.error);
    if (!state.values && !state.legacy) { label(box, text("state.loading")); return; }
    label(box, text("settings.community"), "sectionTitle");
    auto* helpers = combo(box, text("settings.helper"), {{"auto", text("settings.automatic")}, {"paru", text("settings.paru")}, {"yay", text("settings.yay")}},
        state.values ? state.values->helper.value : "auto", [this](std::string value) {
            protocol::SettingsPatch patch; patch.helper = protocol::HelperPreference{std::move(value)}; models_.settings.apply(patch);
        });
    for (int index = 0; index < helpers->count(); ++index) {
        const auto reason = models_.settings.helper_disabled(helpers->itemData(index).toString().toStdString());
        if (reason) {
            if (auto* items = qobject_cast<QStandardItemModel*>(helpers->model())) {
                items->item(index)->setEnabled(false);
                items->item(index)->setToolTip(kit::safe_text(reason->render(copy_)));
            }
        }
    }
    label(box, text("settings.using_helper", {{"helper", state.effective_helper.empty() ? text("state.unavailable") : state.effective_helper}}));
    if (state.available_helpers.empty()) { button(box, text("first.setup"), [this] { setup(); }); }
    label(box, text("settings.updates"), "sectionTitle");
    combo(box, text("settings.frequency"), {{"never", text("settings.never")}, {"daily", text("settings.daily")}, {"weekly", text("settings.weekly")}},
        state.values ? state.values->check_frequency.value : state.legacy->refreshPolicy.value, [this](std::string value) {
            protocol::SettingsPatch patch; patch.check_frequency = protocol::CheckFrequency{std::move(value)}; models_.settings.apply(patch);
        });
    check(box, text("settings.notifications"), state.values ? state.values->notifications : state.legacy->notifications, [this](bool value) {
        protocol::SettingsPatch patch; patch.notifications = value; models_.settings.apply(patch);
    });
    auto* autostart = check(box, text("settings.autostart"), state.values && state.values->autostart, [this](bool value) {
        protocol::SettingsPatch patch; patch.autostart = value; models_.settings.apply(patch);
    });
    autostart->setEnabled(state.values.has_value());
    label(box, text("settings.autostart_help"));
    label(box, text("settings.storage"), "sectionTitle");
    label(box, text("settings.cache")); label(box, copy_.size(state.cache_bytes));
    button(box, text("settings.clear_cache"), [this] {
        models_.settings.request_clear_cache();
        auto* dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(kit::safe_text(text("settings.cache_title", {{"size", copy_.size(models_.settings.state().cache_bytes)}})));
        auto* content = new QVBoxLayout(dialog);
        label(*content, text("settings.cache_body")); label(*content, text("settings.cache_offline")); label(*content, text("settings.cache_busy"));
        button(*content, text("action.cancel"), [this, dialog] { models_.settings.clear_cache(false); dialog->reject(); });
        button(*content, text("settings.cache_confirm"), [this, dialog] { models_.settings.clear_cache(true); dialog->accept(); }, models_.settings.state().cache_confirmation);
        dialog->open();
    });
    label(box, text("settings.advanced"), "sectionTitle");
    auto* review = check(box, text("settings.review_builds"), state.values && state.values->review_builds, [this](bool value) {
        protocol::SettingsPatch patch; patch.review_builds = value; models_.settings.apply(patch);
    });
    review->setEnabled(state.values.has_value());
    auto* logs = check(box, text("settings.logs"), state.values && state.values->show_log, [this](bool value) {
        protocol::SettingsPatch patch; patch.show_log = value; models_.settings.apply(patch);
    });
    logs->setEnabled(state.values.has_value());
    if (!state.values) { label(box, text("state.unavailable")); }
    label(box, text("settings.appearance"), "sectionTitle");
    combo(box, text("settings.theme"), {{"system", text("settings.system")}, {"light", text("settings.light")}, {"dark", text("settings.dark")}},
        state.values ? state.values->theme.value : models_.shell.state().theme, [this](std::string value) {
            protocol::SettingsPatch patch; patch.theme = protocol::Theme{std::move(value)}; models_.settings.apply(patch);
        });
}

void Window::git_page(QVBoxLayout& box) {
    const auto& state = models_.git.state();
    const std::array<const char*, 4> steps{"git.choose", "git.review", "git.scope", "git.install_step"};
    label(box, text("git.step", {{"step", static_cast<std::int64_t>(state.step) + 1}, {"step_name", text(steps.at(static_cast<std::size_t>(state.step)))}}), "sectionTitle");
    error(box, state.error);
    if (state.loading) { label(box, text("git.fetching")); }
    if (state.redirect) {
        const auto& redirect = std::get<protocol::GitRedirectQuestion>(state.redirect->choice.value);
        label(box, text("git.redirect_title"), "sectionTitle");
        label(box, text("git.redirect_body", {{"original_host", redirect.from_host}, {"destination_host", redirect.to_host}}));
        button(box, text("action.cancel"), [this] { models_.git.confirm_redirect(false); });
        button(box, text("action.continue"), [this] { models_.git.confirm_redirect(true); }, !state.loading);
        return;
    }
    if (state.step == presenter::GitStep::Url) {
        label(box, text("git.url_label"));
        auto* url = entry(box, text("git.url_placeholder"), state.url, "git-url");
        label(box, text("git.url_help"));
        label(box, text("git.select_script"));
        auto* script = entry(box, text("git.script"), state.script, "git-script");
        button(box, text("action.cancel"), [this] { models_.shell.back(); });
        button(box, text("action.next"), [this, url, script] { models_.git.inspect(url->text().toStdString(), script->text().toStdString(), "user"); }, !state.loading, true);
    } else if (state.step == presenter::GitStep::Review && state.preview) {
        const auto& preview = *state.preview;
        std::vector<kit::Fact> facts{{text("git.repository"), preview.host + "/" + preview.owner_repo},
            {text("git.commit"), preview.commit}, {text("git.script"), preview.script_path}};
        box.addWidget(new kit::TrustGrid(facts, text("git.risk")));
        auto* code = new QPlainTextEdit;
        code->setReadOnly(true);
        code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        code->setPlainText(kit::safe_code(preview.script_text));
        code->setAccessibleName(kit::safe_text(text("git.script")));
        code->setMinimumHeight(220);
        box.addWidget(code);
        label(box, text("git.external_risk"));
        auto* external = column();
        auto& references = *static_cast<QVBoxLayout*>(external->layout());
        for (const auto& ref : preview.external_refs) { label(references, ref); }
        disclosure(box, *external, text("git.other_files"), copy_);
        check(box, text("git.review_ack"), state.acknowledged, [this](bool value) { models_.git.acknowledge(value); });
        button(box, text("nav.back"), [this] { models_.git.back(); });
        button(box, text("action.next"), [this] { models_.git.next(); }, state.acknowledged, true);
    } else if (state.step == presenter::GitStep::Scope && state.preview) {
        auto* group = new QButtonGroup(box.parentWidget());
        for (const auto* scope : {"user", "system"}) {
            auto* radio = new QRadioButton(kit::safe_text(text(std::string("git.") + scope)));
            radio->setMinimumHeight(44);
            group->addButton(radio);
            const bool available = std::ranges::any_of(state.preview->scope_options, [scope](const auto& option) { return option.value == scope; });
            radio->setEnabled(available);
            radio->setChecked(state.scope == scope);
            connect(radio, &QRadioButton::toggled, this, [this, scope](bool selected) { if (selected) { models_.git.scope(scope); } });
            box.addWidget(radio);
            label(box, text(std::string("git.") + scope + "_body"));
            if (!available) { label(box, text("git.scope_unavailable")); }
        }
        label(box, state.preview->host + "/" + state.preview->owner_repo);
        label(box, state.preview->commit); label(box, state.preview->script_path);
        if (state.scope == "system") { label(box, text("git.system_risk")); }
        if (state.plan) { label(box, text("confirm.exact")); for (const auto& item : state.plan->installs) { label(box, item.name); } label(box, text("confirm.download", {{"size", copy_.size(state.plan->download_bytes)}})); }
        button(box, text("nav.back"), [this] { models_.git.back(); });
        button(box, text(state.scope == "system" ? "git.install_system" : "git.install_user"), [this] { models_.git.install(); }, state.plan.has_value() && state.acknowledged && !state.loading, true);
    } else {
        if (state.failed) {
            label(box, text("git.cleanup_unknown"));
            button(box, text("action.review"), [this] { navigate("activity"); });
            button(box, text("action.retry"), [this] { unavailable(); });
        } else if (state.result) {
            label(box, text("activity.ready", {{"app", state.result->name}}));
            label(box, text(state.result->scope.value == "user" ? "git.installed_user" : "git.installed_system"));
            if (state.result->launcher_ids.empty()) { label(box, text("git.no_launcher")); }
            else { button(box, text("action.open"), [this, id = state.result->launcher_ids.front(), name = state.result->name] { launch(id, name); }); }
        } else { activity_page(box); }
        button(box, text("git.view_installed"), [this] { navigate("installed"); });
    }
}

}
