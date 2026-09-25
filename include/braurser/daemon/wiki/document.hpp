#ifndef BRAURSER_DAEMON_WIKI_DOCUMENT_HPP
#define BRAURSER_DAEMON_WIKI_DOCUMENT_HPP

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace braurser::daemon::wiki {

inline constexpr std::size_t max_blocks = 2000;
inline constexpr std::size_t max_block_text = 64 * 1024;
inline constexpr std::string_view attribution_text =
    "ArchWiki contributors — GNU Free Documentation License 1.3 or later";

struct Text {
    std::string s;
    bool emphasis{false};
    bool strong{false};
    bool code{false};
};
struct Link { std::string label; std::string href; };
using Inline = std::variant<Text, Link>;
struct Heading { int level; std::string text; std::string anchor; };
struct Paragraph { std::vector<Inline> inlines; };
struct List { bool ordered; std::vector<std::vector<Inline>> items; };
struct Code { std::string text; };
struct Note { std::vector<Inline> inlines; };
struct Warning { std::vector<Inline> inlines; };
using Block = std::variant<Heading, Paragraph, List, Code, Note, Warning>;
struct Section { int level; std::string title; std::string anchor; };
struct Document {
    std::string title;
    std::string source_url;
    std::chrono::system_clock::time_point fetched_at{};
    std::vector<Block> blocks;
    std::vector<Section> sections;
};

}

#endif
