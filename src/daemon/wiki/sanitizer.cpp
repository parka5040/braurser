#include <braurser/daemon/wiki/sanitizer.hpp>
#include <braurser/daemon/core/text.hpp>

#include <gumbo.h>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace braurser::daemon::wiki {
namespace {

std::string clean(std::string_view input) {
    std::string result;
    result.reserve(std::min(input.size(), max_block_text));
    bool space = false;
    for (std::size_t i = 0; i < input.size() && result.size() < max_block_text; ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == 0xc2 && i + 1 < input.size() &&
            (static_cast<unsigned char>(input[i + 1]) == 0xa0 ||
             (static_cast<unsigned char>(input[i + 1]) >= 0x80 && static_cast<unsigned char>(input[i + 1]) <= 0x9f))) {
            if (static_cast<unsigned char>(input[i + 1]) == 0xa0) space = true;
            ++i;
            continue;
        }
        if (std::isspace(c)) { space = true; continue; }
        if (c < 32 || c == 127) continue;
        if (space && !result.empty() && result.size() < max_block_text) result.push_back(' ');
        space = false;
        result.push_back(static_cast<char>(c));
    }
    if (!result.empty() && result.back() == ' ') result.pop_back();
    if (result.size() == max_block_text && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) {
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) result.pop_back();
        if (!result.empty()) result.pop_back();
    }
    return result;
}

const GumboVector* children(const GumboNode* node) {
    if (node->type == GUMBO_NODE_ELEMENT) return &node->v.element.children;
    if (node->type == GUMBO_NODE_DOCUMENT) return &node->v.document.children;
    return nullptr;
}

std::string_view attribute(const GumboNode* node, const char* key) {
    if (node->type != GUMBO_NODE_ELEMENT) return {};
    const auto* a = gumbo_get_attribute(&node->v.element.attributes, key);
    return a ? std::string_view(a->value) : std::string_view{};
}

bool has_class(const GumboNode* node, std::string_view target) {
    const auto classes = attribute(node, "class");
    for (std::size_t start = 0; start < classes.size();) {
        while (start < classes.size() && std::isspace(static_cast<unsigned char>(classes[start]))) ++start;
        auto end = start;
        while (end < classes.size() && !std::isspace(static_cast<unsigned char>(classes[end]))) ++end;
        if (classes.substr(start, end - start) == target) return true;
        start = end;
    }
    return false;
}

bool excluded(const GumboNode* node) {
    if (node->type == GUMBO_NODE_COMMENT || node->type == GUMBO_NODE_TEMPLATE) return true;
    if (node->type != GUMBO_NODE_ELEMENT) return false;
    switch (node->v.element.tag) {
        case GUMBO_TAG_SCRIPT: case GUMBO_TAG_STYLE: case GUMBO_TAG_NOSCRIPT:
        case GUMBO_TAG_FORM: case GUMBO_TAG_INPUT: case GUMBO_TAG_BUTTON:
        case GUMBO_TAG_IFRAME: case GUMBO_TAG_OBJECT: case GUMBO_TAG_EMBED:
        case GUMBO_TAG_SVG: case GUMBO_TAG_MATH: case GUMBO_TAG_IMG:
        case GUMBO_TAG_AUDIO: case GUMBO_TAG_VIDEO: return true;
        default: break;
    }
    return (node->v.element.tag == GUMBO_TAG_SUP && has_class(node, "reference")) ||
           (node->v.element.tag == GUMBO_TAG_SPAN && has_class(node, "mw-editsection")) ||
           (node->v.element.tag == GUMBO_TAG_DIV &&
            (has_class(node, "toc") || has_class(node, "archwiki-template-meta-related-articles"))) ||
           (node->v.element.tag == GUMBO_TAG_TABLE && has_class(node, "navbox"));
}

