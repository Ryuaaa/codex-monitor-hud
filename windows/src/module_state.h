#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace codex_monitor {

enum class Page {
    kHome,
    kCodex,
    kComputer,
};

enum class ModuleId {
    kSystemDiagnosis,
    kTargetProcessTree,
    kSystemResources,
    kCommitAndPageFile,
    kTopMemoryProcesses,
    kCodexFiveHourQuota,
    kCodexWeeklyQuota,
    kCodexSubscriptionType,
    kCodexAccountTokenUsage,
    kCodexRecentTasks,
    kOpenAIServiceStatus,
};

struct ModuleDefinition {
    ModuleId id;
    std::string_view key;
    std::wstring_view displayName;
    Page nativePage;
    bool requiresPerformanceSampling;
    bool requiresCodexData;
    bool requiresServiceStatus;
    bool defaultHomeVisible;
    bool defaultNativePageVisible;
};

constexpr std::size_t kModuleCount = 11;

struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct SettingsState {
    Page currentPage = Page::kHome;
    bool alwaysOnTop = true;
    std::vector<ModuleId> homeOrder;
    std::array<bool, kModuleCount> homeVisible{};
    std::array<bool, kModuleCount> nativePageVisible{};
    std::optional<WindowPlacement> windowPlacement;
};

const std::array<ModuleDefinition, kModuleCount>& ModuleRegistry();
SettingsState DefaultSettings();

std::size_t ModuleIndex(ModuleId id);
std::string_view ModuleKey(ModuleId id);
std::optional<ModuleId> ModuleIdFromKey(std::string_view key);

std::vector<ModuleId> SanitizeHomeOrder(const std::vector<ModuleId>& requestedOrder);
std::vector<ModuleId> VisibleHomeModules(const SettingsState& settings);
std::vector<ModuleId> VisibleModulesForNativePage(
    const SettingsState& settings,
    Page page);
bool HomeNeedsPerformanceData(const SettingsState& settings);
bool HomeNeedsCodexData(const SettingsState& settings);
bool HomeNeedsServiceStatus(const SettingsState& settings);
bool MoveHomeModule(SettingsState& settings, ModuleId id, int direction);

std::string SerializeSettings(const SettingsState& settings);
SettingsState ParseSettings(std::string_view text);

WindowPlacement ClampWindowPlacement(
    WindowPlacement placement,
    const std::vector<WindowPlacement>& visibleWorkAreas);

}  // namespace codex_monitor
