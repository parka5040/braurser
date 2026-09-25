#ifndef BRAURSER_DAEMON_CORE_FILES_HPP
#define BRAURSER_DAEMON_CORE_FILES_HPP

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace braurser::daemon::core {

struct XdgPaths {
    std::filesystem::path cache;
    std::filesystem::path state;
    std::filesystem::path config;
    std::filesystem::path runtime;
};

struct FileError {
    int code{};
    std::string message;
};

/// out: resolves absolute XDG paths with HOME-based defaults.
/// err: returns an error if a required path is missing or relative.
std::expected<XdgPaths, FileError> resolve_xdg_paths();

/// in: path is an absolute directory path.
/// out: creates missing components privately and verifies the target owner and mode.
/// err: rejects symlinks, foreign ownership and non-directories.
std::expected<void, FileError> ensure_private_dir(const std::filesystem::path& path);

/// in: path is an absolute file path and mode grants no group or other access.
/// out: atomically replaces the file and syncs its directory.
std::expected<void, FileError> atomic_write(const std::filesystem::path& path,
                                              std::string_view bytes, mode_t mode = 0600);

/// in: path is an absolute file path and max_bytes is the maximum accepted length.
/// out: returns the file contents when within the limit.
/// err: symlinks are rejected unless explicitly permitted.
std::expected<std::string, FileError> read_bounded(const std::filesystem::path& path,
                                                     std::size_t max_bytes, bool follow_symlinks = false);

/// in: sub lies strictly beneath root without parent traversal.
/// out: removes the sub-tree without following symlinks.
/// err: rejects escape attempts and symbolic root directories.
std::expected<void, FileError> remove_tree_confined(const std::filesystem::path& root,
                                                      const std::filesystem::path& sub);

}

#endif
