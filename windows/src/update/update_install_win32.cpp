#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_install_win32.h"

#include "update/update_helper_win32.h"

#include <array>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace codex_monitor::update {
namespace {

constexpr std::string_view kInstallerPrefix =
    "CodexMonitorHUD-windows-x64-";
constexpr std::string_view kInstallerSuffix = ".msi";

bool IsCancelled(
    const WindowsUpdateInstallCancellationCheck& cancelled) noexcept {
    if (!cancelled) return false;
    try {
        return cancelled();
    } catch (...) {
        return true;
    }
}

WindowsUpdateInstallResult Failure(
    WindowsUpdateInstallStatus status,
    std::string targetVersion = {}) noexcept {
    WindowsUpdateInstallResult result;
    result.status = status;
    result.targetVersion = std::move(targetVersion);
    return result;
}

bool ValidRequest(const WindowsUpdateInstallRequest& request) {
    if (request.currentVersion.empty() || request.updatesRoot.empty() ||
        !request.updatesRoot.is_absolute()) {
        return false;
    }
    const std::optional<SemanticVersion> current =
        ParseSemVerTag(request.currentVersion);
    const SemanticVersion& target = request.release.version;
    const std::optional<SemanticVersion> reparsedTarget =
        ParseSemVerTag(target.canonical);
    if (!current.has_value() || !current->IsStable() ||
        !current->buildMetadata.empty() ||
        current->canonical != request.currentVersion ||
        !reparsedTarget.has_value() || !reparsedTarget->IsStable() ||
        !reparsedTarget->buildMetadata.empty() ||
        reparsedTarget->canonical != target.canonical ||
        CompareSemVerPrecedence(*reparsedTarget, *current) <= 0) {
        return false;
    }

    std::string expectedInstaller(kInstallerPrefix);
    expectedInstaller.append(target.canonical);
    expectedInstaller.append(kInstallerSuffix);
    return request.release.installer.name == expectedInstaller &&
           request.release.checksum.name == expectedInstaller + ".sha256" &&
           !request.release.installer.browserDownloadUrl.empty() &&
           !request.release.checksum.browserDownloadUrl.empty();
}

std::string LowercaseDigest(const Sha256Digest& digest) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2U] = kHex[digest[index] >> 4U];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0fU];
    }
    return result;
}

#ifdef _WIN32

