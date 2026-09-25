#include <braurser/qt/app.hpp>
#include <presenter/fixtures.hpp>
#include <braurser/qt/internal.hpp>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QScrollArea>
#include <QTimer>
#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace kit = braurser::qt::kit;

namespace {
const std::array<std::string, 7> pages{"cards", "badges", "controls", "banners", "progress", "document", "empty"};

struct Options {
    std::string page = "cards";
    std::string output;
    kit::Theme theme = kit::Theme::System;
    int width = 1000;
};

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
            int width = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), width);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || width < 280 || width > 3840) {
                throw std::invalid_argument("invalid width");
            }
            result.width = width;
        } else if (flag == "--render-png") {
            if (value.empty() || value.size() > 4096) { throw std::invalid_argument("invalid output path"); }
            result.output = value;
        } else { throw std::invalid_argument("unknown option"); }
    }
    return result;
}

class Gallery final : public QWidget {
public:
    explicit Gallery(const Options& configuration) {
        setWindowTitle("braurser — Qt component gallery");
        theme_.manage(*this);
        theme_.set_theme(configuration.theme);
        auto* root = kit::detail::column(this);
        root->setSizeConstraint(QLayout::SetNoConstraint);
        auto* controls = new QHBoxLayout;
        controls->setContentsMargins(8, 8, 8, 8);
        controls->setSpacing(8);
        page_choice_ = new QComboBox(this);
        for (const auto& id : pages) { page_choice_->addItem(kit::safe_text(id)); }
        kit::detail::target(*page_choice_, "Sections", "Choose a component gallery page.");
        auto* theme_choice = new QComboBox(this);
        theme_choice->addItems({"System", "Light", "Dark"});
        theme_choice->setCurrentIndex(static_cast<int>(configuration.theme));
        kit::detail::target(*theme_choice, "Theme", "Choose System, Light, or Dark appearance.");
        controls->addWidget(page_choice_, 1);
        controls->addWidget(theme_choice);
        root->addLayout(controls);
        content_ = new QWidget;
        content_layout_ = kit::detail::column(content_);
        const std::array<kit::NavigationItem, 7> navigation{{
            {"cards", "Discover", "braurser-discover-symbolic", {}, "Discover"},
            {"badges", "Updates", "braurser-updates-symbolic", 12, "Updates, 12 available"},
            {"controls", "Settings", "braurser-settings-symbolic", {}, "Settings"},
            {"banners", "Installed", "braurser-installed-symbolic", {}, "Installed"},
            {"progress", "Activity", "braurser-activity-symbolic", 2, "Activity, 2 waiting"},
            {"document", "Arch Wiki", "braurser-wiki-symbolic", {}, "Arch Wiki"},
            {"empty", "Search", "braurser-search-symbolic", {}, "Search"}
        }};
        shell_ = new kit::ResponsiveShell(*content_, navigation, "Navigation", [this](const std::string& id) { select(id); });
        if (configuration.output.empty()) {
            auto* scroll = new QScrollArea(this);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidget(shell_);
            scroll->setAccessibleName("Component gallery");
            scroll->setAccessibleDescription("Scroll to inspect the selected gallery page.");
            root->addWidget(scroll, 1);
        } else { root->addWidget(shell_, 1); }
        connect(page_choice_, &QComboBox::currentIndexChanged, this, [this](int index) { select(pages.at(static_cast<std::size_t>(index))); });
        connect(theme_choice, &QComboBox::currentIndexChanged, this, [this](int index) { theme_.set_theme(static_cast<kit::Theme>(index)); });
        select(configuration.page);
        resize(configuration.width, 860);
    }

    int natural_height(int width) const { return shell_->heightForWidth(width) + 76; }

private:
    void select(const std::string& id) {
        const auto position = std::ranges::find(pages, id);
        if (position == pages.end()) { return; }
        const QSignalBlocker blocker(page_choice_);
        page_choice_->setCurrentIndex(static_cast<int>(position - pages.begin()));
        while (auto* item = content_layout_->takeAt(0)) { delete item->widget(); delete item; }
        auto* page = new QWidget(content_);
        auto* box = kit::detail::column(page, 4);
        box->setSpacing(16);
        content_layout_->addWidget(page);
        content_layout_->addStretch();
        if (id == "cards") { cards(*box); }
        else if (id == "badges") { badges(*box); }
        else if (id == "controls") { controls(*box); }
        else if (id == "banners") { banners(*box); }
        else if (id == "progress") { progress(*box); }
        else if (id == "document") { document(*box); }
        else { empty(*box); }
        shell_->select(id);
        shell_->updateGeometry();
    }

