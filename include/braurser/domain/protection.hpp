#ifndef BRAURSER_DOMAIN_PROTECTION_HPP
#define BRAURSER_DOMAIN_PROTECTION_HPP

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::domain {

/// in: files is the text of one pacman local database files record.
/// out: true only when it installs usr/lib/modules/<release>/vmlinuz.
bool installs_kernel_image(std::string_view files);

/// in: removals is the complete removal set; hold is HoldPkg; kernels lists every installed kernel package.
/// out: removal names that must not be removed, in removal order without duplicates.
std::vector<std::string> protected_removals(std::span<const std::string> removals,
                                            std::span<const std::string> hold,
                                            std::span<const std::string> kernels);

}

#endif
