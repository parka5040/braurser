#include <braurser/domain/variants.hpp>
#include <algorithm>
#include <array>
#include <string_view>

namespace braurser::domain {
namespace {

struct Suffix { std::string_view text; VariantKind kind; };
constexpr std::array suffixes{
    Suffix{"-bin", VariantKind::Bin}, Suffix{"-git", VariantKind::Git},
    Suffix{"-appimage", VariantKind::AppImage}, Suffix{"-nightly", VariantKind::Nightly},
    Suffix{"-beta", VariantKind::Beta}, Suffix{"-stable", VariantKind::Stable},
    Suffix{"-wayland", VariantKind::Wayland}
};

std::pair<std::string_view, VariantKind> variant(std::string_view name) {
    for (const auto& suffix : suffixes) {
        if (name.size() > suffix.text.size() && name.ends_with(suffix.text))
            return {name.substr(0, name.size() - suffix.text.size()), suffix.kind};
    }
    return {name, VariantKind::Standard};
}

bool development(VariantKind kind) {
    return kind == VariantKind::Git || kind == VariantKind::Nightly || kind == VariantKind::Beta;
}

int priority(const VariantOption& option) {
    const auto kind = option.kind;
    if (kind == VariantKind::Standard && option.candidate.source == SourceKind::Official) return 0;
    if (kind == VariantKind::Bin) return 1;
    if ((kind == VariantKind::Standard || kind == VariantKind::Stable) &&
        option.candidate.source == SourceKind::Community) return 2;
    if (kind == VariantKind::AppImage) return 3;
    if (kind == VariantKind::Wayland) return 4;
    return 5;
}

std::optional<std::string> normalize(std::string_view url) {
    if (url.size() > 2048) return std::nullopt;
    std::string result(url);
    const auto scheme = result.find("://");
    if (scheme != std::string::npos) {
        const auto host_end = result.find_first_of("/?#", scheme + 3);
        const auto end = host_end == std::string::npos ? result.size() : host_end;
        for (std::size_t i = 0; i < end; ++i) {
            if (result[i] >= 'A' && result[i] <= 'Z') result[i] = static_cast<char>(result[i] - 'A' + 'a');
        }
        const auto path_end = result.find_first_of("?#", end);
        const auto path_limit = path_end == std::string::npos ? result.size() : path_end;
        std::size_t trim = path_limit;
        while (trim > end && result[trim - 1] == '/') --trim;
        result.erase(trim, path_limit - trim);
    } else {
        while (!result.empty() && result.back() == '/') result.pop_back();
    }
    return result;
}

}

std::optional<VariantFamily> resolve_family(const PackageName& requested,
                                            std::span<const VariantCandidate> available) {
    const auto [base, ignored] = variant(requested.value());
    static_cast<void>(ignored);
    VariantFamily family{std::string(base), {}, std::nullopt, false};
    for (const auto& candidate : available) {
        const auto name = candidate.name.value();
        if (name == base) {
            family.options.push_back({candidate, VariantKind::Standard, false});
            continue;
        }
        for (const auto& suffix : suffixes) {
            if (name.size() == base.size() + suffix.text.size() && name.starts_with(base) &&
                name.ends_with(suffix.text)) {
                family.options.push_back({candidate, suffix.kind, development(suffix.kind)});
                break;
            }
        }
    }
    if (family.options.size() < 2) return std::nullopt;
    std::sort(family.options.begin(), family.options.end(), [](const auto& a, const auto& b) {
        if (a.development != b.development) return !a.development;
        if (!a.development && priority(a) != priority(b)) return priority(a) < priority(b);
        if (a.candidate.name.value() != b.candidate.name.value())
            return a.candidate.name.value() < b.candidate.name.value();
        return a.candidate.source < b.candidate.source;
    });
    for (std::size_t i = 0; i < family.options.size(); ++i) {
        if (priority(family.options[i]) < 5) {
            family.recommended = i;
            break;
        }
    }
    std::optional<std::string> url;
    for (const auto& option : family.options) {
        if (option.candidate.upstream_url.empty()) continue;
        const auto current = normalize(option.candidate.upstream_url);
        if (!current) {
            family.urls_differ = true;
            continue;
        }
        if (url && *url != *current) family.urls_differ = true;
        if (!url) url = *current;
    }
    return family;
}

}