std::optional<std::string> safe_link(std::string_view href) {
    if (href.empty() || href.size() > 8192 || href.starts_with("//") ||
        std::any_of(href.begin(), href.end(), [](unsigned char c) { return c <= 32 || c == 127; })) return std::nullopt;
    const auto colon = href.find(':');
    const auto separator = href.find_first_of("/?#");
    if (colon != std::string_view::npos && (separator == std::string_view::npos || colon < separator) &&
        !href.starts_with("https://")) return std::nullopt;
    CURLU* raw = curl_url();
    if (!raw) return std::nullopt;
    auto url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(raw, curl_url_cleanup);
    if (curl_url_set(raw, CURLUPART_URL, "https://wiki.archlinux.org/", 0) != CURLUE_OK ||
        curl_url_set(raw, CURLUPART_URL, std::string(href).c_str(), 0) != CURLUE_OK) return std::nullopt;
    char* scheme = nullptr;
    char* user = nullptr;
    char* password = nullptr;
    char* full = nullptr;
    const bool valid = curl_url_get(raw, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
                       std::string_view(scheme) == "https" &&
                       curl_url_get(raw, CURLUPART_USER, &user, 0) != CURLUE_OK &&
                       curl_url_get(raw, CURLUPART_PASSWORD, &password, 0) != CURLUE_OK &&
                       curl_url_get(raw, CURLUPART_URL, &full, 0) == CURLUE_OK &&
                       std::string_view(full).starts_with("https://") && std::strlen(full) <= 8192;
    std::optional<std::string> result;
    if (valid) result = full;
    if (scheme) curl_free(scheme);
    if (user) curl_free(user);
    if (password) curl_free(password);
    if (full) curl_free(full);
    return result;
}

void append_text(std::vector<Inline>& out, std::string_view raw, bool em, bool strong, bool code) {
    const bool leading = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()));
    const bool trailing = !raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()));
    auto value = clean(raw);
    if (leading && !out.empty()) {
        auto& last = out.back();
        if (auto* text = std::get_if<Text>(&last)) {
            if (!text->s.empty() && text->s.back() != ' ') text->s += ' ';
        } else value.insert(value.begin(), ' ');
    }
    if (trailing && !value.empty() && value.back() != ' ') value += ' ';
    if (value.empty()) return;
    if (!out.empty()) {
        if (auto* last = std::get_if<Text>(&out.back()); last && last->emphasis == em &&
            last->strong == strong && last->code == code) {
            last->s += value;
            return;
        }
    }
    out.emplace_back(Text{std::move(value), em, strong, code});
}

void walk_inline(const GumboNode* node, std::vector<Inline>& out, unsigned depth,
                 bool em = false, bool strong = false, bool code = false, bool include_lists = false) {
    if (depth > 96 || excluded(node) || inline_text(out).size() >= max_block_text) return;
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_WHITESPACE || node->type == GUMBO_NODE_CDATA) {
        append_text(out, node->v.text.text, em, strong, code);
        return;
    }
    if (node->type == GUMBO_NODE_ELEMENT) {
        const auto tag = node->v.element.tag;
        if (tag == GUMBO_TAG_A) {
            std::vector<Inline> label;
            if (const auto* kids = children(node)) {
                for (unsigned i = 0; i < kids->length; ++i)
                    walk_inline(static_cast<GumboNode*>(kids->data[i]), label, depth + 1, em, strong, code, include_lists);
            }
            auto text = clean(inline_text(label));
            if (auto link = safe_link(attribute(node, "href")); link && !text.empty()) {
                out.emplace_back(Link{std::move(text), std::move(*link)});
            } else append_text(out, text, em, strong, code);
            return;
        }
        em |= tag == GUMBO_TAG_EM || tag == GUMBO_TAG_I;
        strong |= tag == GUMBO_TAG_STRONG || tag == GUMBO_TAG_B;
        code |= tag == GUMBO_TAG_CODE;
        if ((tag == GUMBO_TAG_UL || tag == GUMBO_TAG_OL) && !include_lists) return;
        if (tag == GUMBO_TAG_LI && include_lists && !out.empty())
            append_text(out, " ", em, strong, code);
    }
    if (const auto* kids = children(node)) {
        for (unsigned i = 0; i < kids->length && inline_text(out).size() < max_block_text; ++i)
            walk_inline(static_cast<GumboNode*>(kids->data[i]), out, depth + 1, em, strong, code, include_lists);
    }
}

