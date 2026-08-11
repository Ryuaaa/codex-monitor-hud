#include "module_state.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool Contains(const std::vector<codex_monitor::ModuleId>& modules,
              codex_monitor::ModuleId id) {
    return std::find(modules.begin(), modules.end(), id) != modules.end();
}

void TestNewInstallDefaultsUseRegistryPolicy() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    const codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    Expect(settings.currentPage == Page::kHome,
           "the homepage must be the first-run page");
    Expect(settings.homeOrder.size() == codex_monitor::kModuleCount,
           "every registered module must have a homepage order entry");

    const std::vector<ModuleId> home = codex_monitor::VisibleHomeModules(settings);
    Expect(home.size() == 9,
           "a new homepage must combine six core performance cards with quotas and current activity");
    Expect(Contains(home, ModuleId::kSystemDiagnosis),
           "the system diagnosis must be visible on a new homepage");
    Expect(codex_monitor::HomeNeedsPerformanceData(settings),
           "the default homepage must request performance data");
    Expect(Contains(home, ModuleId::kCpuTrend),
           "a new homepage must show the in-memory CPU trend");
    Expect(Contains(home, ModuleId::kCodexFiveHourQuota) &&
               Contains(home, ModuleId::kCodexWeeklyQuota),
           "a new homepage must show both independently selectable quota windows");
    Expect(Contains(home, ModuleId::kCodexTaskActivity),
           "a new homepage must show current task activity at a glance");
    Expect(codex_monitor::HomeNeedsCodexData(settings),
           "visible homepage quotas must request the existing low-frequency Codex refresh");
    Expect(codex_monitor::HomeNeedsCodexActivity(settings),
           "visible current activity must request its demand-gated local scan");
    Expect(!codex_monitor::HomeNeedsServiceStatus(settings),
           "the service-status module must not add homepage network work by default");

    const std::vector<ModuleId> computer =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kComputer);
    Expect(computer.size() == 7,
           "the computer page must add live I/O and CPU trend to the five original performance modules");
    Expect(Contains(computer, ModuleId::kSystemDiagnosis),
           "the system diagnosis must be visible on a new computer page");
    Expect(Contains(computer, ModuleId::kSystemIoThroughput),
           "live network and disk throughput must default on for the computer page");
    Expect(Contains(computer, ModuleId::kCpuTrend),
           "the ten-minute CPU trend must default on for the computer page");
    Expect(!Contains(home, ModuleId::kSystemIoThroughput),
           "live I/O must remain an opt-in homepage module");
    for (ModuleId id : computer) {
        const codex_monitor::ModuleDefinition& definition =
            codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)];
        Expect(definition.nativePage == Page::kComputer &&
                   definition.requiresPerformanceSampling &&
                   !definition.requiresCodexData &&
                   !definition.requiresServiceStatus,
               "every computer-page module must be a performance-only module");
    }

    const std::vector<ModuleId> codex =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kCodex);
    Expect(codex.size() == 8,
           "the Codex page defaults to quotas, forecast, subscription, cost estimate, current activity, recent history, and service status");
    Expect(Contains(codex, ModuleId::kCodexFiveHourQuota),
           "the five-hour quota must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kCodexWeeklyQuota),
           "the weekly quota must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kCodexQuotaForecast),
           "the quota trend forecast must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kCodexTaskActivity),
           "local current task activity must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kCodexRecentTasks),
           "recent task history must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kOpenAIServiceStatus),
           "OpenAI service status must be visible on a new Codex page");
    Expect(!Contains(codex, ModuleId::kCodexAccountTokenUsage),
           "account Token usage must default to off");
    Expect(Contains(codex, ModuleId::kCodexTokenCostEstimate),
           "Token and API-equivalent cost estimates must be visible on the Codex page but remain off the homepage");
    Expect(!Contains(home, ModuleId::kCodexTokenCostEstimate),
           "the heavier local cost scan must not run from the homepage by default");
    Expect(codex_monitor::VisibleModulesForNativePage(settings, Page::kHome).empty(),
           "Home is composed by its own order and must not be a native module page");
}

