#include "blob.hpp"

#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/files.hpp>

#include <git2.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <unordered_set>
#include <sys/stat.h>

namespace braurser::daemon::manual {

std::optional<std::uint64_t> internal::blob_size(git_repository* repo, const git_oid& oid) {
    git_odb* raw = nullptr;
    if (!repo || git_repository_odb(&raw, repo) != 0) return std::nullopt;
    std::unique_ptr<git_odb, decltype(&git_odb_free)> odb(raw, git_odb_free);
    std::size_t length = 0;
    git_object_t type = GIT_OBJECT_INVALID;
    if (git_odb_read_header(&length, &type, odb.get(), &oid) != 0 || type != GIT_OBJECT_BLOB)
        return std::nullopt;
    return static_cast<std::uint64_t>(length);
}

namespace {

ManualError fail(std::string code) { return {std::move(code), {}, {}, {}}; }

template<auto Release>
struct Releaser {
    template<class T>
    void operator()(T* pointer) const noexcept { Release(pointer); }
};

template<class T, auto Release>
using Owned = std::unique_ptr<T, Releaser<Release>>;

struct Node {
    std::string path;
    git_oid oid{};
    git_filemode_t mode{};
};

bool git_dir(std::string_view name) {
    return name.size() == 4 && name[0] == '.' &&
           std::tolower(static_cast<unsigned char>(name[1])) == 'g' &&
           std::tolower(static_cast<unsigned char>(name[2])) == 'i' &&
           std::tolower(static_cast<unsigned char>(name[3])) == 't';
}

bool safe_name(std::string_view name) {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." && !git_dir(name) &&
           name.find_first_of("/\\") == name.npos && name.find('\0') == name.npos;
}

bool safe_relative(std::string_view path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/') return false;
    std::size_t offset = 0;
    while (offset < path.size()) {
        auto end = path.find('/', offset);
        if (!safe_name(path.substr(offset, end == path.npos ? end : end - offset))) return false;
        offset = end == path.npos ? path.size() : end + 1;
        if (end != path.npos && offset == path.size()) return false;
    }
    return true;
}

bool inside_target(const std::string& path, std::string_view link) {
    if (link.empty() || link.size() > 4096 || link.front() == '/' || link.find('\0') != link.npos) return false;
    const auto parent = std::filesystem::path(path).parent_path();
    const auto resolved = (parent / std::filesystem::path(link)).lexically_normal();
    if (!safe_relative(resolved.generic_string())) return false;
    for (const auto& part : std::filesystem::path(link)) {
        if (part == "." || part == "..") continue;
        if (!safe_name(part.string())) return false;
    }
    return true;
}

std::expected<std::vector<Node>, ManualError> enumerate(git_repository* repo, git_tree* root) {
    std::vector<Node> nodes;
    std::uint64_t total = 0;
    std::function<std::expected<void, ManualError>(git_tree*, std::string, unsigned)> walk;
    walk = [&](git_tree* tree, std::string prefix, unsigned depth) -> std::expected<void, ManualError> {
        if (depth > 64) return std::unexpected(fail("tree-depth"));
        for (std::size_t index = 0; index < git_tree_entrycount(tree); ++index) {
            const auto* entry = git_tree_entry_byindex(tree, index);
            if (!entry) return std::unexpected(fail("tree-entry"));
            const char* raw = git_tree_entry_name(entry);
            if (!raw || !safe_name(raw)) return std::unexpected(fail("tree-path"));
            auto path = prefix + raw;
            if (path.size() > 4096 || nodes.size() >= 50000) return std::unexpected(fail("tree-limit"));
            const auto mode = git_tree_entry_filemode(entry);
            if (mode != GIT_FILEMODE_TREE && mode != GIT_FILEMODE_BLOB &&
                mode != GIT_FILEMODE_BLOB_EXECUTABLE && mode != GIT_FILEMODE_LINK &&
                mode != GIT_FILEMODE_COMMIT) return std::unexpected(fail("tree-mode"));
            nodes.push_back({path, *git_tree_entry_id(entry), mode});
            if (mode == GIT_FILEMODE_TREE) {
                git_tree* raw_tree = nullptr;
                if (git_tree_lookup(&raw_tree, repo, git_tree_entry_id(entry)) != 0)
                    return std::unexpected(fail("tree-read"));
                Owned<git_tree, git_tree_free> child(raw_tree);
                auto result = walk(child.get(), path + "/", depth + 1);
                if (!result) return result;
            } else if (mode != GIT_FILEMODE_COMMIT) {
                const auto size = internal::blob_size(repo, *git_tree_entry_id(entry));
                if (!size) return std::unexpected(fail("blob-read"));
                if (*size > 512ULL * 1024 * 1024 - total)
                    return std::unexpected(fail("tree-size"));
                total += *size;
            }
        }
        return {};
    };
    auto result = walk(root, {}, 0);
    if (!result) return std::unexpected(result.error());
    return nodes;
}

struct Snapshot {
    Owned<git_repository, git_repository_free> repo;
    Owned<git_commit, git_commit_free> commit;
    Owned<git_tree, git_tree_free> tree;
    std::vector<Node> nodes;
};

std::expected<Snapshot, ManualError> snapshot(const std::filesystem::path& path, std::string_view revision) {
    static const int initialized = git_libgit2_init();
    if (initialized < 0) return std::unexpected(fail("git-init"));
    if (!path.is_absolute() || revision.empty() || revision.size() > 255 ||
        !std::all_of(revision.begin(), revision.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '/' || c == '_' || c == '-' || c == '.';
        }) || revision.find("..") != revision.npos) return std::unexpected(fail("git-revision"));
    git_repository* raw_repo = nullptr;
    if (git_repository_open_bare(&raw_repo, path.c_str()) != 0) return std::unexpected(fail("git-open"));
    Owned<git_repository, git_repository_free> repo(raw_repo);
    git_object* raw_object = nullptr;
    const auto name = std::string(revision);
    if (git_revparse_single(&raw_object, repo.get(), name.c_str()) != 0)
        return std::unexpected(fail("git-revision"));
    Owned<git_object, git_object_free> object(raw_object);
    git_object* peeled = nullptr;
    if (git_object_peel(&peeled, object.get(), GIT_OBJECT_COMMIT) != 0)
        return std::unexpected(fail("git-commit"));
    git_commit* raw_commit = reinterpret_cast<git_commit*>(peeled);
    Owned<git_commit, git_commit_free> commit(raw_commit);
    git_tree* raw_tree = nullptr;
    if (git_commit_tree(&raw_tree, commit.get()) != 0) return std::unexpected(fail("git-tree"));
    Owned<git_tree, git_tree_free> tree(raw_tree);
    auto nodes = enumerate(repo.get(), tree.get());
    if (!nodes) return std::unexpected(nodes.error());
    return Snapshot{std::move(repo), std::move(commit), std::move(tree), std::move(*nodes)};
}

std::expected<std::string, ManualError> blob_text(git_repository* repo, const git_oid& oid,
                                                   std::size_t limit) {
    const auto declared = internal::blob_size(repo, oid);
    if (!declared) return std::unexpected(fail("blob-read"));
    if (*declared > limit) return std::unexpected(fail("script-size"));
    git_blob* raw = nullptr;
    if (git_blob_lookup(&raw, repo, &oid) != 0) return std::unexpected(fail("blob-read"));
    Owned<git_blob, git_blob_free> blob(raw);
    const auto size = git_blob_rawsize(blob.get());
    if (static_cast<std::uint64_t>(size) > limit) return std::unexpected(fail("script-size"));
    std::string_view bytes(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                           static_cast<std::size_t>(size));
    if (bytes.find('\0') != bytes.npos) return std::unexpected(fail("script-binary"));
    return std::string(bytes);
}

}

