#include <braurser/gtk/application.hpp>
#include <array>
#include <type_traits>

namespace braurser::gtk {
namespace {
class AppGrid final : public Gtk::Widget {
public:
    AppGrid() : Glib::ObjectBase("BraurserAppGrid") { set_name("available-apps"); set_hexpand(true); }
    ~AppGrid() override { for (auto* child : children_) { child->unparent(); } }
    void append(Gtk::Widget& child) { children_.push_back(&child); child.set_parent(*this); queue_resize(); }
protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }
    void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural, int& minimum_baseline, int& natural_baseline) const override {
        minimum_baseline = -1; natural_baseline = -1;
        if (orientation == Gtk::Orientation::HORIZONTAL) { minimum = 240; natural = 1200; }
        else { minimum = natural = arrange(std::max(240, for_size), false); }
    }
    void size_allocate_vfunc(int width, int, int) override { arrange(width, true); }
    void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override {
        for (auto* child : children_) { snapshot_child(*child, snapshot); }
    }
private:
    int arrange(int width, bool allocate) const {
        const auto columns = p::app_grid_columns(static_cast<unsigned>(std::max(0, width)));
        const int cell = (width - 12 * (static_cast<int>(columns) - 1)) / static_cast<int>(columns);
        int y = 0;
        for (std::size_t start = 0; start < children_.size(); start += columns) {
            int height = 0;
            const auto end = std::min(children_.size(), start + columns);
            for (auto index = start; index < end; ++index) {
                int minimum = 0, natural = 0, baseline = -1, natural_baseline = -1;
                children_[index]->measure(Gtk::Orientation::VERTICAL, cell, minimum, natural, baseline, natural_baseline);
                height = std::max(height, natural);
            }
            if (allocate) {
                for (auto index = start; index < end; ++index) {
                    children_[index]->size_allocate(Gtk::Allocation(static_cast<int>(index - start) * (cell + 12), y, cell, height), -1);
                }
            }
            y += height + 12;
        }
        return std::max(0, y - 12);
    }
    std::vector<Gtk::Widget*> children_;
};
}

std::string page_id(p::Destination destination) {
    switch (destination) {
    case p::Destination::Discover: return "discover";
    case p::Destination::Search: return "search";
    case p::Destination::Updates: return "updates";
    case p::Destination::Installed: return "installed";
    case p::Destination::Settings: return "settings";
    case p::Destination::Activity: return "activity";
    case p::Destination::Details: return "details";
    case p::Destination::Git: return "git";
    }
    throw std::invalid_argument("unknown destination");
}
p::Destination destination(std::string_view id) {
    for (auto page : {p::Destination::Discover, p::Destination::Search, p::Destination::Updates,
                     p::Destination::Installed, p::Destination::Settings, p::Destination::Activity,
                     p::Destination::Details, p::Destination::Git}) {
        if (page_id(page) == id) { return page; }
    }
    throw std::invalid_argument("unknown page");
}
std::vector<kit::TextRun> document_runs(const std::vector<wire::Inline>& inlines) {
    std::vector<kit::TextRun> runs;
    for (const auto& item : inlines) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, wire::WikiText>) { runs.push_back({value.s, value.emphasis, value.strong, value.code, {}}); }
            else { runs.push_back({value.label, false, false, false, value.href}); }
        }, item.value);
    }
    return runs;
}
std::vector<kit::DocumentBlock> document_blocks(const wire::WikiDocument& document, const p::Copy& copy) {
    std::vector<kit::DocumentBlock> blocks;
    for (const auto& item : document.blocks) {
        kit::DocumentBlock block;
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, wire::WikiHeading>) { block.kind = kit::BlockKind::Heading; block.level = value.level; block.text = value.text; }
            else if constexpr (std::is_same_v<T, wire::WikiCode>) { block.kind = kit::BlockKind::Code; block.text = value.text; }
            else if constexpr (std::is_same_v<T, wire::WikiList>) {
                block.kind = kit::BlockKind::List;
                for (const auto& row : value.items) { block.items.push_back(document_runs(row)); }
            } else {
                block.runs = document_runs(value.inlines);
                if constexpr (std::is_same_v<T, wire::WikiNote>) { block.kind = kit::BlockKind::Note; block.text = copy.text("wiki.note"); }
                if constexpr (std::is_same_v<T, wire::WikiWarning>) { block.kind = kit::BlockKind::Warning; block.text = copy.text("wiki.warning"); }
            }
        }, item.value);
        blocks.push_back(std::move(block));
    }
    return blocks;
}



