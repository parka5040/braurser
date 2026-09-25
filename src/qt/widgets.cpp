#include <braurser/qt/kit.hpp>
#include <braurser/qt/internal.hpp>
#include <theme_tokens.hpp>
#include <QApplication>
#include <QButtonGroup>
#include <QFormLayout>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace braurser::qt::kit {
namespace {
using namespace detail;
using namespace braurser::theme;

const char* source_icon(SourceKind kind) {
    switch (kind) {
    case SourceKind::Official: return "braurser-official-symbolic";
    case SourceKind::Community: return "braurser-community-symbolic";
    case SourceKind::Git: return "braurser-git-symbolic";
    case SourceKind::Outside: return "braurser-outside-symbolic";
    case SourceKind::Foreign: return "braurser-package-symbolic";
    }
    throw std::invalid_argument("unknown source kind");
}

class BadgeExplanation final : public QObject {
public:
    explicit BadgeExplanation(QWidget& widget) : QObject(&widget), widget_(widget) { widget.installEventFilter(this); }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::FocusIn) {
            QToolTip::showText(widget_.mapToGlobal(QPoint(0, widget_.height())), widget_.toolTip(), &widget_);
        } else if (event->type() == QEvent::FocusOut) { QToolTip::hideText(); }
        return false;
    }
private:
    QWidget& widget_;
};

class BadgeLabel final : public QLabel {
public:
    BadgeLabel(std::string_view text, QWidget* parent) : QLabel(safe_text(text), parent) {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setAccessibleName(safe_text(text));
        setAccessibleDescription(safe_text(text));
    }
    QSize minimumSizeHint() const override { return {0, sizeHint().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
            fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};

void badge(QFrame& frame, std::string_view icon, std::string_view text, std::string_view explanation) {
    auto* layout = new QHBoxLayout(&frame);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(br_space_2);
    layout->addWidget(new IconLabel(icon, br_badge_icon_size, &frame));
    layout->addWidget(new BadgeLabel(text, &frame), 1);
    name(frame, text, std::string(text) + (explanation.empty() ? "" : ". " + std::string(explanation)));
    if (!explanation.empty()) {
        frame.setToolTip("<qt>" + safe_text(explanation).toHtmlEscaped() + "</qt>");
        new BadgeExplanation(frame);
    }
    frame.setFocusPolicy(explanation.empty() ? Qt::NoFocus : Qt::StrongFocus);
    frame.setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
}
}

SourceBadge::SourceBadge(const Source& source, QWidget* parent) : QFrame(parent) {
    setProperty("brRole", "sourceBadge");
    badge(*this, source_icon(source.kind), source.label, source.explanation);
}

ImpactBadge::ImpactBadge(ImpactKind kind, std::string_view text, std::string_view explanation, QWidget* parent) : QFrame(parent) {
    const char* icon = nullptr;
    switch (kind) {
    case ImpactKind::Reboot: icon = "braurser-restart-symbolic"; break;
    case ImpactKind::Logout: icon = "braurser-logout-symbolic"; break;
    case ImpactKind::RestartApp: icon = "braurser-app-restart-symbolic"; break;
    case ImpactKind::RestartService: icon = "braurser-settings-symbolic"; break;
    case ImpactKind::None: icon = "braurser-success-symbolic"; break;
    case ImpactKind::Unknown: icon = "braurser-info-symbolic"; break;
    default: throw std::invalid_argument("unknown impact kind");
    }
    if (kind != ImpactKind::None && kind != ImpactKind::Unknown) { setProperty("brRole", "warning"); }
    badge(*this, icon, text, explanation);
}

AppCard::AppCard(std::string_view title, std::string_view summary, const Source& source,
                 std::string_view accessible_name, std::string_view icon, QWidget* parent) : QPushButton(parent) {
    setProperty("brRole", "appCard");
    target(*this, accessible_name, source.explanation);
    setMinimumHeight(br_app_row_height_min);
    auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(br_card_padding, br_card_padding, br_card_padding, br_card_padding);
    row->setSpacing(br_space_3);
    row->addWidget(new IconLabel(icon, br_app_icon_size, this), 0, Qt::AlignTop);
    auto* text = new QWidget(this);
    auto* box = column(text);
    auto* heading = label(title, text);
    heading->setProperty("brRole", "sectionTitle");
    box->addWidget(heading);
    auto* description = new SummaryLabel(summary, text);
    box->addWidget(description);
    auto* source_badge = new SourceBadge(source, text);
    source_badge->setFocusPolicy(Qt::NoFocus);
    box->addWidget(source_badge);
    text->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(text, 1);
}

QSize AppCard::sizeHint() const { return layout()->sizeHint().expandedTo(QSize(280, 72)); }
QSize AppCard::minimumSizeHint() const { return {44, 72}; }
int AppCard::heightForWidth(int width) const { return std::max(72, layout()->totalHeightForWidth(width)); }

PackageRow::PackageRow(std::string_view package, std::string_view summary, const Source& source,
                       std::string_view installation_state, std::string_view accessible_name,
                       std::span<const Action> row_actions, QWidget* parent) : QFrame(parent) {
    setProperty("brRole", "card");
    setMinimumHeight(br_component_row_height_min);
    name(*this, accessible_name, installation_state);
    auto* box = column(this, br_card_padding);
    auto* row = new QHBoxLayout;
    row->setSpacing(br_space_2);
    row->addWidget(new IconLabel("braurser-package-symbolic", br_package_icon_size, this));
    row->addWidget(label(package, this, true), 1);
    box->addLayout(row);
    box->addWidget(label(summary, this, true));
    box->addWidget(new SourceBadge(source, this));
    box->addWidget(label(installation_state, this));
    detail::actions(*box, row_actions);
}

TrustGrid::TrustGrid(std::span<const Fact> facts, std::string_view notice, QWidget* parent) : QWidget(parent) {
    if (facts.size() > 64) { throw std::length_error("too many facts"); }
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(br_space_3);
    form->setVerticalSpacing(br_space_2);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const auto& fact : facts) { form->addRow(label(fact.label, this), label(fact.value, this, true)); }
    if (!notice.empty()) { form->addRow(new Banner(Severity::Warning, notice, {}, {}, this)); }
}