void TestOrderSanitizationAndMovement() {
    using codex_monitor::ModuleId;
    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    settings.homeOrder = {
        ModuleId::kSystemResources,
        ModuleId::kSystemResources,
        ModuleId::kTargetProcessTree,
    };
    settings.homeOrder = codex_monitor::SanitizeHomeOrder(settings.homeOrder);
    Expect(settings.homeOrder.size() == codex_monitor::kModuleCount,
           "duplicate order entries must be removed and missing modules restored");
    Expect(settings.homeOrder[0] == ModuleId::kSystemResources,
           "the first valid user order entry must be retained");
    Expect(codex_monitor::MoveHomeModule(settings, ModuleId::kTargetProcessTree, -1),
           "a module must move up when there is a preceding row");
    Expect(settings.homeOrder[0] == ModuleId::kTargetProcessTree,
           "moving up must swap with the preceding module");
    Expect(!codex_monitor::MoveHomeModule(settings, ModuleId::kTargetProcessTree, -1),
           "the first module must not move beyond the registry boundary");
}

void TestVersionElevenRoundTripAndIndependentQuotaSwitches() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    settings.currentPage = Page::kCodex;
    settings.alwaysOnTop = false;
    settings.windowLocked = true;
    settings.windowScale = 1.375;
    settings.opacityPercent = 82;
    settings.theme = codex_monitor::HudTheme::kPurple;
    settings.homeOrder = codex_monitor::SanitizeHomeOrder({
        ModuleId::kCodexWeeklyQuota,
        ModuleId::kTopMemoryProcesses,
        ModuleId::kCodexFiveHourQuota,
    });
    settings.homeVisible.fill(false);
    settings.nativePageVisible.fill(false);
    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kCodexFiveHourQuota)] = true;
    settings.nativePageVisible[
        codex_monitor::ModuleIndex(ModuleId::kCodexWeeklyQuota)] = true;
    settings.nativePageVisible[
        codex_monitor::ModuleIndex(ModuleId::kOpenAIServiceStatus)] = true;
    settings.weeklyQuotaAlert.enabled = true;
    settings.weeklyQuotaAlert.thresholdPercent = 25.0;
    settings.weeklyQuotaAlert.mode =
        codex_monitor::codex::WeeklyQuotaAlertMode::kNaturalDay;
    settings.windowPlacement = codex_monitor::WindowPlacement{-900, 120, 720, 640};

    const std::string serialized = codex_monitor::SerializeSettings(settings);
    Expect(serialized.find("version=11\n") == 0,
           "the settings whitelist must be serialized as version 11");

    const codex_monitor::SettingsState parsed = codex_monitor::ParseSettings(serialized);
    Expect(parsed.currentPage == Page::kCodex,
           "the current page must survive persistence");
    Expect(!parsed.alwaysOnTop, "the topmost preference must survive persistence");
    Expect(parsed.windowLocked, "the position and size lock must survive persistence");
    Expect(parsed.windowScale == 1.375,
           "the uniform window scale must survive persistence");
    Expect(parsed.opacityPercent == 82,
           "the selected whole-window opacity must survive persistence");
    Expect(parsed.theme == codex_monitor::HudTheme::kPurple,
           "the low-saturation theme must survive persistence");
    Expect(!parsed.legacyWindowSizeNeedsMigration,
           "a current uniform frame must not be treated as legacy shape data");
    Expect(parsed.homeOrder == settings.homeOrder,
           "the homepage module order must survive persistence");
    Expect(parsed.windowPlacement && parsed.windowPlacement->x == -900 &&
               parsed.windowPlacement->width == 720,
           "window position and size must survive persistence");
    Expect(parsed.weeklyQuotaAlert.enabled &&
               parsed.weeklyQuotaAlert.thresholdPercent == 25.0 &&
               parsed.weeklyQuotaAlert.mode ==
                   codex_monitor::codex::WeeklyQuotaAlertMode::kNaturalDay,
           "weekly alert enablement, threshold, and mode must round-trip together");

    const std::vector<ModuleId> home = codex_monitor::VisibleHomeModules(parsed);
    const std::vector<ModuleId> nativeCodex =
        codex_monitor::VisibleModulesForNativePage(parsed, Page::kCodex);
    Expect(home.size() == 1 && Contains(home, ModuleId::kCodexFiveHourQuota),
           "the five-hour quota homepage switch must round-trip independently");
    Expect(!Contains(home, ModuleId::kCodexWeeklyQuota),
           "the weekly quota homepage switch must remain off independently");
    Expect(nativeCodex.size() == 2 &&
               Contains(nativeCodex, ModuleId::kCodexWeeklyQuota) &&
               Contains(nativeCodex, ModuleId::kOpenAIServiceStatus),
           "the weekly quota and service-status native-page switches must round-trip");
    Expect(!Contains(nativeCodex, ModuleId::kCodexFiveHourQuota),
           "the five-hour quota native-page switch must remain off independently");
    Expect(!Contains(nativeCodex, ModuleId::kCodexQuotaForecast),
           "an explicit native-page selection must not silently add the forecast module");
    Expect(!Contains(nativeCodex, ModuleId::kCodexTokenCostEstimate),
           "an explicit native-page selection must not silently add the cost Beta module");
    Expect(!Contains(nativeCodex, ModuleId::kCodexTaskActivity),
           "an explicit native-page selection must not silently add activity scanning");
}