AppWindow::AppWindow(Models& models, const p::Copy& copy, Platform& platform, int width)
        : models_(models), copy_(copy), platform_(platform) {
    set_default_size(width, 768);
    theme_.manage(*this);
    root_.append(header_);
    root_.append(status_);
    header_.set_margin(12);
    back_.set_child(*Gtk::make_managed<kit::Icon>("braurser-back-symbolic", 20));
    back_.set_tooltip_text(copy_.text("nav.back"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(back_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, copy_.text("nav.back").c_str(), -1);
    back_.set_size_request(44, 44);
    back_.signal_clicked().connect([this] { models_.shell.back(); });
    header_.append(back_);
    heading_.set_xalign(0);
    heading_.set_wrap(true);
    heading_.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    heading_.set_hexpand(true);
    heading_.add_css_class("page-title");
    heading_.property_accessible_role() = Gtk::Accessible::Role::HEADING;
    header_.append(heading_);
    auto* check_caption = Gtk::make_managed<Gtk::Label>(copy_.text("action.check_again"));
    check_caption->set_wrap(true);
    check_caption->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    check_caption->set_max_width_chars(10);
    check_again_.set_child(*check_caption);
    check_again_.set_size_request(44, 44);
    check_again_.signal_clicked().connect([this] { models_.updates.check(); });
    header_.append(check_again_);
    header_.append(activity_button_);
    activity_button_.set_child(*Gtk::make_managed<kit::Icon>("braurser-activity-symbolic", 20));
    activity_button_.set_tooltip_text(copy_.text("nav.activity"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(activity_button_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, copy_.text("nav.activity").c_str(), -1);
    activity_button_.set_size_request(44, 44);
    activity_button_.signal_clicked().connect([this] { navigate(p::Destination::Activity); });
    content_.append(query_);
    query_.set_max_length(4096);
    query_.set_size_request(44, 44);
    query_.set_icon_from_icon_name("edit-clear-symbolic", Gtk::Entry::IconPosition::SECONDARY);
    query_.set_icon_tooltip_text(copy_.text("action.clear_search"), Gtk::Entry::IconPosition::SECONDARY);
    query_.signal_icon_press().connect([this](Gtk::Entry::IconPosition position) {
        if (position == Gtk::Entry::IconPosition::SECONDARY) { query_.set_text(""); query_.grab_focus(); }
    });
    query_.signal_changed().connect([this] {
        if (syncing_) { return; }
        if (models_.shell.state().location.destination == p::Destination::Installed) { models_.installed.query(query_.get_text()); }
        else { models_.search.query(query_.get_text()); }
    });
    query_.signal_activate().connect([this] {
        if (models_.shell.state().location.destination == p::Destination::Installed) { models_.installed.refresh(); }
        else { if (models_.shell.state().location.destination != p::Destination::Search) { navigate(p::Destination::Search); } models_.search.submit(); }
    });
    scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll_.set_child(body_);
    scroll_.set_vexpand(true);
    content_.append(scroll_);
    content_.append(actions_);
    std::vector<kit::NavigationItem> navigation;
    for (const auto* name : {"discover", "search", "updates", "installed", "settings"}) {
        navigation.push_back({name, copy_.text(std::string("nav.") + name), "braurser-" + std::string(name) + "-symbolic", {}, copy_.text(std::string("nav.") + name)});
    }
    shell_ = std::make_unique<kit::ResponsiveShell>(content_, navigation, copy_.text("nav.menu"), [this](const auto& id) { navigate(destination(id)); });
    shell_->set_vexpand(true);
    root_.append(*shell_);
    set_child(root_);
    subscriptions_.push_back(p::bind_page_retention(models_.shell, models_.details, models_.installed));
    observe(models_.shell); observe(models_.discover); observe(models_.search); observe(models_.details);
    observe(models_.variants); observe(models_.confirm); observe(models_.activity); observe(models_.updates);
    observe(models_.installed); observe(models_.git); observe(models_.settings); observe(models_.first);
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(sigc::mem_fun(*this, &AppWindow::key_pressed), false);
    add_controller(keys);
    signal_close_request().connect([this] {
        if (!models_.shell.state().active_job) { return false; }
        auto& box = dialog(copy_.text("activity.close_title"));
        label(box, copy_.text("activity.close_body"));
        button(box, "activity.keep_window", [this] { dialog_->hide(); });
        button(box, "activity.keep_background", [this] { dialog_->hide(); hide(); });
        dialog_->present();
        return true;
    }, false);
    tick_ = add_tick_callback([this](const auto&) {
        const int width_now = get_width();
        if (width_now > 0 && width_now != last_width_) { last_width_ = width_now; models_.shell.width(static_cast<unsigned>(width_now)); }
        return true;
    });
    render();
}

AppWindow::~AppWindow() {
    alive_.reset();
    idle_.disconnect();
    remove_tick_callback(tick_);
    subscriptions_.clear();
    capture_.reset();
    dialog_.reset();
    if (filters_.get_parent()) { filters_.unparent(); }
}

void AppWindow::navigate(p::Destination target, std::string id) {
    models_.shell.navigate({target, std::move(id), scroll_.get_vadjustment()->get_value()});
    refresh();
}

void AppWindow::refresh() {
    switch (models_.shell.state().location.destination) {
    case p::Destination::Discover: models_.first.refresh(); models_.shell.refresh(); break;
    case p::Destination::Search: models_.search.facets(); models_.search.submit(); break;
    case p::Destination::Details: models_.details.load(models_.shell.state().location.id);
        for (const auto& item : models_.search.state().results) { models_.details.summary(item); }
        for (const auto& item : models_.installed.state().packages) { models_.details.summary(item); }
        models_.details.load_wiki(false); break;
    case p::Destination::Updates: models_.updates.check(); break;
    case p::Destination::Installed: models_.installed.refresh(); break;
    case p::Destination::Settings: models_.settings.refresh(); break;
    case p::Destination::Activity: models_.activity.refresh(); break;
    case p::Destination::Git: break;
    }
}

void AppWindow::set_theme(kit::Theme value) { theme_.set_theme(value); }

void AppWindow::render_png(const std::string& path, int width, std::function<void(bool)> done) {
    scroll_.set_propagate_natural_height(false);
    scroll_.set_min_content_height(0);
    render();
    capture_ = std::make_unique<FrameCapture>(*this, root_, path, width, std::move(done));
}

void AppWindow::render() {
    const auto location = models_.shell.state().location;
    const auto id = page_id(location.destination);
    const auto title = location.destination == p::Destination::Details ? copy_.text("action.details") :
        location.destination == p::Destination::Git ? copy_.text("discover.git") : copy_.text("nav." + id);
    set_title(copy_.text("window.title", {{"page", title}}));
    heading_.set_text(title);
    shell_->select(id);
    back_.set_sensitive(!models_.shell.state().history.empty());
    check_again_.set_visible(location.destination == p::Destination::Updates);
    check_again_.set_sensitive(!models_.updates.state().loading);
    syncing_ = true;
    query_.set_visible(location.destination == p::Destination::Discover || location.destination == p::Destination::Search || location.destination == p::Destination::Installed);
    const auto query = location.destination == p::Destination::Installed ? models_.installed.state().query : models_.search.state().query.text;
    if (query_.get_text() != query) { query_.set_text(query); }
    query_.set_placeholder_text(copy_.text(location.destination == p::Destination::Installed ? "search.installed_placeholder" : "search.placeholder"));
    syncing_ = false;
    clear(body_); clear(actions_); clear(status_);
    if (models_.shell.state().connectivity == "offline") { status_.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Info, copy_.text("offline.title"), copy_.text("offline.local_ready"))); }
    if (!platform_error_.empty()) { label(status_, platform_error_); }
    if (models_.shell.state().connected && !models_.shell.state().community_available) { label(status_, copy_.text("error.aur_title")); }
    restart_banner();
    if (plan_pending_ && models_.confirm.state().plan && !models_.confirm.state().submitting && models_.confirm.state().pending_build_files == 0) {
        plan_pending_ = false; show_confirm();
    }
    error(status_, models_.confirm.state().error);
    if (models_.confirm.state().error && models_.confirm.state().error->key == "confirm.plan_changed" && models_.confirm.state().plan) { button(status_, "action.review", [this] { show_confirm(); }); }
    switch (location.destination) {
    case p::Destination::Discover: discover_page(); break;
    case p::Destination::Search: search_page(); break;
    case p::Destination::Details: details_page(); break;
    case p::Destination::Updates: updates_page(); break;
    case p::Destination::Installed: installed_page(); break;
    case p::Destination::Settings: settings_page(); break;
    case p::Destination::Activity: activity_page(body_); break;
    case p::Destination::Git: git_page(); break;
    }
}

void AppWindow::schedule() {
    if (idle_.connected()) { return; }
    idle_ = Glib::signal_idle().connect([this] { render(); return false; });
}

void AppWindow::clear(Gtk::Box& box) { while (auto* child = box.get_first_child()) { box.remove(*child); } }

Gtk::Label& AppWindow::label(Gtk::Box& box, const std::string& text, bool heading) {
    auto* result = Gtk::make_managed<Gtk::Label>(kit::safe_text(text));
    result->set_wrap(true);
    result->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    result->set_xalign(0);
    result->set_hexpand(true);
    result->set_selectable(!heading);
    if (heading) { result->add_css_class("section-title"); result->property_accessible_role() = Gtk::Accessible::Role::HEADING; }
    box.append(*result);
    return *result;
}

Gtk::Button& AppWindow::button_text(Gtk::Box& box, const std::string& text, std::function<void()> action, bool enabled, bool primary) {
    auto* result = Gtk::make_managed<Gtk::Button>();
    auto* caption = Gtk::make_managed<Gtk::Label>(text);
    caption->set_wrap(true); caption->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    result->set_child(*caption);
    result->set_size_request(44, 44);
    result->set_sensitive(enabled);
    if (primary) { result->add_css_class("primary"); }
    result->signal_clicked().connect(std::move(action));
    box.append(*result);
    return *result;
}

Gtk::Button& AppWindow::button(Gtk::Box& box, std::string_view key, std::function<void()> action, bool enabled, bool primary) {
    return button_text(box, copy_.text(key), std::move(action), enabled, primary);
}

Gtk::CheckButton& AppWindow::check(Gtk::Box& box, std::string_view key, bool active, std::function<void(bool)> action, bool enabled) {
    auto* result = Gtk::make_managed<Gtk::CheckButton>();
    auto* caption = Gtk::make_managed<Gtk::Label>(copy_.text(key));
    caption->set_wrap(true); caption->set_wrap_mode(Pango::WrapMode::WORD_CHAR); caption->set_xalign(0);
    result->set_child(*caption);
    result->set_size_request(44, 44);
    result->set_active(active); result->set_sensitive(enabled);
    result->signal_toggled().connect([result, action = std::move(action)] { action(result->get_active()); });
    box.append(*result);
    return *result;
}

void AppWindow::error(Gtk::Box& box, const std::optional<p::Text>& value) { if (value) { label(box, value->render(copy_)).add_css_class("banner-error"); } }

kit::Source AppWindow::source(const wire::PackageRef& package) const {
    const auto key = std::string(p::source_key(package));
    return {key == "official" ? kit::SourceKind::Official : key == "community" ? kit::SourceKind::Community : key == "git" ? kit::SourceKind::Git : kit::SourceKind::Foreign,
        copy_.text("source." + key), copy_.text("source." + key + "_help", {{"repository", package.repo.value_or(copy_.text("state.unavailable"))}})};
}

void AppWindow::empty(Gtk::Box& box, const p::EmptyState& state) {
    box.append(*Gtk::make_managed<kit::EmptyState>("braurser-package-symbolic", state.title.render(copy_), state.body.render(copy_)));
}

void AppWindow::disclose(Gtk::Box& box, const std::string& title, Gtk::Widget& child, bool expanded) {
    box.append(*Gtk::make_managed<kit::Disclosure>(title, child, copy_.text("a11y.reveal", {{"section", title}}), copy_.text("a11y.collapse", {{"section", title}}), expanded));
}

Gtk::Box& AppWindow::dialog(const std::string& title) {
    dialog_ = std::make_unique<Gtk::Window>();
    dialog_->set_title(title); dialog_->set_transient_for(*this); dialog_->set_modal(true);
    dialog_->set_default_size(std::min(std::max(get_width() - 32, 280), 600), 520);
    theme_.manage(*dialog_);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(16); scroll->set_child(*box); dialog_->set_child(*scroll);
    label(*box, title, true);
    return *box;
}

void AppWindow::unavailable() {
    auto& box = dialog(copy_.text("error.unsupported_title"));
    label(box, copy_.text("error.unsupported_body"));
    button(box, "action.close", [this] { dialog_->hide(); });
    dialog_->present();
}

void AppWindow::external(const std::string& url) {
    if (!platform_.open_link(url)) { platform_error_ = copy_.text("error.generic_title"); schedule(); }
}

void AppWindow::launch(const std::string& id, const std::string& name) {
    if (!platform_.open_desktop(id)) { platform_error_ = copy_.text("installed.open_failed", {{"app", name}}); schedule(); }
}

void AppWindow::discover_page() {
    const auto& state = models_.discover.state();
    label(body_, copy_.text("discover.title"), true);
    if (state.loading) { label(body_, copy_.text("state.loading")); }
    error(body_, state.notice);
    if (!models_.first.state().helper_available) {
        auto* welcome = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        label(*welcome, models_.first.state().title.render(copy_), true);
        label(*welcome, models_.first.state().body.render(copy_));
        label(*welcome, copy_.text("first.official_ready"));
        label(*welcome, copy_.text("first.helper_body"));
        button(*welcome, "first.setup", [this] { setup(); }, models_.first.state().setup_supported);
        disclose(body_, copy_.text("first.title"), *welcome);
    }
    label(body_, copy_.text("discover.categories"), true);
    auto* categories = Gtk::make_managed<Gtk::FlowBox>();
    categories->set_selection_mode(Gtk::SelectionMode::NONE);
    categories->set_max_children_per_line(3);
    categories->set_min_children_per_line(1);
    categories->set_column_spacing(8); categories->set_row_spacing(8);
    for (const auto& category : state.categories) {
        auto* item = Gtk::make_managed<Gtk::Button>(category.label.render(copy_));
        item->set_size_request(44, 44);
        item->signal_clicked().connect([this, id = category.id] {
            auto selection = models_.search.state().selection;
            selection.category = id;
            models_.search.filters(selection);
            models_.search.sort("name");
            models_.shell.navigate({p::Destination::Search, id, 0});
        });
        categories->append(*item);
    }
    body_.append(*categories);
    if (!state.app_ids.empty()) {
        label(body_, copy_.text("discover.available"), true);
        auto* apps = Gtk::make_managed<AppGrid>();
        for (const auto& id : state.app_ids) {
            const auto& results = state.apps;
            const auto found = std::ranges::find_if(results, [&id](const auto& item) { return item.package.name == id; });
            const auto title = found == results.end() ? id : found->appName.value_or(id);
            const auto summary = found == results.end() ? copy_.text("state.unavailable") : found->description;
            const auto badge = found == results.end() ? kit::Source{kit::SourceKind::Foreign, copy_.text("state.unavailable"), {}} : source(found->package);
            auto* card = Gtk::make_managed<kit::AppCard>(title, summary, badge,
                copy_.text("a11y.app_card", {{"app", title}, {"summary", summary}, {"source", badge.label}}));
            card->set_size_request(240, -1);
            card->signal_clicked().connect([this, id] { navigate(p::Destination::Details, id); });
            apps->append(*card);
        }
        body_.append(*apps);
    }
    button(body_, "discover.git", [this] { navigate(p::Destination::Git); });
}

void AppWindow::search_page() {
    const auto& state = models_.search.state();
    if (state.selection.category) {
        const auto category = *state.selection.category;
        label(body_, copy_.contains("category." + category) ? copy_.text("category." + category) : category, true);
    }
    auto* toolbar = Gtk::make_managed<Gtk::FlowBox>();
    toolbar->set_name("search-toolbar");
    toolbar->set_selection_mode(Gtk::SelectionMode::NONE);
    toolbar->set_min_children_per_line(1); toolbar->set_max_children_per_line(100);
    toolbar->set_column_spacing(8); toolbar->set_row_spacing(8);
    const auto active = p::active_filters(state.selection);
    for (const auto& filter : active) {
        const auto title = copy_.contains(filter.label.key) ? filter.label.render(copy_) : state.selection.category.value_or(copy_.text("filter.title"));
        auto* chip = Gtk::make_managed<kit::FilterChip>(title, copy_.text("filter.remove", {{"filter", title}}), true);
        chip->set_halign(Gtk::Align::START);
        chip->remove_button().signal_clicked().connect([this, removed = filter.removed] { models_.search.filters(removed); });
        toolbar->append(*chip);
    }
    auto* controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    controls->set_hexpand(true); controls->set_halign(Gtk::Align::END);
    button_text(*controls, copy_.text("filter.count", {{"count", static_cast<std::int64_t>(active.size())}}), [this] { show_filters(); });
    if (state.mapping.foreign_note) { label(body_, state.mapping.foreign_note->render(copy_)); }
    if (state.mapping.disabled_reason) { label(body_, state.mapping.disabled_reason->render(copy_)); }
    if (state.updates_checked_at) { label(body_, copy_.text("filter.snapshot", {{"date", copy_.date(*state.updates_checked_at)}})); }
    auto* sort = Gtk::make_managed<Gtk::DropDown>(std::vector<Glib::ustring>{copy_.text("search.sort_relevance"), copy_.text("search.sort_name")});
    sort->set_selected(state.query.sort.value == "name" ? 1U : 0U);
    sort->set_size_request(44, 44);
    sort->property_selected().signal_changed().connect([this, sort] { models_.search.sort(sort->get_selected() == 0 ? "relevance" : "name"); });
    gtk_accessible_update_property(GTK_ACCESSIBLE(sort->gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, copy_.text("search.sort").c_str(), -1);
    controls->append(*sort);
    toolbar->append(*controls);
    body_.append(*toolbar);
    if (state.loading) { label(body_, copy_.text("state.loading")); }
    error(body_, state.error);
    for (const auto& [name, text] : state.source_errors) { static_cast<void>(name); label(body_, text.render(copy_)); }
    if (state.stale) { label(body_, copy_.text("offline.local_ready")); }
    if (state.total) { label(body_, copy_.text("search.results", {{"count", static_cast<std::int64_t>(*state.total)}})); }
    if (!state.loading && !state.error && state.results.empty()) { empty(body_, p::empty_search(state.query.text)); button(body_, "filter.clear", [this] { models_.search.filters({}); }); }
    for (const auto& package : state.results) { package_card(body_, package); }
    if (!state.results.empty()) { label(body_, copy_.text("search.range", {{"first", static_cast<std::int64_t>(state.visible_first)}, {"last", static_cast<std::int64_t>(state.visible_last)}})); }
    if (state.has_previous) { button(body_, "search.previous", [this] { models_.search.previous_page(); }, !state.loading); }
    if (state.has_next) { button(body_, "search.next", [this] { models_.search.next_page(); }, !state.loading); }
}

void AppWindow::package_card(Gtk::Box& box, const wire::PackageSummary& package) {
    auto* card = Gtk::make_managed<kit::AppCard>(package.package.name, package.description, source(package.package),
        copy_.text("a11y.app_card", {{"app", package.package.name}, {"summary", package.description}, {"source", source(package.package).label}}));
    card->signal_clicked().connect([this, id = package.package.name] { navigate(p::Destination::Details, id); });
    box.append(*card);
    if (package.installed) { label(box, copy_.text("state.installed")); }
}

void AppWindow::show_filters() {
    auto selection = std::make_shared<p::FilterSelection>(models_.search.state().selection);
    Gtk::Box* box = nullptr;
    const bool narrow = get_width() < 840;
    if (narrow) { box = &dialog(copy_.text("filter.title")); }
    else {
        if (!filters_.get_parent()) { filters_.set_parent(query_); }
        box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        box->set_margin(12); filters_.set_child(*box);
        label(*box, copy_.text("filter.title"), true);
    }
    const auto choices = [this, box](std::string_view title, const std::vector<std::string>& keys, unsigned selected, std::function<void(unsigned)> changed) {
        label(*box, copy_.text(title));
        std::vector<Glib::ustring> labels;
        for (const auto& key : keys) { labels.emplace_back(copy_.text(key)); }
        auto* choice = Gtk::make_managed<Gtk::DropDown>(labels);
        choice->set_selected(selected); choice->set_size_request(44, 44);
        choice->property_selected().signal_changed().connect([choice, changed] { changed(choice->get_selected()); });
        box->append(*choice);
    };
    choices("filter.installation", {"filter.any", "filter.installed", "filter.not_installed"}, static_cast<unsigned>(selection->installation), [selection](unsigned value) { selection->installation = static_cast<p::Installation>(value); });
    choices("filter.added_as", {"filter.any", "filter.explicit", "filter.dependency"}, static_cast<unsigned>(selection->added_as), [selection](unsigned value) { selection->added_as = static_cast<p::AddedAs>(value); });
    choices("filter.source_type", {"filter.all", "filter.official", "filter.community"}, static_cast<unsigned>(selection->source), [selection](unsigned value) { selection->source = static_cast<p::SourceFilter>(value); });
    check(*box, "filter.orphans", selection->orphans, [selection](bool value) { selection->orphans = value; });
    label(*box, copy_.text("filter.orphans_help"));
    check(*box, "filter.updates", selection->updates, [selection](bool value) { selection->updates = value; });
    check(*box, "filter.apps_only", selection->apps_only, [selection](bool value) { selection->apps_only = value; });
    const auto facet = [this, box](std::string_view title, const auto& values, std::vector<std::string>& selected) {
        label(*box, copy_.text(title));
        std::vector<Glib::ustring> names{copy_.text("filter.any")};
        unsigned current = 0;
        for (const auto& value : values) { names.emplace_back(value.name + " (" + std::to_string(value.packageCount) + ")"); if (!selected.empty() && selected.front() == value.name) { current = static_cast<unsigned>(names.size() - 1); } }
        auto* choice = Gtk::make_managed<Gtk::DropDown>(names); choice->set_selected(current); choice->set_size_request(44, 44);
        choice->property_selected().signal_changed().connect([choice, values, target = &selected] { target->clear(); if (choice->get_selected() > 0 && choice->get_selected() <= values.size()) { target->push_back(values[choice->get_selected() - 1].name); } });
        box->append(*choice);
    };
    facet("filter.repositories", models_.search.state().facets.repos, selection->repositories);
    facet("filter.groups", models_.search.state().facets.groups, selection->groups);
    button(*box, "filter.clear", [this, narrow] { models_.search.filters({}); if (narrow) { dialog_->hide(); } else { filters_.popdown(); } });
    button(*box, "filter.apply", [this, selection, narrow] {
        models_.search.filters(*selection);
        if (narrow) { dialog_->hide(); } else { filters_.popdown(); }
        scroll_.grab_focus();
    }, true, true);
    if (narrow) { dialog_->present(); } else { filters_.popup(); }
}

void AppWindow::details_page() {
    const auto& state = models_.details.state();
    if (state.loading) { label(body_, copy_.text("state.loading")); }
    error(body_, state.error);
    if (!state.package) { return; }
    const auto& package = *state.package;
    const auto name = package.appMetadata ? package.appMetadata->name : package.details.package.name;
    auto* heading = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    const auto path = package.appMetadata && package.appMetadata->icon_path ? *package.appMetadata->icon_path :
        state.summary ? state.summary->iconPath.value_or("") : "";
    if (const auto texture = kit::app_icon_texture(path, std::clamp(get_scale_factor(), 1, 4))) {
        auto* icon = Gtk::make_managed<Gtk::Image>(texture);
        icon->set_pixel_size(64); icon->set_size_request(64, 64);
        icon->property_accessible_role() = Gtk::Accessible::Role::PRESENTATION;
        heading->append(*icon);
    } else { heading->append(*Gtk::make_managed<kit::Icon>("braurser-package-symbolic", 64)); }
    heading->get_first_child()->set_name("details-icon");
    auto* identity = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    identity->set_hexpand(true);
    label(*identity, name, true);
    label(*identity, package.appMetadata ? package.appMetadata->summary : package.details.description);
    heading->append(*identity);
    body_.append(*heading);
    body_.append(*Gtk::make_managed<kit::SourceBadge>(source(package.details.package)));
    if (!package.appMetadata) { label(body_, copy_.text("state.system_component")); }
    label(body_, copy_.text("detail.about"), true);
    if (package.appMetadata && !package.appMetadata->description_paragraphs.empty()) {
        for (const auto& paragraph : package.appMetadata->description_paragraphs) { label(body_, paragraph); }
    } else { label(body_, copy_.text("detail.no_description")); }
    for (const auto& [key, value] : state.facts) { label(body_, key.render(copy_) + ": " + value); }
    label(body_, copy_.text("detail.installed_version") + ": " + (state.summary ? state.summary->installedVersion.value_or(copy_.text("state.unavailable")) : copy_.text("state.unavailable")));
    label(body_, copy_.text("detail.download_size") + ": " + copy_.size(state.summary ? state.summary->downloadSize : std::nullopt));
    label(body_, copy_.text("detail.installed_size") + ": " + copy_.size(state.summary ? state.summary->installedSize : std::nullopt));
    if (package.appMetadata && !package.appMetadata->screenshots.empty()) { label(body_, copy_.text("detail.screenshot_missing")); }
    if (state.community) {
        label(body_, copy_.text("community.title"), true);
        for (const auto& [key, value] : models_.details.community_rows(copy_)) { label(body_, key + ": " + value); }
        label(body_, copy_.text("community.context"));
    }
    if (package.source.value == "community") {
        auto* notice = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        label(*notice, copy_.text("community.notice"));
        disclose(body_, copy_.text("community.about"), *notice);
        auto* build = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        label(*build, state.build_disclosure.render(copy_));
        if (state.build_warning) { label(*build, state.build_warning->render(copy_)); }
        if (state.build) {
            for (const auto& file : state.build->files) {
                label(*build, copy_.text("detail.build_file", {{"file", file.path}}));
                code(*build, file.text);
            }
        }
        disclose(body_, copy_.text("detail.build_review"), *build);
    }
    auto* dependencies = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    for (const auto& item : package.details.dependencies) { label(*dependencies, item); }
    if (package.details.dependencies.empty()) { label(*dependencies, copy_.text("detail.no_dependencies")); }
    disclose(body_, copy_.text("detail.dependencies", {{"count", static_cast<std::int64_t>(package.details.dependencies.size())}}), *dependencies);
    button(body_, "variant.choose", [this] { models_.details.variants(); variants_pending_ = true; });
    if (variants_pending_ && state.variants) { variants_pending_ = false; models_.variants.choices(*state.variants); show_variants(name); }
    wiki_page();
    if (state.can_open && package.appMetadata) {
        button(actions_, "action.open", [this, id = package.appMetadata->launchables.front(), name] { launch(id, name); }, true, true);
    }
    if (state.can_remove) { button(actions_, "action.remove", [this] { request_plan({"remove", models_.details.state().id}); }); }
    else { button(actions_, "action.install", [this] { request_plan({"install", models_.details.state().id}); }, true, true); }
    if (state.update_available) { button(actions_, "updates.item_update", [this] { request_plan({"update-community", models_.details.state().id}); }); }
}

void AppWindow::code(Gtk::Box& box, const std::string& text) {
    auto* area = Gtk::make_managed<Gtk::TextView>();
    area->set_editable(false); area->set_cursor_visible(false); area->set_monospace(true);
    area->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
    area->get_buffer()->set_text(kit::safe_code(text));
    box.append(*area);
}

void AppWindow::wiki_page() {
    const auto& state = models_.details.state();
    label(body_, copy_.text("wiki.title"), true);
    button(body_, "wiki.search", [this] {
        auto* escaped = g_uri_escape_string(models_.details.state().id.c_str(), nullptr, FALSE);
        const auto url = std::string("https://wiki.archlinux.org/index.php?search=") + escaped;
        g_free(escaped); external(url);
    });
    error(body_, state.wiki_error);
    if (!state.wiki) {
        label(body_, copy_.text(state.offline ? "wiki.offline_uncached" : "wiki.empty_title"));
        label(body_, copy_.text("wiki.empty_body"));
        return;
    }
    const auto& wiki = *state.wiki;
    label(body_, wiki.document.title, true);
    if (wiki.stale || state.offline) { label(body_, copy_.text("wiki.cached", {{"date", copy_.date(wiki.fetchedAt)}})); }
    button(body_, "wiki.open", [this, url = wiki.sourceUrl] { external(url); }, kit::allowed_link(wiki.sourceUrl));
    if (!wiki.document.sections.empty()) {
        auto* sections = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        for (const auto& section : wiki.document.sections) { label(*sections, section.title); }
        disclose(body_, copy_.text("wiki.sections"), *sections);
    }
    body_.append(*Gtk::make_managed<kit::DocumentView>(document_blocks(wiki.document, copy_), copy_.text("wiki.code_copy")));
    label(body_, state.wiki_attribution.render(copy_));
    button(body_, "wiki.license", [this] { external("https://www.gnu.org/licenses/fdl-1.3.html"); });
}

void AppWindow::show_variants(const std::string& name) {
    auto& box = dialog(copy_.text("variant.title", {{"app", name}}));
    label(box, copy_.text("variant.body"));
    const auto& state = models_.variants.state();
    if (state.note) { label(box, state.note->render(copy_)); }
    Gtk::CheckButton* first = nullptr;
    for (std::size_t i = 0; i < state.options.size(); ++i) {
        const auto& option = state.options[i];
        auto* item = Gtk::make_managed<Gtk::CheckButton>(option.title.render(copy_));
        if (first) { item->set_group(*first); } else { first = item; }
        item->set_active(state.selected == i); item->set_size_request(44, 44);
        item->signal_toggled().connect([this, item, i] { if (item->get_active()) { models_.variants.select(i); } });
        box.append(*item);
        label(box, option.body.render(copy_)); label(box, option.option.package.name);
        if (option.reason) { label(box, option.reason->render(copy_)); }
    }
    button(box, "action.cancel", [this] { dialog_->hide(); });
    button(box, "action.continue", [this] {
        const auto& choice = models_.variants.state();
        if (!choice.selected) { return; }
        const auto id = choice.options[*choice.selected].option.package.name;
        dialog_->hide(); navigate(p::Destination::Details, id);
    });
    dialog_->present();
}

void AppWindow::updates_page() {
    const auto& state = models_.updates.state();
    if (state.loading) { label(body_, copy_.text("updates.checking")); }
    error(body_, state.error);
    if (!state.report) { label(body_, copy_.text("updates.never_checked")); return; }
    label(body_, copy_.text("updates.last_checked", {{"relative_time", copy_.date(state.report->checked_at)}})).add_css_class("secondary");
    const auto count = state.report->official.size() + state.report->community.size() + state.report->manual.size();
    label(body_, copy_.text("updates.count", {{"count", static_cast<std::int64_t>(count)}}));
    if (count == 0) { empty(body_, {{"updates.current_title", {}}, {"updates.current_body", {}}, {}}); }
    if (count != 0) {
        button(body_, "updates.all", [this] { models_.updates.plan_system(); update_plan_pending_ = true; }, models_.updates.can_update(), true).set_halign(Gtk::Align::START);
    }
    for (std::size_t index = 0; index < state.report->news.size(); ++index) {
        const auto& news = state.report->news[index];
        auto* notice = Gtk::make_managed<kit::Banner>(kit::Severity::Warning, copy_.text("updates.news_title"), news.title);
        label(*notice, copy_.date(news.published)).add_css_class("secondary");
        button(*notice, "updates.news_read", [this, url = news.url] { external(url); }, kit::allowed_link(news.url)).set_halign(Gtk::Align::START);
        if (news.requires_manual_intervention) {
            check(*notice, "updates.news_ack", news.acknowledged, [this, index](bool value) { models_.updates.acknowledge_news(index, value); });
        }
        body_.append(*notice);
    }
    if (!state.news_available) {
        auto* notice = Gtk::make_managed<kit::Banner>(kit::Severity::Warning, copy_.text("updates.news_title"), copy_.text("updates.news_unavailable"));
        check(*notice, "updates.news_unavailable_ack", state.missing_news_acknowledged, [this](bool value) { models_.updates.acknowledge_missing_news(value); });
        body_.append(*notice);
    }
    for (const auto& group : state.groups) {
        if (group.items.empty()) { continue; }
        const auto& key = group.title.key;
        const auto* icon = key == "updates.restart" ? "braurser-restart-symbolic" :
            key == "updates.logout" ? "braurser-logout-symbolic" : key == "updates.apps" ? "braurser-app-restart-symbolic" :
            key == "updates.none" ? "braurser-success-symbolic" : "braurser-info-symbolic";
        auto* heading = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        heading->append(*Gtk::make_managed<kit::Icon>(icon, 24));
        label(*heading, copy_.text("updates.group_count", {{"group", group.title.render(copy_)}, {"count", static_cast<std::int64_t>(group.items.size())}}), true);
        body_.append(*heading);
        auto* rows = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
        rows->add_css_class("card");
        for (const auto& index : group.items) {
            const std::array lists{&state.report->official, &state.report->community, &state.report->manual};
            const auto& item = lists[index.source]->at(index.item);
            if (rows->get_first_child()) { rows->append(*Gtk::make_managed<Gtk::Separator>()); }
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            auto* details = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
            details->set_hexpand(true);
            label(*details, item.package.name).add_css_class("row-title");
            label(*details, copy_.text("updates.version_change", {{"old_version", item.from_version}, {"new_version", item.to_version}})).add_css_class("secondary");
            details->append(*Gtk::make_managed<kit::SourceBadge>(source(item.package)));
            if (item.restart.detail) { label(*details, *item.restart.detail).add_css_class("secondary"); }
            row->append(*details);
            if (item.source.value == "community" || item.source.value == "git") {
                const auto blocker = models_.updates.item_blocker();
                if (blocker) { label(*details, blocker->render(copy_)).add_css_class("secondary"); }
                auto& update = button(*row, "updates.item_update", [this, id = item.package.name, manual = item.source.value == "git"] {
                    if (manual) { models_.git.upgrade(id); navigate(p::Destination::Git); }
                    else { models_.updates.plan_item("community", id); update_plan_pending_ = true; }
                }, models_.updates.can_update() && !blocker);
                update.set_valign(Gtk::Align::CENTER);
            }
            rows->append(*row);
        }
        body_.append(*rows);
    }
    if (update_plan_pending_ && state.plan) {
        update_plan_pending_ = false;
        if (state.plan->transactions.size() == 1 && state.plan->blockers.empty()) { models_.confirm.plan(state.plan->transactions.front()); plan_pending_ = true; }
        else { unavailable(); }
    }
}

void AppWindow::request_plan(wire::PlanParams params) {
    plan_pending_ = true;
    models_.confirm.request(std::move(params));
}
void AppWindow::setup() {
    auto& box = dialog(copy_.text("first.setup"));
    for (const auto* helper : {"yay", "paru"}) {
        button(box, std::string("first.") + helper, [this, helper] {
            models_.first.helper(helper); plan_pending_ = true; models_.first.review(models_.confirm); dialog_->hide();
        }, models_.first.state().setup_supported);
    }
    dialog_->present();
}
void AppWindow::show_confirm() {
    const auto& state = models_.confirm.state();
    if (!state.plan) { return; }
    const auto& plan = *state.plan;
    auto& box = dialog(models_.confirm.action().render(copy_));
    label(box, copy_.text("confirm.exact"), true);
    for (const auto& [title, items] : std::array<std::pair<std::string, std::vector<wire::PackageRef>>, 3>{{{"confirm.also_installs", plan.installs}, {"confirm.also_updates", plan.upgrades}, {"confirm.also_removes", plan.removals}}}) {
        if (!items.empty()) { label(box, copy_.text(title), true); for (const auto& item : items) { label(box, item.name); } }
    }
    for (const auto& replacement : plan.replacements) { label(box, copy_.text("confirm.replacement", {{"old_package", replacement.old.name}, {"new_package", replacement.new_.name}})); }
    for (const auto& blocker : plan.protected_blockers) { label(box, copy_.text("confirm.protected", {{"package", blocker.name}})); }
    label(box, copy_.text("confirm.download", {{"size", copy_.size(plan.download_bytes)}}));
    label(box, plan.install_delta_bytes ? copy_.text("confirm.storage_add", {{"size", copy_.size(plan.install_delta_bytes)}}) : copy_.text("confirm.storage_unknown"));
    error(box, state.error);
    button(box, "action.cancel", [this] { dialog_->hide(); });
    auto& submit = button_text(box, models_.confirm.action().render(copy_), [this] {
        models_.confirm.submit();
        dialog_->hide();
        navigate(p::Destination::Activity);
    }, models_.confirm.can_confirm(), true);
    if (plan.requires_build_review) {
        label(box, copy_.text("detail.build_review"));
        if (state.pending_build_files == 0 && !state.build_files.empty() && state.build_files.size() == (plan.build_pins.empty() ? plan.build_review_refs.size() : plan.build_pins.size())) {
            for (const auto& build : state.build_files) {
                label(box, build.packageBase);
                for (const auto& file : build.files) { if (file.kind == "text") { label(box, file.name); code(box, file.text); } }
                bool heading = false;
                for (const auto& file : build.files) {
                    if (file.kind != "binary") { continue; }
                    if (!heading) { label(box, copy_.text("detail.build_binary")); heading = true; }
                    label(box, copy_.text("detail.build_binary_file", {{"file", file.name}, {"size", copy_.size(file.size)}, {"sha256", file.sha256.substr(0, 12)}}));
                }
            }
            check(box, "confirm.build_ack", state.build_acknowledged, [this, target = &submit](bool value) { models_.confirm.acknowledge_build(value); target->set_sensitive(models_.confirm.can_confirm()); });
        } else { label(box, copy_.text("detail.build_unavailable")); }
    }
    if (state.requires_dependents_ack) { check(box, "confirm.remove_dependents_ack", state.dependents_acknowledged, [this, target = &submit](bool value) { models_.confirm.acknowledge_dependents(value); target->set_sensitive(models_.confirm.can_confirm()); }); }
    dialog_->present();
}

void AppWindow::activity_page(Gtk::Box& box) {
    const auto& state = models_.activity.state();
    error(box, state.error);
    if (state.auth_message) { label(box, state.auth_message->render(copy_)); }
    if (!state.job_id) { empty(box, {{"activity.empty_title", {}}, {"activity.empty_body", {}}, {}}); }
    else {
        label(box, *state.job_id);
        box.append(*Gtk::make_managed<kit::ProgressPanel>(kit::ProgressState{state.phase.render(copy_), state.phase.render(copy_), state.fraction,
            {}, state.cancel_label.render(copy_), state.can_cancel}, [this] { models_.activity.cancel(); }));
    }
    if (state.question) {
        label(box, copy_.text("activity.needs_attention"), true);
        std::visit([this, &box](const auto& question) {
            using T = std::decay_t<decltype(question)>;
            if constexpr (std::is_same_v<T, wire::ProviderQuestion>) {
                for (const auto& option : question.options) { button_text(box, option.name, [this, id = option.name] { models_.activity.answer(id); }); }
            } else if constexpr (std::is_same_v<T, wire::GitRedirectQuestion>) {
                label(box, copy_.text("git.redirect_body", {{"original_host", question.from_host}, {"destination_host", question.to_host}}));
                button(box, "action.cancel", [this] { models_.activity.answer("cancel"); });
                button(box, "action.continue", [this] { models_.activity.answer("continue"); });
            } else { label(box, copy_.text("error.unsupported_body")); }
        }, state.question->choice.value);
    }
    auto* log = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    std::string lines;
    for (const auto& line : state.log) { lines += line + '\n'; }
    if (lines.size() > 65536) { lines.resize(65536); }
    code(*log, lines);
    button(*log, "activity.copy_log", [this, lines] { get_clipboard()->set_text(lines); });
    label(*log, copy_.text("activity.log_warning"));
    disclose(box, copy_.text("activity.log"), *log);
    label(box, copy_.text("activity.waiting", {{"count", static_cast<std::int64_t>(state.queue.size())}}), true);
    for (const auto& job : state.queue) {
        label(box, job.jobId);
        if (job.state.value == "queued") { button(box, "activity.remove_queue", [this, id = job.jobId] { models_.activity.dequeue(id); }); }
        button(box, job.state.value == "needs-attention" ? "action.continue" : "action.details", [this, id = job.jobId] { models_.activity.attach(id); });
    }
    label(box, copy_.text("activity.recent"), true);
    for (const auto& job : state.recent) { button_text(box, job.jobId, [this, id = job.jobId] { models_.activity.attach(id); }); }
}

void AppWindow::installed_page() {
    const auto& state = models_.installed.state();
    button(body_, "discover.git", [this] { navigate(p::Destination::Git); });
    const std::array<std::string, 4> views{"installed.apps", "installed.components", "installed.git", "installed.outside"};
    std::vector<Glib::ustring> names; for (const auto& key : views) { names.emplace_back(copy_.text(key)); }
    auto* choice = Gtk::make_managed<Gtk::DropDown>(names);
    choice->set_selected(static_cast<unsigned>(state.view)); choice->set_size_request(44, 44);
    choice->property_selected().signal_changed().connect([this, choice] { models_.installed.view(static_cast<p::InstalledView>(choice->get_selected())); });
    body_.append(*choice);
    if (state.loading) { label(body_, copy_.text("state.loading")); }
    error(body_, state.error);
    if (state.view == p::InstalledView::Apps || state.view == p::InstalledView::Components) {
        for (const auto& package : state.packages) { package_card(body_, package); }
        if (!state.loading && !state.error && state.packages.empty()) { empty(body_, p::empty_installed(state.view)); }
    } else if (state.view == p::InstalledView::Git) {
        for (const auto& receipt : state.receipts) {
            label(body_, receipt.name, true);
            label(body_, copy_.text(receipt.scope.value == "user" ? "git.user" : "git.system"));
            label(body_, receipt.commit);
            for (const auto& id : receipt.launcher_ids) { button(body_, "action.open", [this, id, name = receipt.name] { launch(id, name); }, valid_desktop_id(id)); }
            button(body_, receipt.removable ? "action.remove" : "confirm.removal_unavailable", [this, id = receipt.receipt_id] { wire::PlanParams params; params.kind = "manual-remove"; params.receiptId = id; request_plan(params); }, receipt.removable);
            if (!receipt.removable) { label(body_, copy_.text("confirm.git_untracked_removal")); }
        }
        if (!state.loading && !state.error && state.receipts.empty()) { empty(body_, p::empty_installed(state.view)); }
    } else {
        label(body_, copy_.text("installed.outside_help"));
        for (const auto& entry : state.outside) {
            label(body_, entry.name, true);
            label(body_, copy_.text("installed.outside_unmanaged"));
            label(body_, copy_.text("installed.outside_notice"));
            button(body_, "action.open", [this, entry] { launch(entry.desktop_id, entry.name); }, valid_desktop_id(entry.desktop_id));
        }
        if (!state.loading && !state.error && state.outside.empty()) { empty(body_, p::empty_installed(state.view)); }
    }
}

void AppWindow::settings_page() {
    const auto& state = models_.settings.state();
    error(body_, state.error);
    label(body_, copy_.text("settings.community"), true);
    if (!models_.first.state().helper_available) { button(body_, "first.setup", [this] { setup(); }, models_.first.state().setup_supported); }
    label(body_, copy_.text("settings.helper"));
    for (const auto* helper : {"auto", "paru", "yay"}) {
        const auto reason = models_.settings.helper_disabled(helper);
        button(body_, std::string_view(helper) == "auto" ? "settings.automatic" : std::string("settings.") + helper, [this, value = std::string(helper)] {
            wire::SettingsPatch patch; patch.helper = wire::HelperPreference{value}; models_.settings.apply(patch);
        }, !reason);
        if (reason) { label(body_, reason->render(copy_)); }
    }
    label(body_, copy_.text("settings.using_helper", {{"helper", state.effective_helper.empty() ? copy_.text("state.unavailable") : state.effective_helper}}));
    label(body_, copy_.text("settings.updates"), true);
    label(body_, copy_.text("settings.frequency"));
    for (const auto* frequency : {"never", "daily", "weekly"}) {
        button(body_, std::string("settings.") + frequency, [this, value = std::string(frequency)] { wire::SettingsPatch patch; patch.check_frequency = wire::CheckFrequency{value}; models_.settings.apply(patch); });
    }
    const bool notifications = state.values ? state.values->notifications : state.legacy && state.legacy->notifications;
    check(body_, "settings.notifications", notifications, [this](bool value) { wire::SettingsPatch patch; patch.notifications = value; models_.settings.apply(patch); }, state.values.has_value() || state.legacy.has_value());
    check(body_, "settings.autostart", state.values && state.values->autostart, [this](bool value) { wire::SettingsPatch patch; patch.autostart = value; models_.settings.apply(patch); }, state.values.has_value());
    label(body_, copy_.text("settings.autostart_help"));
    label(body_, copy_.text("settings.storage"), true);
    label(body_, copy_.text("settings.cache") + ": " + copy_.size(state.cache_bytes));
    button(body_, "settings.clear_cache", [this] {
        models_.settings.request_clear_cache();
        auto& box = dialog(copy_.text("settings.cache_title", {{"size", copy_.size(models_.settings.state().cache_bytes)}}));
        label(box, copy_.text("settings.cache_body")); label(box, copy_.text("settings.cache_offline"));
        button(box, "action.cancel", [this] { models_.settings.clear_cache(false); dialog_->hide(); });
        button(box, "settings.cache_confirm", [this] { models_.settings.clear_cache(true); dialog_->hide(); });
        dialog_->present();
    }, !state.active_transaction && !state.clearing_cache);
    label(body_, copy_.text("settings.cache_busy"));
    label(body_, copy_.text("settings.advanced"), true);
    check(body_, "settings.review_builds", state.values && state.values->review_builds, [this](bool value) { wire::SettingsPatch patch; patch.review_builds = value; models_.settings.apply(patch); }, state.values.has_value());
    check(body_, "settings.logs", state.values && state.values->show_log, [this](bool value) { wire::SettingsPatch patch; patch.show_log = value; models_.settings.apply(patch); }, state.values.has_value());
    label(body_, copy_.text("settings.appearance"), true);
    for (const auto* theme : {"system", "light", "dark"}) {
        button(body_, std::string("settings.") + theme, [this, value = std::string(theme)] { wire::SettingsPatch patch; patch.theme = wire::Theme{value}; models_.settings.apply(patch); });
    }
    if (state.values) { theme_.set_theme(state.values->theme.value == "dark" ? kit::Theme::Dark : state.values->theme.value == "light" ? kit::Theme::Light : kit::Theme::System); }
}

void AppWindow::git_page() {
    const auto& state = models_.git.state();
    const std::array<std::string, 4> names{"git.choose", "git.review", "git.scope", "git.install_step"};
    label(body_, copy_.text("git.step", {{"step", static_cast<std::int64_t>(state.step) + 1}, {"step_name", copy_.text(names[static_cast<unsigned>(state.step)])}}), true);
    error(body_, state.error);
    if (state.loading) { label(body_, copy_.text("git.fetching")); }
    if (state.redirect) {
        const auto* redirect = std::get_if<wire::GitRedirectQuestion>(&state.redirect->choice.value);
        if (redirect) {
            label(body_, copy_.text("git.redirect_title"), true);
            label(body_, copy_.text("git.redirect_body", {{"original_host", redirect->from_host}, {"destination_host", redirect->to_host}}));
            button(body_, "action.cancel", [this] { models_.git.confirm_redirect(false); });
            button(body_, "action.continue", [this] { models_.git.confirm_redirect(true); });
        }
    } else if (state.step == p::GitStep::Url) {
        label(body_, copy_.text("git.url_label"));
        auto* url = Gtk::make_managed<Gtk::Entry>(); url->set_width_chars(1); url->set_max_width_chars(1); url->set_hexpand(true); url->set_max_length(4096); url->set_text(state.url); url->set_placeholder_text(copy_.text("git.url_placeholder")); url->set_size_request(44, 44); body_.append(*url);
        label(body_, copy_.text("git.url_help"));
        label(body_, copy_.text("git.select_script"));
        auto* script = Gtk::make_managed<Gtk::Entry>(); script->set_width_chars(1); script->set_max_width_chars(1); script->set_hexpand(true); script->set_max_length(4096); script->set_text(state.script); script->set_size_request(44, 44); body_.append(*script);
        button(body_, "action.cancel", [this] { models_.shell.back(); });
        button(body_, "action.next", [this, url, script] { models_.git.inspect(url->get_text(), script->get_text(), "user"); }, !state.loading);
    } else if (state.step == p::GitStep::Review && state.preview) {
        const auto& preview = *state.preview;
        label(body_, copy_.text("git.repository") + ": " + preview.host + "/" + preview.owner_repo);
        label(body_, copy_.text("git.commit") + ": " + preview.commit);
        button(body_, "action.copy", [this, commit = preview.commit] { get_clipboard()->set_text(commit); });
        label(body_, copy_.text("git.script") + ": " + preview.script_path);
        label(body_, copy_.text("git.risk")); label(body_, copy_.text("git.external_risk"));
        code(body_, preview.script_text);
        auto* refs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        for (const auto& ref : preview.external_refs) { label(*refs, ref); }
        disclose(body_, copy_.text("git.other_files"), *refs);
        check(body_, "git.review_ack", state.acknowledged, [this](bool value) { models_.git.acknowledge(value); });
        button(body_, "nav.back", [this] { models_.git.back(); });
        button(body_, "action.next", [this] { models_.git.next(); }, state.acknowledged);
    } else if (state.step == p::GitStep::Scope && state.preview) {
        for (const auto* scope : {"user", "system"}) {
            const bool supported = std::ranges::any_of(state.preview->scope_options, [scope](const auto& option) { return option.value == scope; });
            check(body_, std::string_view(scope) == "user" ? "git.user" : "git.system", state.scope == scope, [this, scope](bool active) { if (active) { models_.git.scope(scope); } }, supported);
            label(body_, copy_.text(std::string("git.") + scope + "_body"));
            if (!supported) { label(body_, copy_.text("git.scope_unavailable")); }
        }
        if (state.scope == "system") { label(body_, copy_.text("git.system_risk")); }
        label(body_, state.preview->host + "/" + state.preview->owner_repo); label(body_, state.preview->commit); label(body_, state.preview->script_path);
        if (state.plan) { label(body_, copy_.text("confirm.exact")); for (const auto& item : state.plan->installs) { label(body_, item.name); } label(body_, copy_.text("confirm.download", {{"size", copy_.size(state.plan->download_bytes)}})); }
        button(body_, "nav.back", [this] { models_.git.back(); });
        button(body_, state.scope == "system" ? "git.install_system" : "git.install_user", [this] {
            models_.git.install();
        }, state.plan.has_value() && state.acknowledged && !state.scope.empty() && !state.loading);
    } else if (state.step == p::GitStep::Progress) {
        if (state.result) {
            label(body_, copy_.text("activity.ready", {{"app", state.result->name}}));
            label(body_, copy_.text(state.result->scope.value == "user" ? "git.installed_user" : "git.installed_system"));
            if (state.result->launcher_ids.empty()) { label(body_, copy_.text("git.no_launcher")); }
            for (const auto& id : state.result->launcher_ids) { button(body_, "action.open", [this, id, name = state.result->name] { launch(id, name); }, valid_desktop_id(id)); }
        } else if (state.failed) {
            label(body_, copy_.text("git.cleanup_unknown"));
            button(body_, "action.review", [this] { navigate(p::Destination::Activity); });
        } else { activity_page(body_); }
        button(body_, "git.view_installed", [this] { models_.installed.view(p::InstalledView::Git); navigate(p::Destination::Installed); });
    }
}

void AppWindow::restart_banner() {
    if (models_.shell.state().restart.level.value != "reboot") { return; }
    std::array<kit::Action, 2> actions{{{copy_.text("restart.later"), [this] { models_.shell.acknowledge_restart(); }, true, false},
        {copy_.text("restart.now"), [this] {
            auto& box = dialog(copy_.text("restart.confirm_title"));
            label(box, copy_.text("restart.confirm_body"));
            button(box, "action.cancel", [this] { dialog_->hide(); });
            button(box, "restart.now", [this] {
                dialog_->hide();
                platform_.reboot([this, weak = std::weak_ptr<int>(alive_)](bool success) {
                    if (!weak.expired() && !success) { platform_error_ = copy_.text("restart.failed"); schedule(); }
                });
            });
            dialog_->present();
        }, true, true}}};
    status_.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Warning, copy_.text("restart.title"), copy_.text("restart.body"), actions));
}

bool AppWindow::key_pressed(guint key, guint, Gdk::ModifierType modifiers) {
    const bool control = (modifiers & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    const bool alt = (modifiers & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
    if (control) {
        if (key == GDK_KEY_f || key == GDK_KEY_k) { navigate(p::Destination::Search); query_.grab_focus(); return true; }
        if (key >= GDK_KEY_1 && key <= GDK_KEY_4) {
            const std::array<p::Destination, 4> pages{p::Destination::Discover, p::Destination::Search, p::Destination::Updates, p::Destination::Installed};
            navigate(pages[key - GDK_KEY_1]); return true;
        }
        if (key == GDK_KEY_comma) { navigate(p::Destination::Settings); return true; }
        if (key == GDK_KEY_j) { navigate(p::Destination::Activity); return true; }
        if (key == GDK_KEY_r) { refresh(); return true; }
    }
    if (alt && key == GDK_KEY_Left) { models_.shell.back(); return true; }
    if (key == GDK_KEY_Escape) {
        filters_.popdown();
        if (models_.shell.state().location.destination == p::Destination::Activity) { models_.shell.back(); }
        return true;
    }
    if (key == GDK_KEY_F1 && models_.details.state().wiki) { external(models_.details.state().wiki->sourceUrl); return true; }
    return false;
}

}
