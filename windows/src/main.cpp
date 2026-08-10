#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <winrt/base.h>

#include "../resources/resource.h"
#include "codex/codex_cost_hybrid.h"
#include "codex/codex_usage_math.h"
#include "codex/codex_worker.h"
#include "module_state.h"
#include "performance_diagnosis.h"
#include "performance_worker.h"
#include "performance_snapshot.h"
#include "service_status_worker.h"
#include "settings_store_win32.h"
#include "snapshot_math.h"
#include "update/update_worker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"CodexMonitorHUDWindowsWindow";
constexpr wchar_t kSettingsWindowClassName[] = L"CodexMonitorHUDWindowsSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"Codex Monitor HUD";
constexpr wchar_t kSingletonName[] = L"Local\\CodexMonitorHUDWindowsFoundation";

#ifndef CODEX_MONITOR_WINDOWS_VERSION
#define CODEX_MONITOR_WINDOWS_VERSION "0.3.0"
#endif
constexpr char kApplicationVersion[] = CODEX_MONITOR_WINDOWS_VERSION;

constexpr int kPinButtonId = 1001;
constexpr int kMinimizeButtonId = 1002;
constexpr int kHomePageButtonId = 1010;
constexpr int kCodexPageButtonId = 1011;
constexpr int kComputerPageButtonId = 1012;
constexpr int kSettingsButtonId = 1013;
constexpr int kSettingsVisibleBaseId = 3000;
constexpr int kSettingsMoveUpBaseId = 3100;
constexpr int kSettingsMoveDownBaseId = 3200;
constexpr int kSettingsTopmostId = 3300;
constexpr int kSettingsCloseId = 3301;
constexpr int kSettingsCheckUpdatesId = 3302;
constexpr int kSettingsNativeVisibleBaseId = 3400;
constexpr UINT_PTR kSampleTimerId = 2001;
constexpr UINT kSampleReadyMessage = WM_APP + 1;
constexpr UINT kCodexReadyMessage = WM_APP + 2;
constexpr UINT kServiceStatusReadyMessage = WM_APP + 3;
constexpr UINT kUpdateReadyMessage = WM_APP + 4;
constexpr UINT kFastSampleIntervalMs = 5000;
constexpr ULONGLONG kSlowSampleIntervalMs = 20000;
constexpr int kMinimumWidth = 390;
constexpr int kMinimumHeight = 360;

struct ModuleViews {
    codex_monitor::ModuleId id;
    HWND homeCard = nullptr;
    HWND nativeCard = nullptr;
};

struct SettingsRow {
    codex_monitor::ModuleId id;
    HWND nameLabel = nullptr;
    HWND homeVisibleCheck = nullptr;
    HWND nativeVisibleCheck = nullptr;
    HWND moveUpButton = nullptr;
    HWND moveDownButton = nullptr;
};

struct AppState {
    HWND mainWindow = nullptr;
    HWND heading = nullptr;
    HWND subtitle = nullptr;
    HWND status = nullptr;
    HWND pinButton = nullptr;
    HWND minimizeButton = nullptr;
    HWND homePageButton = nullptr;
    HWND codexPageButton = nullptr;
    HWND computerPageButton = nullptr;
    HWND settingsButton = nullptr;
    HWND codexNotice = nullptr;
    HWND emptyHomeNotice = nullptr;
    HWND settingsWindow = nullptr;
    HWND settingsHeading = nullptr;
    HWND settingsTopmostCheck = nullptr;
    HWND settingsUpdateStatus = nullptr;
    HWND settingsCheckUpdatesButton = nullptr;
    HWND settingsCloseButton = nullptr;
    std::vector<ModuleViews> moduleViews;
    std::vector<SettingsRow> settingsRows;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT settingsBodyFont = nullptr;
    HFONT settingsSmallFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH cardBrush = nullptr;
    bool samplingPaused = true;
    bool timerActive = false;
    bool windowMinimized = false;
    bool hasPerformanceSnapshot = false;
    bool codexPaused = true;
    bool codexWorkerAvailable = false;
    bool hasCodexRefresh = false;
    bool codexDataAvailable = false;
    bool codexLastRefreshSucceeded = false;
    bool windowHidden = true;
    bool serviceStatusPaused = true;
    bool serviceStatusWorkerAvailable = false;
    bool hasServiceStatusRefresh = false;
    bool serviceStatusLastRefreshSucceeded = false;
    bool serviceStatusShowingLastKnown = false;
    bool updateWorkerAvailable = false;
    int contentScrollRow = 0;
    int contentScrollMaximumRow = 0;
    int contentVisibleRows = 1;
    int wheelDeltaRemainder = 0;
    int settingsScrollOffset = 0;
    int settingsScrollMaximum = 0;
    int settingsWheelDeltaRemainder = 0;
    ULONGLONG nextSlowSampleTick = 0;
    std::filesystem::path settingsPath;
    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    codex_monitor::PerformanceWorker performanceWorker;
    codex_monitor::PerformanceSnapshot latestSnapshot;
    codex_monitor::codex::CodexWorker codexWorker;
    codex_monitor::codex::CodexDataState latestCodexData;
    codex_monitor::codex::AppServerRefreshReport latestCodexReport;
    std::optional<codex_monitor::codex::QuotaForecastRefresh>
        latestQuotaForecast;
    std::optional<codex_monitor::codex::CodexCostRefresh>
        latestCostHistory;
    std::chrono::seconds codexNextRefreshDelay{60};
    codex_monitor::OpenAIServiceStatusWorker serviceStatusWorker;
    std::optional<codex_monitor::OpenAIServiceStatusModel> latestServiceStatus;
    std::optional<std::chrono::system_clock::time_point>
        serviceStatusLastSuccessfulRefresh;
    std::chrono::seconds serviceStatusNextRefreshDelay{60};
    codex_monitor::update::WindowsUpdateWorker updateWorker;
    std::optional<codex_monitor::update::CompletedWindowsUpdateCheck>
        latestUpdateCheck;
    std::string availableUpdateVersion;
};

void LayoutControls(HWND window, AppState& state);
void LayoutSettingsControls(HWND window, AppState& state);
void UpdateSamplingDemand(HWND window, AppState& state);
void UpdateCodexDemand(HWND window, AppState& state);
void UpdateServiceStatusDemand(HWND window, AppState& state);
void PersistSettings(AppState& state);
void RefreshSettingsControls(AppState& state);
void RefreshUpdateControls(AppState& state);

int ScaleForDpi(HWND window, int value) {
    const UINT dpi = window ? GetDpiForWindow(window) : GetDpiForSystem();
    return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT CreateUiFont(HWND window, int pointSize, int weight) {
    const int height = -MulDiv(pointSize, static_cast<int>(GetDpiForWindow(window)), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void ApplyFont(HWND control, HFONT font) {
    if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void DeleteFonts(AppState& state) {
    if (state.headingFont) DeleteObject(state.headingFont);
    if (state.bodyFont) DeleteObject(state.bodyFont);
    if (state.smallFont) DeleteObject(state.smallFont);
    if (state.settingsBodyFont) DeleteObject(state.settingsBodyFont);
    if (state.settingsSmallFont) DeleteObject(state.settingsSmallFont);
    state.headingFont = nullptr;
    state.bodyFont = nullptr;
    state.smallFont = nullptr;
    state.settingsBodyFont = nullptr;
    state.settingsSmallFont = nullptr;
}

void RecreateFonts(HWND window, AppState& state) {
    const HFONT oldHeadingFont = state.headingFont;
    const HFONT oldBodyFont = state.bodyFont;
    const HFONT oldSmallFont = state.smallFont;
    state.headingFont = CreateUiFont(window, 17, FW_SEMIBOLD);
    state.bodyFont = CreateUiFont(window, 10, FW_MEDIUM);
    state.smallFont = CreateUiFont(window, 9, FW_NORMAL);

    ApplyFont(state.heading, state.headingFont);
    ApplyFont(state.subtitle, state.smallFont);
    ApplyFont(state.status, state.smallFont);
    ApplyFont(state.pinButton, state.smallFont);
    ApplyFont(state.minimizeButton, state.smallFont);
    ApplyFont(state.homePageButton, state.smallFont);
    ApplyFont(state.codexPageButton, state.smallFont);
    ApplyFont(state.computerPageButton, state.smallFont);
    ApplyFont(state.settingsButton, state.smallFont);
    ApplyFont(state.codexNotice, state.bodyFont);
    ApplyFont(state.emptyHomeNotice, state.bodyFont);
    for (const ModuleViews& views : state.moduleViews) {
        ApplyFont(views.homeCard, state.bodyFont);
        ApplyFont(views.nativeCard, state.bodyFont);
    }
    if (oldHeadingFont) DeleteObject(oldHeadingFont);
    if (oldBodyFont) DeleteObject(oldBodyFont);
    if (oldSmallFont) DeleteObject(oldSmallFont);
}

void RecreateSettingsFonts(HWND window, AppState& state) {
    const HFONT oldBodyFont = state.settingsBodyFont;
    const HFONT oldSmallFont = state.settingsSmallFont;
    state.settingsBodyFont = CreateUiFont(window, 10, FW_MEDIUM);
    state.settingsSmallFont = CreateUiFont(window, 9, FW_NORMAL);

    ApplyFont(state.settingsHeading, state.settingsBodyFont);
    ApplyFont(state.settingsTopmostCheck, state.settingsSmallFont);
    ApplyFont(state.settingsUpdateStatus, state.settingsSmallFont);
    ApplyFont(state.settingsCheckUpdatesButton, state.settingsSmallFont);
    ApplyFont(state.settingsCloseButton, state.settingsSmallFont);
    for (const SettingsRow& row : state.settingsRows) {
        ApplyFont(row.nameLabel, state.settingsSmallFont);
        ApplyFont(row.homeVisibleCheck, state.settingsSmallFont);
        ApplyFont(row.nativeVisibleCheck, state.settingsSmallFont);
        ApplyFont(row.moveUpButton, state.settingsSmallFont);
        ApplyFont(row.moveDownButton, state.settingsSmallFont);
    }

    if (oldBodyFont) DeleteObject(oldBodyFont);
    if (oldSmallFont) DeleteObject(oldSmallFont);
}

std::wstring FormatBytes(std::uint64_t bytes) {
    constexpr double kMebibyte = 1024.0 * 1024.0;
    constexpr double kGibibyte = 1024.0 * 1024.0 * 1024.0;
    std::wostringstream output;
    output << std::fixed << std::setprecision(1);
    if (bytes >= static_cast<std::uint64_t>(kGibibyte)) {
        output << static_cast<double>(bytes) / kGibibyte << L" GB";
    } else {
        output << static_cast<double>(bytes) / kMebibyte << L" MB";
    }
    return output.str();
}

std::wstring FormatPercent(const std::optional<double>& percent,
                           bool needsBaseline,
                           bool partial = false) {
    if (!percent) return needsBaseline ? L"waiting for next sample" : L"unavailable";
    std::wostringstream output;
    output << std::fixed << std::setprecision(1) << *percent << L"%";
    if (partial) output << L" (partial)";
    return output.str();
}

std::wstring FormatByteRatio(std::uint64_t used, std::uint64_t total) {
    std::wostringstream output;
    output << FormatBytes(used) << L" / " << FormatBytes(total);
    if (total > 0) {
        const double percent = 100.0 * static_cast<double>(used) / static_cast<double>(total);
        output << L" (" << std::fixed << std::setprecision(0)
               << std::clamp(percent, 0.0, 100.0) << L"%)";
    }
    return output.str();
}

std::wstring TruncatedProcessName(const std::wstring& rawName) {
    std::wstring name = codex_monitor::NormalizedExecutableName(rawName);
    if (name.empty()) name = L"unknown";
    constexpr std::size_t kMaximumLength = 24;
    if (name.size() > kMaximumLength) {
        name.resize(kMaximumLength - 3);
        name += L"...";
    }
    return name;
}

std::wstring SanitizeDisplayText(std::wstring_view raw, std::size_t maximumCharacters) {
    std::wstring result;
    result.reserve(std::min(raw.size(), maximumCharacters));
    bool pendingSpace = false;
    bool truncated = false;
    for (wchar_t character : raw) {
        const bool whitespace = std::iswspace(static_cast<wint_t>(character)) != 0;
        const bool control = character < 0x20 || character == 0x7f;
        if (whitespace || control) {
            pendingSpace = !result.empty();
            continue;
        }
        if (pendingSpace) {
            if (result.size() >= maximumCharacters) {
                truncated = true;
                break;
            }
            result.push_back(L' ');
            pendingSpace = false;
        }
        if (result.size() >= maximumCharacters) {
            truncated = true;
            break;
        }
        result.push_back(character);
    }

    while (!result.empty() && result.back() == L' ') result.pop_back();
    if (truncated && maximumCharacters >= 4) {
        result.resize(std::min(result.size(), maximumCharacters - 3));
        if (!result.empty() && result.back() >= 0xd800 && result.back() <= 0xdbff) {
            result.pop_back();
        }
        result += L"...";
    }
    return result;
}

std::wstring FormatTokenCount(std::int64_t value) {
    if (value < 0) return L"当前未返回";
    std::wstring digits = std::to_wstring(value);
    for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(digits.size()) - 3;
         position > 0; position -= 3) {
        digits.insert(static_cast<std::size_t>(position), 1, L',');
    }
    return digits;
}

std::wstring CurrentLocalDate() {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    wchar_t buffer[11]{};
    swprintf_s(buffer, _countof(buffer), L"%04u-%02u-%02u",
               static_cast<unsigned int>(local.wYear),
               static_cast<unsigned int>(local.wMonth),
               static_cast<unsigned int>(local.wDay));
    return buffer;
}

std::optional<std::wstring> FormatUnixLocalTime(std::int64_t seconds) {
    const __time64_t raw = static_cast<__time64_t>(seconds);
    if (static_cast<std::int64_t>(raw) != seconds) return std::nullopt;
    std::tm local{};
    if (_localtime64_s(&local, &raw) != 0) return std::nullopt;
    wchar_t buffer[32]{};
    if (std::wcsftime(buffer, _countof(buffer), L"%Y-%m-%d %H:%M", &local) == 0) {
        return std::nullopt;
    }
    return std::wstring(buffer);
}

std::optional<std::wstring> FormatLocalTime(
    std::chrono::system_clock::time_point time) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) return std::nullopt;
    wchar_t buffer[32]{};
    if (std::wcsftime(buffer, _countof(buffer), L"%Y-%m-%d %H:%M", &local) ==
        0) {
        return std::nullopt;
    }
    return std::wstring(buffer);
}

std::wstring FriendlyPlanType(std::wstring_view rawPlan) {
    const std::wstring cleaned = SanitizeDisplayText(rawPlan, 32);
    std::wstring normalized = cleaned;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](wchar_t character) {
                       return character >= L'A' && character <= L'Z'
                           ? static_cast<wchar_t>(character - L'A' + L'a')
                           : character;
                   });
    if (normalized == L"free") return L"Free";
    if (normalized == L"plus") return L"Plus";
    if (normalized == L"pro") return L"Pro";
    if (normalized == L"team") return L"Team";
    if (normalized == L"business") return L"Business";
    if (normalized == L"enterprise") return L"Enterprise";
    if (normalized == L"edu" || normalized == L"education") return L"Education";
    return cleaned.empty() ? L"当前未返回" : cleaned;
}

