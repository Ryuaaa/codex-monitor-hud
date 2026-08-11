#pragma once

#include "update/update_msi_identity_win32.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor::update {

inline constexpr std::wstring_view kWindowsHudExecutableName =
    L"CodexMonitorHUD.exe";
inline constexpr std::wstring_view kWindowsHudProductName =
    L"Codex Monitor HUD";

struct WindowsHudExecutableIdentity {
    std::wstring productName;
    std::wstring originalFilename;
    std::wstring fileVersion;
    std::wstring productVersion;
    unsigned int majorVersion = 0;
    unsigned int minorVersion = 0;
    unsigned int patchVersion = 0;
};

enum class WindowsHudExecutableIdentityStatus {
    kValid,
    kInvalidExpectedVersion,
    kProductNameMismatch,
    kOriginalFilenameMismatch,
    kFileVersionMismatch,
    kProductVersionMismatch,
    kFixedVersionMismatch,
};

// Applies the post-install executable identity policy without touching a file.
// The expected version uses the same canonical three-part limits as the MSI.
[[nodiscard]] WindowsHudExecutableIdentityStatus
ValidateWindowsHudExecutableIdentity(
    const WindowsHudExecutableIdentity& identity,
    std::string_view expectedVersion) noexcept;

enum class WindowsInstalledHudLaunchStatus {
    kStarted,
    kInvalidInput,
    kPathNotAbsolute,
    kPathNotCanonical,
    kUnsafePathAncestor,
    kFileOpenFailed,
    kUnsafeFileType,
    kPathIdentityMismatch,
    kSignatureVerificationFailed,
    kSignerCertificateUnavailable,
    kPublisherFingerprintMismatch,
    kVersionResourceUnavailable,
    kExecutableIdentityRejected,
    kProcessStartFailed,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsInstalledHudLaunchResult {
    WindowsInstalledHudLaunchStatus status =
        WindowsInstalledHudLaunchStatus::kUnexpected;
    WindowsHudExecutableIdentityStatus identityStatus =
        WindowsHudExecutableIdentityStatus::kValid;

    [[nodiscard]] bool started() const noexcept {
        return status == WindowsInstalledHudLaunchStatus::kStarted;
    }
};

// Verifies the exact installed HUD executable and launches it without a
// verification-to-execution path replacement gap. The absolute DOS path must
// name CodexMonitorHUD.exe on a local fixed drive. All ancestors and the final
// non-reparse file are held without delete/write sharing while Authenticode,
// the pinned publisher certificate, the signed version resource, and
// CreateProcessW run. An unverified file is never started.
[[nodiscard]] WindowsInstalledHudLaunchResult
VerifyAndLaunchInstalledWindowsHud(
    const std::filesystem::path& installedExecutablePath,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint) noexcept;

}  // namespace codex_monitor::update
