#include "update/update_asset_download_win32.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace codex_monitor::update {
namespace {

constexpr std::wstring_view kInitialHost = L"github.com";
constexpr std::wstring_view kInitialPathPrefix =
    L"/Ryuaaa/codex-monitor-hud/releases/download/";
constexpr std::wstring_view kReleaseAssetCdnHost =
    L"release-assets.githubusercontent.com";
constexpr std::wstring_view kReleaseAssetCdnPathPrefix =
    L"/github-production-release-asset/";
constexpr std::wstring_view kWindowsAssetFilenamePrefix =
    L"CodexMonitorHUD-windows-x64-";
constexpr std::wstring_view kInstallerFilenameSuffix = L".msi";
constexpr std::wstring_view kChecksumFilenameSuffix = L".msi.sha256";
constexpr std::size_t kMaximumUrlCharacters = 16 * 1024;
constexpr std::size_t kMaximumFilenameCharacters = 240;

struct ParsedAbsoluteHttpsUrl {
    std::wstring host;
    std::wstring pathAndQuery;
};

bool IsAsciiLetter(wchar_t value) noexcept {
    return (value >= L'a' && value <= L'z') ||
           (value >= L'A' && value <= L'Z');
}

bool IsAsciiDigit(wchar_t value) noexcept {
    return value >= L'0' && value <= L'9';
}

bool IsHexDigit(wchar_t value) noexcept {
    return IsAsciiDigit(value) ||
           (value >= L'a' && value <= L'f') ||
           (value >= L'A' && value <= L'F');
}

unsigned int HexDigitValue(wchar_t value) noexcept {
    if (IsAsciiDigit(value)) {
        return static_cast<unsigned int>(value - L'0');
    }
    if (value >= L'a' && value <= L'f') {
        return static_cast<unsigned int>(value - L'a') + 10U;
    }
    return static_cast<unsigned int>(value - L'A') + 10U;
}

bool EqualsAsciiCaseInsensitive(std::wstring_view lhs,
                                std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        wchar_t left = lhs[index];
        wchar_t right = rhs[index];
        if (left >= L'A' && left <= L'Z') left += L'a' - L'A';
        if (right >= L'A' && right <= L'Z') right += L'a' - L'A';
        if (left != right) return false;
    }
    return true;
}

bool IsPrintableAsciiUrl(std::wstring_view value) noexcept {
    if (value.empty() || value.size() > kMaximumUrlCharacters) return false;
    for (const wchar_t character : value) {
        if (character < 0x21 || character > 0x7e) return false;
    }
    return true;
}

std::optional<ParsedAbsoluteHttpsUrl> ParseAbsoluteHttpsUrl(
    std::wstring_view url) {
    if (!IsPrintableAsciiUrl(url) || url.find(L'#') != std::wstring_view::npos) {
        return std::nullopt;
    }

    const std::size_t schemeSeparator = url.find(L"://");
    if (schemeSeparator == std::wstring_view::npos ||
        !EqualsAsciiCaseInsensitive(url.substr(0, schemeSeparator), L"https")) {
        return std::nullopt;
    }

    const std::size_t authorityBegin = schemeSeparator + 3;
    const std::size_t pathBegin = url.find(L'/', authorityBegin);
    if (pathBegin == std::wstring_view::npos || pathBegin == authorityBegin) {
        return std::nullopt;
    }
    const std::wstring_view authority =
        url.substr(authorityBegin, pathBegin - authorityBegin);
    if (authority.find(L'@') != std::wstring_view::npos ||
        authority.find(L':') != std::wstring_view::npos ||
        authority.front() == L'.' || authority.back() == L'.') {
        return std::nullopt;
    }

    std::wstring host;
    host.reserve(authority.size());
    for (wchar_t character : authority) {
        if (!(IsAsciiLetter(character) || IsAsciiDigit(character) ||
              character == L'-' || character == L'.')) {
            return std::nullopt;
        }
        if (character >= L'A' && character <= L'Z') {
            character += L'a' - L'A';
        }
        host.push_back(character);
    }

    const std::wstring_view pathAndQuery = url.substr(pathBegin);
    if (pathAndQuery.find(L'\\') != std::wstring_view::npos) {
        return std::nullopt;
    }
    return ParsedAbsoluteHttpsUrl{std::move(host),
                                  std::wstring(pathAndQuery)};
}