void TestVersionSevenIoMigrationPreservesExplicitChoices() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    const codex_monitor::SettingsState migrated = codex_monitor::ParseSettings(
        "version=7\n"
        "home_order=codex-weekly-quota,codex-five-hour-quota,system-resources\n"
        "home_visible=codex-five-hour-quota\n"
        "native_visible=system-resources,codex-weekly-quota\n");

    Expect(migrated.homeOrder.size() == codex_monitor::kModuleCount &&
               Contains(migrated.homeOrder, ModuleId::kSystemIoThroughput),
           "version 7 order migration must append the new live-I/O module");
    const std::vector<ModuleId> home =
        codex_monitor::VisibleHomeModules(migrated);
    const std::vector<ModuleId> computer =
        codex_monitor::VisibleModulesForNativePage(migrated, Page::kComputer);
    const std::vector<ModuleId> codex =
        codex_monitor::VisibleModulesForNativePage(migrated, Page::kCodex);
    Expect(!Contains(home, ModuleId::kSystemIoThroughput) &&
               !Contains(computer, ModuleId::kSystemIoThroughput),
           "an explicit version 7 selection must not be rewritten during migration");
    Expect(Contains(home, ModuleId::kCodexFiveHourQuota) &&
               !Contains(home, ModuleId::kCodexWeeklyQuota) &&
               Contains(codex, ModuleId::kCodexWeeklyQuota) &&
               !Contains(codex, ModuleId::kCodexFiveHourQuota),
           "I/O migration must keep five-hour and weekly switches independent");
}

void TestLegacyShapeMigrationKeepsOriginAndResetsUniformScale() {
    const codex_monitor::SettingsState migrated = codex_monitor::ParseSettings(
        "version=8\n"
        "window=-720,90,970,360\n"
        "always_on_top=0\n");
    Expect(migrated.windowPlacement && migrated.windowPlacement->x == -720 &&
               migrated.windowPlacement->y == 90,
           "legacy migration must retain the user's saved screen origin");
    Expect(migrated.windowScale == 1.0,
           "legacy independent width and height must reset to one uniform scale");
    Expect(migrated.legacyWindowSizeNeedsMigration,
           "legacy shape data must request a one-time uniform frame rebuild");

    const codex_monitor::SettingsState damaged = codex_monitor::ParseSettings(
        "version=9\n"
        "window=10,20,800,240\n"
        "window_scale=nan\n"
        "opacity_percent=95\n"
        "theme=neon\n");
    Expect(damaged.windowScale == 1.0 &&
               damaged.legacyWindowSizeNeedsMigration,
           "a damaged scale must safely rebuild the saved shape at 100 percent");
    Expect(damaged.opacityPercent == 100,
           "unsupported opacity values must retain the safe opaque default");
    Expect(damaged.theme == codex_monitor::HudTheme::kBlue,
           "unknown themes must retain the low-saturation blue default");
}

