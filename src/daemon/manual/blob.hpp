#ifndef BRAURSER_DAEMON_MANUAL_BLOB_HPP
#define BRAURSER_DAEMON_MANUAL_BLOB_HPP

#include <git2.h>
#include <cstdint>
#include <optional>

namespace braurser::daemon::manual::internal {

std::optional<std::uint64_t> blob_size(git_repository* repo, const git_oid& oid);

}

#endif
