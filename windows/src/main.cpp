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

#include "performance_snapshot.h"
#include "snapshot_math.h"
#include "windows_sampler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace {

constexpr wchar_t kWindowClassName[] = L"CodexMonitorHUDWindowsWindow";
constexpr wchar_t kWindowTitle[] = L"Codex Monitor HUD - Windows performance preview";
constexpr wchar_t kSingletonName[] = L"Local\\CodexMonitorHUDWindowsFoundation";

constexpr int kPinButtonId = 1001;
constexpr int kMinimizeButtonId = 1002;
constexpr UINT_PTR kSampleTimerId = 2001;
constexpr UINT kSampleIntervalMs = 5000;
constexpr int kMinimumWidth = 390;
constexpr int kMinimumHeight = 600;

struct AppState {
    HWND heading = nullptr;
    HWND subtitle = nullptr;
    HWND status = nullptr;
    HWND pinButton = nullptr;
    HWND minimizeButton = nullptr;
    std::array<HWND, 4> moduleCards{};
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH cardBrush = nullptr;
    bool alwaysOnTop = true;
    bool samplingPaused = false;
    bool timerActive = false;
    codex_monitor::WindowsSampler sampler;
    codex_monitor::PerformanceSnapshot latestSnapshot;
};

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
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void DeleteFonts(AppState& state) {
    if (state.headingFont) DeleteObject(state.headingFont);
    if (state.bodyFont) DeleteObject(state.bodyFont);
    if (state.smallFont) DeleteObject(state.smallFont);
    state.headingFont = nullptr;
    state.bodyFont = nullptr;
    state.smallFont = nullptr;
}

void RecreateFonts(HWND window, AppState& state) {
    DeleteFonts(state);
    state.headingFont = CreateUiFont(window, 17, FW_SEMIBOLD);
    state.bodyFont = CreateUiFont(window, 10, FW_MEDIUM);
    state.smallFont = CreateUiFont(window, 9, FW_NORMAL);

    ApplyFont(state.heading, state.headingFont);
    ApplyFont(state.subtitle, state.smallFont);
    ApplyFont(state.status, state.smallFont);
    ApplyFont(state.pinButton, state.smallFont);
    ApplyFont(state.minimizeButton, state.smallFont);
    for (HWND card : state.moduleCards) ApplyFont(card, state.bodyFont);
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
    if (!snapshot.raw.processListAvailable) {
        output << L"Process list unavailable\r\n";
    } else if (snapshot.topMemoryProcesses.empty()) {
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

void UpdateModuleCards(AppState& state) {
    const std::array<std::wstring, 4> text = {
        BuildTargetCardText(state.latestSnapshot),
        BuildSystemCardText(state.latestSnapshot),
        BuildCommitCardText(state.latestSnapshot),
        BuildRankingCardText(state.latestSnapshot),
    };
    for (std::size_t index = 0; index < text.size(); ++index) {
        SetWindowTextW(state.moduleCards[index], text[index].c_str());
    }
}

void UpdateStatusText(AppState& state) {
    std::wostringstream output;
    output << L"Always on top: " << (state.alwaysOnTop ? L"On" : L"Off") << L"  |  Sampler: ";
    if (state.samplingPaused) {
        output << L"paused while minimized";
    } else if (state.timerActive) {
        output << L"5 seconds";
    } else {
        output << L"timer unavailable";
    }
    if (state.latestSnapshot.unreadableProcessMetricCount > 0) {
        output << L"  |  Metric gaps: " << state.latestSnapshot.unreadableProcessMetricCount;
    }
    SetWindowTextW(state.status, output.str().c_str());
}

void UpdateTopmostState(HWND window, AppState& state, bool enabled) {
    state.alwaysOnTop = enabled;
    SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowTextW(state.pinButton, enabled ? L"Unpin" : L"Pin on top");
    UpdateStatusText(state);
}

void LayoutControls(HWND window, AppState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = ScaleForDpi(window, 16);
    const int gap = ScaleForDpi(window, 10);
    const int buttonWidth = ScaleForDpi(window, 86);
    const int buttonHeight = ScaleForDpi(window, 30);

    const int buttonsWidth = buttonWidth * 2 + gap;
    const int textWidth = std::max(ScaleForDpi(window, 120), width - margin * 2 - buttonsWidth - gap);
    MoveWindow(state.heading, margin, margin, textWidth, ScaleForDpi(window, 28), TRUE);
    MoveWindow(state.pinButton, width - margin - buttonsWidth, margin, buttonWidth, buttonHeight, TRUE);
    MoveWindow(state.minimizeButton, width - margin - buttonWidth, margin, buttonWidth, buttonHeight, TRUE);

    int y = margin + ScaleForDpi(window, 34);
    MoveWindow(state.subtitle, margin, y, width - margin * 2, ScaleForDpi(window, 22), TRUE);
    y += ScaleForDpi(window, 26);
    MoveWindow(state.status, margin, y, width - margin * 2, ScaleForDpi(window, 22), TRUE);
    y += ScaleForDpi(window, 30);

    const bool useTwoColumns = width >= ScaleForDpi(window, 700);
    const int columns = useTwoColumns ? 2 : 1;
    const int rows = useTwoColumns ? 2 : 4;
    const int cardWidth = (width - margin * 2 - gap * (columns - 1)) / columns;
    const int availableHeight = std::max(ScaleForDpi(window, 340), height - y - margin);
    const int cardHeight = std::max(ScaleForDpi(window, 92),
                                    (availableHeight - gap * (rows - 1)) / rows);

    for (int index = 0; index < static_cast<int>(state.moduleCards.size()); ++index) {
        const int row = index / columns;
        const int column = index % columns;
        const int x = margin + column * (cardWidth + gap);
        const int cardY = y + row * (cardHeight + gap);
        MoveWindow(state.moduleCards[index], x, cardY, cardWidth, cardHeight, TRUE);
    }
}

HWND CreateLabel(HWND parent, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

bool CreateControls(HWND window, AppState& state) {
    state.backgroundBrush = CreateSolidBrush(RGB(20, 24, 32));
    state.cardBrush = CreateSolidBrush(RGB(31, 38, 50));
    state.heading = CreateLabel(window, L"Codex Monitor HUD", SS_LEFT | SS_CENTERIMAGE);
    state.subtitle = CreateLabel(window, L"Windows native metrics - milestone 2",
                                 SS_LEFT | SS_CENTERIMAGE);
    state.status = CreateLabel(window, L"Starting sampler", SS_LEFT | SS_CENTERIMAGE);

    state.pinButton = CreateWindowExW(0, L"BUTTON", L"Unpin",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 0, 0, window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPinButtonId)),
                                      GetModuleHandleW(nullptr), nullptr);
    state.minimizeButton = CreateWindowExW(0, L"BUTTON", L"Minimize",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           0, 0, 0, 0, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMinimizeButtonId)),
                                           GetModuleHandleW(nullptr), nullptr);

    constexpr std::array<const wchar_t*, 4> initialLabels = {
        L"CODEX / CHATGPT PROCESS TREE\r\nStarting sampler",
        L"SYSTEM CPU & PHYSICAL MEMORY\r\nStarting sampler",
        L"COMMIT & PAGE FILE\r\nStarting sampler",
        L"TOP 5 PROCESSES BY WORKING SET\r\nStarting sampler",
    };
    for (std::size_t index = 0; index < initialLabels.size(); ++index) {
        state.moduleCards[index] = CreateLabel(window, initialLabels[index], SS_LEFT | WS_BORDER);
    }

    if (!state.backgroundBrush || !state.cardBrush || !state.heading || !state.subtitle ||
        !state.status || !state.pinButton || !state.minimizeButton) {
        return false;
    }
    for (HWND card : state.moduleCards) {
        if (!card) return false;
    }

    RecreateFonts(window, state);
    UpdateTopmostState(window, state, true);
    LayoutControls(window, state);
    return true;
}