    void title(QVBoxLayout& box, std::string_view text) {
        auto* heading = kit::detail::label(text);
        heading->setProperty("brRole", "pageTitle");
        box.addWidget(heading);
    }

    void cards(QVBoxLayout& box) {
        title(box, "Find something useful");
        const kit::Source official{kit::SourceKind::Official, "Official", "Provided by your distribution’s extra repository."};
        const kit::Source community{kit::SourceKind::Community, "Community build", "Build instructions shared by an AUR contributor, not an official distribution package."};
        for (const auto& item : braurser::presenter::fixtures::data().packages) {
            const auto& source = item.package.source.value == "official" ? official : community;
            box.addWidget(new kit::AppCard(item.appName.value_or(item.package.name), item.description, source, item.description));
        }
        auto* disabled = new kit::AppCard("Not available", "This software is no longer available from the selected source.", official,
            "Not available. This software is no longer available from the selected source.");
        disabled->setEnabled(false);
        box.addWidget(disabled);
        const std::array<kit::Action, 1> update{{{"Update", {}, true, false}}};
        box.addWidget(new kit::PackageRow("krita", "Create digital paintings and illustrations.", community,
            "Installed", "krita. Create digital paintings and illustrations. Installed.", update));
        box.addWidget(new kit::PackageRow("glibc", "System component", official,
            "Installed as a helper", "glibc. System component. Installed as a helper."));
    }

    void badges(QVBoxLayout& box) {
        title(box, "Source");
        const std::array<kit::Source, 5> sources{{
            {kit::SourceKind::Official, "Official", "Provided by your distribution’s extra repository."},
            {kit::SourceKind::Community, "Community build", "Build instructions shared by an AUR contributor, not an official distribution package."},
            {kit::SourceKind::Git, "From Git", "Installed from a Git repository using a script you approved."},
            {kit::SourceKind::Outside, "Outside the store", "This launcher is not managed by braurser."},
            {kit::SourceKind::Foreign, "Outside official repositories", "Not found in your current official repositories. Its original source has not been confirmed."}
        }};
        for (const auto& source : sources) { box.addWidget(new kit::SourceBadge(source)); }
        const std::array<std::string, 6> impacts{"Needs a restart", "Needs you to log out and back in", "Restart these apps", "Restart updated services", "No restart needed", "Could not determine"};
        for (std::size_t i = 0; i < impacts.size(); ++i) { box.addWidget(new kit::ImpactBadge(static_cast<kit::ImpactKind>(i), impacts[i], impacts[i])); }
        const std::array<kit::Fact, 4> facts{{{"Maintainer", "No maintainer listed"}, {"Updated", "Not available"}, {"Votes", "42"}, {"Popularity", "1.2"}}};
        box.addWidget(new kit::TrustGrid(facts, "This build has no listed maintainer. Updates and fixes may be delayed."));
        box.addWidget(kit::detail::label("Community activity is context, not a security check."));
    }

    void controls(QVBoxLayout& box) {
        title(box, "Filters");
        box.addWidget(new kit::FilterChip("Installed", "Remove filter: Installed", true));
        box.addWidget(new kit::FilterChip("Apps only", "Remove filter: Apps only", false));
        auto* disabled = new kit::FilterChip("Installed as a helper", "Remove filter: Installed as a helper", false);
        disabled->setEnabled(false);
        box.addWidget(disabled);
        box.addWidget(kit::detail::label("This filter applies to installed software."));
        box.addWidget(new kit::PasswordField("Password for alex", "Caps Lock is on."));
        auto* caps = new kit::PasswordField("Password for alex", "Caps Lock is on.");
        caps->set_caps_lock(true);
        box.addWidget(caps);
        auto* checking = new kit::PasswordField("Password for alex", "Caps Lock is on.");
        checking->setEnabled(false);
        box.addWidget(checking);
        box.addWidget(kit::detail::label("Authenticating…"));
        box.addWidget(new kit::Disclosure("Helper software (0)", *kit::detail::label("No additional helper software is needed."),
            "Show Helper software", "Hide Helper software"));
        box.addWidget(new kit::Disclosure("About community builds", *kit::detail::label("Community activity is context, not a security check."),
            "Show About community builds", "Hide About community builds", true));
    }

