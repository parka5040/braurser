#include <braurser/presenter/models.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <pwd.h>
#include <unistd.h>

namespace braurser::askpass {
using PasswordSink = std::function<bool(std::span<const char>)>;

bool write_password(std::vector<char>& password, std::vector<char>& output, const PasswordSink& sink) {
    struct Wipe {
        std::vector<char>& first;
        std::vector<char>& second;
        ~Wipe() { ::explicit_bzero(first.data(), first.size()); ::explicit_bzero(second.data(), second.size()); }
    } wipe{password, output};
    if (password.size() > 4096 || std::ranges::any_of(password, [](char c) { return c == '\0' || c == '\n' || c == '\r'; })) { return false; }
    if (output.size() < password.size() + 1) { return false; }
    std::ranges::copy(password, output.begin());
    output[password.size()] = '\n';
    try { return sink(std::span(output.data(), password.size() + 1)); }
    catch (...) { return false; }
}

presenter::AuthAction action_from_environment(std::string_view action) {
    if (action == "install") { return presenter::AuthAction::Install; }
    if (action == "remove") { return presenter::AuthAction::Remove; }
    if (action == "git") { return presenter::AuthAction::Git; }
    return presenter::AuthAction::Update;
}

presenter::AuthBinding local_binding(int, char**, std::string_view action, const std::string& account, const presenter::Copy& copy) {
    return {"sudo-askpass", "sudo-askpass", action_from_environment(action), copy.text("state.unavailable"), {account},
        std::chrono::steady_clock::now() + std::chrono::minutes(2), false, true};
}

std::string current_account() {
    std::vector<char> buffer(65536, '\0');
    passwd entry{};
    passwd* result = nullptr;
    const int status = ::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result);
    if (status != 0 || !result || !entry.pw_name) { throw std::runtime_error("account unavailable"); }
    const auto length = ::strnlen(entry.pw_name, 257);
    if (length == 0 || length > 256) { throw std::runtime_error("invalid account"); }
    const std::string name(entry.pw_name, length);
    if (!protocol::valid_utf8(name)) { throw std::runtime_error("invalid account"); }
    return name;
}

class StdoutChannel final : public presenter::CredentialChannel {
public:
    bool submit(const presenter::AuthBinding&, std::string_view, std::span<const char> secret) override {
        std::vector<char> output(4097, '\0');
        std::vector<char> password(secret.begin(), secret.end());
        submitted = write_password(password, output, [](std::span<const char> bytes) {
            std::size_t written = 0;
            while (written < bytes.size()) {
                const auto count = ::write(STDOUT_FILENO, bytes.data() + written, bytes.size() - written);
                if (count < 0 && errno == EINTR) { continue; }
                if (count <= 0) { return false; }
                written += static_cast<std::size_t>(count);
            }
            return true;
        });
        return submitted;
    }
    void cancel(const presenter::AuthBinding&) override {}
    bool submitted = false;
};
}
