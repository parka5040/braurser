#include <braurser/qt/kit.hpp>
#include <theme_tokens.hpp>
#include <gtest/gtest.h>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QVBoxLayout>
#include <array>
#include <limits>
#include <filesystem>
#include <QTemporaryDir>
#include <QImage>

namespace kit = braurser::qt::kit;

namespace {
QColor expected(std::string_view name, kit::Theme mode) {
    for (const auto& token : braurser::theme::kColors) {
        if (token.name == name) { return QColor::fromRgb(mode == kit::Theme::Dark ? token.dark : token.light); }
    }
    return {};
}

TEST(QtKitBadges, NarrowForeignBadgeElidesWithoutClippingAndKeepsAccessibleText) {
    const std::string title = "Outside official repositories";
    kit::SourceBadge badge({kit::SourceKind::Foreign, title, "Its original source has not been confirmed."});
    badge.resize(160, badge.sizeHint().height());
    badge.show(); QApplication::processEvents();
    EXPECT_TRUE(badge.accessibleDescription().contains(QString::fromStdString(title)));
    const auto captions = badge.findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
    QLabel* caption = nullptr;
    for (auto* candidate : captions) { if (candidate->text() == QString::fromStdString(title)) { caption = candidate; } }
    ASSERT_NE(caption, nullptr);
    EXPECT_EQ(caption->text(), QString::fromStdString(title));
    EXPECT_GT(caption->width(), 0);
    EXPECT_LT(caption->width(), caption->fontMetrics().horizontalAdvance(caption->text()));
    EXPECT_GE(caption->height(), caption->fontMetrics().height());
    EXPECT_FALSE(badge.grab().isNull());
}

TEST(QtKitPalette, AllRolesMatchGeneratedTokens) {
    const std::array<std::string_view, QPalette::NColorRoles> names{
        "br_text", "br_surface", "br_surface", "br_subtle", "br_separator", "br_line", "br_text",
        "br_on_accent", "br_text", "br_surface", "br_bg", "br_separator", "br_selected", "br_on_selected",
        "br_accent", "br_accent", "br_subtle", "", "br_surface", "br_text", "br_muted", "br_accent"
    };
    for (const auto mode : {kit::Theme::Light, kit::Theme::Dark}) {
        const auto palette = kit::theme_palette(mode);
        for (int index = 0; index < QPalette::NColorRoles; ++index) {
            const auto role = static_cast<QPalette::ColorRole>(index);
            const auto token = names.at(static_cast<std::size_t>(index));
            if (token.empty() || role == QPalette::NoRole) { continue; }
            for (const auto group : {QPalette::Active, QPalette::Inactive}) {
                EXPECT_EQ(palette.color(group, role), expected(token, mode)) << index;
            }
            std::string_view disabled = token;
            switch (role) {
            case QPalette::WindowText: case QPalette::Text: case QPalette::ButtonText: case QPalette::ToolTipText:
            case QPalette::PlaceholderText: case QPalette::HighlightedText: case QPalette::Link: case QPalette::LinkVisited:
            case QPalette::BrightText: disabled = "br_muted"; break;
            case QPalette::Window: case QPalette::Base: case QPalette::AlternateBase: case QPalette::Button:
            case QPalette::ToolTipBase: case QPalette::Highlight: case QPalette::Light: case QPalette::Midlight:
                disabled = "br_subtle"; break;
            default: break;
            }
            EXPECT_EQ(palette.color(QPalette::Disabled, role), expected(disabled, mode)) << index;
        }
    }
}

TEST(QtKitPalette, StylesheetsResolvedAndDesktopFontPreserved) {
    const auto original = QApplication::font().family();
    kit::ThemeManager manager;
    for (const auto mode : {kit::Theme::Light, kit::Theme::Dark}) {
        manager.set_theme(mode);
        EXPECT_EQ(manager.resolved(), mode);
        EXPECT_EQ(QApplication::palette(), kit::theme_palette(mode));
        const auto stylesheet = kit::theme_stylesheet(mode);
        EXPECT_FALSE(stylesheet.contains("${"));
        EXPECT_FALSE(stylesheet.contains("font-family"));
        EXPECT_TRUE(stylesheet.contains(expected("br_bg", mode).name(), Qt::CaseInsensitive));
        EXPECT_EQ(QApplication::font().family(), original);
    }
    EXPECT_THROW(manager.set_theme(static_cast<kit::Theme>(99)), std::invalid_argument);
    EXPECT_THROW(kit::theme_palette(kit::Theme::System), std::invalid_argument);
}

TEST(QtKitIcons, EveryResourceRendersAndSymbolicTintMatches) {
    const auto names = kit::icon_names();
    ASSERT_EQ(names.size(), 30);
    for (const auto& name : names) {
        ASSERT_TRUE(QFile::exists(":/org/braurser/kit/icons/" + name + ".svg"));
        for (const int size : {16, 20, 24, 48}) {
            const QColor color("#BBC4FF");
            const auto image = kit::icon_pixmap(name.toStdString(), size, color).toImage();
            ASSERT_FALSE(image.isNull());
            bool painted = false;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 128) {
                        painted = true;
                        if (name != "braurser") {
                            EXPECT_NEAR(pixel.red(), color.red(), 1);
                            EXPECT_NEAR(pixel.green(), color.green(), 1);
                            EXPECT_NEAR(pixel.blue(), color.blue(), 1);
                        }
                    }
                }
            }
            EXPECT_TRUE(painted) << name.toStdString();
        }
    }
    EXPECT_THROW(kit::icon_pixmap("../../file", 24, Qt::black), std::invalid_argument);
    EXPECT_THROW(kit::icon_pixmap("missing", 24, Qt::black), std::invalid_argument);
    EXPECT_THROW(kit::icon_pixmap("braurser-package-symbolic", 513, Qt::black), std::invalid_argument);
}

