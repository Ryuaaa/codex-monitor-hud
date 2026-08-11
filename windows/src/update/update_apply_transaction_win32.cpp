#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_apply_transaction_win32.h"

#include <array>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <msi.h>
#endif

namespace codex_monitor::update {
namespace {

WindowsUpdateApplyResult ApplyFailure(
    WindowsUpdateApplyStatus status) noexcept {
    WindowsUpdateApplyResult result;
    result.status = status;
    return result;
}

WindowsInstallerVerificationStatus MapManifestStatus(
    Sha256ManifestParseStatus status) noexcept {
    switch (status) {
        case Sha256ManifestParseStatus::kInvalidExpectedFileName:
            return WindowsInstallerVerificationStatus::kInvalidExpectedFileName;
        case Sha256ManifestParseStatus::kTooLarge:
            return WindowsInstallerVerificationStatus::kManifestTooLarge;
        case Sha256ManifestParseStatus::kMalformed:
            return WindowsInstallerVerificationStatus::kInvalidManifest;
        case Sha256ManifestParseStatus::kFileNameMismatch:
            return WindowsInstallerVerificationStatus::
                kManifestFileNameMismatch;
        case Sha256ManifestParseStatus::kValid:
            break;
    }
    return WindowsInstallerVerificationStatus::kUnexpected;
}

std::optional<std::wstring> PrintableAsciiToWide(
    std::string_view value) {
    if (value.empty() || value.size() > 255) return std::nullopt;
    std::wstring converted;
    converted.reserve(value.size());
    for (const unsigned char character : value) {
        if (character < 0x21 || character > 0x7e) return std::nullopt;
        converted.push_back(static_cast<wchar_t>(character));
    }
    return converted;
}

#ifdef _WIN32

class KernelHandle {
public:
    KernelHandle() = default;
    explicit KernelHandle(HANDLE value) noexcept : value_(value) {}
    ~KernelHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;

