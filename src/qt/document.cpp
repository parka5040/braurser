#include <braurser/qt/kit.hpp>
#include <braurser/qt/internal.hpp>
#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QUrl>
#include <algorithm>
#include <stdexcept>

namespace braurser::qt::kit {
namespace {
constexpr std::size_t text_limit = 65536;
constexpr std::size_t document_limit = 1048576;

class DocumentBrowser final : public QTextBrowser {
public:
    explicit DocumentBrowser(QWidget* parent) : QTextBrowser(parent) {
        setProperty("brRole", "prose");
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setReadOnly(true);
        setMinimumWidth(0);
        setMaximumWidth(std::max(280, fontMetrics().averageCharWidth() * 72 + 28));
        setFrameShape(QFrame::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setAccessibleName("Document text");
        setAccessibleDescription("Selectable article text. Activate an HTTPS link to request opening it.");
    }
    QVariant loadResource(int, const QUrl&) override { return {}; }
    int heightForWidth(int width) const override {
        const int inset = property("brRole") == "prose" ? 0 : 28;
        document()->setTextWidth(std::max(1, width - inset));
        return std::clamp(static_cast<int>(std::ceil(document()->size().height())) + inset, 44, 4000);
    }
    QSize sizeHint() const override { return {480, heightForWidth(480)}; }
    QSize minimumSizeHint() const override { return {0, 44}; }
protected:
    void changeEvent(QEvent* event) override {
        QTextBrowser::changeEvent(event);
        if (event->type() == QEvent::PaletteChange) {
            std::vector<std::pair<int, int>> anchors;
            for (auto block = document()->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (fragment.charFormat().isAnchor()) { anchors.emplace_back(fragment.position(), fragment.length()); }
                }
            }
            for (const auto& [position, length] : anchors) {
                QTextCursor cursor(document());
                cursor.setPosition(position);
                cursor.setPosition(position + length, QTextCursor::KeepAnchor);
                QTextCharFormat format;
                format.setForeground(palette().brush(QPalette::Link));
                cursor.mergeCharFormat(format);
            }
        }
    }
};

void insert_runs(QTextCursor& cursor, std::span<const TextRun> runs) {
    for (const auto& run : runs) {
        QTextCharFormat format;
        format.setFontItalic(run.emphasis);
        format.setFontWeight(run.strong ? QFont::DemiBold : QFont::Normal);
        if (run.code) { format.setFontFamilies({QFontDatabase::systemFont(QFontDatabase::FixedFont).family()}); }
        if (allowed_link(run.href)) {
            format.setAnchor(true);
            format.setAnchorHref(safe_text(run.href));
            format.setFontUnderline(true);
            format.setForeground(QApplication::palette().brush(QPalette::Link));
        }
        cursor.insertText(safe_text(run.text), format);
    }
}

void validate_document(std::span<const DocumentBlock> document) {
    if (document.size() > 256) { throw std::length_error("too many document blocks"); }
    std::size_t size = 0;
    const auto text = [&size](std::string_view value) {
        if (value.size() > text_limit || value.size() > document_limit - size) { throw std::length_error("document exceeds limit"); }
        size += value.size();
    };
    const auto runs = [&text](const auto& values) {
        if (values.size() > 1024) { throw std::length_error("too many document runs"); }
        for (const auto& run : values) { text(run.text); text(run.href); }
    };
    for (const auto& block : document) {
        text(block.text);
        runs(block.runs);
        if (block.items.size() > 256) { throw std::length_error("too many list items"); }
        for (const auto& item : block.items) { runs(item); }
        switch (block.kind) {
        case BlockKind::Heading:
            if (block.level < 1 || block.level > 6) { throw std::invalid_argument("invalid heading level"); }
            break;
        case BlockKind::Paragraph: case BlockKind::List: case BlockKind::Code: case BlockKind::Note: case BlockKind::Warning: break;
        default: throw std::invalid_argument("unknown document block");
        }
    }
}
}

QString safe_text(std::string_view text) {
    if (text.size() > text_limit) { throw std::length_error("kit text exceeds limit"); }
    auto result = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    for (auto& character : result) {
        const auto value = character.unicode();
        if ((value < 32 && value != '\n' && value != '\r' && value != '\t') || value == 127 || value == 0xfffe || value == 0xffff) {
            character = QChar::ReplacementCharacter;
        }
    }
    return result;
}

bool allowed_link(std::string_view href) {
    if (href.size() > 4096 || !href.starts_with("https://") || std::ranges::any_of(href, [](unsigned char c) {
            return c <= 32 || c == 127 || c == '\\';
        })) { return false; }
    const QByteArray bytes(href.data(), static_cast<qsizetype>(href.size()));
    const auto text = QString::fromUtf8(bytes);
    if (text.toUtf8() != bytes) { return false; }
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() &&
        url.userInfo().isEmpty() && !url.authority().contains('@');
}

DocumentView::DocumentView(std::span<const DocumentBlock> document, std::string_view copy_label, QWidget* parent) : QWidget(parent) {
    validate_document(document);
    auto* box = detail::column(this, 4);
    box->setSpacing(16);
    for (const auto& block : document) {
        auto* browser = new DocumentBrowser(this);
        browsers_.push_back(browser);
        connect(browser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
            const auto bytes = url.toEncoded();
            if (allowed_link(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())))) { Q_EMIT link_activated(url); }
        });
        auto* native_document = new QTextDocument(browser);
        native_document->setDefaultFont(QApplication::font());
        browser->setDocument(native_document);
        QTextCursor cursor(native_document);
        QTextBlockFormat paragraph;
        paragraph.setBottomMargin(8);
        cursor.setBlockFormat(paragraph);
        switch (block.kind) {
        case BlockKind::Heading: {
            QTextCharFormat format;
            format.setFontWeight(QFont::DemiBold);
            const double point_size = QApplication::font().pointSizeF();
            format.setFontPointSize((point_size > 0 ? point_size : 11.25) * (block.level == 1 ? 1.6 : 1.2));
            paragraph.setHeadingLevel(static_cast<int>(block.level));
            cursor.setBlockFormat(paragraph);
            cursor.insertText(safe_text(block.text), format);
            browser->setAccessibleName(safe_text(block.text));
            break;
        }
        case BlockKind::Paragraph:
            insert_runs(cursor, block.runs);
            break;
        case BlockKind::List: {
            QTextListFormat list_format;
            list_format.setStyle(QTextListFormat::ListDisc);
            auto* list = cursor.createList(list_format);
            bool first = true;
            for (const auto& item : block.items) {
                if (!first) { cursor.insertBlock(); list->add(cursor.block()); }
                insert_runs(cursor, item);
                first = false;
            }
            break;
        }
        case BlockKind::Code: {
            QTextCharFormat format;
            format.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            cursor.insertText(safe_text(block.text), format);
            browser->setProperty("brRole", "code");
            browser->setLineWrapMode(QTextEdit::NoWrap);
            browser->setAccessibleName("Code");
            browser->setAccessibleDescription("Selectable code. The adjacent Copy button copies this block.");
            break;
        }
        case BlockKind::Note:
        case BlockKind::Warning: {
            browser->setProperty("brRole", block.kind == BlockKind::Warning ? "warning" : "info");
            QTextCharFormat format;
            format.setFontWeight(QFont::DemiBold);
            cursor.insertText(safe_text(block.text), format);
            cursor.insertBlock();
            insert_runs(cursor, block.runs);
            browser->setAccessibleName(safe_text(block.text));
            break;
        }
        }
        cursor.movePosition(QTextCursor::Start);
        browser->setTextCursor(cursor);
        browser->ensureCursorVisible();
        box->addWidget(browser);
        if (block.kind == BlockKind::Code) {
            const auto text = safe_text(block.text);
            auto* copy = new detail::ActionButton(Action{std::string(copy_label), [text] {
                QApplication::clipboard()->setText(text);
            }, true, false}, this);
            box->addWidget(copy);
        }
    }
}

QString DocumentView::plain_text() const {
    QStringList parts;
    for (const auto* browser : browsers_) { parts.push_back(browser->toPlainText()); }
    return parts.join('\n');
}

}