TEST(QtKitIcons, LocalAppIconsAreBoundedAndFailClosed) {
    QTemporaryDir directory(QString::fromStdString((std::filesystem::temp_directory_path() / "braurser-icons-XXXXXX").string()));
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("app.png");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_TRUE(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    QImage image(128, 64, QImage::Format_ARGB32);
    image.fill(QColor("#123456"));
    ASSERT_TRUE(image.save(&file, "PNG")); file.close();
    const auto icon = kit::app_icon_pixmap(path.toStdString(), 64);
    ASSERT_FALSE(icon.isNull());
    EXPECT_EQ(icon.size(), QSize(64, 32));
    EXPECT_EQ(icon.toImage().pixelColor(0, 0), QColor("#123456"));
    for (const auto* invalid : {"", "relative.png", "https://example.org/app.png", "/dev/null", "/missing/app.png"}) {
        EXPECT_TRUE(kit::app_icon_pixmap(invalid, 64).isNull());
    }
    EXPECT_TRUE(kit::app_icon_pixmap(path.toStdString() + std::string("\0.png", 5), 64).isNull());
    EXPECT_TRUE(kit::app_icon_pixmap(path.toStdString(), 513).isNull());
    const auto link = directory.filePath("link.png");
    std::filesystem::create_symlink(path.toStdString(), link.toStdString());
    EXPECT_TRUE(kit::app_icon_pixmap(link.toStdString(), 64).isNull());
    QImage oversized(1025, 1, QImage::Format_ARGB32); oversized.fill(Qt::black);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_TRUE(oversized.save(&file, "PNG")); file.close();
    EXPECT_TRUE(kit::app_icon_pixmap(path.toStdString(), 64).isNull());
    std::filesystem::resize_file(path.toStdString(), 1024 * 1024 + 1);
    EXPECT_TRUE(kit::app_icon_pixmap(path.toStdString(), 64).isNull());
}

TEST(QtKitDocument, TextIsLiteralAndOnlyHttpsAnchorsSurvive) {
    const std::array<kit::DocumentBlock, 1> blocks{{{kit::BlockKind::Paragraph, 1, {}, {
        {"<b>x</b>", false, false, false, "javascript:alert(1)"},
        {"<img src='file:///etc/passwd'>", false, false, false, "file:///etc/passwd"},
        {"http", false, false, false, "http://example.org"},
        {"https", true, true, false, "https://example.org/path?a=1&b=2"}
    }, {}}}};
    kit::DocumentView view(blocks, "Copy code");
    EXPECT_TRUE(view.plain_text().contains("<b>x</b>"));
    EXPECT_TRUE(view.plain_text().contains("<img src='file:///etc/passwd'>"));
    const auto browsers = view.findChildren<QTextBrowser*>();
    ASSERT_EQ(browsers.size(), 1);
    EXPECT_FALSE(browsers.front()->openLinks());
    EXPECT_FALSE(browsers.front()->openExternalLinks());
    int anchors = 0;
    for (auto block = browsers.front()->document()->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.charFormat().isAnchor()) {
                ++anchors;
                EXPECT_EQ(fragment.text(), "https");
                EXPECT_EQ(fragment.charFormat().anchorHref(), "https://example.org/path?a=1&b=2");
            }
        }
    }
    EXPECT_EQ(anchors, 1);
    QSignalSpy links(&view, &kit::DocumentView::link_activated);
    browsers.front()->anchorClicked(QUrl("file:///etc/passwd"));
    EXPECT_EQ(links.count(), 0);
    browsers.front()->anchorClicked(QUrl("https://example.org"));
    EXPECT_EQ(links.count(), 1);
}

