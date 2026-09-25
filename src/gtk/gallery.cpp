#include <braurser/gtk/application.hpp>
#include <presenter/fixtures.hpp>
#include <braurser/gtk/render.hpp>
#include <array>
#include <charconv>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>

namespace kit = braurser::gtk::kit;

namespace {
const std::array<std::string, 7> pages{"cards", "badges", "controls", "banners", "progress", "document", "empty"};

struct Options {
    std::string page = "cards";
    kit::Theme theme = kit::Theme::System;
    int width = 1000;
    int exit_after = 0;
    std::string render_png;
};

int integer(std::string_view value, int minimum, int maximum) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < minimum || result > maximum) {
        throw std::invalid_argument("invalid numeric option");
    }
    return result;
}

Options options(int argc, char** argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        if (i + 1 == argc) { throw std::invalid_argument("option requires a value"); }
        const std::string_view value(argv[++i]);
        if (flag == "--page") {
            if (std::ranges::find(pages, value) == pages.end()) { throw std::invalid_argument("unknown page"); }
            result.page = value;
        } else if (flag == "--theme") {
            if (value == "light") { result.theme = kit::Theme::Light; }
            else if (value == "dark") { result.theme = kit::Theme::Dark; }
            else if (value == "system") { result.theme = kit::Theme::System; }
            else { throw std::invalid_argument("unknown theme"); }
        } else if (flag == "--width") {
            result.width = integer(value, 280, 3840);
        } else if (flag == "--render-png") {
            if (value.empty() || value.size() > 4096) { throw std::invalid_argument("invalid PNG path"); }
            result.render_png = value;
        } else if (flag == "--exit-after-ms") {
            result.exit_after = integer(value, 1, 600000);
        } else { throw std::invalid_argument("unknown option"); }
    }
    return result;
}

Gtk::Label& text(Gtk::Box& box, std::string_view value) {
    auto* label = Gtk::make_managed<Gtk::Label>(std::string(value));
    label->set_wrap(true);
    label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    label->set_xalign(0);
    box.append(*label);
    return *label;
}

