#include "codex_json_win32.h"

#include <winrt/base.h>

// wingdi.h defines GetObject as GetObjectW under UNICODE. Undefine it before
// the C++/WinRT projection declares IJsonValue::GetObject.
#ifdef GetObject
#undef GetObject
#endif

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace codex_monitor::codex {
namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

MethodFailure Failure(MethodFailureKind kind,
                      std::wstring field,
                      std::wstring message) {
    return MethodFailure{kind, std::move(field), std::move(message)};
}

bool ParseRootObject(std::string_view json,
                     JsonObject& root,
                     MethodFailure& failure) {
    if (json.size() > std::numeric_limits<std::uint32_t>::max()) {
        failure = Failure(MethodFailureKind::kMalformedJson, L"$", L"JSON input is too large");
        return false;
    }
    try {
        const winrt::hstring input = winrt::to_hstring(json);
        if (!JsonObject::TryParse(input, root)) {
            failure = Failure(MethodFailureKind::kMalformedJson, L"$",
                              L"Expected a valid JSON object");
            return false;
        }
        return true;
    } catch (const winrt::hresult_error&) {
        failure = Failure(MethodFailureKind::kMalformedJson, L"$",
                          L"Windows JSON parser rejected the input");
        return false;
    }
}

std::optional<IJsonValue> FindValue(const JsonObject& object, const wchar_t* key) {
    const winrt::hstring name(key);
    if (!object.HasKey(name)) return std::nullopt;
    return object.Lookup(name);
}

std::wstring CopyString(const winrt::hstring& value) {
    return std::wstring(value.c_str(), value.size());
}

bool ReadOptionalString(const JsonObject& object,
                        const wchar_t* key,
                        const wchar_t* field,
                        std::optional<std::wstring>& output,
                        MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value || value->ValueType() == JsonValueType::Null) {
        output.reset();
        return true;
    }
    if (value->ValueType() != JsonValueType::String) {
        failure = Failure(MethodFailureKind::kUnexpectedType, field,
                          L"Expected a string or null");
        return false;
    }
    output = CopyString(value->GetString());
    return true;
}

bool ReadRequiredString(const JsonObject& object,
                        const wchar_t* key,
                        const wchar_t* field,
                        std::wstring& output,
                        MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value) {
        failure = Failure(MethodFailureKind::kMissingField, field,
                          L"Required string is missing");
        return false;
    }
    if (value->ValueType() != JsonValueType::String) {
        failure = Failure(MethodFailureKind::kUnexpectedType, field,
                          L"Expected a string");
        return false;
    }
    output = CopyString(value->GetString());
    return true;
}

bool ReadIntegerValue(const IJsonValue& value,
                      const wchar_t* field,
                      std::int64_t& output,
                      MethodFailure& failure) {
    if (value.ValueType() != JsonValueType::Number) {
        failure = Failure(MethodFailureKind::kUnexpectedType, field,
                          L"Expected an integer JSON number");
        return false;
    }
    const double number = value.GetNumber();
    if (!std::isfinite(number) || std::trunc(number) != number) {
        failure = Failure(MethodFailureKind::kUnexpectedType, field,
                          L"Expected a finite integer JSON number");
        return false;
    }
    if (std::fabs(number) > static_cast<double>(kMaximumSafeJsonInteger)) {
        failure = Failure(MethodFailureKind::kUnsafeInteger, field,
                          L"Integer exceeds the exact JSON number range");
        return false;
    }
    output = static_cast<std::int64_t>(number);
    return true;
}

bool ReadOptionalInteger(const JsonObject& object,
                         const wchar_t* key,
                         const wchar_t* field,
                         std::optional<std::int64_t>& output,
                         MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value || value->ValueType() == JsonValueType::Null) {
        output.reset();
        return true;
    }
    std::int64_t parsed = 0;
    if (!ReadIntegerValue(*value, field, parsed, failure)) return false;
    output = parsed;
    return true;
}

bool ReadRequiredInteger(const JsonObject& object,
                         const wchar_t* key,
                         const wchar_t* field,
                         std::int64_t& output,
                         MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value) {
        failure = Failure(MethodFailureKind::kMissingField, field,
                          L"Required integer is missing");
        return false;
    }
    return ReadIntegerValue(*value, field, output, failure);
}

bool ReadOptionalObject(const JsonObject& object,
                        const wchar_t* key,
                        const wchar_t* field,
                        std::optional<JsonObject>& output,
                        MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(object, key);
    if (!value || value->ValueType() == JsonValueType::Null) {
        output.reset();
        return true;
    }
    if (value->ValueType() != JsonValueType::Object) {
        failure = Failure(MethodFailureKind::kUnexpectedType, field,
                          L"Expected an object or null");
        return false;
    }
    output = value->GetObject();
    return true;
}