    KernelHandle(KernelHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    KernelHandle& operator=(KernelHandle&& other) noexcept {
        if (this == &other) return *this;
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct FileIdentity {
    DWORD volumeSerialNumber = 0;
    DWORD fileIndexHigh = 0;
    DWORD fileIndexLow = 0;
};

bool ReadFileIdentity(HANDLE handle, FileIdentity* identity) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) return false;
    identity->volumeSerialNumber = information.dwVolumeSerialNumber;
    identity->fileIndexHigh = information.nFileIndexHigh;
    identity->fileIndexLow = information.nFileIndexLow;
    return true;
}

bool SameFileIdentity(const FileIdentity& lhs,
                      const FileIdentity& rhs) noexcept {
    return lhs.volumeSerialNumber == rhs.volumeSerialNumber &&
           lhs.fileIndexHigh == rhs.fileIndexHigh &&
           lhs.fileIndexLow == rhs.fileIndexLow;
}

bool SameWindowsPath(std::wstring_view lhs,
                     std::wstring_view rhs) noexcept {
    if (lhs.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        rhs.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
               static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

bool IsAsciiLetter(wchar_t value) noexcept {
    return (value >= L'a' && value <= L'z') ||
           (value >= L'A' && value <= L'Z');
}

bool IsReservedDosDeviceSegment(std::wstring_view segment) noexcept {
    const std::size_t dot = segment.find(L'.');
    std::wstring_view base = segment.substr(0, dot);
    if (base.empty() || base.size() > 4) return false;

    std::array<wchar_t, 4> upper{};
    for (std::size_t index = 0; index < base.size(); ++index) {
        wchar_t character = base[index];
        if (character >= L'a' && character <= L'z') {
            character = static_cast<wchar_t>(character - (L'a' - L'A'));
        }
        upper[index] = character;
    }
    const std::wstring_view normalized(upper.data(), base.size());
    if (normalized == L"CON" || normalized == L"PRN" ||
        normalized == L"AUX" || normalized == L"NUL") {
        return true;
    }
    return normalized.size() == 4 &&
           (normalized.substr(0, 3) == L"COM" ||
            normalized.substr(0, 3) == L"LPT") &&
           normalized[3] >= L'1' && normalized[3] <= L'9';
}

bool IsCanonicalPathSegment(std::wstring_view segment) noexcept {
    if (segment.empty() || segment == L"." || segment == L".." ||
        segment.back() == L'.' || segment.back() == L' ' ||
        IsReservedDosDeviceSegment(segment)) {
        return false;
    }
    for (const wchar_t character : segment) {
        if (character < 0x20 || character == L'<' || character == L'>' ||
            character == L':' || character == L'"' || character == L'|' ||
            character == L'?' || character == L'*' || character == L'/' ||
            character == L'\\') {
            return false;
        }
    }
    return true;
}

std::optional<std::wstring> CanonicalAbsoluteInstallerPath(
    const std::filesystem::path& installerPath,
    std::wstring_view expectedInstallerFileName) {
    const std::wstring input = installerPath.native();
    if (input.size() < 4 || input.size() >= 32760 ||
        !IsAsciiLetter(input[0]) || input[1] != L':' ||
        input[2] != L'\\' || input.back() == L'\\' ||
        input.find(L'/') != std::wstring::npos ||
        input.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }

    const std::wstring root = input.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return std::nullopt;

    std::wstring normalized(32768, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (written == 0 || written >= normalized.size()) return std::nullopt;
    normalized.resize(written);
    if (!SameWindowsPath(input, normalized)) return std::nullopt;

    std::size_t segmentBegin = 3;
    while (segmentBegin < normalized.size()) {
        const std::size_t separator = normalized.find(L'\\', segmentBegin);
        const std::size_t segmentEnd =
            separator == std::wstring::npos ? normalized.size() : separator;
        if (!IsCanonicalPathSegment(normalized.substr(
                segmentBegin, segmentEnd - segmentBegin))) {
            return std::nullopt;
        }
        if (separator == std::wstring::npos) break;
        segmentBegin = separator + 1;
    }

    const std::size_t fileSeparator = normalized.find_last_of(L'\\');
    if (fileSeparator == std::wstring::npos ||
        normalized.substr(fileSeparator + 1) != expectedInstallerFileName) {
        return std::nullopt;
    }
    return normalized;
}

std::wstring ExtendedDosPath(std::wstring_view dosPath) {
    std::wstring result = L"\\\\?\\";
    result.append(dosPath);
    return result;
}

bool IsNonReparseDirectory(HANDLE directory) noexcept {
    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo,
                                      &information, sizeof(information))) {
        return false;
    }
    return (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsNonReparseFile(HANDLE file) noexcept {
    FILE_ATTRIBUTE_TAG_INFO information{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo,
                                      &information, sizeof(information))) {
        return false;
    }
    return (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::optional<std::wstring> FinalDosPathFromHandle(HANDLE handle,
                                                   bool allowRoot) {
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, kFlags);
    if (required == 0 || required >= 32768) return std::nullopt;

    std::wstring path(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, path.data(), static_cast<DWORD>(path.size()), kFlags);
    if (written == 0 || written >= path.size()) return std::nullopt;
    path.resize(written);
    constexpr std::wstring_view kPrefix = L"\\\\?\\";
    if (path.size() < kPrefix.size() + 3 ||
        path.substr(0, kPrefix.size()) != kPrefix ||
        !IsAsciiLetter(path[kPrefix.size()]) ||
        path[kPrefix.size() + 1] != L':' ||
        path[kPrefix.size() + 2] != L'\\' ||
        (!allowRoot && path.back() == L'\\')) {
        return std::nullopt;
    }
    return path;
}

KernelHandle OpenDirectoryWithoutFollowingReparsePoints(
    const std::wstring& path,
    bool volumeRoot = false) noexcept {
    const DWORD flags =
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    if (!volumeRoot) {
        KernelHandle deleteProtected{CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            flags, nullptr)};
        if (deleteProtected) return deleteProtected;
        if (GetLastError() != ERROR_ACCESS_DENIED) return {};
        // A directory on which this token cannot acquire DELETE cannot be
        // renamed by the same token. Keep an attribute handle to pin its file
        // identity and reject every reparse point. User-writable descendants
        // take the DELETE branch above and remain actively rename-locked.
    }
    return KernelHandle{CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        flags, nullptr)};
}

struct LockedInstallerPath {
    std::vector<KernelHandle> ancestors;
    KernelHandle file;
    KernelHandle reopenedFile;
    std::filesystem::path canonicalPath;
};

struct LockInstallerResult {
    WindowsUpdateApplyStatus status =
        WindowsUpdateApplyStatus::kPathResolutionFailed;
    std::optional<LockedInstallerPath> locked;
};

LockInstallerResult LockCanonicalInstallerPath(
    const std::wstring& canonicalPath) {
    const std::size_t separator = canonicalPath.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator < 2) return {};
    const std::wstring parentPath = separator == 2
        ? canonicalPath.substr(0, 3)
        : canonicalPath.substr(0, separator);

    LockedInstallerPath locked;
    std::vector<std::wstring> ancestorPaths;
    ancestorPaths.push_back(ExtendedDosPath(canonicalPath.substr(0, 3)));
    std::size_t end = parentPath.find(L'\\', 3);
    while (end != std::wstring::npos) {
        ancestorPaths.push_back(ExtendedDosPath(parentPath.substr(0, end)));
        end = parentPath.find(L'\\', end + 1);
    }
    if (parentPath.size() > 3) {
        ancestorPaths.push_back(ExtendedDosPath(parentPath));
    }
    locked.ancestors.reserve(ancestorPaths.size());

    for (std::size_t index = 0; index < ancestorPaths.size(); ++index) {
        const std::wstring& path = ancestorPaths[index];
        KernelHandle ancestor =
            OpenDirectoryWithoutFollowingReparsePoints(path, index == 0U);
        if (!ancestor) {
            return {WindowsUpdateApplyStatus::kPathResolutionFailed,
                    std::nullopt};
        }
        if (!IsNonReparseDirectory(ancestor.get())) {
            return {WindowsUpdateApplyStatus::kUnsafePathAncestor,
                    std::nullopt};
        }
        locked.ancestors.push_back(std::move(ancestor));
    }

    HANDLE lockedParent = locked.ancestors.back().get();
    FileIdentity parentIdentity{};
    const std::optional<std::wstring> finalParent =
        FinalDosPathFromHandle(lockedParent, parentPath.size() == 3);
    const std::wstring expectedParent = ExtendedDosPath(parentPath);
    if (!finalParent.has_value() ||
        !SameWindowsPath(*finalParent, expectedParent) ||
        !ReadFileIdentity(lockedParent, &parentIdentity)) {
        return {WindowsUpdateApplyStatus::kFileIdentityMismatch,
                std::nullopt};
    }

    // Every non-root ancestor is already held with DELETE access and without
    // FILE_SHARE_DELETE. Reopening the parent would conflict with that active
    // rename lock. The handle-derived final path plus the identity read above
    // prove the exact parent while the retained ancestor handles prevent it
    // from being replaced for the remainder of the transaction.

    const std::wstring extendedFilePath = ExtendedDosPath(canonicalPath);
    locked.file = KernelHandle{CreateFileW(
        extendedFilePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
            FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    if (!locked.file) {
        return {WindowsUpdateApplyStatus::kFileOpenFailed, std::nullopt};
    }
    if (!IsNonReparseFile(locked.file.get())) {
        return {WindowsUpdateApplyStatus::kUnsafeFileType, std::nullopt};
    }

    FileIdentity fileIdentity{};
    const std::optional<std::wstring> finalFile =
        FinalDosPathFromHandle(locked.file.get(), false);
    if (!finalFile.has_value() ||
        !SameWindowsPath(*finalFile, extendedFilePath) ||
        !ReadFileIdentity(locked.file.get(), &fileIdentity)) {
        return {WindowsUpdateApplyStatus::kFileIdentityMismatch,
                std::nullopt};
    }

    locked.reopenedFile = KernelHandle{CreateFileW(
        finalFile->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    FileIdentity reopenedFileIdentity{};
    if (!locked.reopenedFile || !IsNonReparseFile(locked.reopenedFile.get()) ||
        !ReadFileIdentity(locked.reopenedFile.get(), &reopenedFileIdentity) ||
        !SameFileIdentity(fileIdentity, reopenedFileIdentity)) {
        return {WindowsUpdateApplyStatus::kFileIdentityMismatch,
                std::nullopt};
    }

    locked.canonicalPath = std::filesystem::path(canonicalPath);
    return {WindowsUpdateApplyStatus::kInstalled,
            std::optional<LockedInstallerPath>{std::move(locked)}};
}

#endif

}  // namespace

namespace {

using WindowsMsiInstallOperation =
    std::function<int(const std::filesystem::path& verifiedInstallerPath)>;

#ifdef _WIN32

class ScopedMsiInternalUi {
public:
    ScopedMsiInternalUi() noexcept
        : previous_(MsiSetInternalUI(
              static_cast<INSTALLUILEVEL>(
                  static_cast<unsigned int>(INSTALLUILEVEL_NONE) |
                  static_cast<unsigned int>(INSTALLUILEVEL_SOURCERESONLY)),
              nullptr)) {}

    ~ScopedMsiInternalUi() {
        static_cast<void>(MsiSetInternalUI(previous_, nullptr));
    }

    ScopedMsiInternalUi(const ScopedMsiInternalUi&) = delete;
    ScopedMsiInternalUi& operator=(const ScopedMsiInternalUi&) = delete;

private:
    INSTALLUILEVEL previous_ = INSTALLUILEVEL_DEFAULT;
};

int InstallWindowsMsiSynchronously(
    const std::filesystem::path& verifiedInstallerPath) noexcept {
    ScopedMsiInternalUi quietInstaller;
    return static_cast<int>(MsiInstallProductW(
        verifiedInstallerPath.c_str(), L"REBOOT=ReallySuppress"));
}

#else

int InstallWindowsMsiSynchronously(
    const std::filesystem::path&) noexcept {
    return -1;
}

#endif

WindowsUpdateApplyResult ApplyVerifiedWindowsMsiUpdateImpl(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint,
    const WindowsMsiInstallOperation& installSynchronously) noexcept {
    WindowsUpdateApplyResult result;
    try {
        // Freeze every caller-owned security decision before touching the file.
        // This also makes the synchronous callback lifetime independent of a
        // concurrently replaced std::function object owned by the caller.
        const std::string expectedFileNameCopy(expectedInstallerFileName);
        const std::string sha256ManifestCopy(sha256Manifest);
        const std::string expectedVersionCopy(expectedVersion);
        const std::optional<PublisherCertificateSha256> fingerprintCopy =
            trustedPublisherFingerprint;
        const WindowsMsiInstallOperation installCallback =
            installSynchronously;
        if (!installCallback) {
            return ApplyFailure(WindowsUpdateApplyStatus::kInvalidInput);
        }

        const Sha256ManifestParseResult manifest =
            ParseWindowsInstallerSha256Manifest(
                sha256ManifestCopy, expectedFileNameCopy);
        if (!manifest.valid()) {
            result.status = WindowsUpdateApplyStatus::kChecksumRejected;
            result.checksum.status = MapManifestStatus(manifest.status);
            return result;
        }

        if (installerPath.empty() || !installerPath.is_absolute()) {
            return ApplyFailure(WindowsUpdateApplyStatus::kPathNotAbsolute);
        }
        const std::optional<std::wstring> expectedFileName =
            PrintableAsciiToWide(expectedFileNameCopy);
        if (!expectedFileName.has_value()) {
            return ApplyFailure(WindowsUpdateApplyStatus::kInvalidInput);
        }

#ifdef _WIN32
        const std::optional<std::wstring> canonicalPath =
            CanonicalAbsoluteInstallerPath(installerPath, *expectedFileName);
        if (!canonicalPath.has_value()) {
            return ApplyFailure(WindowsUpdateApplyStatus::kPathNotCanonical);
        }

        LockInstallerResult lockResult =
            LockCanonicalInstallerPath(*canonicalPath);
        if (!lockResult.locked.has_value()) {
            return ApplyFailure(lockResult.status);
        }

        // Keep this object alive through the callback. Its handles deny
        // ancestor renames and MSI writes/deletes, closing the path replacement
        // window between the three verification gates and installer launch.
        LockedInstallerPath& locked = *lockResult.locked;
        result.checksum = VerifyDownloadedWindowsInstallerChecksum(
            locked.canonicalPath, expectedFileNameCopy, sha256ManifestCopy);
        if (!result.checksum.checksumVerified()) {
            result.status = WindowsUpdateApplyStatus::kChecksumRejected;
            return result;
        }

        result.publisherAndIdentity = VerifyWindowsMsiIdentityAndPublisher(
            locked.canonicalPath, expectedVersionCopy, fingerprintCopy);
        if (!result.publisherAndIdentity.verified()) {
            result.status =
                WindowsUpdateApplyStatus::kPublisherOrIdentityRejected;
            return result;
        }

        result.installAttempted = true;
        try {
            result.installerExitCode =
                installCallback(locked.canonicalPath);
        } catch (...) {
            result.status = WindowsUpdateApplyStatus::kInstallCallbackThrew;
            return result;
        }
        if (result.installerExitCode != ERROR_SUCCESS &&
            result.installerExitCode != ERROR_SUCCESS_REBOOT_REQUIRED) {
            result.status = WindowsUpdateApplyStatus::kInstallFailed;
            return result;
        }
        result.status = WindowsUpdateApplyStatus::kInstalled;
        result.rebootRequired =
            result.installerExitCode == ERROR_SUCCESS_REBOOT_REQUIRED;
        return result;
#else
        (void)expectedVersionCopy;
        (void)fingerprintCopy;
        return ApplyFailure(WindowsUpdateApplyStatus::kUnsupportedPlatform);
#endif
    } catch (...) {
        result.status = WindowsUpdateApplyStatus::kUnexpected;
        return result;
    }
}

}  // namespace

WindowsUpdateApplyResult ApplyVerifiedWindowsMsiUpdate(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint) noexcept {
    return ApplyVerifiedWindowsMsiUpdateImpl(
        installerPath, expectedInstallerFileName, sha256Manifest,
        expectedVersion, trustedPublisherFingerprint,
        InstallWindowsMsiSynchronously);
}

#if defined(CODEX_MONITOR_UPDATE_APPLY_TRANSACTION_TESTING)

WindowsUpdateApplyResult ApplyVerifiedWindowsMsiUpdateForTesting(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint,
    const SynchronousWindowsMsiInstallCallback& installSynchronously) noexcept {
    return ApplyVerifiedWindowsMsiUpdateImpl(
        installerPath, expectedInstallerFileName, sha256Manifest,
        expectedVersion, trustedPublisherFingerprint, installSynchronously);
}

#endif

}  // namespace codex_monitor::update