TEST(QtKitDocument, BoundsLinksAndCodeCopy) {
    for (std::string_view link : {"javascript:alert(1)", "http://example.org", "https://", "https://user:secret@example.org",
            "https://@example.org", "https://example.org/\nfoo", "https://example.org\\evil"}) {
        EXPECT_FALSE(kit::allowed_link(link));
    }
    EXPECT_TRUE(kit::allowed_link("https://wiki.archlinux.org/title/Krita"));
    EXPECT_THROW(kit::safe_text(std::string(65537, 'x')), std::length_error);
    EXPECT_EQ(kit::safe_text(std::string("a\0b", 3)), QString("a�b"));
    std::vector<kit::DocumentBlock> large(257);
    EXPECT_THROW((kit::DocumentView(large, "Copy")), std::length_error);
    const std::array<kit::DocumentBlock, 1> code{{{kit::BlockKind::Code, 1, "<b>literal & code</b>", {}, {}}}};
    kit::DocumentView view(code, "Copy code");
    const auto buttons = view.findChildren<QPushButton*>();
    ASSERT_EQ(buttons.size(), 1);
    buttons.front()->click();
    EXPECT_EQ(QApplication::clipboard()->text(), "<b>literal & code</b>");
    EXPECT_EQ(view.plain_text(), "<b>literal & code</b>");
}

TEST(QtKitDocument, ManyBlocksUseOneBrowserAndPreserveBlockFormatting) {
    std::vector<kit::DocumentBlock> blocks(256);
    for (auto& block : blocks) { block.kind = kit::BlockKind::Paragraph; block.runs = {{"literal <b>text</b>", false, false, false, {}}}; }
    blocks.front() = {kit::BlockKind::Heading, 2, "Heading", {}, {}};
    blocks[1] = {kit::BlockKind::List, 1, {}, {}, {{{"First", false, false, false, {}}}, {{"Second", false, false, false, {}}}}};
    blocks[2] = {kit::BlockKind::Code, 1, "sample code", {}, {}};
    kit::DocumentView view(blocks, "Copy code");
    const auto browsers = view.findChildren<QTextBrowser*>();
    ASSERT_EQ(browsers.size(), 1);
    EXPECT_EQ(browsers.front()->document()->begin().blockFormat().headingLevel(), 2);
    EXPECT_GT(browsers.front()->heightForWidth(480), 4000);
    EXPECT_EQ(view.plain_text().count("literal <b>text</b>"), 253);
    EXPECT_TRUE(view.plain_text().contains("First\nSecond"));
    EXPECT_TRUE(view.plain_text().contains("sample code"));
    EXPECT_EQ(view.findChildren<QPushButton*>().size(), 1);
    EXPECT_FALSE(browsers.front()->document()->resource(QTextDocument::ImageResource, QUrl("https://example.org/image.png")).isValid());
}

TEST(QtKitPassword, ClearRemovesTextUndoAndRevealWithoutEmittingSecret) {
    kit::PasswordField password("Password for alex", "Caps Lock is on.");
    EXPECT_EQ(password.entry().echoMode(), QLineEdit::Password);
    password.entry().setText("sensitive-value");
    password.reveal_button().click();
    EXPECT_EQ(password.entry().echoMode(), QLineEdit::Normal);
    QSignalSpy changes(&password.entry(), &QLineEdit::textChanged);
    password.clear();
    EXPECT_TRUE(password.entry().text().isEmpty());
    EXPECT_FALSE(password.entry().isUndoAvailable());
    EXPECT_FALSE(password.reveal_button().isChecked());
    EXPECT_EQ(password.entry().echoMode(), QLineEdit::Password);
    EXPECT_EQ(changes.count(), 0);
    EXPECT_FALSE(password.entry().accessibleName().isEmpty());
    EXPECT_FALSE(password.entry().accessibleDescription().isEmpty());
}