bool IsSafeExpectedFilename(std::wstring_view filename) noexcept {
    if (filename.empty() || filename.size() > kMaximumFilenameCharacters ||
        filename.front() == L'.' || filename.back() == L'.') {
        return false;
    }
    if (filename.find(L"..") != std::wstring_view::npos) return false;
    for (const wchar_t character : filename) {
        if (!(IsAsciiLetter(character) || IsAsciiDigit(character) ||
              character == L'-' || character == L'_' ||
              character == L'.')) {
            return false;
        }
    }
    return true;
}

bool IsCanonicalVersionPart(std::wstring_view value) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == L'0')) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), IsAsciiDigit);
}

std::optional<std::wstring_view> ExtractWindowsAssetVersion(
    std::wstring_view filename) noexcept {
    if (!IsSafeExpectedFilename(filename) ||
        filename.size() <= kWindowsAssetFilenamePrefix.size() ||
        filename.substr(0, kWindowsAssetFilenamePrefix.size()) !=
            kWindowsAssetFilenamePrefix) {
        return std::nullopt;
    }

    std::wstring_view suffix;
    if (filename.size() >= kChecksumFilenameSuffix.size() &&
        filename.substr(filename.size() - kChecksumFilenameSuffix.size()) ==
            kChecksumFilenameSuffix) {
        suffix = kChecksumFilenameSuffix;
    } else if (filename.size() >= kInstallerFilenameSuffix.size() &&
               filename.substr(filename.size() -
                               kInstallerFilenameSuffix.size()) ==
                   kInstallerFilenameSuffix) {
        suffix = kInstallerFilenameSuffix;
    } else {
        return std::nullopt;
    }

    const std::size_t versionBegin = kWindowsAssetFilenamePrefix.size();
    const std::size_t versionLength =
        filename.size() - versionBegin - suffix.size();
    const std::wstring_view version =
        filename.substr(versionBegin, versionLength);
    const std::size_t firstDot = version.find(L'.');
    if (firstDot == std::wstring_view::npos) return std::nullopt;
    const std::size_t secondDot = version.find(L'.', firstDot + 1);
    if (secondDot == std::wstring_view::npos ||
        version.find(L'.', secondDot + 1) != std::wstring_view::npos ||
        !IsCanonicalVersionPart(version.substr(0, firstDot)) ||
        !IsCanonicalVersionPart(version.substr(
            firstDot + 1, secondDot - firstDot - 1)) ||
        !IsCanonicalVersionPart(version.substr(secondDot + 1))) {
        return std::nullopt;
    }
    return version;
}

bool IsSafeReleaseTag(std::wstring_view tag) noexcept {
    if (tag.empty() || tag.size() > 128 || tag.front() == L'.' ||
        tag.back() == L'.' || tag.find(L"..") != std::wstring_view::npos) {
        return false;
    }
    for (const wchar_t character : tag) {
        if (!(IsAsciiLetter(character) || IsAsciiDigit(character) ||
              character == L'-' || character == L'_' ||
              character == L'.' || character == L'+')) {
            return false;
        }
    }
    return true;
}

bool HasCanonicalCdnPath(std::wstring_view pathAndQuery) noexcept {
    const std::size_t queryPosition = pathAndQuery.find(L'?');
    const std::wstring_view path = pathAndQuery.substr(0, queryPosition);
    if (path.size() <= kReleaseAssetCdnPathPrefix.size() ||
        path.substr(0, kReleaseAssetCdnPathPrefix.size()) !=
            kReleaseAssetCdnPathPrefix ||
        path.find(L"//") != std::wstring_view::npos ||
        path.find(L"/./") != std::wstring_view::npos ||
        path.find(L"/../") != std::wstring_view::npos ||
        (path.size() >= 2 && path.substr(path.size() - 2) == L"/.") ||
        (path.size() >= 3 && path.substr(path.size() - 3) == L"/..")) {
        return false;
    }

    for (std::size_t index = 0; index < path.size(); ++index) {
        if (path[index] != L'%') continue;
        if (index + 2 >= path.size() || !IsHexDigit(path[index + 1]) ||
            !IsHexDigit(path[index + 2])) {
            return false;
        }
        const unsigned int decoded =
            HexDigitValue(path[index + 1]) * 16U +
            HexDigitValue(path[index + 2]);
        // Encoded separators and dot segments can be normalized differently
        // by intermediaries, so the updater refuses them outright.
        if (decoded == static_cast<unsigned int>(L'/') ||
            decoded == static_cast<unsigned int>(L'\\') ||
            decoded == static_cast<unsigned int>(L'.') || decoded == 0U) {
            return false;
        }
        index += 2;
    }

    if (queryPosition != std::wstring_view::npos) {
        const std::wstring_view query = pathAndQuery.substr(queryPosition + 1);
        if (query.empty() || query.find(L'?') != std::wstring_view::npos) {
            return false;
        }
        for (std::size_t index = 0; index < query.size(); ++index) {
            if (query[index] != L'%') continue;
            if (index + 2 >= query.size() || !IsHexDigit(query[index + 1]) ||
                !IsHexDigit(query[index + 2])) {
                return false;
            }
            index += 2;
        }
    }
    return true;
}

