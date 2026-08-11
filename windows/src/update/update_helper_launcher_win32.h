#pragma once

#include "update/update_installed_hud_win32.h"

#include <filesystem>
#include <string>

namespace codex_monitor::update {

enum class WindowsUpdateHelperLauncherStatus {
    kStarted,
    kInvalidInput,
    kPublisherNotConfigured,
    kNotRunningFromInstalledHud,
    kCurrentProcessHandleFailed,
    kHelperCopyFailed,
    kHelperCopyRejectedOrStartFailed,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsUpdateHelperLauncherRequest {
    // The directory is created by the update download stage and must already
    // contain the selected MSI. The temporary helper copy is created here with
    // CREATE_NEW semantics as CodexMonitorHUD.exe.
    std::filesystem::path privateUpdateDirectory;
    std::filesystem::path installerPath;
    std::string installerSha256;
    std::string currentVersion;
    std::string targetVersion;
};

struct WindowsUpdateHelperLauncherResult {
    WindowsUpdateHelperLauncherStatus status =
        WindowsUpdateHelperLauncherStatus::kUnexpected;
    std::filesystem::path helperCopyPath;
    WindowsInstalledHudLaunchResult launch;

    [[nodiscard]] bool started() const noexcept {
        return status == WindowsUpdateHelperLauncherStatus::kStarted;
    }
};

// Copies the currently running, installed HUD into the prepared private update
// directory, re-verifies that copy against the compiled publisher and current
// version, and launches its internal helper mode with only an inherited handle
// to this exact process. The caller may close the HUD only after kStarted.
[[nodiscard]] WindowsUpdateHelperLauncherResult
LaunchPreparedWindowsUpdateHelper(
    const WindowsUpdateHelperLauncherRequest& request) noexcept;

}  // namespace codex_monitor::update
