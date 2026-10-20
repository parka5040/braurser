#include <braurser/daemon/jobs/aur.hpp>
#include <braurser/validate/validate.hpp>
#include <braurser/domain/vercmp.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <deque>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::jobs {
namespace {
Error untrusted() { return {protocol::kUntrustedInput, "job.dependency_unavailable"}; }

bool provided(std::string_view name, std::string_view version, const std::vector<std::string>& provides,
              const Dependency& dependency) {
    if (name == dependency.name && satisfies_version(version, dependency)) return true;
    return std::ranges::any_of(provides, [&](const auto& value) {
        const auto parsed = parse_dependency(value);
        return parsed && parsed->name == dependency.name &&
            (dependency.comparison.empty() || (parsed->comparison == "=" && satisfies_version(parsed->version, dependency)));
    });
}

bool dynamic_version(std::string_view text) {
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        const auto trim = [](std::string_view value) {
            const auto first = value.find_first_not_of(" \t\r");
            return first == value.npos ? std::string_view{} : value.substr(first);
        };
        line = trim(line);
        if (line.starts_with("pkgver")) {
            line = trim(line.substr(6));
            if (line.starts_with("()")) {
                line = trim(line.substr(2));
                if (line.empty() || line.starts_with('{')) return true;
            }
        }
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return false;
}

std::string package_version(const SourcePackage& package) {
    return (package.epoch.empty() || package.epoch == "0" ? "" : package.epoch + ":") +
        package.pkgver + "-" + package.pkgrel;
}
}

