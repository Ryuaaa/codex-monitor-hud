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
    kTargetProcessTree,
    kSystemResources,
    kCommitAndPageFile,
    kTopMemoryProcesses,
};

struct ModuleDefinition {
    ModuleId id;
    std::string_view key;
    std::wstring_view displayName;
    bool requiresPerformanceSampling;
};

constexpr std::size_t kModuleCount = 4;

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
    std::optional<WindowPlacement> windowPlacement;
};

const std::array<ModuleDefinition, kModuleCount>& ModuleRegistry();
SettingsState DefaultSettings();

std::size_t ModuleIndex(ModuleId id);
std::string_view ModuleKey(ModuleId id);
std::optional<ModuleId> ModuleIdFromKey(std::string_view key);

std::vector<ModuleId> SanitizeHomeOrder(const std::vector<ModuleId>& requestedOrder);
std::vector<ModuleId> VisibleHomeModules(const SettingsState& settings);
bool MoveHomeModule(SettingsState& settings, ModuleId id, int direction);

std::string SerializeSettings(const SettingsState& settings);
SettingsState ParseSettings(std::string_view text);

WindowPlacement ClampWindowPlacement(
    WindowPlacement placement,
    const std::vector<WindowPlacement>& visibleWorkAreas);

}  // namespace codex_monitor
