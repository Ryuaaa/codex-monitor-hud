#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "update_fetch_win32.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace codex_monitor::update {
namespace {

#ifndef CODEX_MONITOR_WINDOWS_VERSION
#define CODEX_MONITOR_WINDOWS_VERSION "0.3.0"
#endif
#define CODEX_MONITOR_WIDEN_IMPL(value) L##value
#define CODEX_MONITOR_WIDEN(value) CODEX_MONITOR_WIDEN_IMPL(value)

constexpr wchar_t kGitHubApiHost[] = L"api.github.com";
constexpr wchar_t kGitHubReleasesPath[] =
    L"/repos/Ryuaaa/codex-monitor-hud/releases?per_page=20";
constexpr wchar_t kUserAgent[] =
    L"Codex-Monitor-HUD-Windows/" CODEX_MONITOR_WIDEN(
        CODEX_MONITOR_WINDOWS_VERSION);
constexpr std::size_t kMaximumResponseBytes = 2 * 1024 * 1024;
constexpr auto kOverallTimeout = std::chrono::seconds{10};
constexpr DWORD kMaximumStageTimeoutMilliseconds = 2000;

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) noexcept : value_(value) {}
    ~InternetHandle() {
        if (value_) WinHttpCloseHandle(value_);
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    HINTERNET get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HINTERNET value_ = nullptr;
};

bool IsCancelled(const WindowsUpdateCancellationCheck& cancelled) noexcept {
    try {
        return cancelled && cancelled();
    } catch (...) {
        return true;
    }
}

WindowsUpdateFetchResult Failure(WindowsUpdateFetchFailureKind kind,
                                 const wchar_t* message) {
    WindowsUpdateFetchResult result;
    result.failure = kind;
    result.error = message;
    return result;
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

bool ApplyRemainingTimeout(HINTERNET handle,
                           std::chrono::steady_clock::time_point deadline) {
    const DWORD remaining = RemainingMilliseconds(deadline);
    if (remaining == 0) return false;
    const DWORD timeout =
        std::min(remaining, kMaximumStageTimeoutMilliseconds);
    return WinHttpSetTimeouts(handle, timeout, timeout, timeout, timeout) == TRUE;
}

bool TryReadContentLength(HINTERNET request, std::uint64_t* length) noexcept {
    wchar_t value[32]{};
    DWORD valueBytes = sizeof(value);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX, value, &valueBytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }

    std::uint64_t parsed = 0;
    std::size_t position = 0;
    for (; position < _countof(value) && value[position] != L'\0';
         ++position) {
        const wchar_t character = value[position];
        if (character < L'0' || character > L'9') return false;
        const std::uint64_t digit =
            static_cast<std::uint64_t>(character - L'0');
        if (parsed >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    if (position == 0 || position == _countof(value)) return false;
    *length = parsed;
    return true;
}

bool StrictUtf8ToWide(const std::string& bytes, std::wstring* text) {
    if (bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (bytes.empty()) {
        text->clear();
        return true;
    }

    const int byteCount = static_cast<int>(bytes.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), byteCount, nullptr, 0);
    if (required <= 0) return false;

    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                            byteCount, converted.data(), required) != required) {
        return false;
    }
    *text = std::move(converted);
    return true;
}

}  // namespace