bool ParseRateLimitWindow(const JsonObject& rate,
                          const wchar_t* key,
                          const wchar_t* field,
                          std::optional<RateLimitWindow>& output,
                          MethodFailure& failure) {
    std::optional<JsonObject> object;
    if (!ReadOptionalObject(rate, key, field, object, failure)) return false;
    if (!object) {
        output.reset();
        return true;
    }

    std::int64_t usedPercent = 0;
    const std::wstring usedField = std::wstring(field) + L".usedPercent";
    if (!ReadRequiredInteger(*object, L"usedPercent", usedField.c_str(),
                             usedPercent, failure)) {
        return false;
    }
    if (usedPercent < std::numeric_limits<std::int32_t>::min() ||
        usedPercent > std::numeric_limits<std::int32_t>::max()) {
        failure = Failure(MethodFailureKind::kUnexpectedType, usedField,
                          L"Integer is outside the schema int32 range");
        return false;
    }

    RateLimitWindow window;
    window.usedPercent = static_cast<std::int32_t>(usedPercent);
    const std::wstring durationField = std::wstring(field) + L".windowDurationMins";
    if (!ReadOptionalInteger(*object, L"windowDurationMins", durationField.c_str(),
                             window.windowDurationMinutes, failure)) {
        return false;
    }
    const std::wstring resetField = std::wstring(field) + L".resetsAt";
    if (!ReadOptionalInteger(*object, L"resetsAt", resetField.c_str(),
                             window.resetsAtUnixSeconds, failure)) {
        return false;
    }
    output = std::move(window);
    return true;
}

bool SelectRateLimitSnapshot(const JsonObject& root,
                             JsonObject& selected,
                             bool& selectedCodexLimitId,
                             MethodFailure& failure) {
    const std::optional<IJsonValue> bucketsValue = FindValue(root, L"rateLimitsByLimitId");
    if (bucketsValue && bucketsValue->ValueType() != JsonValueType::Null) {
        if (bucketsValue->ValueType() != JsonValueType::Object) {
            failure = Failure(MethodFailureKind::kUnexpectedType,
                              L"rateLimitsByLimitId",
                              L"Expected an object or null");
            return false;
        }
        const JsonObject buckets = bucketsValue->GetObject();
        if (const std::optional<IJsonValue> codexValue = FindValue(buckets, L"codex")) {
            if (codexValue->ValueType() != JsonValueType::Object) {
                failure = Failure(MethodFailureKind::kUnexpectedType,
                                  L"rateLimitsByLimitId.codex",
                                  L"Expected a rate-limit object");
                return false;
            }
            selected = codexValue->GetObject();
            selectedCodexLimitId = true;
            return true;
        }
    }

    const std::optional<IJsonValue> legacy = FindValue(root, L"rateLimits");
    if (!legacy) {
        failure = Failure(MethodFailureKind::kMissingField, L"rateLimits",
                          L"No codex bucket or legacy rateLimits object was returned");
        return false;
    }
    if (legacy->ValueType() != JsonValueType::Object) {
        failure = Failure(MethodFailureKind::kUnexpectedType, L"rateLimits",
                          L"Expected a rate-limit object");
        return false;
    }
    selected = legacy->GetObject();
    selectedCodexLimitId = false;
    return true;
}

bool ParseUsageSummary(const JsonObject& object,
                       UsageSummary& summary,
                       MethodFailure& failure) {
    return ReadOptionalInteger(object, L"currentStreakDays",
                               L"summary.currentStreakDays",
                               summary.currentStreakDays, failure) &&
           ReadOptionalInteger(object, L"lifetimeTokens",
                               L"summary.lifetimeTokens",
                               summary.lifetimeTokens, failure) &&
           ReadOptionalInteger(object, L"longestRunningTurnSec",
                               L"summary.longestRunningTurnSec",
                               summary.longestRunningTurnSeconds, failure) &&
           ReadOptionalInteger(object, L"longestStreakDays",
                               L"summary.longestStreakDays",
                               summary.longestStreakDays, failure) &&
           ReadOptionalInteger(object, L"peakDailyTokens",
                               L"summary.peakDailyTokens",
                               summary.peakDailyTokens, failure);
}