Result<void> resolve_aur(Snapshot& snapshot, std::string_view target, std::string_view architecture,
                         std::span<const catalog::LocalRecord> installed, const AurLookup& lookup,
                         const AurBase& base_lookup, const AurFetch& fetch, const OfficialLookup& official, bool official_updates) {
    if (!br_valid_pkgname(target.data(), target.size())) return std::unexpected(untrusted());
    snapshot.aur_installed.assign(installed.begin(), installed.end());
    for (const auto& local : installed) {
        snapshot.fingerprint += "\ninstalled:" + local.package.name + ":" + local.package.version + ":" +
            std::to_string(static_cast<int>(local.package.reason)) + ":" + nlohmann::json(local.provides).dump();
    }
    struct Pending { Dependency dependency; unsigned depth{}; std::string parent; std::string parent_package; bool target{}; };
    std::deque<Pending> pending{{{std::string(target), {}, {}}, 0, {}, {}, true}};
    std::map<std::string, AurPin, std::less<>> pins;
    std::map<std::string, SourceInfo, std::less<>> metadata;
    std::map<std::string, std::vector<Dependency>, std::less<>> constraints;
    std::map<std::string, std::string, std::less<>> resolved;
    std::map<std::string, std::set<std::string>, std::less<>> edges;
    std::map<std::string, std::set<std::string>, std::less<>> package_edges;
    std::map<std::string, protocol::PackageRef, std::less<>> officials;
    std::size_t inspected = 0;
    while (!pending.empty()) {
        auto request = std::move(pending.front());
        pending.pop_front();
        if (++inspected > 8192 || request.depth > 8) return std::unexpected(untrusted());
        const auto& dependency = request.dependency;
        constraints[dependency.name].push_back(dependency);
        if (!request.target && std::ranges::any_of(installed, [&](const auto& local) {
            return provided(local.package.name, local.package.version, local.provides, dependency);
        })) continue;
        auto detail = lookup(dependency.name);
        if (!request.target) {
            std::optional<catalog::RepoPackage> found;
            if (detail && detail->details.package.source.value == "official" &&
                detail->details.package.name == dependency.name && satisfies_version(detail->details.version, dependency)) {
                catalog::RepoPackage package;
                package.name = detail->details.package.name;
                package.version = detail->details.version;
                package.repo = detail->details.package.repo.value_or("");
                found = std::move(package);
            } else if (official) found = official(dependency);
            if (found) {
                if (!br_valid_pkgname(found->name.data(), found->name.size()) ||
                    !br_valid_repo_name(found->repo.data(), found->repo.size()) ||
                    !provided(found->name, found->version, found->provides, dependency)) return std::unexpected(untrusted());
                const auto key = found->repo + "/" + found->name;
                const auto previous = std::ranges::find_if(snapshot.aur_official, [&](const auto& package) {
                    return package.repo == found->repo && package.name == found->name;
                });
                if (previous == snapshot.aur_official.end()) snapshot.aur_official.push_back(*found);
                else {
                    if (previous->version != found->version) return std::unexpected(untrusted());
                    if (!found->provides.empty()) previous->provides = found->provides;
                }
                officials[key] = {{"official"}, found->repo, found->name};
                if (officials.size() > 256) return std::unexpected(untrusted());
                continue;
            }
        }
        if (!detail || detail->details.package.source.value != "community") detail = lookup("aur/" + dependency.name);
        if (!detail || detail->details.package.source.value != "community" ||
            detail->details.package.name != dependency.name) return std::unexpected(untrusted());
        const auto found_base = base_lookup(dependency.name);
        if (!found_base) return std::unexpected(untrusted());
        const auto& base = *found_base;
        if (!br_valid_pkgname(base.data(), base.size())) return std::unexpected(untrusted());
        if (!pins.contains(base)) {
            if (pins.size() >= 16) return std::unexpected(untrusted());
            auto tree = fetch(base);
            if (!tree) return std::unexpected(tree.error());
            auto info = parse_srcinfo(tree->srcinfo, architecture);
            if (!info || info->pkgbase != base) return std::unexpected(untrusted());
            const auto valid_oid = [](std::string_view value) {
                return (value.size() == 40 || value.size() == 64) && std::ranges::all_of(value, [](unsigned char ch) {
                    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
                });
            };
            if (!valid_oid(tree->commit) || !valid_oid(tree->tree)) return std::unexpected(untrusted());
            const auto pkgbuild = std::ranges::find_if(tree->files, [](const auto& file) { return file.path == "PKGBUILD"; });
            if (pkgbuild == tree->files.end() || !pkgbuild->text) return std::unexpected(untrusted());
            pins.emplace(base, AurPin{base, {}, tree->commit, tree->tree, tree->srcinfo, request.target,
                                     dynamic_version(*pkgbuild->text)});
            metadata.emplace(base, std::move(*info));
        }
        auto& pin = pins.at(base);
        const auto& packages = metadata.at(base).packages;
        const auto package = std::ranges::find_if(packages, [&](const auto& item) { return item.name == dependency.name; });
        if (package == packages.end() || !satisfies_version(package_version(*package), dependency)) return std::unexpected(untrusted());
        if (!request.parent.empty() && request.parent != base) edges[request.parent].insert(base);
        if (!request.parent_package.empty()) package_edges[request.parent_package].insert(dependency.name);
        if (request.target) {
            pin.target = true;
            snapshot.planned_version = package_version(*package);
        }
        if (resolved.contains(dependency.name)) {
            if (resolved.at(dependency.name) != base) return std::unexpected(untrusted());
            continue;
        }
        resolved.emplace(dependency.name, base);
        pin.pkgnames.push_back(dependency.name);
        for (const auto* list : {&package->depends, &package->makedepends}) {
            for (const auto& text : *list) {
                const auto parsed = parse_dependency(text);
                if (!parsed) return std::unexpected(untrusted());
                pending.push_back({*parsed, request.depth + 1, base, dependency.name, false});
            }
        }
    }
    if (official_updates && !officials.empty())
        return std::unexpected(Error{protocol::kSystemUpdateRequired, "updates.system_first"});
    std::map<std::string, unsigned, std::less<>> package_colors;
    std::map<std::string, unsigned, std::less<>> heights;
    std::function<bool(const std::string&, unsigned)> acyclic = [&](const std::string& name, unsigned depth) {
        if (depth > 8 || package_colors[name] == 1) return false;
        if (package_colors[name] == 2) return heights[name] <= 8 - depth;
        package_colors[name] = 1;
        for (const auto& child : package_edges[name]) {
            if (!acyclic(child, depth + 1)) return false;
            heights[name] = std::max(heights[name], heights[child] + 1);
            if (heights[name] > 8) return false;
        }
        package_colors[name] = 2;
        return true;
    };
    for (const auto& [name, base] : resolved) {
        static_cast<void>(base);
        if (!acyclic(name, 0)) return std::unexpected(untrusted());
    }
    std::map<std::string, unsigned, std::less<>> colors;
    std::vector<AurPin> ordered;
    std::function<bool(const std::string&, unsigned)> visit = [&](const std::string& base, unsigned depth) {
        if (depth > 8 || colors[base] == 1) return false;
        if (colors[base] == 2) return true;
        colors[base] = 1;
        for (const auto& child : edges[base]) if (!visit(child, depth + 1)) return false;
        colors[base] = 2;
        auto pin = pins.at(base);
        std::ranges::sort(pin.pkgnames);
        for (const auto& name : pin.pkgnames) {
            for (const auto& constraint : constraints[name]) pin.constraints.emplace_back(name, constraint);
        }
        ordered.push_back(std::move(pin));
        return true;
    };
    for (const auto& [base, pin] : pins) {
        static_cast<void>(pin);
        if (!visit(base, 0)) return std::unexpected(untrusted());
    }
    for (const auto& pin : ordered) {
        for (const auto& package : metadata.at(pin.pkgbase).packages) {
            if (std::ranges::find(pin.pkgnames, package.name) != pin.pkgnames.end() &&
                !aur_version_matches(pin, package, package_version(package))) return std::unexpected(untrusted());
        }
    }
    snapshot.aur_pins = std::move(ordered);
    snapshot.plan.requires_build_review = true;
    for (const auto& [name, reference] : officials) {
        snapshot.official_deps.push_back(name);
        snapshot.plan.installs.push_back(reference);
    }
    for (const auto& pin : snapshot.aur_pins) {
        snapshot.plan.build_pins.push_back({pin.pkgbase, pin.pkgnames, pin.commit});
        snapshot.fingerprint += "\npin:" + pin.pkgbase + ":" + pin.commit + ":" + pin.tree +
            (pin.dynamic_version ? ":dynamic" : ":static");
        for (const auto& [name, constraint] : pin.constraints)
            snapshot.fingerprint += "\nconstraint:" + name + ":" + constraint.name + constraint.comparison + constraint.version;
        for (const auto& name : pin.pkgnames) {
            protocol::PackageRef reference{{"community"}, {}, name};
            snapshot.plan.build_review_refs.push_back(reference);
            snapshot.plan.installs.push_back(reference);
            if (name == target && snapshot.plan.kind.value == "update-community") snapshot.plan.upgrades.push_back(reference);
        }
    }
    const auto target_install = std::ranges::find_if(snapshot.plan.installs, [&](const auto& reference) {
        return reference.source.value == "community" && reference.name == target;
    });
    if (target_install != snapshot.plan.installs.end())
        std::rotate(snapshot.plan.installs.begin(), target_install, target_install + 1);
    snapshot.fingerprint += "\nofficial:" + nlohmann::json(snapshot.official_deps).dump();
    for (const auto& package : snapshot.aur_official)
        snapshot.fingerprint += "\nprovider:" + package.repo + "/" + package.name + ":" + package.version + ":" + nlohmann::json(package.provides).dump();
    return {};
}

