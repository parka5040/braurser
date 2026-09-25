#include <braurser/daemon/wiki/cache.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/validate/validate.hpp>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace braurser::daemon::wiki {
namespace {
using json = nlohmann::json;

std::string filename(std::string_view title) {
    if (title.empty() || title.size() > 255) return {};
    std::string lower(title);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    br_sha256_ctx context{};
    br_sha256_init(&context);
    br_sha256_update(&context, reinterpret_cast<const uint8_t*>(lower.data()), lower.size());
    std::array<uint8_t, 32> digest{};
    br_sha256_final(&context, digest.data());
    char hex[65]{};
    br_hex32(digest.data(), hex);
    return std::string(hex) + ".json";
}

json encode_inlines(const std::vector<Inline>& inlines) {
    json result = json::array();
    for (const auto& inline_value : inlines) {
        if (auto* text = std::get_if<Text>(&inline_value))
            result.push_back({{"type", "text"}, {"s", text->s}, {"emphasis", text->emphasis},
                              {"strong", text->strong}, {"code", text->code}});
        else {
            const auto& link = std::get<Link>(inline_value);
            result.push_back({{"type", "link"}, {"label", link.label}, {"href", link.href}});
        }
    }
    return result;
}

std::vector<Inline> decode_inlines(const json& array) {
    if (!array.is_array() || array.size() > max_block_text) throw json::type_error::create(302, "invalid inline array", &array);
    std::vector<Inline> result;
    std::size_t length = 0;
    for (const auto& value : array) {
        const auto type = value.at("type").get<std::string>();
        if (type == "text") {
            auto text = value.at("s").get<std::string>();
            length += text.size();
            result.emplace_back(Text{std::move(text), value.at("emphasis").get<bool>(),
                                     value.at("strong").get<bool>(), value.at("code").get<bool>()});
        } else if (type == "link") {
            auto label = value.at("label").get<std::string>();
            auto href = value.at("href").get<std::string>();
            if (!href.starts_with("https://") || href.size() > 8192) throw json::type_error::create(302, "invalid cached link", &value);
            length += label.size();
            result.emplace_back(Link{std::move(label), std::move(href)});
        } else throw json::type_error::create(302, "invalid inline kind", &value);
        if (length > max_block_text) throw json::type_error::create(302, "inline overflow", &array);
    }
    return result;
}

json encode_document(const Document& doc) {
    json blocks = json::array();
    for (const auto& block : doc.blocks) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Heading>)
                blocks.push_back({{"type", "heading"}, {"level", value.level}, {"text", value.text}, {"anchor", value.anchor}});
            else if constexpr (std::is_same_v<T, Code>)
                blocks.push_back({{"type", "code"}, {"text", value.text}});
            else if constexpr (std::is_same_v<T, List>) {
                json items = json::array();
                for (const auto& item : value.items) items.push_back(encode_inlines(item));
                blocks.push_back({{"type", "list"}, {"ordered", value.ordered}, {"items", std::move(items)}});
            } else {
                const char* type = std::is_same_v<T, Note> ? "note" :
                                   std::is_same_v<T, Warning> ? "warning" : "paragraph";
                blocks.push_back({{"type", type}, {"inlines", encode_inlines(value.inlines)}});
            }
        }, block);
    }
    return { {"title", doc.title}, {"source_url", doc.source_url}, {"blocks", std::move(blocks)} };
}

