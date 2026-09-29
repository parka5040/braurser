#include <braurser/daemon/catalog/sources.hpp>
#include <braurser/validate/validate.hpp>

#include <libxml/xmlreader.h>
#include <zlib.h>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace braurser::daemon::catalog {
namespace {

struct GzipInput {
    gzFile file{};
    std::uint64_t expanded{};
    bool failed{};
};

int gzip_read(void* context, char* buffer, int length) {
    auto& input = *static_cast<GzipInput*>(context);
    if (input.expanded >= 256ULL * 1024 * 1024) {
        input.failed = true;
        return -1;
    }
    const auto cap = static_cast<unsigned>(std::min<std::uint64_t>(static_cast<unsigned>(length),
                                            256ULL * 1024 * 1024 - input.expanded));
    const int count = gzread(input.file, buffer, cap);
    if (count < 0) input.failed = true;
    if (count > 0) input.expanded += static_cast<unsigned>(count);
    return count;
}

int gzip_close(void*) { return 0; }

std::string attribute(xmlTextReaderPtr reader, const char* key) {
    xmlChar* text = xmlTextReaderGetAttribute(reader, BAD_CAST key);
    if (!text) return {};
    std::string value(reinterpret_cast<const char*>(text));
    xmlFree(text);
    return value.size() <= 65536 ? value : std::string{};
}

std::string locale_base() {
    const char* lang = std::getenv("LANG");
    if (!lang) return {};
    std::string result(lang);
    if (const auto cut = result.find_first_of(".@"); cut != std::string::npos) result.resize(cut);
    if (result.size() > 32) return {};
    return result;
}

bool safe_component(std::string_view text) {
    return br_valid_path_component(text.data(), text.size());
}

bool safe_https(std::string_view text) {
    return text.starts_with("https://") && text.size() <= 2048 && text.find_first_of("\r\n\t \\<>") == text.npos;
}

}

