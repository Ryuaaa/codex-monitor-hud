#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_msi_identity_win32.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <propidl.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "msi.lib")
#pragma comment(lib, "wintrust.lib")
#endif

namespace codex_monitor::update {
namespace {

bool IsAsciiDigit(char value) noexcept {
    return value >= '0' && value <= '9';
}

bool ParseCanonicalVersionPart(std::string_view value,
                               std::uint32_t maximum) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) {
        return false;
    }

    std::uint32_t parsed = 0;
    for (const char character : value) {
        if (!IsAsciiDigit(character)) return false;
        const std::uint32_t digit =
            static_cast<std::uint32_t>(character - '0');
        if (parsed > (maximum - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    return parsed <= maximum;
}

bool IsCanonicalWindowsInstallerVersion(
    std::string_view version) noexcept {
    const std::size_t firstDot = version.find('.');
    if (firstDot == std::string_view::npos) return false;
    const std::size_t secondDot = version.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos ||
        version.find('.', secondDot + 1) != std::string_view::npos) {
        return false;
    }

    return ParseCanonicalVersionPart(version.substr(0, firstDot), 255U) &&
           ParseCanonicalVersionPart(
               version.substr(firstDot + 1, secondDot - firstDot - 1),
               255U) &&
           ParseCanonicalVersionPart(version.substr(secondDot + 1), 65535U);
}

bool EqualsExpectedVersion(const std::wstring& actual,
                           std::string_view expected) noexcept {
    if (actual.size() != expected.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != static_cast<wchar_t>(
                                 static_cast<unsigned char>(expected[index]))) {
            return false;
        }
    }
    return true;
}

WindowsMsiIdentityVerificationResult VerificationFailure(
    WindowsMsiIdentityVerificationStatus status) noexcept {
    WindowsMsiIdentityVerificationResult result;
    result.status = status;
    return result;
}

#ifdef _WIN32

constexpr std::size_t kMaximumProductNameCharacters = 256;
constexpr std::size_t kMaximumProductVersionCharacters = 64;
constexpr std::size_t kMaximumUpgradeCodeCharacters = 64;
constexpr std::size_t kMaximumTemplateCharacters = 64;

class FileHandle {
public:
    explicit FileHandle(HANDLE value) noexcept : value_(value) {}
    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    FileHandle(FileHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    FileHandle& operator=(FileHandle&& other) noexcept {
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

bool ReadFileIdentity(HANDLE file, FileIdentity* identity) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file, &information) == FALSE) return false;
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

std::optional<std::filesystem::path> NormalizeAbsolutePath(
    const std::filesystem::path& path) {
    const std::wstring& nativePath = path.native();
    if (nativePath.empty() || nativePath.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }

    const DWORD requiredCharacters =
        GetFullPathNameW(nativePath.c_str(), 0, nullptr, nullptr);
    if (requiredCharacters == 0) return std::nullopt;
    std::vector<wchar_t> buffer(requiredCharacters, L'\0');
    const DWORD writtenCharacters = GetFullPathNameW(
        nativePath.c_str(), requiredCharacters, buffer.data(), nullptr);
    if (writtenCharacters == 0 || writtenCharacters >= requiredCharacters) {
        return std::nullopt;
    }

    std::filesystem::path normalized(
        std::wstring(buffer.data(), writtenCharacters));
    if (!normalized.is_absolute()) return std::nullopt;
    return normalized;
}

WindowsMsiIdentityVerificationStatus OpenAndHoldSafeAncestors(
    const std::filesystem::path& absolutePath,
    std::vector<FileHandle>* heldAncestors) {
    const std::filesystem::path parent = absolutePath.parent_path();
    if (parent.empty() || parent.root_path().empty()) {
        return WindowsMsiIdentityVerificationStatus::kPathResolutionFailed;
    }

    const auto openAndHold = [heldAncestors](
                                 const std::filesystem::path& current) {
        // Permit ordinary reads and writes inside the directory while
        // withholding FILE_SHARE_DELETE so this exact ancestor cannot be
        // renamed or replaced until verification finishes.
        FileHandle ancestor(CreateFileW(
            current.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!ancestor) {
            return WindowsMsiIdentityVerificationStatus::kPathResolutionFailed;
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                ancestor.get(), FileAttributeTagInfo, &attributes,
                static_cast<DWORD>(sizeof(attributes))) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return WindowsMsiIdentityVerificationStatus::kPathResolutionFailed;
        }
        if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return WindowsMsiIdentityVerificationStatus::kUnsafePathAncestor;
        }
        heldAncestors->push_back(std::move(ancestor));
        return WindowsMsiIdentityVerificationStatus::kVerified;
    };