Document decode_document(const json& json_doc) {
    Document doc;
    doc.title = json_doc.at("title").get<std::string>();
    doc.source_url = json_doc.at("source_url").get<std::string>();
    if (doc.title.size() > 255 || !doc.source_url.starts_with("https://wiki.archlinux.org/title/") ||
        doc.source_url.size() > 8192) throw json::type_error::create(302, "invalid cached document", &json_doc);
    const auto& blocks = json_doc.at("blocks");
    if (!blocks.is_array() || blocks.size() > max_blocks) throw json::type_error::create(302, "invalid blocks", &blocks);
    for (const auto& block : blocks) {
        const auto type = block.at("type").get<std::string>();
        if (type == "heading") {
            const auto level = block.at("level").get<int>();
            auto text = block.at("text").get<std::string>();
            auto anchor = block.at("anchor").get<std::string>();
            if (level < 2 || level > 4 || text.size() > max_block_text || anchor.size() > max_block_text)
                throw json::type_error::create(302, "invalid heading", &block);
            doc.sections.push_back({level, text, anchor});
            doc.blocks.emplace_back(Heading{level, std::move(text), std::move(anchor)});
        } else if (type == "code") {
            auto text = block.at("text").get<std::string>();
            if (text.size() > max_block_text) throw json::type_error::create(302, "invalid code", &block);
            doc.blocks.emplace_back(Code{std::move(text)});
        } else if (type == "list") {
            List list{block.at("ordered").get<bool>(), {}};
            const auto& items = block.at("items");
            if (!items.is_array() || items.size() > max_blocks) throw json::type_error::create(302, "invalid list", &block);
            std::size_t total = 0;
            for (const auto& item : items) {
                auto inlines = decode_inlines(item);
                for (const auto& part : inlines) {
                    total += std::visit([](const auto& value) {
                        if constexpr (requires { value.s; }) return value.s.size();
                        else return value.label.size();
                    }, part);
                    if (total > max_block_text) throw json::type_error::create(302, "list overflow", &block);
                }
                list.items.push_back(std::move(inlines));
            }
            doc.blocks.emplace_back(std::move(list));
        } else if (type == "paragraph") doc.blocks.emplace_back(Paragraph{decode_inlines(block.at("inlines"))});
        else if (type == "note") doc.blocks.emplace_back(Note{decode_inlines(block.at("inlines"))});
        else if (type == "warning") doc.blocks.emplace_back(Warning{decode_inlines(block.at("inlines"))});
        else throw json::type_error::create(302, "invalid block", &block);
    }
    return doc;
}

}

WikiCache::WikiCache(std::filesystem::path cache_root, const core::Clock& clock)
    : root_(std::move(cache_root) / "braurser" / "wiki"), clock_(clock) {}

std::optional<CachedArticle> WikiCache::read(std::string_view title, std::string_view package_name) const {
    const auto name = filename(title);
    if (name.empty()) return std::nullopt;
    const auto content = core::read_bounded(root_ / name, 4 * 1024 * 1024);
    if (!content) return std::nullopt;
    try {
        const auto data = json::parse(*content);
        if (data.at("version").get<int>() != 1 || data.at("key").get<std::string>() != title ||
            data.at("package").get<std::string>() != package_name) return std::nullopt;
        const auto seconds = data.at("fetched_at").get<std::int64_t>();
        if (seconds < 0 || seconds > 4'102'444'800LL) return std::nullopt;
        const auto fetched = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
        const auto age = clock_.now() - fetched;
        if (age < std::chrono::seconds(0)) return std::nullopt;
        if (data.at("negative").get<bool>()) {
            if (age > std::chrono::hours(24)) return std::nullopt;
            return CachedArticle{std::nullopt, false};
        }
        auto doc = decode_document(data.at("document"));
        doc.fetched_at = fetched;
        return CachedArticle{std::move(doc), age > std::chrono::hours(24 * 7)};
    } catch (const json::exception&) { return std::nullopt; }
}

bool WikiCache::write(std::string_view title, const std::optional<Document>& document,
                      std::string_view package_name) const {
    const auto name = filename(title);
    if (name.empty() || !root_.is_absolute()) return false;
    if (!core::ensure_private_dir(root_)) return false;
    json data = {{"version", 1}, {"key", title}, {"package", package_name}, {"negative", !document},
                 {"fetched_at", std::chrono::duration_cast<std::chrono::seconds>(clock_.now().time_since_epoch()).count()}};
    if (document) data["document"] = encode_document(*document);
    const auto bytes = data.dump();
    return bytes.size() <= 4 * 1024 * 1024 && core::atomic_write(root_ / name, bytes, 0600).has_value();
}

}