const std::array<kit::NavigationItem, 2> navigation{{
    {"discover", "Discover", "braurser-discover-symbolic", {}, "Discover"},
    {"updates", "Updates", "braurser-updates-symbolic", 12, "Updates, 12 available"}
}};

TEST(QtKitNavigation, SelectAndActivateAreExclusiveAndAccessible) {
    std::string activated;
    kit::NavigationSidebar sidebar(navigation, [&activated](const std::string& id) { activated = id; });
    sidebar.select("discover");
    EXPECT_EQ(sidebar.selected(), "discover");
    sidebar.select("updates");
    EXPECT_EQ(sidebar.selected(), "updates");
    EXPECT_TRUE(activated.empty());
    sidebar.select("unknown");
    EXPECT_EQ(sidebar.selected(), "updates");
    const auto buttons = sidebar.findChildren<QPushButton*>();
    ASSERT_EQ(buttons.size(), 2);
    for (auto* button : buttons) {
        EXPECT_GE(button->minimumHeight(), 44);
        EXPECT_TRUE(button->isCheckable());
        EXPECT_FALSE(button->accessibleName().isEmpty());
        EXPECT_FALSE(button->accessibleDescription().isEmpty());
    }
    buttons.front()->click();
    EXPECT_EQ(activated, "discover");
    EXPECT_EQ(sidebar.selected(), "discover");
}

TEST(QtKitCode, AcceptsLargeReviewTextAndShowsHiddenFormatting) {
    const std::string large(200 * 1024, 'x');
    EXPECT_EQ(kit::safe_code(large), QString::fromStdString(large));
    EXPECT_EQ(kit::safe_code(std::string(448 * 1024, 'y')).size(), 448 * 1024);
    EXPECT_THROW(kit::safe_code(std::string(448 * 1024 + 1, 'x')), std::length_error);
    EXPECT_EQ(kit::safe_code("a\u202Eb\u2066c\u2069d\u200Be\uFEFFf\u061Cg"),
              QString::fromUtf8("a\uFFFDb\uFFFDc\uFFFDd\uFFFDe\uFFFDf\uFFFDg"));
    EXPECT_EQ(kit::safe_code(std::string("x\0y\x1b", 4)), QString::fromUtf8("x\uFFFDy\uFFFD"));
    const std::string script = "echo \u05E9\u05DC\u05D5\u05DD \u0645\u0631\u062D\u0628\u0627\n\tdone\r\n";
    EXPECT_EQ(kit::safe_code(script), QString::fromStdString(script));
    EXPECT_EQ(kit::safe_text("a\u202Eb"), QString::fromUtf8("a\u202Eb"));
}

TEST(QtKitShell, BreakpointsAndMaximumContentWidth) {
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->addWidget(new QLabel("Content"));
    kit::ResponsiveShell shell(*content, navigation, "Navigation", {});
    shell.show();
    for (const auto width : {839, 840, 1080, 2000, 360}) {
        shell.resize(width, 700);
        QApplication::processEvents();
        EXPECT_EQ(shell.width(), width);
        EXPECT_EQ(shell.sidebar_visible(), width >= 840) << width;
        const int inset = width < 480 ? 16 : 24;
        EXPECT_EQ(shell.content_width(), std::min(1200, width - 2 * inset - (width >= 840 ? 240 : 0))) << width;
    }
}

