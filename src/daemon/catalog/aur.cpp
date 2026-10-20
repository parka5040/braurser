#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/validate/validate.hpp>

#include <nlohmann/json.hpp>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <map>
#include <memory>
#include <streambuf>
#include <istream>
#include <unordered_map>

namespace braurser::daemon::catalog {
namespace {

CatalogError failure(std::string code) { return {std::move(code), {}}; }

class InflateBuffer final : public std::streambuf {
public:
    explicit InflateBuffer(std::span<const std::byte> compressed) : bytes_(compressed) {
        valid_ = compressed.size() <= 64ULL * 1024 * 1024 &&
                 inflateInit2(&stream_, MAX_WBITS + 16) == Z_OK;
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }
    ~InflateBuffer() override { if (valid_) inflateEnd(&stream_); }
    bool good() const { return valid_ && complete_ && !failed_; }
protected:
    int_type underflow() override {
        if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (!valid_ || complete_ || failed_) return traits_type::eof();
        while (true) {
            if (stream_.avail_in == 0 && offset_ < bytes_.size()) {
                const auto amount = std::min<std::size_t>(bytes_.size() - offset_, 16384);
                stream_.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(bytes_.data() + offset_));
                stream_.avail_in = static_cast<uInt>(amount);
                offset_ += amount;
            }
            stream_.next_out = reinterpret_cast<Bytef*>(buffer_.data());
            stream_.avail_out = static_cast<uInt>(buffer_.size());
            const int status = inflate(&stream_, Z_NO_FLUSH);
            const auto count = buffer_.size() - stream_.avail_out;
            if (count > 768ULL * 1024 * 1024 - total_) { failed_ = true; return traits_type::eof(); }
            total_ += count;
            if (status == Z_STREAM_END) {
                complete_ = true;
                if (stream_.avail_in != 0 || offset_ != bytes_.size()) failed_ = true;
            } else if (status != Z_OK) { failed_ = true; return traits_type::eof(); }
            if (count) {
                setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
                return traits_type::to_int_type(*gptr());
            }
            if (complete_ || failed_ || (offset_ == bytes_.size() && stream_.avail_in == 0)) {
                if (!complete_) failed_ = true;
                return traits_type::eof();
            }
        }
    }
private:
    std::span<const std::byte> bytes_;
    z_stream stream_{};
    std::array<char, 16384> buffer_{};
    std::size_t offset_{};
    std::size_t total_{};
    bool valid_{};
    bool complete_{};
    bool failed_{};
};

constexpr std::array<std::string_view, 23> keys{
    "Name", "PackageBase", "Version", "Description", "URL", "Maintainer", "Submitter",
    "NumVotes", "Popularity", "OutOfDate", "FirstSubmitted", "LastModified", "License",
    "Keywords", "Groups", "Depends", "MakeDepends", "CheckDepends", "OptDepends", "Provides",
    "Conflicts", "Replaces", "CoMaintainers"
};
using Values = std::array<std::vector<std::string>, 23>;

std::size_t slot(std::string_view key) {
    const auto found = std::ranges::find(keys, key);
    return static_cast<std::size_t>(found - keys.begin());
}

std::string_view field(const Values& values, std::string_view key) {
    const auto index = slot(key);
    return index < values.size() && !values[index].empty() ? std::string_view(values[index].front()) : std::string_view{};
}

template<class T>
T integer(const Values& values, const char* key) {
    const auto value = field(values, key);
    T number{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
    return ec == std::errc{} && end == value.data() + value.size() ? number : T{};
}

std::vector<std::string> list(const Values& values, std::string_view key) {
    const auto index = slot(key);
    return index < values.size() ? values[index] : std::vector<std::string>{};
}

std::optional<AurPackage> make_package(const Values& values) {
    AurPackage result;
    result.name = field(values, "Name");
    result.package_base = field(values, "PackageBase");
    result.version = field(values, "Version");
    if (!br_valid_pkgname(result.name.data(), result.name.size()) || result.version.empty()) return std::nullopt;
    result.description = field(values, "Description");
    result.url = field(values, "URL");
    if (!field(values, "Maintainer").empty()) result.maintainer = field(values, "Maintainer");
    if (!field(values, "Submitter").empty()) result.submitter = field(values, "Submitter");
    result.votes = integer<std::uint32_t>(values, "NumVotes");
    const auto popularity = field(values, "Popularity");
    if (!popularity.empty()) {
        double parsed{};
        auto [end, ec] = std::from_chars(popularity.data(), popularity.data() + popularity.size(), parsed);
        if (ec == std::errc{} && end == popularity.data() + popularity.size() && std::isfinite(parsed)) result.popularity = parsed;
    }
    if (!field(values, "OutOfDate").empty()) result.out_of_date = integer<std::int64_t>(values, "OutOfDate");
    result.first_submitted = integer<std::int64_t>(values, "FirstSubmitted");
    result.last_modified = integer<std::int64_t>(values, "LastModified");
    result.licenses = list(values, "License");
    result.keywords = list(values, "Keywords");
    result.groups = list(values, "Groups");
    result.depends = list(values, "Depends");
    result.makedepends = list(values, "MakeDepends");
    result.checkdepends = list(values, "CheckDepends");
    result.optdepends = list(values, "OptDepends");
    result.provides = list(values, "Provides");
    result.conflicts = list(values, "Conflicts");
    result.replaces = list(values, "Replaces");
    result.comaintainers = list(values, "CoMaintainers");
    return result;
}

class AurSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    explicit AurSax(const AurSink& sink) : sink_(sink) {}
    RecordCounts counts;
    bool okay{true};
    bool null() override { return put({}); }
    bool boolean(bool) override { return put({}); }
    bool number_integer(number_integer_t value) override { return put(std::to_string(value)); }
    bool number_unsigned(number_unsigned_t value) override { return put(std::to_string(value)); }
    bool number_float(number_float_t, const string_t& value) override { return put(value); }
    bool string(string_t& value) override { return put(value); }
    bool binary(binary_t&) override { okay = false; return false; }
    bool start_object(std::size_t) override {
        ++depth_;
        if (depth_ != 2) { okay = false; return false; }
        for (auto& values : values_) values.clear();
        value_count_ = 0;
        value_bytes_ = 0;
        key_ = values_.size();
        return true;
    }
    bool key(string_t& value) override {
        if (value.size() > 65536) { okay = false; return false; }
        key_ = slot(value);
        return true;
    }
    bool end_object() override {
        if (depth_ != 2) { okay = false; return false; }
        if (++records_ > 300000) { okay = false; return false; }
        if (auto package = make_package(values_)) { sink_(std::move(*package)); ++counts.packages; }
        else ++counts.rejected;
        --depth_;
        return true;
    }
    bool start_array(std::size_t) override {
        ++depth_;
        if (depth_ == 1) {
            if (root_started_) { okay = false; return false; }
            root_started_ = true;
            return true;
        }
        if (depth_ != 3) { okay = false; return false; }
        return true;
    }
    bool end_array() override {
        if (depth_ != 1 && depth_ != 3) { okay = false; return false; }
        --depth_;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        okay = false;
        return false;
    }
    bool finished() const { return depth_ == 0 && root_started_ && okay; }
private:
    bool put(const std::string& value) {
        if (value.size() > 65536 || ++value_count_ > 20000 || value.size() > 1024 * 1024 - value_bytes_) {
            okay = false; return false;
        }
        value_bytes_ += value.size();
        if (depth_ == 2 || depth_ == 3) {
            if (key_ < values_.size()) values_[key_].push_back(value);
        }
        else { okay = false; return false; }
        return true;
    }
    int depth_{};
    std::size_t records_{};
    bool root_started_{};
    Values values_;
    std::size_t key_{};
    std::size_t value_count_{};
    std::size_t value_bytes_{};
    const AurSink& sink_;
};

std::optional<Values> from_json(const nlohmann::json& item) {
    if (!item.is_object()) return std::nullopt;
    Values values;
    std::size_t count = 0;
    std::size_t bytes = 0;
    for (auto it = item.begin(); it != item.end(); ++it) {
        if (it.key().size() > 65536) return std::nullopt;
        auto collect = [&](const nlohmann::json& value) -> bool {
            std::string str;
            if (value.is_string()) str = value.get<std::string>();
            else if (value.is_number()) str = value.dump();
            else return value.is_null();
            if (str.size() > 65536 || ++count > 20000 || str.size() > 1024 * 1024 - bytes) return false;
            bytes += str.size();
            const auto index = slot(it.key());
            if (index < values.size()) values[index].push_back(std::move(str));
            return true;
        };
        if (it->is_array()) {
            for (const auto& value : *it) if (!collect(value)) return std::nullopt;
        } else if (!collect(*it)) return std::nullopt;
    }
    return values;
}

std::string escape(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    for (const char raw_ch : text) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out += static_cast<char>(ch);
        else { out += '%'; out += digits[ch >> 4]; out += digits[ch & 15]; }
    }
    return out;
}

}

