#ifndef BRAURSER_DAEMON_CORE_TEXT_HPP
#define BRAURSER_DAEMON_CORE_TEXT_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace braurser::daemon::core {

/// in: text may contain terminal escape sequences and C0 controls.
/// out: returns printable text with newlines and tabs preserved.
std::string strip_terminal_controls(std::string_view text);

/// in: text may contain credential fields or URL userinfo.
/// out: returns text with sensitive values replaced by [REDACTED].
std::string redact(std::string_view text);

/// in: max is the maximum number of input bytes retained.
/// out: returns a UTF-8-safe prefix and an ellipsis if shortened.
std::string bounded_line(std::string_view text, std::size_t max);

}

#endif
