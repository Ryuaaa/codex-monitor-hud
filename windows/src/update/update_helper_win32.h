#pragma once

#include "update/update_apply_transaction_win32.h"
#include "update/update_installed_hud_win32.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor::update {

enum class WindowsUpdateHelperWaitStatus {
    kExited,
    kInvalidHandle,
    kProcessIdentityMismatch,
    kTimedOut,
    kWaitFailed,
    kUnsupportedPlatform,
};

enum class WindowsUpdateHelperStatus {
    kCompletedAndRestarted,
    kInvalidInput,
    kPublisherNotConfigured,
    kOldProcessRejected,
    kOldProcessWaitTimedOut,
    kOldProcessWaitFailed,
    kInstallFailedAndPreviousVersionRestarted,
    kInstallRejectedOrFailed,
    kInstalledExecutableRejected,
    kRestartFailed,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsUpdateHelperRequest {
    // This must be a real inherited process handle, not a PID-derived lookup.
    // The helper consumes and closes it on Windows. The PID and creation-time
    // ticks are independently checked before waiting, preventing PID reuse or
    // an unrelated inherited handle from authorizing installation.
    std::uintptr_t inheritedOldProcessHandle = 0;
    std::uint32_t expectedOldProcessId = 0;
    std::uint64_t expectedOldProcessCreationTime = 0;
    std::chrono::milliseconds oldProcessExitTimeout{120000};

    std::filesystem::path installerPath;
    std::string expectedInstallerFileName;
    std::string sha256Manifest;
    // Used only for verified recovery when the MSI transaction fails after
    // the old HUD has exited.
    std::string previousVersion;
    std::string expectedVersion;
    std::optional<PublisherCertificateSha256> trustedPublisherFingerprint;
    std::filesystem::path installedExecutablePath;
};

struct WindowsUpdateHelperResult {
    WindowsUpdateHelperStatus status =
        WindowsUpdateHelperStatus::kUnexpected;
    WindowsUpdateHelperWaitStatus waitStatus =
        WindowsUpdateHelperWaitStatus::kWaitFailed;
    WindowsUpdateApplyResult apply;
    WindowsInstalledHudLaunchResult launch;
    WindowsInstalledHudLaunchResult previousVersionRecoveryLaunch;

    [[nodiscard]] bool completedAndRestarted() const noexcept {
        return status ==
            WindowsUpdateHelperStatus::kCompletedAndRestarted;
    }
};

// Runs the non-interactive helper transaction. It first proves that the exact
// inherited old-HUD process has exited, then performs the continuously locked
// MSI transaction, and only after a successful install verifies and starts the
// installed HUD. If installation fails, it attempts to restart the old version
// only after the same publisher and exact previous-version checks pass. It
// never deletes an installed application file itself.
[[nodiscard]] WindowsUpdateHelperResult RunWindowsUpdateHelper(
    const WindowsUpdateHelperRequest& request) noexcept;

// Returns the release publisher pin compiled into a formal Windows build.
// Development builds intentionally return std::nullopt and cannot install.
[[nodiscard]] std::optional<PublisherCertificateSha256>
ConfiguredWindowsUpdatePublisherFingerprint() noexcept;

// Reads the per-user MSI installation folder recorded by the current product.
[[nodiscard]] std::optional<std::filesystem::path>
InstalledWindowsHudExecutablePath() noexcept;

// Recognizes only the internal --codex-monitor-update-helper-v1 mode. Normal
// launches return std::nullopt and continue into the HUD. A recognized helper
// invocation always returns a process exit code and never opens the UI.
[[nodiscard]] std::optional<int> TryRunWindowsUpdateHelperCommandLine()
    noexcept;

#if defined(CODEX_MONITOR_UPDATE_HELPER_TESTING)
using WindowsUpdateHelperWaitOperation =
    std::function<WindowsUpdateHelperWaitStatus()>;
using WindowsUpdateHelperApplyOperation =
    std::function<WindowsUpdateApplyResult()>;
using WindowsUpdateHelperVerifyAndLaunchOperation =
    std::function<WindowsInstalledHudLaunchResult()>;

// Test-only sequencing seam. The production HUD does not expose injected
// wait, installer, verification, or launch callbacks.
[[nodiscard]] WindowsUpdateHelperResult
RunWindowsUpdateHelperSequenceForTesting(
    const WindowsUpdateHelperWaitOperation& waitForOldProcess,
    const WindowsUpdateHelperApplyOperation& applyUpdate,
    const WindowsUpdateHelperVerifyAndLaunchOperation&
        verifyAndLaunchInstalledHud,
    const WindowsUpdateHelperVerifyAndLaunchOperation&
        verifyAndRestartPreviousHud) noexcept;

#ifdef _WIN32
[[nodiscard]] WindowsUpdateHelperWaitStatus
WaitForOldWindowsHudProcessForTesting(
    std::uintptr_t inheritedProcessHandle,
    std::uint32_t expectedProcessId,
    std::uint64_t expectedCreationTime,
    const std::filesystem::path& expectedExecutablePath,
    std::chrono::milliseconds timeout) noexcept;
#endif
#endif

}  // namespace codex_monitor::update