void TestWeeklyQuotaAlertDefaultsAndMigrationAreOptIn() {
    using codex_monitor::codex::WeeklyQuotaAlertMode;

    const codex_monitor::SettingsState defaults =
        codex_monitor::DefaultSettings();
    Expect(!defaults.weeklyQuotaAlert.enabled &&
               defaults.weeklyQuotaAlert.thresholdPercent == 15.0 &&
               defaults.weeklyQuotaAlert.mode ==
                   WeeklyQuotaAlertMode::kRolling24Hours,
           "weekly alerts must default off at fifteen percent in rolling mode");

    const auto oldVersion = codex_monitor::ParseSettings(
        "version=8\n"
        "weekly_quota_alert_enabled=1\n"
        "weekly_quota_alert_threshold=20\n"
        "weekly_quota_alert_mode=naturalDay\n");
    Expect(!oldVersion.weeklyQuotaAlert.enabled,
           "an older settings schema must never silently opt into background alerts");

    const auto appearanceVersion = codex_monitor::ParseSettings(
        "version=9\n"
        "window=10,20,575,850\n"
        "window_locked=1\n"
        "window_scale=1.2500\n"
        "opacity_percent=82\n"
        "theme=green\n"
        "weekly_quota_alert_enabled=1\n"
        "weekly_quota_alert_threshold=20\n"
        "weekly_quota_alert_mode=naturalDay\n");
    Expect(!appearanceVersion.weeklyQuotaAlert.enabled,
           "the version 9 appearance schema must not opt into a version 10 alert");
    Expect(appearanceVersion.windowScale == 1.25 &&
               appearanceVersion.windowLocked &&
               appearanceVersion.opacityPercent == 82 &&
               appearanceVersion.theme == codex_monitor::HudTheme::kGreen &&
               !appearanceVersion.legacyWindowSizeNeedsMigration,
           "version 9 appearance settings must migrate without loss");

    const auto migratedVersionTen = codex_monitor::ParseSettings(
        "version=10\nweekly_quota_alert_enabled=1\n"
        "weekly_quota_alert_threshold=20\n"
        "weekly_quota_alert_mode=naturalDay\n");
    Expect(migratedVersionTen.weeklyQuotaAlert.enabled &&
               migratedVersionTen.weeklyQuotaAlert.thresholdPercent == 20.0 &&
               migratedVersionTen.weeklyQuotaAlert.mode ==
                   WeeklyQuotaAlertMode::kNaturalDay,
           "a complete enabled version 10 alert must survive the version 11 migration");

    const auto partialCurrent = codex_monitor::ParseSettings(
        "version=11\nweekly_quota_alert_enabled=1\n"
        "weekly_quota_alert_threshold=20\n");
    Expect(!partialCurrent.weeklyQuotaAlert.enabled &&
               partialCurrent.weeklyQuotaAlert.thresholdPercent == 15.0,
           "a partial current alert triplet must fall back to safe defaults");

    const auto invalidThreshold = codex_monitor::ParseSettings(
        "version=11\nweekly_quota_alert_enabled=1\n"
        "weekly_quota_alert_threshold=101\n"
        "weekly_quota_alert_mode=naturalDay\n");
    Expect(!invalidThreshold.weeklyQuotaAlert.enabled,
           "an out-of-range alert threshold must keep the feature disabled");

    const auto duplicateField = codex_monitor::ParseSettings(
        "version=11\nweekly_quota_alert_enabled=1\n"
        "weekly_quota_alert_enabled=0\n"
        "weekly_quota_alert_threshold=20\n"
        "weekly_quota_alert_mode=rolling24h\n");
    Expect(!duplicateField.weeklyQuotaAlert.enabled,
           "duplicate alert fields must keep the opt-in feature disabled");
}

