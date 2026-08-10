#pragma once

#include "update/github_release_selector.h"
#include "update/update_fetch_win32.h"

#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor::update {

enum class WindowsUpdateCheckStatus {
    kUpdateAvailable,
    kUpToDate,
    kInvalidCurrentVersion,
    kFetchFailed,
    kInvalidResponse,
};

struct WindowsUpdateCheckResult {
    WindowsUpdateCheckStatus status =
        WindowsUpdateCheckStatus::kInvalidResponse;
    std::optional<SelectedWindowsRelease> release;
    std::wstring error;

    [[nodiscard]] bool succeeded() const noexcept {
        return status == WindowsUpdateCheckStatus::kUpdateAvailable ||
               status == WindowsUpdateCheckStatus::kUpToDate;
    }
};

WindowsUpdateCheckResult CheckForWindowsUpdate(
    std::string_view currentVersion,
    const WindowsUpdateCancellationCheck& cancelled = {}) noexcept;

WindowsUpdateCheckResult EvaluateWindowsUpdateReleaseJson(
    std::string_view currentVersion,
    std::wstring_view json) noexcept;

}  // namespace codex_monitor::update
