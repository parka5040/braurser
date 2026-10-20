#include <braurser/qt/app.hpp>
#include <braurser/qt/internal.hpp>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QScrollBar>
#include <QShortcut>
#include <QUrlQuery>
#include <algorithm>

namespace braurser::qt {
namespace {
using presenter::Destination;
const std::array<std::pair<std::string_view, Destination>, 8> destinations{{
    {"discover", Destination::Discover}, {"search", Destination::Search}, {"updates", Destination::Updates},
    {"installed", Destination::Installed}, {"settings", Destination::Settings}, {"activity", Destination::Activity},
    {"details", Destination::Details}, {"git", Destination::Git}
}};
std::string page_name(Destination value) {
    for (const auto& [name, destination] : destinations) { if (destination == value) { return std::string(name); } }
    return "discover";
}
}

Window::Window(Models& models, presenter::Copy& copy, Platform& platform)
    : models_(models), copy_(copy), platform_(platform), scroll_(new QScrollArea(this)) {
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setFocusPolicy(Qt::NoFocus);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* header = new QWidget(this);
    header->setObjectName("page-header");
    auto* header_box = new QHBoxLayout(header);
    header_box->setContentsMargins(12, 12, 12, 12); header_box->setSpacing(8);
    const auto header_action = [this, header, header_box](std::string_view key, std::string_view icon, std::function<void()> action) {
        auto* result = new QPushButton(header);
        result->setProperty("brRole", "iconButton");
        result->setIcon(kit::symbolic_icon(icon)); result->setIconSize(QSize(20, 20));
        result->setFixedSize(44, 44);
        result->setAccessibleName(kit::safe_text(text(key))); result->setToolTip(kit::safe_text(text(key)));
        connect(result, &QPushButton::clicked, result, std::move(action));
        header_box->addWidget(result);
        return result;
    };
    back_ = header_action("nav.back", "braurser-back-symbolic", [this] { models_.shell.back(); });
    heading_ = kit::detail::label({}, header);
    heading_->setProperty("brRole", "pageTitle");
    heading_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header_box->addWidget(heading_, 1);
    check_again_ = new kit::detail::ActionButton({text("action.check_again"), [this] { models_.updates.check(); }}, header);
    check_again_->setMaximumWidth(120);
    header_box->addWidget(check_again_);
    header_action("nav.activity", "braurser-activity-symbolic", [this] { navigate("activity"); });
    setMenuWidget(header);
    std::vector<kit::NavigationItem> navigation;
    for (const auto& [name, destination] : destinations) {
        if (destination == Destination::Details || destination == Destination::Git || destination == Destination::Activity) { continue; }
        const auto title = text("nav." + std::string(name));
        navigation.push_back({std::string(name), title, "braurser-" + std::string(name) + "-symbolic", {}, title});
    }
    auto* body = column();
    auto* body_box = static_cast<QVBoxLayout*>(body->layout());
    body_box->setContentsMargins(0, 0, 0, 0);
    body_box->addWidget(scroll_, 1);
    action_bar_ = column(body);
    body_box->addWidget(action_bar_);
    action_bar_->hide();
    shell_ = new kit::ResponsiveShell(*body, navigation, text("nav.menu"), [this](const std::string& page) { navigate(page); }, this);
    setCentralWidget(shell_);
    themes_.manage(*this);
    const auto observe = [this](auto& model) { subscriptions_.push_back(model.observe([this](const auto&) { schedule(); })); };
    subscriptions_.push_back(presenter::bind_page_retention(models_.shell, models_.details, models_.installed));
    observe(models_.shell); observe(models_.discover); observe(models_.search); observe(models_.details);
    observe(models_.variants); observe(models_.confirm); observe(models_.activity); observe(models_.updates);
    observe(models_.installed); observe(models_.git); observe(models_.settings); observe(models_.first);
    const auto shortcut = [this](const char* key, std::function<void()> action) {
        auto* binding = new QShortcut(QKeySequence(QString::fromLatin1(key)), this);
        connect(binding, &QShortcut::activated, this, std::move(action));
    };
    const auto search = [this] {
        navigate("search");
        QTimer::singleShot(0, this, [this] {
            if (auto* field = findChild<QLineEdit*>("search-query")) { field->setFocus(); field->selectAll(); }
        });
    };
    shortcut("Ctrl+F", search); shortcut("Ctrl+K", search);
    shortcut("Ctrl+1", [this] { navigate("discover"); });
    shortcut("Ctrl+2", search);
    shortcut("Ctrl+3", [this] { navigate("updates"); });
    shortcut("Ctrl+4", [this] { navigate("installed"); });
    shortcut("Ctrl+,", [this] { navigate("settings"); });
    shortcut("Ctrl+J", [this] { navigate("activity"); });
    shortcut("Ctrl+R", [this] { refresh(); });
    shortcut("Alt+Left", [this] { models_.shell.back(); });
    shortcut("Escape", [this] { if (models_.shell.state().location.destination == Destination::Activity) { models_.shell.back(); } });
    shortcut("F1", [this] { if (models_.shell.state().location.destination == Destination::Details) { wiki_search(); } });
    resize(1024, 768);
    render();
}

std::string Window::text(std::string_view key, const presenter::CopyArgs& args) const { return copy_.text(key, args); }
void Window::schedule() {
    if (scheduled_) { return; }
    scheduled_ = true;
    QMetaObject::invokeMethod(this, [this] { scheduled_ = false; render(); }, Qt::QueuedConnection);
}

void Window::navigate(std::string_view page) {
    const auto found = std::ranges::find_if(destinations, [page](const auto& value) { return value.first == page; });
    if (found == destinations.end()) { return; }
    models_.shell.navigate({found->second, {}, 0});
    refresh();
}

void Window::refresh() {
    switch (models_.shell.state().location.destination) {
    case Destination::Search: models_.search.facets(); models_.search.submit(); break;
    case Destination::Updates: models_.updates.check(); break;
    case Destination::Installed: models_.installed.refresh(); break;
    case Destination::Settings: models_.settings.refresh(); break;
    case Destination::Activity: models_.activity.refresh(); break;
    case Destination::Details: models_.details.load_wiki(true); break;
    default: break;
    }
    models_.shell.refresh();
    models_.first.refresh();
}

void Window::render() {
    const auto destination = models_.shell.state().location.destination;
    const auto& location_id = models_.shell.state().location.id;
    const bool same_page = destination == rendered_ && location_id == rendered_id_;
    scroll_positions_[page_name(rendered_) + ":" + rendered_id_] = scroll_->verticalScrollBar()->value();
    const int position = scroll_positions_[page_name(destination) + ":" + location_id];
    std::map<QString, QString> drafts;
    if (same_page) {
        for (const auto* name : {"git-url", "git-script", "discover-query"}) {
            if (auto* field = findChild<QLineEdit*>(name)) { drafts.emplace(field->objectName(), field->text()); }
        }
    }
    QString focused;
    int cursor = 0;
    if (same_page) {
        if (auto* field = qobject_cast<QLineEdit*>(QApplication::focusWidget()); field && isAncestorOf(field)) {
            focused = field->objectName(); cursor = field->cursorPosition();
        }
    }
    delete scroll_->takeWidget();
    auto* content = column();
    auto& box = *static_cast<QVBoxLayout*>(content->layout());
    const auto page = page_name(destination);
    const auto title = destination == Destination::Details ? text("action.details") :
        destination == Destination::Git ? text("discover.git") : text("nav." + page);
    setWindowTitle(kit::safe_text(text("window.title", {{"page", title}})));
    heading_->setText(kit::safe_text(title));
    back_->setEnabled(!models_.shell.state().history.empty());
    check_again_->setVisible(destination == Destination::Updates);
    check_again_->setEnabled(!models_.updates.state().loading);
    shell_->select(page);
    if (!models_.shell.state().connected) {
        const std::array<kit::Action, 1> actions{{{text("error.reconnect"), [this] { refresh(); }}}};
        box.addWidget(new kit::Banner(kit::Severity::Warning, text("error.core_title"), text("error.core_body"), actions));
    } else if (models_.shell.state().connectivity == "offline") {
        box.addWidget(new kit::Banner(kit::Severity::Info, text("offline.title"), text("offline.local_ready")));
    }
    if (models_.shell.state().connected && !models_.shell.state().community_available) { label(box, text("error.aur_title")); }
    if (models_.shell.state().restart.level.value == "reboot") {
        const std::array<kit::Action, 2> actions{{{text("restart.later"), [this] { models_.shell.acknowledge_restart(); }}, {text("restart.now"), [this] { restart(); }}}};
        box.addWidget(new kit::Banner(kit::Severity::Warning, text("restart.title"), text("restart.body"), actions));
    }
    error(box, models_.confirm.state().error);
    if (models_.confirm.state().error && models_.confirm.state().error->key == "confirm.plan_changed" && models_.confirm.state().plan) { button(box, text("action.review"), [this] { show_confirm(); }); }
    if (plan_pending_ && models_.confirm.state().plan && !models_.confirm.state().submitting && models_.confirm.state().pending_build_files == 0) { plan_pending_ = false; show_confirm(); }
    auto* action_box = static_cast<QVBoxLayout*>(action_bar_->layout());
    while (auto* item = action_box->takeAt(0)) { delete item->widget(); delete item; }
    action_bar_->hide();
    if (destination == Destination::Details && models_.details.state().package) {
        button(*action_box, models_.details.state().primary.render(copy_), [this] { details_action(); }, true, true);
        if (models_.details.state().can_remove && models_.details.state().can_open) {
            button(*action_box, text("action.remove"), [this] { request_plan({"remove", models_.details.state().id}); });
        }
        action_bar_->show();
    }
    switch (destination) {
    case Destination::Discover: discover_page(box); break;
    case Destination::Search: search_page(box); break;
    case Destination::Updates: updates_page(box); break;
    case Destination::Installed: installed_page(box); break;
    case Destination::Settings: settings_page(box); break;
    case Destination::Activity: activity_page(box); break;
    case Destination::Details: details_page(box); break;
    case Destination::Git: git_page(box); break;
    }
    box.addStretch();
    scroll_->setWidget(content);
    rendered_ = destination;
    rendered_id_ = location_id;
    scroll_->verticalScrollBar()->setValue(position);
    for (const auto& [name, value] : drafts) {
        if (auto* field = content->findChild<QLineEdit*>(name)) { field->setText(value); }
    }
    if (!focused.isEmpty()) {
        if (auto* field = content->findChild<QLineEdit*>(focused)) { field->setFocus(); field->setCursorPosition(cursor); }
    }
    if (models_.settings.state().values) {
        const auto& theme = models_.settings.state().values->theme.value;
        themes_.set_theme(theme == "dark" ? kit::Theme::Dark : theme == "light" ? kit::Theme::Light : kit::Theme::System);
    } else {
        const auto& theme = models_.shell.state().theme;
        themes_.set_theme(theme == "dark" ? kit::Theme::Dark : theme == "light" ? kit::Theme::Light : kit::Theme::System);
    }
}

void Window::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    const auto viewport = width() < 480 ? presenter::Viewport::Narrow : width() < 840 ? presenter::Viewport::Compact :
        width() < 1080 ? presenter::Viewport::Wide : presenter::Viewport::Large;
    if (viewport != models_.shell.state().viewport) { models_.shell.width(static_cast<unsigned>(width())); }
}

