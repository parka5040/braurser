#include "selection.hpp"
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::launcher {
namespace {
bool executable(const std::filesystem::path& path) {
    struct stat info{};
    return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && access(path.c_str(), X_OK) == 0;
}
}

std::optional<std::string> select_ui(std::string_view environment, std::string_view setting,
                                     std::string_view desktop) {
    if (!environment.empty()) {
        if (environment != "gtk" && environment != "qt") return std::nullopt;
        return std::string(environment);
    }
    if (setting != "auto") {
        if (setting != "gtk" && setting != "qt") return std::nullopt;
        return std::string(setting);
    }
    if (desktop.size() > 4096) return std::nullopt;
    for (std::size_t offset = 0; offset < desktop.size();) {
        const auto end = desktop.find(':', offset);
        const auto part = desktop.substr(offset, end == desktop.npos ? desktop.npos : end - offset);
        std::string name(part);
        std::ranges::transform(name, name.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (name.find("kde") != std::string::npos || name.find("lxqt") != std::string::npos) return "qt";
        if (end == desktop.npos) break;
        offset = end + 1;
    }
    return "gtk";
}

std::optional<Selection> select_binary(const std::filesystem::path& directory, std::string_view ui) {
    if (ui != "gtk" && ui != "qt") return std::nullopt;
    const auto primary = directory / (ui == "gtk" ? "braurser-gtk" : "braurser-qt");
    if (executable(primary)) return Selection{primary, false};
    const auto alternate = directory / (ui == "gtk" ? "braurser-qt" : "braurser-gtk");
    if (executable(alternate)) return Selection{alternate, true};
    return std::nullopt;
}

}
