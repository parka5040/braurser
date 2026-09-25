#ifndef BRAURSER_QT_APP_HPP
#define BRAURSER_QT_APP_HPP

#include <braurser/presenter/models.hpp>
#include <braurser/qt/kit.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QMainWindow>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace braurser::qt {

class Dispatcher final : public QObject, public presenter::Dispatcher {
public:
    void post(std::function<void()> callback) override;
};

class Timer final : public QObject, public presenter::Timer {
public:
    Token after(std::chrono::milliseconds delay, std::function<void()> callback) override;
    void cancel(Token token) override;
private:
    Token next_ = 1;
    std::map<Token, QTimer*> timers_;
};

class Platform {
public:
    virtual ~Platform() = default;
    virtual bool open_url(const QUrl& url) = 0;
    virtual bool open_desktop(std::string_view id) = 0;
    virtual void reboot(std::function<void(bool)> completed) = 0;
};

class DesktopPlatform final : public QObject, public Platform {
public:
    bool open_url(const QUrl& url) override;
    bool open_desktop(std::string_view id) override;
    void reboot(std::function<void(bool)> completed) override;
};

struct Models {
    Models(presenter::Rpc& rpc, presenter::Timer& timer);
    presenter::Shell shell;
    presenter::Discover discover;
    presenter::Search search;
    presenter::Details details;
    presenter::VariantChooser variants;
    presenter::Confirm confirm;
    presenter::Activity activity;
    presenter::Updates updates;
    presenter::Installed installed;
    presenter::GitWizard git;
    presenter::Settings settings;
    presenter::FirstRun first;
};

class Window final : public QMainWindow {
public:
    Window(Models& models, presenter::Copy& copy, Platform& platform);
    void navigate(std::string_view page);
    void refresh();
    void render();
    void show_confirm();
    void show_variants();
    void restart();
protected:
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
private:
    std::string text(std::string_view key, const presenter::CopyArgs& args = {}) const;
    void schedule();
    void discover_page(QVBoxLayout& box);
    void search_page(QVBoxLayout& box);
    void details_page(QVBoxLayout& box);
    void updates_page(QVBoxLayout& box);
    void installed_page(QVBoxLayout& box);
    void settings_page(QVBoxLayout& box);
    void activity_page(QVBoxLayout& box);
    void git_page(QVBoxLayout& box);
    void filters();
    void open_details(std::string id);
    void unavailable();
    void launch(std::string id, std::string app);
    void wiki_search();
    void details_action();
    void request_plan(protocol::PlanParams params);
    void setup();
    void error(QVBoxLayout& box, const std::optional<presenter::Text>& value);
    kit::Source source(const protocol::PackageRef& ref) const;
    void package(QVBoxLayout& box, const protocol::PackageSummary& value);
    Models& models_;
    presenter::Copy& copy_;
    Platform& platform_;
    kit::ThemeManager themes_;
    QScrollArea* scroll_;
    QPushButton* back_;
    QLabel* heading_;
    QPushButton* check_again_;
    QWidget* action_bar_;
    kit::ResponsiveShell* shell_;
    std::vector<presenter::Subscription> subscriptions_;
    QPointer<QDialog> confirm_dialog_;
    bool scheduled_ = false;
    bool plan_pending_ = false;
    bool restart_dismissed_ = false;
    presenter::Destination rendered_ = presenter::Destination::Discover;
    std::string rendered_id_;
    std::map<std::string, int> scroll_positions_;
};

QLabel* label(QVBoxLayout& box, std::string_view value, const char* role = nullptr);
QPushButton* button(QVBoxLayout& box, std::string_view value, std::function<void()> action, bool enabled = true, bool primary = false);
QLineEdit* entry(QVBoxLayout& box, std::string_view name, std::string_view value, const char* id);
QCheckBox* check(QVBoxLayout& box, std::string_view name, bool value, std::function<void(bool)> action);
QComboBox* combo(QVBoxLayout& box, std::string_view name, const std::vector<std::pair<std::string, std::string>>& values, std::string_view selected, std::function<void(std::string)> action);
QWidget* column(QWidget* parent = nullptr);
kit::Disclosure* disclosure(QVBoxLayout& box, QWidget& child, std::string_view title, const presenter::Copy& copy, bool expanded = false);
std::vector<kit::DocumentBlock> document_blocks(const protocol::WikiDocument& document);
bool valid_desktop_id(std::string_view id);

}

#endif
