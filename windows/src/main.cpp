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

#include "module_state.h"
#include "performance_worker.h"
#include "performance_snapshot.h"
#include "settings_store_win32.h"
#include "snapshot_math.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClassName[] = L"CodexMonitorHUDWindowsWindow";
constexpr wchar_t kSettingsWindowClassName[] = L"CodexMonitorHUDWindowsSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"Codex Monitor HUD - Windows shell preview";
constexpr wchar_t kSingletonName[] = L"Local\\CodexMonitorHUDWindowsFoundation";

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
constexpr UINT_PTR kSampleTimerId = 2001;
constexpr UINT kSampleReadyMessage = WM_APP + 1;
constexpr UINT kFastSampleIntervalMs = 5000;
constexpr ULONGLONG kSlowSampleIntervalMs = 20000;
constexpr int kMinimumWidth = 390;
constexpr int kMinimumHeight = 600;

struct ModuleViews {
    codex_monitor::ModuleId id;
    HWND homeCard = nullptr;
    HWND computerCard = nullptr;
};

struct SettingsRow {
    codex_monitor::ModuleId id;
    HWND visibleCheck = nullptr;
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
    ULONGLONG nextSlowSampleTick = 0;
    std::filesystem::path settingsPath;
    codex_monitor::SettingsState settings = codex_monitor::DefaultSettings();
    codex_monitor::PerformanceWorker performanceWorker;
    codex_monitor::PerformanceSnapshot latestSnapshot;
};

