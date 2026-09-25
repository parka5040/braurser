#include <braurser/gtk/kit.hpp>
#include <theme_tokens.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace braurser::gtk::kit {
namespace {
using namespace braurser::theme;

void name(Gtk::Widget& widget, std::string_view text, std::string_view description = {}) {
    const auto label = safe_text(text);
    const auto detail = safe_text(description);
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, detail.c_str(), -1);
}

Gtk::Label& label(std::string_view text, bool selectable = false) {
    auto* widget = Gtk::make_managed<Gtk::Label>(safe_text(text));
    widget->set_xalign(0);
    widget->set_wrap(true);
    widget->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    widget->set_selectable(selectable);
    widget->set_hexpand(true);
    return *widget;
}

Icon& image(std::string_view icon, int size, std::string_view token = "br_text") {
    return *Gtk::make_managed<Icon>(icon, size, token);
}

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

void actions(Gtk::Box& box, std::span<const Action> values) {
    if (values.size() > 2) { throw std::length_error("at most two actions"); }
    for (const auto& action : values) {
        auto* button = Gtk::make_managed<Gtk::Button>();
        button->set_child(label(action.label));
        button->set_size_request(br_target_width_min, br_target_height_min);
        button->set_sensitive(action.enabled);
        if (action.primary) { button->add_css_class("primary"); }
        if (action.activate) { button->signal_clicked().connect(action.activate); }
        box.append(*button);
    }
}

void badge(Gtk::Box& box, std::string_view icon, std::string_view text, std::string_view explanation, std::string_view token = "br_text") {
    box.set_spacing(br_space_2);
    box.set_halign(Gtk::Align::START);
    box.set_hexpand(false);
    box.add_css_class("source-badge");
    box.append(image(icon, br_badge_icon_size, token));
    auto& caption = label(text);
    caption.set_hexpand(false);
    box.append(caption);
    name(box, text, explanation);
    box.set_tooltip_text(safe_text(explanation));
    box.set_focusable(!explanation.empty());
}
}

SourceBadge::SourceBadge(const Source& source) : Gtk::Box(Gtk::Orientation::HORIZONTAL, br_space_2) {
    add_css_class("source-badge");
    badge(*this, source_icon(source.kind), source.label, source.explanation);
}

ImpactBadge::ImpactBadge(ImpactKind kind, std::string_view text, std::string_view explanation)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, br_space_2) {
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
    badge(*this, icon, text, explanation, kind == ImpactKind::None ? "br_success" : kind == ImpactKind::Unknown ? "br_info" : "br_warning");
    if (kind != ImpactKind::None && kind != ImpactKind::Unknown) { add_css_class("banner-warning"); }
}

AppCard::AppCard(std::string_view title, std::string_view summary, const Source& source,
                 std::string_view accessible_name, std::string_view icon) {
    add_css_class("card");
    name(*this, accessible_name);
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, br_space_3);
    auto& glyph = image(icon, br_app_icon_size);
    glyph.set_valign(Gtk::Align::START);
    row->append(glyph);
    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, br_space_2);
    auto& heading = label(title);
    heading.add_css_class("section-title");
    text->append(heading);
    auto& description = label(summary);
    description.set_lines(2);
    description.set_ellipsize(Pango::EllipsizeMode::END);
    description.add_css_class("secondary");
    text->append(description);
    auto* source_badge = Gtk::make_managed<SourceBadge>(source);
    source_badge->set_focusable(false);
    text->append(*source_badge);
    row->append(*text);
    set_child(*row);
    set_size_request(br_target_width_min, br_app_row_height_min);
}

PackageRow::PackageRow(std::string_view package, std::string_view summary, const Source& source,
                       std::string_view installation_state, std::string_view accessible_name,
                       std::span<const Action> row_actions)
    : Gtk::Box(Gtk::Orientation::VERTICAL, br_space_2) {
    set_size_request(-1, br_component_row_height_min);
    name(*this, accessible_name);
    add_css_class("card");
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, br_space_2);
    row->append(image("braurser-package-symbolic", br_package_icon_size));
    row->append(label(package, true));
    append(*row);
    append(label(summary, true));
    append(*Gtk::make_managed<SourceBadge>(source));
    append(label(installation_state));
    actions(*this, row_actions);
}

