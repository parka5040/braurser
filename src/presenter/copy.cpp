#include <braurser/presenter/copy.hpp>
#include <braurser/protocol/dto.hpp>
#include <copy_data.hpp>
#include <unicode/datefmt.h>
#include <unicode/smpdtfmt.h>
#include <unicode/msgfmt.h>
#include <unicode/messagepattern.h>
#include <unicode/numfmt.h>
#include <unicode/numberformatter.h>
#include <unicode/measunit.h>
#include <unicode/uchar.h>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>

namespace braurser::presenter {
namespace {
icu::UnicodeString unicode(std::string_view value) {
    if (value.size() > 65536 || !protocol::valid_utf8(value)) { throw std::invalid_argument("invalid copy text"); }
    return icu::UnicodeString::fromUTF8(icu::StringPiece(value.data(), static_cast<std::int32_t>(value.size())));
}
icu::UnicodeString isolated(std::string_view value) {
    const auto input = unicode(value);
    icu::UnicodeString output;
    output.append(static_cast<UChar32>(0x2068));
    for (std::int32_t index = 0; index < input.length();) {
        const auto character = input.char32At(index);
        if (!u_hasBinaryProperty(character, UCHAR_BIDI_CONTROL)) { output.append(character); }
        index += U16_LENGTH(character);
    }
    output.append(static_cast<UChar32>(0x2069));
    return output;
}
std::string utf8(const icu::UnicodeString& value) {
    std::string result;
    value.toUTF8String(result);
    return result;
}
std::string format_date(double milliseconds, const std::string& locale) {
    std::unique_ptr<icu::DateFormat> format(icu::DateFormat::createDateTimeInstance(icu::DateFormat::kMedium, icu::DateFormat::kShort, icu::Locale(locale.c_str())));
    if (!format) { throw std::runtime_error("date formatting failed"); }
    icu::UnicodeString output;
    format->format(milliseconds, output);
    return utf8(output);
}
std::set<std::string> arguments(const icu::UnicodeString& pattern) {
    UErrorCode status = U_ZERO_ERROR;
    icu::MessagePattern parsed(pattern, nullptr, status);
    if (U_FAILURE(status)) { throw std::invalid_argument("invalid copy pattern"); }
    std::set<std::string> result;
    for (std::int32_t index = 0; index < parsed.countParts(); ++index) {
        const auto& part = parsed.getPart(index);
        if (part.getType() == UMSGPAT_PART_TYPE_ARG_NAME || part.getType() == UMSGPAT_PART_TYPE_ARG_NUMBER) {
            result.insert(utf8(parsed.getSubstring(part)));
        }
    }
    return result;
}
}
Copy::Copy(std::string locale, MissingCopy missing)
    : Copy(nlohmann::json::parse(kEnglishCopy).get<std::map<std::string, std::string, std::less<>>>(), std::move(locale), missing) {}
Copy::Copy(std::map<std::string, std::string, std::less<>> entries, std::string locale, MissingCopy missing)
    : entries_(std::move(entries)), locale_(std::move(locale)), missing_(missing) {
    if (locale_.empty() || locale_.size() > 64 || entries_.size() > 4096) { throw std::invalid_argument("invalid copy catalog"); }
    for (const auto& [key, pattern] : entries_) {
        if (key.empty() || key.size() > 128) { throw std::invalid_argument("invalid copy key"); }
        static_cast<void>(arguments(unicode(pattern)));
    }
    if (missing_ == MissingCopy::BuildDefault) {
#ifdef NDEBUG
        missing_ = MissingCopy::Visible;
#else
        missing_ = MissingCopy::Throw;
#endif
    }
}
bool Copy::contains(std::string_view key) const { return entries_.contains(key); }
std::vector<std::string> Copy::keys() const {
    std::vector<std::string> result;
    for (const auto& [key, value] : entries_) { static_cast<void>(value); result.push_back(key); }
    return result;
}
std::string Copy::text(std::string_view key, const CopyArgs& args) const {
    const auto missing = [&]() -> std::string {
        if (missing_ == MissingCopy::Throw) { throw std::invalid_argument("missing copy key or argument"); }
        if (key.size() > 128 || !protocol::valid_utf8(key)) { return "⟨copy⟩"; }
        return "⟨" + std::string(key) + "⟩";
    };
    const auto found = entries_.find(key);
    if (found == entries_.end()) { return missing(); }
    const auto pattern = unicode(found->second);
    const auto required = arguments(pattern);
    for (const auto& name : required) { if (!args.contains(name)) { return missing(); } }
    if (args.size() > 64) { throw std::invalid_argument("too many copy arguments"); }
    std::vector<icu::UnicodeString> names;
    std::vector<icu::Formattable> values;
    for (const auto& [name, value] : args) {
        names.push_back(unicode(name));
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::string>) {
                values.emplace_back(isolated(item));
            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                const double milliseconds = std::chrono::duration<double, std::milli>(item.time_since_epoch()).count();
                values.emplace_back(milliseconds, icu::Formattable::kIsDate);
            } else {
                if constexpr (std::is_same_v<T, double>) { if (!std::isfinite(item)) { throw std::invalid_argument("nonfinite copy number"); } }
                values.emplace_back(item);
            }
        }, value);
    }
    UErrorCode status = U_ZERO_ERROR;
    icu::MessageFormat format(pattern, icu::Locale(locale_.c_str()), status);
    icu::UnicodeString output;
    format.format(names.data(), values.data(), static_cast<std::int32_t>(names.size()), output, status);
    if (U_FAILURE(status)) { return missing(); }
    return utf8(output);
}
std::string Copy::number(double value) const {
    if (!std::isfinite(value)) { throw std::invalid_argument("nonfinite number"); }
    UErrorCode status = U_ZERO_ERROR;
    auto output = icu::number::NumberFormatter::withLocale(icu::Locale(locale_.c_str())).formatDouble(value, status).toString(status);
    if (U_FAILURE(status)) { throw std::runtime_error("number formatting failed"); }
    return utf8(output);
}
std::string Copy::date(std::chrono::system_clock::time_point value) const {
    return format_date(std::chrono::duration<double, std::milli>(value.time_since_epoch()).count(), locale_);
}
std::string Copy::date(std::string_view iso8601) const {
    if (iso8601.size() < 20 || iso8601.size() > 40 || !protocol::valid_utf8(iso8601)) { return text("state.unavailable"); }
    const auto input = unicode(iso8601);
    for (const auto* pattern : {"yyyy-MM-dd'T'HH:mm:ssXXX", "yyyy-MM-dd'T'HH:mm:ss.SSSXXX"}) {
        UErrorCode status = U_ZERO_ERROR;
        icu::SimpleDateFormat parser(unicode(pattern), icu::Locale::getRoot(), status);
        parser.setLenient(false);
        icu::ParsePosition position(0);
        const auto milliseconds = parser.parse(input, position);
        if (U_SUCCESS(status) && position.getErrorIndex() < 0 && position.getIndex() == input.length()) {
            return format_date(milliseconds, locale_);
        }
    }
    return text("state.unavailable");
}
std::string Copy::size(std::optional<std::uint64_t> bytes) const {
    if (!bytes) { return text("state.unavailable"); }
    UErrorCode status = U_ZERO_ERROR;
    auto output = icu::number::NumberFormatter::withLocale(icu::Locale(locale_.c_str()))
        .unit(icu::MeasureUnit::getByte()).unitWidth(UNUM_UNIT_WIDTH_SHORT)
        .formatDouble(static_cast<double>(*bytes), status).toString(status);
    if (U_FAILURE(status)) { throw std::runtime_error("size formatting failed"); }
    return utf8(output);
}

}