WindowsUpdateFetchResult FetchWindowsUpdateReleasesJson(
    const WindowsUpdateCancellationCheck& cancelled) noexcept {
    try {
        if (IsCancelled(cancelled)) {
            return Failure(WindowsUpdateFetchFailureKind::kCancelled,
                           L"Update check was cancelled");
        }
        const auto deadline = std::chrono::steady_clock::now() + kOverallTimeout;

        InternetHandle session{WinHttpOpen(
            kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session || !ApplyRemainingTimeout(session.get(), deadline)) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Unable to initialize the update request");
        }

        InternetHandle connection{WinHttpConnect(
            session.get(), kGitHubApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connection) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Unable to connect to the update service");
        }
        if (IsCancelled(cancelled)) {
            return Failure(WindowsUpdateFetchFailureKind::kCancelled,
                           L"Update check was cancelled");
        }
        if (RemainingMilliseconds(deadline) == 0) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Update request timed out");
        }

        InternetHandle request{WinHttpOpenRequest(
            connection.get(), L"GET", kGitHubReleasesPath, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE)};
        if (!request || !ApplyRemainingTimeout(request.get(), deadline)) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Unable to create the update request");
        }

        DWORD disabledFeatures = WINHTTP_DISABLE_COOKIES |
                                 WINHTTP_DISABLE_REDIRECTS |
                                 WINHTTP_DISABLE_AUTHENTICATION;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                              &disabledFeatures, sizeof(disabledFeatures))) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Unable to apply the update request privacy policy");
        }
        DWORD autoLogonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY,
                              &autoLogonPolicy, sizeof(autoLogonPolicy))) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Unable to disable automatic update credentials");
        }
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP |
                              WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION,
                              &decompression, sizeof(decompression))) {
            return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                           L"Unable to configure the update response");
        }

        constexpr wchar_t kHeaders[] =
            L"Accept: application/vnd.github+json\r\n"
            L"X-GitHub-Api-Version: 2022-11-28\r\n";
        constexpr DWORD kHeaderLength =
            static_cast<DWORD>(_countof(kHeaders) - 1);

        if (!WinHttpSendRequest(request.get(), kHeaders, kHeaderLength,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            const bool wasCancelled = IsCancelled(cancelled);
            return Failure(wasCancelled
                               ? WindowsUpdateFetchFailureKind::kCancelled
                               : WindowsUpdateFetchFailureKind::kNetwork,
                           wasCancelled ? L"Update check was cancelled"
                                        : L"Unable to send the update request");
        }
        if (IsCancelled(cancelled)) {
            return Failure(WindowsUpdateFetchFailureKind::kCancelled,
                           L"Update check was cancelled");
        }
        if (!ApplyRemainingTimeout(request.get(), deadline) ||
            !WinHttpReceiveResponse(request.get(), nullptr)) {
            const bool wasCancelled = IsCancelled(cancelled);
            return Failure(wasCancelled
                               ? WindowsUpdateFetchFailureKind::kCancelled
                               : WindowsUpdateFetchFailureKind::kNetwork,
                           wasCancelled ? L"Update check was cancelled"
                                        : L"Update service did not respond");
        }

        DWORD statusCode = 0;
        DWORD statusCodeBytes = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                request.get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            return Failure(WindowsUpdateFetchFailureKind::kHttp,
                           L"Update service returned an unreadable HTTP status");
        }
        if (statusCode != HTTP_STATUS_OK) {
            return Failure(WindowsUpdateFetchFailureKind::kHttp,
                           L"Update service returned an HTTP error");
        }

        std::uint64_t contentLength = 0;
        if (TryReadContentLength(request.get(), &contentLength) &&
            contentLength > kMaximumResponseBytes) {
            return Failure(WindowsUpdateFetchFailureKind::kResponseTooLarge,
                           L"Update response was too large");
        }

        std::string response;
        response.reserve(64 * 1024);
        std::array<char, 16 * 1024> buffer{};
        for (;;) {
            if (IsCancelled(cancelled)) {
                return Failure(WindowsUpdateFetchFailureKind::kCancelled,
                               L"Update check was cancelled");
            }
            if (!ApplyRemainingTimeout(request.get(), deadline)) {
                return Failure(WindowsUpdateFetchFailureKind::kNetwork,
                               L"Update request timed out");
            }

            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.get(), buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytesRead)) {
                const bool wasCancelled = IsCancelled(cancelled);
                return Failure(wasCancelled
                                   ? WindowsUpdateFetchFailureKind::kCancelled
                                   : WindowsUpdateFetchFailureKind::kNetwork,
                               wasCancelled
                                   ? L"Update check was cancelled"
                                   : L"Unable to read the update response");
            }
            if (bytesRead == 0) break;
            if (response.size() > kMaximumResponseBytes - bytesRead) {
                return Failure(WindowsUpdateFetchFailureKind::kResponseTooLarge,
                               L"Update response was too large");
            }
            response.append(buffer.data(), bytesRead);
        }

        if (IsCancelled(cancelled)) {
            return Failure(WindowsUpdateFetchFailureKind::kCancelled,
                           L"Update check was cancelled");
        }

        if (response.empty()) {
            return Failure(WindowsUpdateFetchFailureKind::kInvalidResponse,
                           L"Update service returned an empty response");
        }

        std::wstring json;
        if (!StrictUtf8ToWide(response, &json)) {
            return Failure(WindowsUpdateFetchFailureKind::kInvalidUtf8,
                           L"Update service returned invalid UTF-8");
        }

        WindowsUpdateFetchResult result;
        result.succeeded = true;
        result.json = std::move(json);
        result.failure = WindowsUpdateFetchFailureKind::kNone;
        return result;
    } catch (...) {
        return Failure(WindowsUpdateFetchFailureKind::kUnexpected,
                       L"Update check failed unexpectedly");
    }
}

}  // namespace codex_monitor::update