Result<std::vector<std::string>> aur_archives(const AurPin& pin, const std::filesystem::path& packages,
                                             std::string_view architecture) {
    auto info = parse_srcinfo(pin.srcinfo, architecture);
    if (!info || info->pkgbase != pin.pkgbase || pin.pkgnames.empty() || pin.pkgnames.size() > 64 || !packages.is_absolute())
        return std::unexpected(untrusted());
    std::vector<std::string> result;
    for (const auto& name : pin.pkgnames) {
        const auto package = std::ranges::find_if(info->packages, [&](const auto& item) { return item.name == name; });
        if (package == info->packages.end()) return std::unexpected(untrusted());
        std::optional<std::string> found;
        std::error_code error;
        std::filesystem::directory_iterator entry(packages, error);
        std::size_t count = 0;
        for (const auto end = std::filesystem::directory_iterator{}; !error && entry != end; entry.increment(error)) {
            if (++count > 1024) return std::unexpected(untrusted());
            const auto filename = entry->path().filename().string();
            if (!br_is_pkg_archive_name(filename.data(), filename.size())) continue;
            auto stem = std::string_view(filename).substr(0, filename.rfind(".pkg.tar."));
            const auto split = [&stem]() {
                const auto dash = stem.rfind('-');
                if (dash == stem.npos) return std::string_view{};
                const auto value = stem.substr(dash + 1);
                stem = stem.substr(0, dash);
                return value;
            };
            const auto arch = split();
            const auto pkgrel = split();
            const auto pkgver = split();
            if (stem != name || pkgver.empty() || pkgrel.empty() || (arch != architecture && arch != "any")) continue;
            const auto epoch = pkgver.find(':') == pkgver.npos && !package->epoch.empty() && package->epoch != "0" ?
                package->epoch + ":" : "";
            const auto version = epoch + std::string(pkgver) + "-" + std::string(pkgrel);
            if (!aur_version_matches(pin, *package, version)) continue;
            struct stat status{};
            if (lstat(entry->path().c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid() || found)
                return std::unexpected(untrusted());
            found = entry->path().string();
        }
        if (error || !found) return std::unexpected(Error{protocol::kUntrustedInput, "job.build_mismatch"});
        result.push_back(std::move(*found));
    }
    return result;
}

bool aur_version_matches(const AurPin& pin, const SourcePackage& package, std::string_view version) {
    const auto expected = package_version(package);
    if (pin.dynamic_version ? domain::vercmp(version, expected) < 0 : version != expected) return false;
    return std::ranges::all_of(pin.constraints, [&](const auto& item) {
        return item.first != package.name || satisfies_version(version, item.second);
    });
}

Result<void> plan_aur_official(Snapshot& snapshot, core::ProcessRunner& runner) {
    if (snapshot.official_deps.empty()) return {};
    if (snapshot.official_deps.size() > 256) return std::unexpected(untrusted());
    for (const auto& target : snapshot.official_deps) {
        std::size_t slash{};
        if (!br_split_target(target.data(), target.size(), &slash) || slash == target.size())
            return std::unexpected(untrusted());
    }
    core::ProcessSpec spec;
    spec.executable = "/usr/bin/pacman";
    spec.args = {"--sync", "--print", "--print-format", "%r/%n %v", "--needed", "--"};
    spec.args.insert(spec.args.end(), snapshot.official_deps.begin(), snapshot.official_deps.end());
    spec.env = {{"LC_ALL", "C"}};
    spec.timeout = std::chrono::seconds(30);
    spec.max_stdout = 4 * 1024 * 1024;
    spec.max_stderr = 64 * 1024;
    const auto result = runner.run(spec);
    if (!result || result->exit_code != 0 || result->signaled || result->timed_out ||
        result->out_truncated || result->err_truncated || result->out.size() > spec.max_stdout)
        return std::unexpected(untrusted());
    std::set<std::string> seen;
    std::vector<protocol::PackageRef> references;
    auto approved = snapshot.aur_official;
    for (std::string_view remaining = result->out; !remaining.empty();) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        remaining = end == remaining.npos ? std::string_view{} : remaining.substr(end + 1);
        if (line.empty()) continue;
        const auto space = line.find(' ');
        const auto slash = line.find('/');
        if (line.size() > 1024 || space == line.npos || slash == line.npos || slash >= space)
            return std::unexpected(untrusted());
        const auto repo = line.substr(0, slash);
        const auto name = line.substr(slash + 1, space - slash - 1);
        const auto version = line.substr(space + 1);
        if (!br_valid_repo_name(repo.data(), repo.size()) || !br_valid_pkgname(name.data(), name.size()) ||
            !parse_dependency(std::string(name) + "=" + std::string(version)) ||
            !seen.emplace(name).second || seen.size() > 512) return std::unexpected(untrusted());
        references.push_back({{"official"}, std::string(repo), std::string(name)});
        const auto existing = std::ranges::find_if(approved, [&](const auto& item) { return item.name == name; });
        if (existing != approved.end()) {
            if (existing->repo != repo || existing->version != version) return std::unexpected(untrusted());
        } else {
            catalog::RepoPackage package;
            package.name = name;
            package.repo = repo;
            package.version = version;
            approved.push_back(std::move(package));
        }
    }
    std::erase_if(snapshot.plan.installs, [](const auto& ref) { return ref.source.value == "official"; });
    if (snapshot.plan.installs.size() + references.size() > 512) return std::unexpected(untrusted());
    snapshot.plan.installs.insert(snapshot.plan.installs.end(), references.begin(), references.end());
    snapshot.aur_official = std::move(approved);
    snapshot.fingerprint += "\nofficial-transaction:" + result->out;
    return {};
}