std::wstring CodexFailureReason(const AppState& state) {
    using Failure = codex_monitor::codex::AppServerClientFailureKind;
    if (!state.codexWorkerAvailable) return L"Codex 读取组件未能启动";
    if (state.latestCodexReport.failure) {
        switch (*state.latestCodexReport.failure) {
            case Failure::kStartFailed:
                return L"未找到或无法启动 codex.exe";
            case Failure::kInitializeRejected:
                return L"Codex 接口初始化失败";
            case Failure::kWriteFailed:
                return L"Codex 接口请求发送失败";
            case Failure::kTransportFailed:
                return L"Codex 接口连接失败";
            case Failure::kTimedOut:
                return L"Codex 接口读取超时";
            case Failure::kCancelled:
                return L"Codex 读取已暂停";
        }
    }
    if (state.codexPaused) return L"Codex 读取已暂停";
    if (state.codexWorker.IsBusy()) return L"正在连接本机 Codex 只读接口";
    return L"Codex 接口当前未返回有效数据";
}

std::wstring BuildTargetCardText(const codex_monitor::PerformanceSnapshot& snapshot) {
    std::wostringstream output;
    output << L"CODEX / CHATGPT PROCESS TREE\r\n";
    if (!snapshot.raw.processListAvailable) {
        output << L"Process list unavailable\r\n"
               << L"CPU share: unavailable\r\n"
               << L"Working set: unavailable";
        return output.str();
    }

    if (snapshot.targetRootCount == 0) {
        output << L"Not detected\r\nCPU share: 0.0%\r\nWorking set: 0.0 MB";
        return output.str();
    }

    output << L"Roots: " << snapshot.targetRootCount << L"  |  Tree: "
           << snapshot.targetProcessCount << L" process(es)\r\n";
    output << L"CPU whole-machine share: "
           << FormatPercent(snapshot.targetCpuPercent, snapshot.systemCpuNeedsBaseline,
                            snapshot.targetCpuPartial)
           << L"\r\n";
    output << L"Working set: ";
    if (snapshot.targetWorkingSetAvailable) {
        output << FormatBytes(snapshot.targetWorkingSetBytes);
        if (snapshot.targetWorkingSetPartial) output << L" (partial)";
    } else {
        output << L"unavailable";
    }
    return output.str();
}

std::wstring BuildSystemCardText(const codex_monitor::PerformanceSnapshot& snapshot) {
    std::wostringstream output;
    output << L"SYSTEM CPU & PHYSICAL MEMORY\r\n";
    output << L"CPU: "
           << FormatPercent(snapshot.systemCpuPercent, snapshot.systemCpuNeedsBaseline) << L"\r\n";
    if (snapshot.raw.physicalMemoryAvailable) {
        const std::uint64_t used =
            snapshot.raw.physicalTotalBytes >= snapshot.raw.physicalAvailableBytes
                ? snapshot.raw.physicalTotalBytes - snapshot.raw.physicalAvailableBytes
                : 0;
        output << L"Physical: " << FormatByteRatio(used, snapshot.raw.physicalTotalBytes) << L"\r\n";
        output << L"Available: " << FormatBytes(snapshot.raw.physicalAvailableBytes) << L"\r\n";
    } else {
        output << L"Physical memory: unavailable\r\n";
    }
    if (snapshot.raw.processListAvailable) {
        output << L"Processes: " << snapshot.raw.processes.size() << L"  |  WS readable: "
               << snapshot.readableWorkingSetProcessCount;
    } else {
        output << L"Process list: unavailable";
    }
    return output.str();
}

std::wstring BuildCommitCardText(const codex_monitor::PerformanceSnapshot& snapshot) {
    std::wostringstream output;
    output << L"COMMIT & PAGE FILE\r\n";
    if (snapshot.raw.commitAvailable) {
        output << L"Committed: "
               << FormatByteRatio(snapshot.raw.commitTotalBytes, snapshot.raw.commitLimitBytes)
               << L"\r\n";
        output << L"Commit peak: " << FormatBytes(snapshot.raw.commitPeakBytes) << L"\r\n";
    } else {
        output << L"Commit: unavailable\r\n";
    }
    if (snapshot.raw.pageFileAvailable) {
        output << L"Page file: "
               << FormatByteRatio(snapshot.raw.pageFileUsedBytes,
                                  snapshot.raw.pageFileTotalBytes)
               << L"\r\n";
        output << L"Page-file peak: " << FormatBytes(snapshot.raw.pageFilePeakBytes);
    } else {
        output << L"Page file: unavailable";
    }
    return output.str();
}

std::wstring BuildRankingCardText(const codex_monitor::PerformanceSnapshot& snapshot) {
    std::wostringstream output;
    output << L"TOP 5 PROCESSES BY WORKING SET\r\n";
    if (!snapshot.topMemoryRankingAvailable || snapshot.topMemoryProcesses.empty()) {
        output << L"Working-set metrics unavailable\r\n";
    } else {
        for (std::size_t index = 0; index < snapshot.topMemoryProcesses.size(); ++index) {
            const codex_monitor::RankedProcess& process = snapshot.topMemoryProcesses[index];
            output << index + 1 << L". " << TruncatedProcessName(process.executableName)
                   << L"  " << FormatBytes(process.workingSetBytes) << L"\r\n";
        }
    }
    output << L"Thermal pressure: system not provided";
    return output.str();
}

std::wstring DiagnosisPercent(const std::optional<double>& percent) {
    if (!percent) return L"--";
    std::wostringstream output;
    output << std::fixed << std::setprecision(0) << *percent << L"%";
    return output.str();
}

std::wstring_view PressureLabel(codex_monitor::SystemPressure pressure) {
    switch (pressure) {
        case codex_monitor::SystemPressure::kUnavailable:
            return L"Data unavailable";
        case codex_monitor::SystemPressure::kComfortable:
            return L"Comfortable";
        case codex_monitor::SystemPressure::kElevated:
            return L"Elevated";
        case codex_monitor::SystemPressure::kHigh:
            return L"High";
    }
    return L"Data unavailable";
}

std::wstring_view BottleneckLabel(codex_monitor::SystemBottleneck bottleneck) {
    switch (bottleneck) {
        case codex_monitor::SystemBottleneck::kUnavailable:
            return L"Unknown";
        case codex_monitor::SystemBottleneck::kNone:
            return L"None";
        case codex_monitor::SystemBottleneck::kCpu:
            return L"CPU";
        case codex_monitor::SystemBottleneck::kMemory:
            return L"Memory";
        case codex_monitor::SystemBottleneck::kMixed:
            return L"CPU + memory";
    }
    return L"Unknown";
}

std::wstring_view TargetImpactLabel(codex_monitor::TargetImpact impact) {
    switch (impact) {
        case codex_monitor::TargetImpact::kUnavailable:
            return L"Unknown";
        case codex_monitor::TargetImpact::kNotDetected:
            return L"Not detected";
        case codex_monitor::TargetImpact::kLow:
            return L"Low";
        case codex_monitor::TargetImpact::kPossible:
            return L"Possible";
        case codex_monitor::TargetImpact::kHigh:
            return L"High";
    }
    return L"Unknown";
}

std::wstring_view ConfidenceLabel(codex_monitor::DiagnosisConfidence confidence) {
    switch (confidence) {
        case codex_monitor::DiagnosisConfidence::kLow:
            return L"Low";
        case codex_monitor::DiagnosisConfidence::kMedium:
            return L"Medium";
        case codex_monitor::DiagnosisConfidence::kHigh:
            return L"High";
    }
    return L"Low";
}

std::wstring BuildDiagnosisCardText(
    const codex_monitor::PerformanceSnapshot& snapshot) {
    const codex_monitor::PerformanceDiagnosis diagnosis =
        codex_monitor::DiagnosePerformance(snapshot);
    std::wostringstream output;
    output << L"SYSTEM + CODEX/CHATGPT\r\n"
           << L"Pressure: " << PressureLabel(diagnosis.pressure)
           << L"\r\nBottleneck: " << BottleneckLabel(diagnosis.bottleneck)
           << L"  |  Impact: " << TargetImpactLabel(diagnosis.targetImpact)
           << L"\r\nCPU " << DiagnosisPercent(diagnosis.cpuPercent)
           << L"  |  RAM " << DiagnosisPercent(diagnosis.physicalMemoryPercent)
           << L"  |  Commit " << DiagnosisPercent(diagnosis.commitPercent)
           << L"\r\nConfidence: " << ConfidenceLabel(diagnosis.confidence)
           << L"  |  snapshot";
    return output.str();
}

std::wstring_view CodexModuleHeading(codex_monitor::ModuleId id) {
    switch (id) {
        case codex_monitor::ModuleId::kCodexFiveHourQuota:
            return L"CODEX 5-HOUR QUOTA";
        case codex_monitor::ModuleId::kCodexWeeklyQuota:
            return L"CODEX WEEKLY QUOTA";
        case codex_monitor::ModuleId::kCodexQuotaForecast:
            return L"CODEX QUOTA TREND FORECAST";
        case codex_monitor::ModuleId::kCodexSubscriptionType:
            return L"CODEX SUBSCRIPTION TYPE";
        case codex_monitor::ModuleId::kCodexAccountTokenUsage:
            return L"CODEX ACCOUNT TOKEN USAGE";
        case codex_monitor::ModuleId::kCodexTokenCostEstimate:
            return L"CODEX TOKEN USAGE & COST (BETA)";
        case codex_monitor::ModuleId::kCodexRecentTasks:
            return L"CODEX RECENT TASKS (HISTORY)";
        case codex_monitor::ModuleId::kOpenAIServiceStatus:
            return L"OPENAI OFFICIAL SERVICE STATUS";
        case codex_monitor::ModuleId::kSystemDiagnosis:
        case codex_monitor::ModuleId::kTargetProcessTree:
        case codex_monitor::ModuleId::kSystemResources:
        case codex_monitor::ModuleId::kCommitAndPageFile:
        case codex_monitor::ModuleId::kTopMemoryProcesses:
            return L"CODEX";
    }
    return L"CODEX";
}

std::wstring BuildCodexUnavailableText(codex_monitor::ModuleId id) {
    std::wstring detail = L"正在连接本机 Codex 只读接口\r\n当前未返回；不会模拟数值";
    if (id == codex_monitor::ModuleId::kCodexQuotaForecast) {
        detail = L"正在等待额度数据\r\n至少需要15分钟历史才能判断趋势";
    }
    if (id == codex_monitor::ModuleId::kCodexRecentTasks) {
        detail = L"正在连接本机 Codex 只读接口\r\n"
                 L"当前未返回；状态只表示 app-server 进程范围，"
                 L"不代表桌面版全局";
    }
    if (id == codex_monitor::ModuleId::kCodexTokenCostEstimate) {
        detail = L"正在读取本机 Codex Token 历史\r\n"
                 L"费用为 API 等价估算，不是订阅账单";
    }
    return std::wstring(CodexModuleHeading(id)) + L"\r\n" + detail;
}