FilterChip::FilterChip(std::string_view text, std::string_view remove_name, bool selected, QWidget* parent)
    : QWidget(parent), toggle_(new ActionButton(Action{std::string(text), {}, true, false}, this)), remove_(new QToolButton(this)) {
    auto* box = new QHBoxLayout(this);
    box->setContentsMargins(0, 4, 0, 4);
    box->setSpacing(br_space_2);
    toggle_->setProperty("brRole", "chip");
    toggle_->setCheckable(true);
    toggle_->setChecked(selected);
    target(*remove_, remove_name);
    remove_->setProperty("brRole", "iconButton");
    remove_->setIcon(symbolic_icon("braurser-close-symbolic"));
    remove_->setIconSize(QSize(br_glyph_size_small, br_glyph_size_small));
    remove_->setFixedSize(br_target_width_min, br_target_height_min);
    box->addWidget(toggle_, 1);
    box->addWidget(remove_);
}

QPushButton& FilterChip::toggle() { return *toggle_; }
QToolButton& FilterChip::remove_button() { return *remove_; }

Banner::Banner(Severity severity, std::string_view heading, std::string_view body,
               std::span<const Action> banner_actions, QWidget* parent) : QFrame(parent) {
    const char* icon = nullptr;
    const char* role = nullptr;
    switch (severity) {
    case Severity::Info: icon = "braurser-info-symbolic"; role = "info"; break;
    case Severity::Success: icon = "braurser-success-symbolic"; role = "success"; break;
    case Severity::Warning: icon = "braurser-warning-symbolic"; role = "warning"; break;
    case Severity::Error: icon = "braurser-error-symbolic"; role = "error"; break;
    default: throw std::invalid_argument("unknown severity");
    }
    setProperty("brRole", role);
    setProperty("brBanner", true);
    name(*this, heading, body);
    auto* box = column(this, br_card_padding);
    box->addWidget(new IconLabel(icon, br_glyph_size, this), 0, Qt::AlignLeft);
    auto* title = label(heading, this);
    title->setProperty("brRole", "sectionTitle");
    box->addWidget(title);
    if (!body.empty()) { box->addWidget(label(body, this, true)); }
    detail::actions(*box, banner_actions);
}

