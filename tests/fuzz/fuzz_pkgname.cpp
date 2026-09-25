#include <braurser/validate/validate.hpp>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <string>

namespace {

bool package(const std::string& text) {
    if (text.empty() || text.size() > 255) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '@' || c == '_' || c == '+' ||
              (i > 0 && (c == '.' || c == '-')))) return false;
    }
    return true;
}

bool repo(const std::string& text) {
    if (text.empty() || text.size() > 64 || text[0] == '-' || text[0] == '.') return false;
    for (const char c : text) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool target(const std::string& text, std::size_t& slash) {
    slash = text.find('/');
    if (slash == std::string::npos) {
        slash = text.size();
        return package(text);
    }
    return repo(text.substr(0, slash)) && package(text.substr(slash + 1));
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string input(reinterpret_cast<const char*>(data), size);
    if (br_valid_pkgname(input.data(), input.size()) != package(input)) std::abort();
    std::size_t expected = 0, actual = 0;
    const bool valid = target(input, expected);
    if (br_split_target(input.data(), input.size(), &actual) != valid) std::abort();
    if (valid && actual != expected) std::abort();
    return 0;
}