std::wstring BuildServiceStatusCardText(const AppState& state) {
    std::wostringstream output;
    output << L"OPENAI OFFICIAL SERVICE STATUS\r\n";
    if (state.latestServiceStatus) {
        output << std::wstring(state.latestServiceStatus->headline.begin(),
                               state.latestServiceStatus->headline.end())
               << L"\r\n"
               << std::wstring(state.latestServiceStatus->detail.begin(),
                               state.latestServiceStatus->detail.end());
        if (state.serviceStatusLastSuccessfulRefresh) {
            if (const auto updated =
                    FormatLocalTime(*state.serviceStatusLastSuccessfulRefresh)) {
                output << L"\r\nUpdated: " << *updated;
            }
        }
        if (!state.serviceStatusPaused && state.serviceStatusWorker.IsBusy()) {
            output << L"\r\nRefreshing; showing last status";
        } else if (state.serviceStatusShowingLastKnown) {
            output << L"\r\nUpdate failed; showing last status";
        } else {
            output << L"\r\nRefresh cadence: 15 min";
        }
    } else if (!state.serviceStatusWorkerAvailable) {
        output << L"Status reader unavailable";
    } else if (!state.serviceStatusPaused && state.serviceStatusWorker.IsBusy()) {
        output << L"Checking the official OpenAI status page";
    } else if (state.hasServiceStatusRefresh &&
               !state.serviceStatusLastRefreshSucceeded) {
        const auto retryMinutes = std::max<std::int64_t>(
            1, std::chrono::duration_cast<std::chrono::minutes>(
                   state.serviceStatusNextRefreshDelay).count());
        output << L"Official status temporarily unavailable\r\nRetry in "
               << retryMinutes << L" min";
    } else if (state.serviceStatusPaused) {
        output << L"Refresh paused while this module is not visible";
    } else {
        output << L"Waiting for the official OpenAI status page";
    }
    output << L"\r\nSource: status.openai.com";
    return output.str();
}

const codex_monitor::codex::RateLimitWindow* SelectQuotaWindow(
    const codex_monitor::codex::RateLimitsData& limits,
    bool weekly) {
    const codex_monitor::codex::RateLimitWindow* primary =
        limits.primary ? &*limits.primary : nullptr;
    const codex_monitor::codex::RateLimitWindow* secondary =
        limits.secondary ? &*limits.secondary : nullptr;

    for (const codex_monitor::codex::RateLimitWindow* candidate :
         {primary, secondary}) {
        if (!candidate || !candidate->windowDurationMinutes) continue;
        const bool candidateIsWeekly = *candidate->windowDurationMinutes > 1440;
        if (candidateIsWeekly == weekly) return candidate;
    }

    // Older responses may omit duration. In that case the documented primary
    // slot is treated as the short window and secondary as the weekly window.
    const codex_monitor::codex::RateLimitWindow* fallback = weekly ? secondary : primary;
    return fallback && !fallback->windowDurationMinutes ? fallback : nullptr;
}

template <typename T>
void AppendMethodRefreshWarning(
    const codex_monitor::codex::MethodState<T>& method,
    std::wostringstream& output) {
    if (method.lastFailure && method.lastValue) {
        output << L"\r\n更新失败，显示上次数据";
    }
}

std::wstring BuildQuotaCardText(const AppState& state, bool weekly) {
    const auto& method = state.latestCodexData.rateLimits;
    std::wostringstream output;
    output << CodexModuleHeading(
        weekly ? codex_monitor::ModuleId::kCodexWeeklyQuota
               : codex_monitor::ModuleId::kCodexFiveHourQuota);
    if (!method.lastValue) {
        output << L"\r\n" << CodexFailureReason(state);
        return output.str();
    }

    const codex_monitor::codex::RateLimitWindow* window =
        SelectQuotaWindow(*method.lastValue, weekly);
    if (!window) {
        output << L"\r\n当前未返回";
        if (method.lastFailure) output << L"；本次更新失败";
        return output.str();
    }

    const int used = std::clamp(static_cast<int>(window->usedPercent), 0, 100);
    output << L"\r\n剩余 " << 100 - used << L"%";
    if (window->resetsAtUnixSeconds) {
        if (const auto reset = FormatUnixLocalTime(*window->resetsAtUnixSeconds)) {
            output << L"\r\n恢复：" << *reset;
        } else {
            output << L"\r\n恢复时间：当前未返回";
        }
    } else {
        output << L"\r\n恢复时间：当前未返回";
    }
    AppendMethodRefreshWarning(method, output);
    return output.str();
}

std::wstring_view ForecastConfidenceLabel(
    codex_monitor::codex::QuotaForecastConfidence confidence) {
    switch (confidence) {
        case codex_monitor::codex::QuotaForecastConfidence::kLow:
            return L"低";
        case codex_monitor::codex::QuotaForecastConfidence::kMedium:
            return L"中";
        case codex_monitor::codex::QuotaForecastConfidence::kHigh:
            return L"高";
        case codex_monitor::codex::QuotaForecastConfidence::kUnavailable:
            return L"";
    }
    return L"";
}

void AppendQuotaForecastLine(
    std::wostringstream& output,
    std::wstring_view label,
    const codex_monitor::codex::QuotaWindowForecastRefresh& window) {
    using codex_monitor::codex::QuotaForecastState;
    using codex_monitor::codex::QuotaForecastUnavailableReason;

    output << L"\r\n" << label << L"：";
    if (!window.windowReturned) {
        output << L"当前未返回";
        return;
    }

    const auto& forecast = window.forecast;
    switch (forecast.state) {
        case QuotaForecastState::kUnavailable:
            if (forecast.unavailableReason ==
                QuotaForecastUnavailableReason::kInsufficientHistory) {
                output << L"正在积累历史（至少15分钟）";
            } else if (forecast.unavailableReason ==
                       QuotaForecastUnavailableReason::kInvalidCurrentState) {
                output << L"恢复时间未返回";
            } else {
                output << L"暂时无法计算";
            }
            return;
        case QuotaForecastState::kStable:
            output << L"近期平稳";
            break;
        case QuotaForecastState::kLastsToReset: {
            const int projected = std::clamp(
                static_cast<int>(std::lround(
                    forecast.projectedRemainingAtResetPercent.value_or(0.0))),
                0, 100);
            output << L"可撑到恢复，预计剩余 " << projected << L"%";
            break;
        }
        case QuotaForecastState::kMayExhaustEarly:
            output << L"可能提前用完";
            if (forecast.projectedExhaustAtUnixSeconds) {
                const auto exhaustAt = static_cast<std::int64_t>(
                    std::llround(*forecast.projectedExhaustAtUnixSeconds));
                if (const auto localTime = FormatUnixLocalTime(exhaustAt)) {
                    output << L" · " << *localTime;
                }
            }
            break;
    }
    const std::wstring_view confidence =
        ForecastConfidenceLabel(forecast.confidence);
    if (!confidence.empty()) output << L" · " << confidence << L"置信";
}

std::wstring BuildQuotaForecastCardText(const AppState& state) {
    std::wostringstream output;
    output << CodexModuleHeading(
        codex_monitor::ModuleId::kCodexQuotaForecast);
    if (!state.latestQuotaForecast) {
        if (!state.latestCodexData.rateLimits.lastValue) {
            output << L"\r\n" << CodexFailureReason(state);
        } else {
            output << L"\r\n等待下一次额度更新";
        }
        output << L"\r\n至少需要15分钟历史才能判断趋势";
        return output.str();
    }

    AppendQuotaForecastLine(output, L"5小时", state.latestQuotaForecast->fiveHour);
    AppendQuotaForecastLine(output, L"每周", state.latestQuotaForecast->weekly);
    if (state.latestQuotaForecast->historySaveFailed) {
        output << L"\r\n历史保存失败；趋势可能无法继续积累";
    } else if (state.latestCodexData.rateLimits.lastFailure) {
        output << L"\r\n更新失败，显示上次预测";
    }
    return output.str();
}

std::wstring BuildSubscriptionCardText(const AppState& state) {
    std::optional<std::wstring> plan;
    bool displayingStaleData = false;
    if (state.latestCodexData.account.lastValue &&
        state.latestCodexData.account.lastValue->planType) {
        plan = state.latestCodexData.account.lastValue->planType;
        displayingStaleData = state.latestCodexData.account.lastFailure.has_value();
    } else if (state.latestCodexData.rateLimits.lastValue &&
               state.latestCodexData.rateLimits.lastValue->planType) {
        plan = state.latestCodexData.rateLimits.lastValue->planType;
        displayingStaleData = state.latestCodexData.rateLimits.lastFailure.has_value();
    }

    std::wostringstream output;
    output << CodexModuleHeading(codex_monitor::ModuleId::kCodexSubscriptionType);
    if (!plan) {
        output << L"\r\n" << CodexFailureReason(state);
        return output.str();
    }
    output << L"\r\n订阅：" << FriendlyPlanType(*plan);
    if (displayingStaleData) output << L"\r\n更新失败，显示上次数据";
    return output.str();
}

std::wstring BuildTokenUsageCardText(const AppState& state) {
    const auto& method = state.latestCodexData.usage;
    std::wostringstream output;
    output << CodexModuleHeading(codex_monitor::ModuleId::kCodexAccountTokenUsage);
    if (!method.lastValue) {
        output << L"\r\n" << CodexFailureReason(state);
        return output.str();
    }

    const auto totals = codex_monitor::codex::CalculateUsageCalendarTotals(
        *method.lastValue, CurrentLocalDate());
    if (!totals || !totals->sourceAvailable) {
        output << L"\r\n当前未返回每日 Token 记录";
        AppendMethodRefreshWarning(method, output);
        return output.str();
    }
    if (!totals->latestDate || !totals->latestTokens) {
        output << L"\r\n当前没有可显示的每日 Token 记录";
        AppendMethodRefreshWarning(method, output);
        return output.str();
    }

    if (totals->todayAvailable && totals->todayTokens) {
        output << L"\r\n今日：" << FormatTokenCount(*totals->todayTokens);
    } else {
        output << L"\r\n最新 " << *totals->latestDate << L"："
               << FormatTokenCount(*totals->latestTokens);
    }
    output << L"\r\n近7日：" << FormatTokenCount(totals->last7DaysTokens)
           << L"  |  近30日：" << FormatTokenCount(totals->thirtyDayTokens);
    output << L"\r\n本月：" << FormatTokenCount(totals->monthToDateTokens);
    if (totals->monthForecastTokens) {
        output << L"  |  月末约：" << FormatTokenCount(*totals->monthForecastTokens);
    } else {
        output << L"  |  月末约：当前数据不足";
    }
    if (totals->saturated) output << L"（达到上限，非精确）";
    AppendMethodRefreshWarning(method, output);
    return output.str();
}

std::wstring FormatEstimatedUsd(double value) {
    if (!std::isfinite(value) || value < 0.0) return L"--";
    std::wostringstream output;
    output << L'$' << std::fixed
           << std::setprecision(value > 0.0 && value < 0.01 ? 4 : 2)
           << value;
    return output.str();
}

void AppendHybridCostPeriod(
    std::wostringstream& output,
    std::wstring_view label,
    const codex_monitor::codex::CodexHybridPeriodSummary& period) {
    output << label << L'：';
    if (!period.tokensAvailable) {
        output << L"--";
        return;
    }
    output << FormatTokenCount(period.tokens) << L" · ";
    if (period.estimatedUsd) {
        output << FormatEstimatedUsd(*period.estimatedUsd);
    } else {
        output << L"费用样本不足";
    }
}

std::wstring BuildTokenCostCardText(const AppState& state) {
    codex_monitor::codex::UsageCalendarTotals official;
    if (state.latestCodexData.usage.lastValue) {
        const auto calculated = codex_monitor::codex::CalculateUsageCalendarTotals(
            *state.latestCodexData.usage.lastValue, CurrentLocalDate());
        if (calculated) official = *calculated;
    }

    codex_monitor::codex::CodexCostSummary local;
    if (state.latestCostHistory && state.latestCostHistory->localSummary) {
        local = *state.latestCostHistory->localSummary;
    }
    const codex_monitor::codex::CodexCostHybridSummary summary =
        codex_monitor::codex::CalculateCodexCostHybridSummary(official, local);

    std::wostringstream output;
    output << CodexModuleHeading(
        codex_monitor::ModuleId::kCodexTokenCostEstimate);
    if (!summary.available) {
        output << L"\r\n";
        if (!state.latestCostHistory) {
            output << L"正在读取本机 Codex Token 历史";
        } else {
            switch (state.latestCostHistory->status) {
                case codex_monitor::codex::CodexCostRefreshStatus::kCodexHomeUnavailable:
                    output << L"Codex 当前未返回本机数据目录";
                    break;
                case codex_monitor::codex::CodexCostRefreshStatus::kScanFailed:
                    output << L"本机 Token 历史读取失败";
                    break;
                case codex_monitor::codex::CodexCostRefreshStatus::kNoTokenEvents:
                    output << L"本机记录暂时没有 Token 数据";
                    break;
                case codex_monitor::codex::CodexCostRefreshStatus::kAvailable:
                    output << L"当前没有可显示的 Token 数据";
                    break;
            }
        }
        output << L"\r\n费用将在取得模型样本后估算";
        return output.str();
    }

    output << L"\r\n";
    AppendHybridCostPeriod(output, L"近30日", summary.last30Days);
    output << L"\r\n";
    AppendHybridCostPeriod(output, L"今日", summary.today);
    output << L"  |  ";
    AppendHybridCostPeriod(output, L"近7日", summary.last7Days);
    output << L"\r\n";
    AppendHybridCostPeriod(output, L"本月", summary.monthToDate);
    output << L"  |  月末约：";
    if (summary.monthForecastEstimatedUsd) {
        output << FormatEstimatedUsd(*summary.monthForecastEstimatedUsd);
    } else {
        output << L"数据不足";
    }

    if (local.available) {
        output << L"\r\n本机计价样本覆盖：" << std::fixed << std::setprecision(0)
               << summary.pricedTokenPercent << L'%';
        if (!summary.topModel.empty()) {
            output << L"  |  主模型："
                   << std::wstring(summary.topModel.begin(), summary.topModel.end());
        }
    }
    if (state.latestCostHistory) {
        if (state.latestCostHistory->coverageIncomplete) {
            output << L"\r\n本机历史仍在补齐";
        }
        if (state.latestCostHistory->skippedCompressedFiles > 0) {
            output << L"\r\n压缩历史暂未读取";
        }
        if (state.latestCostHistory->showingLastKnown) {
            output << L"\r\n本次读取失败，显示上次历史";
        }
        if (state.latestCostHistory->historyCacheSaveFailed) {
            output << L"\r\n历史缓存保存失败；下次启动将重新扫描";
        }
    }
    if (summary.saturated) output << L"\r\n数值达到显示上限";
    const auto UsesOfficial = [](const auto& period) {
        return period.tokensAvailable && period.usedOfficialTokens;
    };
    const auto UsesLocal = [](const auto& period) {
        return period.tokensAvailable && !period.usedOfficialTokens;
    };
    const bool anyOfficial =
        UsesOfficial(summary.today) || UsesOfficial(summary.last7Days) ||
        UsesOfficial(summary.last30Days) ||
        UsesOfficial(summary.monthToDate);
    const bool anyLocal =
        UsesLocal(summary.today) || UsesLocal(summary.last7Days) ||
        UsesLocal(summary.last30Days) || UsesLocal(summary.monthToDate);
    if (anyOfficial && anyLocal) {
        output << L"\r\nToken：官方优先，缺失周期用本机样本"
                  L" · 费用：本机样本API等价估算";
    } else if (anyOfficial) {
        output << L"\r\nToken：官方汇总 · 费用：本机样本API等价估算";
    } else {
        output << L"\r\nToken与费用：本机样本API等价估算";
    }
    return output.str();
}

