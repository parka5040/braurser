#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/daemon/core/files.hpp>

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>
#include <zlib.h>
#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <span>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::catalog {
namespace {

std::string fixture(const char* name) {
    std::ifstream input(std::filesystem::path(CATALOG_FIXTURES) / name, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string gzip(std::string_view text) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
    std::string result(compressBound(static_cast<uLong>(text.size())), '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
    stream.avail_in = static_cast<uInt>(text.size());
    stream.next_out = reinterpret_cast<Bytef*>(result.data());
    stream.avail_out = static_cast<uInt>(result.size());
    const int status = deflate(&stream, Z_FINISH);
    result.resize(static_cast<std::size_t>(stream.total_out));
    deflateEnd(&stream);
    return status == Z_STREAM_END ? result : std::string{};
}

std::string archive_bytes(int filter, std::string desc = fixture("sync.desc")) {
    std::string result(1024 * 1024, '\0');
    std::size_t used = 0;
    archive* writer = archive_write_new();
    archive_write_set_format_pax_restricted(writer);
    if (filter == ARCHIVE_FILTER_GZIP) archive_write_add_filter_gzip(writer);
    if (filter == ARCHIVE_FILTER_ZSTD) archive_write_add_filter_zstd(writer);
    if (filter == ARCHIVE_FILTER_XZ) archive_write_add_filter_xz(writer);
    if (archive_write_open_memory(writer, result.data(), result.size(), &used) != ARCHIVE_OK) {
        archive_write_free(writer);
        return {};
    }
    archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, "fixture-app-1.2-3/desc");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, static_cast<la_int64_t>(desc.size()));
    archive_write_header(writer, entry);
    archive_write_data(writer, desc.data(), desc.size());
    archive_entry_free(entry);
    archive_write_close(writer);
    archive_write_free(writer);
    result.resize(used);
    return result;
}

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("braurser-catalog-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

class FakeProcess final : public core::ProcessRunner {
public:
    std::vector<core::ProcessSpec> specs;
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec& spec) override {
        specs.push_back(spec);
        core::ProcessResult result;
        result.exit_code = 0;
        result.out = spec.args == std::vector<std::string>{"--repo-list"} ? "testing\ncustom\n" : "/var/lib/pacman\n";
        return result;
    }
};

class FakeDecoder final : public core::ProcessRunner {
public:
    std::string png_bytes;
    std::vector<core::ProcessSpec> specs;
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec& spec) override {
        specs.push_back(spec);
        std::ofstream file(spec.args.at(1), std::ios::binary);
        file.write(png_bytes.data(), static_cast<std::streamsize>(png_bytes.size()));
        core::ProcessResult result;
        result.exit_code = 0;
        return result;
    }
};

class FakeHttp final : public core::HttpClient {
public:
    core::HttpResponse response;
    std::vector<core::HttpRequest> requests;
    std::expected<core::HttpResponse, core::HttpError> get(const core::HttpRequest& request) override {
        requests.push_back(request);
        return response;
    }
};

TEST(CatalogSource, PacmanConfigurationBoundsAndValidates) {
    FakeProcess runner;
    auto configured = PacmanConfig(runner).read();
    ASSERT_TRUE(configured);
    EXPECT_EQ(configured->names, (std::vector<std::string>{"testing", "custom"}));
    EXPECT_EQ(configured->db_path, "/var/lib/pacman");
    ASSERT_EQ(runner.specs.size(), 2);
    for (const auto& spec : runner.specs) {
        EXPECT_EQ(spec.executable, "/usr/bin/pacman-conf");
        EXPECT_EQ(spec.env, (std::vector<std::pair<std::string, std::string>>{{"LC_ALL", "C"}}));
        EXPECT_EQ(spec.max_stdout, 65536);
        EXPECT_EQ(spec.timeout, std::chrono::seconds(10));
    }
}