    std::filesystem::path current = parent.root_path();
    WindowsMsiIdentityVerificationStatus status = openAndHold(current);
    if (status != WindowsMsiIdentityVerificationStatus::kVerified) {
        return status;
    }
    for (const std::filesystem::path& component : parent.relative_path()) {
        if (component.empty() || component == L"." || component == L"..") {
            return WindowsMsiIdentityVerificationStatus::kPathResolutionFailed;
        }
        current /= component;
        status = openAndHold(current);
        if (status != WindowsMsiIdentityVerificationStatus::kVerified) {
            return status;
        }
    }
    return WindowsMsiIdentityVerificationStatus::kVerified;
}

std::optional<std::filesystem::path> FinalAbsolutePathFromHandle(HANDLE file) {
    constexpr DWORD kFinalPathFlags =
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD requiredCharacters =
        GetFinalPathNameByHandleW(file, nullptr, 0, kFinalPathFlags);
    if (requiredCharacters == 0) return std::nullopt;
    std::vector<wchar_t> buffer(requiredCharacters, L'\0');
    const DWORD writtenCharacters = GetFinalPathNameByHandleW(
        file, buffer.data(), requiredCharacters, kFinalPathFlags);
    if (writtenCharacters == 0 || writtenCharacters >= requiredCharacters) {
        return std::nullopt;
    }

    std::wstring finalPath(buffer.data(), writtenCharacters);
    if (finalPath.rfind(L"\\\\?\\", 0) != 0) return std::nullopt;
    return std::filesystem::path(std::move(finalPath));
}

class MsiHandle {
public:
    MsiHandle() = default;
    ~MsiHandle() {
        if (value_ != 0) MsiCloseHandle(value_);
    }

    MsiHandle(const MsiHandle&) = delete;
    MsiHandle& operator=(const MsiHandle&) = delete;

    [[nodiscard]] MSIHANDLE get() const noexcept { return value_; }
    [[nodiscard]] MSIHANDLE* put() noexcept { return &value_; }

private:
    MSIHANDLE value_ = 0;
};

bool ConstantTimeFingerprintEquals(
    const PublisherCertificateSha256& lhs,
    const PublisherCertificateSha256& rhs) noexcept {
    volatile std::uint8_t difference = 0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        difference = static_cast<std::uint8_t>(
            difference |
            static_cast<std::uint8_t>(lhs[index] ^ rhs[index]));
    }
    return difference == 0;
}

void CloseWinTrustState(GUID* action,
                        WINTRUST_DATA* trustData) noexcept {
    if (trustData->hWVTStateData == nullptr) return;
    trustData->dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(WinVerifyTrust(
        reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), action, trustData));
}

