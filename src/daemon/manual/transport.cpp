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
#include <map>
#include <braurser/validate/validate.hpp>
#include <array>
#include <functional>
#include <deque>

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
        auto result = upstream_.request_bounded(url, body, post, remaining_);
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
    GitReply::Body output;
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
    auto amount = stream.output.read(std::span<char>(data, limit), stream.position);
    if (!amount) {
        last_error = amount.error();
        giterr_set_str(GIT_ERROR_NET, amount.error().code.c_str());
        return GIT_ERROR;
    }
    stream.position += *amount;
    *bytes_read = *amount;
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

std::mutex repository_mutexes_mutex;
std::map<std::string, std::weak_ptr<std::mutex>> repository_mutexes;

std::shared_ptr<std::mutex> repository_mutex(const std::filesystem::path& path) {
    std::lock_guard lock(repository_mutexes_mutex);
    std::erase_if(repository_mutexes, [](const auto& entry) { return entry.second.expired(); });
    auto& entry = repository_mutexes[path.string()];
    auto mutex = entry.lock();
    if (!mutex) {
        mutex = std::make_shared<std::mutex>();
        entry = mutex;
    }
    return mutex;
}

}

std::expected<std::filesystem::path, ManualError> fetch_bare(
    GitHttp& http, std::string_view url, const std::filesystem::path& cache_root,
    std::string_view preview_id, std::string_view revision) {
    auto parsed = validate_git_url(url);
    if (!parsed || !cache_root.is_absolute() || preview_id.empty() || preview_id.size() > (preview_id.starts_with("aur-") ? 255U : 128U) ||
        (!(preview_id.starts_with("aur-") && br_valid_pkgname(preview_id.data() + 4, preview_id.size() - 4)) &&
         !std::all_of(preview_id.begin(), preview_id.end(), [](unsigned char ch) {
             return std::isalnum(ch) || ch == '-';
         }))) return std::unexpected(fail("fetch-input"));
    static const int initialized = git_libgit2_init();
    if (initialized < 0) return std::unexpected(fail("git-init"));
    auto preview_dir = cache_root / "braurser/manual" / std::string(preview_id);
    const auto bare = preview_dir / "repo.git";
    const auto mutex = repository_mutex(bare);
    std::lock_guard lock(*mutex);
    for (const auto& part : {cache_root / "braurser", cache_root / "braurser/manual", preview_dir}) {
        if (!core::ensure_private_dir(part)) return std::unexpected(fail("fetch-cache"));
    }
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
    options.callbacks.transport = create_transport;
    options.callbacks.payload = static_cast<GitHttp*>(&budgeted);
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
    git_reference* raw_pin = nullptr;
    const auto pin = std::string("refs/braurser/pins/") + hex.data();
    if (git_reference_create(&raw_pin, repo.get(), pin.c_str(), &oid, 1, nullptr) != 0)
        return std::unexpected(fail("git-pin"));
    git_reference_free(raw_pin);
    return BuildRepository{hex.data(), std::string(text)};
}