class HoldProcess final : public core::ProcessRunner {
public:
    std::string output;
    int exit_code{};
    std::vector<core::ProcessSpec> specs;
    std::expected<core::ProcessResult, core::ProcessError> run(const core::ProcessSpec& spec) override {
        specs.push_back(spec);
        core::ProcessResult result;
        result.exit_code = exit_code;
        result.out = output;
        return result;
    }
};

TEST(CatalogSource, HoldPackagesAreValidatedAndFailClosed) {
    HoldProcess runner;
    runner.output = "pacman\nglibc\npacman\n";
    auto hold = PacmanConfig(runner).hold_packages();
    ASSERT_TRUE(hold);
    EXPECT_EQ(*hold, (std::vector<std::string>{"pacman", "glibc"}));
    ASSERT_EQ(runner.specs.size(), 1U);
    EXPECT_EQ(runner.specs.front().executable, "/usr/bin/pacman-conf");
    EXPECT_EQ(runner.specs.front().args, (std::vector<std::string>{"HoldPkg"}));
    runner.output.clear();
    hold = PacmanConfig(runner).hold_packages();
    ASSERT_TRUE(hold);
    EXPECT_TRUE(hold->empty());
    for (const auto* bad : {"pacman\n--root\n", "Bad Name\n", "pacman\n\nglibc\n"}) {
        runner.output = bad;
        EXPECT_FALSE(PacmanConfig(runner).hold_packages()) << bad;
    }
    runner.output = "pacman\n";
    runner.exit_code = 1;
    EXPECT_FALSE(PacmanConfig(runner).hold_packages());
}

TEST(CatalogSource, SyncArchivesGeneratedInAllSupportedFormats) {
    for (int filter : {ARCHIVE_FILTER_GZIP, ARCHIVE_FILTER_ZSTD, ARCHIVE_FILTER_XZ}) {
        auto bytes = archive_bytes(filter);
        ASSERT_FALSE(bytes.empty());
        auto parsed = SyncDbReader{}.read_bytes(std::as_bytes(std::span(bytes)), "custom");
        ASSERT_TRUE(parsed) << parsed.error().code;
        ASSERT_EQ(parsed->packages.size(), 1);
        EXPECT_EQ(parsed->packages[0].name, "fixture-app");
        EXPECT_EQ(parsed->packages[0].depends, (std::vector<std::string>{"libfixture>=1"}));
        EXPECT_EQ(parsed->packages[0].licenses.size(), 2);
        EXPECT_EQ(parsed->packages[0].download_size, 42);
    }
    EXPECT_FALSE(parse_repo_desc("%NAME%\n../bad\n\n%VERSION%\n1\n", "custom"));
    auto rejected = archive_bytes(ARCHIVE_FILTER_GZIP, "%NAME%\n../bad\n\n%VERSION%\n1\n");
    auto records = SyncDbReader{}.read_bytes(std::as_bytes(std::span(rejected)), "custom");
    ASSERT_TRUE(records);
    EXPECT_TRUE(records->packages.empty());
    EXPECT_EQ(records->rejected, 1);
    std::string invalid = "not tar";
    EXPECT_FALSE(SyncDbReader{}.read_bytes(std::as_bytes(std::span(invalid)), "custom"));
}

TEST(CatalogSource, AurSaxStreamingAndRpcBudget) {
    auto zipped = gzip(fixture("aur.json"));
    auto packages = AurDump{}.parse(std::as_bytes(std::span(zipped)));
    ASSERT_TRUE(packages) << packages.error().code;
    ASSERT_EQ(packages->size(), 1);
    EXPECT_EQ(packages->front().name, "fixture-app");
    EXPECT_EQ(packages->front().votes, 10);
    FakeHttp http;
    http.response.status = 200;
    http.response.body = R"({"type":"multiinfo","results":[{"Name":"fixture-app","Version":"2.0-1"}]})";
    AurRpcClient client(http, 1);
    const std::vector<std::string> names = {"fixture-app"};
    auto response = client.info(names);
    ASSERT_TRUE(response) << response.error().code;
    ASSERT_EQ(response->size(), 1);
    EXPECT_EQ(http.requests.front().allowed_hosts, (std::vector<std::string>{"aur.archlinux.org"}));
    EXPECT_FALSE(client.info(names));
}