Disclosure::Disclosure(std::string_view title, QWidget& child, std::string_view show_name,
                       std::string_view hide_name, bool initially_expanded, QWidget* parent)
    : QWidget(parent), toggle_(new QToolButton(this)) {
    auto* box = column(this, 4);
    toggle_->setText(safe_text(title));
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setCheckable(true);
    target(*toggle_, initially_expanded ? hide_name : show_name, title);
    toggle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    box->addWidget(toggle_);
    box->addWidget(&child);
    const auto show = safe_text(show_name);
    const auto hide = safe_text(hide_name);
    const auto change = [this, &child, show, hide](bool open) {
        child.setVisible(open);
        toggle_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        toggle_->setAccessibleName(open ? hide : show);
        toggle_->setAccessibleDescription(open ? hide : show);
    };
    connect(toggle_, &QToolButton::toggled, this, change);
    toggle_->setChecked(initially_expanded);
    change(initially_expanded);
}

void Disclosure::set_expanded(bool open) { toggle_->setChecked(open); }
bool Disclosure::expanded() const { return toggle_->isChecked(); }
QToolButton& Disclosure::toggle() { return *toggle_; }

ProgressPanel::ProgressPanel(const ProgressState& state, std::function<void()> cancel, QWidget* parent)
    : QWidget(parent), phase_(label({}, this)), progress_(new ProgressBar(this)), steps_(label({}, this)),
      cancel_(new ActionButton(Action{state.cancel_label, std::move(cancel), state.cancel_enabled, false}, this)) {
    auto* box = column(this, 4);
    box->addWidget(phase_);
    progress_->setFixedHeight(br_progress_height);
    box->addWidget(progress_);
    box->addWidget(steps_);
    box->addWidget(cancel_);
    update(state);
}

void ProgressPanel::update(const ProgressState& state) {
    if (state.steps.size() > 32 || (state.fraction && (!std::isfinite(*state.fraction) || *state.fraction < 0 || *state.fraction > 1))) {
        throw std::invalid_argument("invalid progress state");
    }
    phase_->setText(safe_text(state.phase));
    name(*progress_, state.accessible_name, state.phase);
    progress_->setTextVisible(false);
    progress_->setRange(0, state.fraction ? 1000 : 0);
    if (state.fraction) { progress_->setValue(static_cast<int>(std::round(*state.fraction * 1000))); }
    QStringList steps;
    for (const auto& step : state.steps) { steps.push_back(safe_text(step)); }
    steps_->setText(steps.join('\n'));
    auto* text = cancel_->findChild<QLabel*>();
    if (text) { text->setText(safe_text(state.cancel_label)); }
    name(*cancel_, state.cancel_label);
    cancel_->setEnabled(state.cancel_enabled);
}