WindowsMsiIdentityVerificationStatus VerifySignatureAndPublisher(
    const std::filesystem::path& installerPath,
    HANDLE lockedFile,
    const PublisherCertificateSha256& trustedFingerprint) noexcept {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = installerPath.c_str();
    fileInfo.hFile = lockedFile;

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags =
        WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_DISABLE_MD2_MD4;
    trustData.dwUIContext = WTD_UICONTEXT_EXECUTE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG trustStatus =
        WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
                       &action, &trustData);
    if (trustStatus != ERROR_SUCCESS) {
        CloseWinTrustState(&action, &trustData);
        return WindowsMsiIdentityVerificationStatus::
            kSignatureVerificationFailed;
    }

    CRYPT_PROVIDER_DATA* provider =
        WTHelperProvDataFromStateData(trustData.hWVTStateData);
    CRYPT_PROVIDER_SGNR* signer = provider == nullptr
        ? nullptr
        : WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    CRYPT_PROVIDER_CERT* certificate = signer == nullptr
        ? nullptr
        : WTHelperGetProvCertFromChain(signer, 0);
    if (certificate == nullptr || certificate->pCert == nullptr) {
        CloseWinTrustState(&action, &trustData);
        return WindowsMsiIdentityVerificationStatus::
            kSignerCertificateUnavailable;
    }

    PublisherCertificateSha256 actualFingerprint{};
    DWORD fingerprintBytes =
        static_cast<DWORD>(actualFingerprint.size());
    const BOOL fingerprintRead = CertGetCertificateContextProperty(
        certificate->pCert, CERT_SHA256_HASH_PROP_ID,
        actualFingerprint.data(), &fingerprintBytes);
    CloseWinTrustState(&action, &trustData);

    if (fingerprintRead == FALSE ||
        fingerprintBytes != actualFingerprint.size()) {
        return WindowsMsiIdentityVerificationStatus::
            kSignerCertificateUnavailable;
    }
    if (!ConstantTimeFingerprintEquals(
            actualFingerprint, trustedFingerprint)) {
        return WindowsMsiIdentityVerificationStatus::
            kPublisherFingerprintMismatch;
    }
    return WindowsMsiIdentityVerificationStatus::kVerified;
}

enum class MsiTextReadStatus {
    kSuccess,
    kQueryFailed,
    kMissing,
    kDuplicate,
    kTooLong,
    kTypeMismatch,
};

bool IsMsiStringColumnType(std::wstring_view type) noexcept {
    if (type.size() < 2) return false;
    const wchar_t kind = type.front();
    if (kind != L's' && kind != L'S' && kind != L'l' && kind != L'L') {
        return false;
    }
    return std::all_of(type.begin() + 1, type.end(), [](wchar_t value) {
        return value >= L'0' && value <= L'9';
    });
}

MsiTextReadStatus ReadRecordString(MSIHANDLE record,
                                   UINT field,
                                   std::size_t maximumCharacters,
                                   std::wstring* output) {
    if (MsiRecordIsNull(record, field) != FALSE) {
        return MsiTextReadStatus::kMissing;
    }

    wchar_t emptyBuffer[1] = {L'\0'};
    DWORD requiredCharacters = 0;
    const UINT lengthStatus = MsiRecordGetStringW(
        record, field, emptyBuffer, &requiredCharacters);
    if (lengthStatus != ERROR_SUCCESS &&
        lengthStatus != ERROR_MORE_DATA) {
        return MsiTextReadStatus::kQueryFailed;
    }
    if (requiredCharacters == 0) return MsiTextReadStatus::kMissing;
    if (requiredCharacters > maximumCharacters ||
        requiredCharacters == std::numeric_limits<DWORD>::max()) {
        return MsiTextReadStatus::kTooLong;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(requiredCharacters) + 1U, L'\0');
    DWORD bufferCharacters = requiredCharacters + 1U;
    if (MsiRecordGetStringW(record, field, buffer.data(),
                            &bufferCharacters) != ERROR_SUCCESS ||
        bufferCharacters == 0 ||
        bufferCharacters > requiredCharacters) {
        return MsiTextReadStatus::kQueryFailed;
    }
    output->assign(buffer.data(), bufferCharacters);
    return MsiTextReadStatus::kSuccess;
}

