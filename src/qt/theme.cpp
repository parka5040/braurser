#include <braurser/qt/kit.hpp>
#include <theme_tokens.hpp>
#include <QApplication>
#include <QAccessibilityHints>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QIconEngine>
#include <QImageReader>
#include <QBuffer>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QPainter>
#include <QPixmapCache>
#include <QPointer>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStyleOption>
#include <QSvgRenderer>
#include <QTimer>
#include <algorithm>
#include <stdexcept>

static void register_qt_resources() {
    Q_INIT_RESOURCE(braurser_qt_kit);
}

namespace braurser::qt::kit {
namespace {
class FocusOverlay final : public QWidget {
public:
    explicit FocusOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::Accent), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 12, 12);
    }
};

class FocusStyle final : public QProxyStyle {
public:
    FocusStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {
        qApp->installEventFilter(this);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* current) {
            focused_ = current;
            refresh();
        });
    }
    ~FocusStyle() override { delete overlay_; }
    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override {
        if (element == PE_FrameFocusRect) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(QPen(option->palette.color(QPalette::Accent), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(option->rect).adjusted(-3, -3, 3, 3), 12, 12);
            painter->restore();
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (focused_ && (object == focused_ || object == focused_->window()) &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
             event->type() == QEvent::Show || event->type() == QEvent::Hide ||
             event->type() == QEvent::PaletteChange)) {
            QTimer::singleShot(0, this, [this] { refresh(); });
        }
        return false;
    }
private:
    void refresh() {
        if (!focused_ || !focused_->isVisible() || !focused_->isEnabled()) {
            if (overlay_) { overlay_->hide(); }
            return;
        }
        auto* window = focused_->window();
        if (!overlay_ || overlay_->parentWidget() != window) {
            delete overlay_;
            overlay_ = new FocusOverlay(window);
        }
        const QPoint position = focused_->mapTo(window, QPoint(0, 0));
        overlay_->setGeometry(QRect(position, focused_->size()).adjusted(-4, -4, 4, 4));
        overlay_->raise();
        overlay_->show();
        overlay_->update();
    }
    QPointer<QWidget> focused_;
    QPointer<FocusOverlay> overlay_;
};

QString resource_name(std::string_view name) {
    if (name.empty()) { name = "braurser-package-symbolic"; }
    if (name.size() > 128 || std::ranges::any_of(name, [](unsigned char c) {
            return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-');
        })) {
        throw std::invalid_argument("invalid icon name");
    }
    return ":/org/braurser/kit/icons/" + QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())) + ".svg";
}

class SymbolicEngine final : public QIconEngine {
public:
    SymbolicEngine(std::string name, QPalette::ColorRole role) : name_(std::move(name)), role_(role) {}
    QIconEngine* clone() const override { return new SymbolicEngine(name_, role_); }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        const auto palette = QApplication::palette();
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        const auto role = mode == QIcon::Selected || state == QIcon::On ? QPalette::HighlightedText : role_;
        return icon_pixmap(name_, std::clamp(std::min(size.width(), size.height()), 1, 512), palette.color(group, role));
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const auto image = pixmap(rect.size(), mode, state);
        painter->drawPixmap(rect.center() - QPoint(image.width() / 2, image.height() / 2), image);
    }
private:
    std::string name_;
    QPalette::ColorRole role_;
};

unsigned portal_value(QVariant value) {
    if (value.metaType() == QMetaType::fromType<QDBusVariant>()) { value = value.value<QDBusVariant>().variant(); }
    if (value.metaType().id() != QMetaType::UInt) { return 0; }
    const auto scheme = value.toUInt();
    return scheme <= 2 ? scheme : 0;
}
}

void ensure_resources() {
    static const bool registered = [] { register_qt_resources(); return true; }();
    static_cast<void>(registered);
}

QColor token_color(std::string_view name, Theme theme) {
    if (theme != Theme::Light && theme != Theme::Dark) { throw std::invalid_argument("unresolved theme"); }
    for (const auto& color : braurser::theme::kColors) {
        if (color.name == name) { return QColor::fromRgb(theme == Theme::Dark ? color.dark : color.light); }
    }
    throw std::invalid_argument("unknown color token");
}

