#include "dialog.hpp"
#include <braurser/qt/app.hpp>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <cstring>

namespace braurser::askpass {
namespace {
class Password final : public QLineEdit {
public:
    explicit Password(QWidget* parent) : QLineEdit(parent) {
        setEchoMode(QLineEdit::Password);
        setMaxLength(1024);
        setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
        setContextMenuPolicy(Qt::NoContextMenu);
        setDragEnabled(false);
        setMinimumHeight(44);
    }
    ~Password() override { wipe(); }
    void wipe() {
        const QSignalBlocker blocker(this);
        QString value = text();
        if (!value.isEmpty()) { ::explicit_bzero(const_cast<QChar*>(value.constData()), static_cast<std::size_t>(value.size()) * sizeof(QChar)); }
        clear();
        setEchoMode(QLineEdit::Password);
    }
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) { event->accept(); return; }
        QLineEdit::keyPressEvent(event);
    }
};

struct PasswordCopy {
    Password& field;
    QString text;
    QByteArray encoded;
    std::vector<char> bytes;
    ~PasswordCopy() {
        ::explicit_bzero(bytes.data(), bytes.size());
        ::explicit_bzero(encoded.data(), static_cast<std::size_t>(encoded.size()));
        if (!text.isEmpty()) { ::explicit_bzero(const_cast<QChar*>(text.constData()), static_cast<std::size_t>(text.size()) * sizeof(QChar)); }
        encoded.clear(); text.clear(); field.wipe();
    }
};
}

Dialog::Dialog(presenter::Auth& auth, const presenter::Copy& copy) {
    setModal(true);
    setWindowTitle(qt::kit::safe_text(copy.text("auth.window_title")));
    resize(460, 360);
    auto* box = new QVBoxLayout(this);
    qt::label(*box, copy.text("auth.title"), "sectionTitle");
    qt::label(*box, auth.state().operation.render(copy));
    qt::label(*box, copy.text("auth.account"));
    qt::label(*box, auth.state().account);
    auto* field = new Password(this);
    const auto name = copy.text("auth.password", {{"user", auth.state().account}});
    auto* caption = qt::label(*box, name);
    caption->setBuddy(field);
    field->setAccessibleName(qt::kit::safe_text(name));
    box->addWidget(field);
    auto* show = qt::check(*box, copy.text("auth.show"), false, [field, &auth](bool visible) {
        auth.show_password(visible);
        field->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        field->setFocus(Qt::OtherFocusReason);
    });
    qt::label(*box, copy.text("auth.context"));
    qt::button(*box, copy.text("action.cancel"), [this, &auth, field] { field->wipe(); auth.cancel(); reject(); });
    const auto submit = [this, &auth, field] {
        try {
            PasswordCopy password{*field, {}, {}, {}};
            password.text = field->text();
            password.encoded = password.text.toUtf8();
            password.bytes.assign(password.encoded.begin(), password.encoded.end());
            auth.password(password.bytes);
        } catch (...) { auth.cancel(); reject(); return; }
        auth.submit();
        if (auth.state().message && auth.state().message->key == "auth.checking") { accept(); } else { reject(); }
    };
    qt::button(*box, copy.text("auth.authenticate"), submit, true, true);
    connect(field, &QLineEdit::returnPressed, this, submit);
    connect(this, &QDialog::rejected, this, [&auth, field] { field->wipe(); auth.cancel(); });
    subscription_ = auth.observe([this, field, show, account = auth.state().account](const auto& state) {
        if (!state.active || state.account != account) {
            field->wipe();
            const QSignalBlocker blocker(show); show->setChecked(false);
            if (!terminating_ && (state.account != account || (state.message && state.message->key == "auth.timeout"))) {
                terminating_ = true; reject();
            }
        }
    });
    field->setFocus(Qt::OtherFocusReason);
}

}