void TestVersionThreeMigrationPreservesExplicitVisibility() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    const codex_monitor::SettingsState migrated = codex_monitor::ParseSettings(
        "version=3\n"
        "page=codex\n"
        "home_order=codex-weekly-quota,system-resources\n"
        "home_visible=codex-weekly-quota\n"
        "native_visible=codex-five-hour-quota,codex-weekly-quota,codex-subscription-type,codex-recent-tasks\n");

    Expect(!codex_monitor::HomeNeedsServiceStatus(migrated),
           "version 3 migration must not silently add service-status homepage work");
    Expect(!codex_monitor::HomeNeedsCodexActivity(migrated),
           "version 3 migration must not silently add activity scanning");
    const std::vector<ModuleId> nativeCodex =
        codex_monitor::VisibleModulesForNativePage(migrated, Page::kCodex);
    Expect(!Contains(nativeCodex, ModuleId::kOpenAIServiceStatus),
           "version 3 migration must preserve an explicit Codex-page selection");
    Expect(!Contains(nativeCodex, ModuleId::kCodexQuotaForecast),
           "version 3 migration must not silently enable the forecast module");
    Expect(!Contains(nativeCodex, ModuleId::kCodexTaskActivity),
           "version 3 migration must not silently enable activity scanning");
    Expect(migrated.homeOrder.size() == codex_monitor::kModuleCount,
           "version 3 migration must append the service-status module to homepage order");
}

void TestVersionOneMigrationPreservesOldHomeChoices() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    const codex_monitor::SettingsState migrated = codex_monitor::ParseSettings(
        "version=1\n"
        "page=computer\n"
        "always_on_top=0\n"
        "home_order=top-memory-processes,target-process-tree,commit-page-file,system-resources\n"
        "home_visible=target-process-tree,system-resources\n"
        "window=-400,80,650,620\n");

    const std::vector<ModuleId> expectedOldOrder = {
        ModuleId::kTopMemoryProcesses,
        ModuleId::kTargetProcessTree,
        ModuleId::kCommitAndPageFile,
        ModuleId::kSystemResources,
    };
    Expect(std::equal(expectedOldOrder.begin(), expectedOldOrder.end(),
                      migrated.homeOrder.begin()),
           "version 1 migration must preserve the old four-module order prefix");
    Expect(migrated.homeOrder.size() == codex_monitor::kModuleCount,
           "version 1 migration must append registry additions to the saved order");

    const std::vector<ModuleId> visibleHome = codex_monitor::VisibleHomeModules(migrated);
    Expect(visibleHome.size() == 2 &&
               Contains(visibleHome, ModuleId::kTargetProcessTree) &&
               Contains(visibleHome, ModuleId::kSystemResources),
           "version 1 migration must preserve the old homepage visibility selection");
    for (ModuleId id : visibleHome) {
        Expect(!codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)]
                    .requiresCodexData,
               "version 1 migration must not auto-enable a new Codex homepage module");
    }
    Expect(!codex_monitor::HomeNeedsCodexData(migrated),
           "sanitizing a version 1 order must not start Codex data work");
    Expect(!codex_monitor::HomeNeedsCodexActivity(migrated),
           "sanitizing a version 1 order must not start activity scanning");
    Expect(!codex_monitor::HomeNeedsServiceStatus(migrated),
           "sanitizing a version 1 order must not start service-status work");

    const std::vector<ModuleId> nativeCodex =
        codex_monitor::VisibleModulesForNativePage(migrated, Page::kCodex);
    Expect(Contains(nativeCodex, ModuleId::kCodexFiveHourQuota) &&
               Contains(nativeCodex, ModuleId::kCodexWeeklyQuota) &&
               Contains(nativeCodex, ModuleId::kCodexRecentTasks) &&
               Contains(nativeCodex, ModuleId::kOpenAIServiceStatus) &&
               !Contains(nativeCodex, ModuleId::kCodexQuotaForecast) &&
               !Contains(nativeCodex, ModuleId::kCodexTaskActivity) &&
               !Contains(nativeCodex, ModuleId::kCodexAccountTokenUsage),
           "version 1 migration must keep pre-v5 Codex defaults without enabling forecast");
    const std::vector<ModuleId> nativeComputer =
        codex_monitor::VisibleModulesForNativePage(migrated, Page::kComputer);
    Expect(Contains(nativeComputer, ModuleId::kSystemDiagnosis) &&
               Contains(nativeComputer, ModuleId::kSystemIoThroughput),
           "version 1 migration without a native visibility key must use current computer defaults");
}

