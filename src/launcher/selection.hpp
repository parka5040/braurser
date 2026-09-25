#ifndef BRAURSER_LAUNCHER_SELECTION_HPP
#define BRAURSER_LAUNCHER_SELECTION_HPP

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace braurser::launcher {

std::optional<std::string> select_ui(std::string_view environment, std::string_view setting,
                                     std::string_view desktop);
struct Selection {
    std::filesystem::path binary;
    bool fallback{};
};
std::optional<Selection> select_binary(const std::filesystem::path& directory, std::string_view ui);

}

#endif