std::expected<Preview, ManualError> preview_repository(
    const std::filesystem::path& repo, std::string_view revision, const GitUrl& url,
    std::string_view id, std::optional<EntryKind> selected) {
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-';
    })) return std::unexpected(fail("preview-id"));
    auto source = snapshot(repo, revision);
    if (!source) return std::unexpected(source.error());
    Preview result;
    result.preview_id = id;
    result.host = url.host;
    result.owner_repo = url.owner_repo;
    std::array<char, GIT_OID_MAX_HEXSIZE + 1> hex{};
    git_oid_tostr(hex.data(), hex.size(), git_commit_id(source->commit.get()));
    result.commit = hex.data();
    const Node* script = nullptr;
    const Node* makefile = nullptr;
    for (const auto& node : source->nodes) {
        if (node.path == "install.sh" && (node.mode == GIT_FILEMODE_BLOB ||
                                            node.mode == GIT_FILEMODE_BLOB_EXECUTABLE)) script = &node;
        if (node.path == "Makefile" && node.mode == GIT_FILEMODE_BLOB) makefile = &node;
    }
    if (script) result.entry_points.push_back({"install.sh", EntryKind::Script});
    std::optional<std::string> make_text;
    if (makefile) {
        auto text = blob_text(source->repo.get(), makefile->oid, 256 * 1024);
        if (!text) return std::unexpected(text.error());
        make_text = *text;
        if (make_text->starts_with("install:") || make_text->find("\ninstall:") != std::string::npos)
            result.entry_points.push_back({"Makefile", EntryKind::MakeInstall});
    }
    if (result.entry_points.empty()) return std::unexpected(fail("Unsupported"));
    if (result.entry_points.size() > 1 && !selected) return result;
    auto choice = selected.value_or(result.entry_points.front().kind);
    if (std::ranges::none_of(result.entry_points, [choice](const EntryPoint& item) {
        return item.kind == choice;
    })) return std::unexpected(fail("entry-point"));
    if (choice == EntryKind::Script) {
        auto text = blob_text(source->repo.get(), script->oid, 256 * 1024);
        if (!text) return std::unexpected(text.error());
        result.selected_script_text = std::move(*text);
    } else result.selected_script_text = *make_text;
    std::unordered_set<std::string_view> node_paths;
    node_paths.reserve(source->nodes.size());
    for (const auto& node : source->nodes) node_paths.insert(node.path);
    std::set<std::string> paths;
    std::set<std::string> urls;
    std::string_view text(result.selected_script_text);
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find_first_of(" \t\r\n\"'`<>|;(){}", start);
        if (end == text.npos) end = text.size();
        auto token = text.substr(start, end - start);
        while (!token.empty() && (token.front() == '.' || token.front() == '/')) token.remove_prefix(1);
        if (token.starts_with("http://") || token.starts_with("https://")) urls.insert(std::string(token));
        if (node_paths.contains(token)) paths.insert(std::string(token));
        start = end + 1;
    }
    result.referenced_files.assign(paths.begin(), paths.end());
    result.external_urls.assign(urls.begin(), urls.end());
    return result;
}