    void banners(QVBoxLayout& box) {
        title(box, "Installed");
        const std::array<kit::Action, 2> retry{{{"Try again", {}, true, true}, {"Dismiss", {}, true, false}}};
        const std::array<kit::Action, 1> open{{{"Open", {}, true, true}}};
        box.addWidget(new kit::Banner(kit::Severity::Info, "You’re offline", "Connect to the internet to browse available software. Installed software is still available.", retry));
        box.addWidget(new kit::Banner(kit::Severity::Success, "Updates installed.", "No restart needed", open));
        const std::array<kit::Action, 2> restart{{{"Restart later", {}, true, false}, {"Restart now", {}, true, true}}};
        box.addWidget(new kit::Banner(kit::Severity::Warning, "Restart to finish updating", "Save your work before restarting.", restart));
        box.addWidget(new kit::Banner(kit::Severity::Error, "Community builds are unavailable", "Official software is still available.", retry));
        box.addWidget(new kit::Banner(kit::Severity::Warning, "Read before updating", "Read and acknowledge the required notices before updating."));
    }

    void progress(QVBoxLayout& box) {
        title(box, "Activity");
        box.addWidget(new kit::ProgressPanel(kit::ProgressState{"Downloading", "Installing Krita. Downloading. 48 of 100 MiB.", 0.48,
            {"Downloading", "Installing", "Finishing"}, "Cancel", true}, [] {}));
        box.addWidget(new kit::ProgressPanel(kit::ProgressState{"Building", "Installing Krita. Building. Progress amount not available", {},
            {"Building", "Installing", "Finishing"}, "Cancel after this step", true}, [] {}));
        box.addWidget(new kit::ProgressPanel(kit::ProgressState{"Installing", "Installing Krita. Installing. Progress amount not available", {},
            {"Installing", "Finishing"}, "Finishing…", false}, [] {}));
        box.addWidget(kit::detail::label("This step can’t be interrupted while system files are being changed."));
        box.addWidget(new kit::ProgressPanel(kit::ProgressState{"Finishing", "Installing Krita. Finishing. 100%", 1.0,
            {"Finishing"}, "Cancellation requested. Waiting for a safe stopping point.", false}, [] {}));
    }

    void document(QVBoxLayout& box) {
        title(box, "Arch Wiki");
        const auto blocks = braurser::qt::document_blocks(braurser::presenter::fixtures::data().wiki.document);
        box.addWidget(new kit::DocumentView(blocks, "Copy code"));
        box.addWidget(kit::detail::label("From the ArchWiki, GNU FDL 1.3 or later"));
    }

    void empty(QVBoxLayout& box) {
        title(box, "Search");
        box.addWidget(new kit::EmptyState("braurser-search-symbolic", "No results for “paint”", "Try a different name or remove a filter.", kit::Action{"Clear filters", {}, true, false}));
        box.addWidget(new kit::EmptyState("braurser-wiki-symbolic", "No ArchWiki article found", "There isn’t a linked article for this software.", kit::Action{"Search ArchWiki", {}, true, false}));
        box.addWidget(new kit::EmptyState("braurser-updates-symbolic", "You’re up to date", "No updates were found at the last check."));
        box.addWidget(new kit::EmptyState("braurser-activity-symbolic", "Nothing is running", "Installations, removals, and updates will appear here."));
    }

    kit::ThemeManager theme_;
    QComboBox* page_choice_;
    QWidget* content_;
    QVBoxLayout* content_layout_;
    kit::ResponsiveShell* shell_;
};
}

int main(int argc, char** argv) {
    try {
        const auto configuration = options(argc, argv);
        if (!configuration.output.empty()) { qputenv("QT_QPA_PLATFORM", "offscreen"); }
        QApplication app(argc, argv);
        app.setApplicationName("braurser-qt-gallery");
        Gallery gallery(configuration);
        if (configuration.output.empty()) { gallery.show(); return app.exec(); }
        gallery.resize(configuration.width, 4000);
        gallery.show();
        QApplication::processEvents();
        gallery.resize(configuration.width, std::clamp(gallery.natural_height(configuration.width), 240, 4000));
        QApplication::processEvents();
        if (auto* focused = QApplication::focusWidget()) { focused->clearFocus(); }
        QApplication::processEvents();
        const auto path = kit::safe_text(configuration.output);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate, QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || !gallery.grab().save(&file, "PNG")) {
            throw std::runtime_error("could not save render");
        }
        std::cout << "rendered " << configuration.output << ' ' << gallery.width() << 'x' << gallery.height() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gallery: " << error.what() << '\n';
        return 1;
    }
}