bool IsModuleCard(const AppState& state, HWND control) {
    return std::find(state.moduleCards.begin(), state.moduleCards.end(), control) !=
           state.moduleCards.end();
}

void RefreshPerformanceSnapshot(AppState& state) {
    state.latestSnapshot = state.sampler.Sample();
    UpdateModuleCards(state);
    UpdateStatusText(state);
}

void StopSamplingTimer(HWND window, AppState& state) {
    if (state.timerActive) KillTimer(window, kSampleTimerId);
    state.timerActive = false;
}

void StartSamplingTimer(HWND window, AppState& state) {
    if (state.samplingPaused) return;
    state.timerActive = SetTimer(window, kSampleTimerId, kSampleIntervalMs, nullptr) != 0;
    UpdateStatusText(state);
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
            RefreshPerformanceSnapshot(*state);
            StartSamplingTimer(window, *state);
            return 0;

        case WM_COMMAND:
            if (!state || HIWORD(wParam) != BN_CLICKED) break;
            if (LOWORD(wParam) == kPinButtonId) {
                UpdateTopmostState(window, *state, !state->alwaysOnTop);
                return 0;
            }
            if (LOWORD(wParam) == kMinimizeButtonId) {
                ShowWindow(window, SW_MINIMIZE);
                return 0;
            }
            break;

        case WM_TIMER:
            if (state && wParam == kSampleTimerId && !state->samplingPaused) {
                RefreshPerformanceSnapshot(*state);
                return 0;
            }
            break;

        case WM_SIZE:
            if (!state) return 0;
            if (wParam == SIZE_MINIMIZED) {
                StopSamplingTimer(window, *state);
                state->samplingPaused = true;
                state->sampler.ResetCpuBaseline();
                UpdateStatusText(*state);
                return 0;
            }
            LayoutControls(window, *state);
            if (state->samplingPaused) {
                state->samplingPaused = false;
                RefreshPerformanceSnapshot(*state);
                StartSamplingTimer(window, *state);
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
                RecreateFonts(window, *state);
                LayoutControls(window, *state);
            }
            return 0;

        case WM_CTLCOLORSTATIC:
            if (state) {
                HDC device = reinterpret_cast<HDC>(wParam);
                HWND control = reinterpret_cast<HWND>(lParam);
                SetBkMode(device, TRANSPARENT);
                SetTextColor(device, IsModuleCard(*state, control) ? RGB(228, 233, 241)
                                                                  : RGB(203, 211, 223));
                return reinterpret_cast<INT_PTR>(IsModuleCard(*state, control)
                                                     ? state->cardBrush
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
                StopSamplingTimer(window, *state);
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

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&windowClass)) {
        CloseHandle(singleton);
        return 1;
    }

    AppState state;
    const UINT systemDpi = GetDpiForSystem();
    const int width = MulDiv(460, static_cast<int>(systemDpi), 96);
    const int height = MulDiv(680, static_cast<int>(systemDpi), 96);
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int edgeMargin = MulDiv(24, static_cast<int>(systemDpi), 96);
    const int x = workArea.left + edgeMargin;
    const int y = std::max(workArea.top + edgeMargin, workArea.bottom - height - edgeMargin);

    const DWORD windowStyle = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                              WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, kWindowClassName,
                                  kWindowTitle, windowStyle, x, y, width, height,
                                  nullptr, nullptr, instance, &state);
    if (!window) {
        CloseHandle(singleton);
        return 1;
    }

    ShowWindow(window, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    CloseHandle(singleton);
    return static_cast<int>(message.wParam);
}
