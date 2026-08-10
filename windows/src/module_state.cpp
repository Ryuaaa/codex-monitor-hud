#include "module_state.h"

#include <algorithm>
#include <charconv>
#include <sstream>

namespace codex_monitor {
namespace {

constexpr std::array<ModuleDefinition, kModuleCount> kRegistry{{
    {ModuleId::kSystemDiagnosis,
     "system-diagnosis",
     L"System diagnosis & Codex / ChatGPT impact",
     Page::kComputer,
     true,
     false,
     false,
     true,
     true},
    {ModuleId::kTargetProcessTree,
     "target-process-tree",
     L"Codex / ChatGPT process tree",
     Page::kComputer,
     true,
     false,
     false,
     true,
     true},
    {ModuleId::kSystemResources,
     "system-resources",
     L"System CPU & physical memory",
     Page::kComputer,
     true,
     false,
     false,
     true,
     true},
    {ModuleId::kCommitAndPageFile,
     "commit-page-file",
     L"Commit & page file",
     Page::kComputer,
     true,
     false,
     false,
     true,
     true},
    {ModuleId::kTopMemoryProcesses,
     "top-memory-processes",
     L"Top processes by memory",
     Page::kComputer,
     true,
     false,
     false,
     true,
     true},
    {ModuleId::kCodexFiveHourQuota,
     "codex-five-hour-quota",
     L"Codex 5-hour quota",
     Page::kCodex,
     false,
     true,
     false,
     false,
     true},
    {ModuleId::kCodexWeeklyQuota,
     "codex-weekly-quota",
     L"Codex weekly quota",
     Page::kCodex,
     false,
     true,
     false,
     false,
     true},
    {ModuleId::kCodexSubscriptionType,
     "codex-subscription-type",
     L"Codex subscription type",
     Page::kCodex,
     false,
     true,
     false,
     false,
     true},
    {ModuleId::kCodexAccountTokenUsage,
     "codex-account-token-usage",
     L"Codex account Token usage",
     Page::kCodex,
     false,
     true,
     false,
     false,
     false},
    {ModuleId::kCodexRecentTasks,
     "codex-recent-tasks",
     L"Codex recent tasks (history)",
     Page::kCodex,
     false,
     true,
     false,
     false,
     true},
    {ModuleId::kOpenAIServiceStatus,
     "openai-service-status",
     L"OpenAI official service status",
     Page::kCodex,
     false,
     false,
     true,
     false,
     true},
}};

std::string_view PageKey(Page page) {
    switch (page) {
        case Page::kHome:
            return "home";
        case Page::kCodex:
            return "codex";
        case Page::kComputer:
            return "computer";
    }
    return "home";
}

Page PageFromKey(std::string_view key) {
    if (key == "codex") return Page::kCodex;
    if (key == "computer") return Page::kComputer;
    return Page::kHome;
}

std::vector<std::string_view> Split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        result.push_back(value.substr(start, end == std::string_view::npos
                                                ? value.size() - start
                                                : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::optional<int> ParseInt(std::string_view value) {
    if (value.empty()) return std::nullopt;
    int parsed = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return parsed;
}

long long IntersectionArea(const WindowPlacement& left, const WindowPlacement& right) {
    const long long intersectionWidth = std::max(
        0LL,
        std::min(static_cast<long long>(left.x) + left.width,
                 static_cast<long long>(right.x) + right.width) -
            std::max(static_cast<long long>(left.x), static_cast<long long>(right.x)));
    const long long intersectionHeight = std::max(
        0LL,
        std::min(static_cast<long long>(left.y) + left.height,
                 static_cast<long long>(right.y) + right.height) -
            std::max(static_cast<long long>(left.y), static_cast<long long>(right.y)));
    return intersectionWidth * intersectionHeight;
}

}  // namespace

const std::array<ModuleDefinition, kModuleCount>& ModuleRegistry() {
    return kRegistry;
}

SettingsState DefaultSettings() {
    SettingsState settings;
    for (std::size_t index = 0; index < kRegistry.size(); ++index) {
        const ModuleDefinition& definition = kRegistry[index];
        settings.homeOrder.push_back(definition.id);
        settings.homeVisible[index] = definition.defaultHomeVisible;
        settings.nativePageVisible[index] = definition.defaultNativePageVisible;
    }
    return settings;
}

std::size_t ModuleIndex(ModuleId id) {
    for (std::size_t index = 0; index < kRegistry.size(); ++index) {
        if (kRegistry[index].id == id) return index;
    }
    return 0;
}

std::string_view ModuleKey(ModuleId id) {
    return kRegistry[ModuleIndex(id)].key;
}

std::optional<ModuleId> ModuleIdFromKey(std::string_view key) {
    for (const ModuleDefinition& definition : kRegistry) {
        if (definition.key == key) return definition.id;
    }
    return std::nullopt;
}

std::vector<ModuleId> SanitizeHomeOrder(const std::vector<ModuleId>& requestedOrder) {
    std::vector<ModuleId> result;
    std::array<bool, kModuleCount> included{};
    for (ModuleId id : requestedOrder) {
        const std::size_t index = ModuleIndex(id);
        if (!included[index] && kRegistry[index].id == id) {
            included[index] = true;
            result.push_back(id);
        }
    }
    for (const ModuleDefinition& definition : kRegistry) {
        const std::size_t index = ModuleIndex(definition.id);
        if (!included[index]) result.push_back(definition.id);
    }
    return result;
}

std::vector<ModuleId> VisibleHomeModules(const SettingsState& settings) {
    std::vector<ModuleId> result;
    for (ModuleId id : SanitizeHomeOrder(settings.homeOrder)) {
        if (settings.homeVisible[ModuleIndex(id)]) result.push_back(id);
    }
    return result;
}

std::vector<ModuleId> VisibleModulesForNativePage(
    const SettingsState& settings,
    Page page) {
    std::vector<ModuleId> result;
    if (page == Page::kHome) return result;

    for (const ModuleDefinition& definition : kRegistry) {
        const std::size_t index = ModuleIndex(definition.id);
        if (definition.nativePage == page && settings.nativePageVisible[index]) {
            result.push_back(definition.id);
        }
    }
    return result;
}

bool HomeNeedsPerformanceData(const SettingsState& settings) {
    for (ModuleId id : VisibleHomeModules(settings)) {
        if (kRegistry[ModuleIndex(id)].requiresPerformanceSampling) return true;
    }
    return false;
}

bool HomeNeedsCodexData(const SettingsState& settings) {
    for (ModuleId id : VisibleHomeModules(settings)) {
        if (kRegistry[ModuleIndex(id)].requiresCodexData) return true;
    }
    return false;
}

bool HomeNeedsServiceStatus(const SettingsState& settings) {
    for (ModuleId id : VisibleHomeModules(settings)) {
        if (kRegistry[ModuleIndex(id)].requiresServiceStatus) return true;
    }
    return false;
}

bool MoveHomeModule(SettingsState& settings, ModuleId id, int direction) {
    settings.homeOrder = SanitizeHomeOrder(settings.homeOrder);
    const auto current = std::find(settings.homeOrder.begin(), settings.homeOrder.end(), id);
    if (current == settings.homeOrder.end() || direction == 0) return false;
    const auto index = static_cast<std::size_t>(std::distance(settings.homeOrder.begin(), current));
    if ((direction < 0 && index == 0) ||
        (direction > 0 && index + 1 >= settings.homeOrder.size())) {
        return false;
    }
    const std::size_t destination = direction < 0 ? index - 1 : index + 1;
    std::swap(settings.homeOrder[index], settings.homeOrder[destination]);
    return true;
}

std::string SerializeSettings(const SettingsState& settings) {
    const std::vector<ModuleId> order = SanitizeHomeOrder(settings.homeOrder);
    std::ostringstream output;
    output << "version=4\n";
    output << "page=" << PageKey(settings.currentPage) << '\n';
    output << "always_on_top=" << (settings.alwaysOnTop ? 1 : 0) << '\n';
    output << "home_order=";
    for (std::size_t index = 0; index < order.size(); ++index) {
        if (index > 0) output << ',';
        output << ModuleKey(order[index]);
    }
    output << "\nhome_visible=";
    bool wroteVisible = false;
    for (ModuleId id : order) {
        if (!settings.homeVisible[ModuleIndex(id)]) continue;
        if (wroteVisible) output << ',';
        output << ModuleKey(id);
        wroteVisible = true;
    }
    output << "\nnative_visible=";
    bool wroteNativeVisible = false;
    for (const ModuleDefinition& definition : kRegistry) {
        if (!settings.nativePageVisible[ModuleIndex(definition.id)]) continue;
        if (wroteNativeVisible) output << ',';
        output << definition.key;
        wroteNativeVisible = true;
    }
    output << '\n';
    if (settings.windowPlacement) {
        const WindowPlacement& placement = *settings.windowPlacement;
        output << "window=" << placement.x << ',' << placement.y << ','
               << placement.width << ',' << placement.height << '\n';
    }
    return output.str();
}

SettingsState ParseSettings(std::string_view text) {
    SettingsState settings = DefaultSettings();
    bool homeVisibleKeyFound = false;
    bool nativeVisibleKeyFound = false;
    std::vector<ModuleId> requestedOrder;

    for (std::string_view line : Split(text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t separator = line.find('=');
        if (separator == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, separator);
        const std::string_view value = line.substr(separator + 1);

        if (key == "page") {
            settings.currentPage = PageFromKey(value);
        } else if (key == "always_on_top") {
            if (value == "0") settings.alwaysOnTop = false;
            if (value == "1") settings.alwaysOnTop = true;
        } else if (key == "home_order") {
            requestedOrder.clear();
            for (std::string_view token : Split(value, ',')) {
                if (const auto id = ModuleIdFromKey(token)) requestedOrder.push_back(*id);
            }
        } else if (key == "home_visible") {
            std::array<bool, kModuleCount> requestedVisibility{};
            bool recognizedVisibleModule = false;
            for (std::string_view token : Split(value, ',')) {
                if (const auto id = ModuleIdFromKey(token)) {
                    requestedVisibility[ModuleIndex(*id)] = true;
                    recognizedVisibleModule = true;
                }
            }
            if (value.empty() || recognizedVisibleModule) {
                homeVisibleKeyFound = true;
                settings.homeVisible = requestedVisibility;
            }
        } else if (key == "native_visible") {
            std::array<bool, kModuleCount> requestedVisibility{};
            bool recognizedVisibleModule = false;
            for (std::string_view token : Split(value, ',')) {
                if (const auto id = ModuleIdFromKey(token)) {
                    requestedVisibility[ModuleIndex(*id)] = true;
                    recognizedVisibleModule = true;
                }
            }
            if (value.empty() || recognizedVisibleModule) {
                nativeVisibleKeyFound = true;
                settings.nativePageVisible = requestedVisibility;
            }
        } else if (key == "window") {
            const std::vector<std::string_view> parts = Split(value, ',');
            if (parts.size() != 4) continue;
            const auto x = ParseInt(parts[0]);
            const auto y = ParseInt(parts[1]);
            const auto width = ParseInt(parts[2]);
            const auto height = ParseInt(parts[3]);
            if (x && y && width && height && *width >= 100 && *height >= 100) {
                settings.windowPlacement = WindowPlacement{*x, *y, *width, *height};
            }
        }
    }

    settings.homeOrder = SanitizeHomeOrder(requestedOrder);
    // Missing visibility keys mean an older settings file. Registry defaults
    // enable the current performance cards, keep Codex Home cards off, and
    // establish independent defaults for the native pages.
    if (!homeVisibleKeyFound) {
        for (std::size_t index = 0; index < kRegistry.size(); ++index) {
            settings.homeVisible[index] = kRegistry[index].defaultHomeVisible;
        }
    }
    if (!nativeVisibleKeyFound) {
        for (std::size_t index = 0; index < kRegistry.size(); ++index) {
            settings.nativePageVisible[index] =
                kRegistry[index].defaultNativePageVisible;
        }
    }
    return settings;
}

WindowPlacement ClampWindowPlacement(
    WindowPlacement placement,
    const std::vector<WindowPlacement>& visibleWorkAreas) {
    if (visibleWorkAreas.empty()) return placement;

    const WindowPlacement* targetArea = &visibleWorkAreas.front();
    long long greatestIntersection = IntersectionArea(placement, *targetArea);
    for (const WindowPlacement& area : visibleWorkAreas) {
        const long long intersection = IntersectionArea(placement, area);
        if (intersection > greatestIntersection) {
            greatestIntersection = intersection;
            targetArea = &area;
        }
    }

    placement.width = std::clamp(placement.width, 1, targetArea->width);
    placement.height = std::clamp(placement.height, 1, targetArea->height);
    placement.x = std::clamp(placement.x, targetArea->x,
                             targetArea->x + targetArea->width - placement.width);
    placement.y = std::clamp(placement.y, targetArea->y,
                             targetArea->y + targetArea->height - placement.height);
    return placement;
}

}  // namespace codex_monitor
