#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace codex_monitor::update {

inline constexpr std::uint64_t kMaximumWindowsInstallerBytes =
    256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaximumSha256ManifestBytes = 512;

using Sha256Digest = std::array<std::uint8_t, 32>;

enum class Sha256ManifestParseStatus {
    kValid,
    kInvalidExpectedFileName,
    kTooLarge,
    kMalformed,
    kFileNameMismatch,
};

struct Sha256ManifestParseResult {
    Sha256ManifestParseStatus status =
        Sha256ManifestParseStatus::kMalformed;
    Sha256Digest digest{};

    [[nodiscard]] bool valid() const noexcept {
        return status == Sha256ManifestParseStatus::kValid;
    }
};

// Accepts exactly one checksum record in the format emitted by
// windows/build-installer.ps1:
//   <64 hexadecimal SHA-256 characters><two spaces><exact MSI file name>
// A missing final newline, LF, or CRLF is accepted. Any other whitespace,
// extra line, path component, or file name is rejected.
[[nodiscard]] Sha256ManifestParseResult ParseWindowsInstallerSha256Manifest(
    std::string_view contents,
    std::string_view expectedInstallerFileName) noexcept;

// Compares all 32 bytes without data-dependent early exit.
[[nodiscard]] bool ConstantTimeSha256Equals(
    const Sha256Digest& lhs,
    const Sha256Digest& rhs) noexcept;

[[nodiscard]] bool IsAllowedWindowsInstallerFileSize(
    std::uint64_t fileSizeBytes) noexcept;

enum class WindowsInstallerVerificationStatus {
    kChecksumVerified,
    kInvalidExpectedFileName,
    kManifestTooLarge,
    kInvalidManifest,
    kManifestFileNameMismatch,
    kTargetFileNameMismatch,
    kFileOpenFailed,
    kUnsafeFileType,
    kFileMetadataFailed,
    kEmptyFile,
    kFileTooLarge,
    kHashInitializationFailed,
    kHashFailed,
    kFileReadFailed,
    kDigestMismatch,
    kUnsupportedPlatform,
    kUnexpected,
};

struct WindowsInstallerVerificationResult {
    WindowsInstallerVerificationStatus status =
        WindowsInstallerVerificationStatus::kUnexpected;
    std::uint64_t fileSizeBytes = 0;

    [[nodiscard]] bool checksumVerified() const noexcept {
        return status ==
               WindowsInstallerVerificationStatus::kChecksumVerified;
    }
};

// Verifies a completed download without executing it. The target path's final
// component and the checksum manifest must both exactly match the expected MSI
// name. The opened file must be a non-reparse regular file no larger than the
// fixed 256 MiB hard limit. SHA-256 is calculated incrementally with Windows
// CNG/BCrypt while write and delete sharing remain denied.
// This is only the transport-integrity stage. It must never authorize
// execution by itself; Authenticode publisher and MSI identity verification
// are separate mandatory gates.
[[nodiscard]] WindowsInstallerVerificationResult
VerifyDownloadedWindowsInstallerChecksum(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest) noexcept;

}  // namespace codex_monitor::update
