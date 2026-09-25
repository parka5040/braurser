#ifndef BRAURSER_DOMAIN_VARIANTS_HPP
#define BRAURSER_DOMAIN_VARIANTS_HPP

#include <braurser/domain/types.hpp>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace braurser::domain {

enum class VariantKind { Standard, Bin, Git, AppImage, Nightly, Beta, Stable, Wayland };

struct VariantCandidate {
    PackageName name;
    SourceKind source;
    std::string upstream_url;
};

struct VariantOption {
    VariantCandidate candidate;
    VariantKind kind;
    bool development;
};

struct VariantFamily {
    std::string base;
    std::vector<VariantOption> options;
    std::optional<std::size_t> recommended;
    bool urls_differ;
};

/// in: requested is validated; available contains validated candidates.
/// out: a sorted family of at least two variants, or nullopt.
std::optional<VariantFamily> resolve_family(const PackageName& requested,
                                            std::span<const VariantCandidate> available);

}

#endif
