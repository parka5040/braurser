#include "core.hpp"
#include "dialog.hpp"
#include <braurser/qt/app.hpp>
#include <QApplication>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::askpass {
namespace {
class StdoutChannel final : public presenter::CredentialChannel {
public:
    bool submitted = false;
    bool submit(const presenter::AuthBinding&, std::string_view, std::span<const char> bytes) override {
        std::vector<char> password(bytes.begin(), bytes.end());
        submitted = write_password(password, [](std::span<const char> output) {
            std::size_t offset = 0;
            while (offset < output.size()) {
                const auto written = ::write(STDOUT_FILENO, output.data() + offset, output.size() - offset);
                if (written < 0 && errno == EINTR) { continue; }
                if (written <= 0) { return false; }
                offset += static_cast<std::size_t>(written);
            }
            return true;
        });
        return submitted;
    }
    void cancel(const presenter::AuthBinding&) override {}
};
}
}

int main(int, char** argv) {
    ::umask(0077);
    if (::prctl(PR_SET_DUMPABLE, 0) != 0) { return 1; }
    const rlimit limit{0, 0};
    static_cast<void>(::setrlimit(RLIMIT_CORE, &limit));
    std::signal(SIGPIPE, SIG_IGN);
    int count = 1;
    char* arguments[]{argv[0], nullptr};
    QApplication app(count, arguments);
    app.setApplicationName("braurser-askpass-qt");
    const auto account = braurser::askpass::current_account();
    if (!account) { return 1; }
    braurser::presenter::Copy copy;
    braurser::qt::Timer timer;
    braurser::askpass::StdoutChannel channel;
    braurser::presenter::Auth auth(channel, timer);
    const char* environment = std::getenv("BRAURSER_AUTH_ACTION");
    const auto action = braurser::askpass::action(environment && ::strnlen(environment, 32) < 32 ? std::string_view(environment) : std::string_view{});
    auth.bind({"sudo-askpass", std::to_string(::getpid()), action, copy.text("state.unavailable"), {*account},
        std::chrono::steady_clock::now() + std::chrono::minutes(2), false, true});
    if (!auth.state().password_ui) { return 1; }
    braurser::askpass::Dialog dialog(auth, copy);
    const int result = dialog.exec();
    if (result != QDialog::Accepted) { auth.cancel(); }
    return result == QDialog::Accepted && channel.submitted ? 0 : 1;
}
