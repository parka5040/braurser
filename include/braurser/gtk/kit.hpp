#ifndef BRAURSER_GTK_KIT_HPP
#define BRAURSER_GTK_KIT_HPP

#include <gtkmm.h>
#include <giomm/dbusproxy.h>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::gtk::kit {

enum class Theme { System, Light, Dark };
enum class SourceKind { Official, Community, Git, Outside, Foreign };
enum class ImpactKind { Reboot, Logout, RestartApp, RestartService, None, Unknown };
enum class Severity { Info, Success, Warning, Error };

void ensure_resources();
std::string safe_text(std::string_view text);
/// in: script or build text of at most 256 KiB
/// out: valid UTF-8 with controls, bidi and invisible format characters shown as U+FFFD
/// err: throws std::length_error above 256 KiB
std::string safe_code(std::string_view text);

Glib::RefPtr<Gdk::Texture> icon_texture(std::string_view icon, const Gdk::RGBA& color, int size, int scale = 1);
/// in: absolute local PNG path, at most 1 MiB and 1024 pixels per side
/// out: texture or empty for unavailable or invalid input
Glib::RefPtr<Gdk::Texture> app_icon_texture(std::string_view path);

class Icon final : public Gtk::Widget {
public:
    explicit Icon(std::string_view icon, int size, std::string_view token = "br_text");
    ~Icon() override;
protected:
    void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                       int& minimum_baseline, int& natural_baseline) const override;
    void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;
private:
    std::string icon_;
    std::string token_;
    int size_;
    sigc::connection theme_connection_;
};

class ThemeManager final : public sigc::trackable {
public:
    ThemeManager();
    ~ThemeManager();
    void manage(Gtk::Window& window);
    void set_theme(Theme theme);
    Theme resolved() const;
private:
    void apply();
    void read_portal();
    Glib::RefPtr<Gtk::CssProvider> provider_;
    Glib::RefPtr<Gtk::CssProvider> preferences_;
    Glib::RefPtr<Gdk::Display> display_;
    Glib::RefPtr<Gtk::Settings> settings_;
    Glib::RefPtr<Gio::DBus::Proxy> portal_;
    sigc::connection portal_connection_;
    sigc::connection owner_connection_;
    sigc::connection settings_connection_;
    sigc::connection contrast_connection_;
    sigc::connection font_connection_;
    sigc::connection motion_connection_;
    Theme choice_ = Theme::System;
    Theme resolved_ = Theme::Light;
    unsigned portal_scheme_ = 0;
    bool fallback_dark_ = false;
    bool applying_ = false;
    bool attached_ = false;
};

struct Action {
    std::string label;
    std::function<void()> activate;
    bool enabled = true;
    bool primary = false;
};

struct Source {
    SourceKind kind = SourceKind::Official;
    std::string label;
    std::string explanation;
};

class SourceBadge final : public Gtk::Box {
public:
    explicit SourceBadge(const Source& source);
};

class ImpactBadge final : public Gtk::Box {
public:
    ImpactBadge(ImpactKind kind, std::string_view label, std::string_view explanation);
};

class AppCard final : public Gtk::Button {
public:
    AppCard(std::string_view title, std::string_view summary, const Source& source,
            std::string_view accessible_name, std::string_view icon = {});
};

class PackageRow final : public Gtk::Box {
public:
    PackageRow(std::string_view name, std::string_view summary, const Source& source,
               std::string_view installation_state, std::string_view accessible_name,
               std::span<const Action> actions = {});
};

struct Fact { std::string label; std::string value; };

class TrustGrid final : public Gtk::Grid {
public:
    TrustGrid(std::span<const Fact> facts, std::string_view notice = {});
};

class FilterChip final : public Gtk::Box {
public:
    FilterChip(std::string_view text, std::string_view remove_name, bool selected);
    Gtk::ToggleButton& toggle();
    Gtk::Button& remove_button();
private:
    Gtk::ToggleButton toggle_;
    Gtk::Button remove_;
};

class Banner final : public Gtk::Box {
public:
    Banner(Severity severity, std::string_view heading, std::string_view body,
           std::span<const Action> actions = {});
};

class Disclosure final : public Gtk::Expander {
public:
    Disclosure(std::string_view title, Gtk::Widget& child, std::string_view show_name,
               std::string_view hide_name, bool expanded = false);
private:
    std::string show_name_;
    std::string hide_name_;
};

struct ProgressState {
    std::string phase;
    std::string accessible_name;
    std::optional<double> fraction;
    std::vector<std::string> steps;
    std::string cancel_label;
    bool cancel_enabled = true;
};

class ProgressPanel final : public Gtk::Box {
public:
    explicit ProgressPanel(const ProgressState& state, std::function<void()> cancel);
    ~ProgressPanel() override;
    void update(const ProgressState& state);
private:
    Gtk::Label phase_;
    Gtk::ProgressBar progress_;
    Gtk::Box steps_{Gtk::Orientation::VERTICAL, 4};
    Gtk::Button cancel_;
    sigc::connection pulse_;
};

class PasswordField final : public Gtk::Box {
public:
    PasswordField(std::string_view label, std::string_view caps_warning);
    Gtk::PasswordEntry& entry();
    void set_caps_lock(bool active);
    void clear();
private:
    Gtk::PasswordEntry entry_;
    Gtk::Label warning_;
};

class EmptyState final : public Gtk::Box {
public:
    EmptyState(std::string_view icon, std::string_view title, std::string_view body,
               const std::optional<Action>& action = std::nullopt);
};

struct NavigationItem {
    std::string id;
    std::string label;
    std::string icon;
    std::optional<unsigned> count;
    std::string accessible_name;
};

class NavigationSidebar final : public Gtk::Box {
public:
    NavigationSidebar(std::span<const NavigationItem> items,
                      std::function<void(const std::string&)> activate);
    void select(std::string_view id);
private:
    std::vector<std::pair<std::string, Gtk::ToggleButton*>> buttons_;
};

class ResponsiveShell final : public Gtk::Widget {
public:
    ResponsiveShell(Gtk::Widget& content, std::span<const NavigationItem> items,
                    std::string_view navigation_name,
                    std::function<void(const std::string&)> activate);
    ~ResponsiveShell() override;
    void select(std::string_view id);
protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;
    void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum,
                       int& natural, int& minimum_baseline, int& natural_baseline) const override;
    void size_allocate_vfunc(int width, int height, int baseline) override;
    void snapshot_vfunc(const Glib::RefPtr<Gtk::Snapshot>& snapshot) override;
private:
    struct Geometry { int inset; int sidebar; int body; int menu; bool wide; };
    Geometry geometry(int width) const;
    NavigationSidebar sidebar_;
    NavigationSidebar drawer_;
    Gtk::Box body_{Gtk::Orientation::VERTICAL, 8};
    Gtk::MenuButton menu_;
    Gtk::Popover popover_;
};

struct TextRun {
    std::string text;
    bool emphasis = false;
    bool strong = false;
    bool code = false;
    std::string href;
};

enum class BlockKind { Heading, Paragraph, List, Code, Note, Warning };

struct DocumentBlock {
    BlockKind kind = BlockKind::Paragraph;
    unsigned level = 1;
    std::string text;
    std::vector<TextRun> runs;
    std::vector<std::vector<TextRun>> items;
};

std::string build_markup(std::span<const TextRun> runs);
bool allowed_link(std::string_view href);

class DocumentView final : public Gtk::Box {
public:
    DocumentView(std::span<const DocumentBlock> document, std::string_view copy_label);
};

}

#endif
