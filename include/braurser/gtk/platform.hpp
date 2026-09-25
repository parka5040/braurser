#ifndef BRAURSER_GTK_PLATFORM_HPP
#define BRAURSER_GTK_PLATFORM_HPP

#include <braurser/gtk/kit.hpp>
#include <braurser/presenter/models.hpp>
#include <glibmm/dispatcher.h>
#include <mutex>

namespace braurser::gtk {

class MainDispatcher final : public presenter::Dispatcher {
public:
    MainDispatcher();
    ~MainDispatcher() override;
    void post(std::function<void()> callback) override;
    void drain();
private:
    std::mutex mutex_;
    std::deque<std::function<void()>> callbacks_;
    Glib::Dispatcher wake_;
    sigc::connection connection_;
};

class MainTimer final : public presenter::Timer {
public:
    ~MainTimer() override;
    Token after(std::chrono::milliseconds delay, std::function<void()> callback) override;
    void cancel(Token token) override;
private:
    Token next_ = 1;
    std::map<Token, sigc::connection> connections_;
};

bool valid_desktop_id(std::string_view id);

class Platform {
public:
    virtual ~Platform() = default;
    virtual bool open_desktop(std::string_view id) = 0;
    virtual bool open_link(std::string_view uri) = 0;
    virtual void reboot(std::function<void(bool)> complete) = 0;
};

class DesktopPlatform final : public Platform {
public:
    bool open_desktop(std::string_view id) override;
    bool open_link(std::string_view uri) override;
    void reboot(std::function<void(bool)> complete) override;
};

}

#endif
