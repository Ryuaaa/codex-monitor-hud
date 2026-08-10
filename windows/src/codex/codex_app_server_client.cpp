#include "codex_app_server_client.h"

#include "codex_json_win32.h"
#include "codex_process.h"

#include <winrt/base.h>

// Some Windows SDK include orders define GetObject as GetObjectW. Keep that
// macro away from the C++/WinRT IJsonValue projection.
#ifdef GetObject
#undef GetObject
#endif

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace codex_monitor::codex {
namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

constexpr std::int32_t kInitializeRequestId = 1;
constexpr std::int32_t kRateLimitsRequestId = 2;
constexpr std::int32_t kAccountRequestId = 3;
constexpr std::int32_t kUsageRequestId = 4;
constexpr std::int32_t kThreadListRequestId = 5;
constexpr std::chrono::milliseconds kCancellationPollInterval{200};

enum class EnvelopeKind {
    kResponse,
    kNotification,
    kUnknownStringId,
    kMalformed,
};

struct ResponseEnvelope {
    EnvelopeKind kind = EnvelopeKind::kMalformed;
    std::int32_t id = 0;
    bool isError = false;
    std::string resultJson;
};

std::optional<IJsonValue> FindValue(const JsonObject& object, const wchar_t* key) {
    const winrt::hstring name(key);
    if (!object.HasKey(name)) return std::nullopt;
    return object.Lookup(name);
}

std::string Stringify(const JsonObject& object) {
    return winrt::to_string(object.Stringify());
}

std::optional<std::string> BuildInitializeRequest(std::string_view version) {
    if (version.empty() || version.size() > 128) return std::nullopt;
    try {
        JsonObject clientInfo;
        clientInfo.Insert(L"name", JsonValue::CreateStringValue(L"codex-monitor-hud"));
        clientInfo.Insert(L"title", JsonValue::CreateStringValue(L"Codex Monitor HUD"));
        clientInfo.Insert(L"version",
                          JsonValue::CreateStringValue(winrt::to_hstring(version)));
        JsonObject params;
        params.Insert(L"clientInfo", clientInfo);
        JsonObject request;
        request.Insert(L"id", JsonValue::CreateNumberValue(kInitializeRequestId));
        request.Insert(L"method", JsonValue::CreateStringValue(L"initialize"));
        request.Insert(L"params", params);
        return Stringify(request);
    } catch (const winrt::hresult_error&) {
        return std::nullopt;
    }
}

std::string BuildInitializedNotification() {
    JsonObject notification;
    notification.Insert(L"method", JsonValue::CreateStringValue(L"initialized"));
    notification.Insert(L"params", JsonObject{});
    return Stringify(notification);
}

std::string BuildNullParamsRequest(std::int32_t id, const wchar_t* method) {
    JsonObject request;
    request.Insert(L"id", JsonValue::CreateNumberValue(id));
    request.Insert(L"method", JsonValue::CreateStringValue(method));
    request.Insert(L"params", JsonValue::CreateNullValue());
    return Stringify(request);
}

std::string BuildAccountRequest() {
    JsonObject params;
    params.Insert(L"refreshToken", JsonValue::CreateBooleanValue(false));
    JsonObject request;
    request.Insert(L"id", JsonValue::CreateNumberValue(kAccountRequestId));
    request.Insert(L"method", JsonValue::CreateStringValue(L"account/read"));
    request.Insert(L"params", params);
    return Stringify(request);
}

std::string BuildThreadListRequest() {
    JsonObject params;
    params.Insert(L"limit", JsonValue::CreateNumberValue(5));
    params.Insert(L"sortKey", JsonValue::CreateStringValue(L"recency_at"));
    params.Insert(L"sortDirection", JsonValue::CreateStringValue(L"desc"));
    params.Insert(L"useStateDbOnly", JsonValue::CreateBooleanValue(true));
    JsonObject request;
    request.Insert(L"id", JsonValue::CreateNumberValue(kThreadListRequestId));
    request.Insert(L"method", JsonValue::CreateStringValue(L"thread/list"));
    request.Insert(L"params", params);
    return Stringify(request);
}

ResponseEnvelope ParseEnvelope(std::string_view line) {
    ResponseEnvelope envelope;
    try {
        JsonObject root;
        if (!JsonObject::TryParse(winrt::to_hstring(line), root)) return envelope;

        if (const auto version = FindValue(root, L"jsonrpc")) {
            if (version->ValueType() != JsonValueType::String ||
                version->GetString() != L"2.0") {
                return envelope;
            }
        }

        const auto idValue = FindValue(root, L"id");
        if (!idValue) {
            const auto method = FindValue(root, L"method");
            if (method && method->ValueType() == JsonValueType::String) {
                envelope.kind = EnvelopeKind::kNotification;
            }
            return envelope;
        }
        if (idValue->ValueType() == JsonValueType::String) {
            envelope.kind = EnvelopeKind::kUnknownStringId;
            return envelope;
        }
        if (idValue->ValueType() != JsonValueType::Number) return envelope;
        const double numericId = idValue->GetNumber();
        if (!std::isfinite(numericId) || std::trunc(numericId) != numericId ||
            numericId < 0 || numericId > kMaximumSafeJsonInteger ||
            numericId > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
            return envelope;
        }
        envelope.id = static_cast<std::int32_t>(numericId);

        const auto result = FindValue(root, L"result");
        const auto error = FindValue(root, L"error");
        const bool hasResult = result.has_value();
        const bool hasError = error && error->ValueType() != JsonValueType::Null;
        if (hasResult == hasError) return envelope;

        envelope.kind = EnvelopeKind::kResponse;
        envelope.isError = hasError;
        if (hasResult) envelope.resultJson = winrt::to_string(result->Stringify());
        return envelope;
    } catch (const winrt::hresult_error&) {
        return envelope;
    }
}

MethodFailure GenericMethodFailure(const wchar_t* field, const wchar_t* message) {
    return MethodFailure{MethodFailureKind::kMissingField, field, message};
}

template <typename T>
void ApplyGenericFailure(MethodState<T>& state,
                         const wchar_t* field,
                         const wchar_t* message) {
    ApplyMethodResult(
        state,
        MethodParseResult<T>::Failure(GenericMethodFailure(field, message)));
}

AppServerClientFailureKind FailureForReadStatus(ReadLineStatus status) {
    return status == ReadLineStatus::kTimeout ||
                   status == ReadLineStatus::kProcessTimeout
               ? AppServerClientFailureKind::kTimedOut
               : AppServerClientFailureKind::kTransportFailed;
}

void FailPendingMethods(CodexDataState& data,
                        const AppServerRefreshReport& report,
                        const wchar_t* field,
                        const wchar_t* message) {
    if (!report.rateLimitsResponseReceived) {
        ApplyGenericFailure(data.rateLimits, field, message);
    }
    if (!report.accountResponseReceived) {
        ApplyGenericFailure(data.account, field, message);
    }
    if (!report.usageResponseReceived) {
        ApplyGenericFailure(data.usage, field, message);
    }
    if (!report.threadListResponseReceived) {
        ApplyGenericFailure(data.threadList, field, message);
    }
}

}  // namespace