std::optional<std::wstring> PrintableAsciiToWide(
    std::string_view value) {
    if (value.empty() || value.size() > kMaximumUrlCharacters) {
        return std::nullopt;
    }
    std::wstring converted;
    converted.reserve(value.size());
    for (const unsigned char character : value) {
        if (character < 0x21 || character > 0x7e) return std::nullopt;
        converted.push_back(static_cast<wchar_t>(character));
    }
    return converted;
}

std::optional<AllowedUpdateAssetUrl> ParseInitialUpdateAssetUrlWide(
    std::wstring_view url,
    std::wstring_view expectedFilename) {
    const std::optional<std::wstring_view> assetVersion =
        ExtractWindowsAssetVersion(expectedFilename);
    if (!assetVersion.has_value()) return std::nullopt;
    const std::optional<ParsedAbsoluteHttpsUrl> parsed =
        ParseAbsoluteHttpsUrl(url);
    if (!parsed.has_value() || parsed->host != kInitialHost ||
        parsed->pathAndQuery.find(L'?') != std::wstring::npos) {
        return std::nullopt;
    }

    if (parsed->pathAndQuery.size() <= kInitialPathPrefix.size() ||
        parsed->pathAndQuery.substr(0, kInitialPathPrefix.size()) !=
            kInitialPathPrefix) {
        return std::nullopt;
    }
    const std::wstring_view remainder(parsed->pathAndQuery.data() +
                                           kInitialPathPrefix.size(),
                                       parsed->pathAndQuery.size() -
                                           kInitialPathPrefix.size());
    const std::size_t separator = remainder.find(L'/');
    if (separator == std::wstring_view::npos ||
        remainder.find(L'/', separator + 1) != std::wstring_view::npos) {
        return std::nullopt;
    }
    const std::wstring_view tag = remainder.substr(0, separator);
    const std::wstring_view filename = remainder.substr(separator + 1);
    const bool tagMatchesVersion =
        tag == *assetVersion ||
        (tag.size() == assetVersion->size() + 1 && tag.front() == L'v' &&
         tag.substr(1) == *assetVersion);
    if (!IsSafeReleaseTag(tag) || !tagMatchesVersion ||
        filename != expectedFilename) {
        return std::nullopt;
    }
    return AllowedUpdateAssetUrl{parsed->host, parsed->pathAndQuery};
}

std::optional<AllowedUpdateAssetUrl>
ParseGitHubReleaseAssetRedirectUrlWide(std::wstring_view url) {
    const std::optional<ParsedAbsoluteHttpsUrl> parsed =
        ParseAbsoluteHttpsUrl(url);
    if (!parsed.has_value() || parsed->host != kReleaseAssetCdnHost ||
        !HasCanonicalCdnPath(parsed->pathAndQuery)) {
        return std::nullopt;
    }
    return AllowedUpdateAssetUrl{parsed->host, parsed->pathAndQuery};
}

#ifdef _WIN32

#ifndef CODEX_MONITOR_WINDOWS_VERSION
#define CODEX_MONITOR_WINDOWS_VERSION "0.3.0"
#endif
#define CODEX_MONITOR_ASSET_WIDEN_IMPL(value) L##value
#define CODEX_MONITOR_ASSET_WIDEN(value) \
    CODEX_MONITOR_ASSET_WIDEN_IMPL(value)

constexpr wchar_t kUserAgent[] =
    L"Codex-Monitor-HUD-Windows/" CODEX_MONITOR_ASSET_WIDEN(
        CODEX_MONITOR_WINDOWS_VERSION);
constexpr auto kOverallTimeout = std::chrono::minutes{2};
constexpr DWORD kMaximumStageTimeoutMilliseconds = 10 * 1000;
constexpr std::uint64_t kHardMaximumAssetBytes = 1024ULL * 1024ULL * 1024ULL;

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) noexcept : value_(value) {}
    ~InternetHandle() {
        if (value_ != nullptr) WinHttpCloseHandle(value_);
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    InternetHandle(InternetHandle&& other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }
    InternetHandle& operator=(InternetHandle&& other) noexcept {
        if (this == &other) return *this;
        if (value_ != nullptr) WinHttpCloseHandle(value_);
        value_ = other.value_;
        other.value_ = nullptr;
        return *this;
    }

    HINTERNET get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HINTERNET value_ = nullptr;
};

