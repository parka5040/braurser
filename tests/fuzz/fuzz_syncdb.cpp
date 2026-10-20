#include <braurser/daemon/catalog/sources.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace braurser::daemon::catalog;
    if (size > 1024 * 1024) return 0;
    const auto text = std::string_view(reinterpret_cast<const char*>(data), size);
    (void)parse_repo_desc(text, "core");
    (void)SyncDbReader{}.read_bytes(std::as_bytes(std::span(data, size)), "core", [](RepoPackage&&) {});
    return 0;
}
