#include <braurser/qt/app.hpp>
#include <braurser/qt/internal.hpp>
#include <QButtonGroup>
#include <QDialog>
#include <QGridLayout>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QStandardItemModel>
#include <QUrl>
#include <algorithm>

namespace braurser::qt {
namespace {
class ResponsiveLayout final : public QLayout {
public:
    ResponsiveLayout(QWidget* parent, bool grid) : QLayout(parent), grid_(grid) { setContentsMargins(0, 0, 0, 0); setSpacing(12); }
    ~ResponsiveLayout() override { while (auto* item = takeAt(0)) { delete item; } }
    void addItem(QLayoutItem* item) override { items_.push_back(item); }
    int count() const override { return static_cast<int>(items_.size()); }
    QLayoutItem* itemAt(int index) const override { return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr; }
    QLayoutItem* takeAt(int index) override {
        auto* item = itemAt(index);
        if (item) { items_.erase(items_.begin() + index); }
        return item;
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override { return {grid_ ? 240 : 0, 0}; }
    QSize sizeHint() const override { return {600, heightForWidth(600)}; }
    void setGeometry(const QRect& rect) override { QLayout::setGeometry(rect); arrange(rect, true); }
private:
    int arrange(const QRect& rect, bool apply) const {
        const int columns = static_cast<int>(presenter::app_grid_columns(static_cast<unsigned>(std::max(0, rect.width()))));
        const int cell = grid_ ? (rect.width() - (columns - 1) * spacing()) / columns : 0;
        int x = 0;
        int y = 0;
        int row_height = 0;
        for (int index = 0; index < count(); ++index) {
            auto* item = itemAt(index);
            const int width = grid_ ? cell : std::min(rect.width(), item->sizeHint().width());
            if (x > 0 && ((grid_ && index % columns == 0) || (!grid_ && x + width > rect.width()))) {
                y += row_height + spacing(); x = 0; row_height = 0;
            }
            const int height = item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
            if (!grid_ && index == count() - 1) { x = std::max(x, rect.width() - width); }
            if (apply) { item->setGeometry(QRect(rect.x() + x, rect.y() + y, width, height)); }
            x += width + spacing(); row_height = std::max(row_height, height);
        }
        return y + row_height;
    }
    bool grid_;
    std::vector<QLayoutItem*> items_;
};
}

void Window::discover_page(QVBoxLayout& box) {
    const auto& state = models_.discover.state();
    label(box, text("discover.title"), "sectionTitle");
    auto* search = entry(box, text("search.placeholder"), {}, "discover-query");
    connect(search, &QLineEdit::returnPressed, this, [this, search] {
        models_.search.query(search->text().toStdString()); navigate("search");
    });
    error(box, state.notice);
    if (state.loading) { label(box, text("state.loading")); }
    if (!models_.first.state().helper_available && models_.settings.state().legacy) {
        auto* first = column();
        auto& content = *static_cast<QVBoxLayout*>(first->layout());
        label(content, models_.first.state().title.render(copy_), "sectionTitle");
        label(content, models_.first.state().body.render(copy_));
        label(content, text("first.official_ready"));
        label(content, text("first.helper_body"));
        button(content, text("first.setup"), [this] { setup(); }, models_.first.state().setup_supported);
        button(content, text("first.browse"), [this] {
            auto selection = models_.search.state().selection;
            selection.source = presenter::SourceFilter::Official;
            models_.search.filters(selection); navigate("search");
        });
        disclosure(box, *first, text("first.title"), copy_);
    }
    label(box, text("discover.categories"), "sectionTitle");
    auto* grid_widget = new QWidget;
    auto* grid = new QGridLayout(grid_widget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(12);
    const int columns = width() < 480 ? 1 : width() < 840 ? 2 : 3;
    int index = 0;
    for (const auto& category : state.categories) {
        auto* cell = column();
        auto& cell_box = *static_cast<QVBoxLayout*>(cell->layout());
        auto* category_button = button(cell_box, category.label.render(copy_), [this, category] {
            auto selection = models_.search.state().selection;
            selection.category = category.id;
            models_.search.filters(selection);
            models_.search.sort("name");
            models_.shell.navigate({presenter::Destination::Search, category.id, 0});
        });
        category_button->setIcon(kit::symbolic_icon(category.icon.starts_with("braurser-") ? category.icon : "braurser-" + category.icon));
        grid->addWidget(cell, index / columns, index % columns);
        ++index;
    }
    box.addWidget(grid_widget);
    if (!state.app_ids.empty()) {
        label(box, text("discover.available"), "sectionTitle");
        auto* apps = new QWidget;
        apps->setObjectName("available-apps");
        auto* cards = new ResponsiveLayout(apps, true);
        for (const auto& id : state.app_ids) {
            const auto& results = state.apps;
            const auto found = std::ranges::find_if(results, [&id](const auto& item) { return item.package.name == id; });
            const auto title = found == results.end() ? id : found->appName.value_or(id);
            const auto summary = found == results.end() ? text("state.unavailable") : found->description;
            const auto badge = found == results.end() ? kit::Source{kit::SourceKind::Foreign, text("state.unavailable"), {}} : source(found->package);
            auto* card = new kit::AppCard(title, summary, badge,
                text("a11y.app_card", {{"app", title}, {"summary", summary}, {"source", badge.label}}));
            card->setMinimumWidth(240);
            connect(card, &QPushButton::clicked, this, [this, id] { open_details(id); });
            cards->addWidget(card);
        }
        box.addWidget(apps);
    }
    button(box, text("discover.git"), [this] { navigate("git"); });
}

void Window::search_page(QVBoxLayout& box) {
    const auto& state = models_.search.state();
    std::string placeholder = text("search.placeholder");
    if (state.selection.category) {
        const auto key = "category." + *state.selection.category;
        const auto category = copy_.contains(key) ? text(key) : *state.selection.category;
        label(box, category, "sectionTitle");
        placeholder = text("search.in_category", {{"category", category}});
    }
    auto* search = entry(box, placeholder, state.query.text, "search-query");
    connect(search, &QLineEdit::textEdited, this, [this](const QString& value) { models_.search.query(value.toStdString()); });
    connect(search, &QLineEdit::returnPressed, this, [this] { models_.search.submit(); });
    auto* toolbar = new QWidget;
    toolbar->setObjectName("search-toolbar");
    auto* row = new ResponsiveLayout(toolbar, false);
    const auto active = presenter::active_filters(state.selection);
    for (const auto& filter : active) {
        const auto title = copy_.contains(filter.label.key) ? filter.label.render(copy_) : state.selection.category.value_or(text("filter.title"));
        auto* chip = new kit::FilterChip(title, text("filter.remove", {{"filter", title}}), true);
        connect(&chip->remove_button(), &QToolButton::clicked, this, [this, removed = filter.removed] { models_.search.filters(removed); });
        row->addWidget(chip);
    }
    auto* controls = new QWidget;
    auto* controls_box = new QHBoxLayout(controls);
    controls_box->setContentsMargins(0, 0, 0, 0); controls_box->setSpacing(8);
    auto* filters_button = new kit::detail::ActionButton({text("filter.count", {{"count", static_cast<std::int64_t>(active.size())}}), [this] { filters(); }});
    controls_box->addWidget(filters_button);
    auto* sort = new QComboBox;
    sort->setAccessibleName(kit::safe_text(text("search.sort")));
    sort->setMinimumHeight(44);
    sort->addItem(kit::safe_text(text("search.sort_relevance")), "relevance");
    sort->addItem(kit::safe_text(text("search.sort_name")), "name");
    sort->setCurrentIndex(state.query.sort.value == "name" ? 1 : 0);
    connect(sort, &QComboBox::activated, this, [this, sort](int index) { models_.search.sort(sort->itemData(index).toString().toStdString()); });
    controls_box->addWidget(sort);
    row->addWidget(controls);
    box.addWidget(toolbar);
    if (state.total) { label(box, text("search.results", {{"count", static_cast<std::int64_t>(*state.total)}}))->setObjectName("results-summary"); }
    if (state.loading) { label(box, text("state.loading")); }
    if (state.stale) { label(box, text("offline.local_ready")); }
    error(box, state.error);
    error(box, state.mapping.foreign_note);
    if (state.updates_checked_at) { label(box, text("filter.snapshot", {{"date", copy_.date(*state.updates_checked_at)}})); }
    for (const auto& [name, warning] : state.source_errors) { static_cast<void>(name); error(box, warning); }
    if (state.results.empty() && !state.loading && !state.error) {
        const auto empty = presenter::empty_search(state.query.text);
        box.addWidget(new kit::EmptyState("braurser-search-symbolic", empty.title.render(copy_), empty.body.render(copy_),
            kit::Action{text("filter.clear"), [this] { models_.search.filters({}); }}));
    }
    for (const auto& value : state.results) { package(box, value); }
    if (state.next_cursor) { button(box, text("action.next"), [this] { models_.search.next_page(); }, !state.loading); }
}

void Window::filters() {
    auto* dialog = new QDialog(this, width() >= 840 ? Qt::Popup : Qt::Dialog);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(kit::safe_text(text("filter.title")));
    dialog->resize(std::min(width() - 24, 480), std::min(height() - 48, 660));
    auto* root = new QVBoxLayout(dialog);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    auto* content = column();
    auto& box = *static_cast<QVBoxLayout*>(content->layout());
    scroll->setWidget(content); root->addWidget(scroll);
    auto selection = std::make_shared<presenter::FilterSelection>(models_.search.state().selection);
    const auto mapping = presenter::map_filters(*selection);
    auto* installation = combo(box, text("filter.installation"),
        {{"0", text("filter.any")}, {"1", text("filter.installed")}, {"2", text("filter.not_installed")}},
        std::to_string(static_cast<int>(selection->installation)), [selection](std::string value) { selection->installation = static_cast<presenter::Installation>(std::stoi(value)); });
    if (auto* items = qobject_cast<QStandardItemModel*>(installation->model())) { items->item(2)->setEnabled(mapping.not_installed_enabled); }
    combo(box, text("filter.added_as"), {{"0", text("filter.any")}, {"1", text("filter.explicit")}, {"2", text("filter.dependency")}},
        std::to_string(static_cast<int>(selection->added_as)), [selection, installation](std::string value) {
            selection->added_as = static_cast<presenter::AddedAs>(std::stoi(value));
            const auto mapped = presenter::map_filters(*selection);
            if (auto* items = qobject_cast<QStandardItemModel*>(installation->model())) { items->item(2)->setEnabled(mapped.not_installed_enabled); }
            if (!mapped.not_installed_enabled) { installation->setCurrentIndex(1); selection->installation = presenter::Installation::Installed; }
        });
    check(box, text("filter.orphans"), selection->orphans, [selection, installation](bool value) {
        selection->orphans = value;
        const auto mapped = presenter::map_filters(*selection);
        if (auto* items = qobject_cast<QStandardItemModel*>(installation->model())) { items->item(2)->setEnabled(mapped.not_installed_enabled); }
        if (!mapped.not_installed_enabled) { installation->setCurrentIndex(1); selection->installation = presenter::Installation::Installed; }
    });
    label(box, text("filter.orphans_help"));
    check(box, text("filter.updates"), selection->updates, [selection](bool value) { selection->updates = value; });
    auto* sources = combo(box, text("filter.source_type"), {{"0", text("filter.all")}, {"1", text("filter.official")}, {"2", text("filter.community")}},
        std::to_string(static_cast<int>(selection->source)), [selection](std::string value) { selection->source = static_cast<presenter::SourceFilter>(std::stoi(value)); });
    if (auto* items = qobject_cast<QStandardItemModel*>(sources->model())) { items->item(2)->setEnabled(mapping.community_enabled); }
    label(box, text("filter.repositories"), "sectionTitle");
    auto* repository_search = entry(box, text("filter.repository_search"), {}, "repository-search");
    std::set<std::string> repositories;
    for (const auto& item : models_.search.state().facets.repos) { repositories.insert(item.name); }
    std::vector<QWidget*> repository_rows;
    for (const auto& repository : repositories) {
        auto* item = check(box, repository, std::ranges::find(selection->repositories, repository) != selection->repositories.end(),
            [selection, repository, sources](bool value) {
                if (value) { selection->repositories.push_back(repository); } else { std::erase(selection->repositories, repository); }
                if (auto* items = qobject_cast<QStandardItemModel*>(sources->model())) { items->item(2)->setEnabled(presenter::map_filters(*selection).community_enabled); }
            });
        repository_rows.push_back(item->parentWidget());
        item->parentWidget()->setAccessibleName(kit::safe_text(repository));
    }
    connect(repository_search, &QLineEdit::textEdited, dialog, [repository_rows](const QString& query) {
        for (auto* row : repository_rows) { row->setVisible(row->accessibleName().contains(query, Qt::CaseInsensitive)); }
    });
    if (repositories.empty()) { label(box, text("state.unavailable")); }
    label(box, text("filter.groups"), "sectionTitle");
    std::vector<std::pair<std::string, std::string>> groups{{"", text("filter.any")}};
    for (const auto& item : models_.search.state().facets.groups) { groups.emplace_back(item.name, item.name + " (" + std::to_string(item.packageCount) + ")"); }
    combo(box, text("filter.group_search"), groups, selection->groups.empty() ? "" : selection->groups.front(), [selection](std::string value) { selection->groups.clear(); if (!value.empty()) { selection->groups.push_back(value); } });
    check(box, text("filter.apps_only"), selection->apps_only, [selection](bool value) { selection->apps_only = value; });
    button(*root, text("filter.clear"), [this, dialog] { models_.search.filters({}); dialog->accept(); });
    button(*root, text("action.cancel"), [dialog] { dialog->reject(); });
    button(*root, text("filter.apply"), [this, dialog, selection] {
        models_.search.filters(*selection); dialog->accept();
        QTimer::singleShot(0, this, [this] { if (auto* summary = findChild<QLabel*>("results-summary")) { summary->setFocus(Qt::OtherFocusReason); } });
    }, true, true);
    dialog->open();
}

void Window::details_page(QVBoxLayout& box) {
    const auto& state = models_.details.state();
    if (state.loading) { label(box, text("state.loading")); }
    error(box, state.error);
    if (!state.package) { return; }
    const auto& info = *state.package;
    const auto title = info.appMetadata ? info.appMetadata->name : info.details.package.name;
    auto* heading = new QHBoxLayout;
    heading->setSpacing(12);
    const auto path = info.appMetadata && info.appMetadata->icon_path ? *info.appMetadata->icon_path :
        state.summary ? state.summary->iconPath.value_or("") : "";
    const auto pixmap = kit::app_icon_pixmap(path, 64);
    QWidget* icon = nullptr;
    if (!pixmap.isNull()) {
        auto* image = new QLabel;
        image->setPixmap(pixmap); image->setAlignment(Qt::AlignCenter); image->setFixedSize(64, 64);
        icon = image;
    } else { icon = new kit::detail::IconLabel("braurser-package-symbolic", 64); }
    icon->setObjectName("details-icon");
    heading->addWidget(icon, 0, Qt::AlignVCenter);
    auto* identity = column();
    auto& identity_box = *static_cast<QVBoxLayout*>(identity->layout());
    identity_box.setContentsMargins(0, 0, 0, 0); identity_box.setSpacing(4);
    label(identity_box, title, "sectionTitle");
    label(identity_box, info.appMetadata ? info.appMetadata->summary : info.details.description);
    heading->addWidget(identity, 1);
    box.addLayout(heading);
    if (!info.appMetadata) { label(box, text("state.system_component")); }
    auto badge = source(info.details.package);
    if (state.community) { badge = {kit::SourceKind::Community, text("source.community"), text("source.community_help")}; }
    box.addWidget(new kit::SourceBadge(badge));
    label(box, info.details.package.name);
    button(box, text("variant.choose"), [this] {
        if (models_.details.state().variants) { show_variants(); } else { models_.details.variants(); }
    });
    if (info.appMetadata && !info.appMetadata->screenshots.empty()) { label(box, text("detail.screenshot_missing")); }
    label(box, text("detail.about"), "sectionTitle");
    if (info.appMetadata && !info.appMetadata->description_paragraphs.empty()) {
        for (const auto& paragraph : info.appMetadata->description_paragraphs) { label(box, paragraph); }
    } else { label(box, text("detail.no_description")); }
    std::vector<kit::Fact> facts;
    for (const auto& [name, value] : state.facts) { facts.push_back({name.render(copy_), value.empty() ? text("state.unavailable") : value}); }
    facts.push_back({text("detail.installed_version"), state.summary ? state.summary->installedVersion.value_or(text("state.unavailable")) : text("state.unavailable")});
    facts.push_back({text("detail.download_size"), copy_.size(state.summary ? state.summary->downloadSize : std::nullopt)});
    facts.push_back({text("detail.installed_size"), copy_.size(state.summary ? state.summary->installedSize : std::nullopt)});
    box.addWidget(new kit::TrustGrid(facts));
    if (state.community) {
        label(box, text("community.title"), "sectionTitle");
        facts.clear();
        for (const auto& [name, value] : models_.details.community_rows(copy_)) { facts.push_back({name, value}); }
        box.addWidget(new kit::TrustGrid(facts, text("community.context")));
        if (!state.community->maintainer) { label(box, text("community.orphan_notice")); }
        if (state.community->out_of_date_since) { label(box, text("community.outdated_notice")); }
        auto* community = column();
        label(*static_cast<QVBoxLayout*>(community->layout()), text("community.notice"));
        disclosure(box, *community, text("community.about"), copy_);
    }
    auto* build = column();
    auto& build_box = *static_cast<QVBoxLayout*>(build->layout());
    label(build_box, state.build_disclosure.render(copy_));
    error(build_box, state.build_warning);
    if (state.build) {
        for (const auto& file : state.build->files) {
            label(build_box, text("detail.build_file", {{"file", file.path}}));
            auto* code = new QPlainTextEdit(kit::safe_text(file.text));
            code->setReadOnly(true); code->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            code->setAccessibleName(kit::safe_text(text("detail.build_file", {{"file", file.path}})));
            build_box.addWidget(code);
        }
    }
    disclosure(box, *build, text("detail.build_review"), copy_);
    auto* helpers = column();
    auto& helper_box = *static_cast<QVBoxLayout*>(helpers->layout());
    for (const auto& helper : info.details.dependencies) { label(helper_box, helper); }
    if (info.details.dependencies.empty()) { label(helper_box, text("detail.no_dependencies")); }
    disclosure(box, *helpers, text("detail.dependencies", {{"count", static_cast<std::int64_t>(info.details.dependencies.size())}}), copy_);
    label(box, text("wiki.title"), "sectionTitle");
    button(box, text("wiki.search"), [this] { wiki_search(); });
    error(box, state.wiki_error);
    if (state.wiki) {
        const auto& wiki = *state.wiki;
        label(box, wiki.document.title, "sectionTitle");
        if (wiki.stale || wiki.document.stale || state.offline) { label(box, text("wiki.cached", {{"date", copy_.date(wiki.fetchedAt)}})); }
        button(box, text("wiki.open"), [this, url = wiki.sourceUrl] { platform_.open_url(QUrl(kit::safe_text(url))); });
        auto* view = new kit::DocumentView(document_blocks(wiki.document), text("wiki.code_copy"));
        connect(view, &kit::DocumentView::link_activated, this, [this](const QUrl& url) { platform_.open_url(url); });
        box.addWidget(view);
        label(box, state.wiki_attribution.render(copy_));
        button(box, text("wiki.license"), [this] { platform_.open_url(QUrl("https://www.gnu.org/licenses/fdl-1.3.html")); });
    } else if (!state.wiki_error) {
        label(box, text(state.offline ? "wiki.offline_uncached" : "wiki.empty_title"));
        label(box, text("wiki.empty_body"));
    }
}

void Window::details_action() {
    const auto& current = models_.details.state();
    if (current.can_open && current.package && current.package->appMetadata && !current.update_available) {
        launch(current.package->appMetadata->launchables.front(), current.package->appMetadata->name);
    } else { request_plan({current.update_available ? "update-community" : current.can_remove ? "remove" : "install", current.id}); }
}

void Window::show_variants() {
    const auto& details = models_.details.state();
    if (!details.variants) { return; }
    models_.variants.choices(*details.variants);
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(kit::safe_text(text("variant.title", {{"app", details.id}})));
    auto* root = new QVBoxLayout(dialog);
    auto* scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    auto* content = column();
    auto* box = static_cast<QVBoxLayout*>(content->layout());
    scroll->setWidget(content); root->addWidget(scroll);
    label(*box, text("variant.body"));
    const auto& state = models_.variants.state();
    error(*box, state.note);
    if (!state.selected) { label(*box, text("variant.no_stable")); }
    auto* group = new QButtonGroup(dialog);
    std::vector<QRadioButton*> radios;
    for (std::size_t index = 0; index < state.options.size(); ++index) {
        const auto& row = state.options[index];
        auto* radio = new QRadioButton(kit::safe_text(row.title.render(copy_)), dialog);
        radio->setMinimumHeight(44);
        group->addButton(radio);
        radio->setChecked(state.selected == index);
        box->addWidget(radio);
        radios.push_back(radio);
        label(*box, row.body.render(copy_));
        label(*box, row.option.package.name);
        if (row.reason) { label(*box, row.reason->render(copy_)); }
    }
    button(*root, text("action.cancel"), [dialog] { dialog->reject(); });
    auto* next = button(*root, text("action.continue"), [this, dialog] { dialog->accept(); const auto& choice = models_.variants.state(); if (choice.selected) { request_plan({"install", models_.details.state().id, choice.options[*choice.selected].option.package.name}); } }, state.selected.has_value());
    for (std::size_t index = 0; index < radios.size(); ++index) {
        connect(radios[index], &QRadioButton::toggled, dialog, [this, next, index](bool selected) {
            if (selected) { models_.variants.select(index); next->setEnabled(true); }
        });
    }
    dialog->resize(std::min(width() - 24, 540), std::min(height() - 48, 640));
    dialog->open();
}

}
