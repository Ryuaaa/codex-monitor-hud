#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <algorithm>
#include <array>

namespace {

constexpr wchar_t kWindowClassName[] = L"CodexMonitorHUDWindowsWindow";
constexpr wchar_t kWindowTitle[] = L"Codex Monitor HUD - Windows foundation";
constexpr wchar_t kSingletonName[] = L"Local\\CodexMonitorHUDWindowsFoundation";

constexpr int kPinButtonId = 1001;
constexpr int kMinimizeButtonId = 1002;
constexpr int kMinimumWidth = 390;
constexpr int kMinimumHeight = 520;

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
    state.bodyFont = CreateUiFont(window, 11, FW_MEDIUM);
    state.smallFont = CreateUiFont(window, 9, FW_NORMAL);

    ApplyFont(state.heading, state.headingFont);
    ApplyFont(state.subtitle, state.smallFont);
    ApplyFont(state.status, state.smallFont);
    ApplyFont(state.pinButton, state.smallFont);
    ApplyFont(state.minimizeButton, state.smallFont);
    for (HWND card : state.moduleCards) ApplyFont(card, state.bodyFont);
}

void UpdateTopmostState(HWND window, AppState& state, bool enabled) {
    state.alwaysOnTop = enabled;
    SetWindowPos(window, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowTextW(state.pinButton, enabled ? L"Unpin" : L"Pin on top");
    SetWindowTextW(state.status,
                   enabled ? L"Always on top: On  |  UI shell only - no monitoring data"
                           : L"Always on top: Off  |  UI shell only - no monitoring data");
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
    const int availableHeight = std::max(ScaleForDpi(window, 280), height - y - margin);
    const int cardHeight = std::max(ScaleForDpi(window, 72),
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
    state.subtitle = CreateLabel(window, L"Windows native foundation - milestone 1", SS_LEFT | SS_CENTERIMAGE);
    state.status = CreateLabel(window, L"Always on top: On", SS_LEFT | SS_CENTERIMAGE);

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

    constexpr std::array<const wchar_t*, 4> labels = {
        L"CODEX ACCOUNT & QUOTA\r\nPlaceholder - provider not connected",
        L"TASKS & TOKEN USAGE\r\nPlaceholder - provider not connected",
        L"SYSTEM CPU & MEMORY\r\nPlaceholder - sampler not implemented",
        L"TOP APPS & HEALTH\r\nPlaceholder - sampler not implemented",
    };
    for (size_t index = 0; index < labels.size(); ++index) {
        state.moduleCards[index] = CreateLabel(window, labels[index],
                                               SS_LEFT | SS_CENTERIMAGE | WS_BORDER);
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

        case WM_SIZE:
            if (state && wParam != SIZE_MINIMIZED) LayoutControls(window, *state);
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
    const int height = MulDiv(620, static_cast<int>(systemDpi), 96);
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
