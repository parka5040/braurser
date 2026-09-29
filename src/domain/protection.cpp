#include <braurser/domain/protection.hpp>

#include <algorithm>
#include <array>
#include <set>

namespace braurser::domain {
namespace {

constexpr std::array<std::string_view, 9> essentials{
    "base", "filesystem", "glibc", "pacman", "systemd", "bash", "coreutils", "sudo", "polkit"};

bool kernel_line(std::string_view line) {
    constexpr std::string_view prefix = "usr/lib/modules/";
    constexpr std::string_view suffix = "/vmlinuz";
    if (!line.starts_with(prefix) || !line.ends_with(suffix) ||
        line.size() <= prefix.size() + suffix.size()) return false;
    const auto release = line.substr(prefix.size(), line.size() - prefix.size() - suffix.size());
    return release.find('/') == std::string_view::npos && release != "." && release != "..";
}

}

bool installs_kernel_image(std::string_view files) {
    for (std::size_t start = 0; start < files.size();) {
        auto end = files.find('\n', start);
        if (end == std::string_view::npos) end = files.size();
        auto line = files.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (kernel_line(line)) return true;
        start = end + 1;
    }
    return false;
}

std::vector<std::string> protected_removals(std::span<const std::string> removals,
                                            std::span<const std::string> hold,
                                            std::span<const std::string> kernels) {
    const std::set<std::string, std::less<>> removing(removals.begin(), removals.end());
    const bool every_kernel = !kernels.empty() && std::ranges::all_of(kernels, [&](const std::string& name) {
        return removing.contains(name);
    });
    std::set<std::string, std::less<>> guarded(hold.begin(), hold.end());
    guarded.insert(essentials.begin(), essentials.end());
    if (every_kernel) guarded.insert(kernels.begin(), kernels.end());
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const auto& name : removals)
        if (guarded.contains(name) && seen.insert(name).second) result.push_back(name);
    return result;
}

}