std::vector<Inline> bounded_inline(const GumboNode* node, unsigned depth, bool include_lists = false) {
    std::vector<Inline> raw;
    walk_inline(node, raw, depth, false, false, false, include_lists);
    std::vector<Inline> result;
    std::size_t used = 0;
    for (auto& part : raw) {
        if (used >= max_block_text) break;
        const auto value = std::visit([](const auto& v) -> const std::string& {
            if constexpr (requires { v.s; }) return v.s;
            else return v.label;
        }, part);
        if (value.size() > max_block_text - used) {
            if (max_block_text - used > 3) {
                auto shortened = core::bounded_line(value, max_block_text - used - 3);
                result.emplace_back(Text{std::move(shortened), false, false, false});
            }
            break;
        }
        used += value.size();
        result.push_back(std::move(part));
    }
    return result;
}

void push(Document& doc, Block block) {
    if (doc.blocks.size() < max_blocks) doc.blocks.push_back(std::move(block));
}

void list_items(const GumboNode* node, List& list, unsigned depth, unsigned nesting, std::size_t& total) {
    if (depth > 96 || nesting > 24 || list.items.size() >= max_blocks || total >= max_block_text) return;
    const auto* kids = children(node);
    if (!kids) return;
    for (unsigned i = 0; i < kids->length && list.items.size() < max_blocks; ++i) {
        const auto* item = static_cast<GumboNode*>(kids->data[i]);
        if (excluded(item) || item->type != GUMBO_NODE_ELEMENT || item->v.element.tag != GUMBO_TAG_LI) continue;
        auto content = bounded_inline(item, depth + 1);
        if (nesting && !content.empty()) content.insert(content.begin(), Text{std::string(nesting * 2, ' '), false, false, false});
        std::size_t bytes = 0;
        for (const auto& part : content) {
            bytes += std::visit([](const auto& value) {
                if constexpr (requires { value.s; }) return value.s.size();
                else return value.label.size();
            }, part);
        }
        if (bytes > max_block_text - total) {
            std::vector<Inline> truncated;
            std::size_t remaining = max_block_text - total;
            for (const auto& part : content) {
                if (remaining == 0) break;
                if (const auto* text = std::get_if<Text>(&part)) {
                    if (text->s.size() > remaining && remaining < 4) break;
                    auto prefix = text->s.size() > remaining
                                      ? core::bounded_line(text->s, remaining - 3) : text->s;
                    remaining -= prefix.size();
                    truncated.emplace_back(Text{std::move(prefix), text->emphasis, text->strong, text->code});
                } else {
                    const auto& link = std::get<Link>(part);
                    if (link.label.size() > remaining) break;
                    remaining -= link.label.size();
                    truncated.push_back(link);
                }
            }
            content = std::move(truncated);
        }
        total += inline_text(content).size();
        if (!content.empty()) list.items.push_back(std::move(content));
        const auto* children_of_item = children(item);
        if (!children_of_item) continue;
        for (unsigned j = 0; j < children_of_item->length; ++j) {
            const auto* child = static_cast<GumboNode*>(children_of_item->data[j]);
            if (child->type == GUMBO_NODE_ELEMENT &&
                (child->v.element.tag == GUMBO_TAG_UL || child->v.element.tag == GUMBO_TAG_OL))
                list_items(child, list, depth + 1, nesting + 1, total);
        }
    }
}

void table_rows(const GumboNode* node, Document& doc, unsigned depth) {
    if (depth > 96 || doc.blocks.size() >= max_blocks || excluded(node)) return;
    if (node->type == GUMBO_NODE_ELEMENT && node->v.element.tag == GUMBO_TAG_TR) {
        std::string row;
        if (const auto* kids = children(node)) {
            for (unsigned i = 0; i < kids->length; ++i) {
                const auto* cell = static_cast<GumboNode*>(kids->data[i]);
                if (cell->type != GUMBO_NODE_ELEMENT ||
                    (cell->v.element.tag != GUMBO_TAG_TH && cell->v.element.tag != GUMBO_TAG_TD)) continue;
                auto value = clean(inline_text(bounded_inline(cell, depth + 1)));
                if (!value.empty()) {
                    if (!row.empty()) row += " — ";
                    row += value;
                }
            }
        }
        row = clean(row);
        if (!row.empty()) push(doc, Paragraph{{Text{std::move(row), false, false, false}}});
        return;
    }
    if (const auto* kids = children(node))
        for (unsigned i = 0; i < kids->length; ++i) table_rows(static_cast<GumboNode*>(kids->data[i]), doc, depth + 1);
}