class Gallery final : public Gtk::Window {
public:
    explicit Gallery(const Options& configuration) {
        set_title("braurser — GTK component gallery");
        set_default_size(configuration.width, 860);
        theme_.manage(*this);
        theme_.set_theme(configuration.theme);
        root_.append(controls_);
        controls_.set_margin(8);
        page_choice_.set_model(Gtk::StringList::create({"Cards", "Badges", "Controls", "Banners", "Progress", "Document", "Empty states"}));
        page_choice_.set_hexpand(true);
        page_choice_.set_size_request(44, 44);
        theme_choice_.set_model(Gtk::StringList::create({"System", "Light", "Dark"}));
        theme_choice_.set_size_request(44, 44);
        theme_choice_.set_selected(static_cast<unsigned>(configuration.theme));
        controls_.append(page_choice_);
        controls_.append(theme_choice_);
        gtk_accessible_update_property(GTK_ACCESSIBLE(page_choice_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, "Sections", -1);
        gtk_accessible_update_property(GTK_ACCESSIBLE(theme_choice_.gobj()), GTK_ACCESSIBLE_PROPERTY_LABEL, "Theme", -1);
        const std::array<kit::NavigationItem, 7> navigation{{
            {"cards", "Discover", "braurser-discover-symbolic", {}, "Discover"},
            {"badges", "Updates", "braurser-updates-symbolic", 12, "Updates, 12 available"},
            {"controls", "Settings", "braurser-settings-symbolic", {}, "Settings"},
            {"banners", "Installed", "braurser-installed-symbolic", {}, "Installed"},
            {"progress", "Activity", "braurser-activity-symbolic", 2, "Activity, 2 waiting"},
            {"document", "Arch Wiki", "braurser-wiki-symbolic", {}, "Arch Wiki"},
            {"empty", "Search", "braurser-search-symbolic", {}, "Search"}
        }};
        shell_ = std::make_unique<kit::ResponsiveShell>(stack_, navigation, "Navigation", [this](const std::string& id) { select(id); });
        shell_->set_vexpand(true);
        root_.append(*shell_);
        set_child(root_);
        build_cards();
        build_badges();
        build_controls();
        build_banners();
        build_progress();
        build_document();
        build_empty();
        page_choice_.property_selected().signal_changed().connect([this] {
            const auto index = page_choice_.get_selected();
            if (index < pages.size()) { select(pages[index]); }
        });
        theme_choice_.property_selected().signal_changed().connect([this] {
            theme_.set_theme(static_cast<kit::Theme>(theme_choice_.get_selected()));
        });
        select(configuration.page);
        if (configuration.exit_after) {
            exit_ = Glib::signal_timeout().connect([this] { close(); return false; }, static_cast<unsigned>(configuration.exit_after));
        }
        allocation_ = Glib::signal_timeout().connect([this] {
            if (!get_mapped() || get_width() <= 0 || get_height() <= 0) { return true; }
            std::cout << "allocation " << get_width() << ' ' << get_height() << '\n' << std::flush;
            return false;
        }, 500);
    }

    ~Gallery() override { exit_.disconnect(); allocation_.disconnect(); }

    void render_png(const std::string& path, int width, std::function<void(bool)> complete) {
        exit_.disconnect();
        allocation_.disconnect();
        stack_.set_hhomogeneous(false);
        stack_.set_vhomogeneous(false);
        auto* scroll = dynamic_cast<Gtk::ScrolledWindow*>(stack_.get_visible_child());
        if (!scroll) { throw std::runtime_error("page has no scroll container"); }
        scroll->set_propagate_natural_height(false);
        scroll->set_min_content_height(0);
        capture_ = std::make_unique<braurser::gtk::FrameCapture>(*this, root_, path, width, std::move(complete));
    }

private:
    Gtk::Box& page(const std::string& id, const std::string& heading) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 16);
        box->set_margin(4);
        text(*box, heading).add_css_class("page-title");
        auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
        scroll->set_child(*box);
        stack_.add(*scroll, id);
        return *box;
    }

    void select(const std::string& id) {
        stack_.set_visible_child(id);
        shell_->select(id);
        const auto found = std::ranges::find(pages, id);
        if (found != pages.end()) { page_choice_.set_selected(static_cast<unsigned>(found - pages.begin())); }
    }

    void build_cards() {
        auto& box = page("cards", "Find something useful");
        const kit::Source official{kit::SourceKind::Official, "Official", "Provided by your distribution’s extra repository."};
        const kit::Source community{kit::SourceKind::Community, "Community build", "Build instructions shared by an AUR contributor, not an official distribution package."};
        for (const auto& item : braurser::presenter::fixtures::data().packages) {
            const auto& source = item.package.source.value == "official" ? official : community;
            box.append(*Gtk::make_managed<kit::AppCard>(item.appName.value_or(item.package.name), item.description, source, item.description));
        }
        auto* disabled = Gtk::make_managed<kit::AppCard>("Not available", "This software is no longer available from the selected source.", official,
            "Not available. This software is no longer available from the selected source.");
        disabled->set_sensitive(false);
        box.append(*disabled);
        const std::array<kit::Action, 1> update{{{"Update", {}, true, false}}};
        box.append(*Gtk::make_managed<kit::PackageRow>("krita", "Create digital paintings and illustrations.", community,
            "Installed", "krita. Create digital paintings and illustrations. Installed.", update));
        box.append(*Gtk::make_managed<kit::PackageRow>("glibc", "System component", official,
            "Installed as a helper", "glibc. System component. Installed as a helper."));
    }

    void build_badges() {
        auto& box = page("badges", "Source");
        const std::array<kit::Source, 5> sources{{
            {kit::SourceKind::Official, "Official", "Provided by your distribution’s extra repository."},
            {kit::SourceKind::Community, "Community build", "Build instructions shared by an AUR contributor, not an official distribution package."},
            {kit::SourceKind::Git, "From Git", "Installed from a Git repository using a script you approved."},
            {kit::SourceKind::Outside, "Outside the store", "This launcher is not managed by braurser."},
            {kit::SourceKind::Foreign, "Outside official repositories", "Not found in your current official repositories. Its original source has not been confirmed."}
        }};
        for (const auto& source : sources) { box.append(*Gtk::make_managed<kit::SourceBadge>(source)); }
        const std::array<std::string, 6> impacts{"Needs a restart", "Needs you to log out and back in", "Restart these apps", "Restart updated services", "No restart needed", "Could not determine"};
        for (std::size_t i = 0; i < impacts.size(); ++i) {
            box.append(*Gtk::make_managed<kit::ImpactBadge>(static_cast<kit::ImpactKind>(i), impacts[i], impacts[i]));
        }
        const std::array<kit::Fact, 4> facts{{{"Maintainer", "No maintainer listed"}, {"Updated", "Not available"}, {"Votes", "42"}, {"Popularity", "1.2"}}};
        box.append(*Gtk::make_managed<kit::TrustGrid>(facts, "This build has no listed maintainer. Updates and fixes may be delayed."));
        text(box, "Community activity is context, not a security check.");
    }

    void build_controls() {
        auto& box = page("controls", "Filters");
        box.append(*Gtk::make_managed<kit::FilterChip>("Installed", "Remove filter: Installed", true));
        box.append(*Gtk::make_managed<kit::FilterChip>("Apps only", "Remove filter: Apps only", false));
        auto* disabled = Gtk::make_managed<kit::FilterChip>("Installed as a helper", "Remove filter: Installed as a helper", false);
        disabled->set_sensitive(false);
        box.append(*disabled);
        text(box, "This filter applies to installed software.");
        auto* password = Gtk::make_managed<kit::PasswordField>("Password for alex", "Caps Lock is on.");
        box.append(*password);
        auto* caps = Gtk::make_managed<kit::PasswordField>("Password for alex", "Caps Lock is on.");
        caps->set_caps_lock(true);
        box.append(*caps);
        auto* checking = Gtk::make_managed<kit::PasswordField>("Password for alex", "Caps Lock is on.");
        checking->set_sensitive(false);
        box.append(*checking);
        text(box, "Authenticating…");
        auto* content = Gtk::make_managed<Gtk::Label>("No additional helper software is needed.");
        content->set_wrap(true);
        box.append(*Gtk::make_managed<kit::Disclosure>("Helper software (0)", *content,
            "Show Helper software", "Hide Helper software"));
        auto* expanded = Gtk::make_managed<Gtk::Label>("Community activity is context, not a security check.");
        expanded->set_wrap(true);
        box.append(*Gtk::make_managed<kit::Disclosure>("About community builds", *expanded,
            "Show About community builds", "Hide About community builds", true));
    }

    void build_banners() {
        auto& box = page("banners", "Installed");
        const std::array<kit::Action, 2> retry{{{"Try again", {}, true, true}, {"Dismiss", {}, true, false}}};
        const std::array<kit::Action, 1> open{{{"Open", {}, true, true}}};
        box.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Info, "You’re offline", "Connect to the internet to browse available software. Installed software is still available.", retry));
        box.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Success, "Updates installed.", "No restart needed", open));
        const std::array<kit::Action, 2> restart{{{"Restart later", {}, true, false}, {"Restart now", {}, true, true}}};
        box.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Warning, "Restart to finish updating", "Save your work before restarting.", restart));
        box.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Error, "Community builds are unavailable", "Official software is still available.", retry));
        box.append(*Gtk::make_managed<kit::Banner>(kit::Severity::Warning, "Read before updating", "Read and acknowledge the required notices before updating."));
    }

    void build_progress() {
        auto& box = page("progress", "Activity");
        box.append(*Gtk::make_managed<kit::ProgressPanel>(kit::ProgressState{
            "Downloading", "Installing Krita. Downloading. 48 of 100 MiB.", 0.48,
            {"Downloading", "Installing", "Finishing"}, "Cancel", true}, [] {}));
        box.append(*Gtk::make_managed<kit::ProgressPanel>(kit::ProgressState{
            "Building", "Installing Krita. Building. Progress amount not available", {},
            {"Building", "Installing", "Finishing"}, "Cancel after this step", true}, [] {}));
        box.append(*Gtk::make_managed<kit::ProgressPanel>(kit::ProgressState{
            "Installing", "Installing Krita. Installing. Progress amount not available", {},
            {"Installing", "Finishing"}, "Finishing…", false}, [] {}));
        text(box, "This step can’t be interrupted while system files are being changed.");
        box.append(*Gtk::make_managed<kit::ProgressPanel>(kit::ProgressState{
            "Finishing", "Installing Krita. Finishing. 100%", 1.0,
            {"Finishing"}, "Cancellation requested. Waiting for a safe stopping point.", false}, [] {}));
    }

    void build_document() {
        auto& box = page("document", "Arch Wiki");
        const braurser::presenter::Copy copy;
        const auto blocks = braurser::gtk::document_blocks(braurser::presenter::fixtures::data().wiki.document, copy);
        box.append(*Gtk::make_managed<kit::DocumentView>(blocks, copy.text("wiki.code_copy")));
        text(box, "From the ArchWiki, GNU FDL 1.3 or later");
    }

    void build_empty() {
        auto& box = page("empty", "Search");
        box.append(*Gtk::make_managed<kit::EmptyState>("braurser-search-symbolic", "No results for “paint”", "Try a different name or remove a filter.", kit::Action{"Clear filters", {}, true, false}));
        box.append(*Gtk::make_managed<kit::EmptyState>("braurser-wiki-symbolic", "No ArchWiki article found", "There isn’t a linked article for this software.", kit::Action{"Search ArchWiki", {}, true, false}));
        box.append(*Gtk::make_managed<kit::EmptyState>("braurser-updates-symbolic", "You’re up to date", "No updates were found at the last check."));
        box.append(*Gtk::make_managed<kit::EmptyState>("braurser-activity-symbolic", "Nothing is running", "Installations, removals, and updates will appear here."));
    }

    std::unique_ptr<braurser::gtk::FrameCapture> capture_;
    kit::ThemeManager theme_;
    Gtk::Box root_{Gtk::Orientation::VERTICAL, 8};
    Gtk::Box controls_{Gtk::Orientation::HORIZONTAL, 8};
    Gtk::DropDown page_choice_;
    Gtk::DropDown theme_choice_;
    Gtk::Stack stack_;
    std::unique_ptr<kit::ResponsiveShell> shell_;
    sigc::connection exit_;
    sigc::connection allocation_;
};
}

int main(int argc, char** argv) {
    try {
        const auto configuration = options(argc, argv);
        if (!configuration.render_png.empty()) { ::umask(0077); }
        const auto app = Gtk::Application::create("org.braurser.Gallery", Gio::Application::Flags::NON_UNIQUE);
        std::unique_ptr<Gallery> gallery;
        int render_status = 0;
        app->signal_activate().connect([&] {
            gallery = std::make_unique<Gallery>(configuration);
            app->add_window(*gallery);
            if (!configuration.render_png.empty()) {
                try { gallery->render_png(configuration.render_png, configuration.width, [&](bool success) {
                    render_status = success ? 0 : 1;
                    std::cout << (success ? "rendered " : "render failed ") << configuration.render_png << '\n';
                    app->quit();
                }); }
                catch (const std::exception& error) { std::cerr << "gallery: " << error.what() << '\n'; render_status = 1; app->quit(); }
            } else { gallery->present(); }
        });
        const int status = app->run(1, argv);
        return render_status != 0 ? render_status : status;
    } catch (const std::exception& error) {
        std::cerr << "gallery: " << error.what() << '\n';
        return 1;
    }
}
