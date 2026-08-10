#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "codex/codex_types.h"

namespace codex_monitor::codex {

struct UsageCalendarTotals {
    // False means the protocol did not return dailyUsageBuckets. An engaged,
    // empty bucket array is available data whose period totals are all zero.
    bool sourceAvailable = false;

    bool todayAvailable = false;
    std::optional<std::int64_t> todayTokens;
    std::optional<std::wstring> latestDate;
    std::optional<std::int64_t> latestTokens;

    std::int64_t last7DaysTokens = 0;
    std::int64_t previous7DaysTokens = 0;
    std::int64_t thirtyDayTokens = 0;
    std::int64_t monthToDateTokens = 0;

    // Available only when the latest valid bucket belongs to the reference
    // month. It is a linear extrapolation from month-to-date usage through the
    // latest reported day, not a billing prediction.
    std::optional<std::int64_t> monthForecastTokens;

    // Any overflowing sum or forecast is clamped to int64_t max and sets this
    // flag, so callers can avoid presenting the clamped value as exact.
    bool saturated = false;
};

// referenceLocalDate must be a real Gregorian date in canonical YYYY-MM-DD
// form. Invalid reference dates return nullopt. Invalid, negative-token, and
// future buckets are ignored.
std::optional<UsageCalendarTotals> CalculateUsageCalendarTotals(
    const UsageData& usage,
    std::wstring_view referenceLocalDate);

}  // namespace codex_monitor::codex
