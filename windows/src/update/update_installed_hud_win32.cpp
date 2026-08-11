#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update/update_installed_hud_win32.h"

#include <array>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "wintrust.lib")
#endif
#endif

namespace codex_monitor::update {
namespace {

struct ParsedVersion {
    unsigned int major = 0;
    unsigned int minor = 0;
    unsigned int patch = 0;
};

bool ParseVersionPart(std::string_view value,
                      unsigned int maximum,
                      unsigned int* output) noexcept {
    if (value.empty() || (value.size() > 1U && value.front() == '0')) {
        return false;
    }
    unsigned int parsed = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') return false;
        const unsigned int digit =
            static_cast<unsigned int>(character - '0');
        if (parsed > (maximum - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    *output = parsed;
    return true;
}

std::optional<ParsedVersion> ParseCanonicalVersion(
    std::string_view version) noexcept {
    const std::size_t firstDot = version.find('.');
    if (firstDot == std::string_view::npos) return std::nullopt;
    const std::size_t secondDot = version.find('.', firstDot + 1U);
    if (secondDot == std::string_view::npos ||
        version.find('.', secondDot + 1U) != std::string_view::npos) {
        return std::nullopt;
    }

    ParsedVersion parsed;
    if (!ParseVersionPart(version.substr(0, firstDot), 255U,
                          &parsed.major) ||
        !ParseVersionPart(
            version.substr(firstDot + 1U,
                           secondDot - firstDot - 1U),
            255U, &parsed.minor) ||
        !ParseVersionPart(version.substr(secondDot + 1U), 65535U,
                          &parsed.patch)) {
        return std::nullopt;
    }
    return parsed;
}

bool EqualsAscii(std::wstring_view wide,
                 std::string_view ascii) noexcept {
    if (wide.size() != ascii.size()) return false;
    for (std::size_t index = 0; index < wide.size(); ++index) {
        if (wide[index] != static_cast<wchar_t>(
                               static_cast<unsigned char>(ascii[index]))) {
            return false;
        }
    }
    return true;
}

WindowsInstalledHudLaunchResult LaunchFailure(
    WindowsInstalledHudLaunchStatus status) noexcept {
    WindowsInstalledHudLaunchResult result;
    result.status = status;
    return result;
}

#ifdef _WIN32

class KernelHandle {
public:
    KernelHandle() = default;
    explicit KernelHandle(HANDLE value) noexcept : value_(value) {}
    ~KernelHandle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;

    KernelHandle(KernelHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    KernelHandle& operator=(KernelHandle&& other) noexcept {
        if (this == &other) return *this;
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
        value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

bool IsAsciiLetter(wchar_t value) noexcept {
    return (value >= L'a' && value <= L'z') ||
           (value >= L'A' && value <= L'Z');
}

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

bool IsReservedDosDeviceSegment(std::wstring_view segment) noexcept {
    const std::size_t dot = segment.find(L'.');
    const std::wstring_view base = segment.substr(0, dot);
    if (base.empty() || base.size() > 4U) return false;

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
    return normalized.size() == 4U &&
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

std::optional<std::wstring> CanonicalInstalledExecutablePath(
    const std::filesystem::path& installedExecutablePath) {
    const std::wstring input = installedExecutablePath.native();
    if (input.size() < 4U || input.size() >= 32760U ||
        !IsAsciiLetter(input[0]) || input[1] != L':' ||
        input[2] != L'\\' || input.back() == L'\\' ||
        input.find(L'/') != std::wstring::npos ||
        input.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }

    const std::wstring root = input.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) return std::nullopt;

    std::wstring normalized(32768U, L'\0');
    const DWORD written = GetFullPathNameW(
        input.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (written == 0 || written >= normalized.size()) return std::nullopt;
    normalized.resize(written);
    if (!SameWindowsPath(input, normalized)) return std::nullopt;

    std::size_t segmentBegin = 3U;
    while (segmentBegin < normalized.size()) {
        const std::size_t separator =
            normalized.find(L'\\', segmentBegin);
        const std::size_t segmentEnd = separator == std::wstring::npos
            ? normalized.size()
            : separator;
        if (!IsCanonicalPathSegment(normalized.substr(
                segmentBegin, segmentEnd - segmentBegin))) {
            return std::nullopt;
        }
        if (separator == std::wstring::npos) break;
        segmentBegin = separator + 1U;
    }

    const std::size_t filenameBegin = normalized.find_last_of(L'\\');
    if (filenameBegin == std::wstring::npos ||
        !SameWindowsPath(normalized.substr(filenameBegin + 1U),
                         kWindowsHudExecutableName)) {
        return std::nullopt;
    }
    return normalized;
}

std::wstring ExtendedDosPath(std::wstring_view path) {
    std::wstring extended = L"\\\\?\\";
    extended.append(path);
    return extended;
}

bool IsNonReparseDirectory(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO information{};
    return GetFileInformationByHandleEx(
               handle, FileAttributeTagInfo, &information,
               static_cast<DWORD>(sizeof(information))) != FALSE &&
           (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool IsNonReparseFile(HANDLE handle) noexcept {
    FILE_ATTRIBUTE_TAG_INFO information{};
    return GetFileInformationByHandleEx(
               handle, FileAttributeTagInfo, &information,
               static_cast<DWORD>(sizeof(information))) != FALSE &&
           (information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

KernelHandle OpenRenameProtectedDirectory(
    const std::wstring& path,
    bool volumeRoot) noexcept {
    const DWORD flags =
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
    if (!volumeRoot) {
        KernelHandle deleteProtected(CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            flags, nullptr));
        if (deleteProtected) return deleteProtected;
        if (GetLastError() != ERROR_ACCESS_DENIED) return {};
    }
    return KernelHandle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        flags, nullptr));
}

std::optional<std::wstring> FinalDosPathFromHandle(HANDLE handle) {
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0,
                                                      kFlags);
    if (required == 0 || required >= 32768U) return std::nullopt;
    std::wstring path(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, path.data(), static_cast<DWORD>(path.size()), kFlags);
    if (written == 0 || written >= path.size()) return std::nullopt;
    path.resize(written);
    return path;
}

struct LockedInstalledExecutable {
    std::vector<KernelHandle> ancestors;
    KernelHandle file;
    std::wstring canonicalPath;
    std::wstring extendedPath;
};

struct LockInstalledExecutableResult {
    WindowsInstalledHudLaunchStatus status =
        WindowsInstalledHudLaunchStatus::kPathIdentityMismatch;
    std::optional<LockedInstalledExecutable> locked;
};

LockInstalledExecutableResult LockInstalledExecutable(
    const std::wstring& canonicalPath) {
    const std::size_t separator = canonicalPath.find_last_of(L'\\');
    if (separator == std::wstring::npos || separator < 2U) return {};
    const std::wstring parent = separator == 2U
        ? canonicalPath.substr(0, 3U)
        : canonicalPath.substr(0, separator);

    LockedInstalledExecutable locked;
    std::vector<std::wstring> ancestorPaths;
    ancestorPaths.push_back(ExtendedDosPath(canonicalPath.substr(0, 3U)));
    std::size_t end = parent.find(L'\\', 3U);
    while (end != std::wstring::npos) {
        ancestorPaths.push_back(ExtendedDosPath(parent.substr(0, end)));
        end = parent.find(L'\\', end + 1U);
    }
    if (parent.size() > 3U) {
        ancestorPaths.push_back(ExtendedDosPath(parent));
    }

    locked.ancestors.reserve(ancestorPaths.size());
    for (std::size_t index = 0; index < ancestorPaths.size(); ++index) {
        KernelHandle directory = OpenRenameProtectedDirectory(
            ancestorPaths[index], index == 0U);
        if (!directory) {
            return {WindowsInstalledHudLaunchStatus::kPathIdentityMismatch,
                    std::nullopt};
        }
        if (!IsNonReparseDirectory(directory.get())) {
            return {WindowsInstalledHudLaunchStatus::kUnsafePathAncestor,
                    std::nullopt};
        }
        locked.ancestors.push_back(std::move(directory));
    }

    locked.extendedPath = ExtendedDosPath(canonicalPath);
    locked.file = KernelHandle(CreateFileW(
        locked.extendedPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!locked.file) {
        return {WindowsInstalledHudLaunchStatus::kFileOpenFailed,
                std::nullopt};
    }
    if (!IsNonReparseFile(locked.file.get())) {
        return {WindowsInstalledHudLaunchStatus::kUnsafeFileType,
                std::nullopt};
    }
    const std::optional<std::wstring> finalPath =
        FinalDosPathFromHandle(locked.file.get());
    if (!finalPath.has_value() ||
        !SameWindowsPath(*finalPath, locked.extendedPath)) {
        return {WindowsInstalledHudLaunchStatus::kPathIdentityMismatch,
                std::nullopt};
    }

    locked.canonicalPath = canonicalPath;
    return {WindowsInstalledHudLaunchStatus::kStarted,
            std::optional<LockedInstalledExecutable>{std::move(locked)}};
}

bool ConstantTimeFingerprintEquals(
    const PublisherCertificateSha256& lhs,
    const PublisherCertificateSha256& rhs) noexcept {
    volatile std::uint8_t difference = 0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        difference = static_cast<std::uint8_t>(
            difference | static_cast<std::uint8_t>(lhs[index] ^ rhs[index]));
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

WindowsInstalledHudLaunchStatus VerifySignatureAndPublisher(
    const LockedInstalledExecutable& locked,
    const PublisherCertificateSha256& trustedFingerprint) noexcept {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = locked.canonicalPath.c_str();
    fileInfo.hFile = locked.file.get();

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
    const LONG trustStatus = WinVerifyTrust(
        reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trustData);
    if (trustStatus != ERROR_SUCCESS) {
        CloseWinTrustState(&action, &trustData);
        return WindowsInstalledHudLaunchStatus::kSignatureVerificationFailed;
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
        return WindowsInstalledHudLaunchStatus::
            kSignerCertificateUnavailable;
    }

    PublisherCertificateSha256 actualFingerprint{};
    DWORD fingerprintBytes =
        static_cast<DWORD>(actualFingerprint.size());
    const BOOL read = CertGetCertificateContextProperty(
        certificate->pCert, CERT_SHA256_HASH_PROP_ID,
        actualFingerprint.data(), &fingerprintBytes);
    CloseWinTrustState(&action, &trustData);
    if (read == FALSE || fingerprintBytes != actualFingerprint.size()) {
        return WindowsInstalledHudLaunchStatus::
            kSignerCertificateUnavailable;
    }
    if (!ConstantTimeFingerprintEquals(actualFingerprint,
                                       trustedFingerprint)) {
        return WindowsInstalledHudLaunchStatus::
            kPublisherFingerprintMismatch;
    }
    return WindowsInstalledHudLaunchStatus::kStarted;
}

std::optional<std::wstring> ReadVersionString(
    const std::vector<std::uint8_t>& versionData,
    WORD language,
    WORD codePage,
    std::wstring_view name) {
    wchar_t query[128]{};
    const int written = swprintf_s(
        query, 128U, L"\\StringFileInfo\\%04x%04x\\%.*s",
        language, codePage, static_cast<int>(name.size()), name.data());
    if (written <= 0 ||
        static_cast<std::size_t>(written) >= 128U) {
        return std::nullopt;
    }

    void* rawValue = nullptr;
    UINT characters = 0;
    if (!VerQueryValueW(
            const_cast<std::uint8_t*>(versionData.data()), query,
            &rawValue, &characters) || rawValue == nullptr ||
        characters == 0U || characters > 512U) {
        return std::nullopt;
    }
    const auto* value = static_cast<const wchar_t*>(rawValue);
    if (value[characters - 1U] != L'\0') return std::nullopt;
    const std::wstring_view bounded(value, characters - 1U);
    if (bounded.find(L'\0') != std::wstring_view::npos) {
        return std::nullopt;
    }
    return std::wstring(bounded);
}

std::optional<WindowsHudExecutableIdentity> ReadExecutableIdentity(
    const LockedInstalledExecutable& locked) {
    DWORD unused = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(
        locked.canonicalPath.c_str(), &unused);
    constexpr DWORD kMaximumVersionResourceBytes = 2U * 1024U * 1024U;
    if (bytes == 0U || bytes > kMaximumVersionResourceBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> versionData(bytes);
    if (!GetFileVersionInfoW(locked.canonicalPath.c_str(), 0, bytes,
                             versionData.data())) {
        return std::nullopt;
    }

    void* rawFixed = nullptr;
    UINT fixedBytes = 0;
    if (!VerQueryValueW(versionData.data(), L"\\", &rawFixed,
                        &fixedBytes) || rawFixed == nullptr ||
        fixedBytes < sizeof(VS_FIXEDFILEINFO)) {
        return std::nullopt;
    }
    const auto* fixed = static_cast<const VS_FIXEDFILEINFO*>(rawFixed);
    if (fixed->dwSignature != VS_FFI_SIGNATURE ||
        fixed->dwFileType != VFT_APP) {
        return std::nullopt;
    }

    void* rawTranslations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(versionData.data(),
                        L"\\VarFileInfo\\Translation",
                        &rawTranslations, &translationBytes) ||
        rawTranslations == nullptr ||
        translationBytes < sizeof(WORD) * 2U ||
        translationBytes % (sizeof(WORD) * 2U) != 0U ||
        translationBytes > sizeof(WORD) * 2U * 16U) {
        return std::nullopt;
    }

    const auto* translations = static_cast<const WORD*>(rawTranslations);
    const std::size_t translationCount =
        translationBytes / (sizeof(WORD) * 2U);
    for (std::size_t index = 0; index < translationCount; ++index) {
        const WORD language = translations[index * 2U];
        const WORD codePage = translations[index * 2U + 1U];
        std::optional<std::wstring> productName = ReadVersionString(
            versionData, language, codePage, L"ProductName");
        std::optional<std::wstring> originalFilename = ReadVersionString(
            versionData, language, codePage, L"OriginalFilename");
        std::optional<std::wstring> fileVersion = ReadVersionString(
            versionData, language, codePage, L"FileVersion");
        std::optional<std::wstring> productVersion = ReadVersionString(
            versionData, language, codePage, L"ProductVersion");
        if (!productName || !originalFilename || !fileVersion ||
            !productVersion) {
            continue;
        }

        WindowsHudExecutableIdentity identity;
        identity.productName = std::move(*productName);
        identity.originalFilename = std::move(*originalFilename);
        identity.fileVersion = std::move(*fileVersion);
        identity.productVersion = std::move(*productVersion);
        identity.majorVersion = HIWORD(fixed->dwFileVersionMS);
        identity.minorVersion = LOWORD(fixed->dwFileVersionMS);
        identity.patchVersion = HIWORD(fixed->dwFileVersionLS);
        if (fixed->dwFileVersionMS != fixed->dwProductVersionMS ||
            fixed->dwFileVersionLS != fixed->dwProductVersionLS ||
            LOWORD(fixed->dwFileVersionLS) != 0U) {
            return std::nullopt;
        }
        return identity;
    }
    return std::nullopt;
}

WindowsInstalledHudLaunchStatus StartLockedExecutable(
    const LockedInstalledExecutable& locked) noexcept {
    std::wstring commandLine = L"\"";
    commandLine.append(locked.canonicalPath);
    commandLine.push_back(L'"');
    const std::filesystem::path workingDirectory =
        std::filesystem::path(locked.canonicalPath).parent_path();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            locked.canonicalPath.c_str(), commandLine.data(), nullptr,
            nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
            workingDirectory.c_str(), &startup, &process)) {
        return WindowsInstalledHudLaunchStatus::kProcessStartFailed;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return WindowsInstalledHudLaunchStatus::kStarted;
}

bool IsLowercaseSha256(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::wstring AsciiToWide(std::string_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

WindowsInstalledHudLaunchStatus StartLockedUpdateHelper(
    const LockedInstalledExecutable& locked,
    const WindowsUpdateHelperChildRequest& request) noexcept {
    if (request.inheritedOldProcessHandle == 0U ||
        request.expectedOldProcessId == 0U ||
        request.expectedOldProcessCreationTime == 0U ||
        request.installerPath.empty() ||
        !request.installerPath.is_absolute() ||
        request.installerPath.native().find(L'"') != std::wstring::npos ||
        !IsLowercaseSha256(request.installerSha256) ||
        !ParseCanonicalVersion(request.targetVersion).has_value() ||
        !ParseCanonicalVersion(request.previousVersion).has_value()) {
        return WindowsInstalledHudLaunchStatus::kInvalidInput;
    }

    HANDLE inheritedHandle = reinterpret_cast<HANDLE>(
        request.inheritedOldProcessHandle);
    DWORD handleFlags = 0;
    if (inheritedHandle == nullptr ||
        inheritedHandle == INVALID_HANDLE_VALUE ||
        !GetHandleInformation(inheritedHandle, &handleFlags) ||
        (handleFlags & HANDLE_FLAG_INHERIT) == 0U) {
        return WindowsInstalledHudLaunchStatus::kInvalidInput;
    }

    SIZE_T attributeBytes = 0;
    static_cast<void>(InitializeProcThreadAttributeList(
        nullptr, 1, 0, &attributeBytes));
    if (attributeBytes == 0U) {
        return WindowsInstalledHudLaunchStatus::kProcessStartFailed;
    }
    std::vector<std::uint8_t> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(
            attributes, 1, 0, &attributeBytes)) {
        return WindowsInstalledHudLaunchStatus::kProcessStartFailed;
    }
    struct AttributeListCleanup {
        PPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~AttributeListCleanup() {
            if (value != nullptr) DeleteProcThreadAttributeList(value);
        }
    } attributeCleanup{attributes};

    HANDLE inheritedHandles[1] = {inheritedHandle};
    if (!UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
        return WindowsInstalledHudLaunchStatus::kProcessStartFailed;
    }

    std::wstring commandLine = L"\"";
    commandLine.append(locked.canonicalPath);
    commandLine.append(L"\" ");
    commandLine.append(kWindowsUpdateHelperMode);
    commandLine.push_back(L' ');
    commandLine.append(std::to_wstring(
        request.inheritedOldProcessHandle));
    commandLine.push_back(L' ');
    commandLine.append(std::to_wstring(request.expectedOldProcessId));
    commandLine.push_back(L' ');
    commandLine.append(std::to_wstring(
        request.expectedOldProcessCreationTime));
    commandLine.append(L" \"");
    commandLine.append(request.installerPath.native());
    commandLine.append(L"\" ");
    commandLine.append(AsciiToWide(request.installerSha256));
    commandLine.push_back(L' ');
    commandLine.append(AsciiToWide(request.targetVersion));
    commandLine.push_back(L' ');
    commandLine.append(AsciiToWide(request.previousVersion));

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const std::filesystem::path workingDirectory =
        std::filesystem::path(locked.canonicalPath).parent_path();
    if (!CreateProcessW(
            locked.canonicalPath.c_str(), commandLine.data(), nullptr,
            nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            nullptr, workingDirectory.c_str(),
            &startup.StartupInfo, &process)) {
        return WindowsInstalledHudLaunchStatus::kProcessStartFailed;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return WindowsInstalledHudLaunchStatus::kStarted;
}

#endif

}  // namespace

WindowsHudExecutableIdentityStatus ValidateWindowsHudExecutableIdentity(
    const WindowsHudExecutableIdentity& identity,
    std::string_view expectedVersion) noexcept {
    const std::optional<ParsedVersion> expected =
        ParseCanonicalVersion(expectedVersion);
    if (!expected.has_value()) {
        return WindowsHudExecutableIdentityStatus::kInvalidExpectedVersion;
    }
    if (identity.productName != kWindowsHudProductName) {
        return WindowsHudExecutableIdentityStatus::kProductNameMismatch;
    }
    if (identity.originalFilename != kWindowsHudExecutableName) {
        return WindowsHudExecutableIdentityStatus::kOriginalFilenameMismatch;
    }
    if (!EqualsAscii(identity.fileVersion, expectedVersion)) {
        return WindowsHudExecutableIdentityStatus::kFileVersionMismatch;
    }
    if (!EqualsAscii(identity.productVersion, expectedVersion)) {
        return WindowsHudExecutableIdentityStatus::kProductVersionMismatch;
    }
    if (identity.majorVersion != expected->major ||
        identity.minorVersion != expected->minor ||
        identity.patchVersion != expected->patch) {
        return WindowsHudExecutableIdentityStatus::kFixedVersionMismatch;
    }
    return WindowsHudExecutableIdentityStatus::kValid;
}

WindowsInstalledHudLaunchResult VerifyAndLaunchInstalledWindowsHud(
    const std::filesystem::path& installedExecutablePath,
    std::string_view expectedVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint) noexcept {
    try {
        if (!trustedPublisherFingerprint.has_value() ||
            !ParseCanonicalVersion(expectedVersion).has_value()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kInvalidInput);
        }
        if (installedExecutablePath.empty() ||
            !installedExecutablePath.is_absolute()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kPathNotAbsolute);
        }

#ifdef _WIN32
        const std::optional<std::wstring> canonical =
            CanonicalInstalledExecutablePath(installedExecutablePath);
        if (!canonical.has_value()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kPathNotCanonical);
        }
        LockInstalledExecutableResult lockResult =
            LockInstalledExecutable(*canonical);
        if (!lockResult.locked.has_value()) {
            return LaunchFailure(lockResult.status);
        }
        LockedInstalledExecutable& locked = *lockResult.locked;

        const WindowsInstalledHudLaunchStatus signatureStatus =
            VerifySignatureAndPublisher(locked,
                                        *trustedPublisherFingerprint);
        if (signatureStatus != WindowsInstalledHudLaunchStatus::kStarted) {
            return LaunchFailure(signatureStatus);
        }

        const std::optional<WindowsHudExecutableIdentity> identity =
            ReadExecutableIdentity(locked);
        if (!identity.has_value()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::
                    kVersionResourceUnavailable);
        }
        const WindowsHudExecutableIdentityStatus identityStatus =
            ValidateWindowsHudExecutableIdentity(*identity,
                                                 expectedVersion);
        if (identityStatus != WindowsHudExecutableIdentityStatus::kValid) {
            WindowsInstalledHudLaunchResult result = LaunchFailure(
                WindowsInstalledHudLaunchStatus::
                    kExecutableIdentityRejected);
            result.identityStatus = identityStatus;
            return result;
        }

        return LaunchFailure(StartLockedExecutable(locked));
#else
        (void)installedExecutablePath;
        return LaunchFailure(
            WindowsInstalledHudLaunchStatus::kUnsupportedPlatform);
#endif
    } catch (...) {
        return LaunchFailure(WindowsInstalledHudLaunchStatus::kUnexpected);
    }
}

WindowsInstalledHudLaunchResult VerifyAndLaunchWindowsUpdateHelperCopy(
    const std::filesystem::path& helperExecutablePath,
    std::string_view currentVersion,
    const std::optional<PublisherCertificateSha256>&
        trustedPublisherFingerprint,
    const WindowsUpdateHelperChildRequest& request) noexcept {
    try {
        if (!trustedPublisherFingerprint.has_value() ||
            !ParseCanonicalVersion(currentVersion).has_value()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kInvalidInput);
        }
        if (helperExecutablePath.empty() ||
            !helperExecutablePath.is_absolute()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kPathNotAbsolute);
        }

#ifdef _WIN32
        const std::optional<std::wstring> canonical =
            CanonicalInstalledExecutablePath(helperExecutablePath);
        if (!canonical.has_value()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kPathNotCanonical);
        }
        LockInstalledExecutableResult lockResult =
            LockInstalledExecutable(*canonical);
        if (!lockResult.locked.has_value()) {
            return LaunchFailure(lockResult.status);
        }
        LockedInstalledExecutable& locked = *lockResult.locked;

        const WindowsInstalledHudLaunchStatus signatureStatus =
            VerifySignatureAndPublisher(locked,
                                        *trustedPublisherFingerprint);
        if (signatureStatus != WindowsInstalledHudLaunchStatus::kStarted) {
            return LaunchFailure(signatureStatus);
        }
        const std::optional<WindowsHudExecutableIdentity> identity =
            ReadExecutableIdentity(locked);
        if (!identity.has_value()) {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::
                    kVersionResourceUnavailable);
        }
        const WindowsHudExecutableIdentityStatus identityStatus =
            ValidateWindowsHudExecutableIdentity(*identity,
                                                 currentVersion);
        if (identityStatus != WindowsHudExecutableIdentityStatus::kValid) {
            WindowsInstalledHudLaunchResult result = LaunchFailure(
                WindowsInstalledHudLaunchStatus::
                    kExecutableIdentityRejected);
            result.identityStatus = identityStatus;
            return result;
        }
        return LaunchFailure(StartLockedUpdateHelper(locked, request));
#else
        (void)helperExecutablePath;
        (void)request;
        return LaunchFailure(
            WindowsInstalledHudLaunchStatus::kUnsupportedPlatform);
#endif
    } catch (...) {
        return LaunchFailure(WindowsInstalledHudLaunchStatus::kUnexpected);
    }
}

}  // namespace codex_monitor::update