TrustGrid::TrustGrid(std::span<const Fact> facts, std::string_view notice) {
    if (facts.size() > 64) { throw std::length_error("too many facts"); }
    set_column_spacing(br_space_3);
    set_row_spacing(br_space_2);
    int index = 0;
    for (const auto& fact : facts) {
        auto& key = label(fact.label);
        key.set_max_width_chars(16);
        key.set_hexpand(false);
        attach(key, 0, index);
        attach(label(fact.value, true), 1, index);
        ++index;
    }
    if (!notice.empty()) {
        auto* warning = Gtk::make_managed<ImpactBadge>(ImpactKind::Unknown, notice, notice);
        warning->add_css_class("banner-warning");
        attach(*warning, 0, index, 2, 1);
    }
}

FilterChip::FilterChip(std::string_view text, std::string_view remove_name, bool selected)
    : Gtk::Box(Gtk::Orientation::HORIZONTAL, br_space_2) {
    toggle_.set_child(label(text));
    toggle_.add_css_class("chip");
    toggle_.set_active(selected);
    toggle_.set_hexpand(true);
    toggle_.set_size_request(br_target_width_min, br_target_height_min);
    remove_.set_child(image("braurser-close-symbolic", br_glyph_size_small));
    remove_.set_size_request(br_target_width_min, br_target_height_min);
    name(remove_, remove_name);
    append(toggle_);
    append(remove_);
}

Gtk::ToggleButton& FilterChip::toggle() { return toggle_; }
Gtk::Button& FilterChip::remove_button() { return remove_; }

Banner::Banner(Severity severity, std::string_view heading, std::string_view body,
               std::span<const Action> banner_actions) : Gtk::Box(Gtk::Orientation::VERTICAL, br_space_2) {
    const char* icon = nullptr;
    const char* style = nullptr;
    switch (severity) {
    case Severity::Info: icon = "braurser-info-symbolic"; style = "banner-info"; break;
    case Severity::Success: icon = "braurser-success-symbolic"; style = "banner-success"; break;
    case Severity::Warning: icon = "braurser-warning-symbolic"; style = "banner-warning"; break;
    case Severity::Error: icon = "braurser-error-symbolic"; style = "banner-error"; break;
    default: throw std::invalid_argument("unknown severity");
    }
    add_css_class("card");
    add_css_class(style);
    property_accessible_role() = Gtk::Accessible::Role::STATUS;
    name(*this, heading, body);
    auto& glyph = image(icon, br_glyph_size, std::string("br_") + (style + 7));
    glyph.set_halign(Gtk::Align::START);
    append(glyph);
    auto& title = label(heading);
    title.add_css_class("section-title");
    append(title);
    append(label(body, true));
    actions(*this, banner_actions);
}

Disclosure::Disclosure(std::string_view title, Gtk::Widget& child, std::string_view show_name,
                       std::string_view hide_name, bool expanded)
    : show_name_(safe_text(show_name)), hide_name_(safe_text(hide_name)) {
    auto& title_label = label(title);
    title_label.set_size_request(br_target_width_min, br_target_height_min);
    set_label_widget(title_label);
    set_child(child);
    set_expanded(expanded);
    name(*this, expanded ? hide_name_ : show_name_);
    property_expanded().signal_changed().connect([this] { name(*this, get_expanded() ? hide_name_ : show_name_); });
}

ProgressPanel::ProgressPanel(const ProgressState& state, std::function<void()> cancel)
    : Gtk::Box(Gtk::Orientation::VERTICAL, br_space_2) {
    phase_.set_wrap(true);
    phase_.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    phase_.set_xalign(0);
    append(phase_);
    append(progress_);
    append(steps_);
    append(cancel_);
    cancel_.set_size_request(br_target_width_min, br_target_height_min);
    if (cancel) { cancel_.signal_clicked().connect(std::move(cancel)); }
    update(state);
}

ProgressPanel::~ProgressPanel() { pulse_.disconnect(); }

void ProgressPanel::update(const ProgressState& state) {
    if (state.steps.size() > 32 || (state.fraction && (!std::isfinite(*state.fraction) || *state.fraction < 0 || *state.fraction > 1))) {
        throw std::invalid_argument("invalid progress state");
    }
    pulse_.disconnect();
    phase_.set_text(safe_text(state.phase));
    name(progress_, state.accessible_name);
    gtk_accessible_update_state(GTK_ACCESSIBLE(progress_.gobj()), GTK_ACCESSIBLE_STATE_BUSY, !state.fraction.has_value(), -1);
    if (state.fraction) {
        progress_.set_fraction(*state.fraction);
    } else {
        progress_.pulse();
        gtk_accessible_reset_property(GTK_ACCESSIBLE(progress_.gobj()), GTK_ACCESSIBLE_PROPERTY_VALUE_NOW);
        pulse_ = Glib::signal_timeout().connect([this] {
            const auto settings = Gtk::Settings::get_default();
            if (settings && settings->property_gtk_enable_animations().get_value()) { progress_.pulse(); }
            return true;
        }, 120);
    }
    while (auto* child = steps_.get_first_child()) { steps_.remove(*child); }
    for (const auto& step : state.steps) { steps_.append(label(step)); }
    cancel_.set_child(label(state.cancel_label));
    cancel_.set_sensitive(state.cancel_enabled);
}