std::expected<BuildTree, ManualError> read_build_tree(
    const std::filesystem::path& bare, std::string_view revision) {
    const bool head = revision == "refs/heads/braurser-preview";
    if (!bare.is_absolute() || std::filesystem::is_symlink(bare) ||
        (!head && ((revision.size() != 40 && revision.size() != 64) ||
         !std::ranges::all_of(revision, [](unsigned char ch) {
             return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
         })))) return std::unexpected(fail("git-revision"));
    static const int initialized = git_libgit2_init();
    if (initialized < 0) return std::unexpected(fail("git-init"));
    git_repository* raw_repo = nullptr;
    if (git_repository_open_bare(&raw_repo, bare.c_str()) != 0) return std::unexpected(fail("git-open"));
    std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
    const std::string name(revision);
    git_object* raw_object = nullptr;
    if (git_revparse_single(&raw_object, repo.get(), name.c_str()) != 0) return std::unexpected(fail("git-revision"));
    std::unique_ptr<git_object, decltype(&git_object_free)> object(raw_object, git_object_free);
    const git_oid oid = *git_object_id(object.get());
    git_commit* raw_commit = nullptr;
    if (git_commit_lookup(&raw_commit, repo.get(), &oid) != 0) return std::unexpected(fail("git-commit"));
    std::unique_ptr<git_commit, decltype(&git_commit_free)> commit(raw_commit, git_commit_free);
    git_tree* raw_tree = nullptr;
    if (git_commit_tree(&raw_tree, commit.get()) != 0) return std::unexpected(fail("git-tree"));
    std::unique_ptr<git_tree, decltype(&git_tree_free)> tree(raw_tree, git_tree_free);
    BuildTree result;
    std::array<char, GIT_OID_MAX_HEXSIZE + 1> hex{};
    git_oid_tostr(hex.data(), hex.size(), &oid);
    result.commit = hex.data();
    if (!head && result.commit != revision) return std::unexpected(fail("git-revision"));
    git_oid_tostr(hex.data(), hex.size(), git_tree_id(tree.get()));
    result.tree = hex.data();
    std::uint64_t total = 0;
    std::size_t entries = 0;
    bool oversized_text = false;
    std::function<bool(git_tree*, const std::string&, unsigned)> walk;
    walk = [&](git_tree* current, const std::string& prefix, unsigned depth) {
        if (depth > 32) return false;
        for (std::size_t index = 0; index < git_tree_entrycount(current); ++index) {
            const auto* entry = git_tree_entry_byindex(current, index);
            if (!entry || ++entries > 512) return false;
            const char* raw = git_tree_entry_name(entry);
            const std::string component = raw ? raw : "";
            if (!br_valid_path_component(component.data(), component.size()) ||
                component.find('\\') != component.npos || !protocol::valid_utf8(component)) return false;
            std::string lower = component;
            std::ranges::transform(lower, lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (lower == ".git") return false;
            const auto path = prefix + component;
            if (path.size() > 1024) return false;
            const auto mode = git_tree_entry_filemode(entry);
            if (mode == GIT_FILEMODE_TREE) {
                git_tree* raw_child = nullptr;
                if (git_tree_lookup(&raw_child, repo.get(), git_tree_entry_id(entry)) != 0) return false;
                std::unique_ptr<git_tree, decltype(&git_tree_free)> child(raw_child, git_tree_free);
                if (!walk(child.get(), path + "/", depth + 1)) return false;
                continue;
            }
            if ((mode != GIT_FILEMODE_BLOB && mode != GIT_FILEMODE_BLOB_EXECUTABLE && mode != GIT_FILEMODE_LINK) ||
                result.files.size() >= 256) return false;
            const auto size = internal::blob_size(repo.get(), *git_tree_entry_id(entry));
            if (!size || *size > 4 * 1024 * 1024 - total) return false;
            total += *size;
            git_blob* raw_blob = nullptr;
            if (git_blob_lookup(&raw_blob, repo.get(), git_tree_entry_id(entry)) != 0) return false;
            std::unique_ptr<git_blob, decltype(&git_blob_free)> blob(raw_blob, git_blob_free);
            if (git_blob_rawsize(blob.get()) != *size) return false;
            const std::string_view bytes(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                                         static_cast<std::size_t>(*size));
            if (mode == GIT_FILEMODE_LINK) {
                if (bytes.empty() || bytes.size() > 4096 || bytes.front() == '/' ||
                    bytes.find_first_of("\\\r\n") != bytes.npos || bytes.find('\0') != bytes.npos) return false;
                const auto resolved = (std::filesystem::path(path).parent_path() / std::string(bytes)).lexically_normal();
                for (const auto& part : resolved) if (part == ".." || part == ".git") return false;
            }
            br_sha256_ctx checksum{};
            br_sha256_init(&checksum);
            br_sha256_update(&checksum, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
            std::array<std::uint8_t, 32> hash{};
            br_sha256_final(&checksum, hash.data());
            std::array<char, 65> digest{};
            br_hex32(hash.data(), digest.data());
            BuildFile file{path, static_cast<unsigned>(mode), *size, digest.data(), {}};
            if (bytes.find('\0') == bytes.npos && protocol::valid_utf8(bytes)) {
                if (*size > 448 * 1024) { oversized_text = true; return false; }
                file.text = std::string(bytes);
            }
            if (path == ".SRCINFO") {
                if (mode != GIT_FILEMODE_BLOB || !file.text || file.text->empty()) return false;
                result.srcinfo = *file.text;
            }
            result.files.push_back(std::move(file));
        }
        return true;
    };
    if (!walk(tree.get(), "", 0) || result.srcinfo.empty() ||
        std::ranges::none_of(result.files, [](const auto& file) {
            return file.path == "PKGBUILD" && file.text && !file.text->empty() &&
                (file.mode == GIT_FILEMODE_BLOB || file.mode == GIT_FILEMODE_BLOB_EXECUTABLE);
        })) return std::unexpected(fail(oversized_text ? "build-text-too-large" : "build-tree"));
    for (const auto& file : result.files) {
        if (file.mode != GIT_FILEMODE_LINK) continue;
        std::deque<std::string> remaining;
        for (const auto& part : std::filesystem::path(file.path)) remaining.push_back(part.string());
        std::vector<std::string> resolved;
        unsigned expanded = 0;
        while (!remaining.empty()) {
            auto part = std::move(remaining.front());
            remaining.pop_front();
            if (part.empty() || part == ".") continue;
            if (part == "..") {
                if (resolved.empty()) return std::unexpected(fail("tree-symlink"));
                resolved.pop_back();
                continue;
            }
            resolved.push_back(std::move(part));
            std::string path;
            for (const auto& component : resolved) {
                if (!path.empty()) path += '/';
                path += component;
            }
            const auto link = std::ranges::find_if(result.files, [&](const auto& candidate) {
                return candidate.path == path && candidate.mode == GIT_FILEMODE_LINK;
            });
            if (link == result.files.end()) continue;
            if (!link->text || ++expanded > 256) return std::unexpected(fail("tree-symlink"));
            resolved.pop_back();
            const std::filesystem::path target(*link->text);
            for (auto component = target.end(); component != target.begin();) remaining.push_front((--component)->string());
        }
        if (resolved.empty()) return std::unexpected(fail("tree-symlink"));
    }
    git_reference* raw_pin = nullptr;
    const auto pin = "refs/braurser/pins/" + result.commit;
    if (git_reference_create(&raw_pin, repo.get(), pin.c_str(), &oid, 1, nullptr) != 0)
        return std::unexpected(fail("git-pin"));
    git_reference_free(raw_pin);
    if (git_reference_ensure_log(repo.get(), pin.c_str()) != 0 ||
        !core::touch_file(bare / "logs" / pin)) return std::unexpected(fail("git-pin"));
    return result;
}

std::expected<BuildTree, ManualError> fetch_build_tree(
    GitHttp& http, std::string_view pkgbase, const std::filesystem::path& cache_root) {
    if (!br_valid_pkgname(pkgbase.data(), pkgbase.size())) return std::unexpected(fail("build-input"));
    auto bare = fetch_bare(http, "https://aur.archlinux.org/" + std::string(pkgbase) + ".git",
                           cache_root, "aur-" + std::string(pkgbase), "HEAD");
    if (!bare) return std::unexpected(bare.error());
    return read_build_tree(*bare, "refs/heads/braurser-preview");
}

std::expected<void, ManualError> prune_pins(
    const std::filesystem::path& cache_root, std::chrono::seconds max_age,
    std::span<const std::string> keep) {
    if (!cache_root.is_absolute() || max_age < std::chrono::seconds::zero())
        return std::unexpected(fail("pin-cache"));
    static const int initialized = git_libgit2_init();
    if (initialized < 0) return std::unexpected(fail("git-init"));
    const auto root = cache_root / "braurser/manual";
    std::error_code error;
    if (!std::filesystem::exists(root, error) && !error) return {};
    if (error || !core::ensure_private_dir(root)) return std::unexpected(fail("pin-cache"));
    struct CachedRepo {
        std::filesystem::path path;
        std::uint64_t size{};
        std::filesystem::file_time_type used{};
        bool protected_pin{};
    };
    std::vector<CachedRepo> repos;
    std::uint64_t total = 0;
    std::size_t entries = 0;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - max_age;
    try {
        for (const auto& directory : std::filesystem::directory_iterator(root)) {
            if (++entries > 65536) return std::unexpected(fail("pin-budget"));
            const auto name = directory.path().filename().string();
            if (!name.starts_with("aur-") || !br_valid_pkgname(name.data() + 4, name.size() - 4) ||
                directory.symlink_status().type() != std::filesystem::file_type::directory) continue;
            const auto bare = directory.path() / "repo.git";
            if (!core::ensure_private_dir(directory.path()) ||
                std::filesystem::symlink_status(bare).type() != std::filesystem::file_type::directory) continue;
            git_repository* raw_repo = nullptr;
            if (git_repository_open_bare(&raw_repo, bare.c_str()) != 0) continue;
            std::unique_ptr<git_repository, decltype(&git_repository_free)> repo(raw_repo, git_repository_free);
            git_reference_iterator* raw_iterator = nullptr;
            if (git_reference_iterator_glob_new(&raw_iterator, repo.get(), "refs/braurser/pins/*") != 0)
                return std::unexpected(fail("git-pin"));
            std::unique_ptr<git_reference_iterator, decltype(&git_reference_iterator_free)> iterator(raw_iterator, git_reference_iterator_free);
            CachedRepo cached{directory.path(), 0, std::filesystem::last_write_time(bare), false};
            git_reference* raw_ref = nullptr;
            int status = 0;
            std::vector<std::string> expired;
            while ((status = git_reference_next(&raw_ref, iterator.get())) == 0) {
                std::unique_ptr<git_reference, decltype(&git_reference_free)> reference(raw_ref, git_reference_free);
                const std::string ref(git_reference_name(reference.get()));
                const auto commit = std::string_view(ref).substr(std::string_view("refs/braurser/pins/").size());
                const bool protected_pin = std::ranges::find(keep, commit) != keep.end();
                cached.protected_pin = cached.protected_pin || protected_pin;
                auto stamp = std::filesystem::last_write_time(bare / "logs" / ref, error);
                if (error) {
                    error.clear();
                    stamp = std::filesystem::last_write_time(bare / ref, error);
                }
                if (error) {
                    error.clear();
                    stamp = std::filesystem::last_write_time(bare / "packed-refs", error);
                }
                if (error) { error.clear(); cached.protected_pin = true; continue; }
                cached.used = std::max(cached.used, stamp);
                if (!protected_pin && stamp < cutoff) expired.push_back(ref);
            }
            if (status != GIT_ITEROVER) return std::unexpected(fail("git-pin"));
            for (const auto& ref : expired) {
                if (git_reference_remove(repo.get(), ref.c_str()) != 0) return std::unexpected(fail("git-pin"));
            }
            for (const auto& file : std::filesystem::recursive_directory_iterator(directory.path())) {
                if (++entries > 65536) return std::unexpected(fail("pin-budget"));
                if (file.symlink_status().type() != std::filesystem::file_type::regular) continue;
                const auto size = file.file_size();
                if (size > UINT64_MAX - total || size > UINT64_MAX - cached.size)
                    return std::unexpected(fail("pin-budget"));
                cached.size += size;
                total += size;
            }
            repos.push_back(std::move(cached));
        }
        std::ranges::sort(repos, {}, &CachedRepo::used);
        for (const auto& repo : repos) {
            if (total <= 256ULL * 1024 * 1024) break;
            if (repo.protected_pin) continue;
            if (!core::remove_tree_confined(root, repo.path)) return std::unexpected(fail("pin-cache"));
            total -= repo.size;
        }
    } catch (const std::filesystem::filesystem_error&) { return std::unexpected(fail("pin-cache")); }
    return {};
}

}