PasswordField::PasswordField(std::string_view title, std::string_view caps_warning, QWidget* parent)
    : QWidget(parent), entry_(new QLineEdit(this)), reveal_(new QToolButton(this)), warning_(label(caps_warning, this)) {
    auto* box = column(this, 4);
    auto* caption = label(title, this);
    caption->setBuddy(entry_);
    box->addWidget(caption);
    auto* row = new QHBoxLayout;
    row->setSpacing(br_space_2);
    target(*entry_, title, "Only enter your password if you just approved this action in braurser.");
    entry_->setEchoMode(QLineEdit::Password);
    entry_->setMaxLength(4096);
    entry_->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    target(*reveal_, "Show password", "Show or hide the password without changing it.");
    reveal_->setText("Show");
    reveal_->setCheckable(true);
    connect(reveal_, &QToolButton::toggled, this, [this](bool shown) {
        entry_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
        reveal_->setText(shown ? "Hide" : "Show");
        reveal_->setAccessibleName(shown ? "Hide password" : "Show password");
        entry_->setFocus(Qt::OtherFocusReason);
    });
    row->addWidget(entry_, 1);
    row->addWidget(reveal_);
    box->addLayout(row);
    warning_->setProperty("brRole", "warning");
    warning_->setVisible(false);
    name(*warning_, caps_warning);
    box->addWidget(warning_);
}

PasswordField::~PasswordField() { clear(); }
QLineEdit& PasswordField::entry() { return *entry_; }
QToolButton& PasswordField::reveal_button() { return *reveal_; }
void PasswordField::set_caps_lock(bool active) { warning_->setVisible(active); }
void PasswordField::clear() {
    const QSignalBlocker block_entry(entry_);
    const QSignalBlocker block_reveal(reveal_);
    QString value = entry_->text();
    value.fill(QChar(0));
    entry_->setText(value);
    value.fill(QChar(0));
    value.clear();
    entry_->clear();
    entry_->setEchoMode(QLineEdit::Password);
    reveal_->setChecked(false);
    reveal_->setText("Show");
    reveal_->setAccessibleName("Show password");
}

EmptyState::EmptyState(std::string_view icon, std::string_view title, std::string_view body,
                       const std::optional<Action>& action, QWidget* parent) : QWidget(parent) {
    auto* box = column(this, br_card_padding);
    box->setSpacing(br_space_3);
    box->addWidget(new IconLabel(icon, br_app_icon_size, this));
    auto* heading = label(title, this);
    heading->setProperty("brRole", "pageTitle");
    box->addWidget(heading);
    box->addWidget(label(body, this, true));
    if (action) { detail::actions(*box, std::span(&*action, 1)); }
}

NavigationSidebar::NavigationSidebar(std::span<const NavigationItem> items,
                                     std::function<void(const std::string&)> activate, QWidget* parent) : QWidget(parent) {
    if (items.size() > 32) { throw std::length_error("too many navigation items"); }
    setAccessibleName("Navigation");
    setAccessibleDescription("Choose a section.");
    auto* box = column(this, 4);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    std::set<std::string> ids;
    for (const auto& item : items) {
        if (item.id.empty() || item.id.size() > 128 || !ids.insert(item.id).second) { throw std::invalid_argument("invalid navigation id"); }
        auto* button = new NavigationButton(this);
        button->setProperty("brRole", "navigation");
        button->setFlat(true);
        button->setCheckable(true);
        target(*button, item.accessible_name.empty() ? item.label : item.accessible_name, item.label);
        auto* row = new QHBoxLayout(button);
        row->setContentsMargins(12, 4, 12, 4);
        row->setSpacing(br_space_2);
        row->addWidget(new IconLabel(item.icon, br_glyph_size_small, button));
        auto* caption = label(item.label, button);
        caption->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(caption, 1);
        if (item.count) {
            auto* count = label(std::to_string(*item.count), button);
            count->setProperty("brRole", "sourceBadge");
            count->setAttribute(Qt::WA_TransparentForMouseEvents);
            count->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
            row->addWidget(count);
        }
        button->setMinimumHeight(std::max(44, row->sizeHint().height()));
        group->addButton(button);
        connect(button, &QPushButton::toggled, this, [button](bool selected) {
            for (auto* text : button->findChildren<QLabel*>()) {
                text->setProperty("brSelected", selected);
                text->style()->unpolish(text);
                text->style()->polish(text);
                text->update();
            }
            button->update();
        });
        connect(button, &QPushButton::clicked, this, [activate, id = item.id] { if (activate) { activate(id); } });
        box->addWidget(button);
        buttons_.emplace_back(item.id, button);
    }
    box->addStretch();
}