std::wstring BuildRecentTasksCardText(const AppState& state) {
    const auto& method = state.latestCodexData.threadList;
    std::wostringstream output;
    output << CodexModuleHeading(codex_monitor::ModuleId::kCodexRecentTasks);
    if (!method.lastValue) {
        output << L"\r\n" << CodexFailureReason(state);
        output << L"\r\n状态口径：仅本次 app-server 进程范围";
        return output.str();
    }

    const std::size_t count = std::min<std::size_t>(3, method.lastValue->threads.size());
    if (count == 0) {
        output << L"\r\n暂无历史任务";
    } else {
        for (std::size_t index = 0; index < count; ++index) {
            const codex_monitor::codex::ProcessLocalThread& task =
                method.lastValue->threads[index];
            std::wstring name = task.name
                ? SanitizeDisplayText(*task.name, 42)
                : std::wstring{};
            if (name.empty()) name = L"未命名任务";
            output << L"\r\n" << index + 1 << L". " << name;
            if (task.recencyAtUnixSeconds) {
                if (const auto updated = FormatUnixLocalTime(*task.recencyAtUnixSeconds)) {
                    output << L" · " << *updated;
                }
            }
        }
    }
    output << L"\r\n状态口径：仅本次 app-server 进程范围";
    AppendMethodRefreshWarning(method, output);
    return output.str();
}

std::wstring BuildCodexCardText(codex_monitor::ModuleId id,
                                const AppState& state) {
    switch (id) {
        case codex_monitor::ModuleId::kCodexFiveHourQuota:
            return BuildQuotaCardText(state, false);
        case codex_monitor::ModuleId::kCodexWeeklyQuota:
            return BuildQuotaCardText(state, true);
        case codex_monitor::ModuleId::kCodexQuotaForecast:
            return BuildQuotaForecastCardText(state);
        case codex_monitor::ModuleId::kCodexSubscriptionType:
            return BuildSubscriptionCardText(state);
        case codex_monitor::ModuleId::kCodexAccountTokenUsage:
            return BuildTokenUsageCardText(state);
        case codex_monitor::ModuleId::kCodexTokenCostEstimate:
            return BuildTokenCostCardText(state);
        case codex_monitor::ModuleId::kCodexRecentTasks:
            return BuildRecentTasksCardText(state);
        case codex_monitor::ModuleId::kOpenAIServiceStatus:
            return BuildServiceStatusCardText(state);
        case codex_monitor::ModuleId::kSystemDiagnosis:
        case codex_monitor::ModuleId::kTargetProcessTree:
        case codex_monitor::ModuleId::kSystemResources:
        case codex_monitor::ModuleId::kCommitAndPageFile:
        case codex_monitor::ModuleId::kTopMemoryProcesses:
            return L"Unavailable module";
    }
    return L"Unavailable module";
}

std::wstring BuildModuleCardText(codex_monitor::ModuleId id,
    const codex_monitor::PerformanceSnapshot& snapshot) {
    switch (id) {
        case codex_monitor::ModuleId::kSystemDiagnosis:
            return BuildDiagnosisCardText(snapshot);
        case codex_monitor::ModuleId::kTargetProcessTree:
            return BuildTargetCardText(snapshot);
        case codex_monitor::ModuleId::kSystemResources:
            return BuildSystemCardText(snapshot);
        case codex_monitor::ModuleId::kCommitAndPageFile:
            return BuildCommitCardText(snapshot);
        case codex_monitor::ModuleId::kTopMemoryProcesses:
            return BuildRankingCardText(snapshot);
        case codex_monitor::ModuleId::kCodexFiveHourQuota:
        case codex_monitor::ModuleId::kCodexWeeklyQuota:
        case codex_monitor::ModuleId::kCodexQuotaForecast:
        case codex_monitor::ModuleId::kCodexSubscriptionType:
        case codex_monitor::ModuleId::kCodexAccountTokenUsage:
        case codex_monitor::ModuleId::kCodexTokenCostEstimate:
        case codex_monitor::ModuleId::kCodexRecentTasks:
            return BuildCodexUnavailableText(id);
        case codex_monitor::ModuleId::kOpenAIServiceStatus:
            return L"OPENAI OFFICIAL SERVICE STATUS\r\nWaiting for official status";
    }
    return L"Unavailable module";
}

void UpdateModuleCards(AppState& state) {
    for (const ModuleViews& views : state.moduleViews) {
        const std::size_t index = codex_monitor::ModuleIndex(views.id);
        const codex_monitor::ModuleDefinition& definition =
            codex_monitor::ModuleRegistry()[index];
        if (!definition.requiresPerformanceSampling) continue;
        const std::wstring text = BuildModuleCardText(views.id, state.latestSnapshot);
        if (state.settings.currentPage == codex_monitor::Page::kHome &&
            state.settings.homeVisible[index]) {
            SetWindowTextW(views.homeCard, text.c_str());
        }
        if (state.settings.currentPage == definition.nativePage &&
            state.settings.nativePageVisible[index]) {
            SetWindowTextW(views.nativeCard, text.c_str());
        }
    }
}

void UpdateCodexCards(AppState& state) {
    for (const ModuleViews& views : state.moduleViews) {
        const codex_monitor::ModuleDefinition& definition =
            codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(views.id)];
        if (!definition.requiresCodexData) continue;
        const std::wstring text = BuildCodexCardText(views.id, state);
        SetWindowTextW(views.homeCard, text.c_str());
        SetWindowTextW(views.nativeCard, text.c_str());
    }
}

void UpdateServiceStatusCards(AppState& state) {
    const std::wstring text = BuildServiceStatusCardText(state);
    for (const ModuleViews& views : state.moduleViews) {
        const codex_monitor::ModuleDefinition& definition =
            codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(views.id)];
        if (!definition.requiresServiceStatus) continue;
        SetWindowTextW(views.homeCard, text.c_str());
        SetWindowTextW(views.nativeCard, text.c_str());
    }
}

bool NativePageNeedsPerformanceData(const codex_monitor::SettingsState& settings,
                                    codex_monitor::Page page) {
    for (codex_monitor::ModuleId id :
         codex_monitor::VisibleModulesForNativePage(settings, page)) {
        if (codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)]
                .requiresPerformanceSampling) return true;
    }
    return false;
}

bool NativePageNeedsCodexData(const codex_monitor::SettingsState& settings,
                              codex_monitor::Page page) {
    for (codex_monitor::ModuleId id :
         codex_monitor::VisibleModulesForNativePage(settings, page)) {
        if (codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)]
                .requiresCodexData) return true;
    }
    return false;
}

bool NativePageNeedsServiceStatus(const codex_monitor::SettingsState& settings,
                                  codex_monitor::Page page) {
    for (codex_monitor::ModuleId id :
         codex_monitor::VisibleModulesForNativePage(settings, page)) {
        if (codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)]
                .requiresServiceStatus) {
            return true;
        }
    }
    return false;
}

bool CurrentPageNeedsPerformance(const AppState& state) {
    switch (state.settings.currentPage) {
        case codex_monitor::Page::kHome:
            return codex_monitor::HomeNeedsPerformanceData(state.settings);
        case codex_monitor::Page::kCodex:
            return false;
        case codex_monitor::Page::kComputer:
            return NativePageNeedsPerformanceData(
                state.settings, codex_monitor::Page::kComputer);
    }
    return false;
}

bool CurrentPageNeedsCodexData(const AppState& state) {
    switch (state.settings.currentPage) {
        case codex_monitor::Page::kHome:
            return codex_monitor::HomeNeedsCodexData(state.settings);
        case codex_monitor::Page::kCodex:
            return NativePageNeedsCodexData(state.settings, codex_monitor::Page::kCodex);
        case codex_monitor::Page::kComputer:
            return false;
    }
    return false;
}

bool CurrentPageShowsQuotaForecast(const AppState& state) {
    const std::size_t index = codex_monitor::ModuleIndex(
        codex_monitor::ModuleId::kCodexQuotaForecast);
    switch (state.settings.currentPage) {
        case codex_monitor::Page::kHome:
            return state.settings.homeVisible[index];
        case codex_monitor::Page::kCodex:
            return state.settings.nativePageVisible[index];
        case codex_monitor::Page::kComputer:
            return false;
    }
    return false;
}

bool CurrentPageShowsCostHistory(const AppState& state) {
    const std::size_t index = codex_monitor::ModuleIndex(
        codex_monitor::ModuleId::kCodexTokenCostEstimate);
    switch (state.settings.currentPage) {
        case codex_monitor::Page::kHome:
            return state.settings.homeVisible[index];
        case codex_monitor::Page::kCodex:
            return state.settings.nativePageVisible[index];
        case codex_monitor::Page::kComputer:
            return false;
    }
    return false;
}

bool CurrentPageNeedsServiceStatus(const AppState& state) {
    switch (state.settings.currentPage) {
        case codex_monitor::Page::kHome:
            return codex_monitor::HomeNeedsServiceStatus(state.settings);
        case codex_monitor::Page::kCodex:
            return NativePageNeedsServiceStatus(
                state.settings, codex_monitor::Page::kCodex);
        case codex_monitor::Page::kComputer:
            return false;
    }
    return false;
}

void UpdateStatusText(AppState& state) {
    std::wostringstream output;
    output << L"Top: " << (state.settings.alwaysOnTop ? L"On" : L"Off");
    const bool needsPerformance = CurrentPageNeedsPerformance(state);
    const bool needsCodex = CurrentPageNeedsCodexData(state);
    const bool needsService = CurrentPageNeedsServiceStatus(state);
    if (needsPerformance) {
        output << L"  |  System: ";
        if (state.windowMinimized) {
            output << L"paused";
        } else if (state.performanceWorker.IsBusy()) {
            output << L"refreshing";
        } else if (state.timerActive) {
            output << L"5 s / 20 s";
        } else {
            output << L"unavailable";
        }
        if (state.hasPerformanceSnapshot &&
            state.latestSnapshot.unreadableProcessMetricCount > 0) {
            output << L" (" << state.latestSnapshot.unreadableProcessMetricCount
                   << L" metric gaps)";
        }
    }
    if (needsCodex) {
        output << L"  |  Codex: ";
        if (state.windowMinimized || state.codexPaused) {
            output << L"paused";
        } else if (!state.codexWorkerAvailable) {
            output << L"unavailable";
        } else if (state.codexWorker.IsBusy()) {
            output << L"refreshing";
        } else if (!state.hasCodexRefresh) {
            output << L"waiting";
        } else if (!state.codexLastRefreshSucceeded) {
            const auto retryMinutes = std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::minutes>(
                       state.codexNextRefreshDelay).count());
            output << retryMinutes << L" min retry";
        } else {
            output << L"5 min";
        }
    }
    if (needsService) {
        output << L"  |  Service: ";
        if (state.windowMinimized || state.windowHidden ||
            state.serviceStatusPaused) {
            output << L"paused";
        } else if (!state.serviceStatusWorkerAvailable) {
            output << L"unavailable";
        } else if (state.serviceStatusWorker.IsBusy()) {
            output << L"refreshing";
        } else if (!state.hasServiceStatusRefresh) {
            output << L"waiting";
        } else if (!state.serviceStatusLastRefreshSucceeded) {
            const auto retryMinutes = std::max<std::int64_t>(
                1, std::chrono::duration_cast<std::chrono::minutes>(
                       state.serviceStatusNextRefreshDelay).count());
            output << retryMinutes << L" min retry";
        } else {
            output << L"15 min";
        }
    }
    if (!needsPerformance && !needsCodex && !needsService) {
        output << L"  |  No active data modules";
    }
    if (!state.availableUpdateVersion.empty()) {
        output << L"  |  Update: "
               << std::wstring(state.availableUpdateVersion.begin(),
                               state.availableUpdateVersion.end());
    }
    SetWindowTextW(state.status, output.str().c_str());
}

