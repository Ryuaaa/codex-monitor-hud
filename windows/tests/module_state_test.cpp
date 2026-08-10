#include "module_state.h"

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

void TestDefaultsUseTheRegistry() {
    const codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    Expect(settings.currentPage == codex_monitor::Page::kHome,
           "the homepage must be the first-run page");
    Expect(settings.homeOrder.size() == codex_monitor::kModuleCount,
           "every registered module must have a homepage order entry");
    Expect(codex_monitor::VisibleHomeModules(settings).size() == codex_monitor::kModuleCount,
           "every registered module must be visible on the first run");
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

void TestSettingsRoundTripAndNoVisibleModules() {
    using codex_monitor::ModuleId;
    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    settings.currentPage = codex_monitor::Page::kComputer;
    settings.alwaysOnTop = false;
    settings.homeOrder = {
        ModuleId::kTopMemoryProcesses,
        ModuleId::kTargetProcessTree,
        ModuleId::kCommitAndPageFile,
        ModuleId::kSystemResources,
    };
    settings.homeVisible.fill(false);
    settings.windowPlacement = codex_monitor::WindowPlacement{-900, 120, 720, 640};

    const std::string serialized = codex_monitor::SerializeSettings(settings);
    const codex_monitor::SettingsState parsed = codex_monitor::ParseSettings(serialized);
    Expect(parsed.currentPage == codex_monitor::Page::kComputer,
           "the current page must survive persistence");
    Expect(!parsed.alwaysOnTop, "the topmost preference must survive persistence");
    Expect(parsed.homeOrder == settings.homeOrder,
           "the homepage module order must survive persistence");
    Expect(codex_monitor::VisibleHomeModules(parsed).empty(),
           "an intentionally empty homepage must not be reset to defaults");
    Expect(parsed.windowPlacement && parsed.windowPlacement->x == -900 &&
               parsed.windowPlacement->width == 720,
           "window position and size must survive persistence");
}

void TestMalformedSettingsFallBackSafely() {
    const codex_monitor::SettingsState parsed = codex_monitor::ParseSettings(
        "page=not-a-page\n"
        "always_on_top=maybe\n"
        "home_order=unknown,target-process-tree,target-process-tree\n"
        "home_visible=unknown\n"
        "window=12,broken,400,-5\n");
    Expect(parsed.currentPage == codex_monitor::Page::kHome,
           "an unknown page must fall back to home");
    Expect(parsed.alwaysOnTop, "an invalid topmost value must retain the default");
    Expect(parsed.homeOrder.size() == codex_monitor::kModuleCount,
           "a malformed order must still contain the full registry");
    Expect(codex_monitor::VisibleHomeModules(parsed).size() == codex_monitor::kModuleCount,
           "unknown visible-module keys must not hide all modules");
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
    TestDefaultsUseTheRegistry();
    TestOrderSanitizationAndMovement();
    TestSettingsRoundTripAndNoVisibleModules();
    TestMalformedSettingsFallBackSafely();
    TestWindowPlacementReturnsToAVisibleWorkArea();
    if (failures != 0) return 1;
    std::cout << "module_state_tests=pass\n";
    return 0;
}
