#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_installer_verifier_win32.h"

#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <vector>
#endif

namespace codex_monitor::update {
namespace {

constexpr std::string_view kInstallerNamePrefix =
    "CodexMonitorHUD-windows-x64-";
constexpr std::string_view kInstallerNameSuffix = ".msi";

bool IsAsciiDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool IsCanonicalVersionPart(std::string_view value) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), IsAsciiDigit);
}

bool IsCanonicalInstallerFileName(std::string_view value) noexcept {
    if (value.size() <=
            kInstallerNamePrefix.size() + kInstallerNameSuffix.size() ||
        value.size() > 255 ||
        value.substr(0, kInstallerNamePrefix.size()) !=
            kInstallerNamePrefix ||
        value.substr(value.size() - kInstallerNameSuffix.size()) !=
            kInstallerNameSuffix) {
        return false;
    }

    std::string_view version = value.substr(
        kInstallerNamePrefix.size(),
        value.size() - kInstallerNamePrefix.size() -
            kInstallerNameSuffix.size());
    const std::size_t firstDot = version.find('.');
    if (firstDot == std::string_view::npos) return false;
    const std::size_t secondDot = version.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos ||
        version.find('.', secondDot + 1) != std::string_view::npos) {
        return false;
    }
    return IsCanonicalVersionPart(version.substr(0, firstDot)) &&
           IsCanonicalVersionPart(version.substr(
               firstDot + 1, secondDot - firstDot - 1)) &&
           IsCanonicalVersionPart(version.substr(secondDot + 1));
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

WindowsInstallerVerificationResult VerificationFailure(
    WindowsInstallerVerificationStatus status,
    std::uint64_t fileSizeBytes = 0) noexcept {
    WindowsInstallerVerificationResult result;
    result.status = status;
    result.fileSizeBytes = fileSizeBytes;
    return result;
}

WindowsInstallerVerificationStatus MapManifestFailure(
    Sha256ManifestParseStatus status) noexcept {
    switch (status) {
        case Sha256ManifestParseStatus::kInvalidExpectedFileName:
            return WindowsInstallerVerificationStatus::
                kInvalidExpectedFileName;
        case Sha256ManifestParseStatus::kTooLarge:
            return WindowsInstallerVerificationStatus::kManifestTooLarge;
        case Sha256ManifestParseStatus::kFileNameMismatch:
            return WindowsInstallerVerificationStatus::
                kManifestFileNameMismatch;
        case Sha256ManifestParseStatus::kMalformed:
            return WindowsInstallerVerificationStatus::kInvalidManifest;
        case Sha256ManifestParseStatus::kValid:
            break;
    }
    return WindowsInstallerVerificationStatus::kUnexpected;
}

#ifdef _WIN32

class FileHandle {
public:
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class AlgorithmHandle {
public:
    AlgorithmHandle() = default;
    ~AlgorithmHandle() {
        if (value_ != nullptr) BCryptCloseAlgorithmProvider(value_, 0);
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
    [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }

private:
    BCRYPT_ALG_HANDLE value_ = nullptr;
};

class HashHandle {
public:
    HashHandle() = default;
    ~HashHandle() {
        if (value_ != nullptr) BCryptDestroyHash(value_);
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;

    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
    [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }

private:
    BCRYPT_HASH_HANDLE value_ = nullptr;
};

bool BCryptSucceeded(NTSTATUS status) noexcept {
    return status >= 0;
}

WindowsInstallerVerificationResult HashAndCompareInstaller(
    HANDLE file,
    std::uint64_t expectedFileSize,
    const Sha256Digest& expectedDigest) {
    AlgorithmHandle algorithm;
    if (!BCryptSucceeded(BCryptOpenAlgorithmProvider(
            algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kHashInitializationFailed,
            expectedFileSize);
    }

    ULONG hashObjectBytes = 0;
    ULONG hashLength = 0;
    ULONG propertyBytes = 0;
    if (!BCryptSucceeded(BCryptGetProperty(
            algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&hashObjectBytes),
            static_cast<ULONG>(sizeof(hashObjectBytes)), &propertyBytes, 0)) ||
        propertyBytes != sizeof(hashObjectBytes) || hashObjectBytes == 0 ||
        !BCryptSucceeded(BCryptGetProperty(
            algorithm.get(), BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength),
            static_cast<ULONG>(sizeof(hashLength)), &propertyBytes, 0)) ||
        propertyBytes != sizeof(hashLength) ||
        hashLength != static_cast<ULONG>(Sha256Digest{}.size())) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kHashInitializationFailed,
            expectedFileSize);
    }

