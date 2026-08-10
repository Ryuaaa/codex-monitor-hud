#pragma once

#include "update/update_installer_verifier_win32.h"
#include "update/update_msi_identity_win32.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace codex_monitor::update {

enum class WindowsUpdateApplyStatus {
    kInstalled,
    kInvalidInput,
    kPathNotAbsolute,
    kPathNotCanonical,
    kPathResolutionFailed,
    kUnsafePathAncestor,
    kFileOpenFailed,
    kUnsafeFileType,
    kFileIdentityMismatch,
    kChecksumRejected,
    kPublisherOrIdentityRejected,
    kInstallFailed,
    kInstallCallbackThrew,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsUpdateApplyResult {
    WindowsUpdateApplyStatus status = WindowsUpdateApplyStatus::kUnexpected;
    WindowsInstallerVerificationResult checksum;
    WindowsMsiIdentityVerificationResult publisherAndIdentity;
    int installerExitCode = -1;
    bool installAttempted = false;
    bool rebootRequired = false;

    [[nodiscard]] bool installed() const noexcept {
        return status == WindowsUpdateApplyStatus::kInstalled;
    }
};

// Executes the security-sensitive apply boundary as one transaction. The MSI
// path must already be a canonical absolute DOS path on a local fixed drive.
// Every ancestor directory and the exact non-reparse MSI file are opened with
// delete/write replacement denied and remain held continuously while SHA-256,
// Authenticode revocation + publisher pinning, MSI identity, and the injected
// synchronous Windows Installer call returns. Windows Installer UI and reboot
// are suppressed; 0 and 3010 are the only accepted success codes. Installation
// is never attempted after any verification or identity failure.
//
// This production API intentionally exposes neither an install callback nor a
// trust-verifier flag. It cannot return early after launching an asynchronous
// installer, and verification always uses the fixed whole-chain revocation
// policy in VerifyWindowsMsiIdentityAndPublisher.
[[nodiscard]] WindowsUpdateApplyResult ApplyVerifiedWindowsMsiUpdate(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint) noexcept;

#if defined(CODEX_MONITOR_UPDATE_APPLY_TRANSACTION_TESTING)
// The injected callback exists only in the dedicated test binary, allowing it
// to prove that locks remain held through the synchronous boundary. The HUD is
// compiled without this declaration and cannot substitute an asynchronous
// launcher for the production Windows Installer call.
using SynchronousWindowsMsiInstallCallback =
    std::function<int(const std::filesystem::path& verifiedInstallerPath)>;

[[nodiscard]] WindowsUpdateApplyResult
ApplyVerifiedWindowsMsiUpdateForTesting(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint,
    const SynchronousWindowsMsiInstallCallback& installSynchronously) noexcept;
#endif

}  // namespace codex_monitor::update
