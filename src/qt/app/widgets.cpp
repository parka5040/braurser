#include <braurser/qt/app.hpp>
#include <braurser/qt/internal.hpp>
#include <QAbstractItemView>
#include <QFontDatabase>
#include <QPlainTextEdit>
#include <type_traits>

namespace braurser::qt {

QLabel* label(QVBoxLayout& box, std::string_view value, const char* role) {
    auto* result = new QLabel(kit::safe_text(value), box.parentWidget());
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    result->setAccessibleName(kit::safe_text(value));
    if (role) { result->setProperty("brRole", role); }
    box.addWidget(result);
    return result;
}

QPushButton* button(QVBoxLayout& box, std::string_view value, std::function<void()> action, bool enabled, bool primary) {
    auto* result = new kit::detail::ActionButton({std::string(value), std::move(action), enabled, primary}, box.parentWidget());
    result->setAutoDefault(false);
    result->setDefault(false);
    box.addWidget(result);
    return result;
}

QLineEdit* entry(QVBoxLayout& box, std::string_view name, std::string_view value, const char* id) {
    auto* result = new QLineEdit(box.parentWidget());
    result->setObjectName(id);
    result->setMinimumHeight(44);
    result->setMaxLength(1024);
    result->setPlaceholderText(kit::safe_text(name));
    result->setAccessibleName(kit::safe_text(name));
    result->setText(kit::safe_text(value));
    result->setClearButtonEnabled(true);
    box.addWidget(result);
    return result;
}

QCheckBox* check(QVBoxLayout& box, std::string_view name, bool value, std::function<void(bool)> action) {
    auto* row = new QWidget(box.parentWidget());
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto* result = new QCheckBox(row);
    result->setFixedWidth(44);
    result->setMinimumHeight(44);
    result->setLayoutDirection(Qt::RightToLeft);
    result->setChecked(value);
    result->setAccessibleName(kit::safe_text(name));
    layout->addWidget(result);
    auto* caption = new QLabel(kit::safe_text(name), row);
    caption->setTextFormat(Qt::PlainText);
    caption->setWordWrap(true);
    caption->setBuddy(result);
    caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    layout->addWidget(caption, 1);
    box.addWidget(row);
    QObject::connect(result, &QCheckBox::toggled, result, std::move(action));
    return result;
}

QComboBox* combo(QVBoxLayout& box, std::string_view name, const std::vector<std::pair<std::string, std::string>>& values,
                 std::string_view selected, std::function<void(std::string)> action) {
    auto* caption = label(box, name);
    auto* result = new QComboBox(box.parentWidget());
    result->setMinimumHeight(44);
    result->setAccessibleName(kit::safe_text(name));
    result->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    result->setMinimumContentsLength(5);
    caption->setBuddy(result);
    for (const auto& [id, title] : values) {
        result->addItem(kit::safe_text(title), kit::safe_text(id));
        if (id == selected) { result->setCurrentIndex(result->count() - 1); }
    }
    QObject::connect(result, &QComboBox::activated, result, [result, action = std::move(action)](int index) {
        action(result->itemData(index).toString().toStdString());
    });
    box.addWidget(result);
    return result;
}

QWidget* column(QWidget* parent) {
    auto* result = new QWidget(parent);
    auto* layout = new QVBoxLayout(result);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(12);
    return result;
}

kit::Disclosure* disclosure(QVBoxLayout& box, QWidget& child, std::string_view title, const presenter::Copy& copy, bool expanded) {
    auto* result = new kit::Disclosure(title, child,
        copy.text("a11y.reveal", {{"section", std::string(title)}}),
        copy.text("a11y.collapse", {{"section", std::string(title)}}), expanded, box.parentWidget());
    box.addWidget(result);
    return result;
}

namespace {
std::vector<kit::TextRun> runs(const std::vector<protocol::Inline>& inlines) {
    std::vector<kit::TextRun> result;
    for (const auto& item : inlines) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, protocol::WikiText>) {
                result.push_back({value.s, value.emphasis, value.strong, value.code, {}});
            } else { result.push_back({value.label, false, false, false, value.href}); }
        }, item.value);
    }
    return result;
}
}

std::vector<kit::DocumentBlock> document_blocks(const protocol::WikiDocument& document) {
    std::vector<kit::DocumentBlock> result;
    for (const auto& item : document.blocks) {
        kit::DocumentBlock block;
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, protocol::WikiHeading>) {
                block.kind = kit::BlockKind::Heading; block.level = value.level; block.text = value.text;
            } else if constexpr (std::is_same_v<T, protocol::WikiCode>) {
                block.kind = kit::BlockKind::Code; block.text = value.text;
            } else if constexpr (std::is_same_v<T, protocol::WikiList>) {
                block.kind = kit::BlockKind::List;
                for (const auto& list_item : value.items) { block.items.push_back(runs(list_item)); }
            } else {
                block.kind = std::is_same_v<T, protocol::WikiNote> ? kit::BlockKind::Note :
                    std::is_same_v<T, protocol::WikiWarning> ? kit::BlockKind::Warning : kit::BlockKind::Paragraph;
                block.runs = runs(value.inlines);
            }
        }, item.value);
        result.push_back(std::move(block));
    }
    return result;
}

}