    std::vector<UCHAR> hashObject(hashObjectBytes);
    HashHandle hash;
    if (!BCryptSucceeded(BCryptCreateHash(
            algorithm.get(), hash.put(), hashObject.data(), hashObjectBytes,
            nullptr, 0, 0))) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kHashInitializationFailed,
            expectedFileSize);
    }

    std::array<UCHAR, 64 * 1024> buffer{};
    std::uint64_t bytesHashed = 0;
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer.data(),
                      static_cast<DWORD>(buffer.size()), &bytesRead,
                      nullptr)) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kFileReadFailed,
                bytesHashed);
        }
        if (bytesRead == 0) break;
        if (bytesHashed >
            kMaximumWindowsInstallerBytes -
                static_cast<std::uint64_t>(bytesRead)) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kFileTooLarge,
                bytesHashed + static_cast<std::uint64_t>(bytesRead));
        }
        if (!BCryptSucceeded(BCryptHashData(
                hash.get(), buffer.data(), bytesRead, 0))) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kHashFailed,
                bytesHashed);
        }
        bytesHashed += static_cast<std::uint64_t>(bytesRead);
    }

    if (bytesHashed != expectedFileSize) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kFileMetadataFailed,
            bytesHashed);
    }

    Sha256Digest actualDigest{};
    if (!BCryptSucceeded(BCryptFinishHash(
            hash.get(), actualDigest.data(),
            static_cast<ULONG>(actualDigest.size()), 0))) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kHashFailed,
            bytesHashed);
    }
    if (!ConstantTimeSha256Equals(actualDigest, expectedDigest)) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kDigestMismatch,
            bytesHashed);
    }

    WindowsInstallerVerificationResult result;
    result.status = WindowsInstallerVerificationStatus::kChecksumVerified;
    result.fileSizeBytes = bytesHashed;
    return result;
}

#endif

}  // namespace

Sha256ManifestParseResult ParseWindowsInstallerSha256Manifest(
    std::string_view contents,
    std::string_view expectedInstallerFileName) noexcept {
    Sha256ManifestParseResult result;
    if (!IsCanonicalInstallerFileName(expectedInstallerFileName)) {
        result.status =
            Sha256ManifestParseStatus::kInvalidExpectedFileName;
        return result;
    }
    if (contents.size() > kMaximumSha256ManifestBytes) {
        result.status = Sha256ManifestParseStatus::kTooLarge;
        return result;
    }

    if (!contents.empty() && contents.back() == '\n') {
        contents.remove_suffix(1);
        if (!contents.empty() && contents.back() == '\r') {
            contents.remove_suffix(1);
        }
    }
    const std::size_t expectedLength =
        64 + 2 + expectedInstallerFileName.size();
    if (contents.size() != expectedLength || contents[64] != ' ' ||
        contents[65] != ' ') {
        result.status = Sha256ManifestParseStatus::kMalformed;
        return result;
    }

    for (std::size_t index = 0; index < result.digest.size(); ++index) {
        const int high = HexValue(contents[index * 2]);
        const int low = HexValue(contents[index * 2 + 1]);
        if (high < 0 || low < 0) {
            result.status = Sha256ManifestParseStatus::kMalformed;
            result.digest.fill(0);
            return result;
        }
        result.digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }

    if (contents.substr(66) != expectedInstallerFileName) {
        result.status = Sha256ManifestParseStatus::kFileNameMismatch;
        result.digest.fill(0);
        return result;
    }
    result.status = Sha256ManifestParseStatus::kValid;
    return result;
}

bool ConstantTimeSha256Equals(const Sha256Digest& lhs,
                              const Sha256Digest& rhs) noexcept {
    volatile std::uint8_t difference = 0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        difference = static_cast<std::uint8_t>(
            difference | static_cast<std::uint8_t>(lhs[index] ^ rhs[index]));
    }
    return difference == 0;
}

bool IsAllowedWindowsInstallerFileSize(
    std::uint64_t fileSizeBytes) noexcept {
    return fileSizeBytes > 0 &&
           fileSizeBytes <= kMaximumWindowsInstallerBytes;
}

WindowsInstallerVerificationResult VerifyDownloadedWindowsInstallerChecksum(
    const std::filesystem::path& installerPath,
    std::string_view expectedInstallerFileName,
    std::string_view sha256Manifest) noexcept {
    try {
        const Sha256ManifestParseResult manifest =
            ParseWindowsInstallerSha256Manifest(
                sha256Manifest, expectedInstallerFileName);
        if (!manifest.valid()) {
            return VerificationFailure(MapManifestFailure(manifest.status));
        }

        const std::filesystem::path expectedPath{
            std::string(expectedInstallerFileName)};
        if (installerPath.empty() ||
            installerPath.filename().native() != expectedPath.native()) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::
                    kTargetFileNameMismatch);
        }

#ifdef _WIN32
        FileHandle file(CreateFileW(
            installerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!file) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kFileOpenFailed);
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                file.get(), FileAttributeTagInfo, &attributes,
                static_cast<DWORD>(sizeof(attributes)))) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kFileMetadataFailed);
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kUnsafeFileType);
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart < 0) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kFileMetadataFailed);
        }
        const std::uint64_t fileSizeBytes =
            static_cast<std::uint64_t>(fileSize.QuadPart);
        if (fileSizeBytes == 0) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kEmptyFile);
        }
        if (!IsAllowedWindowsInstallerFileSize(fileSizeBytes)) {
            return VerificationFailure(
                WindowsInstallerVerificationStatus::kFileTooLarge,
                fileSizeBytes);
        }
        return HashAndCompareInstaller(
            file.get(), fileSizeBytes, manifest.digest);
#else
        (void)installerPath;
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kUnsupportedPlatform);
#endif
    } catch (...) {
        return VerificationFailure(
            WindowsInstallerVerificationStatus::kUnexpected);
    }
}

}  // namespace codex_monitor::update