TEST(CatalogSource, AurFetcherKeepsOldDumpOnInvalidRefresh) {
    TempDir root;
    FakeHttp http;
    http.response.status = 200;
    http.response.body = gzip(fixture("aur.json"));
    AurDumpFetcher fetcher(http);
    auto initial = fetcher.fetch(root.path);
    ASSERT_TRUE(initial) << initial.error().code;
    const auto path = root.path / "braurser/downloads/packages-meta-ext-v1.json.gz";
    ASSERT_TRUE(core::read_bounded(path, 64 * 1024 * 1024));
    http.response.body = "corrupted";
    EXPECT_FALSE(fetcher.fetch(root.path));
    auto old = core::read_bounded(path, 64 * 1024 * 1024);
    ASSERT_TRUE(old);
    EXPECT_NE(*old, "corrupted");
    struct stat info{};
    ASSERT_EQ(stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 077, 0);
}

TEST(CatalogSource, AppStreamRejectsEntitiesAndReadsMetadata) {
    TempDir temp;
    for (const auto* name : {"appstream.xml", "attack.xml", "external.xml"}) {
        auto output = temp.path / (std::string(name) + ".gz");
        auto compressed = gzip(fixture(name));
        std::ofstream file(output, std::ios::binary);
        file.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
        file.close();
        auto metadata = AppStreamReader{}.read(output);
        if (std::string_view(name) != "appstream.xml") { EXPECT_FALSE(metadata); continue; }
        auto records = AppStreamReader{}.read_records(output);
        ASSERT_TRUE(records) << records.error().code;
        ASSERT_EQ(records->front().icons.size(), 1);
        EXPECT_EQ(records->front().icons.front().width, 64);
        ASSERT_TRUE(metadata) << metadata.error().code;
        ASSERT_EQ(metadata->size(), 1);
        EXPECT_EQ(metadata->front().pkgname, "fixture-app");
        EXPECT_EQ(metadata->front().icon_path, "/usr/share/swcatalog/icons/fixture/64x64/fixture.png");
        EXPECT_EQ(metadata->front().description_paragraphs, (std::vector<std::string>{"First paragraph.", "• First bullet"}));
        ASSERT_EQ(metadata->front().screenshots.size(), 1);
    }
}

TEST(CatalogSource, DesktopEntriesAreStrictAndLocalized) {
    auto good = parse_desktop_entry(fixture("fixture.desktop"), "fr_FR.UTF-8");
    ASSERT_TRUE(good) << good.error().code;
    EXPECT_EQ(good->name, "Exemple");
    EXPECT_EQ(good->generic_name, "Demo Program");
    EXPECT_EQ(good->categories.size(), 2);
    EXPECT_FALSE(parse_desktop_entry(fixture("invalid.desktop")));
    EXPECT_FALSE(parse_desktop_entry("[Desktop Entry]\nName=Hi\nType=Application\nName=Again\n"));
    EXPECT_FALSE(parse_desktop_entry(std::string("[Desktop Entry]\nName=") + char(0xC0) + "\n"));
}

TEST(CatalogSource, DesktopLocalesWithoutCodesetDoNotThrow) {
    for (const auto* locale : {"", "C", "POSIX", "fr", "fr_FR", "fr_FR@euro"}) {
        auto entry = parse_desktop_entry(fixture("fixture.desktop"), locale);
        ASSERT_TRUE(entry) << locale << " " << entry.error().code;
        EXPECT_EQ(entry->name, std::string_view(locale).starts_with("fr") ? "Exemple" : "Fixture") << locale;
    }
}