void TestForecastNativeDefaultMigrationPolicy() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    const auto ForecastIsVisible = [](const codex_monitor::SettingsState& settings) {
        return Contains(codex_monitor::VisibleModulesForNativePage(settings, Page::kCodex),
                        ModuleId::kCodexQuotaForecast);
    };

    Expect(ForecastIsVisible(codex_monitor::DefaultSettings()),
           "a genuinely new install must use the current forecast default");
    Expect(!ForecastIsVisible(codex_monitor::ParseSettings("")),
           "an empty existing settings file must not opt into forecast");
    Expect(ForecastIsVisible(codex_monitor::ParseSettings(
               "version=5\npage=codex\n")),
           "a valid version 5 file missing native visibility may use current defaults");
    Expect(!ForecastIsVisible(codex_monitor::ParseSettings(
               "version=4\npage=codex\n")),
           "version 4 missing native visibility must not opt into forecast");
    Expect(!ForecastIsVisible(codex_monitor::ParseSettings(
               "version=broken\npage=codex\n")),
           "an unknown non-empty settings version must not opt into forecast");
    Expect(!ForecastIsVisible(codex_monitor::ParseSettings(
               "version=5\nnative_visible=unknown\n")),
           "a damaged native visibility entry must not opt into forecast");
    Expect(!ForecastIsVisible(codex_monitor::ParseSettings(
               "version=5\nnative_visible=codex-weekly-quota\n")),
           "an explicit version 5 selection must leave unlisted forecast off");
}

void TestActivityNativeDefaultMigrationPolicy() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;
    const auto ActivityIsVisible = [](const codex_monitor::SettingsState& settings) {
        return Contains(
            codex_monitor::VisibleModulesForNativePage(settings, Page::kCodex),
            ModuleId::kCodexTaskActivity);
    };
    Expect(ActivityIsVisible(codex_monitor::DefaultSettings()),
           "a new install must show current task activity on the Codex page");
    Expect(ActivityIsVisible(codex_monitor::ParseSettings(
               "version=11\npage=codex\n")),
           "the current settings schema missing native visibility may use the activity default");
    Expect(ActivityIsVisible(codex_monitor::ParseSettings(
               "version=10\npage=codex\n")),
           "the alert schema missing native visibility may retain the activity default");
    Expect(ActivityIsVisible(codex_monitor::ParseSettings(
               "version=9\npage=codex\n")),
           "the appearance schema missing native visibility may retain the activity default");
    Expect(ActivityIsVisible(codex_monitor::ParseSettings(
               "version=8\npage=codex\n")),
           "the previous schema missing native visibility may retain the activity default");
    Expect(ActivityIsVisible(codex_monitor::ParseSettings(
               "version=7\npage=codex\n")),
           "the previous schema missing native visibility may retain the activity default");
    Expect(!ActivityIsVisible(codex_monitor::ParseSettings(
               "version=6\npage=codex\n")),
           "a version 6 file must not silently opt into new filesystem scanning");
    Expect(!ActivityIsVisible(codex_monitor::ParseSettings(
               "version=7\nnative_visible=codex-weekly-quota\n")),
           "an explicit current selection must leave unlisted activity off");
}

