#ifndef BRAURSER_DAEMON_WIKI_SANITIZER_HPP
#define BRAURSER_DAEMON_WIKI_SANITIZER_HPP

#include <braurser/daemon/wiki/document.hpp>
#include <string>
#include <string_view>

namespace braurser::daemon::wiki {

/// in: html is untrusted markup of at most two MiB.
/// out: returns bounded plain-text blocks and validated HTTPS links.
Document sanitize(std::string_view html);

/// in: inlines contain display text and validated links.
/// out: returns their concatenated text.
std::string inline_text(const std::vector<Inline>& inlines);

}

#endif
