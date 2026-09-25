#ifndef BRAURSER_PROTOCOL_DTO_HPP
#define BRAURSER_PROTOCOL_DTO_HPP

#include <nlohmann/json.hpp>
#include <braurser/validate/validate.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

namespace braurser::protocol {

/// in: text is untrusted UTF-8 bytes.
/// out: true only for well-formed UTF-8.
bool valid_utf8(std::string_view text);

/// in: value is untrusted JSON, key identifies a field.
/// err: std::invalid_argument when size, depth or field constraints are violated.
void validate_wire(const nlohmann::json& value, std::string_view key = {});

template <class Tag>
struct WireEnum {
    std::string value;
    friend bool operator==(const WireEnum&, const WireEnum&) = default;
};

struct SourceKindTag {
    static constexpr std::string_view allowed = "official community git outside";
};
using SourceKind = WireEnum<SourceKindTag>;

struct SearchSortTag {
    static constexpr std::string_view allowed = "relevance name";
};
using SearchSort = WireEnum<SearchSortTag>;

struct VariantKindTag {
    static constexpr std::string_view allowed = "standard bin git appimage nightly beta stable wayland";
};
using VariantKind = WireEnum<VariantKindTag>;

struct RestartLevelTag {
    static constexpr std::string_view allowed = "none restart-app restart-service logout reboot unknown";
};
using RestartLevel = WireEnum<RestartLevelTag>;

struct JobStateTag {
    static constexpr std::string_view allowed = "queued waiting-auth running cancelling succeeded failed cancelled needs-attention";
};
using JobState = WireEnum<JobStateTag>;

struct JobPhaseTag {
    static constexpr std::string_view allowed = "downloading building installing removing finishing checking";
};
using JobPhase = WireEnum<JobPhaseTag>;

struct ProgressUnitTag {
    static constexpr std::string_view allowed = "bytes items";
};
using ProgressUnit = WireEnum<ProgressUnitTag>;

struct AuthStateTag {
    static constexpr std::string_view allowed = "required prompting authorized cancelled failed unavailable";
};
using AuthState = WireEnum<AuthStateTag>;

struct UiPreferenceTag {
    static constexpr std::string_view allowed = "auto gtk qt";
};
using UiPreference = WireEnum<UiPreferenceTag>;

struct HelperPreferenceTag {
    static constexpr std::string_view allowed = "auto paru yay";
};
using HelperPreference = WireEnum<HelperPreferenceTag>;

struct CheckFrequencyTag {
    static constexpr std::string_view allowed = "never daily weekly";
};
using CheckFrequency = WireEnum<CheckFrequencyTag>;

struct ThemeTag {
    static constexpr std::string_view allowed = "system light dark";
};
using Theme = WireEnum<ThemeTag>;

struct ReceiptScopeTag {
    static constexpr std::string_view allowed = "user system";
};
using ReceiptScope = WireEnum<ReceiptScopeTag>;

struct LauncherOwnerTag {
    static constexpr std::string_view allowed = "braurser outside";
};
using LauncherOwner = WireEnum<LauncherOwnerTag>;

struct ConnectivityTag {
    static constexpr std::string_view allowed = "online offline unknown";
};
using Connectivity = WireEnum<ConnectivityTag>;

struct ElevationTag {
    static constexpr std::string_view allowed = "pkexec sudo-askpass unavailable";
};
using Elevation = WireEnum<ElevationTag>;

struct InitTag {
    static constexpr std::string_view allowed = "openrc systemd runit s6 dinit unknown";
};
using Init = WireEnum<InitTag>;

struct SelectionKindTag {
    static constexpr std::string_view allowed = "system community manual";
};
using SelectionKind = WireEnum<SelectionKindTag>;

struct QuestionKindTag {
    static constexpr std::string_view allowed = "variant provider build_review conflict news_ack git_redirect";
};
using QuestionKind = WireEnum<QuestionKindTag>;

struct BlockKindTag {
    static constexpr std::string_view allowed = "heading paragraph list code note warning";
};
using BlockKind = WireEnum<BlockKindTag>;

struct InlineKindTag {
    static constexpr std::string_view allowed = "text link";
};
using InlineKind = WireEnum<InlineKindTag>;

struct TransactionKindTag {
    static constexpr std::string_view allowed = "install remove update-system update-community update-and-install manual-install manual-upgrade manual-remove setup-helper upgrade-system upgrade-community";
};
using TransactionKind = WireEnum<TransactionKindTag>;

template <class Tag>
void from_json(const nlohmann::json& j, WireEnum<Tag>& out) {
    if (!j.is_string()) throw std::invalid_argument("invalid enum");
    const auto text = j.get<std::string>();
    auto allowed = Tag::allowed;
    while (!allowed.empty()) {
        const auto end = allowed.find(' ');
        if (allowed.substr(0, end) == text) {
            out.value = text;
            return;
        }
        if (end == std::string_view::npos) break;
        allowed.remove_prefix(end + 1);
    }
    throw std::invalid_argument("invalid enum");
}

template <class Tag>
void to_json(nlohmann::json& j, const WireEnum<Tag>& value) {
    WireEnum<Tag> checked;
    from_json(nlohmann::json(value.value), checked);
    j = value.value;
}

struct Inline;
struct WikiBlock;
struct QuestionChoice;

void from_json(const nlohmann::json&, Inline&);
void to_json(nlohmann::json&, const Inline&);
void from_json(const nlohmann::json&, WikiBlock&);
void to_json(nlohmann::json&, const WikiBlock&);
void from_json(const nlohmann::json&, QuestionChoice&);
void to_json(nlohmann::json&, const QuestionChoice&);

struct PackageRef {
    SourceKind source{};
    std::optional<std::string> repo{};
    std::string name{};
};

void from_json(const nlohmann::json& j, PackageRef& out);
void to_json(nlohmann::json& j, const PackageRef& value);

struct PackageSummary {
    PackageRef package{};
    std::string version{};
    std::string description{};
    bool installed{};
    std::optional<std::string> installedVersion{};
    std::optional<std::uint64_t> installedSize{};
    std::optional<std::uint64_t> downloadSize{};
    std::optional<std::string> appName{};
    std::optional<std::string> iconPath{};
    std::optional<std::vector<std::string>> alsoIn{};
    std::optional<bool> shadowed{};
};

void from_json(const nlohmann::json& j, PackageSummary& out);
void to_json(nlohmann::json& j, const PackageSummary& value);

struct Screenshot {
    std::string url{};
    std::string caption{};
};

void from_json(const nlohmann::json& j, Screenshot& out);
void to_json(nlohmann::json& j, const Screenshot& value);

struct AppMetadata {
    std::string appstream_id{};
    std::string name{};
    std::string summary{};
    std::vector<std::string> description_paragraphs{};
    std::optional<std::string> icon_path{};
    std::vector<Screenshot> screenshots{};
    std::vector<std::string> categories{};
    std::vector<std::string> launchables{};
};

void from_json(const nlohmann::json& j, AppMetadata& out);
void to_json(nlohmann::json& j, const AppMetadata& value);

struct CommunityInfo {
    std::optional<std::string> maintainer{};
    std::optional<std::string> submitter{};
    std::uint64_t votes{};
    double popularity{};
    std::optional<std::string> out_of_date_since{};
    std::string first_submitted{};
    std::string last_modified{};
    std::string package_base{};
    std::optional<std::string> upstream_url{};
};

void from_json(const nlohmann::json& j, CommunityInfo& out);
void to_json(nlohmann::json& j, const CommunityInfo& value);

struct PackageDetails {
    PackageRef package{};
    std::string version{};
    std::string description{};
    std::vector<std::string> dependencies{};
    std::optional<std::string> url{};
    std::optional<std::vector<std::string>> alsoIn{};
    std::optional<bool> shadowed{};
};

void from_json(const nlohmann::json& j, PackageDetails& out);
void to_json(nlohmann::json& j, const PackageDetails& value);

struct SearchFilters {
    bool installed{};
    bool not_installed{};
    bool explicit_{};
    bool dependency{};
    bool orphans{};
    bool foreign{};
    bool native{};
    bool upgradable{};
    std::vector<std::string> repos{};
    std::vector<std::string> groups{};
    bool aur_only{};
    bool repo_only{};
    bool apps_only{};
    std::optional<std::string> category{};
};

void from_json(const nlohmann::json& j, SearchFilters& out);
void to_json(nlohmann::json& j, const SearchFilters& value);

struct CatalogFilters {
    bool installed{};
    bool explicit_{};
    bool deps{};
    bool orphans{};
    bool foreign{};
    bool native{};
    bool upgradable{};
    std::optional<std::string> sourceRepo{};
    std::optional<std::string> group{};
    bool aurOnly{};
    bool repoOnly{};
    bool appsOnly{};
};

void from_json(const nlohmann::json& j, CatalogFilters& out);
void to_json(nlohmann::json& j, const CatalogFilters& value);

struct SearchQuery {
    std::string text{};
    SearchFilters filters{};
    SearchSort sort{};
    std::optional<std::string> cursor{};
    std::uint32_t limit{};
};

void from_json(const nlohmann::json& j, SearchQuery& out);
void to_json(nlohmann::json& j, const SearchQuery& value);

struct SearchPage {
    std::vector<PackageSummary> packages{};
    std::optional<std::string> next_cursor{};
    bool stale{};
};

void from_json(const nlohmann::json& j, SearchPage& out);
void to_json(nlohmann::json& j, const SearchPage& value);

struct VariantOption {
    PackageRef package{};
    VariantKind kind{};
    bool development{};
    bool recommended{};
};

void from_json(const nlohmann::json& j, VariantOption& out);
void to_json(nlohmann::json& j, const VariantOption& value);

struct VariantChoices {
    std::vector<VariantOption> options{};
    bool urls_differ{};
};

void from_json(const nlohmann::json& j, VariantChoices& out);
void to_json(nlohmann::json& j, const VariantChoices& value);

struct WikiText {
    std::string s{};
    bool emphasis{};
    bool strong{};
    bool code{};
};

void from_json(const nlohmann::json& j, WikiText& out);
void to_json(nlohmann::json& j, const WikiText& value);

struct WikiLink {
    std::string label{};
    std::string href{};
};

void from_json(const nlohmann::json& j, WikiLink& out);
void to_json(nlohmann::json& j, const WikiLink& value);

struct WikiHeading {
    std::uint32_t level{};
    std::string text{};
};

void from_json(const nlohmann::json& j, WikiHeading& out);
void to_json(nlohmann::json& j, const WikiHeading& value);

struct Inline {
    std::variant<WikiText, WikiLink> value;
};

struct WikiParagraph {
    std::vector<Inline> inlines{};
};

void from_json(const nlohmann::json& j, WikiParagraph& out);
void to_json(nlohmann::json& j, const WikiParagraph& value);

struct WikiList {
    bool ordered{};
    std::vector<std::vector<Inline>> items{};
};

void from_json(const nlohmann::json& j, WikiList& out);
void to_json(nlohmann::json& j, const WikiList& value);

struct WikiCode {
    std::string text{};
};

void from_json(const nlohmann::json& j, WikiCode& out);
void to_json(nlohmann::json& j, const WikiCode& value);

struct WikiNote {
    std::vector<Inline> inlines{};
};

void from_json(const nlohmann::json& j, WikiNote& out);
void to_json(nlohmann::json& j, const WikiNote& value);

struct WikiWarning {
    std::vector<Inline> inlines{};
};

void from_json(const nlohmann::json& j, WikiWarning& out);
void to_json(nlohmann::json& j, const WikiWarning& value);

struct WikiBlock {
    std::variant<WikiHeading, WikiParagraph, WikiList, WikiCode, WikiNote, WikiWarning> value;
};

struct WikiSection {
    std::uint32_t level{};
    std::string title{};
};

void from_json(const nlohmann::json& j, WikiSection& out);
void to_json(nlohmann::json& j, const WikiSection& value);

struct WikiDocument {
    std::string title{};
    std::string source_url{};
    std::string fetched_at{};
    bool stale{};
    std::string attribution_key{};
    std::vector<WikiBlock> blocks{};
    std::vector<WikiSection> sections{};
};

void from_json(const nlohmann::json& j, WikiDocument& out);
void to_json(nlohmann::json& j, const WikiDocument& value);

struct NewsItem {
    std::string title{};
    std::string url{};
    std::string published{};
    bool requires_manual_intervention{};
    bool acknowledged{};
};

void from_json(const nlohmann::json& j, NewsItem& out);
void to_json(nlohmann::json& j, const NewsItem& value);

struct RestartAdviceDTO {
    PackageRef package{};
    RestartLevel level{};
    std::string reason_key{};
    std::optional<std::string> detail{};
};

void from_json(const nlohmann::json& j, RestartAdviceDTO& out);
void to_json(nlohmann::json& j, const RestartAdviceDTO& value);

struct UpdateItem {
    PackageRef package{};
    std::string from_version{};
    std::string to_version{};
    SourceKind source{};
    RestartAdviceDTO restart{};
};

void from_json(const nlohmann::json& j, UpdateItem& out);
void to_json(nlohmann::json& j, const UpdateItem& value);

struct UpdateReport {
    std::string report_id{};
    std::string checked_at{};
    std::vector<UpdateItem> official{};
    std::vector<UpdateItem> community{};
    std::vector<UpdateItem> manual{};
    std::vector<NewsItem> news{};
    RestartLevel overall_restart{};
};

void from_json(const nlohmann::json& j, UpdateReport& out);
void to_json(nlohmann::json& j, const UpdateReport& value);

struct Replacement {
    PackageRef old{};
    PackageRef new_{};
};

void from_json(const nlohmann::json& j, Replacement& out);
void to_json(nlohmann::json& j, const Replacement& value);

struct Blocker {
    std::string code{};
    std::string message_key{};
    nlohmann::json args{};
};

void from_json(const nlohmann::json& j, Blocker& out);
void to_json(nlohmann::json& j, const Blocker& value);

struct TransactionPlan {
    std::string plan_id{};
    std::uint64_t revision{};
    std::string approval_token{};
    TransactionKind kind{};
    std::vector<PackageRef> installs{};
    std::vector<PackageRef> upgrades{};
    std::vector<PackageRef> removals{};
    std::vector<Replacement> replacements{};
    std::vector<std::string> conflicts{};
    std::optional<std::uint64_t> download_bytes{};
    std::optional<std::uint64_t> install_delta_bytes{};
    std::vector<RestartAdviceDTO> restart{};
    bool requires_build_review{};
    std::vector<PackageRef> build_review_refs{};
    std::vector<PackageRef> protected_blockers{};
    std::vector<Blocker> blockers{};
};

void from_json(const nlohmann::json& j, TransactionPlan& out);
void to_json(nlohmann::json& j, const TransactionPlan& value);

struct Progress {
    JobPhase phase{};
    std::optional<std::uint64_t> current{};
    std::optional<std::uint64_t> total{};
    ProgressUnit unit{};
};

void from_json(const nlohmann::json& j, Progress& out);
void to_json(nlohmann::json& j, const Progress& value);

struct ProviderQuestion {
    std::vector<PackageRef> options{};
};

void from_json(const nlohmann::json& j, ProviderQuestion& out);
void to_json(nlohmann::json& j, const ProviderQuestion& value);

struct BuildReviewQuestion {
    std::vector<PackageRef> refs{};
    std::optional<std::string> pkgbuild{};
};

void from_json(const nlohmann::json& j, BuildReviewQuestion& out);
void to_json(nlohmann::json& j, const BuildReviewQuestion& value);

struct ConflictQuestion {
    PackageRef a{};
    PackageRef b{};
};

void from_json(const nlohmann::json& j, ConflictQuestion& out);
void to_json(nlohmann::json& j, const ConflictQuestion& value);

struct NewsAckQuestion {
    std::vector<NewsItem> items{};
};

void from_json(const nlohmann::json& j, NewsAckQuestion& out);
void to_json(nlohmann::json& j, const NewsAckQuestion& value);

struct GitRedirectQuestion {
    std::string from_host{};
    std::string to_host{};
};

void from_json(const nlohmann::json& j, GitRedirectQuestion& out);
void to_json(nlohmann::json& j, const GitRedirectQuestion& value);

struct QuestionChoice {
    std::variant<VariantChoices, ProviderQuestion, BuildReviewQuestion, ConflictQuestion, NewsAckQuestion, GitRedirectQuestion> value;
};

struct Question {
    std::string question_id{};
    std::string token{};
    std::string expires_at{};
    QuestionChoice choice{};
};

void from_json(const nlohmann::json& j, Question& out);
void to_json(nlohmann::json& j, const Question& value);

struct Settings {
    UiPreference ui{};
    HelperPreference helper{};
    CheckFrequency check_frequency{};
    bool notifications{};
    bool autostart{};
    bool review_builds{};
    bool show_log{};
    Theme theme{};
};

void from_json(const nlohmann::json& j, Settings& out);
void to_json(nlohmann::json& j, const Settings& value);

struct SettingsPatch {
    std::optional<UiPreference> ui{};
    std::optional<HelperPreference> helper{};
    std::optional<CheckFrequency> check_frequency{};
    std::optional<bool> notifications{};
    std::optional<bool> autostart{};
    std::optional<bool> review_builds{};
    std::optional<bool> show_log{};
    std::optional<Theme> theme{};
};

void from_json(const nlohmann::json& j, SettingsPatch& out);
void to_json(nlohmann::json& j, const SettingsPatch& value);

struct ManualPreview {
    std::string preview_id{};
    std::string host{};
    std::string owner_repo{};
    std::string commit{};
    std::string script_path{};
    std::string script_text{};
    std::vector<ReceiptScope> scope_options{};
    std::vector<std::string> external_refs{};
    bool staging_supported{};
};

void from_json(const nlohmann::json& j, ManualPreview& out);
void to_json(nlohmann::json& j, const ManualPreview& value);

struct ManualReceipt {
    std::string receipt_id{};
    std::string name{};
    ReceiptScope scope{};
    std::string commit{};
    std::string installed_at{};
    std::vector<std::string> launcher_ids{};
    bool removable{};
};

void from_json(const nlohmann::json& j, ManualReceipt& out);
void to_json(nlohmann::json& j, const ManualReceipt& value);

struct LauncherEntry {
    std::string desktop_id{};
    std::string name{};
    std::string exec_display{};
    std::string path{};
    LauncherOwner owner{};
};

void from_json(const nlohmann::json& j, LauncherEntry& out);
void to_json(nlohmann::json& j, const LauncherEntry& value);

struct Capabilities {
    HelperPreference helper{};
    std::string helper_version{};
    Elevation elevation{};
    Init init{};
    std::string distro_id{};
    std::vector<std::string> repos{};
};

void from_json(const nlohmann::json& j, Capabilities& out);
void to_json(nlohmann::json& j, const Capabilities& value);

struct SystemStatus {
    Connectivity connectivity{};
    std::optional<std::uint64_t> catalog_age{};
    std::optional<std::string> active_job{};
    Elevation elevation_method{};
};

void from_json(const nlohmann::json& j, SystemStatus& out);
void to_json(nlohmann::json& j, const SystemStatus& value);

struct HelloParams {
    std::uint32_t apiVersion{};
};

void from_json(const nlohmann::json& j, HelloParams& out);
void to_json(nlohmann::json& j, const HelloParams& value);

struct HelloResult {
    std::string serverVersion{};
    std::uint32_t apiVersion{};
    HelperPreference helper{};
    Capabilities capabilities{};
    std::vector<UiPreference> uiBackends{};
};

void from_json(const nlohmann::json& j, HelloResult& out);
void to_json(nlohmann::json& j, const HelloResult& value);

struct StatusParams {
};

void from_json(const nlohmann::json& j, StatusParams& out);
void to_json(nlohmann::json& j, const StatusParams& value);

struct CatalogSourceStatus {
    std::string id{};
    std::string status{};
    std::optional<std::string> reasonCode{};
    std::uint64_t packageCount{};
    std::uint64_t skippedCount{};
};

void from_json(const nlohmann::json& j, CatalogSourceStatus& out);
void to_json(nlohmann::json& j, const CatalogSourceStatus& value);

struct StatusResult {
    Connectivity connectivity{};
    std::optional<std::uint64_t> catalogAge{};
    std::optional<std::string> activeJob{};
    Elevation elevationMethod{};
    std::string catalogState{"missing"};
    std::optional<std::uint64_t> catalogAgeSeconds{};
    std::vector<CatalogSourceStatus> sources{};
};

void from_json(const nlohmann::json& j, StatusResult& out);
void to_json(nlohmann::json& j, const StatusResult& value);

struct SearchParams {
    std::string text{};
    CatalogFilters filters{};
    std::optional<std::string> cursor{};
    std::uint32_t limit{};
    SearchSort sort{"relevance"};
    std::optional<std::string> category{};
};

void from_json(const nlohmann::json& j, SearchParams& out);
void to_json(nlohmann::json& j, const SearchParams& value);

struct SearchResult {
    std::vector<PackageSummary> packages{};
    std::optional<std::string> nextCursor{};
    bool stale{};
    std::optional<std::string> catalogState{};
};

void from_json(const nlohmann::json& j, SearchResult& out);
void to_json(nlohmann::json& j, const SearchResult& value);

struct GetParams {
    std::string packageId{};
};

void from_json(const nlohmann::json& j, GetParams& out);
void to_json(nlohmann::json& j, const GetParams& value);

struct GetResult {
    PackageDetails details{};
    std::string installedState{};
    SourceKind source{};
    std::optional<AppMetadata> appMetadata{};
};

void from_json(const nlohmann::json& j, GetResult& out);
void to_json(nlohmann::json& j, const GetResult& value);

struct VariantsParams {
    std::string packageId{};
};

void from_json(const nlohmann::json& j, VariantsParams& out);
void to_json(nlohmann::json& j, const VariantsParams& value);

struct VariantsResult {
    VariantChoices choices{};
    bool questionRequired{};
};

void from_json(const nlohmann::json& j, VariantsResult& out);
void to_json(nlohmann::json& j, const VariantsResult& value);

struct FacetsParams {};

void from_json(const nlohmann::json& j, FacetsParams& out);
void to_json(nlohmann::json& j, const FacetsParams& value);

struct FacetCount {
    std::string name{};
    std::uint64_t packageCount{};
};

void from_json(const nlohmann::json& j, FacetCount& out);
void to_json(nlohmann::json& j, const FacetCount& value);

struct CategoryCount {
    std::string id{};
    std::uint64_t packageCount{};
};

void from_json(const nlohmann::json& j, CategoryCount& out);
void to_json(nlohmann::json& j, const CategoryCount& value);

struct FacetsResult {
    std::vector<FacetCount> repos{};
    std::vector<FacetCount> groups{};
    std::vector<CategoryCount> categories{};
};

void from_json(const nlohmann::json& j, FacetsResult& out);
void to_json(nlohmann::json& j, const FacetsResult& value);

struct BuildFilesParams {
    std::string packageId{};
};

void from_json(const nlohmann::json& j, BuildFilesParams& out);
void to_json(nlohmann::json& j, const BuildFilesParams& value);

struct BuildFile {
    std::string name{};
    std::string text{};
};

void from_json(const nlohmann::json& j, BuildFile& out);
void to_json(nlohmann::json& j, const BuildFile& value);

struct BuildFilesResult {
    std::string packageBase{};
    std::vector<BuildFile> files{};
    bool truncated{};
};

void from_json(const nlohmann::json& j, BuildFilesResult& out);
void to_json(nlohmann::json& j, const BuildFilesResult& value);

struct RefreshParams {
    std::vector<SourceKind> sources{};
};

void from_json(const nlohmann::json& j, RefreshParams& out);
void to_json(nlohmann::json& j, const RefreshParams& value);

struct JobIdResult {
    std::string jobId{};
};

void from_json(const nlohmann::json& j, JobIdResult& out);
void to_json(nlohmann::json& j, const JobIdResult& value);

struct WikiGetParams {
    std::string packageId{};
    bool allowRefresh{};
};

void from_json(const nlohmann::json& j, WikiGetParams& out);
void to_json(nlohmann::json& j, const WikiGetParams& value);

struct WikiGetResult {
    WikiDocument document{};
    std::string sourceUrl{};
    std::string attribution{};
    std::string fetchedAt{};
    bool stale{};
};

void from_json(const nlohmann::json& j, WikiGetResult& out);
void to_json(nlohmann::json& j, const WikiGetResult& value);

struct NewsListParams {
    std::optional<std::string> cursor{};
    std::uint32_t limit{};
};

void from_json(const nlohmann::json& j, NewsListParams& out);
void to_json(nlohmann::json& j, const NewsListParams& value);

struct NewsListResult {
    std::vector<NewsItem> items{};
    std::optional<std::string> nextCursor{};
    bool stale{};
};

void from_json(const nlohmann::json& j, NewsListResult& out);
void to_json(nlohmann::json& j, const NewsListResult& value);

struct UpdatesCheckParams {
    bool refresh{};
};

void from_json(const nlohmann::json& j, UpdatesCheckParams& out);
void to_json(nlohmann::json& j, const UpdatesCheckParams& value);

struct UpdatesOptionsParams {
    std::string reportId{};
};

void from_json(const nlohmann::json& j, UpdatesOptionsParams& out);
void to_json(nlohmann::json& j, const UpdatesOptionsParams& value);

struct UpdatesOptionsResult {
    std::string officialAction{};
    std::vector<std::string> communityIndividualIds{};
    std::vector<std::string> manualIndividualIds{};
};

void from_json(const nlohmann::json& j, UpdatesOptionsResult& out);
void to_json(nlohmann::json& j, const UpdatesOptionsResult& value);

struct PlanSelection {
    SelectionKind kind{};
    std::optional<std::string> id{};
};

void from_json(const nlohmann::json& j, PlanSelection& out);
void to_json(nlohmann::json& j, const PlanSelection& value);

struct UpdatesPlanParams {
    std::string reportId{};
    PlanSelection selection{};
    std::optional<std::string> alsoInstallPackageId{};
};

void from_json(const nlohmann::json& j, UpdatesPlanParams& out);
void to_json(nlohmann::json& j, const UpdatesPlanParams& value);

struct UpdatesPlanResult {
    std::string planId{};
    std::vector<TransactionPlan> transactions{};
    std::vector<RestartAdviceDTO> restartAdvice{};
    bool requiresApproval{};
    std::vector<Blocker> blockers{};
};

void from_json(const nlohmann::json& j, UpdatesPlanResult& out);
void to_json(nlohmann::json& j, const UpdatesPlanResult& value);

struct PlanParams {
    std::string kind{};
    std::optional<std::string> packageId{};
    std::optional<std::string> variantId{};
    std::optional<std::string> previewId{};
    std::optional<std::string> receiptId{};
    std::optional<ReceiptScope> scope{};
    std::optional<HelperPreference> helper{};
};

void from_json(const nlohmann::json& j, PlanParams& out);
void to_json(nlohmann::json& j, const PlanParams& value);

struct InstallParams {
    std::string planId{};
    std::string approvalToken{};
};

void from_json(const nlohmann::json& j, InstallParams& out);
void to_json(nlohmann::json& j, const InstallParams& value);

struct RemoveParams : InstallParams {};
struct UpgradeSystemParams : InstallParams {};
struct UpgradeCommunityParams : InstallParams {};

void from_json(const nlohmann::json& j, RemoveParams& out);
void to_json(nlohmann::json& j, const RemoveParams& value);
void from_json(const nlohmann::json& j, UpgradeSystemParams& out);
void to_json(nlohmann::json& j, const UpgradeSystemParams& value);
void from_json(const nlohmann::json& j, UpgradeCommunityParams& out);
void to_json(nlohmann::json& j, const UpgradeCommunityParams& value);


void from_json(const nlohmann::json& j, UpgradeCommunityParams& out);
void to_json(nlohmann::json& j, const UpgradeCommunityParams& value);

struct JobEvent {
    std::uint64_t sequence{};
    std::string kind{};
    nlohmann::json payload{};
};

void from_json(const nlohmann::json& j, JobEvent& out);
void to_json(nlohmann::json& j, const JobEvent& value);

struct JobsGetParams {
    std::string jobId{};
    std::optional<std::string> eventsCursor{};
};

void from_json(const nlohmann::json& j, JobsGetParams& out);
void to_json(nlohmann::json& j, const JobsGetParams& value);

struct JobsGetResult {
    JobState state{};
    std::optional<TransactionPlan> plan{};
    std::optional<Progress> progress{};
    std::optional<nlohmann::json> result{};
    std::optional<nlohmann::json> error{};
    std::vector<JobEvent> events{};
    std::optional<std::string> nextCursor{};
};

void from_json(const nlohmann::json& j, JobsGetResult& out);
void to_json(nlohmann::json& j, const JobsGetResult& value);

struct JobsListParams {
    std::optional<std::string> cursor{};
    std::uint32_t limit{};
};

void from_json(const nlohmann::json& j, JobsListParams& out);
void to_json(nlohmann::json& j, const JobsListParams& value);

struct JobSummary {
    std::string jobId{};
    JobState state{};
};

void from_json(const nlohmann::json& j, JobSummary& out);
void to_json(nlohmann::json& j, const JobSummary& value);

struct JobsListResult {
    std::vector<JobSummary> jobs{};
    std::optional<std::string> nextCursor{};
};

void from_json(const nlohmann::json& j, JobsListResult& out);
void to_json(nlohmann::json& j, const JobsListResult& value);

struct JobsCancelParams {
    std::string jobId{};
};

void from_json(const nlohmann::json& j, JobsCancelParams& out);
void to_json(nlohmann::json& j, const JobsCancelParams& value);

struct JobsCancelResult {
    bool accepted{};
    std::optional<std::string> reason{};
};

void from_json(const nlohmann::json& j, JobsCancelResult& out);
void to_json(nlohmann::json& j, const JobsCancelResult& value);

struct JobsAnswerParams {
    std::string jobId{};
    std::string questionId{};
    std::string choiceId{};
    std::string questionToken{};
};

void from_json(const nlohmann::json& j, JobsAnswerParams& out);
void to_json(nlohmann::json& j, const JobsAnswerParams& value);

struct AcceptedResult {
    bool accepted{};
};

void from_json(const nlohmann::json& j, AcceptedResult& out);
void to_json(nlohmann::json& j, const AcceptedResult& value);

struct ManualPreviewParams {
    std::string httpsUrl{};
    std::string scriptPath{};
    ReceiptScope scope{};
};

void from_json(const nlohmann::json& j, ManualPreviewParams& out);
void to_json(nlohmann::json& j, const ManualPreviewParams& value);

struct ManualInstallParams : InstallParams {};
struct ManualUpgradeParams : InstallParams {};
void from_json(const nlohmann::json& j, ManualInstallParams& out);
void to_json(nlohmann::json& j, const ManualInstallParams& value);
void from_json(const nlohmann::json& j, ManualUpgradeParams& out);
void to_json(nlohmann::json& j, const ManualUpgradeParams& value);

struct ManualListParams {
};

void from_json(const nlohmann::json& j, ManualListParams& out);
void to_json(nlohmann::json& j, const ManualListParams& value);

struct ManualListResult {
    std::vector<ManualReceipt> receipts{};
};

void from_json(const nlohmann::json& j, ManualListResult& out);
void to_json(nlohmann::json& j, const ManualListResult& value);

struct ManualRemoveParams : InstallParams {};
struct SetupHelperParams : InstallParams {};
void from_json(const nlohmann::json& j, ManualRemoveParams& out);
void to_json(nlohmann::json& j, const ManualRemoveParams& value);
void from_json(const nlohmann::json& j, SetupHelperParams& out);
void to_json(nlohmann::json& j, const SetupHelperParams& value);

struct JobsDequeueParams {
    std::string jobId{};
};

void from_json(const nlohmann::json& j, JobsDequeueParams& out);
void to_json(nlohmann::json& j, const JobsDequeueParams& value);

struct JobsDequeueResult {
    bool removed{};
};

void from_json(const nlohmann::json& j, JobsDequeueResult& out);
void to_json(nlohmann::json& j, const JobsDequeueResult& value);

struct UnmanagedListParams {
};

void from_json(const nlohmann::json& j, UnmanagedListParams& out);
void to_json(nlohmann::json& j, const UnmanagedListParams& value);

struct UnmanagedListResult {
    std::vector<LauncherEntry> entries{};
    std::string scanTime{};
};

void from_json(const nlohmann::json& j, UnmanagedListResult& out);
void to_json(nlohmann::json& j, const UnmanagedListResult& value);

struct SettingsGetParams {
};

void from_json(const nlohmann::json& j, SettingsGetParams& out);
void to_json(nlohmann::json& j, const SettingsGetParams& value);

struct SettingsGetResult {
    UiPreference uiPreference{};
    bool notifications{};
    CheckFrequency refreshPolicy{};
    Elevation elevationPreference{};
};

void from_json(const nlohmann::json& j, SettingsGetResult& out);
void to_json(nlohmann::json& j, const SettingsGetResult& value);

struct SettingsSetParams {
    SettingsPatch changedFields{};
};

void from_json(const nlohmann::json& j, SettingsSetParams& out);
void to_json(nlohmann::json& j, const SettingsSetParams& value);

struct SettingsSetResult {
    Settings settings{};
};

void from_json(const nlohmann::json& j, SettingsSetResult& out);
void to_json(nlohmann::json& j, const SettingsSetResult& value);

struct ClearCacheParams {};
void from_json(const nlohmann::json& j, ClearCacheParams& out);
void to_json(nlohmann::json& j, const ClearCacheParams& value);

struct ClearCacheResult {
    std::uint64_t freedBytes{};
};
void from_json(const nlohmann::json& j, ClearCacheResult& out);
void to_json(nlohmann::json& j, const ClearCacheResult& value);

struct RestartStateParams {};
struct RestartAcknowledgeParams {};
void from_json(const nlohmann::json& j, RestartStateParams& out);
void to_json(nlohmann::json& j, const RestartStateParams& value);
void from_json(const nlohmann::json& j, RestartAcknowledgeParams& out);
void to_json(nlohmann::json& j, const RestartAcknowledgeParams& value);

struct RestartStateResult {
    RestartLevel level{"none"};
    std::vector<std::string> reason_keys{};
    std::optional<std::string> since{};
};
void from_json(const nlohmann::json& j, RestartStateResult& out);
void to_json(nlohmann::json& j, const RestartStateResult& value);

struct EmptyResult {};
void from_json(const nlohmann::json& j, EmptyResult& out);
void to_json(nlohmann::json& j, const EmptyResult& value);

struct HelperStatus {
    bool installed{};
    std::optional<std::string> version{};
};
void from_json(const nlohmann::json& j, HelperStatus& out);
void to_json(nlohmann::json& j, const HelperStatus& value);

struct SetupStatusParams {};
void from_json(const nlohmann::json& j, SetupStatusParams& out);
void to_json(nlohmann::json& j, const SetupStatusParams& value);

struct SetupStatusResult {
    HelperStatus paru{};
    HelperStatus yay{};
    HelperPreference chosen{"auto"};
    Elevation elevation{"unavailable"};
    std::vector<std::string> notes{};
};
void from_json(const nlohmann::json& j, SetupStatusResult& out);
void to_json(nlohmann::json& j, const SetupStatusResult& value);

struct NotifierCheckParams {
};

void from_json(const nlohmann::json& j, NotifierCheckParams& out);
void to_json(nlohmann::json& j, const NotifierCheckParams& value);

struct NotifierCheckResult {
    std::uint64_t updateCount{};
    bool notificationSent{};
    std::optional<std::string> reason{};
};

void from_json(const nlohmann::json& j, NotifierCheckResult& out);
void to_json(nlohmann::json& j, const NotifierCheckResult& value);

struct JobProgressNotification {
    std::string jobId{};
    std::uint64_t sequence{};
    Progress progress{};
};

void from_json(const nlohmann::json& j, JobProgressNotification& out);
void to_json(nlohmann::json& j, const JobProgressNotification& value);

struct JobLogNotification {
    std::string jobId{};
    std::uint64_t sequence{};
    std::string line{};
};

void from_json(const nlohmann::json& j, JobLogNotification& out);
void to_json(nlohmann::json& j, const JobLogNotification& value);

struct JobQuestionNotification {
    std::string jobId{};
    std::uint64_t sequence{};
    Question question{};
};

void from_json(const nlohmann::json& j, JobQuestionNotification& out);
void to_json(nlohmann::json& j, const JobQuestionNotification& value);

struct JobAuthStateNotification {
    std::string jobId{};
    std::uint64_t sequence{};
    AuthState state{};
};

void from_json(const nlohmann::json& j, JobAuthStateNotification& out);
void to_json(nlohmann::json& j, const JobAuthStateNotification& value);

struct JobStateNotification {
    std::string jobId{};
    std::uint64_t sequence{};
    JobState state{};
};

void from_json(const nlohmann::json& j, JobStateNotification& out);
void to_json(nlohmann::json& j, const JobStateNotification& value);

struct CatalogChangedNotification {
    std::uint64_t revision{};
};

void from_json(const nlohmann::json& j, CatalogChangedNotification& out);
void to_json(nlohmann::json& j, const CatalogChangedNotification& value);

struct UpdatesAvailableNotification {
    std::string reportId{};
    std::uint64_t count{};
};

void from_json(const nlohmann::json& j, UpdatesAvailableNotification& out);
void to_json(nlohmann::json& j, const UpdatesAvailableNotification& value);

}

#endif
