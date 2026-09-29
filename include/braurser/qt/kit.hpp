#ifndef BRAURSER_QT_KIT_HPP
#define BRAURSER_QT_KIT_HPP

#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QToolButton>
#include <QDBusVariant>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::qt::kit {

enum class Theme { System, Light, Dark };
enum class SourceKind { Official, Community, Git, Outside, Foreign };
enum class ImpactKind { Reboot, Logout, RestartApp, RestartService, None, Unknown };
enum class Severity { Info, Success, Warning, Error };

void ensure_resources();
QString safe_text(std::string_view text);
/// in: script or build text of at most 256 KiB
/// out: text with controls, bidi and invisible format characters shown as U+FFFD
/// err: throws std::length_error above 256 KiB
QString safe_code(std::string_view text);
bool allowed_link(std::string_view href);
QColor token_color(std::string_view name, Theme theme);
QPalette theme_palette(Theme theme);
QString theme_stylesheet(Theme theme);
QStringList icon_names();
QPixmap icon_pixmap(std::string_view name, int size, const QColor& color);
/// in: absolute local PNG path, at most 1 MiB and 1024 pixels per side; size 1 through 512
/// out: scaled pixmap or empty for unavailable or invalid input
QPixmap app_icon_pixmap(std::string_view path, int size);
QIcon symbolic_icon(std::string_view name, QPalette::ColorRole role = QPalette::WindowText);

class ThemeManager final : public QObject {
    Q_OBJECT
public:
    explicit ThemeManager(QObject* parent = nullptr);
    void manage(QWidget& window);
    void set_theme(Theme theme);
    Theme resolved() const;
protected:
    bool eventFilter(QObject* object, QEvent* event) override;
private Q_SLOTS:
    void portal_changed(const QString& group, const QString& key, const QDBusVariant& value);
private:
    void apply();
    void read_portal();
    Theme choice_ = Theme::System;
    Theme resolved_ = Theme::Light;
    unsigned portal_scheme_ = 0;
    bool applying_ = false;
    QPalette platform_palette_;
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

class SourceBadge final : public QFrame {
public:
    explicit SourceBadge(const Source& source, QWidget* parent = nullptr);
};

class ImpactBadge final : public QFrame {
public:
    ImpactBadge(ImpactKind kind, std::string_view label, std::string_view explanation, QWidget* parent = nullptr);
};

class AppCard final : public QPushButton {
public:
    AppCard(std::string_view title, std::string_view summary, const Source& source,
            std::string_view accessible_name, std::string_view icon = {}, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int heightForWidth(int width) const override;
};

class PackageRow final : public QFrame {
public:
    PackageRow(std::string_view name, std::string_view summary, const Source& source,
               std::string_view installation_state, std::string_view accessible_name,
               std::span<const Action> actions = {}, QWidget* parent = nullptr);
};

struct Fact { std::string label; std::string value; };

class TrustGrid final : public QWidget {
public:
    TrustGrid(std::span<const Fact> facts, std::string_view notice = {}, QWidget* parent = nullptr);
};

class FilterChip final : public QWidget {
public:
    FilterChip(std::string_view text, std::string_view remove_name, bool selected, QWidget* parent = nullptr);
    QPushButton& toggle();
    QToolButton& remove_button();
private:
    QPushButton* toggle_;
    QToolButton* remove_;
};

class Banner final : public QFrame {
public:
    Banner(Severity severity, std::string_view heading, std::string_view body,
           std::span<const Action> actions = {}, QWidget* parent = nullptr);
};

class Disclosure final : public QWidget {
public:
    /// in: Child ownership transfers to this widget.
    Disclosure(std::string_view title, QWidget& child, std::string_view show_name,
               std::string_view hide_name, bool expanded = false, QWidget* parent = nullptr);
    void set_expanded(bool expanded);
    bool expanded() const;
    QToolButton& toggle();
private:
    QToolButton* toggle_;
};

struct ProgressState {
    std::string phase;
    std::string accessible_name;
    std::optional<double> fraction;
    std::vector<std::string> steps;
    std::string cancel_label;
    bool cancel_enabled = true;
};

class ProgressPanel final : public QWidget {
public:
    explicit ProgressPanel(const ProgressState& state, std::function<void()> cancel, QWidget* parent = nullptr);
    void update(const ProgressState& state);
private:
    QLabel* phase_;
    QProgressBar* progress_;
    QLabel* steps_;
    QPushButton* cancel_;
};

class PasswordField final : public QWidget {
public:
    PasswordField(std::string_view label, std::string_view caps_warning, QWidget* parent = nullptr);
    ~PasswordField() override;
    QLineEdit& entry();
    QToolButton& reveal_button();
    void set_caps_lock(bool active);
    void clear();
private:
    QLineEdit* entry_;
    QToolButton* reveal_;
    QLabel* warning_;
};

class EmptyState final : public QWidget {
public:
    EmptyState(std::string_view icon, std::string_view title, std::string_view body,
               const std::optional<Action>& action = std::nullopt, QWidget* parent = nullptr);
};

struct NavigationItem {
    std::string id;
    std::string label;
    std::string icon;
    std::optional<unsigned> count;
    std::string accessible_name;
};

class NavigationSidebar final : public QWidget {
public:
    NavigationSidebar(std::span<const NavigationItem> items,
                      std::function<void(const std::string&)> activate, QWidget* parent = nullptr);
    void select(std::string_view id);
    std::string selected() const;
private:
    std::vector<std::pair<std::string, QPushButton*>> buttons_;
};

class ResponsiveShell final : public QWidget {
public:
    /// in: Content ownership transfers to this widget.
    ResponsiveShell(QWidget& content, std::span<const NavigationItem> items,
                    std::string_view navigation_name,
                    std::function<void(const std::string&)> activate, QWidget* parent = nullptr);
    void select(std::string_view id);
    bool sidebar_visible() const;
    int content_width() const;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
protected:
    void resizeEvent(QResizeEvent* event) override;
private:
    NavigationSidebar* sidebar_;
    NavigationSidebar* drawer_;
    QWidget* body_;
    QWidget* content_;
    QToolButton* menu_;
    QFrame* popover_;
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

class DocumentView final : public QWidget {
    Q_OBJECT
public:
    DocumentView(std::span<const DocumentBlock> document, std::string_view copy_label, QWidget* parent = nullptr);
    QString plain_text() const;
Q_SIGNALS:
    void link_activated(const QUrl& url);
private:
    std::vector<QTextBrowser*> browsers_;
};

}

#endif