MsiTextReadStatus ReadMsiProperty(MSIHANDLE database,
                                  const wchar_t* query,
                                  std::size_t maximumCharacters,
                                  std::wstring* output) {
    MsiHandle view;
    if (MsiDatabaseOpenViewW(database, query, view.put()) != ERROR_SUCCESS) {
        return MsiTextReadStatus::kQueryFailed;
    }

    MsiHandle columnInfo;
    if (MsiViewGetColumnInfo(view.get(), MSICOLINFO_TYPES,
                             columnInfo.put()) != ERROR_SUCCESS) {
        return MsiTextReadStatus::kQueryFailed;
    }
    std::wstring columnType;
    const MsiTextReadStatus typeRead = ReadRecordString(
        columnInfo.get(), 1, 16, &columnType);
    if (typeRead != MsiTextReadStatus::kSuccess ||
        !IsMsiStringColumnType(columnType)) {
        return MsiTextReadStatus::kTypeMismatch;
    }

    if (MsiViewExecute(view.get(), 0) != ERROR_SUCCESS) {
        return MsiTextReadStatus::kQueryFailed;
    }
    MsiHandle record;
    const UINT fetchStatus = MsiViewFetch(view.get(), record.put());
    if (fetchStatus == ERROR_NO_MORE_ITEMS) {
        return MsiTextReadStatus::kMissing;
    }
    if (fetchStatus != ERROR_SUCCESS) {
        return MsiTextReadStatus::kQueryFailed;
    }

    const MsiTextReadStatus valueStatus = ReadRecordString(
        record.get(), 1, maximumCharacters, output);
    if (valueStatus != MsiTextReadStatus::kSuccess) return valueStatus;

    MsiHandle duplicate;
    const UINT duplicateStatus = MsiViewFetch(view.get(), duplicate.put());
    if (duplicateStatus == ERROR_SUCCESS) {
        return MsiTextReadStatus::kDuplicate;
    }
    if (duplicateStatus != ERROR_NO_MORE_ITEMS) {
        return MsiTextReadStatus::kQueryFailed;
    }
    static_cast<void>(MsiViewClose(view.get()));
    return MsiTextReadStatus::kSuccess;
}

MsiTextReadStatus ReadMsiTemplate(MSIHANDLE database,
                                  std::wstring* output) {
    MsiHandle summary;
    if (MsiGetSummaryInformationW(database, nullptr, 0,
                                  summary.put()) != ERROR_SUCCESS) {
        return MsiTextReadStatus::kQueryFailed;
    }

    UINT dataType = VT_EMPTY;
    INT integerValue = 0;
    FILETIME fileTime{};
    wchar_t emptyBuffer[1] = {L'\0'};
    DWORD requiredCharacters = 0;
    const UINT lengthStatus = MsiSummaryInfoGetPropertyW(
        summary.get(), PID_TEMPLATE, &dataType, &integerValue, &fileTime,
        emptyBuffer, &requiredCharacters);
    if (dataType == VT_EMPTY) return MsiTextReadStatus::kMissing;
    if (dataType != VT_LPSTR) return MsiTextReadStatus::kTypeMismatch;
    if (lengthStatus != ERROR_SUCCESS &&
        lengthStatus != ERROR_MORE_DATA) {
        return MsiTextReadStatus::kQueryFailed;
    }
    if (requiredCharacters == 0) return MsiTextReadStatus::kMissing;
    if (requiredCharacters > kMaximumTemplateCharacters ||
        requiredCharacters == std::numeric_limits<DWORD>::max()) {
        return MsiTextReadStatus::kTooLong;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(requiredCharacters) + 1U, L'\0');
    DWORD bufferCharacters = requiredCharacters + 1U;
    dataType = VT_EMPTY;
    if (MsiSummaryInfoGetPropertyW(
            summary.get(), PID_TEMPLATE, &dataType, &integerValue,
            &fileTime, buffer.data(), &bufferCharacters) != ERROR_SUCCESS) {
        return MsiTextReadStatus::kQueryFailed;
    }
    if (dataType != VT_LPSTR) return MsiTextReadStatus::kTypeMismatch;
    if (bufferCharacters == 0 ||
        bufferCharacters > requiredCharacters) {
        return MsiTextReadStatus::kQueryFailed;
    }
    output->assign(buffer.data(), bufferCharacters);
    return MsiTextReadStatus::kSuccess;
}

WindowsMsiIdentityVerificationStatus MapMsiPropertyStatus(
    MsiTextReadStatus status) noexcept {
    switch (status) {
        case MsiTextReadStatus::kQueryFailed:
            return WindowsMsiIdentityVerificationStatus::
                kMsiPropertyQueryFailed;
        case MsiTextReadStatus::kMissing:
            return WindowsMsiIdentityVerificationStatus::kMsiPropertyMissing;
        case MsiTextReadStatus::kDuplicate:
            return WindowsMsiIdentityVerificationStatus::
                kMsiPropertyDuplicate;
        case MsiTextReadStatus::kTooLong:
            return WindowsMsiIdentityVerificationStatus::
                kMsiPropertyTooLong;
        case MsiTextReadStatus::kTypeMismatch:
            return WindowsMsiIdentityVerificationStatus::
                kMsiPropertyTypeMismatch;
        case MsiTextReadStatus::kSuccess:
            break;
    }
    return WindowsMsiIdentityVerificationStatus::kUnexpected;
}

