#pragma once

#include "codex/weekly_quota_alert_delivery.h"

#include <windows.h>

namespace codex_monitor::codex {

// Classic Shell notifications work for both MSI-installed and portable HUD
// builds. Registering the notification icon proves that a non-activating
// Windows notification facility is available before delivery state is saved.
class WeeklyQuotaNotificationWin32 {
public:
    WeeklyQuotaNotificationWin32() = default;
    ~WeeklyQuotaNotificationWin32();

    WeeklyQuotaNotificationWin32(const WeeklyQuotaNotificationWin32&) = delete;
    WeeklyQuotaNotificationWin32& operator=(
        const WeeklyQuotaNotificationWin32&) = delete;

    [[nodiscard]] bool Start(HWND owner, HICON icon) noexcept;
    void Stop() noexcept;
    [[nodiscard]] bool available() const noexcept { return registered_; }
    [[nodiscard]] bool Show(
        const WeeklyQuotaAlertNotification& notification) noexcept;

private:
    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool registered_ = false;
};

}  // namespace codex_monitor::codex