void PersistSettings(AppState& state) {
    codex_monitor::SaveSettingsFile(state.settingsPath, state.settings);
}

void UpdateTopmostState(HWND window, AppState& state, bool enabled, bool persist) {
    state.settings.alwaysOnTop = enabled;
    SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowTextW(state.pinButton, enabled ? L"Unpin" : L"Pin on top");
    if (state.settingsTopmostCheck) {
        SendMessageW(state.settingsTopmostCheck, BM_SETCHECK,
                     enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    UpdateStatusText(state);
    if (persist) PersistSettings(state);
}

void StopSamplingTimer(HWND window, AppState& state) {
    if (state.timerActive) KillTimer(window, kSampleTimerId);
    state.timerActive = false;
}

void StartSamplingTimer(HWND window, AppState& state) {
    if (state.samplingPaused) return;
    state.timerActive = SetTimer(window, kSampleTimerId, kFastSampleIntervalMs, nullptr) != 0;
    UpdateStatusText(state);
}

void ShowSamplingRefreshState(AppState& state) {
    constexpr wchar_t message[] =
        L"REFRESHING PERFORMANCE DATA\r\nWaiting for background sample";
    for (const ModuleViews& views : state.moduleViews) {
        const std::size_t index = codex_monitor::ModuleIndex(views.id);
        const codex_monitor::ModuleDefinition& definition =
            codex_monitor::ModuleRegistry()[index];
        if (!definition.requiresPerformanceSampling) continue;
        if (state.settings.currentPage == codex_monitor::Page::kHome &&
            state.settings.homeVisible[index]) {
            SetWindowTextW(views.homeCard, message);
        }
        if (state.settings.currentPage == definition.nativePage &&
            state.settings.nativePageVisible[index]) {
            SetWindowTextW(views.nativeCard, message);
        }
    }
}

void ApplyPerformanceSnapshot(AppState& state, codex_monitor::CompletedSample completed) {
    state.latestSnapshot = std::move(completed.snapshot);
    state.hasPerformanceSnapshot = true;
    if (completed.mode == codex_monitor::SampleMode::kFastAndSlow) {
        state.nextSlowSampleTick = GetTickCount64() + kSlowSampleIntervalMs;
    }
    UpdateModuleCards(state);
    UpdateStatusText(state);
}

void ApplyCodexRefresh(
    AppState& state,
    codex_monitor::codex::CompletedCodexRefresh completed) {
    state.latestCodexData = std::move(completed.data);
    state.latestCodexReport = std::move(completed.report);
    if (completed.quotaForecastUpdate) {
        state.latestQuotaForecast =
            std::move(completed.quotaForecastUpdate);
    }
    if (completed.costHistoryUpdate) {
        state.latestCostHistory =
            std::move(completed.costHistoryUpdate);
    }
    state.codexLastRefreshSucceeded = completed.succeeded;
    state.codexNextRefreshDelay = completed.nextRefreshDelay;
    state.hasCodexRefresh = true;
    state.codexDataAvailable =
        state.latestCodexData.rateLimits.lastValue.has_value() ||
        state.latestCodexData.account.lastValue.has_value() ||
        state.latestCodexData.usage.lastValue.has_value() ||
        state.latestCodexData.threadList.lastValue.has_value();
    UpdateCodexCards(state);
    UpdateStatusText(state);
}

void ApplyServiceStatusRefresh(
    AppState& state,
    codex_monitor::CompletedServiceStatusRefresh completed) {
    state.latestServiceStatus = std::move(completed.status);
    state.serviceStatusLastSuccessfulRefresh = completed.lastSuccessfulRefresh;
    state.serviceStatusLastRefreshSucceeded = completed.succeeded;
    state.serviceStatusShowingLastKnown = completed.showingLastKnown;
    state.serviceStatusNextRefreshDelay = completed.nextRefreshDelay;
    state.hasServiceStatusRefresh = true;
    UpdateServiceStatusCards(state);
    UpdateStatusText(state);
}

void ApplyWindowsUpdateCheck(
    AppState& state,
    codex_monitor::update::CompletedWindowsUpdateCheck completed) {
    state.availableUpdateVersion = completed.availableVersion;
    state.latestUpdateCheck = std::move(completed);
    RefreshUpdateControls(state);
    UpdateStatusText(state);
}

void UpdateSamplingDemand(HWND window, AppState& state) {
    const bool shouldSample = !state.windowMinimized && CurrentPageNeedsPerformance(state);
    if (!shouldSample) {
        StopSamplingTimer(window, state);
        if (!state.samplingPaused) {
            state.performanceWorker.PauseAndInvalidate();
            state.hasPerformanceSnapshot = false;
        }
        state.samplingPaused = true;
        UpdateStatusText(state);
        return;
    }

    if (!state.samplingPaused && state.timerActive) {
        UpdateStatusText(state);
        return;
    }

    state.samplingPaused = false;
    state.hasPerformanceSnapshot = false;
    state.nextSlowSampleTick = 0;
    ShowSamplingRefreshState(state);
    state.performanceWorker.ActivateAndRequestFullSample();
    StartSamplingTimer(window, state);
}

void UpdateCodexDemand(HWND, AppState& state) {
    const bool forecastEnabled = !state.windowMinimized && !state.windowHidden &&
                                 CurrentPageShowsQuotaForecast(state);
    bool costEnabled = false;
    bool costDemandChanged = false;
    if (state.codexWorkerAvailable) {
        state.codexWorker.SetQuotaForecastEnabled(forecastEnabled);
        costEnabled = !state.windowMinimized && !state.windowHidden &&
                      CurrentPageShowsCostHistory(state);
        costDemandChanged =
            state.codexWorker.SetCostHistoryEnabled(costEnabled);
    }
    const bool shouldRefresh =
        !state.windowMinimized && !state.windowHidden &&
        CurrentPageNeedsCodexData(state);
    if (!shouldRefresh) {
        if (!state.codexPaused && state.codexWorkerAvailable) {
            state.codexWorker.PauseAndInvalidate();
        }
        state.codexPaused = true;
        UpdateStatusText(state);
        return;
    }

    if (!state.codexWorkerAvailable) {
        state.codexPaused = false;
        UpdateCodexCards(state);
        UpdateStatusText(state);
        return;
    }
    if (!state.codexPaused) {
        // When another Codex card already keeps the worker active, enabling
        // the cost card must not wait for the next five-minute refresh. This
        // is a one-time user-triggered refresh, not an extra periodic poll.
        if (costEnabled && costDemandChanged) {
            state.codexWorker.RequestRefresh();
        }
        UpdateStatusText(state);
        return;
    }

    state.codexPaused = false;
    state.codexWorker.ActivateAndRefresh();
    if (!state.codexDataAvailable) UpdateCodexCards(state);
    UpdateStatusText(state);
}

void UpdateServiceStatusDemand(HWND, AppState& state) {
    const bool shouldRefresh = !state.windowMinimized && !state.windowHidden &&
                               CurrentPageNeedsServiceStatus(state);
    if (!shouldRefresh) {
        if (!state.serviceStatusPaused && state.serviceStatusWorkerAvailable) {
            state.serviceStatusWorker.PauseAndInvalidate();
        }
        state.serviceStatusPaused = true;
        UpdateServiceStatusCards(state);
        UpdateStatusText(state);
        return;
    }

    if (!state.serviceStatusWorkerAvailable) {
        state.serviceStatusPaused = false;
        UpdateServiceStatusCards(state);
        UpdateStatusText(state);
        return;
    }
    if (!state.serviceStatusPaused) {
        UpdateStatusText(state);
        return;
    }

    state.serviceStatusPaused = false;
    state.serviceStatusWorker.ActivateAndRefresh();
    UpdateServiceStatusCards(state);
    UpdateStatusText(state);
}

HWND CreateLabel(HWND parent, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

HWND CreateButton(HWND parent, const wchar_t* text, int id, DWORD extraStyle = 0) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extraStyle,
                           0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

ModuleViews* FindModuleViews(AppState& state, codex_monitor::ModuleId id) {
    const auto found = std::find_if(
        state.moduleViews.begin(), state.moduleViews.end(),
        [id](const ModuleViews& views) { return views.id == id; });
    return found == state.moduleViews.end() ? nullptr : &*found;
}

SettingsRow* FindSettingsRow(AppState& state, codex_monitor::ModuleId id) {
    const auto found = std::find_if(
        state.settingsRows.begin(), state.settingsRows.end(),
        [id](const SettingsRow& row) { return row.id == id; });
    return found == state.settingsRows.end() ? nullptr : &*found;
}

std::vector<HWND> VisiblePageCards(AppState& state) {
    std::vector<HWND> cards;
    if (state.settings.currentPage == codex_monitor::Page::kHome) {
        for (codex_monitor::ModuleId id : codex_monitor::VisibleHomeModules(state.settings)) {
            if (ModuleViews* views = FindModuleViews(state, id)) cards.push_back(views->homeCard);
        }
    } else {
        for (codex_monitor::ModuleId id : codex_monitor::VisibleModulesForNativePage(
                 state.settings, state.settings.currentPage)) {
            if (ModuleViews* views = FindModuleViews(state, id)) {
                cards.push_back(views->nativeCard);
            }
        }
    }
    return cards;
}

void UpdatePageVisibility(AppState& state) {
    const bool isHome = state.settings.currentPage == codex_monitor::Page::kHome;
    const bool isCodex = state.settings.currentPage == codex_monitor::Page::kCodex;
    const std::vector<codex_monitor::ModuleId> visibleHome =
        codex_monitor::VisibleHomeModules(state.settings);
    const std::vector<codex_monitor::ModuleId> visibleNative =
        codex_monitor::VisibleModulesForNativePage(
            state.settings, state.settings.currentPage);

    for (ModuleViews& views : state.moduleViews) {
        const bool homeVisible = isHome &&
            std::find(visibleHome.begin(), visibleHome.end(), views.id) != visibleHome.end();
        const bool nativeVisible = !isHome &&
            std::find(visibleNative.begin(), visibleNative.end(), views.id) !=
                visibleNative.end();
        ShowWindow(views.homeCard, homeVisible ? SW_SHOW : SW_HIDE);
        ShowWindow(views.nativeCard, nativeVisible ? SW_SHOW : SW_HIDE);
    }

    const bool nativeEmpty = !isHome && visibleNative.empty();
    if (nativeEmpty && isCodex) {
        SetWindowTextW(
            state.codexNotice,
            L"Codex 页面当前未选择任何模块。\r\n\r\n"
            L"请打开设置，启用一个或多个 Codex 模块。");
    } else if (nativeEmpty) {
        SetWindowTextW(state.codexNotice,
                       L"No modules are shown on Computer.\r\n\r\n"
                       L"Open Settings and enable one or more modules for its own page.");
    }
    ShowWindow(state.codexNotice, nativeEmpty ? SW_SHOW : SW_HIDE);
    const bool homeEmpty = isHome && visibleHome.empty();
    ShowWindow(state.emptyHomeNotice, homeEmpty ? SW_SHOW : SW_HIDE);

    CheckRadioButton(state.mainWindow, kHomePageButtonId, kComputerPageButtonId,
                     isHome ? kHomePageButtonId
                            : (isCodex ? kCodexPageButtonId : kComputerPageButtonId));

    if (isHome) {
        SetWindowTextW(state.subtitle, L"Home - selected performance and Codex modules");
    } else if (isCodex) {
        SetWindowTextW(state.subtitle,
                       L"Codex - read-only data and official service status");
    } else {
        SetWindowTextW(state.subtitle, L"Computer performance - Windows native metrics");
    }
}

void UpdateContentScrollBar(HWND window, AppState& state,
                            int totalRows, int visibleRows) {
    state.contentVisibleRows = std::max(1, visibleRows);
    state.contentScrollMaximumRow = std::max(0, totalRows - state.contentVisibleRows);
    state.contentScrollRow =
        std::clamp(state.contentScrollRow, 0, state.contentScrollMaximumRow);

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = std::max(0, totalRows - 1);
    info.nPage = static_cast<UINT>(state.contentVisibleRows);
    info.nPos = state.contentScrollRow;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    ShowScrollBar(window, SB_VERT, totalRows > state.contentVisibleRows);
}

void LayoutCardGrid(HWND window, AppState& state, const std::vector<HWND>& cards,
                    int y, int width, int height, int margin, int gap) {
    if (cards.empty()) {
        UpdateContentScrollBar(window, state, 0, 1);
        return;
    }
    const bool useTwoColumns = width >= ScaleForDpi(window, 700) && cards.size() > 1;
    const int columns = useTwoColumns ? 2 : 1;
    const int rows = (static_cast<int>(cards.size()) + columns - 1) / columns;
    const int cardWidth = (width - margin * 2 - gap * (columns - 1)) / columns;
    const int cardHeight = ScaleForDpi(window, 112);
    const int rowStride = cardHeight + gap;
    const int availableHeight = std::max(0, height - y - margin);
    const int visibleRows = std::max(1, (availableHeight + gap) / rowStride);
    UpdateContentScrollBar(window, state, rows, visibleRows);

    for (std::size_t index = 0; index < cards.size(); ++index) {
        const int row = static_cast<int>(index) / columns;
        const int column = static_cast<int>(index) % columns;
        const bool rowVisible = row >= state.contentScrollRow &&
            row < state.contentScrollRow + state.contentVisibleRows;
        ShowWindow(cards[index], rowVisible ? SW_SHOW : SW_HIDE);
        if (!rowVisible) continue;
        const int x = margin + column * (cardWidth + gap);
        const int cardY = y + (row - state.contentScrollRow) * rowStride;
        MoveWindow(cards[index], x, cardY, cardWidth, cardHeight, TRUE);
    }
}

void LayoutControls(HWND window, AppState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = ScaleForDpi(window, 16);
    const int gap = ScaleForDpi(window, 8);
    const int actionButtonWidth = ScaleForDpi(window, 82);
    const int buttonHeight = ScaleForDpi(window, 28);

    const int actionsWidth = actionButtonWidth * 2 + gap;
    const int headingWidth = std::max(ScaleForDpi(window, 110),
                                      width - margin * 2 - actionsWidth - gap);
    MoveWindow(state.heading, margin, margin, headingWidth, ScaleForDpi(window, 28), TRUE);
    MoveWindow(state.pinButton, width - margin - actionsWidth, margin,
               actionButtonWidth, buttonHeight, TRUE);
    MoveWindow(state.minimizeButton, width - margin - actionButtonWidth, margin,
               actionButtonWidth, buttonHeight, TRUE);

    int y = margin + ScaleForDpi(window, 38);
    const int homeWidth = ScaleForDpi(window, 64);
    const int codexWidth = ScaleForDpi(window, 64);
    const int computerWidth = ScaleForDpi(window, 82);
    const int settingsWidth = ScaleForDpi(window, 76);
    int x = margin;
    MoveWindow(state.homePageButton, x, y, homeWidth, buttonHeight, TRUE);
    x += homeWidth + gap;
    MoveWindow(state.codexPageButton, x, y, codexWidth, buttonHeight, TRUE);
    x += codexWidth + gap;
    MoveWindow(state.computerPageButton, x, y, computerWidth, buttonHeight, TRUE);
    MoveWindow(state.settingsButton, width - margin - settingsWidth, y,
               settingsWidth, buttonHeight, TRUE);

    y += ScaleForDpi(window, 36);
    MoveWindow(state.subtitle, margin, y, width - margin * 2, ScaleForDpi(window, 22), TRUE);
    y += ScaleForDpi(window, 24);
    MoveWindow(state.status, margin, y, width - margin * 2, ScaleForDpi(window, 22), TRUE);
    y += ScaleForDpi(window, 30);

    const std::vector<HWND> cards = VisiblePageCards(state);
    LayoutCardGrid(window, state, cards, y, width, height,
                   margin, ScaleForDpi(window, 10));
    MoveWindow(state.codexNotice, margin, y, width - margin * 2,
               std::max(ScaleForDpi(window, 130), height - y - margin), TRUE);
    MoveWindow(state.emptyHomeNotice, margin, y, width - margin * 2,
               std::max(ScaleForDpi(window, 100), height - y - margin), TRUE);
}

bool CreateControls(HWND window, AppState& state) {
    state.mainWindow = window;
    state.backgroundBrush = CreateSolidBrush(RGB(20, 24, 32));
    state.cardBrush = CreateSolidBrush(RGB(31, 38, 50));
    state.heading = CreateLabel(window, L"Codex Monitor HUD", SS_LEFT | SS_CENTERIMAGE);
    state.subtitle = CreateLabel(window, L"Starting Windows product shell",
                                 SS_LEFT | SS_CENTERIMAGE);
    state.status = CreateLabel(window, L"Evaluating sampler demand", SS_LEFT | SS_CENTERIMAGE);
    state.pinButton = CreateButton(window, L"Unpin", kPinButtonId);
    state.minimizeButton = CreateButton(window, L"Minimize", kMinimizeButtonId);
    state.homePageButton = CreateButton(window, L"Home", kHomePageButtonId,
                                        BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP);
    state.codexPageButton = CreateButton(window, L"Codex", kCodexPageButtonId,
                                         BS_AUTORADIOBUTTON | BS_PUSHLIKE);
    state.computerPageButton = CreateButton(window, L"Computer", kComputerPageButtonId,
                                            BS_AUTORADIOBUTTON | BS_PUSHLIKE);
    state.settingsButton = CreateButton(window, L"Settings", kSettingsButtonId);
    state.codexNotice = CreateLabel(
        window,
        L"Codex 页面当前未选择任何模块。\r\n\r\n"
        L"请打开设置，启用一个或多个 Codex 模块。",
        SS_LEFT | WS_BORDER);
    state.emptyHomeNotice = CreateLabel(
        window,
        L"No modules are shown on Home.\r\n\r\nOpen Settings to choose one or more "
        L"performance or Codex modules.",
        SS_LEFT | WS_BORDER);

    for (const codex_monitor::ModuleDefinition& definition :
         codex_monitor::ModuleRegistry()) {
        ModuleViews views;
        views.id = definition.id;
        const std::wstring initialText = definition.requiresPerformanceSampling
            ? L"Starting sampler"
            : definition.requiresCodexData
                ? BuildCodexUnavailableText(definition.id)
                : BuildServiceStatusCardText(state);
        views.homeCard =
            CreateLabel(window, initialText.c_str(), SS_LEFT | WS_BORDER);
        views.nativeCard =
            CreateLabel(window, initialText.c_str(), SS_LEFT | WS_BORDER);
        state.moduleViews.push_back(views);
    }

    if (!state.backgroundBrush || !state.cardBrush || !state.heading || !state.subtitle ||
        !state.status || !state.pinButton || !state.minimizeButton ||
        !state.homePageButton || !state.codexPageButton || !state.computerPageButton ||
        !state.settingsButton || !state.codexNotice || !state.emptyHomeNotice) {
        return false;
    }
    for (const ModuleViews& views : state.moduleViews) {
        if (!views.homeCard || !views.nativeCard) return false;
    }

    RecreateFonts(window, state);
    UpdateTopmostState(window, state, state.settings.alwaysOnTop, false);
    UpdatePageVisibility(state);
    LayoutControls(window, state);
    return true;
}

bool IsCardControl(const AppState& state, HWND control) {
    if (control == state.codexNotice || control == state.emptyHomeNotice) return true;
    for (const ModuleViews& views : state.moduleViews) {
        if (control == views.homeCard || control == views.nativeCard) return true;
    }
    return false;
}

void CaptureWindowPlacement(HWND window, AppState& state) {
    if (!window || IsIconic(window) || IsZoomed(window)) return;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return;
    state.settings.windowPlacement = codex_monitor::WindowPlacement{
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
    };
}

BOOL CALLBACK CollectMonitorWorkArea(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* areas = reinterpret_cast<std::vector<codex_monitor::WindowPlacement>*>(context);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    areas->push_back({
        info.rcWork.left,
        info.rcWork.top,
        info.rcWork.right - info.rcWork.left,
        info.rcWork.bottom - info.rcWork.top,
    });
    return TRUE;
}

std::vector<codex_monitor::WindowPlacement> VisibleWorkAreas() {
    std::vector<codex_monitor::WindowPlacement> areas;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorWorkArea,
                        reinterpret_cast<LPARAM>(&areas));
    return areas;
}

void ClampWindowToVisibleScreens(HWND window, AppState& state) {
    if (!window || IsIconic(window)) return;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return;
    const codex_monitor::WindowPlacement current{
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
    };
    const codex_monitor::WindowPlacement clamped =
        codex_monitor::ClampWindowPlacement(current, VisibleWorkAreas());
    if (clamped.x != current.x || clamped.y != current.y ||
        clamped.width != current.width || clamped.height != current.height) {
        SetWindowPos(window, nullptr, clamped.x, clamped.y, clamped.width, clamped.height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
    state.settings.windowPlacement = clamped;
}

bool SetContentScrollRow(HWND window, AppState& state, int requestedRow) {
    const int clamped =
        std::clamp(requestedRow, 0, state.contentScrollMaximumRow);
    if (clamped == state.contentScrollRow) return false;
    state.contentScrollRow = clamped;
    LayoutControls(window, state);
    return true;
}

void SwitchPage(HWND window, AppState& state, codex_monitor::Page page) {
    if (state.settings.currentPage == page) return;
    state.settings.currentPage = page;
    state.contentScrollRow = 0;
    state.wheelDeltaRemainder = 0;
    UpdatePageVisibility(state);
    if (state.hasPerformanceSnapshot) UpdateModuleCards(state);
    LayoutControls(window, state);
    UpdateSamplingDemand(window, state);
    UpdateCodexDemand(window, state);
    UpdateServiceStatusDemand(window, state);
    PersistSettings(state);
}

void RefreshUpdateControls(AppState& state) {
    if (!state.settingsUpdateStatus || !state.settingsCheckUpdatesButton) {
        return;
    }
    std::wstring message;
    if (!state.updateWorkerAvailable) {
        message = L"Windows 更新检查当前不可用";
    } else if (state.updateWorker.IsBusy()) {
        message = L"正在检查 Windows 更新…";
    } else if (!state.latestUpdateCheck) {
        message = L"每天自动检查一次，也可以手动检查";
    } else {
        const auto& completed = *state.latestUpdateCheck;
        if (completed.fromCache && !completed.availableVersion.empty()) {
            message = L"已记录 Windows 新版 " +
                      std::wstring(completed.availableVersion.begin(),
                                   completed.availableVersion.end());
        } else {
            switch (completed.result.status) {
                case codex_monitor::update::WindowsUpdateCheckStatus::kUpdateAvailable:
                    message = L"发现 Windows 新版 " +
                              std::wstring(completed.availableVersion.begin(),
                                           completed.availableVersion.end());
                    break;
                case codex_monitor::update::WindowsUpdateCheckStatus::kUpToDate:
                    message = L"当前 Windows 版已是最新版";
                    break;
                case codex_monitor::update::WindowsUpdateCheckStatus::kFetchFailed:
                    message = L"更新检查失败；不会影响监控";
                    break;
                case codex_monitor::update::WindowsUpdateCheckStatus::kInvalidCurrentVersion:
                    message = L"当前版本号无法用于自动更新";
                    break;
                case codex_monitor::update::WindowsUpdateCheckStatus::kInvalidResponse:
                    message = L"GitHub 更新信息格式异常";
                    break;
            }
        }
        if (completed.stateSaveFailed) message += L"；检查记录未保存";
    }
    SetWindowTextW(state.settingsUpdateStatus, message.c_str());
    EnableWindow(state.settingsCheckUpdatesButton,
                 state.updateWorkerAvailable && !state.updateWorker.IsBusy());
}

void RefreshSettingsControls(AppState& state) {
    if (!state.settingsWindow) return;
    state.settings.homeOrder = codex_monitor::SanitizeHomeOrder(state.settings.homeOrder);
    for (std::size_t position = 0; position < state.settings.homeOrder.size(); ++position) {
        const codex_monitor::ModuleId id = state.settings.homeOrder[position];
        SettingsRow* row = FindSettingsRow(state, id);
        if (!row) continue;
        const std::size_t index = codex_monitor::ModuleIndex(id);
        SendMessageW(row->homeVisibleCheck, BM_SETCHECK,
                     state.settings.homeVisible[codex_monitor::ModuleIndex(id)]
                         ? BST_CHECKED
                         : BST_UNCHECKED,
                     0);
        SendMessageW(row->nativeVisibleCheck, BM_SETCHECK,
                     state.settings.nativePageVisible[index]
                         ? BST_CHECKED
                         : BST_UNCHECKED,
                     0);
        EnableWindow(row->moveUpButton, position > 0);
        EnableWindow(row->moveDownButton, position + 1 < state.settings.homeOrder.size());
    }
    SendMessageW(state.settingsTopmostCheck, BM_SETCHECK,
                 state.settings.alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
    RefreshUpdateControls(state);
    LayoutSettingsControls(state.settingsWindow, state);
}

void LayoutSettingsControls(HWND window, AppState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = ScaleForDpi(window, 16);
    const int gap = ScaleForDpi(window, 6);
    const int rowHeight = ScaleForDpi(window, 30);
    const int homeCheckWidth = ScaleForDpi(window, 72);
    const int nativeCheckWidth = ScaleForDpi(window, 102);
    const int smallButtonWidth = ScaleForDpi(window, 48);
    const int fixedControlsWidth = homeCheckWidth + nativeCheckWidth +
                                   smallButtonWidth * 2 + gap * 4;
    const int nameWidth = std::max(ScaleForDpi(window, 180),
                                   width - margin * 2 - fixedControlsWidth);

    const int headingHeight = ScaleForDpi(window, 28);
    const int headingAdvance = ScaleForDpi(window, 40);
    const int rowCount = static_cast<int>(state.settings.homeOrder.size());
    const int contentHeight = margin + headingAdvance +
        rowCount * (rowHeight + gap) + ScaleForDpi(window, 4) +
        rowHeight + ScaleForDpi(window, 8) + rowHeight +
        ScaleForDpi(window, 12) + rowHeight + margin;
    state.settingsScrollMaximum = std::max(0, contentHeight - height);
    state.settingsScrollOffset =
        std::clamp(state.settingsScrollOffset, 0, state.settingsScrollMaximum);

    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = std::max(0, contentHeight - 1);
    scroll.nPage = static_cast<UINT>(std::max(1, height));
    scroll.nPos = state.settingsScrollOffset;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);
    ShowScrollBar(window, SB_VERT, state.settingsScrollMaximum > 0);

    int y = margin - state.settingsScrollOffset;
    MoveWindow(state.settingsHeading, margin, y, width - margin * 2,
               headingHeight, TRUE);
    y += headingAdvance;
    for (codex_monitor::ModuleId id : state.settings.homeOrder) {
        SettingsRow* row = FindSettingsRow(state, id);
        if (!row) continue;
        int x = margin;
        MoveWindow(row->nameLabel, x, y, nameWidth, rowHeight, TRUE);
        x += nameWidth + gap;
        MoveWindow(row->homeVisibleCheck, x, y, homeCheckWidth, rowHeight, TRUE);
        x += homeCheckWidth + gap;
        MoveWindow(row->nativeVisibleCheck, x, y, nativeCheckWidth, rowHeight, TRUE);
        x += nativeCheckWidth + gap;
        MoveWindow(row->moveUpButton, x, y, smallButtonWidth, rowHeight, TRUE);
        x += smallButtonWidth + gap;
        MoveWindow(row->moveDownButton, x, y, smallButtonWidth, rowHeight, TRUE);
        y += rowHeight + gap;
    }
    y += ScaleForDpi(window, 4);
    MoveWindow(state.settingsTopmostCheck, margin, y,
               width - margin * 2, rowHeight, TRUE);
    y += rowHeight + ScaleForDpi(window, 8);
    const int updateButtonWidth = ScaleForDpi(window, 104);
    MoveWindow(state.settingsUpdateStatus, margin, y,
               std::max(ScaleForDpi(window, 180),
                        width - margin * 2 - updateButtonWidth - gap),
               rowHeight, TRUE);
    MoveWindow(state.settingsCheckUpdatesButton,
               width - margin - updateButtonWidth, y,
               updateButtonWidth, rowHeight, TRUE);
    y += rowHeight + ScaleForDpi(window, 12);
    MoveWindow(state.settingsCloseButton, width - margin - ScaleForDpi(window, 82), y,
               ScaleForDpi(window, 82), rowHeight, TRUE);
}

bool SetSettingsScrollOffset(HWND window, AppState& state, int requestedOffset) {
    const int clamped =
        std::clamp(requestedOffset, 0, state.settingsScrollMaximum);
    if (clamped == state.settingsScrollOffset) return false;
    state.settingsScrollOffset = clamped;
    LayoutSettingsControls(window, state);
    return true;
}

bool CreateSettingsControls(HWND window, AppState& state) {
    state.settingsHeading = CreateLabel(
        window,
        L"Columns: module | Home visibility | own-page visibility | Home order",
        SS_LEFT | SS_CENTERIMAGE);
    state.settingsRows.clear();
    for (const codex_monitor::ModuleDefinition& definition :
         codex_monitor::ModuleRegistry()) {
        const int index = static_cast<int>(codex_monitor::ModuleIndex(definition.id));
        SettingsRow row;
        row.id = definition.id;
        row.nameLabel = CreateLabel(
            window, std::wstring(definition.displayName).c_str(),
            SS_LEFT | SS_CENTERIMAGE);
        row.homeVisibleCheck = CreateWindowExW(
            0, L"BUTTON", L"Home",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsVisibleBaseId + index)),
            GetModuleHandleW(nullptr), nullptr);
        const wchar_t* nativePageLabel =
            definition.nativePage == codex_monitor::Page::kCodex ? L"Codex" : L"Computer";
        row.nativeVisibleCheck = CreateWindowExW(
            0, L"BUTTON", nativePageLabel,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kSettingsNativeVisibleBaseId + index)),
            GetModuleHandleW(nullptr), nullptr);
        row.moveUpButton = CreateButton(window, L"Up", kSettingsMoveUpBaseId + index);
        row.moveDownButton = CreateButton(window, L"Down", kSettingsMoveDownBaseId + index);
        state.settingsRows.push_back(row);
    }
    state.settingsTopmostCheck = CreateWindowExW(
        0, L"BUTTON", L"始终置顶",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsTopmostId)),
        GetModuleHandleW(nullptr), nullptr);
    state.settingsUpdateStatus = CreateLabel(
        window, L"每天自动检查一次，也可以手动检查",
        SS_LEFT | SS_CENTERIMAGE);
    state.settingsCheckUpdatesButton =
        CreateButton(window, L"检查更新", kSettingsCheckUpdatesId);
    state.settingsCloseButton = CreateButton(window, L"Close", kSettingsCloseId);

    if (!state.settingsHeading || !state.settingsTopmostCheck ||
        !state.settingsUpdateStatus || !state.settingsCheckUpdatesButton ||
        !state.settingsCloseButton) {
        return false;
    }
    for (const SettingsRow& row : state.settingsRows) {
        if (!row.nameLabel || !row.homeVisibleCheck || !row.nativeVisibleCheck ||
            !row.moveUpButton || !row.moveDownButton) return false;
    }

    RecreateSettingsFonts(window, state);
    RefreshSettingsControls(state);
    return true;
}