WindowsMsiIdentityVerificationStatus MapMsiTemplateStatus(
    MsiTextReadStatus status) noexcept {
    switch (status) {
        case MsiTextReadStatus::kQueryFailed:
        case MsiTextReadStatus::kDuplicate:
            return WindowsMsiIdentityVerificationStatus::
                kMsiSummaryQueryFailed;
        case MsiTextReadStatus::kMissing:
            return WindowsMsiIdentityVerificationStatus::kMsiTemplateMissing;
        case MsiTextReadStatus::kTooLong:
            return WindowsMsiIdentityVerificationStatus::kMsiTemplateTooLong;
        case MsiTextReadStatus::kTypeMismatch:
            return WindowsMsiIdentityVerificationStatus::
                kMsiTemplateTypeMismatch;
        case MsiTextReadStatus::kSuccess:
            break;
    }
    return WindowsMsiIdentityVerificationStatus::kUnexpected;
}

#endif

}  // namespace

WindowsMsiIdentityPolicyStatus ValidateWindowsMsiIdentity(
    const WindowsMsiIdentity& identity,
    std::string_view expectedVersion) noexcept {
    if (!IsCanonicalWindowsInstallerVersion(expectedVersion)) {
        return WindowsMsiIdentityPolicyStatus::kInvalidExpectedVersion;
    }
    if (identity.productName != kWindowsMsiProductName) {
        return WindowsMsiIdentityPolicyStatus::kProductNameMismatch;
    }
    if (!EqualsExpectedVersion(identity.productVersion, expectedVersion)) {
        return WindowsMsiIdentityPolicyStatus::kProductVersionMismatch;
    }
    if (identity.upgradeCode != kWindowsMsiUpgradeCode) {
        return WindowsMsiIdentityPolicyStatus::kUpgradeCodeMismatch;
    }
    if (identity.templateValue != kWindowsMsiTemplate) {
        return WindowsMsiIdentityPolicyStatus::kTemplateMismatch;
    }
    return WindowsMsiIdentityPolicyStatus::kValid;
}

