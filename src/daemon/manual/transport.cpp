#include "blob.hpp"

#include <braurser/daemon/manual/manual.hpp>
#include <braurser/daemon/core/files.hpp>
#include <braurser/daemon/core/text.hpp>
#include <braurser/protocol/dto.hpp>

#include <git2.h>
#include <git2/sys/transport.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

namespace braurser::daemon::manual {
namespace {

ManualError fail(std::string code) { return {std::move(code), {}, {}, {}}; }

class BudgetedHttp final : public GitHttp {
public:
    explicit BudgetedHttp(GitHttp& upstream) : upstream_(upstream) {}
    std::expected<GitReply, ManualError> request(
        std::string_view url, std::string_view body, bool post) override {
        if (std::chrono::steady_clock::now() >= deadline_ || body.size() > remaining_)
            return std::unexpected(fail("git-budget"));
        remaining_ -= body.size();
        auto result = upstream_.request(url, body, post);
        if (!result) return result;
        if (std::chrono::steady_clock::now() >= deadline_ || result->body.size() > remaining_)
            return std::unexpected(fail("git-budget"));
        remaining_ -= result->body.size();
        return result;
    }
private:
    GitHttp& upstream_;
    std::size_t remaining_{512ULL * 1024 * 1024};
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::now() + std::chrono::minutes(10)};
};

struct Subtransport {
    git_smart_subtransport base{};
    GitHttp* http{};
};

struct Stream {
    git_smart_subtransport_stream base{};
    GuardedGitTransport exchange;
    std::string url;
    std::string input;
    std::string output;
    std::size_t position{};
    bool post{};
    bool requested{};
    explicit Stream(GitHttp& http) : exchange(http) {}
};

thread_local std::optional<ManualError> last_error;

int receive(git_smart_subtransport_stream* pointer, char* data, std::size_t limit, std::size_t* bytes_read) {
    auto& stream = *reinterpret_cast<Stream*>(pointer);
    *bytes_read = 0;
    if (!stream.requested) {
        stream.requested = true;
        auto response = stream.exchange.exchange(stream.url, stream.post ? stream.input : std::string_view{});
        if (!response) {
            last_error = response.error();
            giterr_set_str(GIT_ERROR_NET, response.error().code.c_str());
            return GIT_ERROR;
        }
        stream.output = std::move(*response);
    }
    const auto amount = std::min(limit, stream.output.size() - stream.position);
    if (amount != 0) std::memcpy(data, stream.output.data() + stream.position, amount);
    stream.position += amount;
    *bytes_read = amount;
    return 0;
}

int send(git_smart_subtransport_stream* pointer, const char* data, std::size_t length) {
    auto& stream = *reinterpret_cast<Stream*>(pointer);
    if (!stream.post || stream.requested || length > 512ULL * 1024 * 1024 - stream.input.size()) {
        giterr_set_str(GIT_ERROR_NET, "git request exceeds limit");
        return GIT_ERROR;
    }
    if (length != 0) stream.input.append(data, length);
    return 0;
}

void free_stream(git_smart_subtransport_stream* pointer) {
    delete reinterpret_cast<Stream*>(pointer);
}

int action(git_smart_subtransport_stream** output, git_smart_subtransport* pointer,
           const char* url, git_smart_service_t service) {
    if (service != GIT_SERVICE_UPLOADPACK_LS && service != GIT_SERVICE_UPLOADPACK) {
        giterr_set_str(GIT_ERROR_NET, "only git-upload-pack is supported");
        return GIT_ERROR;
    }
    auto& parent = *reinterpret_cast<Subtransport*>(pointer);
    auto parsed = validate_git_url(url ? std::string_view(url) : std::string_view{});
    if (!parsed) {
        giterr_set_str(GIT_ERROR_NET, "invalid git URL");
        return GIT_ERROR;
    }
    auto* stream = new Stream(*parent.http);
    stream->base.subtransport = pointer;
    stream->base.read = receive;
    stream->base.write = send;
    stream->base.free = free_stream;
    stream->url = parsed->url;
    stream->post = service == GIT_SERVICE_UPLOADPACK;
    *output = &stream->base;
    return 0;
}

int close_transport(git_smart_subtransport*) { return 0; }
void free_transport(git_smart_subtransport* pointer) { delete reinterpret_cast<Subtransport*>(pointer); }

int create_subtransport(git_smart_subtransport** output, git_transport*, void* param) {
    auto* result = new Subtransport{};
    result->base.action = action;
    result->base.close = close_transport;
    result->base.free = free_transport;
    result->http = static_cast<GitHttp*>(param);
    *output = &result->base;
    return 0;
}

int create_transport(git_transport** output, git_remote* owner, void* param) {
    git_smart_subtransport_definition definition{create_subtransport, 1, param};
    return git_transport_smart(output, owner, &definition);
}

std::mutex registration_mutex;

class Registered {
public:
    explicit Registered(GitHttp& http) : lock_(registration_mutex) {
        ok_ = git_transport_register("https", create_transport, &http) == 0;
    }
    ~Registered() { if (ok_) git_transport_unregister("https"); }
    bool ok() const { return ok_; }
private:
    std::unique_lock<std::mutex> lock_;
    bool ok_{};
};

}

