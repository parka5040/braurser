#include <braurser/daemon/manual/manual.hpp>

#include <git2.h>
#include <algorithm>
#include <memory>

namespace braurser::daemon::manual {
namespace {

ManualError fail(std::string code) { return {std::move(code), {}, {}, {}}; }

std::expected<std::string, ManualError> diff_script(
    const std::filesystem::path& repo_path, std::string_view old_commit,
    std::string_view new_commit, std::string_view entry_point) {
    if (old_commit.size() != 40 || new_commit.size() != 40 ||
        (entry_point != "install.sh" && entry_point != "Makefile"))
        return std::unexpected(fail("update-input"));
    git_repository* raw_repo = nullptr;
    if (git_repository_open_bare(&raw_repo, repo_path.c_str()) != 0)
        return std::unexpected(fail("update-cache"));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
    auto find_blob = [&](std::string_view commit_text)
        -> std::expected<git_blob*, ManualError> {
        git_oid oid{};
        if (git_oid_fromstr(&oid, std::string(commit_text).c_str()) != 0)
            return std::unexpected(fail("update-commit"));
        git_commit* raw_commit = nullptr;
        if (git_commit_lookup(&raw_commit, repo.get(), &oid) != 0)
            return std::unexpected(fail("update-commit"));
        std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
        git_tree* raw_tree = nullptr;
        if (git_commit_tree(&raw_tree, commit.get()) != 0)
            return std::unexpected(fail("update-tree"));
        std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
        git_tree_entry* raw_entry = nullptr;
        if (git_tree_entry_bypath(&raw_entry, tree.get(), std::string(entry_point).c_str()) != 0)
            return std::unexpected(fail("update-entry"));
        std::unique_ptr<git_tree_entry, decltype(&git_tree_entry_free)> entry(raw_entry, git_tree_entry_free);
        git_blob* raw_blob = nullptr;
        if (git_blob_lookup(&raw_blob, repo.get(), git_tree_entry_id(entry.get())) != 0)
            return std::unexpected(fail("update-blob"));
        return raw_blob;
    };
    auto old = find_blob(old_commit);
    if (!old) return std::unexpected(old.error());
    std::unique_ptr<git_blob, decltype(&git_blob_free)> old_blob(*old, git_blob_free);
    auto next = find_blob(new_commit);
    if (!next) return std::unexpected(next.error());
    std::unique_ptr<git_blob, decltype(&git_blob_free)> new_blob(*next, git_blob_free);
    if (git_blob_rawsize(old_blob.get()) > 256 * 1024 || git_blob_rawsize(new_blob.get()) > 256 * 1024)
        return std::unexpected(fail("update-script-size"));
    git_patch* raw_patch = nullptr;
    git_diff_options options;
    git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
    options.flags = GIT_DIFF_FORCE_TEXT;
    if (git_patch_from_blobs(&raw_patch, old_blob.get(), std::string(entry_point).c_str(),
                             new_blob.get(), std::string(entry_point).c_str(), &options) != 0)
        return std::unexpected(fail("update-diff"));
    std::unique_ptr<git_patch, decltype(&git_patch_free)> patch(raw_patch, git_patch_free);
    git_buf buffer = GIT_BUF_INIT;
    if (git_patch_to_buf(&buffer, patch.get()) != 0) {
        git_buf_dispose(&buffer);
        return std::unexpected(fail("update-diff"));
    }
    if (buffer.size > 256 * 1024) {
        git_buf_dispose(&buffer);
        return std::unexpected(fail("update-diff-size"));
    }
    std::string diff = buffer.size ? std::string(buffer.ptr, buffer.size) : std::string{};
    git_buf_dispose(&buffer);
    return diff;
}

}

std::expected<UpdateCheck, ManualError> check_update(
    GitHttp& http, const Receipt& receipt, const std::filesystem::path& cache_root,
    std::optional<EntryKind> selected) {
    if (receipt.id.empty() || receipt.id.size() > 128 ||
        !std::all_of(receipt.id.begin(), receipt.id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-';
        }) || !cache_root.is_absolute()) return std::unexpected(fail("update-input"));
    const auto repo = cache_root / "braurser/manual" / receipt.id / "repo.git";
    if (!std::filesystem::exists(repo) || std::filesystem::is_symlink(repo))
        return std::unexpected(fail("update-cache"));
    const auto kind = selected.value_or(receipt.entry_point == "Makefile" ?
        EntryKind::MakeInstall : EntryKind::Script);
    auto preview = fetch_preview(http, receipt.url, cache_root, receipt.id, "HEAD", kind);
    if (!preview) return std::unexpected(preview.error());
    if (preview->commit == receipt.commit) return UpdateCheck{};
    auto diff = diff_script(repo, receipt.commit, preview->commit, receipt.entry_point);
    if (!diff) return std::unexpected(diff.error());
    return UpdateCheck{std::move(*preview), std::move(*diff)};
}

}