void TestPageFilteringAndHomeDemandGates() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    settings.homeVisible.fill(false);
    Expect(!codex_monitor::HomeNeedsPerformanceData(settings) &&
               !codex_monitor::HomeNeedsCodexData(settings) &&
               !codex_monitor::HomeNeedsCodexActivity(settings) &&
               !codex_monitor::HomeNeedsServiceStatus(settings),
           "an intentionally empty homepage must not request any data source");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kSystemResources)] = true;
    Expect(codex_monitor::HomeNeedsPerformanceData(settings) &&
               !codex_monitor::HomeNeedsCodexData(settings) &&
               !codex_monitor::HomeNeedsCodexActivity(settings) &&
               !codex_monitor::HomeNeedsServiceStatus(settings),
           "a performance-only homepage must request only performance data");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kCodexWeeklyQuota)] = true;
    Expect(codex_monitor::HomeNeedsPerformanceData(settings) &&
               codex_monitor::HomeNeedsCodexData(settings) &&
               !codex_monitor::HomeNeedsCodexActivity(settings) &&
               !codex_monitor::HomeNeedsServiceStatus(settings),
           "a mixed homepage must request both independent data sources");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kSystemResources)] = false;
    Expect(!codex_monitor::HomeNeedsPerformanceData(settings) &&
               codex_monitor::HomeNeedsCodexData(settings) &&
               !codex_monitor::HomeNeedsCodexActivity(settings) &&
               !codex_monitor::HomeNeedsServiceStatus(settings),
           "a Codex-only homepage must stop performance sampling");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kCodexWeeklyQuota)] = false;
    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kCodexTaskActivity)] = true;
    Expect(!codex_monitor::HomeNeedsPerformanceData(settings) &&
               !codex_monitor::HomeNeedsCodexData(settings) &&
               codex_monitor::HomeNeedsCodexActivity(settings) &&
               !codex_monitor::HomeNeedsServiceStatus(settings),
           "an activity-only homepage must request only the local activity scanner");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kCodexTaskActivity)] = false;
    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kOpenAIServiceStatus)] = true;
    Expect(!codex_monitor::HomeNeedsPerformanceData(settings) &&
               !codex_monitor::HomeNeedsCodexData(settings) &&
               !codex_monitor::HomeNeedsCodexActivity(settings) &&
               codex_monitor::HomeNeedsServiceStatus(settings),
           "a service-status-only homepage must request only official status data");

    settings.nativePageVisible.fill(true);
    const std::vector<ModuleId> computer =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kComputer);
    Expect(computer.size() == 7,
           "the computer page must filter out all nine Codex-page modules even when enabled");
    const std::vector<ModuleId> codex =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kCodex);
    Expect(codex.size() == 9,
           "the Codex page must include its nine modules and filter out computer modules");
}

void TestCpuTrendDefaultsAndMigrationPolicy() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;
    const auto IsHomeVisible = [](const codex_monitor::SettingsState& settings) {
        return Contains(codex_monitor::VisibleHomeModules(settings),
                        ModuleId::kCpuTrend);
    };
    const auto IsComputerVisible = [](const codex_monitor::SettingsState& settings) {
        return Contains(
            codex_monitor::VisibleModulesForNativePage(settings, Page::kComputer),
            ModuleId::kCpuTrend);
    };

    const auto defaults = codex_monitor::DefaultSettings();
    Expect(IsHomeVisible(defaults) && IsComputerVisible(defaults),
           "a genuine new install must show CPU trend on Home and Computer");

    for (const std::string& existing : {
             std::string{},
             std::string{"version=broken\n"},
             std::string{"version=10\npage=computer\n"},
             std::string{"version=11\npage=computer\n"},
         }) {
        const auto parsed = codex_monitor::ParseSettings(existing);
        Expect(!IsHomeVisible(parsed) && !IsComputerVisible(parsed),
               "missing or damaged existing visibility must not opt into the new trend card");
    }

    const auto explicitVersionTen = codex_monitor::ParseSettings(
        "version=10\n"
        "home_visible=system-resources\n"
        "native_visible=system-resources\n");
    Expect(!IsHomeVisible(explicitVersionTen) &&
               !IsComputerVisible(explicitVersionTen),
           "a version 10 explicit selection must remain unchanged");

    const auto explicitVersionEleven = codex_monitor::ParseSettings(
        "version=11\n"
        "home_visible=cpu-trend\n"
        "native_visible=cpu-trend\n");
    Expect(IsHomeVisible(explicitVersionEleven) &&
               IsComputerVisible(explicitVersionEleven),
           "version 11 must persist independently selected trend cards");
}

