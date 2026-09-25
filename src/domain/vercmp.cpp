#include <braurser/domain/vercmp.hpp>

namespace braurser::domain {
namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool alnum(char c) { return digit(c) || letter(c); }

int compare(std::string_view a, std::string_view b) {
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

int segcmp(std::string_view a, std::string_view b) {
    if (a == b) return 0;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const std::size_t ai = i, bj = j;
        while (i < a.size() && !alnum(a[i])) ++i;
        while (j < b.size() && !alnum(b[j])) ++j;
        if (i == a.size() || j == b.size()) break;
        if (i - ai != j - bj) return i - ai > j - bj ? 1 : -1;
        const bool numeric = digit(a[i]);
        const std::size_t start_a = i, start_b = j;
        while (i < a.size() && (numeric ? digit(a[i]) : letter(a[i]))) ++i;
        while (j < b.size() && (numeric ? digit(b[j]) : letter(b[j]))) ++j;
        if (j == start_b) return numeric ? 1 : -1;
        std::string_view x = a.substr(start_a, i - start_a);
        std::string_view y = b.substr(start_b, j - start_b);
        if (numeric) {
            const auto first_x = x.find_first_not_of('0');
            const auto first_y = y.find_first_not_of('0');
            x.remove_prefix(first_x == std::string_view::npos ? x.size() : first_x);
            y.remove_prefix(first_y == std::string_view::npos ? y.size() : first_y);
            if (x.size() != y.size()) return x.size() > y.size() ? 1 : -1;
        }
        if (const int result = compare(x, y); result != 0) return result;
    }
    if (i == a.size() && j == b.size()) return 0;
    if ((i == a.size() && (j == b.size() || !letter(b[j]))) ||
        (i < a.size() && letter(a[i]))) return -1;
    return 1;
}

struct Parts {
    std::string_view epoch;
    std::string_view version;
    std::string_view release;
    bool has_release;
};

Parts split(std::string_view text) {
    std::size_t digits = 0;
    while (digits < text.size() && digit(text[digits])) ++digits;
    std::string_view epoch = "0";
    if (digits < text.size() && text[digits] == ':') {
        if (digits != 0) epoch = text.substr(0, digits);
        text.remove_prefix(digits + 1);
    }
    const auto dash = text.rfind('-');
    if (dash == std::string_view::npos) return {epoch, text, {}, false};
    return {epoch, text.substr(0, dash), text.substr(dash + 1), true};
}

}

int vercmp(std::string_view a, std::string_view b) {
    if (a == b) return 0;
    const auto x = split(a), y = split(b);
    if (const int result = segcmp(x.epoch, y.epoch); result != 0) return result;
    if (const int result = segcmp(x.version, y.version); result != 0) return result;
    return x.has_release && y.has_release ? segcmp(x.release, y.release) : 0;
}

}
