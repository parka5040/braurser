#include <braurser/presenter/copy.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace braurser::presenter {
TEST(Copy, PluralsNumbersAndPlainText) {
    Copy copy("en", MissingCopy::Throw);
    EXPECT_EQ(copy.text("search.results", {{"count", std::int64_t{1}}}), "1 result");
    EXPECT_EQ(copy.text("search.results", {{"count", std::int64_t{1200}}}), "1,200 results");
    EXPECT_EQ(copy.text("confirm.remove_helpers", {{"count", std::int64_t{2}}}), "Remove 2 unused helpers");
    EXPECT_EQ(copy.text("confirm.install_title", {{"app", std::string("<b>Paint</b>")}}), "Install \xE2\x81\xA8<b>Paint</b>\xE2\x81\xA9?");
    EXPECT_EQ(copy.size({}), "Not available");
    EXPECT_NE(copy.size(0), "Not available");
    EXPECT_EQ(copy.number(1200), "1,200");
    EXPECT_NE(copy.date("2026-09-25T12:00:00Z"), "Not available");
    EXPECT_EQ(copy.date("unknown"), "Not available");
    EXPECT_EQ(copy.date("2026-99-25T12:00:00Z"), "Not available");
}
TEST(Copy, MissingKeysAndArgumentsAreExplicit) {
    Copy strict("en", MissingCopy::Throw);
    EXPECT_THROW(strict.text("missing"), std::invalid_argument);
    EXPECT_THROW(strict.text("confirm.install_title"), std::invalid_argument);
    Copy visible("en", MissingCopy::Visible);
    EXPECT_EQ(visible.text("missing"), "⟨missing⟩");
    EXPECT_EQ(visible.text("confirm.install_title"), "⟨confirm.install_title⟩");
    EXPECT_THROW((Copy({{"broken", "{count,plural,one {one}}"}}, "en", MissingCopy::Throw)), std::invalid_argument);
}
TEST(Copy, DateAndNumberFormatsUseIcu) {
    Copy copy({{"date", "{when,date,short}"}, {"number", "{value,number}"}}, "en_US", MissingCopy::Throw);
    EXPECT_FALSE(copy.text("date", {{"when", std::chrono::system_clock::time_point{}}}).empty());
    EXPECT_EQ(copy.text("number", {{"value", 1234.5}}), "1,234.5");
}
TEST(Copy, CatalogHasOverridesAndNoPartialUpdateFlow) {
    Copy copy;
    EXPECT_GT(copy.keys().size(), 450U);
    for (const auto* key : {"updates.item_update", "updates.system_first", "confirm.update_and_install", "variant.different_sites", "variant.stable", "variant.stable_body", "variant.wayland", "variant.wayland_body"}) {
        EXPECT_TRUE(copy.contains(key)) << key;
    }
    for (const auto& key : copy.keys()) {
        EXPECT_FALSE(key.starts_with("updates.partial_")) << key;
        EXPECT_FALSE(key.starts_with("updates.postpone")) << key;
    }
    for (const auto* key : {"updates.will_postpone", "updates.must_together", "updates.cannot_postpone"}) { EXPECT_FALSE(copy.contains(key)); }
}
TEST(Copy, EveryAuthoredDeckKeyIsPresent) {
    const std::string deck = R"(
brand window.title nav.discover nav.search nav.updates nav.installed nav.settings nav.activity nav.menu nav.back nav.back_to
 action.cancel action.close action.done action.continue action.next action.install action.remove action.open action.retry action.dismiss action.details action.view_details action.copy action.copied action.check_again action.review action.previous action.clear_search action.open_external
 state.loading state.unavailable state.installed state.not_installed state.system_component state.pending state.saved
 discover.title discover.categories discover.more_categories discover.available discover.git
 search.placeholder search.in_category search.installed_placeholder search.results search.apps search.sort search.sort_name search.sort_relevance search.empty_title search.empty_body search.start_title search.start_body
 filter.title filter.count filter.clear filter.apply filter.any filter.all filter.installation filter.installed filter.not_installed filter.added_as filter.explicit filter.dependency filter.maintenance filter.orphans filter.orphans_help filter.updates filter.source_type filter.official filter.community filter.foreign_note filter.repositories filter.repository_search filter.groups filter.group_search filter.apps_only filter.apps_help filter.requires_installed filter.remove filter.repository_chip filter.group_chip filter.category_chip filter.snapshot
 source.official source.community source.git source.outside source.foreign source.official_help source.community_help source.git_help source.outside_help source.foreign_help
 detail.build_truncated detail.about detail.version detail.installed_version detail.available_version detail.source detail.package detail.download_size detail.installed_size detail.screenshot detail.screenshot_missing detail.no_description detail.dependencies detail.dependencies_body detail.no_dependencies detail.optional detail.technical detail.build_review detail.build_file detail.build_identity detail.build_mutable detail.build_unavailable
 community.title community.about community.notice community.context community.maintainer community.no_maintainer community.updated community.votes community.popularity community.popularity_help community.outdated community.not_flagged community.orphan_notice community.outdated_notice
 wiki.title wiki.sections wiki.open wiki.search wiki.attribution wiki.license wiki.empty_title wiki.empty_body wiki.loading wiki.error wiki.cached wiki.offline_uncached wiki.note wiki.warning wiki.code_copy wiki.distro_note
 variant.title variant.body variant.recommended variant.official variant.official_body variant.binary variant.binary_body variant.source variant.source_body variant.git variant.git_body variant.appimage variant.appimage_body variant.nightly variant.nightly_body variant.beta variant.beta_body variant.reason_official variant.reason_binary variant.reason_source variant.reason_appimage variant.no_stable variant.choose variant.different_sites variant.stable variant.stable_body variant.wayland variant.wayland_body
 confirm.install_title confirm.remove_title confirm.also_installs confirm.also_updates confirm.also_removes confirm.replaces confirm.replacement confirm.helpers confirm.download confirm.storage_add confirm.storage_free confirm.storage_unknown confirm.exact confirm.build_ack confirm.remove_body confirm.remove_component_body confirm.required_by confirm.remove_dependents_ack confirm.remove_helpers confirm.keep_files confirm.files_unknown confirm.protected confirm.plan_changed confirm.add_queue confirm.queue_note confirm.removal_unavailable confirm.git_untracked_removal confirm.update_and_install
 activity.installing activity.removing activity.updating activity.downloading activity.building activity.installing_phase activity.removing_phase activity.finishing activity.finishing_busy activity.download_amount activity.elapsed activity.log activity.copy_log activity.log_warning activity.waiting activity.remove_queue activity.recent activity.empty_title activity.empty_body activity.ready activity.removed activity.updated activity.install_failed activity.remove_failed activity.update_failed activity.no_changes activity.partial_changes activity.changes_unknown activity.cancel_after activity.cancel_pending activity.cannot_cancel activity.canceled activity.needs_attention activity.auth_wait activity.refresh_state activity.close_title activity.close_body activity.keep_background activity.keep_window
 updates.last_checked updates.never_checked updates.checking updates.count updates.download updates.all updates.current_title updates.current_body updates.restart updates.logout updates.apps updates.none updates.unknown updates.group_count updates.version_change updates.impact_reason updates.unknown_note updates.news_title updates.news_read updates.news_ack updates.news_block updates.news_unavailable updates.news_unavailable_ack updates.reason_dependency updates.reason_group updates.reason_system updates.reason_backend updates.reason_unknown updates.item_update updates.system_first
 restart.title restart.body restart.later restart.now restart.confirm_title restart.confirm_body restart.blocked restart.failed restart.logout_title restart.logout_body restart.apps_title restart.apps_body restart.unknown_title restart.unknown_body
 notification.updates_title notification.updates_body notification.updates_action notification.restart_title notification.restart_body notification.open notification.complete_title notification.complete_body notification.attention_title notification.attention_body
 git.step git.choose git.url_label git.url_placeholder git.url_help git.url_invalid git.credentials_rejected git.fetching git.fetch_failed git.redirect_title git.redirect_body git.review git.repository git.commit git.script git.select_script git.other_files git.risk git.external_risk git.review_ack git.no_script git.unsupported git.script_too_large git.changed git.scope git.user git.user_body git.system git.system_body git.system_risk git.scope_unavailable git.install_user git.install_system git.install_step git.launcher_result git.no_launcher git.installed_user git.installed_system git.view_installed git.cleanup_done git.cleanup_unknown git.removal_scope git.scope_user_description git.scope_system_description
 installed.apps installed.components installed.git installed.outside installed.view installed.outside_unmanaged installed.outside_notice installed.outside_help installed.explicit installed.dependency installed.empty_apps installed.empty_apps_body installed.empty_components installed.empty_components_body installed.empty_git installed.empty_git_body installed.empty_outside installed.empty_outside_body installed.open_failed installed.launcher_missing
 settings.community settings.helper settings.automatic settings.paru settings.yay settings.using_helper settings.helper_missing settings.helper_busy settings.updates settings.frequency settings.never settings.daily settings.weekly settings.notifications settings.autostart settings.autostart_help settings.on settings.off settings.storage settings.cache settings.clear_cache settings.cache_title settings.cache_body settings.cache_offline settings.cache_busy settings.cache_confirm settings.cache_done settings.cache_failed settings.advanced settings.review_builds settings.logs settings.appearance settings.theme settings.system settings.light settings.dark settings.save_failed
 auth.window_title auth.title auth.install auth.remove auth.update auth.setup auth.git auth.account auth.password auth.show auth.hide auth.caps auth.context auth.authenticate auth.checking auth.incorrect auth.denied auth.failed auth.timeout auth.canceled auth.native_wait auth.unavailable_title auth.unavailable_body auth.unavailable_action auth.fallback_notice
 first.yay first.paru first.title first.body first.official_ready first.helper_body first.setup first.browse
 setup.title setup.body setup.review setup.prerequisites setup.build_notice setup.ready setup.failed setup.unsupported setup.instructions setup.auth_help
 offline.title offline.cached offline.local_ready offline.empty offline.update offline.operation_block
 error.aur_title error.aur_body error.repositories_title error.repositories_body error.lock_title error.lock_body error.storage_title error.storage_body error.build_tools_title error.build_tools_body error.package_missing error.integrity_title error.integrity_body error.build_title error.build_body error.core_title error.core_body error.reconnect error.unsupported_title error.unsupported_body error.generic_title error.generic_body
 a11y.app_card a11y.package_row a11y.activity_count a11y.updates_count a11y.progress a11y.indeterminate a11y.reveal a11y.collapse a11y.password_visible a11y.password_hidden a11y.external_link a11y.shortcut_search a11y.shortcut_activity
)";
    Copy copy;
    std::istringstream keys(deck);
    std::set<std::string> expected;
    for (std::string key; keys >> key;) {
        EXPECT_TRUE(expected.insert(key).second) << key;
        EXPECT_TRUE(copy.contains(key)) << key;
    }
    for (const auto& key : copy.keys()) { EXPECT_TRUE(expected.contains(key) || key.starts_with("category.")) << key; }
}
TEST(Copy, LiteralPresenterCopyKeysExist) {
    Copy copy;
    const std::set<std::string> prefixes{"action", "state", "nav", "discover", "search", "filter", "detail", "community", "wiki", "variant", "confirm", "activity", "updates", "git", "installed", "settings", "auth", "first", "setup", "offline", "error", "a11y"};
    for (const auto* directory : {"src/presenter", "include/braurser/presenter"}) {
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(BRAURSER_SOURCE) / directory)) {
            if (entry.path().extension() != ".cpp" && entry.path().extension() != ".hpp") { continue; }
            std::ifstream input(entry.path());
            const std::string source((std::istreambuf_iterator<char>(input)), {});
            for (std::size_t position = 0; position < source.size();) {
                const auto begin = source.find('"', position);
                if (begin == std::string::npos) { break; }
                const auto end = source.find('"', begin + 1);
                if (end == std::string::npos) { break; }
                position = end + 1;
                const auto key = source.substr(begin + 1, end - begin - 1);
                const auto dot = key.find('.');
                if (dot == std::string::npos || dot + 1 == key.size() || !prefixes.contains(key.substr(0, dot)) ||
                    key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_.") != std::string::npos || key.ends_with('_')) { continue; }
                EXPECT_TRUE(copy.contains(key)) << entry.path() << ':' << key;
            }
        }
    }
}
}