std::expected<std::filesystem::path, ManualError> fetch_bare(
    GitHttp& http, std::string_view url, const std::filesystem::path& cache_root,
    std::string_view preview_id, std::string_view revision) {
    auto parsed = validate_git_url(url);
    if (!parsed || !cache_root.is_absolute() || preview_id.empty() || preview_id.size() > 128 ||
        !std::all_of(preview_id.begin(), preview_id.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-';
        })) return std::unexpected(fail("fetch-input"));
    static const int initialized = git_libgit2_init();
    if (initialized < 0) return std::unexpected(fail("git-init"));
    auto preview_dir = cache_root / "braurser/manual" / std::string(preview_id);
    for (const auto& part : {cache_root / "braurser", cache_root / "braurser/manual", preview_dir}) {
        if (!core::ensure_private_dir(part)) return std::unexpected(fail("fetch-cache"));
    }
    const auto bare = preview_dir / "repo.git";
    git_repository* raw = nullptr;
    if (std::filesystem::exists(bare)) {
        if (std::filesystem::is_symlink(bare) || git_repository_open_bare(&raw, bare.c_str()) != 0)
            return std::unexpected(fail("fetch-cache"));
    } else if (git_repository_init(&raw, bare.c_str(), 1) != 0)
        return std::unexpected(fail("fetch-init"));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw, git_repository_free);
    git_remote* raw_remote = nullptr;
    git_remote_create_options create;
    if (git_remote_create_options_init(&create, GIT_REMOTE_CREATE_OPTIONS_VERSION) != 0)
        return std::unexpected(fail("fetch-remote"));
    create.repository = repo.get();
    create.flags = GIT_REMOTE_CREATE_SKIP_INSTEADOF;
    if (git_remote_create_with_opts(&raw_remote, parsed->url.c_str(), &create) != 0)
        return std::unexpected(fail("fetch-remote"));
    std::unique_ptr<git_remote, decltype(&git_remote_free)> remote(raw_remote, git_remote_free);
    BudgetedHttp budgeted(http);
    Registered guard(budgeted);
    if (!guard.ok()) return std::unexpected(fail("fetch-register"));
    last_error.reset();
    const std::string branch = std::string(revision);
    const auto safe_ref = [](std::string_view name) {
        return !name.empty() && name.size() <= 128 && name.find("..") == name.npos &&
               name.front() != '/' && name.back() != '/' && name.find("//") == name.npos &&
               std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                   return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '/' || ch == '.';
               });
    };
    const bool heads = branch.starts_with("refs/heads/");
    std::vector<std::string> specs;
    if (branch == "HEAD") specs = {"+HEAD:refs/heads/braurser-preview"};
    else if (heads || branch.starts_with("refs/tags/")) {
        if (!safe_ref(std::string_view(branch).substr(heads ? 11 : 10))) return std::unexpected(fail("fetch-ref"));
        specs = {"+" + branch + ":refs/heads/braurser-preview"};
    } else if (safe_ref(branch)) specs = {"+refs/heads/" + branch + ":refs/heads/braurser-preview",
                                          "+refs/tags/" + branch + ":refs/tags/braurser-preview"};
    else return std::unexpected(fail("fetch-ref"));
    std::vector<char*> raw_specs;
    for (auto& spec : specs) raw_specs.push_back(spec.data());
    git_strarray array{raw_specs.data(), raw_specs.size()};
    git_fetch_options options;
    git_fetch_options_init(&options, GIT_FETCH_OPTIONS_VERSION);
    options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    if (git_remote_fetch(remote.get(), &array, &options, nullptr) != 0) {
        if (last_error) return std::unexpected(*last_error);
        auto failure = fail("fetch-failed");
        if (auto* error = git_error_last(); error && error->message)
            failure.detail = core::bounded_line(core::redact(error->message), 512);
        return std::unexpected(failure);
    }
    return bare;
}