namespace {
Error build_mismatch() { return {protocol::kUntrustedInput, "job.build_mismatch"}; }

struct ArchiveFd {
    int fd{-1};
    ~ArchiveFd() { if (fd >= 0) close(fd); }
};

Result<std::string> archive_hash(int fd) {
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != getuid() ||
        status.st_size <= 0 || static_cast<std::uint64_t>(status.st_size) > 8ULL * 1024 * 1024 * 1024 ||
        lseek(fd, 0, SEEK_SET) < 0) return std::unexpected(build_mismatch());
    br_sha256_ctx context{};
    br_sha256_init(&context);
    std::array<std::uint8_t, 65536> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        const auto bytes = read(fd, buffer.data(), buffer.size());
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) return std::unexpected(build_mismatch());
        if (bytes == 0) break;
        total += static_cast<std::uint64_t>(bytes);
        if (total > static_cast<std::uint64_t>(status.st_size)) return std::unexpected(build_mismatch());
        br_sha256_update(&context, buffer.data(), static_cast<std::size_t>(bytes));
    }
    if (total != static_cast<std::uint64_t>(status.st_size)) return std::unexpected(build_mismatch());
    std::array<std::uint8_t, 32> digest{};
    std::array<char, 65> hex{};
    br_sha256_final(&context, digest.data());
    br_hex32(digest.data(), hex.data());
    return std::string(hex.data());
}