void LayoutControls(HWND window, AppState& state);
void LayoutSettingsControls(HWND window, AppState& state);
void UpdateSamplingDemand(HWND window, AppState& state);
void PersistSettings(AppState& state);
void RefreshSettingsControls(AppState& state);

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
        ApplyFont(views.computerCard, state.bodyFont);
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
    ApplyFont(state.settingsCloseButton, state.settingsSmallFont);
    for (const SettingsRow& row : state.settingsRows) {
        ApplyFont(row.visibleCheck, state.settingsSmallFont);
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

std::wstring BuildModuleCardText(codex_monitor::ModuleId id,
                                 const codex_monitor::PerformanceSnapshot& snapshot) {
    switch (id) {
        case codex_monitor::ModuleId::kTargetProcessTree:
            return BuildTargetCardText(snapshot);
        case codex_monitor::ModuleId::kSystemResources:
            return BuildSystemCardText(snapshot);
        case codex_monitor::ModuleId::kCommitAndPageFile:
            return BuildCommitCardText(snapshot);
        case codex_monitor::ModuleId::kTopMemoryProcesses:
            return BuildRankingCardText(snapshot);
    }
    return L"Unavailable module";
}

void UpdateModuleCards(AppState& state) {
    for (const ModuleViews& views : state.moduleViews) {
        const std::wstring text = BuildModuleCardText(views.id, state.latestSnapshot);
        if (state.settings.currentPage == codex_monitor::Page::kHome &&
            state.settings.homeVisible[codex_monitor::ModuleIndex(views.id)]) {
            SetWindowTextW(views.homeCard, text.c_str());
        }
        if (state.settings.currentPage == codex_monitor::Page::kComputer) {
            SetWindowTextW(views.computerCard, text.c_str());
        }
    }
}

bool HomeNeedsPerformance(const codex_monitor::SettingsState& settings) {
    for (codex_monitor::ModuleId id : codex_monitor::VisibleHomeModules(settings)) {
        if (codex_monitor::ModuleRegistry()[codex_monitor::ModuleIndex(id)]
                .requiresPerformanceSampling) {
            return true;
        }
    }
    return false;
}

bool CurrentPageNeedsPerformance(const AppState& state) {
    switch (state.settings.currentPage) {
        case codex_monitor::Page::kHome:
            return HomeNeedsPerformance(state.settings);
        case codex_monitor::Page::kCodex:
            return false;
        case codex_monitor::Page::kComputer:
            return true;
    }
    return false;
}

void UpdateStatusText(AppState& state) {
    std::wostringstream output;
    output << L"Always on top: " << (state.settings.alwaysOnTop ? L"On" : L"Off")
           << L"  |  Sampler: ";
    if (state.windowMinimized) {
        output << L"paused while minimized";
    } else if (!CurrentPageNeedsPerformance(state)) {
        output << L"paused; current page has no visible performance modules";
    } else if (state.performanceWorker.IsBusy()) {
        output << L"refreshing in background";
        if (state.timerActive) output << L"; 5 s fast / 20 s full";
    } else if (state.timerActive) {
        output << L"5 s fast / 20 s full";
    } else {
        output << L"timer unavailable";
    }
    if (state.hasPerformanceSnapshot && state.latestSnapshot.unreadableProcessMetricCount > 0) {
        output << L"  |  Metric gaps: " << state.latestSnapshot.unreadableProcessMetricCount;
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
        if (state.settings.currentPage == codex_monitor::Page::kHome &&
            state.settings.homeVisible[codex_monitor::ModuleIndex(views.id)]) {
            SetWindowTextW(views.homeCard, message);
        }
        if (state.settings.currentPage == codex_monitor::Page::kComputer) {
            SetWindowTextW(views.computerCard, message);
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
    } else if (state.settings.currentPage == codex_monitor::Page::kComputer) {
        for (const codex_monitor::ModuleDefinition& definition :
             codex_monitor::ModuleRegistry()) {
            if (ModuleViews* views = FindModuleViews(state, definition.id)) {
                cards.push_back(views->computerCard);
            }
        }
    }
    return cards;
}

void UpdatePageVisibility(AppState& state) {
    const bool isHome = state.settings.currentPage == codex_monitor::Page::kHome;
    const bool isCodex = state.settings.currentPage == codex_monitor::Page::kCodex;
    const bool isComputer = state.settings.currentPage == codex_monitor::Page::kComputer;

    for (ModuleViews& views : state.moduleViews) {
        const bool homeVisible =
            isHome && state.settings.homeVisible[codex_monitor::ModuleIndex(views.id)];
        ShowWindow(views.homeCard, homeVisible ? SW_SHOW : SW_HIDE);
        ShowWindow(views.computerCard, isComputer ? SW_SHOW : SW_HIDE);
    }

    ShowWindow(state.codexNotice, isCodex ? SW_SHOW : SW_HIDE);
    const bool homeEmpty = isHome && codex_monitor::VisibleHomeModules(state.settings).empty();
    ShowWindow(state.emptyHomeNotice, homeEmpty ? SW_SHOW : SW_HIDE);

    CheckRadioButton(state.mainWindow, kHomePageButtonId, kComputerPageButtonId,
                     isHome ? kHomePageButtonId
                            : (isCodex ? kCodexPageButtonId : kComputerPageButtonId));

    if (isHome) {
        SetWindowTextW(state.subtitle, L"Home - selected local performance modules");
    } else if (isCodex) {
        SetWindowTextW(state.subtitle, L"Codex - local account data is outside this milestone");
    } else {
        SetWindowTextW(state.subtitle, L"Computer performance - Windows native metrics");
    }
}

void LayoutCardGrid(HWND window, const std::vector<HWND>& cards,
                    int y, int width, int height, int margin, int gap) {
    if (cards.empty()) return;
    const bool useTwoColumns = width >= ScaleForDpi(window, 700) && cards.size() > 1;
    const int columns = useTwoColumns ? 2 : 1;
    const int rows = (static_cast<int>(cards.size()) + columns - 1) / columns;
    const int cardWidth = (width - margin * 2 - gap * (columns - 1)) / columns;
    const int availableHeight = std::max(ScaleForDpi(window, 120), height - y - margin);
    const int cardHeight = std::max(ScaleForDpi(window, 92),
                                    (availableHeight - gap * (rows - 1)) / rows);
    for (std::size_t index = 0; index < cards.size(); ++index) {
        const int row = static_cast<int>(index) / columns;
        const int column = static_cast<int>(index) % columns;
        const int x = margin + column * (cardWidth + gap);
        const int cardY = y + row * (cardHeight + gap);
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
    LayoutCardGrid(window, cards, y, width, height, margin, ScaleForDpi(window, 10));
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
        L"本机 Codex 数据尚未连接\r\n\r\n"
        L"This Windows milestone does not simulate account, usage, task, token, "
        L"service-status, or cost data.",
        SS_LEFT | WS_BORDER);
    state.emptyHomeNotice = CreateLabel(
        window,
        L"No modules are shown on Home.\r\n\r\nOpen Settings to choose one or more "
        L"local performance modules.",
        SS_LEFT | WS_BORDER);

    for (const codex_monitor::ModuleDefinition& definition :
         codex_monitor::ModuleRegistry()) {
        ModuleViews views;
        views.id = definition.id;
        views.homeCard = CreateLabel(window, L"Starting sampler", SS_LEFT | WS_BORDER);
        views.computerCard = CreateLabel(window, L"Starting sampler", SS_LEFT | WS_BORDER);
        state.moduleViews.push_back(views);
    }

    if (!state.backgroundBrush || !state.cardBrush || !state.heading || !state.subtitle ||
        !state.status || !state.pinButton || !state.minimizeButton ||
        !state.homePageButton || !state.codexPageButton || !state.computerPageButton ||
        !state.settingsButton || !state.codexNotice || !state.emptyHomeNotice) {
        return false;
    }
    for (const ModuleViews& views : state.moduleViews) {
        if (!views.homeCard || !views.computerCard) return false;
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
        if (control == views.homeCard || control == views.computerCard) return true;
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

void SwitchPage(HWND window, AppState& state, codex_monitor::Page page) {
    if (state.settings.currentPage == page) return;
    state.settings.currentPage = page;
    UpdatePageVisibility(state);
    if (state.hasPerformanceSnapshot) UpdateModuleCards(state);
    LayoutControls(window, state);
    UpdateSamplingDemand(window, state);
    PersistSettings(state);
}

void RefreshSettingsControls(AppState& state) {
    if (!state.settingsWindow) return;
    state.settings.homeOrder = codex_monitor::SanitizeHomeOrder(state.settings.homeOrder);
    for (std::size_t position = 0; position < state.settings.homeOrder.size(); ++position) {
        const codex_monitor::ModuleId id = state.settings.homeOrder[position];
        SettingsRow* row = FindSettingsRow(state, id);
        if (!row) continue;
        SendMessageW(row->visibleCheck, BM_SETCHECK,
                     state.settings.homeVisible[codex_monitor::ModuleIndex(id)]
                         ? BST_CHECKED
                         : BST_UNCHECKED,
                     0);
        EnableWindow(row->moveUpButton, position > 0);
        EnableWindow(row->moveDownButton, position + 1 < state.settings.homeOrder.size());
    }
    SendMessageW(state.settingsTopmostCheck, BM_SETCHECK,
                 state.settings.alwaysOnTop ? BST_CHECKED : BST_UNCHECKED, 0);
    LayoutSettingsControls(state.settingsWindow, state);
}

void LayoutSettingsControls(HWND window, AppState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int margin = ScaleForDpi(window, 16);
    const int gap = ScaleForDpi(window, 8);
    const int rowHeight = ScaleForDpi(window, 30);
    const int smallButtonWidth = ScaleForDpi(window, 58);
    const int controlsWidth = smallButtonWidth * 2 + gap;
    const int checkboxWidth = std::max(ScaleForDpi(window, 130),
                                       width - margin * 2 - controlsWidth - gap);

    int y = margin;
    MoveWindow(state.settingsHeading, margin, y, width - margin * 2,
               ScaleForDpi(window, 24), TRUE);
    y += ScaleForDpi(window, 34);
    for (codex_monitor::ModuleId id : state.settings.homeOrder) {
        SettingsRow* row = FindSettingsRow(state, id);
        if (!row) continue;
        MoveWindow(row->visibleCheck, margin, y, checkboxWidth, rowHeight, TRUE);
        MoveWindow(row->moveUpButton, width - margin - controlsWidth, y,
                   smallButtonWidth, rowHeight, TRUE);
        MoveWindow(row->moveDownButton, width - margin - smallButtonWidth, y,
                   smallButtonWidth, rowHeight, TRUE);
        y += rowHeight + gap;
    }
    y += ScaleForDpi(window, 4);
    MoveWindow(state.settingsTopmostCheck, margin, y,
               width - margin * 2, rowHeight, TRUE);
    y += rowHeight + ScaleForDpi(window, 12);
    MoveWindow(state.settingsCloseButton, width - margin - ScaleForDpi(window, 82), y,
               ScaleForDpi(window, 82), rowHeight, TRUE);
}

bool CreateSettingsControls(HWND window, AppState& state) {
    state.settingsHeading = CreateLabel(
        window, L"Home modules - visibility and order", SS_LEFT | SS_CENTERIMAGE);
    state.settingsRows.clear();
    for (const codex_monitor::ModuleDefinition& definition :
         codex_monitor::ModuleRegistry()) {
        const int index = static_cast<int>(codex_monitor::ModuleIndex(definition.id));
        SettingsRow row;
        row.id = definition.id;
        row.visibleCheck = CreateWindowExW(
            0, L"BUTTON", std::wstring(definition.displayName).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsVisibleBaseId + index)),
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
    state.settingsCloseButton = CreateButton(window, L"Close", kSettingsCloseId);

    if (!state.settingsHeading || !state.settingsTopmostCheck || !state.settingsCloseButton) {
        return false;
    }
    for (const SettingsRow& row : state.settingsRows) {
        if (!row.visibleCheck || !row.moveUpButton || !row.moveDownButton) return false;
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

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    const int width = ScaleForDpi(owner, 500);
    const int height = ScaleForDpi(owner, 390);
    const int ownerWidth = static_cast<int>(ownerRect.right - ownerRect.left);
    const int x = static_cast<int>(ownerRect.left) +
                  std::max(0, (ownerWidth - width) / 2);
    const int y = static_cast<int>(ownerRect.top) + ScaleForDpi(owner, 48);
    state.settingsWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kSettingsWindowClassName, L"Codex Monitor HUD Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
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
                        SendMessageW(row->visibleCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    UpdatePageVisibility(*state);
                    if (state->hasPerformanceSnapshot) UpdateModuleCards(*state);
                    LayoutControls(state->mainWindow, *state);
                    UpdateSamplingDemand(state->mainWindow, *state);
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
            if (controlId == kSettingsCloseId) {
                DestroyWindow(window);
                return 0;
            }
            break;
        }

        case WM_SIZE:
            if (state && wParam != SIZE_MINIMIZED) LayoutSettingsControls(window, *state);
            return 0;

        case WM_GETMINMAXINFO:
            if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam)) {
                limits->ptMinTrackSize.x = ScaleForDpi(window, 430);
                limits->ptMinTrackSize.y = ScaleForDpi(window, 360);
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
                state->settingsCloseButton = nullptr;
                state->settingsRows.clear();
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
            UpdateSamplingDemand(window, *state);
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
            return 0;

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
                limits->ptMinTrackSize.x = ScaleForDpi(window, kMinimumWidth);
                limits->ptMinTrackSize.y = ScaleForDpi(window, kMinimumHeight);
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
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = WindowProcedure;
    mainClass.hInstance = instance;
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
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
                              WS_MINIMIZEBOX | WS_CLIPCHILDREN;
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

    CloseHandle(singleton);
    return static_cast<int>(message.wParam);
}
