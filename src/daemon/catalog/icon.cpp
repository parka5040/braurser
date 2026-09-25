#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/validate/validate.hpp>

#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::catalog {
namespace {

bool png(std::string_view bytes) {
    static constexpr unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || std::memcmp(bytes.data(), signature, sizeof(signature)) != 0 ||
        std::string_view(bytes.data() + 12, 4) != "IHDR" ||
        static_cast<unsigned char>(bytes[11]) != 13 || bytes[8] || bytes[9] || bytes[10]) return false;
    auto dimension = [&](std::size_t offset) {
        return (static_cast<unsigned>(static_cast<unsigned char>(bytes[offset])) << 24) |
               (static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + 1])) << 16) |
               (static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + 2])) << 8) |
                static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + 3]));
    };
    const auto width = dimension(16);
    const auto height = dimension(20);
    return width > 0 && height > 0 && width <= 512 && height <= 512;
}

}

std::expected<std::filesystem::path, CatalogError> IconConverter::convert(const std::filesystem::path& input,
                                                                             const std::filesystem::path& cache_root) const {
    if (!input.is_absolute() || !cache_root.is_absolute() ||
        (input.extension() != ".jxl" && input.extension() != ".png"))
        return std::unexpected(CatalogError{"icon-input", {}});
    auto bytes = core::read_bounded(input, 2 * 1024 * 1024);
    if (!bytes) return std::unexpected(CatalogError{"icon-read", {}});
    struct stat info{};
    const int fd = open(input.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return std::unexpected(CatalogError{"icon-stat", {}});
    const int status = fstat(fd, &info);
    close(fd);
    if (status != 0 || !S_ISREG(info.st_mode)) return std::unexpected(CatalogError{"icon-stat", {}});
    const auto directory = cache_root / "braurser" / "icons";
    if (!core::ensure_private_dir(cache_root / "braurser") || !core::ensure_private_dir(directory))
        return std::unexpected(CatalogError{"icon-dir", {}});
    const auto identity = input.string() + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
                          std::to_string(info.st_mtim.tv_nsec);
    br_sha256_ctx context{};
    br_sha256_init(&context);
    br_sha256_update(&context, reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size());
    std::array<std::uint8_t, 32> hash{};
    br_sha256_final(&context, hash.data());
    std::array<char, 65> hex{};
    br_hex32(hash.data(), hex.data());
    const auto destination = directory / (std::string(hex.data()) + ".png");
    auto existing = core::read_bounded(destination, 8 * 1024 * 1024);
    if (existing && png(*existing)) return destination;
    if (input.extension() == ".png") {
        if (!png(*bytes) || !core::atomic_write(destination, *bytes))
            return std::unexpected(CatalogError{"icon-png", {}});
        return destination;
    }
    auto temporary = (directory / ".decode-XXXXXX").string();
    if (!mkdtemp(temporary.data())) return std::unexpected(CatalogError{"icon-temp", {}});
    const auto output = std::filesystem::path(temporary) / "decoded.png";
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/djxl";
    spec.args = {input.string(), output.string()};
    spec.timeout = std::chrono::seconds(10);
    spec.max_stdout = spec.max_stderr = 64 * 1024;
    auto process = runner_.run(spec);
    auto decoded = core::read_bounded(output, 8 * 1024 * 1024);
    auto cleanup = core::remove_tree_confined(directory, temporary);
    if (!process || process->exit_code != 0 || process->timed_out || process->signaled ||
        !decoded || !png(*decoded) || !cleanup || !core::atomic_write(destination, *decoded))
        return std::unexpected(CatalogError{"icon-decode", {}});
    return destination;
}

}
