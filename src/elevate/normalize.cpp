#include "normalize.hpp"

#include "../pacman-helper/request.hpp"

#include <array>
#include <string_view>

namespace braurser::elevate {
namespace {
struct Alias { std::string_view input; std::string_view output; };
constexpr std::array aliases{
    Alias{"-S", "--sync"}, Alias{"-y", "--refresh"}, Alias{"-u", "--sysupgrade"},
    Alias{"-R", "--remove"}, Alias{"-n", "--nosave"}, Alias{"-s", "--recursive"},
    Alias{"-U", "--upgrade"}, Alias{"-D", "--database"},
};
Normalized denied(std::string reason) { return {{}, 0, std::move(reason)}; }
}

Normalized normalize(std::span<const std::string> input) {
    if (input.size() < 3 || input.size() > 530 ||
        (input[0] != "pacman" && input[0] != "/usr/bin/pacman")) return denied("program");
    std::vector<std::string> flags;
    std::vector<std::string> targets;
    bool separator = false, color = false, progress = false, noconfirm = false, config = false;
    for (size_t i = 1; i < input.size(); ++i) {
        const auto& item = input[i];
        if (item.empty() || item.size() > 4096) return denied("argument");
        if (separator) { targets.push_back(item); continue; }
        if (item == "--") { separator = true; continue; }
        if (item == "--config") {
            if (config || i + 1 >= input.size() || input[++i] != "/etc/pacman.conf") return denied("config");
            config = true;
            continue;
        }
        if (item == "--color" || item == "--color=never") {
            if (color || (item == "--color" && (i + 1 >= input.size() || input[++i] != "never"))) return denied("color");
            color = true;
            continue;
        }
        if (item == "--noprogressbar") {
            if (progress) return denied("duplicate");
            progress = true;
            continue;
        }
        if (item == "--noconfirm") { noconfirm = true; continue; }
        if (item == "-q") { flags.push_back("-q"); continue; }
        std::string_view option = item;
        for (const auto& alias : aliases) if (option == alias.input) { option = alias.output; break; }
        flags.emplace_back(option);
    }
    if (!separator) return denied("separator");
    if (flags.empty()) return denied("operation");
    bool db = flags.front() == "--database";
    auto quiet = flags.begin();
    while (quiet != flags.end()) {
        if (*quiet != "-q") { ++quiet; continue; }
        if (!db) return denied("quiet");
        quiet = flags.erase(quiet);
    }
    if (flags.empty()) return denied("operation");
    std::vector<std::string> output{flags.front()};
    auto append = [&](const std::string& flag) {
        for (size_t i = 1; i < flags.size(); ++i) if (flags[i] == flag) {
            output.push_back(flag);
            flags.erase(flags.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    };
    if (output[0] == "--sync") {
        if (append("--refresh")) {
            if (!append("--sysupgrade")) return denied("refresh");
            if (noconfirm) output.emplace_back("--noconfirm");
            append("--needed");
        } else {
            if (!append("--needed")) return denied("needed");
            append("--asdeps");
            if (noconfirm) output.emplace_back("--noconfirm");
        }
    } else if (output[0] == "--remove") {
        if (!append("--nosave") || !append("--recursive")) return denied("remove");
        if (noconfirm) output.emplace_back("--noconfirm");
    } else if (output[0] == "--upgrade") {
        append("--needed");
        if (noconfirm) output.emplace_back("--noconfirm");
    } else if (output[0] == "--database") {
        if (!append("--asdeps") && !append("--asexplicit")) return denied("database");
        if (noconfirm) output.emplace_back("--noconfirm");
    } else return denied("operation");
    if (flags.size() != 1) return denied("option");
    if (color) output.emplace_back("--color=never");
    if (progress) output.emplace_back("--noprogressbar");
    if (!targets.empty() || output[0] != "--sync" ||
        (output.size() >= 2 && output[1] != "--refresh")) {
        output.emplace_back("--");
        output.insert(output.end(), targets.begin(), targets.end());
    }
    std::vector<const char*> raw;
    raw.reserve(output.size());
    for (const auto& arg : output) raw.push_back(arg.c_str());
    br_request req{};
    if (!br_parse_request(static_cast<int>(raw.size()), raw.data(), &req)) return denied("allowlist");
    return {std::move(output), req.operation, {}};
}

bool allowed_for_job(const Normalized& request, std::string_view job_dir) {
    if (!request || job_dir.empty()) return false;
    if (request.operation == 'A' || request.operation == 'E') return true;
    if (request.operation != 'D') return false;
    std::vector<const char*> raw;
    for (const auto& arg : request.args) raw.push_back(arg.c_str());
    br_request parsed{};
    if (!br_parse_request(static_cast<int>(raw.size()), raw.data(), &parsed)) return false;
    for (size_t i = 0; i < parsed.count; ++i) {
        std::string_view path = parsed.targets[i];
        if (path.size() <= job_dir.size() + 1 || path.substr(0, job_dir.size()) != job_dir ||
            path[job_dir.size()] != '/') return false;
    }
    return true;
}
}