std::expected<void, ManualError> materialize_commit(
    const std::filesystem::path& repo, std::string_view commit,
    const std::filesystem::path& destination) {
    if ((commit.size() != 40 && commit.size() != 64) || !std::all_of(commit.begin(), commit.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    }) || !destination.is_absolute() || std::filesystem::is_symlink(destination) ||
        !std::filesystem::is_directory(destination) || !std::filesystem::is_empty(destination))
        return std::unexpected(fail("materialize-input"));
    auto source = snapshot(repo, commit);
    if (!source) return std::unexpected(source.error());
    for (const auto& node : source->nodes) {
        if (node.mode == GIT_FILEMODE_COMMIT) return std::unexpected(fail("submodules-unsupported"));
        if (node.mode != GIT_FILEMODE_LINK) continue;
        auto text = blob_text(source->repo.get(), node.oid, 4096);
        if (!text || !inside_target(node.path, *text)) return std::unexpected(fail("tree-symlink"));
    }
    for (const auto& node : source->nodes) {
        const auto output = destination / node.path;
        if (node.mode == GIT_FILEMODE_TREE) {
            std::error_code ec;
            if (!std::filesystem::create_directory(output, ec) || ec || chmod(output.c_str(), 0755) != 0)
                return std::unexpected(fail("materialize-directory"));
            continue;
        }
        const auto declared = internal::blob_size(source->repo.get(), node.oid);
        if (!declared || *declared > 512ULL * 1024 * 1024) return std::unexpected(fail("blob-read"));
        git_blob* raw = nullptr;
        if (git_blob_lookup(&raw, source->repo.get(), &node.oid) != 0)
            return std::unexpected(fail("blob-read"));
        Owned<git_blob, git_blob_free> blob(raw);
        const auto length = static_cast<std::size_t>(git_blob_rawsize(blob.get()));
        const auto* bytes = static_cast<const char*>(git_blob_rawcontent(blob.get()));
        if (node.mode == GIT_FILEMODE_LINK) {
            std::error_code ec;
            std::filesystem::create_symlink(std::string(bytes, length), output, ec);
            if (ec) return std::unexpected(fail("materialize-link"));
        } else {
            auto parent = output.parent_path();
            auto content = std::string_view(bytes, length);
            auto written = core::atomic_write(output, content, 0600);
            if (!written || chmod(output.c_str(), node.mode == GIT_FILEMODE_BLOB_EXECUTABLE ? 0755 : 0644) != 0)
                return std::unexpected(fail("materialize-file"));
        }
    }
    return {};
}

}