void OpenSettingsWindow(HWND owner, AppState& state) {
    if (state.settingsWindow) {
        ShowWindow(state.settingsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(state.settingsWindow);
        return;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &monitorInfo.rcWork, 0)) {
            monitorInfo.rcWork =
                RECT{0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN)};
        }
    }
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    const int edge = ScaleForDpi(owner, 12);
    const int width = std::min(ScaleForDpi(owner, 760),
                               std::max(1, workWidth - edge * 2));
    const int height = std::min(ScaleForDpi(owner, 560),
                                std::max(1, workHeight - edge * 2));
    const int x = monitorInfo.rcWork.left + (workWidth - width) / 2;
    const int y = monitorInfo.rcWork.top + (workHeight - height) / 2;
    state.settingsScrollOffset = 0;
    state.settingsScrollMaximum = 0;
    state.settingsWheelDeltaRemainder = 0;
    state.settingsWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kSettingsWindowClassName, L"Codex Monitor HUD Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL,
        x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.settingsWindow) {
        RefreshSettingsControls(state);
        ShowWindow(state.settingsWindow, SW_SHOWNORMAL);
        UpdateWindow(state.settingsWindow);
    }
}

LRESULT CALLBACK SettingsWindowProcedure(HWND window, UINT message,
                                         WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE:
            if (!state || !CreateSettingsControls(window, *state)) return -1;
            return 0;

        case WM_COMMAND: {
            if (!state || HIWORD(wParam) != BN_CLICKED) break;
            const int controlId = LOWORD(wParam);
            if (controlId >= kSettingsVisibleBaseId &&
                controlId < kSettingsVisibleBaseId + static_cast<int>(codex_monitor::kModuleCount)) {
                const std::size_t index = static_cast<std::size_t>(controlId - kSettingsVisibleBaseId);
                const codex_monitor::ModuleId id = codex_monitor::ModuleRegistry()[index].id;
                SettingsRow* row = FindSettingsRow(*state, id);
                if (row) {
                    state->settings.homeVisible[index] =
                        SendMessageW(row->homeVisibleCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    UpdatePageVisibility(*state);
                    if (state->hasPerformanceSnapshot) UpdateModuleCards(*state);
                    LayoutControls(state->mainWindow, *state);
                    UpdateSamplingDemand(state->mainWindow, *state);
                    UpdateCodexDemand(state->mainWindow, *state);
                    UpdateServiceStatusDemand(state->mainWindow, *state);
                    PersistSettings(*state);
                }
                return 0;
            }
            if (controlId >= kSettingsNativeVisibleBaseId &&
                controlId < kSettingsNativeVisibleBaseId +
                                static_cast<int>(codex_monitor::kModuleCount)) {
                const std::size_t index = static_cast<std::size_t>(
                    controlId - kSettingsNativeVisibleBaseId);
                const codex_monitor::ModuleId id =
                    codex_monitor::ModuleRegistry()[index].id;
                SettingsRow* row = FindSettingsRow(*state, id);
                if (row) {
                    state->settings.nativePageVisible[index] =
                        SendMessageW(row->nativeVisibleCheck, BM_GETCHECK, 0, 0) ==
                        BST_CHECKED;
                    UpdatePageVisibility(*state);
                    if (state->hasPerformanceSnapshot) UpdateModuleCards(*state);
                    LayoutControls(state->mainWindow, *state);
                    UpdateSamplingDemand(state->mainWindow, *state);
                    UpdateCodexDemand(state->mainWindow, *state);
                    UpdateServiceStatusDemand(state->mainWindow, *state);
                    PersistSettings(*state);
                }
                return 0;
            }
            if (controlId >= kSettingsMoveUpBaseId &&
                controlId < kSettingsMoveUpBaseId + static_cast<int>(codex_monitor::kModuleCount)) {
                const std::size_t index = static_cast<std::size_t>(controlId - kSettingsMoveUpBaseId);
                codex_monitor::MoveHomeModule(
                    state->settings, codex_monitor::ModuleRegistry()[index].id, -1);
                RefreshSettingsControls(*state);
                UpdatePageVisibility(*state);
                LayoutControls(state->mainWindow, *state);
                PersistSettings(*state);
                return 0;
            }
            if (controlId >= kSettingsMoveDownBaseId &&
                controlId < kSettingsMoveDownBaseId + static_cast<int>(codex_monitor::kModuleCount)) {
                const std::size_t index = static_cast<std::size_t>(controlId - kSettingsMoveDownBaseId);
                codex_monitor::MoveHomeModule(
                    state->settings, codex_monitor::ModuleRegistry()[index].id, 1);
                RefreshSettingsControls(*state);
                UpdatePageVisibility(*state);
                LayoutControls(state->mainWindow, *state);
                PersistSettings(*state);
                return 0;
            }
            if (controlId == kSettingsTopmostId) {
                const bool enabled =
                    SendMessageW(state->settingsTopmostCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                UpdateTopmostState(state->mainWindow, *state, enabled, true);
                return 0;
            }
            if (controlId == kSettingsCheckUpdatesId) {
                if (state->updateWorkerAvailable &&
                    state->updateWorker.RequestManualCheck()) {
                    RefreshUpdateControls(*state);
                    UpdateStatusText(*state);
                }
                return 0;
            }
            if (controlId == kSettingsCloseId) {
                DestroyWindow(window);
                return 0;
            }
            break;
        }

        case WM_SIZE:
            if (state && wParam != SIZE_MINIMIZED) LayoutSettingsControls(window, *state);
            return 0;

        case WM_VSCROLL:
            if (state) {
                int requested = state->settingsScrollOffset;
                const int line = ScaleForDpi(window, 30);
                RECT client{};
                GetClientRect(window, &client);
                const int page = std::max(
                    line, static_cast<int>(client.bottom - client.top) - line);
                switch (LOWORD(wParam)) {
                    case SB_LINEUP:
                        requested -= line;
                        break;
                    case SB_LINEDOWN:
                        requested += line;
                        break;
                    case SB_PAGEUP:
                        requested -= page;
                        break;
                    case SB_PAGEDOWN:
                        requested += page;
                        break;
                    case SB_TOP:
                        requested = 0;
                        break;
                    case SB_BOTTOM:
                        requested = state->settingsScrollMaximum;
                        break;
                    case SB_THUMBPOSITION:
                    case SB_THUMBTRACK: {
                        SCROLLINFO info{};
                        info.cbSize = sizeof(info);
                        info.fMask = SIF_TRACKPOS;
                        if (GetScrollInfo(window, SB_VERT, &info)) {
                            requested = info.nTrackPos;
                        }
                        break;
                    }
                    default:
                        return 0;
                }
                SetSettingsScrollOffset(window, *state, requested);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (state) {
                state->settingsWheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
                const int steps = state->settingsWheelDeltaRemainder / WHEEL_DELTA;
                state->settingsWheelDeltaRemainder -= steps * WHEEL_DELTA;
                if (steps != 0) {
                    SetSettingsScrollOffset(
                        window, *state,
                        state->settingsScrollOffset -
                            steps * ScaleForDpi(window, 30));
                }
            }
            return 0;

        case WM_GETMINMAXINFO:
            if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) {
                MONITORINFO monitorInfo{};
                monitorInfo.cbSize = sizeof(monitorInfo);
                const HMONITOR monitor =
                    MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
                if (GetMonitorInfoW(monitor, &monitorInfo)) {
                    const int workWidth =
                        monitorInfo.rcWork.right - monitorInfo.rcWork.left;
                    const int workHeight =
                        monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
                    limits->ptMinTrackSize.x =
                        std::min(ScaleForDpi(window, 520), workWidth);
                    limits->ptMinTrackSize.y =
                        std::min(ScaleForDpi(window, 300), workHeight);
                    limits->ptMaxTrackSize.x = workWidth;
                    limits->ptMaxTrackSize.y = workHeight;
                }
            }
            return 0;

        case WM_DPICHANGED:
            if (state) {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
                RecreateSettingsFonts(window, *state);
                LayoutSettingsControls(window, *state);
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (state) {
                if (state->settingsBodyFont) DeleteObject(state->settingsBodyFont);
                if (state->settingsSmallFont) DeleteObject(state->settingsSmallFont);
                state->settingsBodyFont = nullptr;
                state->settingsSmallFont = nullptr;
                state->settingsWindow = nullptr;
                state->settingsHeading = nullptr;
                state->settingsTopmostCheck = nullptr;
                state->settingsUpdateStatus = nullptr;
                state->settingsCheckUpdatesButton = nullptr;
                state->settingsCloseButton = nullptr;
                state->settingsRows.clear();
                state->settingsScrollOffset = 0;
                state->settingsScrollMaximum = 0;
                state->settingsWheelDeltaRemainder = 0;
            }
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
        case WM_CREATE:
            if (!state || !CreateControls(window, *state)) return -1;
            if (!state->performanceWorker.Start(window, kSampleReadyMessage)) return -1;
            state->codexWorkerAvailable =
                state->codexWorker.Start(
                    window, kCodexReadyMessage, kApplicationVersion,
                    state->settingsPath.empty()
                        ? std::filesystem::path{}
                        : state->settingsPath.parent_path() /
                              L"quota-usage-history.txt",
                    state->settingsPath.empty()
                        ? std::filesystem::path{}
                        : state->settingsPath.parent_path() /
                              L"codex-cost-history-cache.txt");
            state->serviceStatusWorkerAvailable =
                state->serviceStatusWorker.Start(window,
                                                 kServiceStatusReadyMessage);
            state->updateWorkerAvailable = state->updateWorker.Start(
                window, kUpdateReadyMessage, kApplicationVersion,
                state->settingsPath.empty()
                    ? std::filesystem::path{}
                    : state->settingsPath.parent_path() /
                          L"update-state.ini");
            UpdateSamplingDemand(window, *state);
            UpdateCodexDemand(window, *state);
            UpdateServiceStatusDemand(window, *state);
            return 0;

        case WM_COMMAND:
            if (!state || HIWORD(wParam) != BN_CLICKED) break;
            switch (LOWORD(wParam)) {
                case kPinButtonId:
                    UpdateTopmostState(window, *state, !state->settings.alwaysOnTop, true);
                    return 0;
                case kMinimizeButtonId:
                    ShowWindow(window, SW_MINIMIZE);
                    return 0;
                case kHomePageButtonId:
                    SwitchPage(window, *state, codex_monitor::Page::kHome);
                    return 0;
                case kCodexPageButtonId:
                    SwitchPage(window, *state, codex_monitor::Page::kCodex);
                    return 0;
                case kComputerPageButtonId:
                    SwitchPage(window, *state, codex_monitor::Page::kComputer);
                    return 0;
                case kSettingsButtonId:
                    OpenSettingsWindow(window, *state);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_TIMER:
            if (state && wParam == kSampleTimerId && !state->samplingPaused) {
                const codex_monitor::SampleMode mode =
                    GetTickCount64() >= state->nextSlowSampleTick
                        ? codex_monitor::SampleMode::kFastAndSlow
                        : codex_monitor::SampleMode::kFast;
                state->performanceWorker.Request(mode);
                UpdateStatusText(*state);
                return 0;
            }
            break;

        case kSampleReadyMessage:
            if (state) {
                std::optional<codex_monitor::CompletedSample> completed =
                    state->performanceWorker.TakeLatest();
                if (completed && !state->samplingPaused) {
                    ApplyPerformanceSnapshot(*state, std::move(*completed));
                } else {
                    UpdateStatusText(*state);
                }
            }
            return 0;

        case kCodexReadyMessage:
            if (state) {
                std::optional<codex_monitor::codex::CompletedCodexRefresh> completed =
                    state->codexWorker.TakeLatest();
                if (completed && !state->codexPaused) {
                    ApplyCodexRefresh(*state, std::move(*completed));
                } else {
                    UpdateStatusText(*state);
                }
            }
            return 0;

        case kServiceStatusReadyMessage:
            if (state) {
                std::optional<codex_monitor::CompletedServiceStatusRefresh>
                    completed = state->serviceStatusWorker.TakeLatest();
                if (completed && !state->serviceStatusPaused) {
                    ApplyServiceStatusRefresh(*state, std::move(*completed));
                } else {
                    UpdateStatusText(*state);
                }
            }
            return 0;

        case kUpdateReadyMessage:
            if (state) {
                std::optional<
                    codex_monitor::update::CompletedWindowsUpdateCheck>
                    completed = state->updateWorker.TakeLatest();
                if (completed) {
                    ApplyWindowsUpdateCheck(*state, std::move(*completed));
                } else {
                    RefreshUpdateControls(*state);
                    UpdateStatusText(*state);
                }
            }
            return 0;

        case WM_VSCROLL:
            if (state) {
                int requestedRow = state->contentScrollRow;
                switch (LOWORD(wParam)) {
                    case SB_LINEUP:
                        --requestedRow;
                        break;
                    case SB_LINEDOWN:
                        ++requestedRow;
                        break;
                    case SB_PAGEUP:
                        requestedRow -= state->contentVisibleRows;
                        break;
                    case SB_PAGEDOWN:
                        requestedRow += state->contentVisibleRows;
                        break;
                    case SB_TOP:
                        requestedRow = 0;
                        break;
                    case SB_BOTTOM:
                        requestedRow = state->contentScrollMaximumRow;
                        break;
                    case SB_THUMBPOSITION:
                    case SB_THUMBTRACK: {
                        SCROLLINFO info{};
                        info.cbSize = sizeof(info);
                        info.fMask = SIF_TRACKPOS;
                        if (GetScrollInfo(window, SB_VERT, &info)) {
                            requestedRow = info.nTrackPos;
                        }
                        break;
                    }
                    default:
                        return 0;
                }
                SetContentScrollRow(window, *state, requestedRow);
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (state) {
                state->wheelDeltaRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
                const int steps = state->wheelDeltaRemainder / WHEEL_DELTA;
                state->wheelDeltaRemainder -= steps * WHEEL_DELTA;
                if (steps != 0) {
                    SetContentScrollRow(
                        window, *state, state->contentScrollRow - steps);
                }
            }
            return 0;

        case WM_SIZE:
            if (!state) return 0;
            {
                const bool wasMinimized = state->windowMinimized;
                state->windowMinimized = wParam == SIZE_MINIMIZED;
                if (!state->windowMinimized) {
                    if (wasMinimized) {
                        ClampWindowToVisibleScreens(window, *state);
                        CaptureWindowPlacement(window, *state);
                        PersistSettings(*state);
                    }
                    LayoutControls(window, *state);
                }
            }
            UpdateSamplingDemand(window, *state);
            UpdateCodexDemand(window, *state);
            UpdateServiceStatusDemand(window, *state);
            return 0;

        case WM_SHOWWINDOW:
            if (state) {
                state->windowHidden = wParam == FALSE;
                UpdateCodexDemand(window, *state);
                UpdateServiceStatusDemand(window, *state);
            }
            break;

        case WM_EXITSIZEMOVE:
            if (state) {
                CaptureWindowPlacement(window, *state);
                PersistSettings(*state);
            }
            return 0;

        case WM_DISPLAYCHANGE:
            if (state) {
                ClampWindowToVisibleScreens(window, *state);
                LayoutControls(window, *state);
                PersistSettings(*state);
            }
            return 0;

        case WM_GETMINMAXINFO:
            if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) {
                MONITORINFO monitorInfo{};
                monitorInfo.cbSize = sizeof(monitorInfo);
                const HMONITOR monitor =
                    MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
                if (GetMonitorInfoW(monitor, &monitorInfo)) {
                    const int workWidth =
                        monitorInfo.rcWork.right - monitorInfo.rcWork.left;
                    const int workHeight =
                        monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
                    limits->ptMinTrackSize.x =
                        std::min(ScaleForDpi(window, kMinimumWidth), workWidth);
                    limits->ptMinTrackSize.y =
                        std::min(ScaleForDpi(window, kMinimumHeight), workHeight);
                    limits->ptMaxTrackSize.x = workWidth;
                    limits->ptMaxTrackSize.y = workHeight;
                }
            }
            return 0;

        case WM_DPICHANGED:
            if (state) {
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
                ClampWindowToVisibleScreens(window, *state);
                RecreateFonts(window, *state);
                LayoutControls(window, *state);
                CaptureWindowPlacement(window, *state);
                PersistSettings(*state);
            }
            return 0;

        case WM_CTLCOLORSTATIC:
            if (state) {
                HDC device = reinterpret_cast<HDC>(wParam);
                HWND control = reinterpret_cast<HWND>(lParam);
                SetBkMode(device, TRANSPARENT);
                const bool isCard = IsCardControl(*state, control);
                SetTextColor(device, isCard ? RGB(228, 233, 241) : RGB(203, 211, 223));
                return reinterpret_cast<INT_PTR>(isCard ? state->cardBrush
                                                        : state->backgroundBrush);
            }
            break;

        case WM_ERASEBKGND:
            if (state && state->backgroundBrush) {
                RECT client{};
                GetClientRect(window, &client);
                FillRect(reinterpret_cast<HDC>(wParam), &client, state->backgroundBrush);
                return 1;
            }
            break;

        case WM_DESTROY:
            if (state) {
                CaptureWindowPlacement(window, *state);
                PersistSettings(*state);
                StopSamplingTimer(window, *state);
                state->performanceWorker.StopAndJoin();
                state->codexWorker.StopAndJoin();
                state->serviceStatusWorker.StopAndJoin();
                state->updateWorker.RequestStop();
                if (state->settingsWindow) DestroyWindow(state->settingsWindow);
                DeleteFonts(*state);
                if (state->backgroundBrush) DeleteObject(state->backgroundBrush);
                if (state->cardBrush) DeleteObject(state->cardBrush);
                state->backgroundBrush = nullptr;
                state->cardBrush = nullptr;
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterWindowClasses(HINSTANCE instance) {
    HICON applicationIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CODEX_MONITOR_HUD), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON smallApplicationIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_CODEX_MONITOR_HUD), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!applicationIcon) applicationIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!smallApplicationIcon) smallApplicationIcon = LoadIconW(nullptr, IDI_APPLICATION);

    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = WindowProcedure;
    mainClass.hInstance = instance;
    mainClass.hIcon = applicationIcon;
    mainClass.hIconSm = smallApplicationIcon;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&mainClass)) return false;

    WNDCLASSEXW settingsClass{};
    settingsClass.cbSize = sizeof(settingsClass);
    settingsClass.style = CS_HREDRAW | CS_VREDRAW;
    settingsClass.lpfnWndProc = SettingsWindowProcedure;
    settingsClass.hInstance = instance;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground =
        reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    settingsClass.lpszClassName = kSettingsWindowClassName;
    return RegisterClassExW(&settingsClass) != 0;
}

