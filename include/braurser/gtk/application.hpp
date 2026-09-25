#ifndef BRAURSER_GTK_APPLICATION_HPP
#define BRAURSER_GTK_APPLICATION_HPP

#include <braurser/gtk/platform.hpp>
#include <braurser/gtk/render.hpp>

namespace braurser::gtk {
namespace p = braurser::presenter;
namespace wire = braurser::protocol;

struct Models {
    Models(p::Rpc& rpc, p::Timer& timer)
        : shell(rpc), search(rpc, timer), details(rpc), confirm(rpc), activity(rpc), updates(rpc), installed(rpc), git(rpc), settings(rpc), first(rpc) {}
    p::Shell shell;
    p::Discover discover;
    p::Search search;
    p::Details details;
    p::VariantChooser variants;
    p::Confirm confirm;
    p::Activity activity;
    p::Updates updates;
    p::Installed installed;
    p::GitWizard git;
    p::Settings settings;
    p::FirstRun first;
};

std::string page_id(p::Destination destination);
p::Destination destination(std::string_view id);
std::vector<kit::TextRun> document_runs(const std::vector<wire::Inline>& inlines);
std::vector<kit::DocumentBlock> document_blocks(const wire::WikiDocument& document, const p::Copy& copy);

class AppWindow final : public Gtk::ApplicationWindow {
public:
    AppWindow(Models& models, const p::Copy& copy, Platform& platform, int width = 1024);
    ~AppWindow() override;
    void navigate(p::Destination target, std::string id = {});
    void refresh();
    void set_theme(kit::Theme value);
    void render_png(const std::string& path, int width, std::function<void(bool)> done);
    void render();
private:
    template<class Model> void observe(Model& model) {
        subscriptions_.push_back(model.observe([this](const auto&) { schedule(); }));
    }
    void schedule();
    static void clear(Gtk::Box& box);
    Gtk::Label& label(Gtk::Box& box, const std::string& text, bool heading = false);
    Gtk::Button& button_text(Gtk::Box& box, const std::string& text, std::function<void()> action, bool enabled = true, bool primary = false);
    Gtk::Button& button(Gtk::Box& box, std::string_view key, std::function<void()> action, bool enabled = true, bool primary = false);
    Gtk::CheckButton& check(Gtk::Box& box, std::string_view key, bool active, std::function<void(bool)> action, bool enabled = true);
    void error(Gtk::Box& box, const std::optional<p::Text>& value);
    kit::Source source(const wire::PackageRef& package) const;
    void empty(Gtk::Box& box, const p::EmptyState& state);
    void disclose(Gtk::Box& box, const std::string& title, Gtk::Widget& child, bool expanded = false);
    Gtk::Box& dialog(const std::string& title);
    void unavailable();
    void external(const std::string& url);
    void launch(const std::string& id, const std::string& name);
    void discover_page();
    void search_page();
    void package_card(Gtk::Box& box, const wire::PackageSummary& package);
    void show_filters();
    void details_page();
    void code(Gtk::Box& box, const std::string& text);
    void wiki_page();
    void show_variants(const std::string& name);
    void updates_page();
    void show_confirm();
    void request_plan(wire::PlanParams params);
    void setup();
    void activity_page(Gtk::Box& box);
    void installed_page();
    void settings_page();
    void git_page();
    void restart_banner();
    bool key_pressed(guint key, guint code, Gdk::ModifierType modifiers);
    Models& models_;
    const p::Copy& copy_;
    Platform& platform_;
    kit::ThemeManager theme_;
    Gtk::Box root_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box header_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::Button back_;
    Gtk::Label heading_;
    Gtk::Button activity_button_;
    Gtk::Button check_again_;
    Gtk::Box status_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box content_{Gtk::Orientation::VERTICAL, 12};
    Gtk::Entry query_;
    Gtk::ScrolledWindow scroll_;
    Gtk::Box body_{Gtk::Orientation::VERTICAL, 16};
    Gtk::Box actions_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Popover filters_;
    std::unique_ptr<kit::ResponsiveShell> shell_;
    std::unique_ptr<Gtk::Window> dialog_;
    std::unique_ptr<FrameCapture> capture_;
    std::vector<p::Subscription> subscriptions_;
    sigc::connection idle_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    guint tick_ = 0;
    int last_width_ = 0;
    bool syncing_ = false;
    bool variants_pending_ = false;
    bool plan_pending_ = false;
    bool update_plan_pending_ = false;
    std::string platform_error_;
};

}

#endif