void TestMalformedSettingsFallBackSafely() {
    using codex_monitor::Page;

    const codex_monitor::SettingsState parsed = codex_monitor::ParseSettings(
        "version=broken\n"
        "page=not-a-page\n"
        "always_on_top=maybe\n"
        "home_order=unknown,target-process-tree,target-process-tree\n"
        "home_visible=unknown\n"
        "native_visible=unknown\n"
        "window=12,broken,400,-5\n");
    Expect(parsed.currentPage == Page::kHome,
           "an unknown page must fall back to home");
    Expect(parsed.alwaysOnTop, "an invalid topmost value must retain the default");
    Expect(parsed.homeOrder.size() == codex_monitor::kModuleCount,
           "a malformed order must still contain the full registry");
    Expect(codex_monitor::VisibleHomeModules(parsed).size() == 5,
           "unknown homepage visibility keys must retain safe registry defaults");
    Expect(!codex_monitor::HomeNeedsCodexData(parsed),
           "damaged settings must not silently enable new Codex homepage work");
    Expect(!codex_monitor::HomeNeedsCodexActivity(parsed),
           "damaged settings must not silently enable activity scanning");
    Expect(!codex_monitor::HomeNeedsServiceStatus(parsed),
           "damaged settings must not silently enable service-status homepage work");
    Expect(codex_monitor::VisibleModulesForNativePage(parsed, Page::kComputer).size() == 6,
           "damaged native visibility must keep the six pre-trend computer defaults");
    Expect(codex_monitor::VisibleModulesForNativePage(parsed, Page::kCodex).size() == 5,
           "damaged native visibility must keep safe Codex defaults");
    Expect(!parsed.windowPlacement, "an invalid window placement must be ignored");
}

void TestWindowPlacementReturnsToAVisibleWorkArea() {
    using codex_monitor::WindowPlacement;
    const std::vector<WindowPlacement> workAreas = {
        {0, 0, 1920, 1040},
        {1920, 0, 2560, 1400},
    };
    const WindowPlacement offscreen = {8000, -4000, 700, 600};
    const WindowPlacement clamped = codex_monitor::ClampWindowPlacement(offscreen, workAreas);
    Expect(clamped.x >= 0 && clamped.y >= 0 &&
               clamped.x + clamped.width <= 1920 &&
               clamped.y + clamped.height <= 1040,
           "an off-screen saved window must return to a visible monitor");

    const WindowPlacement oversized = {-50, -50, 4000, 3000};
    const WindowPlacement fitted = codex_monitor::ClampWindowPlacement(oversized, workAreas);
    const bool fitsFirst = fitted.x >= 0 && fitted.y >= 0 &&
                           fitted.x + fitted.width <= 1920 &&
                           fitted.y + fitted.height <= 1040;
    const bool fitsSecond = fitted.x >= 1920 && fitted.y >= 0 &&
                            fitted.x + fitted.width <= 4480 &&
                            fitted.y + fitted.height <= 1400;
    Expect(fitsFirst || fitsSecond,
           "an oversized saved window must fit its selected work area");
}

}  // namespace

int main() {
    TestNewInstallDefaultsUseRegistryPolicy();
    TestOrderSanitizationAndMovement();
    TestVersionElevenRoundTripAndIndependentQuotaSwitches();
    TestVersionSevenIoMigrationPreservesExplicitChoices();
    TestLegacyShapeMigrationKeepsOriginAndResetsUniformScale();
    TestWeeklyQuotaAlertDefaultsAndMigrationAreOptIn();
    TestVersionThreeMigrationPreservesExplicitVisibility();
    TestVersionOneMigrationPreservesOldHomeChoices();
    TestForecastNativeDefaultMigrationPolicy();
    TestActivityNativeDefaultMigrationPolicy();
    TestPageFilteringAndHomeDemandGates();
    TestCpuTrendDefaultsAndMigrationPolicy();
    TestMalformedSettingsFallBackSafely();
    TestWindowPlacementReturnsToAVisibleWorkArea();
    if (failures != 0) return 1;
    std::cout << "module_state_tests=pass\n";
    return 0;
}
