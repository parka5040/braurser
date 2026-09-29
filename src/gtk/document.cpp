#include <braurser/gtk/kit.hpp>
#include <glibmm/markup.h>
#include <algorithm>
#include <memory>
#include <stdexcept>

namespace braurser::gtk::kit {
namespace {
constexpr std::size_t text_limit = 65536;
constexpr std::size_t document_limit = 1048576;
constexpr std::size_t code_limit = 262144;

bool hidden_format(gunichar character) {
    return character == 0x061C || (character >= 0x200B && character <= 0x200F) ||
           (character >= 0x202A && character <= 0x202E) || (character >= 0x2060 && character <= 0x2064) ||
           (character >= 0x2066 && character <= 0x2069) || character == 0xFEFF;
}

std::string escaped(std::string_view value) {
    return Glib::Markup::escape_text(safe_text(value));
}

Gtk::Label& prose(Gtk::Box& parent, const std::string& markup) {
    auto* label = Gtk::make_managed<Gtk::Label>();
    label->set_markup(markup);
    label->set_wrap(true);
    label->set_wrap_mode(Pango::WrapMode::WORD_CHAR);
    label->set_xalign(0);
    label->set_selectable(true);
    label->set_hexpand(true);
    label->set_max_width_chars(72);
    parent.append(*label);
    return *label;
}

std::string sanitized(std::string_view text, std::size_t limit, bool code) {
    if (text.empty()) { return {}; }
    if (text.size() > limit) {
        throw std::length_error("kit text exceeds limit");
    }
    std::string result;
    result.reserve(text.size());
    const char* current = text.data();
    const char* end = current + text.size();
    while (current != end) {
        const auto character = g_utf8_get_char_validated(current, end - current);
        if (character == static_cast<gunichar>(-1) || character == static_cast<gunichar>(-2)) {
            result += "\xEF\xBF\xBD";
            ++current;
        } else if ((character < 32 && character != '\n' && character != '\r' && character != '\t') ||
                   character == 0xFFFE || character == 0xFFFF || (code && hidden_format(character))) {
            result += "\xEF\xBF\xBD";
            current = g_utf8_next_char(current);
        } else {
            const char* next = g_utf8_next_char(current);
            result.append(current, static_cast<std::size_t>(next - current));
            current = next;
        }
    }
    return result;
}
}

std::string safe_text(std::string_view text) {
    return sanitized(text, text_limit, false);
}

std::string safe_code(std::string_view text) {
    return sanitized(text, code_limit, true);
}

bool allowed_link(std::string_view href) {
    if (href.size() > 4096 || !href.starts_with("https://") ||
        !g_utf8_validate(href.data(), static_cast<gssize>(href.size()), nullptr) ||
        std::ranges::any_of(href, [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; })) {
        return false;
    }
    const std::string value(href);
    std::unique_ptr<GUri, decltype(&g_uri_unref)> uri(g_uri_parse(value.c_str(), G_URI_FLAGS_NONE, nullptr), g_uri_unref);
    return uri && g_uri_get_host(uri.get()) && *g_uri_get_host(uri.get()) &&
           !g_uri_get_userinfo(uri.get());
}

std::string build_markup(std::span<const TextRun> runs) {
    if (runs.size() > 1024) {
        throw std::length_error("too many document runs");
    }
    std::string result;
    std::size_t input_size = 0;
    for (const auto& run : runs) {
        input_size += run.text.size() + run.href.size();
        if (input_size > document_limit) {
            throw std::length_error("document markup exceeds limit");
        }
        auto text = escaped(run.text);
        if (run.code) { text = "<tt>" + text + "</tt>"; }
        if (run.emphasis) { text = "<i>" + text + "</i>"; }
        if (run.strong) { text = "<b>" + text + "</b>"; }
        if (allowed_link(run.href)) {
            text = "<a href=\"" + escaped(run.href) + "\">" + text + "</a>";
        }
        result += text;
    }
    return result;
}

DocumentView::DocumentView(std::span<const DocumentBlock> document, std::string_view copy_label)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 16) {
    if (document.size() > 256) {
        throw std::length_error("too many document blocks");
    }
    std::size_t size = 0;
    for (const auto& block : document) {
        size += block.text.size();
        const auto count = [&size](const auto& runs) {
            if (runs.size() > 1024) { throw std::length_error("too many document runs"); }
            for (const auto& run : runs) { size += run.text.size() + run.href.size(); }
        };
        count(block.runs);
        if (block.items.size() > 256) { throw std::length_error("too many list items"); }
        for (const auto& item : block.items) { count(item); }
        if (size > document_limit) { throw std::length_error("document exceeds limit"); }
    }
    for (const auto& block : document) {
        switch (block.kind) {
        case BlockKind::Heading: {
            if (block.level < 1 || block.level > 6) { throw std::invalid_argument("invalid heading level"); }
            auto& label = prose(*this, "<b>" + escaped(block.text) + "</b>");
            label.add_css_class(block.level == 1 ? "page-title" : "section-title");
            label.property_accessible_role() = Gtk::Accessible::Role::HEADING;
            gtk_accessible_update_property(GTK_ACCESSIBLE(label.gobj()), GTK_ACCESSIBLE_PROPERTY_LEVEL,
                                           static_cast<int>(block.level), -1);
            break;
        }
        case BlockKind::Paragraph:
            prose(*this, build_markup(block.runs));
            break;
        case BlockKind::List: {
            auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
            list->property_accessible_role() = Gtk::Accessible::Role::LIST;
            for (const auto& item : block.items) {
                auto& label = prose(*list, "• " + build_markup(item));
                label.property_accessible_role() = Gtk::Accessible::Role::LIST_ITEM;
            }
            append(*list);
            break;
        }
        case BlockKind::Code: {
            const auto text = safe_text(block.text);
            auto* code = Gtk::make_managed<Gtk::Label>(text);
            code->set_selectable(true);
            code->set_xalign(0);
            code->add_css_class("code");
            auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
            scroll->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::NEVER);
            scroll->set_child(*code);
            scroll->set_propagate_natural_width(false);
            scroll->set_min_content_height(44);
            append(*scroll);
            auto* copy = Gtk::make_managed<Gtk::Button>(safe_text(copy_label));
            copy->set_size_request(44, 44);
            copy->set_halign(Gtk::Align::START);
            copy->signal_clicked().connect([copy, text] { copy->get_clipboard()->set_text(text); });
            append(*copy);
            break;
        }
        case BlockKind::Note:
        case BlockKind::Warning: {
            auto* note = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
            note->add_css_class(block.kind == BlockKind::Warning ? "banner-warning" : "banner-info");
            note->set_margin(8);
            prose(*note, "<b>" + escaped(block.text) + "</b>");
            prose(*note, build_markup(block.runs));
            append(*note);
            break;
        }
        default:
            throw std::invalid_argument("unknown document block");
        }
    }
}

}
