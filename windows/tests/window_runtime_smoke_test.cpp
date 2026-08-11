#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ChildInventory {
    std::vector<std::wstring> visibleTexts;
    std::size_t visibleCount = 0;
};

struct WindowQuery {
    DWORD processId = 0;
    const wchar_t* expectedClass = nullptr;
    HWND result = nullptr;
};

BOOL CALLBACK FindOwnedTopLevelWindow(HWND window, LPARAM context) {
    auto* query = reinterpret_cast<WindowQuery*>(context);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != query->processId) return TRUE;
    wchar_t className[128]{};
    if (GetClassNameW(window, className, 128) <= 0) return TRUE;
    if (std::wstring(className) != query->expectedClass) return TRUE;
    query->result = window;
    return FALSE;
}

BOOL CALLBACK CollectVisibleChild(HWND child, LPARAM context) {
    auto* inventory = reinterpret_cast<ChildInventory*>(context);
    if (!IsWindowVisible(child)) return TRUE;
    ++inventory->visibleCount;
    const int length = GetWindowTextLengthW(child);
    if (length <= 0 || length > 8'192) return TRUE;
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(child, text.data(), length + 1);
    if (copied > 0) {
        text.resize(static_cast<std::size_t>(copied));
        inventory->visibleTexts.push_back(std::move(text));
    }
    return TRUE;
}

HWND WaitForWindow(DWORD processId,
                   const wchar_t* className,
                   std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        WindowQuery query{processId, className, nullptr};
        EnumWindows(FindOwnedTopLevelWindow,
                    reinterpret_cast<LPARAM>(&query));
        if (query.result) return query.result;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return nullptr;
}

bool ContainsText(const ChildInventory& inventory, const wchar_t* expected) {
    return std::find(inventory.visibleTexts.begin(),
                     inventory.visibleTexts.end(),
                     std::wstring(expected)) != inventory.visibleTexts.end();
}

HWND FindVisibleChildWithText(HWND parent, const wchar_t* expected) {
    HWND child = nullptr;
    while ((child = FindWindowExW(parent, child, nullptr, nullptr)) != nullptr) {
        if (!IsWindowVisible(child)) continue;
        wchar_t text[128]{};
        if (GetWindowTextW(child, text, 128) > 0 &&
            std::wstring(text) == expected) {
            return child;
        }
    }
    return nullptr;
}

void Fail(bool condition, const char* message, int& failures) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || argv[1] == nullptr || *argv[1] == L'\0') {
        std::cerr << "usage: window_runtime_smoke_test <hud.exe>\n";
        return 2;
    }

    std::wstring commandLine = L"\"" + std::wstring(argv[1]) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(argv[1], commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
        std::cerr << "FAIL: could not launch HUD, error="
                  << GetLastError() << '\n';
        return 1;
    }

    CloseHandle(process.hThread);
    int failures = 0;
    WaitForInputIdle(process.hProcess, 10'000);
    HWND window = WaitForWindow(
        process.dwProcessId, L"CodexMonitorHUDWindowsWindow",
        std::chrono::seconds(15));
    Fail(window != nullptr, "the HUD must create its main window", failures);

    if (window) {
        const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
        const LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
        Fail((style & WS_CAPTION) != 0 && (style & WS_THICKFRAME) != 0 &&
                 (style & WS_MINIMIZEBOX) != 0,
             "the HUD must keep standard caption, resize, and minimize behavior",
             failures);
        Fail((style & WS_MAXIMIZEBOX) == 0,
             "the fixed-aspect HUD must not advertise maximize", failures);
        Fail((extended & WS_EX_APPWINDOW) != 0,
             "the HUD must appear in the taskbar", failures);
        Fail(IsWindowVisible(window) != FALSE,
             "the main window must be visible", failures);
        RECT client{};
        Fail(GetClientRect(window, &client) != FALSE &&
                 client.right > client.left && client.bottom > client.top,
             "the main window must have a positive client area", failures);
        Fail(GetDpiForWindow(window) >= 96,
             "the HUD must expose a valid runtime DPI", failures);

        ChildInventory inventory;
        EnumChildWindows(window, CollectVisibleChild,
                         reinterpret_cast<LPARAM>(&inventory));
        Fail(inventory.visibleCount >= 12,
             "the main page must expose its navigation and visible cards",
             failures);
        for (const wchar_t* expected :
             {L"Codex Monitor HUD", L"Home", L"Codex", L"Computer",
              L"Settings", L"Minimize"}) {
            Fail(ContainsText(inventory, expected),
                 "a required visible shell control is missing", failures);
        }

        if (HWND computer = FindVisibleChildWithText(window, L"Computer")) {
            SendMessageW(computer, BM_CLICK, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ChildInventory computerPage;
            EnumChildWindows(window, CollectVisibleChild,
                             reinterpret_cast<LPARAM>(&computerPage));
            bool hasPerformanceCard = false;
            for (const std::wstring& text : computerPage.visibleTexts) {
                if (text.find(L"SYSTEM + CODEX/CHATGPT") !=
                        std::wstring::npos ||
                    text.find(L"Starting sampler") != std::wstring::npos) {
                    hasPerformanceCard = true;
                    break;
                }
            }
            Fail(hasPerformanceCard,
                 "the Computer page must render a performance card",
                 failures);
        } else {
            Fail(false, "the Computer navigation control must be clickable",
                 failures);
        }

        if (HWND settings = FindVisibleChildWithText(window, L"Settings")) {
            SendMessageW(settings, BM_CLICK, 0, 0);
            HWND settingsWindow = WaitForWindow(
                process.dwProcessId, L"CodexMonitorHUDWindowsSettingsWindow",
                std::chrono::seconds(5));
            Fail(settingsWindow != nullptr,
                 "the Settings button must open the settings window",
                 failures);
            if (settingsWindow) PostMessageW(settingsWindow, WM_CLOSE, 0, 0);
        } else {
            Fail(false, "the Settings control must be clickable", failures);
        }

        if (HWND minimize = FindVisibleChildWithText(window, L"Minimize")) {
            SendMessageW(minimize, BM_CLICK, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            Fail(IsIconic(window) != FALSE,
                 "the Minimize button must use true taskbar minimization",
                 failures);
            ShowWindow(window, SW_RESTORE);
        } else {
            Fail(false, "the Minimize control must be clickable", failures);
        }

        PostMessageW(window, WM_CLOSE, 0, 0);
    }

    DWORD wait = WaitForSingleObject(process.hProcess, 15'000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 3);
        WaitForSingleObject(process.hProcess, 5'000);
        Fail(false, "the HUD must stop cleanly after WM_CLOSE", failures);
    } else {
        DWORD exitCode = 0;
        Fail(GetExitCodeProcess(process.hProcess, &exitCode) != FALSE &&
                 exitCode == 0,
             "the HUD smoke run must exit successfully", failures);
    }
    CloseHandle(process.hProcess);

    if (failures != 0) return 1;
    std::cout << "windows_window_runtime_smoke=pass\n";
    return 0;
}
