#ifndef BRAURSER_QT_INTERNAL_HPP
#define BRAURSER_QT_INTERNAL_HPP

#include <braurser/qt/kit.hpp>
#include <QBoxLayout>
#include <QPainter>
#include <QStyle>
#include <QTextLayout>
#include <stdexcept>

namespace braurser::qt::kit::detail {

inline void name(QWidget& widget, std::string_view text, std::string_view description = {}) {
    widget.setAccessibleName(safe_text(text));
    widget.setAccessibleDescription(safe_text(description.empty() ? text : description));
}

inline QLabel* label(std::string_view text, QWidget* parent = nullptr, bool selectable = false) {
    auto* widget = new QLabel(safe_text(text), parent);
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(true);
    widget->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    widget->setMinimumWidth(0);
    widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    if (selectable) {
        widget->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        name(*widget, text);
    }
    return widget;
}

inline void target(QWidget& widget, std::string_view text, std::string_view description = {}) {
    widget.setMinimumSize(44, 44);
    widget.setFocusPolicy(Qt::StrongFocus);
    name(widget, text, description);
}

class IconLabel final : public QWidget {
public:
    IconLabel(std::string_view name, int size, QWidget* parent = nullptr, QPalette::ColorRole role = QPalette::WindowText)
        : QWidget(parent), name_(name), size_(size), role_(role) {
        static_cast<void>(symbolic_icon(name, role));
        setFixedSize(size, size);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const auto* button = qobject_cast<QAbstractButton*>(parentWidget());
        const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        const auto role = button && button->isChecked() ? QPalette::HighlightedText : role_;
        const auto colors = parentWidget() ? parentWidget()->palette() : palette();
        painter.drawPixmap(rect(), icon_pixmap(name_, size_, colors.color(group, role)));
    }
private:
    std::string name_;
    int size_;
    QPalette::ColorRole role_;
};

class ProgressBar final : public QProgressBar {
public:
    explicit ProgressBar(QWidget* parent) : QProgressBar(parent) {}
protected:
    void paintEvent(QPaintEvent* event) override {
        if (minimum() != 0 || maximum() != 0) { QProgressBar::paintEvent(event); return; }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().brush(QPalette::AlternateBase));
        painter.drawRoundedRect(rect(), 3, 3);
        painter.setBrush(palette().brush(QPalette::Accent));
        for (const int offset : {-12, 0, 12}) {
            painter.drawEllipse(QPointF(width() / 2.0 + offset, height() / 2.0), 3, 3);
        }
    }
};

class NavigationButton final : public QPushButton {
public:
    explicit NavigationButton(QWidget* parent) : QPushButton(parent) {
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    QSize sizeHint() const override { return layout()->sizeHint().expandedTo(QSize(44, 44)); }
    QSize minimumSizeHint() const override { return {44, 44}; }
    int heightForWidth(int width) const override { return std::max(44, layout()->totalHeightForWidth(width)); }
protected:
    void paintEvent(QPaintEvent* event) override {
        QPushButton::paintEvent(event);
        if (isChecked()) {
            QPainter painter(this);
            const int x = layoutDirection() == Qt::RightToLeft ? width() - 5 : 2;
            painter.fillRect(x, 12, 3, std::max(1, height() - 24), palette().brush(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::HighlightedText));
        }
    }
};

class SummaryLabel final : public QLabel {
public:
    SummaryLabel(std::string_view text, QWidget* parent) : QLabel(safe_text(text), parent) {
        setTextFormat(Qt::PlainText);
        setWordWrap(true);
        setMinimumWidth(0);
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setProperty("brRole", "secondary");
    }
    int heightForWidth(int width) const override {
        return std::min(2 * fontMetrics().lineSpacing(), QLabel::heightForWidth(width));
    }
    QSize sizeHint() const override { return {200, heightForWidth(200)}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().lineSpacing()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText));
        QTextLayout text_layout(text(), font());
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        text_layout.setTextOption(option);
        text_layout.beginLayout();
        qreal y = 0;
        for (int index = 0; index < 2; ++index) {
            auto line = text_layout.createLine();
            if (!line.isValid()) { break; }
            line.setLineWidth(width());
            if (index == 1 && line.textStart() + line.textLength() < text().size()) {
                const auto remaining = text().mid(line.textStart()).replace('\n', ' ');
                painter.drawText(QPointF(0, y + fontMetrics().ascent()), fontMetrics().elidedText(remaining, Qt::ElideRight, width()));
            } else { line.draw(&painter, QPointF(0, y)); }
            y += line.height();
        }
        text_layout.endLayout();
    }
};

class ActionButton final : public QPushButton {
public:
    explicit ActionButton(const Action& action, QWidget* parent = nullptr) : QPushButton(parent) {
        auto* box = new QVBoxLayout(this);
        box->setContentsMargins(12, 8, 12, 8);
        auto* text = label(action.label, this);
        text->setAlignment(Qt::AlignCenter);
        text->setProperty("brRole", "actionLabel");
        connect(this, &QPushButton::toggled, this, [text](bool selected) {
            text->setProperty("brSelected", selected);
            text->style()->unpolish(text);
            text->style()->polish(text);
            text->update();
        });
        text->setAttribute(Qt::WA_TransparentForMouseEvents);
        box->addWidget(text);
        target(*this, action.label);
        setEnabled(action.enabled);
        if (action.primary) { setProperty("brRole", "primary"); text->setProperty("brRole", "primaryLabel"); }
        if (action.activate) { connect(this, &QPushButton::clicked, this, action.activate); }
        auto policy = sizePolicy();
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    QSize sizeHint() const override { return layout()->sizeHint().expandedTo(QSize(44, 44)); }
    QSize minimumSizeHint() const override { return {44, 44}; }
    int heightForWidth(int width) const override { return std::max(44, layout()->totalHeightForWidth(width)); }
};

inline void actions(QVBoxLayout& layout, std::span<const Action> values) {
    if (values.size() > 2) { throw std::length_error("at most two actions"); }
    for (const auto& action : values) { layout.addWidget(new ActionButton(action)); }
}

inline QVBoxLayout* column(QWidget* parent, int inset = 0) {
    auto* layout = new QVBoxLayout(parent);
    layout->setSpacing(8);
    layout->setContentsMargins(inset, inset, inset, inset);
    return layout;
}

}

#endif