std::expected<std::vector<AppStreamRecord>, CatalogError> AppStreamReader::read_records(const std::filesystem::path& path) const {
    if (!path.is_absolute() || path.extension() != ".gz") return std::unexpected(CatalogError{"appstream-path", {}});
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return std::unexpected(CatalogError{"appstream-open", {}});
    struct stat statbuf{};
    if (fstat(fd, &statbuf) != 0 || !S_ISREG(statbuf.st_mode) || statbuf.st_size > 256LL * 1024 * 1024) {
        close(fd);
        return std::unexpected(CatalogError{"appstream-file", {}});
    }
    GzipInput input{gzdopen(fd, "rb")};
    if (!input.file) { close(fd); return std::unexpected(CatalogError{"appstream-gzip", {}}); }
    using Reader = std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)>;
    Reader reader(xmlReaderForIO(gzip_read, gzip_close, &input, nullptr, nullptr,
                                  XML_PARSE_NONET | XML_PARSE_NO_XXE | XML_PARSE_NOBLANKS), xmlFreeTextReader);
    if (!reader) { gzclose(input.file); return std::unexpected(CatalogError{"appstream-xml", {}}); }
    std::vector<AppStreamRecord> result;
    AppMetadata app;
    std::vector<CachedIcon> icons;
    std::uint32_t icon_width{};
    std::uint32_t icon_height{};
    std::uint32_t best_icon_area{};
    std::string origin;
    std::string element;
    std::string parent;
    std::string lang;
    std::string best_name;
    std::string best_summary;
    std::string selected_icon;
    std::string image_kind;
    std::string homepage_kind;
    std::string launchable_kind;
    std::string component_type;
    const auto locale = locale_base();
    int component_depth = -1;
    int description_depth = -1;
    int paragraph_depth = -1;
    int screenshot_depth = -1;
    bool bullet = false;
    bool acceptable = false;
    std::string paragraph;
    std::string screenshot_caption;
    std::size_t screenshot_start{};
    int status = 0;
    while ((status = xmlTextReaderRead(reader.get())) == 1) {
        const int kind = xmlTextReaderNodeType(reader.get());
        if (kind == XML_READER_TYPE_DOCUMENT_TYPE || kind == XML_READER_TYPE_ENTITY ||
            kind == XML_READER_TYPE_ENTITY_REFERENCE) {
            reader.reset(); gzclose(input.file);
            return std::unexpected(CatalogError{"appstream-entity", {}});
        }
        const xmlChar* raw = xmlTextReaderConstLocalName(reader.get());
        const std::string node = raw ? reinterpret_cast<const char*>(raw) : "";
        const int depth = xmlTextReaderDepth(reader.get());
        if (kind == XML_READER_TYPE_ELEMENT) {
            if (node == "components") origin = attribute(reader.get(), "origin");
            if (node == "component") {
                app = {};
                icons.clear();
                best_icon_area = 0;
                best_name.clear(); best_summary.clear(); selected_icon.clear();
                component_depth = depth;
                component_type = attribute(reader.get(), "type");
                acceptable = component_type == "desktop-application" || component_type == "console-application" ||
                             component_type == "desktop";
            }
            if (!acceptable) continue;
            if (node == "description") description_depth = depth;
            if (description_depth >= 0 && (node == "p" || node == "li") && paragraph_depth < 0) {
                paragraph_depth = depth;
                paragraph.clear();
                bullet = node == "li";
            }
            if (node == "screenshot") {
                screenshot_depth = depth;
                screenshot_caption.clear();
                screenshot_start = app.screenshots.size();
            }
            if (node == "icon") {
                const auto kind_attr = attribute(reader.get(), "type");
                const auto width = attribute(reader.get(), "width");
                const auto height = attribute(reader.get(), "height");
                selected_icon.clear();
                if (kind_attr == "cached" && !width.empty() && !height.empty() &&
                    width.size() <= 4 && height.size() <= 4 && safe_component(origin)) {
                    const auto [wend, wec] = std::from_chars(width.data(), width.data() + width.size(), icon_width);
                    const auto [hend, hec] = std::from_chars(height.data(), height.data() + height.size(), icon_height);
                    if (wec == std::errc{} && hec == std::errc{} &&
                        wend == width.data() + width.size() && hend == height.data() + height.size() &&
                        icon_width > 0 && icon_height > 0 && icon_width <= 512 && icon_height <= 512)
                        selected_icon = width + "x" + height;
                }
            }
            if (node == "image") image_kind = attribute(reader.get(), "type");
            if (node == "url") homepage_kind = attribute(reader.get(), "type");
            if (node == "launchable") launchable_kind = attribute(reader.get(), "type");
            lang = attribute(reader.get(), "xml:lang");
            if (lang.empty()) lang = attribute(reader.get(), "lang");
            parent = element;
            element = node;
        } else if (kind == XML_READER_TYPE_TEXT || kind == XML_READER_TYPE_CDATA) {
            if (!acceptable) continue;
            const xmlChar* raw_text = xmlTextReaderConstValue(reader.get());
            if (!raw_text) continue;
            std::string text(reinterpret_cast<const char*>(raw_text));
            if (text.size() > 65536) { reader.reset(); gzclose(input.file); return std::unexpected(CatalogError{"appstream-text", {}}); }
            if (paragraph_depth >= 0) {
                if (text.size() > 65536 - paragraph.size()) {
                    reader.reset(); gzclose(input.file);
                    return std::unexpected(CatalogError{"appstream-paragraph", {}});
                }
                paragraph += text;
                continue;
            }
            if (element == "id" && parent != "provides") app.appstream_id = text;
            else if (element == "pkgname") app.pkgname = text;
            else if (element == "name" && parent == "developer") app.developer = text;
            else if (element == "developer_name") app.developer = text;
            else if (element == "name") {
                if (lang.empty() && best_name.empty()) best_name = text;
                if (!locale.empty() && lang == locale) app.name = text;
            } else if (element == "summary") {
                if (lang.empty() && best_summary.empty()) best_summary = text;
                if (!locale.empty() && lang == locale) app.summary = text;
            } else if (element == "category" && app.categories.size() < 512) app.categories.push_back(text);
            else if (element == "project_license") app.license = text;
            else if (element == "launchable" && launchable_kind == "desktop-id") app.launchable = text;
            else if (element == "url" && homepage_kind == "homepage" && safe_https(text)) app.homepage = text;
            else if (element == "caption" && screenshot_depth >= 0) screenshot_caption = text;
            else if (element == "image" && screenshot_depth >= 0 && image_kind == "source" &&
                     safe_https(text) && app.screenshots.size() < 5) app.screenshots.push_back({text, screenshot_caption});
            else if (element == "icon" && !selected_icon.empty() && safe_component(text) && icons.size() < 512) {
                auto icon = std::filesystem::path("/usr/share/swcatalog/icons") / origin / selected_icon / text;
                icons.push_back({icon, icon_width, icon_height});
                if (icon_width * icon_height >= best_icon_area) {
                    best_icon_area = icon_width * icon_height;
                    app.icon_path = icon.string();
                }
            }
        } else if (kind == XML_READER_TYPE_END_ELEMENT) {
            if (node == "component" && depth == component_depth) {
                if (app.name.empty()) app.name = best_name;
                if (app.summary.empty()) app.summary = best_summary;
                if (acceptable && !app.appstream_id.empty() &&
                    br_valid_pkgname(app.pkgname.data(), app.pkgname.size())) {
                    if (result.size() >= 300000) {
                        reader.reset(); gzclose(input.file);
                        return std::unexpected(CatalogError{"appstream-records", {}});
                    }
                    result.push_back({std::move(app), std::move(icons)});
                }
                acceptable = false;
                component_depth = -1;
            }
            if (depth == paragraph_depth && (node == "p" || node == "li")) {
                if (!paragraph.empty() && app.description_paragraphs.size() < 512)
                    app.description_paragraphs.push_back(bullet ? "• " + paragraph : paragraph);
                paragraph_depth = -1;
            }
            if (node == "description" && depth == description_depth) description_depth = -1;
            if (node == "screenshot" && depth == screenshot_depth) {
                if (app.screenshots.size() > screenshot_start)
                    app.screenshots.back().caption = screenshot_caption;
                screenshot_depth = -1;
            }
            element.clear();
            parent.clear();
            lang.clear();
        }
    }
    reader.reset();
    const int gzip_status = gzclose(input.file);
    if (status < 0 || input.failed || gzip_status != Z_OK)
        return std::unexpected(CatalogError{"appstream-invalid", {}});
    return result;
}

std::expected<std::vector<AppMetadata>, CatalogError> AppStreamReader::read(const std::filesystem::path& path) const {
    auto records = read_records(path);
    if (!records) return std::unexpected(records.error());
    std::vector<AppMetadata> metadata;
    metadata.reserve(records->size());
    for (auto& record : *records) metadata.push_back(std::move(record.metadata));
    return metadata;
}

std::expected<std::vector<AppMetadata>, CatalogError> AppStreamReader::discover() const {
    std::vector<AppMetadata> result;
    for (const auto* directory : {"/usr/share/swcatalog/xml", "/usr/share/app-info/xmls"}) {
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) continue;
        for (const auto& item : std::filesystem::directory_iterator(directory, ec)) {
            if (item.path().extension() != ".gz" || item.path().stem().extension() != ".xml" ||
                item.is_symlink(ec)) continue;
            auto parsed = read(item.path());
            if (!parsed) return std::unexpected(parsed.error());
            result.insert(result.end(), std::make_move_iterator(parsed->begin()), std::make_move_iterator(parsed->end()));
        }
        if (ec) return std::unexpected(CatalogError{"appstream-discovery", {}});
    }
    return result;
}

}