bool SameWindowsPath(std::wstring_view lhs,
                     std::wstring_view rhs) noexcept {
    if (lhs.size() > static_cast<std::size_t>(
                         std::numeric_limits<int>::max()) ||
        rhs.size() > static_cast<std::size_t>(
                         std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               lhs.data(), static_cast<int>(lhs.size()), rhs.data(),
               static_cast<int>(rhs.size()), TRUE) == CSTR_EQUAL;
}

bool IsCanonicalFixedLocalDirectoryPath(
    const std::filesystem::path& path) {
    const std::wstring input = path.native();
    if (input.size() < 4U || input.size() >= 32760U ||
        !((input[0] >= L'a' && input[0] <= L'z') ||
          (input[0] >= L'A' && input[0] <= L'Z')) ||
        input[1] != L':' || input[2] != L'\\' ||
        input.back() == L'\\' ||
        input.find(L'/') != std::wstring::npos ||
        input.find(L'\0') != std::wstring::npos) {
        return false;
    }
    const std::wstring root = input.substr(0, 3U);
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return false;
    std::wstring normalized(32768U, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (written == 0U || written >= normalized.size()) return false;
    normalized.resize(written);
    return SameWindowsPath(input, normalized);
}

std::optional<std::filesystem::path> CreateFreshPrivateDirectory(
    const std::filesystem::path& updatesRoot) {
    if (!IsCanonicalFixedLocalDirectoryPath(updatesRoot)) return std::nullopt;
    std::error_code createError;
    std::filesystem::create_directories(updatesRoot, createError);
    if (createError || !std::filesystem::is_directory(updatesRoot, createError) ||
        createError) {
        return std::nullopt;
    }

    constexpr char kHex[] = "0123456789abcdef";
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::array<unsigned char, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(),
                            static_cast<ULONG>(random.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return std::nullopt;
        }
        std::wstring name = L"update-";
        name.reserve(7U + random.size() * 2U);
        for (const unsigned char byte : random) {
            name.push_back(static_cast<wchar_t>(kHex[byte >> 4U]));
            name.push_back(static_cast<wchar_t>(kHex[byte & 0x0fU]));
        }
        const std::filesystem::path candidate = updatesRoot / name;
        if (CreateDirectoryW(candidate.c_str(), nullptr)) return candidate;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> ReadSmallFile(
    const std::filesystem::path& path,
    std::size_t maximumBytes) {
    if (maximumBytes == 0U || maximumBytes > 64U * 1024U ||
        path.empty() || !path.is_absolute()) {
        return std::nullopt;
    }
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    struct HandleCleanup {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~HandleCleanup() {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    } cleanup{handle};

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes,
            static_cast<DWORD>(sizeof(attributes))) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        !GetFileSizeEx(handle, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maximumBytes) {
        return std::nullopt;
    }

    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD total = 0;
    while (total < contents.size()) {
        DWORD read = 0;
        if (!ReadFile(handle, contents.data() + total,
                      static_cast<DWORD>(contents.size() - total),
                      &read, nullptr) || read == 0U) {
            return std::nullopt;
        }
        total += read;
    }
    return contents;
}

void BestEffortRemoveFailedPreparation(
    const WindowsUpdateInstallRequest& request,
    const std::filesystem::path& privateDirectory) noexcept {
    if (privateDirectory.empty()) return;
    const std::filesystem::path checksum =
        privateDirectory / request.release.checksum.name;
    const std::filesystem::path installer =
        privateDirectory / request.release.installer.name;
    const std::filesystem::path helper =
        privateDirectory / std::filesystem::path(kWindowsHudExecutableName);
    DeleteFileW(helper.c_str());
    DeleteFileW(installer.c_str());
    DeleteFileW(checksum.c_str());
    RemoveDirectoryW(privateDirectory.c_str());
}

#endif

}  // namespace

WindowsUpdateInstallResult RunWindowsUpdateInstallPlan(
    const WindowsUpdateInstallRequest& request,
    const WindowsUpdateInstallOperations& operations,
    const WindowsUpdateInstallCancellationCheck& cancelled) noexcept {
    std::string targetVersion = request.release.version.canonical;
    try {
        if (!ValidRequest(request) || !operations.publisherConfigured ||
            !operations.createPrivateDirectory || !operations.downloadAsset ||
            !operations.readSmallFile || !operations.launchHelper) {
            return Failure(WindowsUpdateInstallStatus::kInvalidInput,
                           std::move(targetVersion));
        }
        if (IsCancelled(cancelled)) {
            return Failure(WindowsUpdateInstallStatus::kCancelled,
                           std::move(targetVersion));
        }
        if (!operations.publisherConfigured()) {
            return Failure(
                WindowsUpdateInstallStatus::kPublisherNotConfigured,
                std::move(targetVersion));
        }

        WindowsUpdateInstallResult result;
        result.targetVersion = targetVersion;
        const std::optional<std::filesystem::path> privateDirectory =
            operations.createPrivateDirectory(request.updatesRoot);
        if (!privateDirectory.has_value() || privateDirectory->empty() ||
            !privateDirectory->is_absolute() ||
            privateDirectory->parent_path() != request.updatesRoot) {
            result.status =
                WindowsUpdateInstallStatus::kUpdateDirectoryUnavailable;
            return result;
        }
        result.privateUpdateDirectory = *privateDirectory;
        if (IsCancelled(cancelled)) {
            result.status = WindowsUpdateInstallStatus::kCancelled;
            return result;
        }

        const UpdateAssetDownloadResult checksumDownload =
            operations.downloadAsset(
                request.release.checksum,
                kMaximumSha256ManifestBytes,
                *privateDirectory, cancelled);
        if (!checksumDownload.succeeded) {
            result.downloadFailure = checksumDownload.failure;
            result.error = checksumDownload.error;
            result.status =
                checksumDownload.failure ==
                        UpdateAssetDownloadFailureKind::kCancelled
                    ? WindowsUpdateInstallStatus::kCancelled
                    : WindowsUpdateInstallStatus::kChecksumDownloadFailed;
            return result;
        }
        if (IsCancelled(cancelled)) {
            result.status = WindowsUpdateInstallStatus::kCancelled;
            return result;
        }

        const std::filesystem::path checksumPath =
            *privateDirectory / request.release.checksum.name;
        const std::optional<std::string> manifest =
            operations.readSmallFile(
                checksumPath, kMaximumSha256ManifestBytes);
        if (!manifest.has_value()) {
            result.status = WindowsUpdateInstallStatus::kChecksumReadFailed;
            return result;
        }
        const Sha256ManifestParseResult parsed =
            ParseWindowsInstallerSha256Manifest(
                *manifest, request.release.installer.name);
        result.manifestStatus = parsed.status;
        if (!parsed.valid()) {
            result.status = WindowsUpdateInstallStatus::kChecksumRejected;
            return result;
        }

        const UpdateAssetDownloadResult installerDownload =
            operations.downloadAsset(
                request.release.installer, kMaximumWindowsInstallerBytes,
                *privateDirectory, cancelled);
        if (!installerDownload.succeeded) {
            result.downloadFailure = installerDownload.failure;
            result.error = installerDownload.error;
            result.status =
                installerDownload.failure ==
                        UpdateAssetDownloadFailureKind::kCancelled
                    ? WindowsUpdateInstallStatus::kCancelled
                    : WindowsUpdateInstallStatus::kInstallerDownloadFailed;
            return result;
        }
        if (IsCancelled(cancelled)) {
            result.status = WindowsUpdateInstallStatus::kCancelled;
            return result;
        }

        WindowsUpdateHelperLauncherRequest launchRequest;
        launchRequest.privateUpdateDirectory = *privateDirectory;
        launchRequest.installerPath =
            *privateDirectory / request.release.installer.name;
        launchRequest.installerSha256 = LowercaseDigest(parsed.digest);
        launchRequest.currentVersion = request.currentVersion;
        launchRequest.targetVersion = targetVersion;
        const WindowsUpdateHelperLauncherResult launched =
            operations.launchHelper(launchRequest);
        result.launcherStatus = launched.status;
        result.status = launched.started()
            ? WindowsUpdateInstallStatus::kHelperStarted
            : WindowsUpdateInstallStatus::kHelperLaunchFailed;
        return result;
    } catch (...) {
        return Failure(WindowsUpdateInstallStatus::kUnexpected,
                       std::move(targetVersion));
    }
}

WindowsUpdateInstallResult PrepareAndLaunchWindowsUpdate(
    const WindowsUpdateInstallRequest& request,
    const WindowsUpdateInstallCancellationCheck& cancelled) noexcept {
#ifdef _WIN32
    WindowsUpdateInstallOperations operations;
    operations.publisherConfigured = [] {
        return ConfiguredWindowsUpdatePublisherFingerprint().has_value();
    };
    operations.createPrivateDirectory = CreateFreshPrivateDirectory;
    operations.downloadAsset = [](
        const GitHubReleaseAsset& asset,
        std::uint64_t maximumBytes,
        const std::filesystem::path& privateDirectory,
        const WindowsUpdateInstallCancellationCheck& cancellation) {
        return DownloadWindowsUpdateAsset(
            asset.browserDownloadUrl, asset.name, maximumBytes,
            privateDirectory, cancellation);
    };
    operations.readSmallFile = ReadSmallFile;
    operations.launchHelper = [](
        const WindowsUpdateHelperLauncherRequest& launchRequest) {
        return LaunchPreparedWindowsUpdateHelper(launchRequest);
    };
    WindowsUpdateInstallResult result = RunWindowsUpdateInstallPlan(
        request, operations, cancelled);
    if (!result.helperStarted()) {
        BestEffortRemoveFailedPreparation(
            request, result.privateUpdateDirectory);
    }
    return result;
#else
    (void)request;
    (void)cancelled;
    return Failure(WindowsUpdateInstallStatus::kUnsupportedPlatform,
                   request.release.version.canonical);
#endif
}

}  // namespace codex_monitor::update
