#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace codex_monitor::codex {

// Windows.Data.Json exposes JSON numbers as doubles. Values outside JavaScript's
// safe-integer range are rejected before they enter the product model.
constexpr std::int64_t kMaximumSafeJsonInteger = 9007199254740991LL;

enum class MethodFailureKind {
    kMalformedJson,
    kMissingField,
    kUnexpectedType,
    kUnsafeInteger,
};

struct MethodFailure {
    MethodFailureKind kind = MethodFailureKind::kMalformedJson;
    std::wstring field;
    std::wstring message;
};

template <typename T>
struct MethodParseResult {
    std::optional<T> value;
    std::optional<MethodFailure> failure;

    [[nodiscard]] bool ok() const noexcept {
        return value.has_value() && !failure.has_value();
    }

    static MethodParseResult Success(T parsedValue) {
        MethodParseResult result;
        result.value = std::move(parsedValue);
        return result;
    }

    static MethodParseResult Failure(MethodFailure methodFailure) {
        MethodParseResult result;
        result.failure = std::move(methodFailure);
        return result;
    }
};

template <typename T>
struct MethodState {
    // A failed refresh records its own failure but deliberately retains the
    // last successful value. Other method states are independent.
    std::optional<T> lastValue;
    std::optional<MethodFailure> lastFailure;
};

template <typename T>
void ApplyMethodResult(MethodState<T>& state, MethodParseResult<T> result) {
    if (result.ok()) {
        state.lastValue = std::move(result.value);
        state.lastFailure.reset();
        return;
    }
    state.lastFailure = std::move(result.failure);
}

struct RateLimitWindow {
    std::int32_t usedPercent = 0;
    std::optional<std::int64_t> windowDurationMinutes;
    std::optional<std::int64_t> resetsAtUnixSeconds;
};

struct RateLimitsData {
    std::optional<std::wstring> planType;
    std::optional<RateLimitWindow> primary;
    std::optional<RateLimitWindow> secondary;
    bool selectedCodexLimitId = false;
};

struct AccountData {
    // Intentionally excludes account email, auth tokens, and account IDs.
    std::optional<std::wstring> planType;
};

struct DailyUsageBucket {
    std::wstring startDate;
    std::int64_t tokens = 0;
};

struct UsageSummary {
    std::optional<std::int64_t> currentStreakDays;
    std::optional<std::int64_t> lifetimeTokens;
    std::optional<std::int64_t> longestRunningTurnSeconds;
    std::optional<std::int64_t> longestStreakDays;
    std::optional<std::int64_t> peakDailyTokens;
};

struct UsageData {
    // null or an omitted dailyUsageBuckets field remains unavailable; an empty
    // JSON array is represented by an engaged, empty vector.
    std::optional<std::vector<DailyUsageBucket>> dailyUsageBuckets;
    UsageSummary summary;
};

enum class ProcessLocalThreadStatus {
    kNotLoaded,
    kIdle,
    kSystemError,
    kActive,
    kUnknown,
};

struct ProcessLocalThread {
    std::optional<std::wstring> name;
    std::optional<std::int64_t> recencyAtUnixSeconds;

    // This is the status reported by the particular app-server process being
    // queried. It is not a desktop-wide or globally authoritative live state.
    std::optional<ProcessLocalThreadStatus> processLocalStatus;

    // Deliberately no id, preview, cwd, path, session ID, or turn content.
};

struct ThreadListData {
    std::vector<ProcessLocalThread> threads;
    bool usedLegacyThreadsField = false;
};

struct CodexDataState {
    MethodState<RateLimitsData> rateLimits;
    MethodState<AccountData> account;
    MethodState<UsageData> usage;
    MethodState<ThreadListData> threadList;
};

}  // namespace codex_monitor::codex
