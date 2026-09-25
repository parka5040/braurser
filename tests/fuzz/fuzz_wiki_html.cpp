#include <braurser/daemon/wiki/sanitizer.hpp>

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>

using namespace braurser::daemon::wiki;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto doc = sanitize(std::string_view(reinterpret_cast<const char*>(data), size));
    if (doc.blocks.size() > max_blocks) std::abort();
    const auto check = [](const std::vector<Inline>& inlines) {
        std::size_t length = 0;
        for (const auto& part : inlines) {
            if (const auto* link = std::get_if<Link>(&part)) {
                if (!link->href.starts_with("https://")) std::abort();
                length += link->label.size();
            } else length += std::get<Text>(part).s.size();
        }
        if (length > max_block_text) std::abort();
    };
    for (const auto& block : doc.blocks) {
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Heading> || std::is_same_v<T, Code>) {
                if (value.text.size() > max_block_text) std::abort();
            } else if constexpr (std::is_same_v<T, List>) {
                std::size_t total = 0;
                for (const auto& item : value.items) {
                    check(item);
                    total += inline_text(item).size();
                }
                if (total > max_block_text) std::abort();
            } else check(value.inlines);
        }, block);
    }
    return 0;
}
