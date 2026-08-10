#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace codex_monitor::update {

inline constexpr std::wstring_view kWindowsMsiProductName =
    L"Codex Monitor HUD";
inline constexpr std::wstring_view kWindowsMsiUpgradeCode =
    L"{0CA9E00B-2AAF-4393-B466-1AF0F8C2C21F}";
inline constexpr std::wstring_view kWindowsMsiTemplate = L"x64;1033";

using PublisherCertificateSha256 = std::array<std::uint8_t, 32>;

struct WindowsMsiIdentity {
    std::wstring productName;
    std::wstring productVersion;
    std::wstring upgradeCode;
    std::wstring templateValue;
};

enum class WindowsMsiIdentityPolicyStatus {
    kValid,
    kInvalidExpectedVersion,
    kProductNameMismatch,
    kProductVersionMismatch,
    kUpgradeCodeMismatch,
    kTemplateMismatch,
};

// Applies the release identity policy without touching a file. expectedVersion
// must be a canonical three-part Windows Installer version: major and minor
// are 0..255, build is 0..65535, and leading zeroes are rejected.
[[nodiscard]] WindowsMsiIdentityPolicyStatus ValidateWindowsMsiIdentity(
    const WindowsMsiIdentity& identity,
    std::string_view expectedVersion) noexcept;

enum class WindowsMsiIdentityVerificationStatus {
    kVerified,
    kMissingTrustedPublisherFingerprint,
    kInvalidExpectedVersion,
    kPathNotAbsolute,
    kPathResolutionFailed,
    kUnsafePathAncestor,
    kFileOpenFailed,
    kUnsafeFileType,
    kFileIdentityMismatch,
    kSignatureVerificationFailed,
    kSignerCertificateUnavailable,
    kPublisherFingerprintMismatch,
    kMsiOpenFailed,
    kMsiPropertyQueryFailed,
    kMsiPropertyMissing,
    kMsiPropertyDuplicate,
    kMsiPropertyTooLong,
    kMsiPropertyTypeMismatch,
    kMsiSummaryQueryFailed,
    kMsiTemplateMissing,
    kMsiTemplateTooLong,
    kMsiTemplateTypeMismatch,
    kIdentityRejected,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsMsiIdentityVerificationResult {
    WindowsMsiIdentityVerificationStatus status =
        WindowsMsiIdentityVerificationStatus::kUnexpected;
    WindowsMsiIdentityPolicyStatus policyStatus =
        WindowsMsiIdentityPolicyStatus::kValid;

    [[nodiscard]] bool verified() const noexcept {
        return status == WindowsMsiIdentityVerificationStatus::kVerified;
    }
};

// Verifies an already downloaded MSI without executing it. installerPath must
// be absolute. Every ancestor is opened without following its final reparse
// point and held during verification; any reparse ancestor is rejected. The
// final absolute path is recovered from the locked file handle and must reopen
// to the same volume serial number and file index before any path-based API is
// used. The MSI must then have
// a valid Authenticode signature and revocation-checked chain, the leaf signing
// certificate's SHA-256 fingerprint must equal trustedPublisherFingerprint,
// and the read-only MSI database must satisfy ValidateWindowsMsiIdentity.
// A missing pinned publisher fingerprint always fails closed.
[[nodiscard]] WindowsMsiIdentityVerificationResult
VerifyWindowsMsiIdentityAndPublisher(
    const std::filesystem::path& installerPath,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint) noexcept;

}  // namespace codex_monitor::update