PasswordField::PasswordField(std::string_view title, std::string_view caps_warning)
    : Gtk::Box(Gtk::Orientation::VERTICAL, br_space_2), warning_(safe_text(caps_warning)) {
    auto& caption = label(title);
    caption.set_mnemonic_widget(entry_);
    append(caption);
    entry_.set_show_peek_icon(true);
    entry_.set_size_request(br_target_width_min, br_target_height_min);
    name(entry_, title);
    append(entry_);
    warning_.set_wrap(true);
    warning_.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    warning_.set_xalign(0);
    warning_.add_css_class("banner-warning");
    warning_.set_visible(false);
    append(warning_);
}

Gtk::PasswordEntry& PasswordField::entry() { return entry_; }
void PasswordField::set_caps_lock(bool active) { warning_.set_visible(active); }
void PasswordField::clear() { entry_.set_text(""); }

EmptyState::EmptyState(std::string_view icon, std::string_view title, std::string_view body,
                       const std::optional<Action>& action) : Gtk::Box(Gtk::Orientation::VERTICAL, br_space_3) {
    append(image(icon, br_app_icon_size));
    auto& heading = label(title);
    heading.add_css_class("page-title");
    append(heading);
    append(label(body, true));
    if (action) { actions(*this, std::span(&*action, 1)); }
}

NavigationSidebar::NavigationSidebar(std::span<const NavigationItem> items,
                                     std::function<void(const std::string&)> activate)
    : Gtk::Box(Gtk::Orientation::VERTICAL, br_space_2) {
    if (items.size() > 32) { throw std::length_error("too many navigation items"); }
    set_size_request(216, -1);
    property_accessible_role() = Gtk::Accessible::Role::NAVIGATION;
    Gtk::ToggleButton* first = nullptr;
    for (const auto& item : items) {
        if (item.id.size() > 128) { throw std::length_error("navigation id exceeds limit"); }
        auto* button = Gtk::make_managed<Gtk::ToggleButton>();
        button->set_size_request(br_target_width_min, br_target_height_min);
        button->add_css_class("navigation-row");
        button->set_hexpand(true);
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, br_space_2);
        row->append(image(item.icon, br_glyph_size_small));
        row->append(label(item.label));
        if (item.count) {
            auto& count = label(std::to_string(*item.count));
            count.set_hexpand(false);
            count.set_halign(Gtk::Align::END);
            count.set_xalign(1);
            count.add_css_class("navigation-count");
            row->append(count);
        }
        button->set_child(*row);
        name(*button, item.accessible_name.empty() ? item.label : item.accessible_name);
        if (first) { button->set_group(*first); } else { first = button; }
        if (activate) {
            button->signal_clicked().connect([activate, id = item.id] { activate(id); });
        }
        append(*button);
        buttons_.emplace_back(item.id, button);
    }
}

void NavigationSidebar::select(std::string_view id) {
    for (const auto& [key, button] : buttons_) {
        if (key == id) { button->set_active(true); return; }
    }
}

ResponsiveShell::ResponsiveShell(Gtk::Widget& content, std::span<const NavigationItem> items,
                                 std::string_view navigation_name, std::function<void(const std::string&)> activate)
    : Glib::ObjectBase("BraurserResponsiveShell"), sidebar_(items, activate), drawer_(items, [this, activate](const std::string& id) {
        popover_.popdown();
        if (activate) { activate(id); }
    }) {
    sidebar_.set_parent(*this);
    body_.set_parent(*this);
    menu_.set_label(safe_text(navigation_name));
    menu_.set_size_request(br_target_width_min, br_target_height_min);
    menu_.set_halign(Gtk::Align::START);
    popover_.set_child(drawer_);
    menu_.set_popover(popover_);
    menu_.set_parent(*this);
    content.set_vexpand(true);
    content.set_hexpand(true);
    body_.append(content);
}

