#ifndef BRAURSER_PRESENTER_COPY_HPP
#define BRAURSER_PRESENTER_COPY_HPP

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace braurser::presenter {

using CopyValue = std::variant<std::string, std::int64_t, double, std::chrono::system_clock::time_point>;
using CopyArgs = std::map<std::string, CopyValue, std::less<>>;
enum class MissingCopy { BuildDefault, Throw, Visible };

class Copy {
public:
    explicit Copy(std::string locale = "en", MissingCopy missing = MissingCopy::BuildDefault);
    Copy(std::map<std::string, std::string, std::less<>> entries, std::string locale, MissingCopy missing);
    std::string text(std::string_view key, const CopyArgs& args = {}) const;
    bool contains(std::string_view key) const;
    std::vector<std::string> keys() const;
    std::string number(double value) const;
    std::string date(std::chrono::system_clock::time_point value) const;
    std::string date(std::string_view iso8601) const;
    std::string size(std::optional<std::uint64_t> bytes) const;
private:
    std::map<std::string, std::string, std::less<>> entries_;
    std::string locale_;
    MissingCopy missing_;
};

}

#endif