bool ParseDailyUsageBuckets(const JsonObject& root,
                            std::optional<std::vector<DailyUsageBucket>>& output,
                            MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(root, L"dailyUsageBuckets");
    if (!value || value->ValueType() == JsonValueType::Null) {
        output.reset();
        return true;
    }
    if (value->ValueType() != JsonValueType::Array) {
        failure = Failure(MethodFailureKind::kUnexpectedType, L"dailyUsageBuckets",
                          L"Expected an array or null");
        return false;
    }

    std::vector<DailyUsageBucket> buckets;
    std::size_t index = 0;
    for (const IJsonValue& item : value->GetArray()) {
        const std::wstring prefix =
            L"dailyUsageBuckets[" + std::to_wstring(index) + L"]";
        if (item.ValueType() != JsonValueType::Object) {
            failure = Failure(MethodFailureKind::kUnexpectedType, prefix,
                              L"Expected an object");
            return false;
        }
        const JsonObject object = item.GetObject();
        DailyUsageBucket bucket;
        const std::wstring dateField = prefix + L".startDate";
        if (!ReadRequiredString(object, L"startDate", dateField.c_str(),
                                bucket.startDate, failure)) {
            return false;
        }
        const std::wstring tokensField = prefix + L".tokens";
        if (!ReadRequiredInteger(object, L"tokens", tokensField.c_str(),
                                 bucket.tokens, failure)) {
            return false;
        }
        buckets.push_back(std::move(bucket));
        ++index;
    }
    output = std::move(buckets);
    return true;
}

ProcessLocalThreadStatus MapProcessLocalStatus(const std::wstring& type) {
    if (type == L"notLoaded") return ProcessLocalThreadStatus::kNotLoaded;
    if (type == L"idle") return ProcessLocalThreadStatus::kIdle;
    if (type == L"systemError") return ProcessLocalThreadStatus::kSystemError;
    if (type == L"active") return ProcessLocalThreadStatus::kActive;
    return ProcessLocalThreadStatus::kUnknown;
}

bool ParseProcessLocalStatus(const JsonObject& thread,
                             const std::wstring& prefix,
                             std::optional<ProcessLocalThreadStatus>& output,
                             MethodFailure& failure) {
    const std::optional<IJsonValue> value = FindValue(thread, L"status");
    if (!value || value->ValueType() == JsonValueType::Null) {
        output.reset();
        return true;
    }
    const std::wstring statusField = prefix + L".status";
    if (value->ValueType() != JsonValueType::Object) {
        failure = Failure(MethodFailureKind::kUnexpectedType, statusField,
                          L"Expected an object or null");
        return false;
    }
    const JsonObject status = value->GetObject();
    std::wstring type;
    const std::wstring typeField = statusField + L".type";
    if (!ReadRequiredString(status, L"type", typeField.c_str(), type, failure)) {
        return false;
    }
    output = MapProcessLocalStatus(type);
    return true;
}

bool SelectThreadArray(const JsonObject& root,
                       JsonArray& array,
                       bool& usedLegacyThreadsField,
                       MethodFailure& failure) {
    const std::optional<IJsonValue> data = FindValue(root, L"data");
    if (data && data->ValueType() == JsonValueType::Array) {
        array = data->GetArray();
        usedLegacyThreadsField = false;
        return true;
    }
    if (data && data->ValueType() != JsonValueType::Null) {
        failure = Failure(MethodFailureKind::kUnexpectedType, L"data",
                          L"Expected an array");
        return false;
    }

    const std::optional<IJsonValue> threads = FindValue(root, L"threads");
    if (!threads) {
        failure = Failure(MethodFailureKind::kMissingField, L"data",
                          L"Neither data nor the legacy threads array was returned");
        return false;
    }
    if (threads->ValueType() != JsonValueType::Array) {
        failure = Failure(MethodFailureKind::kUnexpectedType, L"threads",
                          L"Expected an array");
        return false;
    }
    array = threads->GetArray();
    usedLegacyThreadsField = true;
    return true;
}

}  // namespace