ResponsiveShell::~ResponsiveShell() {
    sidebar_.unparent();
    body_.unparent();
    menu_.unparent();
}

void ResponsiveShell::select(std::string_view id) { sidebar_.select(id); drawer_.select(id); }
Gtk::SizeRequestMode ResponsiveShell::get_request_mode_vfunc() const { return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH; }

ResponsiveShell::Geometry ResponsiveShell::geometry(int width) const {
    int body_minimum = 0;
    int natural = 0;
    int baseline = -1;
    int natural_baseline = -1;
    body_.measure(Gtk::Orientation::HORIZONTAL, -1, body_minimum, natural, baseline, natural_baseline);
    const int inset = width < static_cast<int>(br_narrow_breakpoint) ? br_page_inset_narrow : br_page_inset;
    int sidebar_minimum = 0;
    sidebar_.measure(Gtk::Orientation::HORIZONTAL, -1, sidebar_minimum, natural, baseline, natural_baseline);
    const int sidebar_width = std::max(216, sidebar_minimum);
    const bool wide = width >= 840 && width >= body_minimum + sidebar_width + 24 + 2 * inset;
    const int available = std::max(body_minimum, width - 2 * inset - (wide ? sidebar_width + 24 : 0));
    const int body_width = std::max(body_minimum, std::min(1200, available));
    int menu_minimum = 0;
    menu_.measure(Gtk::Orientation::VERTICAL, body_width, menu_minimum, natural, baseline, natural_baseline);
    return {inset, sidebar_width, body_width, wide ? 0 : menu_minimum + 8, wide};
}

void ResponsiveShell::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                                    int& minimum_baseline, int& natural_baseline) const {
    minimum_baseline = -1;
    natural_baseline = -1;
    int ignored = -1;
    int ignored_natural = -1;
    if (orientation == Gtk::Orientation::HORIZONTAL) {
        body_.measure(orientation, -1, minimum, natural, ignored, ignored_natural);
        int menu_minimum = 0;
        int menu_natural = 0;
        menu_.measure(orientation, -1, menu_minimum, menu_natural, ignored, ignored_natural);
        minimum = std::max(280, std::max(minimum, menu_minimum) + 2 * static_cast<int>(br_page_inset_narrow));
        natural = std::max(minimum, 1000);
        return;
    }
    const auto layout = geometry(for_size < 0 ? 1000 : for_size);
    body_.measure(orientation, layout.body, minimum, natural, ignored, ignored_natural);
    minimum += layout.menu;
    natural += layout.menu;
    if (layout.wide) {
        int side_minimum = 0;
        int side_natural = 0;
        sidebar_.measure(orientation, layout.sidebar, side_minimum, side_natural, ignored, ignored_natural);
        minimum = std::max(minimum, side_minimum);
        natural = std::max(natural, side_natural);
    }
    minimum += 2 * layout.inset;
    natural += 2 * layout.inset;
}

void ResponsiveShell::snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) {
    if (sidebar_.get_child_visible()) { snapshot_child(sidebar_, snapshot); }
    if (menu_.get_child_visible()) { snapshot_child(menu_, snapshot); }
    snapshot_child(body_, snapshot);
}

void ResponsiveShell::size_allocate_vfunc(int width, int height, int baseline) {
    const auto layout = geometry(width);
    sidebar_.set_child_visible(layout.wide);
    menu_.set_child_visible(!layout.wide);
    const auto allocate = [baseline](Gtk::Widget& widget, int x, int y, int allocated_width, int allocated_height) {
        Gtk::Allocation allocation;
        allocation.set_x(x);
        allocation.set_y(y);
        allocation.set_width(std::max(1, allocated_width));
        allocation.set_height(std::max(1, allocated_height));
        widget.size_allocate(allocation, baseline);
    };
    if (layout.wide) {
        popover_.popdown();
        allocate(sidebar_, layout.inset, layout.inset, layout.sidebar, height - 2 * layout.inset);
    }
    const int start = layout.inset + (layout.wide ? layout.sidebar + 24 : 0);
    const int available = std::max(layout.body, width - start - layout.inset);
    const int x = start + (available - layout.body) / 2;
    if (!layout.wide) { allocate(menu_, x, layout.inset, layout.body, layout.menu - 8); }
    allocate(body_, x, layout.inset + layout.menu, layout.body, height - 2 * layout.inset - layout.menu);
}

}