QPalette theme_palette(Theme theme) {
    const std::pair<QPalette::ColorRole, std::string_view> roles[]{
        {QPalette::Window, "br_bg"}, {QPalette::WindowText, "br_text"}, {QPalette::Text, "br_text"},
        {QPalette::ButtonText, "br_text"}, {QPalette::Base, "br_surface"}, {QPalette::AlternateBase, "br_subtle"},
        {QPalette::Button, "br_surface"}, {QPalette::ToolTipBase, "br_surface"}, {QPalette::ToolTipText, "br_text"},
        {QPalette::PlaceholderText, "br_muted"}, {QPalette::Highlight, "br_selected"}, {QPalette::HighlightedText, "br_on_selected"},
        {QPalette::Link, "br_accent"}, {QPalette::LinkVisited, "br_accent"}, {QPalette::BrightText, "br_on_accent"},
        {QPalette::Light, "br_surface"}, {QPalette::Midlight, "br_subtle"}, {QPalette::Mid, "br_line"},
        {QPalette::Dark, "br_separator"}, {QPalette::Shadow, "br_separator"}, {QPalette::Accent, "br_accent"}
    };
    QPalette result;
    for (const auto& [role, token] : roles) {
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            result.setColor(group, role, token_color(token, theme));
        }
    }
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::ToolTipText,
                      QPalette::PlaceholderText, QPalette::HighlightedText, QPalette::Link, QPalette::LinkVisited, QPalette::BrightText}) {
        result.setColor(QPalette::Disabled, role, token_color("br_muted", theme));
    }
    for (auto role : {QPalette::Window, QPalette::Base, QPalette::AlternateBase, QPalette::Button,
                      QPalette::ToolTipBase, QPalette::Highlight, QPalette::Light, QPalette::Midlight}) {
        result.setColor(QPalette::Disabled, role, token_color("br_subtle", theme));
    }
    return result;
}