class KernelHandle {
public:
    KernelHandle() = default;
    explicit KernelHandle(HANDLE value) noexcept : value_(value) {}
    ~KernelHandle() {
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    KernelHandle(const KernelHandle&) = delete;
    KernelHandle& operator=(const KernelHandle&) = delete;

    KernelHandle(KernelHandle&& other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    KernelHandle& operator=(KernelHandle&& other) noexcept {
        if (this == &other) return *this;
        if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = other.value_;
        other.value_ = INVALID_HANDLE_VALUE;
        return *this;
    }

    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class OutputFile {
public:
    OutputFile(HANDLE handle, std::filesystem::path path)
        : handle_(handle), path_(std::move(path)) {}

    ~OutputFile() {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        if (!committed_) {
            FILE_DISPOSITION_INFO disposition{};
            disposition.DeleteFile = TRUE;
            SetFileInformationByHandle(handle_, FileDispositionInfo,
                                       &disposition, sizeof(disposition));
        }
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        if (!committed_) {
            // A second best-effort removal covers volumes that reject
            // FileDispositionInfo but permit deletion after close.
            DeleteFileW(path_.c_str());
        }
    }

    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    HANDLE get() const noexcept { return handle_; }

    bool Commit() noexcept {
        if (!FlushFileBuffers(handle_)) return false;
        committed_ = true;
        return true;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    bool committed_ = false;
};

struct OpenedHttpResponse {
    InternetHandle connection;
    InternetHandle request;
    DWORD statusCode = 0;
};

bool IsCancelled(
    const UpdateAssetDownloadCancellationCheck& cancelled) noexcept {
    try {
        return cancelled && cancelled();
    } catch (...) {
        return true;
    }
}

DWORD RemainingMilliseconds(
    std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) return 0;
    return static_cast<DWORD>(std::clamp<std::int64_t>(
        remaining.count(), 1,
        static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())));
}

bool ApplyRemainingTimeout(
    HINTERNET handle,
    std::chrono::steady_clock::time_point deadline) noexcept {
    const DWORD remaining = RemainingMilliseconds(deadline);
    if (remaining == 0) return false;
    const DWORD stageTimeout =
        std::min(remaining, kMaximumStageTimeoutMilliseconds);
    return WinHttpSetTimeouts(handle, stageTimeout, stageTimeout,
                              stageTimeout, stageTimeout) == TRUE;
}

UpdateAssetDownloadResult Failure(UpdateAssetDownloadFailureKind kind,
                                  const wchar_t* message) {
    UpdateAssetDownloadResult result;
    result.failure = kind;
    result.error = message;
    return result;
}

bool IsRedirectStatus(DWORD statusCode) noexcept {
    return statusCode == 301 || statusCode == 302 || statusCode == 303 ||
           statusCode == 307 || statusCode == 308;
}

bool OpenGetResponse(
    HINTERNET session,
    const AllowedUpdateAssetUrl& url,
    std::chrono::steady_clock::time_point deadline,
    const UpdateAssetDownloadCancellationCheck& cancelled,
    OpenedHttpResponse* response,
    UpdateAssetDownloadFailureKind* failureKind,
    const wchar_t** failureMessage) noexcept {
    if (IsCancelled(cancelled)) {
        *failureKind = UpdateAssetDownloadFailureKind::kCancelled;
        *failureMessage = L"Update download was cancelled";
        return false;
    }

    InternetHandle connection{WinHttpConnect(
        session, url.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection) {
        *failureKind = UpdateAssetDownloadFailureKind::kNetwork;
        *failureMessage = L"Unable to connect to the update service";
        return false;
    }

    InternetHandle request{WinHttpOpenRequest(
        connection.get(), L"GET", url.pathAndQuery.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE)};
    if (!request || !ApplyRemainingTimeout(request.get(), deadline)) {
        *failureKind = UpdateAssetDownloadFailureKind::kNetwork;
        *failureMessage = L"Unable to create the update download request";
        return false;
    }

    DWORD disabledFeatures = WINHTTP_DISABLE_COOKIES |
                             WINHTTP_DISABLE_REDIRECTS |
                             WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                          &disabledFeatures, sizeof(disabledFeatures))) {
        *failureKind = UpdateAssetDownloadFailureKind::kNetwork;
        *failureMessage = L"Unable to apply the update privacy policy";
        return false;
    }
    DWORD autoLogonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY,
                          &autoLogonPolicy, sizeof(autoLogonPolicy))) {
        *failureKind = UpdateAssetDownloadFailureKind::kNetwork;
        *failureMessage = L"Unable to disable automatic update credentials";
        return false;
    }

    constexpr wchar_t kHeaders[] =
        L"Accept: application/octet-stream\r\n"
        L"Accept-Encoding: identity\r\n";
    constexpr DWORD kHeaderLength =
        static_cast<DWORD>(_countof(kHeaders) - 1);
    if (!WinHttpSendRequest(request.get(), kHeaders, kHeaderLength,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        const bool wasCancelled = IsCancelled(cancelled);
        *failureKind = wasCancelled
                           ? UpdateAssetDownloadFailureKind::kCancelled
                           : UpdateAssetDownloadFailureKind::kNetwork;
        *failureMessage = wasCancelled
                              ? L"Update download was cancelled"
                              : L"Unable to send the update download request";
        return false;
    }
    if (IsCancelled(cancelled)) {
        *failureKind = UpdateAssetDownloadFailureKind::kCancelled;
        *failureMessage = L"Update download was cancelled";
        return false;
    }
    if (!ApplyRemainingTimeout(request.get(), deadline) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        const bool wasCancelled = IsCancelled(cancelled);
        *failureKind = wasCancelled
                           ? UpdateAssetDownloadFailureKind::kCancelled
                           : UpdateAssetDownloadFailureKind::kNetwork;
        *failureMessage = wasCancelled
                              ? L"Update download was cancelled"
                              : L"Update download service did not respond";
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeBytes = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        *failureKind = UpdateAssetDownloadFailureKind::kHttp;
        *failureMessage = L"Update service returned an unreadable HTTP status";
        return false;
    }

    response->connection = std::move(connection);
    response->request = std::move(request);
    response->statusCode = statusCode;
    return true;
}