void Window::closeEvent(QCloseEvent* event) {
    if (!models_.shell.state().active_job) { event->accept(); return; }
    QMessageBox dialog(QMessageBox::Question, kit::safe_text(text("activity.close_title")), kit::safe_text(text("activity.close_body")), QMessageBox::NoButton, this);
    dialog.setTextFormat(Qt::PlainText);
    auto* stay = dialog.addButton(kit::safe_text(text("activity.keep_window")), QMessageBox::RejectRole);
    auto* background = dialog.addButton(kit::safe_text(text("activity.keep_background")), QMessageBox::AcceptRole);
    dialog.setDefaultButton(stay);
    dialog.exec();
    if (dialog.clickedButton() == background) { event->accept(); } else { event->ignore(); }
}

void Window::error(QVBoxLayout& box, const std::optional<presenter::Text>& value) {
    if (value) { box.addWidget(new kit::Banner(kit::Severity::Warning, value->render(copy_), {})); }
}

kit::Source Window::source(const protocol::PackageRef& ref) const {
    const auto key = std::string(presenter::source_key(ref));
    return {key == "official" ? kit::SourceKind::Official : key == "community" ? kit::SourceKind::Community : key == "git" ? kit::SourceKind::Git : kit::SourceKind::Foreign,
        text("source." + key), text("source." + key + "_help", {{"repository", ref.repo.value_or(text("state.unavailable"))}})};
}

