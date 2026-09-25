#ifndef BRAURSER_DOMAIN_VERCMP_HPP
#define BRAURSER_DOMAIN_VERCMP_HPP

#include <string_view>

namespace braurser::domain {

/// in: a and b are version strings.
/// out: -1, 0 or 1 using pacman version ordering.
int vercmp(std::string_view a, std::string_view b);

}

#endif