AppServerRefreshReport CodexAppServerClient::Refresh(
    const std::filesystem::path& executable,
    std::string_view clientVersion,
    const std::function<bool()>& isCancelled) {
    // Never let a failed, cancelled, or incompatible refresh leave a stale
    // filesystem root available to a later local scan.
    codexHome_.reset();
    AppServerRefreshReport report;
    const auto cancelled = [&isCancelled] {
        if (!isCancelled) return false;
        try {
            return isCancelled();
        } catch (...) {
            return true;
        }
    };
    std::optional<std::filesystem::path> initializedCodexHome;
    if (cancelled()) {
        report.failure = AppServerClientFailureKind::kCancelled;
        return report;
    }
    const std::optional<std::string> initialize = BuildInitializeRequest(clientVersion);
    if (!initialize) {
        report.failure = AppServerClientFailureKind::kWriteFailed;
        FailPendingMethods(data_, report, L"$client", L"Initialize request was unavailable");
        return report;
    }

    CodexProcess process;
    if (!process.Start(executable, {L"app-server", L"--stdio"},
                       CodexProcess::kDefaultTotalTimeout)) {
        report.failure = AppServerClientFailureKind::kStartFailed;
        FailPendingMethods(data_, report, L"$transport", L"App-server start failed");
        return report;
    }
    std::size_t responseLineCount = 0;
    const auto acceptResponseLine = [&] {
        ++responseLineCount;
        if (responseLineCount <= kMaximumResponseLines) return true;
        report.failure = AppServerClientFailureKind::kTransportFailed;
        FailPendingMethods(data_, report, L"$transport",
                           L"Response line limit exceeded");
        process.Stop();
        return false;
    };
    if (cancelled()) {
        report.failure = AppServerClientFailureKind::kCancelled;
        process.Stop();
        return report;
    }
    if (!process.WriteLine(*initialize)) {
        report.failure = AppServerClientFailureKind::kWriteFailed;
        FailPendingMethods(data_, report, L"$transport", L"Initialize write failed");
        process.Stop();
        return report;
    }

    for (;;) {
        if (cancelled()) {
            report.failure = AppServerClientFailureKind::kCancelled;
            process.Stop();
            return report;
        }
        const ReadLineResult line = process.ReadLine(kCancellationPollInterval);
        if (line.status == ReadLineStatus::kTimeout) continue;
        if (line.status != ReadLineStatus::kLine) {
            report.failure = FailureForReadStatus(line.status);
            FailPendingMethods(data_, report, L"$transport",
                               L"Initialize response was unavailable");
            process.Stop();
            return report;
        }
        if (!acceptResponseLine()) return report;
        const ResponseEnvelope envelope = ParseEnvelope(line.line);
        if (envelope.kind == EnvelopeKind::kNotification) {
            ++report.ignoredNotificationCount;
            continue;
        }
        if (envelope.kind == EnvelopeKind::kUnknownStringId) {
            ++report.ignoredUnknownIdCount;
            continue;
        }
        if (envelope.kind == EnvelopeKind::kMalformed) {
            ++report.malformedEnvelopeCount;
            continue;
        }
        if (envelope.id != kInitializeRequestId) {
            ++report.ignoredUnknownIdCount;
            continue;
        }
        if (envelope.isError) {
            report.failure = AppServerClientFailureKind::kInitializeRejected;
            FailPendingMethods(data_, report, L"$initialize", L"Initialize was rejected");
            process.Stop();
            return report;
        }
        initializedCodexHome =
            ParseInitializeCodexHomeResultJson(envelope.resultJson);
        report.initialized = true;
        break;
    }

    std::array<std::string, 5> writes;
    try {
        writes = {
            BuildInitializedNotification(),
            BuildNullParamsRequest(kRateLimitsRequestId, L"account/rateLimits/read"),
            BuildAccountRequest(),
            BuildNullParamsRequest(kUsageRequestId, L"account/usage/read"),
            BuildThreadListRequest(),
        };
    } catch (const winrt::hresult_error&) {
        report.failure = AppServerClientFailureKind::kWriteFailed;
        FailPendingMethods(data_, report, L"$transport", L"Request construction failed");
        process.Stop();
        return report;
    }
    for (const std::string& request : writes) {
        if (cancelled()) {
            report.failure = AppServerClientFailureKind::kCancelled;
            process.Stop();
            return report;
        }
        if (!process.WriteLine(request)) {
            report.failure = AppServerClientFailureKind::kWriteFailed;
            FailPendingMethods(data_, report, L"$transport", L"Request write failed");
            process.Stop();
            return report;
        }
    }

    while (!report.allMethodsCompleted()) {
        if (cancelled()) {
            report.failure = AppServerClientFailureKind::kCancelled;
            process.Stop();
            return report;
        }
        const ReadLineResult line = process.ReadLine(kCancellationPollInterval);
        if (line.status == ReadLineStatus::kTimeout) continue;
        if (line.status != ReadLineStatus::kLine) {
            report.failure = FailureForReadStatus(line.status);
            FailPendingMethods(data_, report, L"$transport", L"Response was unavailable");
            process.Stop();
            return report;
        }
        if (!acceptResponseLine()) return report;

        const ResponseEnvelope envelope = ParseEnvelope(line.line);
        if (envelope.kind == EnvelopeKind::kNotification) {
            ++report.ignoredNotificationCount;
            continue;
        }
        if (envelope.kind == EnvelopeKind::kUnknownStringId) {
            ++report.ignoredUnknownIdCount;
            continue;
        }
        if (envelope.kind == EnvelopeKind::kMalformed) {
            ++report.malformedEnvelopeCount;
            continue;
        }

        switch (envelope.id) {
            case kRateLimitsRequestId:
                if (report.rateLimitsResponseReceived) {
                    ++report.ignoredUnknownIdCount;
                    break;
                }
                report.rateLimitsResponseReceived = true;
                if (envelope.isError) {
                    ApplyGenericFailure(data_.rateLimits, L"$jsonrpc.result",
                                        L"Rate-limit request failed");
                } else {
                    ApplyMethodResult(data_.rateLimits,
                                      ParseRateLimitsResultJson(envelope.resultJson));
                }
                break;
            case kAccountRequestId:
                if (report.accountResponseReceived) {
                    ++report.ignoredUnknownIdCount;
                    break;
                }
                report.accountResponseReceived = true;
                if (envelope.isError) {
                    ApplyGenericFailure(data_.account, L"$jsonrpc.result",
                                        L"Account request failed");
                } else {
                    ApplyMethodResult(data_.account,
                                      ParseAccountResultJson(envelope.resultJson));
                }
                break;
            case kUsageRequestId:
                if (report.usageResponseReceived) {
                    ++report.ignoredUnknownIdCount;
                    break;
                }
                report.usageResponseReceived = true;
                if (envelope.isError) {
                    ApplyGenericFailure(data_.usage, L"$jsonrpc.result",
                                        L"Usage request failed");
                } else {
                    ApplyMethodResult(data_.usage,
                                      ParseUsageResultJson(envelope.resultJson));
                }
                break;
            case kThreadListRequestId:
                if (report.threadListResponseReceived) {
                    ++report.ignoredUnknownIdCount;
                    break;
                }
                report.threadListResponseReceived = true;
                if (envelope.isError) {
                    ApplyGenericFailure(data_.threadList, L"$jsonrpc.result",
                                        L"Thread-list request failed");
                } else {
                    // Any returned status remains scoped to this app-server
                    // process by the existing ThreadListData parser/model.
                    ApplyMethodResult(data_.threadList,
                                      ParseThreadListResultJson(envelope.resultJson));
                }
                break;
            default:
                ++report.ignoredUnknownIdCount;
                break;
        }
    }

    process.Stop();
    codexHome_ = std::move(initializedCodexHome);
    return report;
}

}  // namespace codex_monitor::codex