QString theme_stylesheet(Theme theme) {
    if (theme != Theme::Light && theme != Theme::Dark) { throw std::invalid_argument("unresolved theme"); }
    ensure_resources();
    QFile file(theme == Theme::Dark ? ":/org/braurser/kit/braurser-dark.qss" : ":/org/braurser/kit/braurser-light.qss");
    if (!file.open(QIODevice::ReadOnly) || file.size() > 131072) { throw std::runtime_error("theme resource unavailable"); }
    QString result = QString::fromUtf8(file.readAll());
    if (result.contains("${")) { throw std::runtime_error("unresolved theme placeholder"); }
    result.remove(QRegularExpression("font-family\\s*:[^;]*;"));
    const auto focus_rules = result.indexOf("\nQPushButton:focus,");
    if (focus_rules >= 0) { result.truncate(focus_rules); }
    const auto font = QApplication::font();
    const double pixels = font.pixelSize() > 0 ? font.pixelSize() : font.pointSizeF() * 96.0 / 72.0;
    const double scale = std::clamp(pixels / 15.0, 0.5, 4.0);
    const QRegularExpression sizes("font-size: ([0-9]+)px");
    auto matches = sizes.globalMatch(result);
    std::vector<std::pair<QString, QString>> replacements;
    while (matches.hasNext()) {
        const auto match = matches.next();
        replacements.emplace_back(match.captured(), "font-size: " + QString::number(match.captured(1).toDouble() * scale, 'f', 2) + "px");
    }
    for (const auto& [before, after] : replacements) { result.replace(before, after); }
    const auto color = [theme](std::string_view name) { return token_color(name, theme).name(); };
    result += QStringLiteral("\nQPushButton[brRole=\"navigation\"] { border: none; border-radius: 6px; background: transparent; padding: 0; text-align: left; }\n") +
        "QPushButton[brRole=\"navigation\"]:hover { background: " + color("br_hover") + "; }\n" +
        "QPushButton[brRole=\"navigation\"]:pressed { background: " + color("br_pressed") + "; }\n" +
        "QPushButton[brRole=\"navigation\"]:checked { background: " + color("br_selected") + "; color: " + color("br_on_selected") + "; }\n" +
        "QLabel[brSelected=\"true\"] { color: " + color("br_on_selected") + "; }\n" +
        "QPushButton QLabel[brRole=\"actionLabel\"] { color: " + color("br_text") + "; }\n" +
        "QPushButton[brRole=\"primary\"] QLabel[brRole=\"primaryLabel\"] { color: " + color("br_on_accent") + "; }\n" +
        "QPushButton QLabel[brRole=\"actionLabel\"]:disabled, QPushButton[brRole=\"primary\"] QLabel[brRole=\"primaryLabel\"]:disabled { color: " + color("br_muted") + "; }\n" +
        "QPushButton[brRole=\"chip\"] QLabel[brSelected=\"true\"] { color: " + color("br_on_selected") + "; }\n" +
        "QFrame[brRole=\"card\"] { padding: 0; }\n" +
        "QTextBrowser[brRole=\"prose\"] { background: transparent; border: none; padding: 0; }\n" +
        "QLabel[brRole=\"warning\"] { color: " + color("br_warning") + "; background: " + color("br_warning_bg") + "; }\n" +
        "QTextBrowser[brRole=\"warning\"] { color: " + color("br_warning") + "; background: " + color("br_warning_bg") + "; }\n" +
        "QTextBrowser[brRole=\"info\"] { color: " + color("br_info") + "; background: " + color("br_info_bg") + "; }\n" +
        "QPushButton[brRole=\"navigation\"]:disabled { background: " + color("br_subtle") + "; color: " + color("br_muted") + "; }\n" +
        "QPushButton[brRole=\"appCard\"] { background: " + color("br_surface") + "; border: 1px solid " + color("br_separator") + "; padding: 0; }\n" +
        "QPushButton[brRole=\"appCard\"]:hover { background: " + color("br_hover") + "; }\n" +
        "QPushButton[brRole=\"appCard\"]:pressed { background: " + color("br_pressed") + "; }\n" +
        "QPushButton[brRole=\"appCard\"]:disabled { background: " + color("br_subtle") + "; }\n" +
        "QToolButton[brRole=\"iconButton\"] { padding: 0; }\n";
    for (const auto* severity : {"info", "success", "warning", "error"}) {
        result += "QFrame[brRole=\"" + QString::fromLatin1(severity) + "\"] { color: " +
            color(std::string("br_") + severity) + "; }\n";
    }
    const QString mode = theme == Theme::Dark ? "dark" : "light";
    result += "QComboBox::down-arrow { image: url(:/org/braurser/kit/controls/chevron-" + mode + "-normal.svg); width: 20px; height: 20px; }\n";
    result += "QComboBox::down-arrow:disabled { image: url(:/org/braurser/kit/controls/chevron-" + mode + "-disabled.svg); }\n";
    return result;
}

QStringList icon_names() {
    ensure_resources();
    auto names = QDir(":/org/braurser/kit/icons").entryList({"*.svg"}, QDir::Files, QDir::Name);
    for (auto& name : names) { name.chop(4); }
    return names;
}

QPixmap icon_pixmap(std::string_view name, int size, const QColor& color) {
    ensure_resources();
    if (size < 1 || size > 512 || !color.isValid()) { throw std::invalid_argument("invalid icon dimensions or color"); }
    const auto path = resource_name(name);
    const auto key = path + ':' + QString::number(size) + ':' + color.name(QColor::HexArgb);
    QPixmap result;
    if (QPixmapCache::find(key, &result)) { return result; }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) { throw std::invalid_argument("unknown icon"); }
    auto bytes = file.readAll();
    bytes.replace("currentColor", "#000000");
    QSvgRenderer renderer(bytes);
    if (!renderer.isValid()) { throw std::runtime_error("invalid icon resource"); }
    result = QPixmap(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    renderer.render(&painter);
    if (name != "braurser") {
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(result.rect(), color);
    }
    painter.end();
    QPixmapCache::insert(key, result);
    return result;
}