std::optional<std::wstring> ReadLocationHeader(HINTERNET request) {
    DWORD requiredBytes = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                            WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                            &requiredBytes, WINHTTP_NO_HEADER_INDEX) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        requiredBytes < sizeof(wchar_t) ||
        requiredBytes % sizeof(wchar_t) != 0 ||
        requiredBytes > kMaximumUrlCharacters * sizeof(wchar_t)) {
        return std::nullopt;
    }

    std::wstring value(requiredBytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                             WINHTTP_HEADER_NAME_BY_INDEX, value.data(),
                             &requiredBytes, WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (value.empty()) return std::nullopt;
    return value;
}

bool TryReadContentLength(HINTERNET request,
                          std::uint64_t* length) noexcept {
    wchar_t value[32]{};
    DWORD valueBytes = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX, value,
                             &valueBytes, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }

    std::uint64_t parsed = 0;
    std::size_t position = 0;
    while (position < _countof(value) && value[position] != L'\0') {
        const wchar_t character = value[position++];
        if (!IsAsciiDigit(character)) return false;
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - L'0');
        if (parsed >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10ULL) {
            return false;
        }
        parsed = parsed * 10ULL + digit;
    }
    if (position == 0 || position == _countof(value)) return false;
    *length = parsed;
    return true;
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