TEST(QtKitControls, BusyIndicatorIsStaticAndTextScaleGrowsCards) {
    kit::ThemeManager manager;
    manager.set_theme(kit::Theme::Light);
    kit::ProgressPanel panel({"Building", "Building. Amount not available.", {}, {}, "Cancel after this step", true}, {});
    panel.resize(320, 140);
    panel.show();
    QApplication::processEvents();
    if (auto* focused = QApplication::focusWidget()) { focused->clearFocus(); }
    QApplication::processEvents();
    const auto first = panel.grab().toImage();
    QTest::qWait(180);
    EXPECT_EQ(panel.grab().toImage(), first);
    const auto original_font = QApplication::font();
    kit::AppCard normal("A long application title", "A deliberately long summary that wraps onto more than two lines at a narrow width.",
        {kit::SourceKind::Official, "Official", "Distribution package"}, "Open application details");
    normal.ensurePolished();
    const auto normal_height = normal.heightForWidth(280);
    auto large_font = original_font;
    if (large_font.pointSizeF() > 0) { large_font.setPointSizeF(large_font.pointSizeF() * 2); }
    else { large_font.setPixelSize(large_font.pixelSize() * 2); }
    QApplication::setFont(large_font);
    manager.set_theme(kit::Theme::Light);
    kit::AppCard large("A long application title", "A deliberately long summary that wraps onto more than two lines at a narrow width.",
        {kit::SourceKind::Official, "Official", "Distribution package"}, "Open application details");
    large.ensurePolished();
    EXPECT_GT(large.heightForWidth(280), normal_height);
    QApplication::setFont(original_font);
    manager.set_theme(kit::Theme::Light);
}

TEST(QtKitControls, SemanticForegroundsRetainContrast) {
    kit::ThemeManager manager;
    for (const auto mode : {kit::Theme::Light, kit::Theme::Dark}) {
        manager.set_theme(mode);
        const std::array<kit::Action, 1> actions{{{"Try again", {}, true, true}}};
        kit::Banner banner(kit::Severity::Warning, "Warning", "A factual notice.", actions);
        banner.resize(360, 240);
        banner.show();
        QApplication::processEvents();
        auto* button = banner.findChild<QPushButton*>();
        ASSERT_NE(button, nullptr);
        auto* caption = button->findChild<QLabel*>();
        ASSERT_NE(caption, nullptr);
        EXPECT_EQ(caption->palette().color(QPalette::WindowText), expected("br_on_accent", mode));
        button->setEnabled(false);
        QApplication::processEvents();
        EXPECT_EQ(caption->palette().color(QPalette::Disabled, QPalette::WindowText), expected("br_muted", mode));
        EXPECT_EQ(button->grab().toImage().pixelColor(4, button->height() / 2), expected("br_subtle", mode));
        EXPECT_EQ(button->grab().toImage().pixelColor(0, button->height() / 2), expected("br_line", mode));
        button->setEnabled(true);
        QApplication::processEvents();
        EXPECT_EQ(caption->palette().color(QPalette::WindowText), expected("br_on_accent", mode));
        kit::FilterChip chip("Installed", "Remove Installed", true);
        chip.show();
        QApplication::processEvents();
        auto* chip_caption = chip.toggle().findChild<QLabel*>();
        ASSERT_NE(chip_caption, nullptr);
        EXPECT_EQ(chip_caption->palette().color(QPalette::WindowText), expected("br_on_selected", mode));
    }
}

TEST(QtKitControls, ChipsDisclosureProgressAndBounds) {
    kit::FilterChip chip("Installed", "Remove filter: Installed", true);
    EXPECT_TRUE(chip.toggle().isChecked());
    EXPECT_GE(chip.remove_button().minimumWidth(), 44);
    EXPECT_GE(chip.remove_button().minimumHeight(), 44);
    chip.toggle().click();
    EXPECT_FALSE(chip.toggle().isChecked());
    auto* child = new QLabel("Content");
    kit::Disclosure disclosure("Details", *child, "Show details", "Hide details");
    EXPECT_FALSE(disclosure.expanded());
    EXPECT_TRUE(child->isHidden());
    disclosure.toggle().click();
    EXPECT_TRUE(disclosure.expanded());
    EXPECT_FALSE(child->isHidden());
    EXPECT_EQ(disclosure.toggle().accessibleName(), "Hide details");
    kit::ProgressPanel panel({"Building", "Building. Amount not available.", {}, {}, "Cancel after this step", true}, {});
    auto* progress = panel.findChild<QProgressBar*>();
    ASSERT_NE(progress, nullptr);
    EXPECT_EQ(progress->maximum(), 0);
    panel.update({"Downloading", "Downloading", 0.48, {}, "Cancel", true});
    EXPECT_EQ(progress->value(), 480);
    EXPECT_THROW(panel.update({"Bad", "Bad", std::numeric_limits<double>::quiet_NaN(), {}, "Cancel", true}), std::invalid_argument);
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