TEST(CatalogSource, AppStreamReadsUnderLocaleWithoutCodeset) {
    TempDir temp;
    const char* saved = std::getenv("LANG");
    const std::optional<std::string> previous = saved ? std::optional<std::string>(saved) : std::nullopt;
    auto output = temp.path / "appstream.xml.gz";
    auto compressed = gzip(fixture("appstream.xml"));
    std::ofstream file(output, std::ios::binary);
    file.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    file.close();
    for (const auto* lang : {"C", "POSIX", "en_US"}) {
        ASSERT_EQ(setenv("LANG", lang, 1), 0);
        auto metadata = AppStreamReader{}.read(output);
        EXPECT_TRUE(metadata) << lang << " " << (metadata ? std::string() : metadata.error().code);
    }
    if (previous) setenv("LANG", previous->c_str(), 1);
    else unsetenv("LANG");
}

TEST(CatalogSource, IconCacheValidatesPngAndRejectsSymlink) {
    TempDir temp;
    const auto input = temp.path / "icon.png";
    std::string image(33, '\0');
    const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    for (std::size_t i = 0; i < signature.size(); ++i) image[i] = static_cast<char>(signature[i]);
    image[11] = 13;
    image.replace(12, 4, "IHDR");
    image[19] = 1;
    image[23] = 1;
    { std::ofstream file(input, std::ios::binary); file.write(image.data(), static_cast<std::streamsize>(image.size())); }
    FakeProcess runner;
    auto converted = IconConverter(runner).convert(input, temp.path);
    ASSERT_TRUE(converted) << converted.error().code;
    EXPECT_EQ(converted->extension(), ".png");
    EXPECT_TRUE(runner.specs.empty());
    auto saved = core::read_bounded(*converted, 8 * 1024 * 1024);
    ASSERT_TRUE(saved);
    EXPECT_EQ(*saved, image);
    const auto jxl = temp.path / "icon.jxl";
    { std::ofstream file(jxl, std::ios::binary); file << "JXL"; }
    FakeDecoder decoder;
    decoder.png_bytes = image;
    auto decoded = IconConverter(decoder).convert(jxl, temp.path);
    ASSERT_TRUE(decoded) << decoded.error().code;
    ASSERT_EQ(decoder.specs.size(), 1);
    EXPECT_EQ(decoder.specs[0].executable, "/usr/bin/djxl");
    EXPECT_TRUE(decoder.specs[0].env.empty());
    EXPECT_EQ(decoder.specs[0].args.front(), jxl);
    image[18] = 2;
    const auto invalid = temp.path / "too-large.png";
    { std::ofstream file(invalid, std::ios::binary); file.write(image.data(), static_cast<std::streamsize>(image.size())); }
    EXPECT_FALSE(IconConverter(runner).convert(invalid, temp.path));
    std::filesystem::remove(input);
    std::filesystem::create_symlink(*converted, input);
    EXPECT_FALSE(IconConverter(runner).convert(input, temp.path));
}

TEST(CatalogSource, LocalReaderRejectsSymlinkEntries) {
    TempDir temp;
    const auto local = temp.path / "local";
    std::filesystem::create_directory(local);
    const auto pkg = local / "fixture-app-1.2-3";
    std::filesystem::create_directory(pkg);
    { std::ofstream desc(pkg / "desc"); desc << fixture("sync.desc") << "%REASON%\n1\n\n"; }
    std::filesystem::create_directory_symlink(pkg, local / "other-1");
    auto read = LocalDbReader{}.read(temp.path);
    ASSERT_TRUE(read) << read.error().code;
    ASSERT_EQ(read->size(), 1);
    EXPECT_EQ(read->front().package.reason, domain::InstallReason::Dependency);
    std::filesystem::create_symlink(pkg / "desc", pkg / "files");
    EXPECT_FALSE(LocalDbReader{}.files(temp.path, "fixture-app-1.2-3"));
}

}
}