std::expected<RecordCounts, CatalogError> AurDump::parse(std::span<const std::byte> bytes, const AurSink& sink) const {
    if (bytes.empty() || bytes.size() > 64ULL * 1024 * 1024) return std::unexpected(failure("aur-size"));
    InflateBuffer buffer(bytes);
    std::istream stream(&buffer);
    AurSax sax(sink);
    const bool parsed = nlohmann::json::sax_parse(stream, &sax);
    if (!parsed || !sax.finished() || !buffer.good()) return std::unexpected(failure("aur-json"));
    return sax.counts;
}

std::expected<RecordCounts, CatalogError> AurDumpFetcher::fetch(const std::filesystem::path& cache_root, const AurSink& sink) const {
    if (!cache_root.is_absolute()) return std::unexpected(failure("aur-cache-path"));
    const auto directory = cache_root / "braurser" / "downloads";
    auto created = core::ensure_private_dir(cache_root / "braurser");
    if (!created) return std::unexpected(failure("aur-cache-dir"));
    created = core::ensure_private_dir(directory);
    if (!created) return std::unexpected(failure("aur-cache-dir"));
    const auto path = directory / "packages-meta-ext-v1.json.gz";
    const auto meta = directory / "packages-meta-ext-v1.meta.json";
    core::HttpRequest request;
    request.url = "https://aur.archlinux.org/packages-meta-ext-v1.json.gz";
    request.allowed_hosts = {"aur.archlinux.org"};
    request.max_bytes = 64ULL * 1024 * 1024;
    auto old_meta = core::read_bounded(meta, 8192);
    if (old_meta) {
        auto parsed = nlohmann::json::parse(*old_meta, nullptr, false);
        if (parsed.is_object()) {
            if (parsed.contains("etag") && parsed["etag"].is_string()) request.if_none_match = parsed["etag"].get<std::string>();
            if (parsed.contains("last_modified") && parsed["last_modified"].is_string()) request.if_modified_since = parsed["last_modified"].get<std::string>();
        }
    }
    auto response = http_.get(request);
    if (!response) return std::unexpected(failure("aur-http"));
    if (response->not_modified) {
        auto old = core::read_bounded(path, 64ULL * 1024 * 1024);
        if (!old) return std::unexpected(failure("aur-cache-read"));
        return AurDump{}.parse(std::as_bytes(std::span(*old)), sink);
    }
    if (response->status != 200) return std::unexpected(failure("aur-status"));
    auto parsed = AurDump{}.parse(std::as_bytes(std::span(response->body)), sink);
    if (!parsed) return std::unexpected(parsed.error());
    auto written = core::atomic_write(path, response->body);
    if (!written) return std::unexpected(failure("aur-cache-write"));
    nlohmann::json state = { {"etag", response->etag}, {"last_modified", response->last_modified} };
    written = core::atomic_write(meta, state.dump());
    if (!written) return std::unexpected(failure("aur-meta-write"));
    return parsed;
}