void Window::package(QVBoxLayout& box, const protocol::PackageSummary& value) {
    const auto badge = source(value.package);
    auto* card = new kit::AppCard(value.package.name, value.description, badge,
        text("a11y.app_card", {{"app", value.package.name}, {"summary", value.description}, {"source", badge.label}}), "braurser-package-symbolic");
    connect(card, &QPushButton::clicked, this, [this, id = value.package.name] { open_details(id); });
    box.addWidget(card);
}

void Window::open_details(std::string id) {
    models_.shell.navigate({Destination::Details, id, 0});
    models_.details.load(std::move(id));
    for (const auto& item : models_.search.state().results) { models_.details.summary(item); }
    for (const auto& item : models_.installed.state().packages) { models_.details.summary(item); }
    models_.details.load_wiki(false);
}

void Window::unavailable() {
    auto* dialog = new QMessageBox(QMessageBox::Information, kit::safe_text(text("error.unsupported_title")),
                                  kit::safe_text(text("error.unsupported_body")), QMessageBox::NoButton, this);
    dialog->setTextFormat(Qt::PlainText);
    dialog->addButton(kit::safe_text(text("action.close")), QMessageBox::RejectRole);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void Window::launch(std::string id, std::string app) {
    if (platform_.open_desktop(id)) { return; }
    auto* dialog = new QMessageBox(QMessageBox::Warning, kit::safe_text(text("action.open")),
        kit::safe_text(text("installed.open_failed", {{"app", app}})), QMessageBox::NoButton, this);
    dialog->setTextFormat(Qt::PlainText);
    dialog->addButton(kit::safe_text(text("action.close")), QMessageBox::RejectRole);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void Window::wiki_search() {
    QUrl url("https://wiki.archlinux.org/index.php");
    QUrlQuery query;
    query.addQueryItem("search", kit::safe_text(models_.details.state().id));
    url.setQuery(query);
    platform_.open_url(url);
}

void Window::restart() {
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(kit::safe_text(text("restart.confirm_title")));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* box = new QVBoxLayout(dialog);
    label(*box, text("restart.confirm_body"));
    button(*box, text("action.cancel"), [dialog] { dialog->reject(); });
    button(*box, text("restart.now"), [this, dialog] {
        dialog->accept();
        platform_.reboot([self = QPointer<Window>(this)](bool success) {
            if (!self || success) { return; }
            auto* failure = new QMessageBox(QMessageBox::Warning, kit::safe_text(self->text("restart.title")),
                kit::safe_text(self->text("restart.failed")), QMessageBox::NoButton, self);
            failure->setTextFormat(Qt::PlainText);
            failure->addButton(kit::safe_text(self->text("action.close")), QMessageBox::RejectRole);
            failure->setAttribute(Qt::WA_DeleteOnClose);
            failure->open();
        });
    });
    dialog->open();
}

}