QPixmap app_icon_pixmap(std::string_view path, int size) {
    if (size < 1 || size > 512 || path.empty() || path.size() > 4096 || path.front() != '/' || path.find('\0') != std::string_view::npos) { return {}; }
    const int fd = ::open(std::string(path).c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { return {}; }
    struct stat metadata{};
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); return {}; }
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 24 || metadata.st_size > 1024 * 1024) { return {}; }
    auto bytes = file.read(metadata.st_size);
    if (bytes.size() != metadata.st_size) { return {}; }
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) { return {}; }
    QImageReader reader(&buffer, "png");
    reader.setAutoDetectImageFormat(false);
    const auto dimensions = reader.size();
    if (!dimensions.isValid() || dimensions.isEmpty() || dimensions.width() > 1024 || dimensions.height() > 1024) { return {}; }
    reader.setScaledSize(dimensions.scaled(size, size, Qt::KeepAspectRatio));
    return QPixmap::fromImage(reader.read());
}

QIcon symbolic_icon(std::string_view name, QPalette::ColorRole role) {
    ensure_resources();
    if (!QFile::exists(resource_name(name))) { throw std::invalid_argument("unknown icon"); }
    return QIcon(new SymbolicEngine(std::string(name), role));
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent), platform_palette_(QApplication::palette()) {
    if (!qApp) { throw std::runtime_error("theme requires QApplication"); }
    ensure_resources();
    QApplication::setStyle(new FocusStyle);
    qApp->installEventFilter(this);
    if (QApplication::font().family().isEmpty()) { QApplication::setFont(QFont("Noto Sans")); }
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { if (!applying_) { apply(); } });
    connect(QGuiApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged,
        this, [this] { apply(); });
    QDBusConnection::sessionBus().connect("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "SettingChanged", this, SLOT(portal_changed(QString,QString,QDBusVariant)));
    read_portal();
    apply();
}

bool ThemeManager::eventFilter(QObject* object, QEvent* event) {
    if (object == qApp && !applying_) {
        if (event->type() == QEvent::ApplicationPaletteChange) {
            platform_palette_ = QApplication::palette();
            apply();
        } else if (event->type() == QEvent::ApplicationFontChange) { apply(); }
    }
    return false;
}

void ThemeManager::manage(QWidget& window) {
    window.setProperty("brRoot", true);
    window.setAutoFillBackground(true);
}

void ThemeManager::set_theme(Theme theme) {
    switch (theme) {
    case Theme::System: case Theme::Light: case Theme::Dark: choice_ = theme; apply(); return;
    }
    throw std::invalid_argument("unknown theme");
}

Theme ThemeManager::resolved() const { return resolved_; }

void ThemeManager::read_portal() {
    auto message = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings", "Read");
    message.setAutoStartService(false);
    message << "org.freedesktop.appearance" << "color-scheme";
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message, 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<QDBusVariant> reply = *watcher;
        portal_scheme_ = reply.isError() ? 0 : portal_value(reply.value().variant());
        watcher->deleteLater();
        apply();
    });
}

void ThemeManager::portal_changed(const QString& group, const QString& key, const QDBusVariant& value) {
    if (group == "org.freedesktop.appearance" && key == "color-scheme") {
        portal_scheme_ = portal_value(value.variant());
        apply();
    }
}

void ThemeManager::apply() {
    if (applying_) { return; }
    applying_ = true;
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    resolved_ = choice_;
    if (choice_ == Theme::System) {
        resolved_ = scheme == Qt::ColorScheme::Dark || (scheme == Qt::ColorScheme::Unknown && portal_scheme_ == 1)
            ? Theme::Dark : Theme::Light;
    }
    const bool high_contrast = QGuiApplication::styleHints()->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
    QApplication::setPalette(high_contrast ? platform_palette_ : theme_palette(resolved_));
    qApp->setStyleSheet(high_contrast ? QString() : theme_stylesheet(resolved_));
    applying_ = false;
}

}