std::expected<Preview, ManualError> fetch_preview(
    GitHttp& http, std::string_view url, const std::filesystem::path& cache_root,
    std::string_view preview_id, std::string_view revision, std::optional<EntryKind> selected) {
    auto parsed = validate_git_url(url);
    if (!parsed) return std::unexpected(parsed.error());
    auto bare = fetch_bare(http, url, cache_root, preview_id, revision);
    if (!bare) return std::unexpected(bare.error());
    return preview_repository(*bare, "refs/heads/braurser-preview", *parsed, preview_id, selected);
}

std::expected<BuildRepository, ManualError> fetch_build_repository(
    GitHttp& http, std::string_view url, const std::filesystem::path& cache_root,
    std::string_view id) {
    auto bare = fetch_bare(http, url, cache_root, id, "HEAD");
    if (!bare) return std::unexpected(bare.error());
    git_repository* raw_repo = nullptr;
    if (git_repository_open_bare(&raw_repo, bare->c_str()) != 0) return std::unexpected(fail("git-open"));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
    git_oid oid{};
    if (git_reference_name_to_id(&oid, repo.get(), "refs/heads/braurser-preview") != 0)
        return std::unexpected(fail("git-revision"));
    git_commit* raw_commit = nullptr;
    if (git_commit_lookup(&raw_commit, repo.get(), &oid) != 0) return std::unexpected(fail("git-commit"));
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
    git_tree* raw_tree = nullptr;
    if (git_commit_tree(&raw_tree, commit.get()) != 0) return std::unexpected(fail("git-tree"));
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
    git_tree_entry* raw_entry = nullptr;
    if (git_tree_entry_bypath(&raw_entry, tree.get(), "PKGBUILD") != 0)
        return std::unexpected(fail("build-file"));
    std::unique_ptr<git_tree_entry, decltype(&git_tree_entry_free)> entry(raw_entry, git_tree_entry_free);
    if (git_tree_entry_filemode(entry.get()) != GIT_FILEMODE_BLOB)
        return std::unexpected(fail("build-file"));
    const auto declared = internal::blob_size(repo.get(), *git_tree_entry_id(entry.get()));
    if (!declared) return std::unexpected(fail("build-file"));
    if (*declared == 0 || *declared > 256 * 1024) return std::unexpected(fail("build-file-size"));
    git_blob* raw_blob = nullptr;
    if (git_blob_lookup(&raw_blob, repo.get(), git_tree_entry_id(entry.get())) != 0)
        return std::unexpected(fail("build-file"));
    std::unique_ptr<git_blob, decltype(&git_blob_free)> blob(raw_blob, git_blob_free);
    const auto size = git_blob_rawsize(blob.get());
    if (size <= 0 || size > 256 * 1024) return std::unexpected(fail("build-file-size"));
    const std::string_view text(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                                static_cast<std::size_t>(size));
    if (text.find('\0') != text.npos || !protocol::valid_utf8(text))
        return std::unexpected(fail("build-file"));
    std::array<char, GIT_OID_MAX_HEXSIZE + 1> hex{};
    git_oid_tostr(hex.data(), hex.size(), &oid);
    return BuildRepository{hex.data(), std::string(text)};
}

}
