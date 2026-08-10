#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "service_status_fetch_win32.h"

#include "service_status_json_win32.h"

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

namespace codex_monitor {
namespace {

constexpr wchar_t kStatusHost[] = L"status.openai.com";
constexpr wchar_t kStatusPath[] = L"/api/v2/summary.json";
constexpr wchar_t kUserAgent[] = L"CodexMonitorHUD-Windows/0.3";
constexpr std::size_t kMaximumResponseBytes = 1024 * 1024;
constexpr auto kOverallTimeout = std::chrono::seconds{8};
constexpr DWORD kMaximumStageTimeoutMilliseconds = 2000;

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : value_(value) {}
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

bool IsCancelled(const ServiceStatusCancellationCheck& cancelled) noexcept {
    try {
        return cancelled && cancelled();
    } catch (...) {
        return true;
    }
}

OpenAIServiceStatusFetchResult Failure(
    OpenAIServiceStatusFailureKind kind,
    const wchar_t* message) {
    OpenAIServiceStatusFetchResult result;
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

std::wstring Utf8ToWide(const std::string& bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int byteCount = static_cast<int>(bytes.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), byteCount, nullptr, 0);
    if (required <= 0) return {};
    std::wstring text(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                            byteCount, text.data(), required) != required) {
        return {};
    }
    return text;
}

}  // namespace

OpenAIServiceStatusFetchResult FetchOpenAIServiceStatus(
    const ServiceStatusCancellationCheck& cancelled) noexcept {
    try {
        if (IsCancelled(cancelled)) {
            return Failure(OpenAIServiceStatusFailureKind::kCancelled,
                           L"Service status refresh was cancelled");
        }
        const auto deadline = std::chrono::steady_clock::now() + kOverallTimeout;

        InternetHandle session{WinHttpOpen(
            kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session || !ApplyRemainingTimeout(session.get(), deadline)) {
            return Failure(OpenAIServiceStatusFailureKind::kNetwork,
                           L"Unable to initialize the service status request");
        }

        InternetHandle connection{WinHttpConnect(
            session.get(), kStatusHost, INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connection || IsCancelled(cancelled) ||
            RemainingMilliseconds(deadline) == 0) {
            return Failure(IsCancelled(cancelled)
                               ? OpenAIServiceStatusFailureKind::kCancelled
                               : OpenAIServiceStatusFailureKind::kNetwork,
                           IsCancelled(cancelled)
                               ? L"Service status refresh was cancelled"
                               : L"Unable to connect to OpenAI service status");
        }

        InternetHandle request{WinHttpOpenRequest(
            connection.get(), L"GET", kStatusPath, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE)};
        if (!request) {
            return Failure(OpenAIServiceStatusFailureKind::kNetwork,
                           L"Unable to create the service status request");
        }
        if (!ApplyRemainingTimeout(request.get(), deadline)) {
            return Failure(OpenAIServiceStatusFailureKind::kNetwork,
                           L"Unable to apply the service status timeout");
        }

        DWORD disabledFeatures = WINHTTP_DISABLE_COOKIES |
                                 WINHTTP_DISABLE_REDIRECTS |
                                 WINHTTP_DISABLE_AUTHENTICATION;
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                              &disabledFeatures,
                              sizeof(disabledFeatures))) {
            return Failure(OpenAIServiceStatusFailureKind::kNetwork,
                           L"Unable to apply the service status privacy policy");
        }
        DWORD autoLogonPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY,
                         &autoLogonPolicy, sizeof(autoLogonPolicy));
        DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP |
                              WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
        WinHttpSetOption(request.get(), WINHTTP_OPTION_DECOMPRESSION,
                         &decompression, sizeof(decompression));

        constexpr wchar_t kHeaders[] = L"Accept: application/json\r\n";
        constexpr DWORD kHeaderLength =
            static_cast<DWORD>(_countof(kHeaders) - 1);
        if (!WinHttpSendRequest(request.get(), kHeaders, kHeaderLength,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            IsCancelled(cancelled) ||
            !WinHttpReceiveResponse(request.get(), nullptr)) {
            return Failure(IsCancelled(cancelled)
                               ? OpenAIServiceStatusFailureKind::kCancelled
                               : OpenAIServiceStatusFailureKind::kNetwork,
                           IsCancelled(cancelled)
                               ? L"Service status refresh was cancelled"
                               : L"OpenAI service status did not respond");
        }

        DWORD statusCode = 0;
        DWORD statusCodeBytes = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                request.get(), WINHTTP_QUERY_STATUS_CODE |
                                   WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeBytes,
                WINHTTP_NO_HEADER_INDEX) ||
            statusCode != HTTP_STATUS_OK) {
            return Failure(OpenAIServiceStatusFailureKind::kHttp,
                           L"OpenAI service status returned an HTTP error");
        }

        std::string response;
        response.reserve(32 * 1024);
        std::array<char, 16 * 1024> buffer{};
        for (;;) {
            if (IsCancelled(cancelled)) {
                return Failure(OpenAIServiceStatusFailureKind::kCancelled,
                               L"Service status refresh was cancelled");
            }
            const DWORD remaining = RemainingMilliseconds(deadline);
            if (remaining == 0 ||
                !ApplyRemainingTimeout(request.get(), deadline)) {
                return Failure(OpenAIServiceStatusFailureKind::kNetwork,
                               L"OpenAI service status request timed out");
            }
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.get(), buffer.data(),
                                 static_cast<DWORD>(buffer.size()),
                                 &bytesRead)) {
                return Failure(OpenAIServiceStatusFailureKind::kNetwork,
                               L"Unable to read OpenAI service status");
            }
            if (bytesRead == 0) break;
            if (response.size() > kMaximumResponseBytes - bytesRead) {
                return Failure(OpenAIServiceStatusFailureKind::kResponseTooLarge,
                               L"OpenAI service status response was too large");
            }
            response.append(buffer.data(), bytesRead);
        }

        const std::wstring json = Utf8ToWide(response);
        if (json.empty()) {
            return Failure(OpenAIServiceStatusFailureKind::kInvalidResponse,
                           L"OpenAI service status returned invalid UTF-8");
        }
        const auto parsed = ParseOpenAIServiceStatusSummaryJson(json);
        if (!parsed) {
            return Failure(OpenAIServiceStatusFailureKind::kInvalidResponse,
                           L"OpenAI service status returned invalid JSON");
        }

        OpenAIServiceStatusFetchResult result;
        result.succeeded = true;
        result.failure = OpenAIServiceStatusFailureKind::kNone;
        result.status = MapOpenAIServiceStatus(
            parsed->overallIndicator,
            parsed->codexComponentStatus
                ? std::optional<std::string_view>{*parsed->codexComponentStatus}
                : std::nullopt);
        return result;
    } catch (...) {
        return Failure(OpenAIServiceStatusFailureKind::kInvalidResponse,
                       L"OpenAI service status refresh failed unexpectedly");
    }
}

}  // namespace codex_monitor
