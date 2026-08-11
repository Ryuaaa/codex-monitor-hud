#include "codex/weekly_quota_notification_win32.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <iterator>

namespace codex_monitor::codex {
namespace {

constexpr UINT kWeeklyQuotaNotificationIconId = 0x434D5701;

[[nodiscard]] const wchar_t* PeriodLabel(WeeklyQuotaAlertMode mode) noexcept {
    return mode == WeeklyQuotaAlertMode::kNaturalDay
               ? L"今天"
               : L"最近24小时";
}

void CopyBounded(wchar_t* destination,
                 std::size_t destinationCount,
                 const wchar_t* source) noexcept {
    if (!destination || destinationCount == 0) return;
    const int copied = swprintf_s(destination, destinationCount, L"%ls", source);
    if (copied < 0) destination[0] = L'\0';
}

}  // namespace

WeeklyQuotaNotificationWin32::~WeeklyQuotaNotificationWin32() {
    Stop();
}

bool WeeklyQuotaNotificationWin32::Start(HWND owner, HICON icon) noexcept {
    if (registered_ && owner_ == owner) return true;
    Stop();
    if (!owner || !IsWindow(owner)) return false;

    owner_ = owner;
    icon_ = icon;
    if (!icon_) {
        icon_ = LoadIconW(nullptr, IDI_INFORMATION);
    }
    if (!icon_) {
        owner_ = nullptr;
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kWeeklyQuotaNotificationIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = icon_;
    CopyBounded(data.szTip, std::size(data.szTip), L"Codex Monitor HUD");
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        owner_ = nullptr;
        icon_ = nullptr;
        return false;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    static_cast<void>(Shell_NotifyIconW(NIM_SETVERSION, &data));
    registered_ = true;
    return true;
}

void WeeklyQuotaNotificationWin32::Stop() noexcept {
    if (registered_ && owner_) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner_;
        data.uID = kWeeklyQuotaNotificationIconId;
        static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &data));
    }
    owner_ = nullptr;
    icon_ = nullptr;
    registered_ = false;
}

bool WeeklyQuotaNotificationWin32::Show(
    const WeeklyQuotaAlertNotification& notification) noexcept {
    if (!registered_ || !owner_ || !IsWindow(owner_) ||
        !std::isfinite(notification.consumedPercent) ||
        !std::isfinite(notification.thresholdPercent)) {
        return false;
    }

    const int consumed = std::clamp(
        static_cast<int>(std::lround(notification.consumedPercent)), 0, 100);
    const int threshold = std::clamp(
        static_cast<int>(std::lround(notification.thresholdPercent)), 5, 100);

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kWeeklyQuotaNotificationIconId;
    data.uFlags = NIF_INFO;
    CopyBounded(data.szInfoTitle, std::size(data.szInfoTitle),
                L"Codex 周额度提醒");
    swprintf_s(data.szInfo, std::size(data.szInfo),
               L"%ls已使用约 %d%%，达到你设置的 %d%% 阈值。",
               PeriodLabel(notification.mode), consumed, threshold);
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND | NIIF_RESPECT_QUIET_TIME;
    // Shell balloon/toast notifications never activate or foreground owner_.
    if (Shell_NotifyIconW(NIM_MODIFY, &data)) return true;
    // Explorer may have restarted after the icon was registered. Mark the
    // facility unavailable so the next five-minute refresh can register it
    // again instead of remembering this attempt as delivered.
    static_cast<void>(Shell_NotifyIconW(NIM_DELETE, &data));
    registered_ = false;
    return false;
}

}  // namespace codex_monitor::codex