bool dependency_approved(const Snapshot& snapshot, const Dependency& dependency, std::string_view architecture) {
    if (std::ranges::any_of(snapshot.aur_installed, [&](const auto& local) {
        return provided(local.package.name, local.package.version, local.provides, dependency);
    })) return true;
    if (std::ranges::any_of(snapshot.aur_official, [&](const auto& package) {
        return provided(package.name, package.version, package.provides, dependency);
    })) return true;
    for (const auto& pin : snapshot.aur_pins) {
        const auto info = parse_srcinfo(pin.srcinfo, architecture);
        if (!info) return false;
        for (const auto& package : info->packages) {
            if (std::ranges::find(pin.pkgnames, package.name) != pin.pkgnames.end() &&
                provided(package.name, package_version(package), package.provides, dependency)) return true;
        }
    }
    return false;
}
}

Result<std::vector<AurArchive>> validate_aur_archives(const Snapshot& snapshot, const AurPin& pin,
    std::span<const std::string> archives, std::string_view architecture, core::ProcessRunner& runner) {
    const auto info = parse_srcinfo(pin.srcinfo, architecture);
    if (!info || info->pkgbase != pin.pkgbase || archives.empty() || archives.size() != pin.pkgnames.size() || archives.size() > 64)
        return std::unexpected(build_mismatch());
    std::vector<AurArchive> result;
    for (std::size_t index = 0; index < archives.size(); ++index) {
        const auto& path = archives[index];
        if (!std::filesystem::path(path).is_absolute()) return std::unexpected(build_mismatch());
        ArchiveFd file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
        if (file.fd < 0) return std::unexpected(build_mismatch());
        const auto digest = archive_hash(file.fd);
        if (!digest) return std::unexpected(digest.error());
        core::ProcessSpec spec;
        spec.executable = "/usr/bin/bsdtar";
        spec.args = {"-xOf", "/proc/" + std::to_string(getpid()) + "/fd/" + std::to_string(file.fd), ".PKGINFO"};
        spec.env = {{"LC_ALL", "C"}};
        spec.timeout = std::chrono::seconds(10);
        spec.max_stdout = 256 * 1024;
        spec.max_stderr = 64 * 1024;
        const auto output = runner.run(spec);
        if (!output || output->exit_code != 0 || output->signaled || output->timed_out ||
            output->out_truncated || output->err_truncated) return std::unexpected(build_mismatch());
        const auto built = parse_pkginfo(output->out);
        const auto package = std::ranges::find_if(info->packages, [&](const auto& item) { return item.name == pin.pkgnames[index]; });
        if (!built || package == info->packages.end() || built->name != package->name ||
            !aur_version_matches(pin, *package, built->version) || (built->arch != architecture && built->arch != "any"))
            return std::unexpected(build_mismatch());
        std::set<std::string> reviewed(package->depends.begin(), package->depends.end());
        std::set<std::string> actual;
        for (const auto& text : built->depends) {
            const auto dependency = parse_dependency(text);
            if (!dependency || !reviewed.contains(text) || !actual.insert(text).second ||
                !dependency_approved(snapshot, *dependency, architecture)) return std::unexpected(build_mismatch());
        }
        if (actual != reviewed) return std::unexpected(build_mismatch());
        const auto after = archive_hash(file.fd);
        struct stat opened{}, current{};
        if (!after || *after != *digest || fstat(file.fd, &opened) != 0 || lstat(path.c_str(), &current) != 0 ||
            opened.st_dev != current.st_dev || opened.st_ino != current.st_ino || !S_ISREG(current.st_mode))
            return std::unexpected(build_mismatch());
        result.push_back({path, *digest});
    }
    return result;
}

