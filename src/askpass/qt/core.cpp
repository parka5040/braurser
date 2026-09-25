#include "core.hpp"
#include <algorithm>
#include <cstring>
#include <pwd.h>
#include <unistd.h>

namespace braurser::askpass {
namespace {
struct Wipe {
    std::vector<char>& buffer;
    ~Wipe() { ::explicit_bzero(buffer.data(), buffer.size()); }
};
}

presenter::AuthAction action(std::string_view value) {
    if (value == "install") { return presenter::AuthAction::Install; }
    if (value == "remove") { return presenter::AuthAction::Remove; }
    if (value == "git") { return presenter::AuthAction::Git; }
    return presenter::AuthAction::Update;
}

bool write_password(std::vector<char>& password, const std::function<bool(std::span<const char>)>& write) {
    const Wipe wipe{password};
    if (password.size() > 4096 || std::ranges::any_of(password, [](char value) { return value == '\0' || value == '\n' || value == '\r'; })) { return false; }
    std::vector<char> framed(password.size() + 1, '\0');
    const Wipe wipe_framed{framed};
    std::ranges::copy(password, framed.begin());
    framed.back() = '\n';
    try { return write(framed); } catch (...) { return false; }
}

std::optional<std::string> current_account() {
    std::vector<char> buffer(65536, '\0');
    const Wipe wipe{buffer};
    passwd account{};
    passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &account, buffer.data(), buffer.size(), &result) != 0 || !result || !result->pw_name) { return {}; }
    const auto length = ::strnlen(result->pw_name, 257);
    if (length == 0 || length > 256 || !protocol::valid_utf8(std::string_view(result->pw_name, length))) { return {}; }
    return std::string(result->pw_name, length);
}

}