void NavigationSidebar::select(std::string_view id) {
    for (const auto& [key, button] : buttons_) { if (key == id) { button->setChecked(true); return; } }
}

std::string NavigationSidebar::selected() const {
    for (const auto& [key, button] : buttons_) { if (button->isChecked()) { return key; } }
    return {};
}

ResponsiveShell::ResponsiveShell(QWidget& content, std::span<const NavigationItem> items,
                                 std::string_view navigation_name, std::function<void(const std::string&)> activate,
                                 QWidget* parent) : QWidget(parent), sidebar_(new NavigationSidebar(items, activate, this)),
    body_(new QWidget(this)), content_(&content), menu_(new QToolButton(body_)), popover_(new QFrame(this, Qt::Popup)) {
    setMinimumWidth(280);
    auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    popover_->setProperty("brRole", "popover");
    popover_->setAccessibleName(safe_text(navigation_name));
    popover_->setAccessibleDescription("Choose a section. Escape closes navigation.");
    drawer_ = new NavigationSidebar(items, [this, activate](const std::string& id) {
        select(id);
        popover_->hide();
        menu_->setFocus();
        if (activate) { activate(id); }
    }, popover_);
    auto* popup_layout = column(popover_, 4);
    popup_layout->addWidget(drawer_);
    popover_->setFixedWidth(240);
    popover_->hide();
    auto* box = column(body_, 4);
    target(*menu_, navigation_name, "Open navigation. Escape closes navigation.");
    menu_->setText(safe_text(navigation_name));
    menu_->setArrowType(Qt::DownArrow);
    menu_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    box->addWidget(menu_, 0, Qt::AlignLeft);
    box->addWidget(&content, 1);
    connect(menu_, &QToolButton::clicked, this, [this] {
        popover_->move(menu_->mapToGlobal(QPoint(0, menu_->height())));
        popover_->adjustSize();
        popover_->show();
        const auto buttons = drawer_->findChildren<QPushButton*>();
        if (!buttons.empty()) { buttons.front()->setFocus(); }
    });
}

void ResponsiveShell::select(std::string_view id) { sidebar_->select(id); drawer_->select(id); }
bool ResponsiveShell::sidebar_visible() const { return !sidebar_->isHidden(); }
int ResponsiveShell::content_width() const { return body_->width(); }
QSize ResponsiveShell::sizeHint() const { return {1000, heightForWidth(1000)}; }

int ResponsiveShell::heightForWidth(int width) const {
    const bool wide = width >= 840;
    const int inset = width < 480 ? br_page_inset_narrow : br_page_inset;
    const int available = std::clamp(width - 2 * inset - (wide ? 240 : 0) - 8, 1, 1200);
    const int content_height = content_->hasHeightForWidth() ? content_->heightForWidth(available) : content_->sizeHint().height();
    return std::max(wide ? sidebar_->sizeHint().height() : 0, content_height + (wide ? 0 : 52) + 8) + 2 * inset;
}

void ResponsiveShell::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool wide = width() >= 840;
    const int inset = width() < 480 ? br_page_inset_narrow : br_page_inset;
    sidebar_->setVisible(wide);
    menu_->setVisible(!wide);
    if (wide) {
        popover_->hide();
        sidebar_->setGeometry(inset, inset, 216, std::max(1, height() - 2 * inset));
    }
    const int start = inset + (wide ? 240 : 0);
    const int available = std::max(1, width() - start - inset);
    const int content_width = std::min(1200, available);
    body_->setGeometry(start + (available - content_width) / 2, inset, content_width, std::max(1, height() - 2 * inset));
}

}