Result<void> recheck_aur_archives(std::span<const AurArchive> archives) {
    for (const auto& archive : archives) {
        ArchiveFd file{open(archive.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
        if (file.fd < 0) return std::unexpected(build_mismatch());
        const auto digest = archive_hash(file.fd);
        if (!digest || *digest != archive.sha256) return std::unexpected(build_mismatch());
    }
    return {};
}

std::vector<std::string> aur_dependency_names(const Snapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& pin : snapshot.aur_pins) {
        for (const auto& name : pin.pkgnames) {
            if (std::ranges::find(snapshot.targets, name) != snapshot.targets.end()) continue;
            const auto installed = std::ranges::find_if(snapshot.aur_installed, [&](const auto& item) { return item.package.name == name; });
            if (installed == snapshot.aur_installed.end() || installed->package.reason == domain::InstallReason::Dependency)
                result.push_back(name);
        }
    }
    return result;
}

core::ProcessSpec aur_build_spec(const std::filesystem::path& root, const std::filesystem::path& workspace,
                                 const std::filesystem::path& home, bool contained) {
    core::ProcessSpec spec;
    if (contained) {
        spec.executable = "/usr/bin/bwrap";
        spec.args = {"--unshare-pid", "--die-with-parent", "--new-session", "--dev-bind", "/", "/", "--proc", "/proc", "--",
                     "/usr/bin/makepkg", "--nodeps", "--nocheck", "--noconfirm", "--noprogressbar"};
    } else {
        spec.executable = "/usr/bin/makepkg";
        spec.args = {"--nodeps", "--nocheck", "--noconfirm", "--noprogressbar"};
    }
    spec.cwd = root;
    spec.env = {{"PATH", "/usr/bin"}, {"HOME", home.string()}, {"LANG", "C.UTF-8"},
                {"PKGDEST", (workspace / "packages").string()}, {"SRCDEST", (workspace / "sources").string()},
                {"BUILDDIR", (workspace / "build").string()}};
    spec.timeout = std::chrono::hours(4);
    spec.max_stdout = spec.max_stderr = 8 * 1024 * 1024;
    return spec;
}

Result<void> aur_build_space(const std::filesystem::path& workspace) {
    struct statvfs space{};
    constexpr std::uint64_t minimum = 4ULL * 1024 * 1024 * 1024;
    if (statvfs(workspace.c_str(), &space) != 0 || space.f_frsize == 0 ||
        space.f_bavail < (minimum - 1) / space.f_frsize + 1)
        return std::unexpected(Error{protocol::kUnsafeTransaction, "job.build_space"});
    return {};
}

Result<void> finish_aur_workspace(const std::filesystem::path& root, const std::filesystem::path& workspace, bool success) {
    if (!root.is_absolute() || workspace.parent_path() != root || workspace.filename() == "." || workspace.filename() == "..")
        return std::unexpected(untrusted());
    if (success) {
        if (!core::remove_tree_confined(root, workspace)) return std::unexpected(untrusted());
        return {};
    }
    if (!core::ensure_private_dir(workspace) || !core::atomic_write(workspace / ".failed", "failed\n", 0600))
        return std::unexpected(untrusted());
    struct Failed { std::filesystem::path path; timespec modified{}; };
    std::vector<Failed> failed;
    std::error_code error;
    std::filesystem::directory_iterator entry(root, error);
    std::size_t count = 0;
    for (const auto end = std::filesystem::directory_iterator{}; !error && entry != end; entry.increment(error)) {
        if (++count > 4096) return std::unexpected(untrusted());
        struct stat directory{}, marker{};
        if (lstat(entry->path().c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) || directory.st_uid != getuid()) continue;
        if (lstat((entry->path() / ".failed").c_str(), &marker) != 0 || !S_ISREG(marker.st_mode) || marker.st_uid != getuid()) continue;
        failed.push_back({entry->path(), marker.st_mtim});
    }
    if (error) return std::unexpected(untrusted());
    std::ranges::sort(failed, [](const auto& left, const auto& right) {
        if (left.modified.tv_sec != right.modified.tv_sec) return left.modified.tv_sec > right.modified.tv_sec;
        if (left.modified.tv_nsec != right.modified.tv_nsec) return left.modified.tv_nsec > right.modified.tv_nsec;
        return left.path > right.path;
    });
    for (std::size_t index = 3; index < failed.size(); ++index)
        if (!core::remove_tree_confined(root, failed[index].path)) return std::unexpected(untrusted());
    return {};
}

}