codex_monitor::WindowPlacement DefaultWindowPlacement(UINT dpi) {
    const int width = MulDiv(460, static_cast<int>(dpi), 96);
    const int height = MulDiv(680, static_cast<int>(dpi), 96);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int edgeMargin = MulDiv(24, static_cast<int>(dpi), 96);
    return {
        workArea.left + edgeMargin,
        std::max(workArea.top + edgeMargin, workArea.bottom - height - edgeMargin),
        width,
        height,
    };
}

codex_monitor::WindowPlacement InitialWindowPlacement(const AppState& state, UINT dpi) {
    codex_monitor::WindowPlacement placement = DefaultWindowPlacement(dpi);
    if (state.settings.windowPlacement) {
        const int minimumWidth = MulDiv(kMinimumWidth, static_cast<int>(dpi), 96);
        const int minimumHeight = MulDiv(kMinimumHeight, static_cast<int>(dpi), 96);
        if (state.settings.windowPlacement->width >= minimumWidth &&
            state.settings.windowPlacement->height >= minimumHeight) {
            placement = *state.settings.windowPlacement;
        }
    }
    return codex_monitor::ClampWindowPlacement(placement, VisibleWorkAreas());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Keep Windows Runtime initialized for the application lifetime while
    // background workers initialize and release their own MTA threads.
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
        return 1;
    }

    HANDLE singleton = CreateMutexW(nullptr, TRUE, kSingletonName);
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Codex Monitor HUD is already running.", kWindowTitle,
                    MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleton);
        return 0;
    }

    if (!RegisterWindowClasses(instance)) {
        CloseHandle(singleton);
        return 1;
    }

    AppState state;
    state.settingsPath = codex_monitor::DefaultSettingsPath();
    state.settings = codex_monitor::LoadSettingsFile(state.settingsPath);
    const UINT systemDpi = GetDpiForSystem();
    const codex_monitor::WindowPlacement placement = InitialWindowPlacement(state, systemDpi);
    state.settings.windowPlacement = placement;

    const DWORD windowStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                              WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_VSCROLL;
    const DWORD extendedStyle = WS_EX_APPWINDOW |
                                (state.settings.alwaysOnTop ? WS_EX_TOPMOST : 0);
    HWND window = CreateWindowExW(
        extendedStyle, kWindowClassName, kWindowTitle, windowStyle,
        placement.x, placement.y, placement.width, placement.height,
        nullptr, nullptr, instance, &state);
    if (!window) {
        CloseHandle(singleton);
        return 1;
    }

    ShowWindow(window, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        const bool handledBySettings =
            state.settingsWindow && IsDialogMessageW(state.settingsWindow, &message);
        if (!handledBySettings && !IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // The window is already gone, so a slow synchronous network teardown can
    // no longer freeze visible UI while the worker finishes its in-flight call.
    state.updateWorker.StopAndJoin();
    CloseHandle(singleton);
    return static_cast<int>(message.wParam);
}