WindowsMsiIdentityVerificationResult
VerifyWindowsMsiIdentityAndPublisher(
    const std::filesystem::path& installerPath,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint) noexcept {
    if (!trustedPublisherFingerprint.has_value()) {
        return VerificationFailure(
            WindowsMsiIdentityVerificationStatus::
                kMissingTrustedPublisherFingerprint);
    }
    if (!IsCanonicalWindowsInstallerVersion(expectedVersion)) {
        return VerificationFailure(
            WindowsMsiIdentityVerificationStatus::kInvalidExpectedVersion);
    }
    if (installerPath.empty() || !installerPath.is_absolute()) {
        return VerificationFailure(
            WindowsMsiIdentityVerificationStatus::kPathNotAbsolute);
    }

    try {
#ifdef _WIN32
        const std::optional<std::filesystem::path> normalizedPath =
            NormalizeAbsolutePath(installerPath);
        if (!normalizedPath.has_value()) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kPathResolutionFailed);
        }

        std::vector<FileHandle> heldAncestors;
        const WindowsMsiIdentityVerificationStatus ancestorStatus =
            OpenAndHoldSafeAncestors(*normalizedPath, &heldAncestors);
        if (ancestorStatus != WindowsMsiIdentityVerificationStatus::kVerified) {
            return VerificationFailure(ancestorStatus);
        }

        FileHandle lockedFile(CreateFileW(
            normalizedPath->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!lockedFile) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kFileOpenFailed);
        }

        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                lockedFile.get(), FileAttributeTagInfo, &attributes,
                static_cast<DWORD>(sizeof(attributes))) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kUnsafeFileType);
        }

        const std::optional<std::filesystem::path> finalPath =
            FinalAbsolutePathFromHandle(lockedFile.get());
        if (!finalPath.has_value()) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kPathResolutionFailed);
        }

        FileIdentity lockedIdentity;
        if (!ReadFileIdentity(lockedFile.get(), &lockedIdentity)) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kPathResolutionFailed);
        }

        FileHandle reopenedFile(CreateFileW(
            finalPath->c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!reopenedFile) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kPathResolutionFailed);
        }
        FILE_ATTRIBUTE_TAG_INFO reopenedAttributes{};
        if (!GetFileInformationByHandleEx(
                reopenedFile.get(), FileAttributeTagInfo,
                &reopenedAttributes,
                static_cast<DWORD>(sizeof(reopenedAttributes))) ||
            (reopenedAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (reopenedAttributes.FileAttributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kUnsafeFileType);
        }
        FileIdentity reopenedIdentity;
        if (!ReadFileIdentity(reopenedFile.get(), &reopenedIdentity) ||
            !SameFileIdentity(lockedIdentity, reopenedIdentity)) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kFileIdentityMismatch);
        }

        const WindowsMsiIdentityVerificationStatus signatureStatus =
            VerifySignatureAndPublisher(*finalPath, lockedFile.get(),
                                        *trustedPublisherFingerprint);
        if (signatureStatus !=
            WindowsMsiIdentityVerificationStatus::kVerified) {
            return VerificationFailure(signatureStatus);
        }

        MsiHandle database;
        // A null persistence parameter is MSIDBOPEN_READONLY for the explicit
        // Unicode API and cannot create or modify an installer database.
        if (MsiOpenDatabaseW(finalPath->c_str(), nullptr,
                             database.put()) != ERROR_SUCCESS) {
            return VerificationFailure(
                WindowsMsiIdentityVerificationStatus::kMsiOpenFailed);
        }

        WindowsMsiIdentity identity;
        constexpr const wchar_t* kProductNameQuery =
            L"SELECT `Value` FROM `Property` WHERE `Property`="
            L"'ProductName'";
        constexpr const wchar_t* kProductVersionQuery =
            L"SELECT `Value` FROM `Property` WHERE `Property`="
            L"'ProductVersion'";
        constexpr const wchar_t* kUpgradeCodeQuery =
            L"SELECT `Value` FROM `Property` WHERE `Property`="
            L"'UpgradeCode'";

        const std::pair<const wchar_t*, std::pair<std::size_t,
                                                  std::wstring*>>
            properties[] = {
                {kProductNameQuery,
                 {kMaximumProductNameCharacters, &identity.productName}},
                {kProductVersionQuery,
                 {kMaximumProductVersionCharacters,
                  &identity.productVersion}},
                {kUpgradeCodeQuery,
                 {kMaximumUpgradeCodeCharacters, &identity.upgradeCode}},
            };
        for (const auto& property : properties) {
            const MsiTextReadStatus status = ReadMsiProperty(
                database.get(), property.first, property.second.first,
                property.second.second);
            if (status != MsiTextReadStatus::kSuccess) {
                return VerificationFailure(MapMsiPropertyStatus(status));
            }
        }

        const MsiTextReadStatus templateStatus =
            ReadMsiTemplate(database.get(), &identity.templateValue);
        if (templateStatus != MsiTextReadStatus::kSuccess) {
            return VerificationFailure(MapMsiTemplateStatus(templateStatus));
        }

        const WindowsMsiIdentityPolicyStatus policyStatus =
            ValidateWindowsMsiIdentity(identity, expectedVersion);
        if (policyStatus != WindowsMsiIdentityPolicyStatus::kValid) {
            WindowsMsiIdentityVerificationResult result =
                VerificationFailure(
                    WindowsMsiIdentityVerificationStatus::kIdentityRejected);
            result.policyStatus = policyStatus;
            return result;
        }
        WindowsMsiIdentityVerificationResult result;
        result.status = WindowsMsiIdentityVerificationStatus::kVerified;
        return result;
#else
        (void)installerPath;
        return VerificationFailure(
            WindowsMsiIdentityVerificationStatus::kUnsupportedPlatform);
#endif
    } catch (...) {
        return VerificationFailure(
            WindowsMsiIdentityVerificationStatus::kUnexpected);
    }
}

}  // namespace codex_monitor::update