std::expected<std::vector<AurPackage>, CatalogError> AurDump::parse(std::span<const std::byte> bytes) const {
    std::vector<AurPackage> result;
    auto counts = parse(bytes, [&](AurPackage&& package) { result.push_back(std::move(package)); });
    if (!counts) return std::unexpected(counts.error());
    return result;
}

std::expected<std::vector<AurPackage>, CatalogError> AurDumpFetcher::fetch(const std::filesystem::path& cache_root) const {
    std::vector<AurPackage> result;
    auto counts = fetch(cache_root, [&](AurPackage&& package) { result.push_back(std::move(package)); });
    if (!counts) return std::unexpected(counts.error());
    return result;
}

AurRpcClient::AurRpcClient(core::HttpClient& http, std::size_t daily_budget)
    : http_(http), daily_budget_(daily_budget) {}

std::expected<std::vector<AurPackage>, CatalogError> AurRpcClient::info(std::span<const std::string> names) {
    std::lock_guard lock(mutex_);
    std::vector<AurPackage> packages;
    const auto now = std::chrono::system_clock::now();
    const auto day = std::chrono::duration_cast<std::chrono::days>(now.time_since_epoch()).count();
    if (day != day_) { day_ = day; used_ = 0; }
    std::vector<std::string> urls;
    for (const auto& name : names) {
        if (!br_valid_pkgname(name.data(), name.size())) return std::unexpected(failure("aur-name"));
    }
    for (std::size_t offset = 0; offset < names.size();) {
        std::string url = "https://aur.archlinux.org/rpc/v5/info";
        std::size_t batch = 0;
        while (offset < names.size() && batch < 100) {
            auto argument = std::string(batch == 0 ? "?arg%5B%5D=" : "&arg%5B%5D=") + escape(names[offset]);
            if (url.size() + argument.size() > 8192) break;
            url += argument;
            ++offset;
            ++batch;
        }
        if (batch == 0) return std::unexpected(failure("aur-url"));
        urls.push_back(std::move(url));
    }
    if (urls.size() > daily_budget_ - std::min(daily_budget_, used_))
        return std::unexpected(failure("aur-budget"));
    for (const auto& url : urls) {
        core::HttpRequest request;
        request.url = url;
        request.allowed_hosts = {"aur.archlinux.org"};
        request.max_bytes = 8 * 1024 * 1024;
        ++used_;
        auto reply = http_.get(request);
        if (!reply || reply->status != 200) return std::unexpected(failure("aur-rpc-http"));
        auto doc = nlohmann::json::parse(reply->body, nullptr, false);
        if (!doc.is_object() || !doc.contains("type") || !doc["type"].is_string() ||
            doc["type"] == "error" || doc["type"] != "multiinfo")
            return std::unexpected(failure("aur-rpc-json"));
        if (!doc.contains("results") || !doc["results"].is_array() || doc["results"].size() > 100)
            return std::unexpected(failure("aur-rpc-results"));
        for (const auto& item : doc["results"]) {
            auto values = from_json(item);
            if (!values) return std::unexpected(failure("aur-rpc-record"));
            if (auto package = make_package(*values)) packages.push_back(std::move(*package));
        }
    }
    return packages;
}

}
