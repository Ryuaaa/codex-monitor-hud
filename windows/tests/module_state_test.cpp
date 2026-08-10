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
    Expect(home.size() == 4,
           "only the original four performance modules are visible on a new homepage");
    Expect(codex_monitor::HomeNeedsPerformanceData(settings),
           "the default homepage must request performance data");
    Expect(!codex_monitor::HomeNeedsCodexData(settings),
           "new Codex modules must not silently add homepage data work");

    const std::vector<ModuleId> computer =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kComputer);
    Expect(computer.size() == 4,
           "the computer page must contain exactly the four performance modules");
    for (ModuleId id : computer) {
        const codex_monitor::ModuleDefinition& definition =
            codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)];
        Expect(definition.nativePage == Page::kComputer &&
                   definition.requiresPerformanceSampling &&
                   !definition.requiresCodexData,
               "every computer-page module must be a performance-only module");
    }

    const std::vector<ModuleId> codex =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kCodex);
    Expect(codex.size() == 4,
           "the Codex page defaults to both quotas, subscription, and recent history");
    Expect(Contains(codex, ModuleId::kCodexFiveHourQuota),
           "the five-hour quota must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kCodexWeeklyQuota),
           "the weekly quota must be visible on a new Codex page");
    Expect(Contains(codex, ModuleId::kCodexRecentTasks),
           "recent task history must be visible on a new Codex page");
    Expect(!Contains(codex, ModuleId::kCodexAccountTokenUsage),
           "account Token usage must default to off");
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

void TestVersionTwoRoundTripAndIndependentQuotaSwitches() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    settings.currentPage = Page::kCodex;
    settings.alwaysOnTop = false;
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
    settings.windowPlacement = codex_monitor::WindowPlacement{-900, 120, 720, 640};

    const std::string serialized = codex_monitor::SerializeSettings(settings);
    Expect(serialized.find("version=2\n") == 0,
           "the settings whitelist must be serialized as version 2");

    const codex_monitor::SettingsState parsed = codex_monitor::ParseSettings(serialized);
    Expect(parsed.currentPage == Page::kCodex,
           "the current page must survive persistence");
    Expect(!parsed.alwaysOnTop, "the topmost preference must survive persistence");
    Expect(parsed.homeOrder == settings.homeOrder,
           "the homepage module order must survive persistence");
    Expect(parsed.windowPlacement && parsed.windowPlacement->x == -900 &&
               parsed.windowPlacement->width == 720,
           "window position and size must survive persistence");

    const std::vector<ModuleId> home = codex_monitor::VisibleHomeModules(parsed);
    const std::vector<ModuleId> nativeCodex =
        codex_monitor::VisibleModulesForNativePage(parsed, Page::kCodex);
    Expect(home.size() == 1 && Contains(home, ModuleId::kCodexFiveHourQuota),
           "the five-hour quota homepage switch must round-trip independently");
    Expect(!Contains(home, ModuleId::kCodexWeeklyQuota),
           "the weekly quota homepage switch must remain off independently");
    Expect(nativeCodex.size() == 1 &&
               Contains(nativeCodex, ModuleId::kCodexWeeklyQuota),
           "the weekly quota native-page switch must round-trip independently");
    Expect(!Contains(nativeCodex, ModuleId::kCodexFiveHourQuota),
           "the five-hour quota native-page switch must remain off independently");
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

    const std::vector<ModuleId> nativeCodex =
        codex_monitor::VisibleModulesForNativePage(migrated, Page::kCodex);
    Expect(Contains(nativeCodex, ModuleId::kCodexFiveHourQuota) &&
               Contains(nativeCodex, ModuleId::kCodexWeeklyQuota) &&
               Contains(nativeCodex, ModuleId::kCodexRecentTasks) &&
               !Contains(nativeCodex, ModuleId::kCodexAccountTokenUsage),
           "version 1 migration must apply independent Codex-page defaults");
}

void TestPageFilteringAndHomeDemandGates() {
    using codex_monitor::ModuleId;
    using codex_monitor::Page;

    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    settings.homeVisible.fill(false);
    Expect(!codex_monitor::HomeNeedsPerformanceData(settings) &&
               !codex_monitor::HomeNeedsCodexData(settings),
           "an intentionally empty homepage must not request either data source");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kSystemResources)] = true;
    Expect(codex_monitor::HomeNeedsPerformanceData(settings) &&
               !codex_monitor::HomeNeedsCodexData(settings),
           "a performance-only homepage must request only performance data");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kCodexWeeklyQuota)] = true;
    Expect(codex_monitor::HomeNeedsPerformanceData(settings) &&
               codex_monitor::HomeNeedsCodexData(settings),
           "a mixed homepage must request both independent data sources");

    settings.homeVisible[codex_monitor::ModuleIndex(ModuleId::kSystemResources)] = false;
    Expect(!codex_monitor::HomeNeedsPerformanceData(settings) &&
               codex_monitor::HomeNeedsCodexData(settings),
           "a Codex-only homepage must stop performance sampling");

    settings.nativePageVisible.fill(true);
    const std::vector<ModuleId> computer =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kComputer);
    Expect(computer.size() == 4,
           "the computer page must filter out all five Codex modules even when enabled");
    const std::vector<ModuleId> codex =
        codex_monitor::VisibleModulesForNativePage(settings, Page::kCodex);
    Expect(codex.size() == 5,
           "the Codex page must filter out all four computer modules even when enabled");
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
    Expect(codex_monitor::VisibleHomeModules(parsed).size() == 4,
           "unknown homepage visibility keys must retain safe registry defaults");
    Expect(!codex_monitor::HomeNeedsCodexData(parsed),
           "damaged settings must not silently enable new Codex homepage work");
    Expect(codex_monitor::VisibleModulesForNativePage(parsed, Page::kComputer).size() == 4,
           "damaged native visibility must keep the four computer defaults");
    Expect(codex_monitor::VisibleModulesForNativePage(parsed, Page::kCodex).size() == 4,
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
    TestVersionTwoRoundTripAndIndependentQuotaSwitches();
    TestVersionOneMigrationPreservesOldHomeChoices();
    TestPageFilteringAndHomeDemandGates();
    TestMalformedSettingsFallBackSafely();
    TestWindowPlacementReturnsToAVisibleWorkArea();
    if (failures != 0) return 1;
    std::cout << "module_state_tests=pass\n";
    return 0;
}