std::optional<std::filesystem::path> ParseInitializeCodexHomeResultJson(
    std::string_view json) {
    MethodFailure failure;
    JsonObject root;
    if (!ParseRootObject(json, root, failure)) return std::nullopt;

    const std::optional<IJsonValue> value = FindValue(root, L"codexHome");
    if (!value || value->ValueType() != JsonValueType::String) {
        return std::nullopt;
    }

    const std::wstring text = CopyString(value->GetString());
    if (text.empty() || text.find(L'\0') != std::wstring::npos) {
        return std::nullopt;
    }
    try {
        std::filesystem::path path(text);
        if (!path.is_absolute()) return std::nullopt;
        return path;
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

MethodParseResult<RateLimitsData> ParseRateLimitsResultJson(std::string_view json) {
    MethodFailure failure;
    JsonObject root;
    if (!ParseRootObject(json, root, failure)) {
        return MethodParseResult<RateLimitsData>::Failure(std::move(failure));
    }

    JsonObject rate;
    RateLimitsData output;
    if (!SelectRateLimitSnapshot(root, rate, output.selectedCodexLimitId, failure) ||
        !ReadOptionalString(rate, L"planType", L"rateLimits.planType",
                            output.planType, failure) ||
        !ParseRateLimitWindow(rate, L"primary", L"rateLimits.primary",
                              output.primary, failure) ||
        !ParseRateLimitWindow(rate, L"secondary", L"rateLimits.secondary",
                              output.secondary, failure)) {
        return MethodParseResult<RateLimitsData>::Failure(std::move(failure));
    }
    return MethodParseResult<RateLimitsData>::Success(std::move(output));
}

MethodParseResult<AccountData> ParseAccountResultJson(std::string_view json) {
    MethodFailure failure;
    JsonObject root;
    if (!ParseRootObject(json, root, failure)) {
        return MethodParseResult<AccountData>::Failure(std::move(failure));
    }

    AccountData output;
    std::optional<JsonObject> account;
    if (!ReadOptionalObject(root, L"account", L"account", account, failure)) {
        return MethodParseResult<AccountData>::Failure(std::move(failure));
    }
    if (account && !ReadOptionalString(*account, L"planType", L"account.planType",
                                       output.planType, failure)) {
        return MethodParseResult<AccountData>::Failure(std::move(failure));
    }
    return MethodParseResult<AccountData>::Success(std::move(output));
}

MethodParseResult<UsageData> ParseUsageResultJson(std::string_view json) {
    MethodFailure failure;
    JsonObject root;
    if (!ParseRootObject(json, root, failure)) {
        return MethodParseResult<UsageData>::Failure(std::move(failure));
    }

    const std::optional<IJsonValue> summaryValue = FindValue(root, L"summary");
    if (!summaryValue) {
        return MethodParseResult<UsageData>::Failure(
            Failure(MethodFailureKind::kMissingField, L"summary",
                    L"Required summary object is missing"));
    }
    if (summaryValue->ValueType() != JsonValueType::Object) {
        return MethodParseResult<UsageData>::Failure(
            Failure(MethodFailureKind::kUnexpectedType, L"summary",
                    L"Expected an object"));
    }

    UsageData output;
    if (!ParseUsageSummary(summaryValue->GetObject(), output.summary, failure) ||
        !ParseDailyUsageBuckets(root, output.dailyUsageBuckets, failure)) {
        return MethodParseResult<UsageData>::Failure(std::move(failure));
    }
    return MethodParseResult<UsageData>::Success(std::move(output));
}

MethodParseResult<ThreadListData> ParseThreadListResultJson(std::string_view json) {
    MethodFailure failure;
    JsonObject root;
    if (!ParseRootObject(json, root, failure)) {
        return MethodParseResult<ThreadListData>::Failure(std::move(failure));
    }

    ThreadListData output;
    JsonArray threads;
    if (!SelectThreadArray(root, threads, output.usedLegacyThreadsField, failure)) {
        return MethodParseResult<ThreadListData>::Failure(std::move(failure));
    }

    std::size_t index = 0;
    for (const IJsonValue& item : threads) {
        const std::wstring prefix = L"threads[" + std::to_wstring(index) + L"]";
        if (item.ValueType() != JsonValueType::Object) {
            return MethodParseResult<ThreadListData>::Failure(
                Failure(MethodFailureKind::kUnexpectedType, prefix,
                        L"Expected an object"));
        }
        const JsonObject object = item.GetObject();
        ProcessLocalThread thread;
        const std::wstring nameField = prefix + L".name";
        const std::wstring recencyField = prefix + L".recencyAt";
        if (!ReadOptionalString(object, L"name", nameField.c_str(),
                                thread.name, failure) ||
            !ReadOptionalInteger(object, L"recencyAt", recencyField.c_str(),
                                 thread.recencyAtUnixSeconds, failure) ||
            !ParseProcessLocalStatus(object, prefix, thread.processLocalStatus, failure)) {
            return MethodParseResult<ThreadListData>::Failure(std::move(failure));
        }
        output.threads.push_back(std::move(thread));
        ++index;
    }
    return MethodParseResult<ThreadListData>::Success(std::move(output));
}

}  // namespace codex_monitor::codex