void walk_blocks(const GumboNode* node, Document& doc, unsigned depth) {
    if (depth > 96 || doc.blocks.size() >= max_blocks || excluded(node)) return;
    if (node->type == GUMBO_NODE_ELEMENT) {
        const auto tag = node->v.element.tag;
        if (tag == GUMBO_TAG_H2 || tag == GUMBO_TAG_H3 || tag == GUMBO_TAG_H4) {
            const int level = tag == GUMBO_TAG_H2 ? 2 : tag == GUMBO_TAG_H3 ? 3 : 4;
            auto text = clean(inline_text(bounded_inline(node, depth)));
            auto anchor = clean(attribute(node, "id"));
            if (!text.empty()) {
                doc.sections.push_back(Section{level, text, anchor});
                push(doc, Heading{level, std::move(text), std::move(anchor)});
            }
            return;
        }
        if (tag == GUMBO_TAG_P || tag == GUMBO_TAG_DT || tag == GUMBO_TAG_DD) {
            auto inlines = bounded_inline(node, depth);
            if (!inline_text(inlines).empty()) push(doc, Paragraph{std::move(inlines)});
            return;
        }
        if (tag == GUMBO_TAG_UL || tag == GUMBO_TAG_OL) {
            List list{tag == GUMBO_TAG_OL, {}};
            std::size_t total = 0;
            list_items(node, list, depth, 0, total);
            if (!list.items.empty()) push(doc, std::move(list));
            return;
        }
        if (tag == GUMBO_TAG_PRE) {
            auto text = clean(inline_text(bounded_inline(node, depth)));
            if (!text.empty()) push(doc, Code{std::move(text)});
            return;
        }
        if (tag == GUMBO_TAG_TABLE) { table_rows(node, doc, depth); return; }
        if (has_class(node, "archwiki-template-box")) {
            auto inlines = bounded_inline(node, depth, true);
            if (!inline_text(inlines).empty()) {
                if (has_class(node, "archwiki-template-box-warning")) push(doc, Warning{std::move(inlines)});
                else if (has_class(node, "archwiki-template-box-note") || has_class(node, "archwiki-template-box-tip"))
                    push(doc, Note{std::move(inlines)});
                else push(doc, Paragraph{std::move(inlines)});
            }
            return;
        }
    }
    if (const auto* kids = children(node))
        for (unsigned i = 0; i < kids->length && doc.blocks.size() < max_blocks; ++i)
            walk_blocks(static_cast<GumboNode*>(kids->data[i]), doc, depth + 1);
}

}

std::string inline_text(const std::vector<Inline>& inlines) {
    std::string result;
    for (const auto& part : inlines) {
        const auto& value = std::visit([](const auto& v) -> const std::string& {
            if constexpr (requires { v.s; }) return v.s;
            else return v.label;
        }, part);
        if (result.size() >= max_block_text) break;
        result.append(value, 0, std::min(value.size(), max_block_text - result.size()));
    }
    return result;
}

Document sanitize(std::string_view html) {
    Document result;
    if (html.size() > 2 * 1024 * 1024) return result;
    auto* raw = gumbo_parse_with_options(&kGumboDefaultOptions, html.data(), html.size());
    if (!raw) return result;
    auto tree = std::unique_ptr<GumboOutput, void(*)(GumboOutput*)>(raw, [](GumboOutput* output) {
        gumbo_destroy_output(&kGumboDefaultOptions, output);
    });
    walk_blocks(tree->root, result, 0);
    return result;
}

}