bool IsReservedDosDeviceSegment(std::wstring_view segment) noexcept {
    const std::size_t dot = segment.find(L'.');
    std::wstring_view base = segment.substr(0, dot);
    if (base.empty() || base.size() > 4) return false;

    std::array<wchar_t, 4> upper{};
    for (std::size_t index = 0; index < base.size(); ++index) {
        wchar_t character = base[index];
        if (character >= L'a' && character <= L'z') {
            character = static_cast<wchar_t>(
                character - (L'a' - L'A'));
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

bool IsCanonicalDirectorySegment(std::wstring_view segment) noexcept {
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

std::optional<std::wstring> CanonicalAbsoluteDirectoryPath(
    const std::filesystem::path& directory) {
    const std::wstring input = directory.native();
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
        if (!IsCanonicalDirectorySegment(normalized.substr(
                segmentBegin, segmentEnd - segmentBegin))) {
            return std::nullopt;
        }
        if (separator == std::wstring::npos) break;
        segmentBegin = separator + 1;
    }
    return normalized;
}

std::wstring ExtendedDosPath(std::wstring_view dosPath) {
    std::wstring result = L"\\\\?\\";
    result.append(dosPath);
    return result;
}

std::optional<std::wstring> FinalPathFromHandle(HANDLE handle) {
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, kFlags);
    if (required == 0 || required >= 32768) return std::nullopt;

    std::wstring path(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, path.data(), static_cast<DWORD>(path.size()), kFlags);
    if (written == 0 || written >= path.size()) return std::nullopt;
    path.resize(written);
    constexpr std::wstring_view kExtendedDrivePrefix = L"\\\\?\\";
    if (path.size() <= kExtendedDrivePrefix.size() + 3 ||
        path.substr(0, kExtendedDrivePrefix.size()) != kExtendedDrivePrefix ||
        !IsAsciiLetter(path[kExtendedDrivePrefix.size()]) ||
        path[kExtendedDrivePrefix.size() + 1] != L':' ||
        path[kExtendedDrivePrefix.size() + 2] != L'\\' ||
        path.back() == L'\\') {
        return std::nullopt;
    }
    return path;
}

KernelHandle OpenDirectoryWithoutFollowingReparsePoints(
    const std::wstring& path) noexcept {
    return KernelHandle{CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
}

struct LockedDirectoryChain {
    std::vector<KernelHandle> handles;
    std::wstring finalPath;
    FileIdentity finalIdentity;
};

std::optional<LockedDirectoryChain> LockDirectoryChain(
    const std::filesystem::path& directory) {
    const std::optional<std::wstring> canonical =
        CanonicalAbsoluteDirectoryPath(directory);
    if (!canonical.has_value()) return std::nullopt;

    LockedDirectoryChain chain;
    std::vector<std::wstring> lexicalPaths;
    lexicalPaths.push_back(ExtendedDosPath(canonical->substr(0, 3)));
    std::size_t end = canonical->find(L'\\', 3);
    while (end != std::wstring::npos) {
        lexicalPaths.push_back(ExtendedDosPath(canonical->substr(0, end)));
        end = canonical->find(L'\\', end + 1);
    }
    lexicalPaths.push_back(ExtendedDosPath(*canonical));
    chain.handles.reserve(lexicalPaths.size() + 1);

    for (const std::wstring& path : lexicalPaths) {
        KernelHandle handle = OpenDirectoryWithoutFollowingReparsePoints(path);
        if (!handle || !IsNonReparseDirectory(handle.get())) {
            return std::nullopt;
        }
        chain.handles.push_back(std::move(handle));
    }

    HANDLE lockedFinal = chain.handles.back().get();
    const std::optional<std::wstring> finalPath =
        FinalPathFromHandle(lockedFinal);
    if (!finalPath.has_value() ||
        !ReadFileIdentity(lockedFinal, &chain.finalIdentity)) {
        return std::nullopt;
    }

    KernelHandle reopened =
        OpenDirectoryWithoutFollowingReparsePoints(*finalPath);
    FileIdentity reopenedIdentity{};
    if (!reopened || !IsNonReparseDirectory(reopened.get()) ||
        !ReadFileIdentity(reopened.get(), &reopenedIdentity) ||
        !SameFileIdentity(chain.finalIdentity, reopenedIdentity)) {
        return std::nullopt;
    }
    chain.handles.push_back(std::move(reopened));
    chain.finalPath = *finalPath;
    return std::optional<LockedDirectoryChain>{std::move(chain)};
}

bool VerifyCreatedOutputIdentity(
    HANDLE output,
    const LockedDirectoryChain& directory,
    KernelHandle* verifiedParent,
    KernelHandle* verifiedFile) {
    if (!IsNonReparseFile(output)) return false;
    FileIdentity originalFileIdentity{};
    if (!ReadFileIdentity(output, &originalFileIdentity)) return false;

    const std::optional<std::wstring> finalFilePath =
        FinalPathFromHandle(output);
    if (!finalFilePath.has_value()) return false;
    const std::size_t separator = finalFilePath->find_last_of(L'\\');
    if (separator == std::wstring::npos) return false;
    const std::wstring parentPath = finalFilePath->substr(0, separator);
    if (!SameWindowsPath(parentPath, directory.finalPath)) return false;

    KernelHandle parent =
        OpenDirectoryWithoutFollowingReparsePoints(parentPath);
    FileIdentity parentIdentity{};
    if (!parent || !IsNonReparseDirectory(parent.get()) ||
        !ReadFileIdentity(parent.get(), &parentIdentity) ||
        !SameFileIdentity(parentIdentity, directory.finalIdentity)) {
        return false;
    }

    KernelHandle file{CreateFileW(
        finalFilePath->c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    FileIdentity reopenedFileIdentity{};
    if (!file || !IsNonReparseFile(file.get()) ||
        !ReadFileIdentity(file.get(), &reopenedFileIdentity) ||
        !SameFileIdentity(originalFileIdentity, reopenedFileIdentity)) {
        return false;
    }

    *verifiedParent = std::move(parent);
    *verifiedFile = std::move(file);
    return true;
}

#endif  // _WIN32

}  // namespace

std::optional<AllowedUpdateAssetUrl> ParseInitialUpdateAssetUrl(
    std::string_view url,
    std::string_view expectedFilename) noexcept {
    try {
        const std::optional<std::wstring> wideUrl = PrintableAsciiToWide(url);
        const std::optional<std::wstring> wideFilename =
            PrintableAsciiToWide(expectedFilename);
        if (!wideUrl.has_value() || !wideFilename.has_value()) {
            return std::nullopt;
        }
        return ParseInitialUpdateAssetUrlWide(*wideUrl, *wideFilename);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<AllowedUpdateAssetUrl> ParseGitHubReleaseAssetRedirectUrl(
    std::string_view url) noexcept {
    try {
        const std::optional<std::wstring> wideUrl = PrintableAsciiToWide(url);
        if (!wideUrl.has_value()) return std::nullopt;
        return ParseGitHubReleaseAssetRedirectUrlWide(*wideUrl);
    } catch (...) {
        return std::nullopt;
    }
}

UpdateAssetDownloadResult DownloadWindowsUpdateAsset(
    std::string_view browserDownloadUrl,
    std::string_view expectedFilename,
    std::uint64_t maximumBytes,
    const std::filesystem::path& privateDirectory,
    const UpdateAssetDownloadCancellationCheck& cancelled) noexcept {
#ifdef _WIN32
    try {
        const std::optional<AllowedUpdateAssetUrl> initialUrl =
            ParseInitialUpdateAssetUrl(browserDownloadUrl, expectedFilename);
        if (!initialUrl.has_value() || maximumBytes == 0 ||
            maximumBytes > kHardMaximumAssetBytes ||
            privateDirectory.empty() || !privateDirectory.is_absolute()) {
            return Failure(UpdateAssetDownloadFailureKind::kInvalidInput,
                           L"Invalid update download input");
        }
        if (IsCancelled(cancelled)) {
            return Failure(UpdateAssetDownloadFailureKind::kCancelled,
                           L"Update download was cancelled");
        }

        std::optional<LockedDirectoryChain> lockedDirectory =
            LockDirectoryChain(privateDirectory);
        if (!lockedDirectory.has_value()) {
            return Failure(UpdateAssetDownloadFailureKind::kFileSystem,
                           L"Update directory is unavailable or unsafe");
        }

        const auto deadline = std::chrono::steady_clock::now() + kOverallTimeout;
        InternetHandle session{WinHttpOpen(
            kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session || !ApplyRemainingTimeout(session.get(), deadline)) {
            return Failure(UpdateAssetDownloadFailureKind::kNetwork,
                           L"Unable to initialize the update download");
        }

        UpdateAssetDownloadFailureKind openFailure =
            UpdateAssetDownloadFailureKind::kUnexpected;
        const wchar_t* openFailureMessage =
            L"Unable to open the update download";
        OpenedHttpResponse response;
        if (!OpenGetResponse(session.get(), *initialUrl, deadline, cancelled,
                             &response, &openFailure,
                             &openFailureMessage)) {
            return Failure(openFailure, openFailureMessage);
        }

        if (IsRedirectStatus(response.statusCode)) {
            const std::optional<std::wstring> location =
                ReadLocationHeader(response.request.get());
            const std::optional<AllowedUpdateAssetUrl> redirectedUrl =
                location.has_value()
                    ? ParseGitHubReleaseAssetRedirectUrlWide(*location)
                    : std::nullopt;
            if (!redirectedUrl.has_value()) {
                return Failure(
                    UpdateAssetDownloadFailureKind::kRedirectRejected,
                    L"Update download redirect was rejected");
            }

            OpenedHttpResponse redirectedResponse;
            if (!OpenGetResponse(session.get(), *redirectedUrl, deadline,
                                 cancelled, &redirectedResponse,
                                 &openFailure, &openFailureMessage)) {
                return Failure(openFailure, openFailureMessage);
            }
            response = std::move(redirectedResponse);
        }

        if (response.statusCode != HTTP_STATUS_OK) {
            return Failure(IsRedirectStatus(response.statusCode)
                               ? UpdateAssetDownloadFailureKind::kRedirectRejected
                               : UpdateAssetDownloadFailureKind::kHttp,
                           IsRedirectStatus(response.statusCode)
                               ? L"Update download redirected more than once"
                               : L"Update service returned an HTTP error");
        }

        std::uint64_t contentLength = 0;
        const bool hasContentLength =
            TryReadContentLength(response.request.get(), &contentLength);
        if (hasContentLength &&
            contentLength > maximumBytes) {
            return Failure(UpdateAssetDownloadFailureKind::kResponseTooLarge,
                           L"Update asset exceeded its size limit");
        }

        const std::filesystem::path outputPath =
            std::filesystem::path(lockedDirectory->finalPath) /
            std::string(expectedFilename);
        HANDLE rawFile = CreateFileW(
            outputPath.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (rawFile == INVALID_HANDLE_VALUE) {
            return Failure(UpdateAssetDownloadFailureKind::kFileSystem,
                           L"Unable to create the update asset file");
        }
        OutputFile output(rawFile, outputPath);
        KernelHandle verifiedParent;
        KernelHandle verifiedFile;
        if (!VerifyCreatedOutputIdentity(
                output.get(), *lockedDirectory, &verifiedParent,
                &verifiedFile)) {
            return Failure(UpdateAssetDownloadFailureKind::kFileSystem,
                           L"Update asset path or file identity is unsafe");
        }

        std::uint64_t totalBytes = 0;
        std::array<unsigned char, 64 * 1024> buffer{};
        for (;;) {
            if (IsCancelled(cancelled)) {
                return Failure(UpdateAssetDownloadFailureKind::kCancelled,
                               L"Update download was cancelled");
            }
            if (!ApplyRemainingTimeout(response.request.get(), deadline)) {
                return Failure(UpdateAssetDownloadFailureKind::kNetwork,
                               L"Update download timed out");
            }

            DWORD bytesRead = 0;
            if (!WinHttpReadData(response.request.get(), buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytesRead)) {
                const bool wasCancelled = IsCancelled(cancelled);
                return Failure(
                    wasCancelled
                        ? UpdateAssetDownloadFailureKind::kCancelled
                        : UpdateAssetDownloadFailureKind::kNetwork,
                    wasCancelled ? L"Update download was cancelled"
                                 : L"Unable to read the update asset");
            }
            if (bytesRead == 0) break;
            if (bytesRead > maximumBytes ||
                totalBytes > maximumBytes - bytesRead) {
                return Failure(
                    UpdateAssetDownloadFailureKind::kResponseTooLarge,
                    L"Update asset exceeded its size limit");
            }

            DWORD offset = 0;
            while (offset < bytesRead) {
                DWORD bytesWritten = 0;
                if (!WriteFile(output.get(), buffer.data() + offset,
                               bytesRead - offset, &bytesWritten, nullptr) ||
                    bytesWritten == 0) {
                    return Failure(UpdateAssetDownloadFailureKind::kFileSystem,
                                   L"Unable to write the update asset");
                }
                offset += bytesWritten;
            }
            totalBytes += bytesRead;
        }

        if (IsCancelled(cancelled)) {
            return Failure(UpdateAssetDownloadFailureKind::kCancelled,
                           L"Update download was cancelled");
        }
        if (totalBytes == 0) {
            return Failure(UpdateAssetDownloadFailureKind::kHttp,
                           L"Update service returned an empty asset");
        }
        if (hasContentLength && totalBytes != contentLength) {
            return Failure(UpdateAssetDownloadFailureKind::kHttp,
                           L"Update asset was incomplete");
        }
        UpdateAssetDownloadResult result;
        result.succeeded = true;
        result.filePath = outputPath;
        result.bytesWritten = totalBytes;
        result.failure = UpdateAssetDownloadFailureKind::kNone;
        if (!output.Commit()) {
            return Failure(UpdateAssetDownloadFailureKind::kFileSystem,
                           L"Unable to finalize the update asset");
        }
        return result;
    } catch (...) {
        return Failure(UpdateAssetDownloadFailureKind::kUnexpected,
                       L"Update download failed unexpectedly");
    }
#else
    (void)browserDownloadUrl;
    (void)expectedFilename;
    (void)maximumBytes;
    (void)privateDirectory;
    (void)cancelled;
    UpdateAssetDownloadResult result;
    result.failure = UpdateAssetDownloadFailureKind::kUnexpected;
    result.error = L"Update asset downloads are available only on Windows";
    return result;
#endif
}

}  // namespace codex_monitor::update
