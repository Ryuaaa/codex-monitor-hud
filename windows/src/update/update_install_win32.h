#pragma once

#include "update/github_release_selector.h"
#include "update/update_asset_download_win32.h"
#include "update/update_helper_launcher_win32.h"
#include "update/update_installer_verifier_win32.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace codex_monitor::update {

enum class WindowsUpdateInstallStatus {
    kHelperStarted,
    kInvalidInput,
    kCancelled,
    kPublisherNotConfigured,
    kUpdateDirectoryUnavailable,
    kChecksumDownloadFailed,
    kChecksumReadFailed,
    kChecksumRejected,
    kInstallerDownloadFailed,
    kHelperLaunchFailed,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsUpdateInstallRequest {
    SelectedWindowsRelease release;
    std::string currentVersion;
    std::filesystem::path updatesRoot;
};

struct WindowsUpdateInstallResult {
    WindowsUpdateInstallStatus status =
        WindowsUpdateInstallStatus::kUnexpected;
    std::string targetVersion;
    std::filesystem::path privateUpdateDirectory;
    UpdateAssetDownloadFailureKind downloadFailure =
        UpdateAssetDownloadFailureKind::kNone;
    Sha256ManifestParseStatus manifestStatus =
        Sha256ManifestParseStatus::kMalformed;
    WindowsUpdateHelperLauncherStatus launcherStatus =
        WindowsUpdateHelperLauncherStatus::kUnexpected;
    std::wstring error;

    [[nodiscard]] bool helperStarted() const noexcept {
        return status == WindowsUpdateInstallStatus::kHelperStarted;
    }
};

using WindowsUpdateInstallCancellationCheck = std::function<bool()>;
using WindowsUpdatePublisherConfiguredCheck = std::function<bool()>;
using WindowsUpdatePrivateDirectoryCreator =
    std::function<std::optional<std::filesystem::path>(
        const std::filesystem::path& updatesRoot)>;
using WindowsUpdateAssetDownloader = std::function<
    UpdateAssetDownloadResult(
        const GitHubReleaseAsset& asset,
        std::uint64_t maximumBytes,
        const std::filesystem::path& privateDirectory,
        const WindowsUpdateInstallCancellationCheck& cancelled)>;
using WindowsUpdateSmallFileReader =
    std::function<std::optional<std::string>(
        const std::filesystem::path& path,
        std::size_t maximumBytes)>;
using WindowsUpdatePreparedHelperLauncher =
    std::function<WindowsUpdateHelperLauncherResult(
        const WindowsUpdateHelperLauncherRequest& request)>;

struct WindowsUpdateInstallOperations {
    WindowsUpdatePublisherConfiguredCheck publisherConfigured;
    WindowsUpdatePrivateDirectoryCreator createPrivateDirectory;
    WindowsUpdateAssetDownloader downloadAsset;
    WindowsUpdateSmallFileReader readSmallFile;
    WindowsUpdatePreparedHelperLauncher launchHelper;
};

// Runs the fixed update preparation sequence. The injected boundary makes the
// ordering and fail-closed behavior testable without downloading or executing
// an installer: publisher gate, fresh directory, checksum, MSI, then helper.
[[nodiscard]] WindowsUpdateInstallResult RunWindowsUpdateInstallPlan(
    const WindowsUpdateInstallRequest& request,
    const WindowsUpdateInstallOperations& operations,
    const WindowsUpdateInstallCancellationCheck& cancelled = {}) noexcept;

// Production boundary used by the background install worker. A failed or
// cancelled preparation never closes the running HUD and never executes an MSI.
[[nodiscard]] WindowsUpdateInstallResult PrepareAndLaunchWindowsUpdate(
    const WindowsUpdateInstallRequest& request,
    const WindowsUpdateInstallCancellationCheck& cancelled = {}) noexcept;

}  // namespace codex_monitor::update
