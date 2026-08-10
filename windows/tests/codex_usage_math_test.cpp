#include "codex/codex_usage_math.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using codex_monitor::codex::CalculateUsageCalendarTotals;
using codex_monitor::codex::DailyUsageBucket;
using codex_monitor::codex::UsageCalendarTotals;
using codex_monitor::codex::UsageData;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

UsageData Usage(std::initializer_list<DailyUsageBucket> buckets) {
    UsageData usage;
    usage.dailyUsageBuckets = std::vector<DailyUsageBucket>(buckets);
    return usage;
}

UsageCalendarTotals Require(std::optional<UsageCalendarTotals> result,
                            const char* message) {
    Expect(result.has_value(), message);
    return *result;
}

void TestDelayedSettlementAndAnchoredWindows() {
    const UsageData usage = Usage({
        {L"2026-08-09", 50},
        {L"2026-08-08", 25},
        {L"2026-08-02", 40},
    });
    const auto result = CalculateUsageCalendarTotals(usage, L"2026-08-10");
    const UsageCalendarTotals totals = Require(result, "delayed result");
    Expect(totals.sourceAvailable, "delayed source available");
    Expect(!totals.todayAvailable && !totals.todayTokens,
           "missing today must remain unavailable");
    Expect(totals.latestDate == L"2026-08-09" && totals.latestTokens == 50,
           "delayed latest bucket");
    Expect(totals.last7DaysTokens == 75, "delayed latest seven days");
    Expect(totals.previous7DaysTokens == 40, "delayed previous seven days");
    Expect(totals.thirtyDayTokens == 115, "delayed thirty days");
    Expect(totals.monthToDateTokens == 115, "delayed month to date");
    Expect(totals.monthForecastTokens == 396, "delayed monthly forecast");
}

void TestDuplicateBuckets() {
    const UsageData usage = Usage({
        {L"2026-08-10", 100},
        {L"2026-08-10", 75},
        {L"2026-08-09", 25},
    });
    const UsageCalendarTotals totals = Require(
        CalculateUsageCalendarTotals(usage, L"2026-08-10"),
        "duplicate result");
    Expect(totals.todayAvailable && totals.todayTokens == 175,
           "duplicate today buckets add");
    Expect(totals.latestTokens == 175 && totals.last7DaysTokens == 200,
           "duplicate latest and window totals");
}

void TestLeapYearAndMonthBoundary() {
    const UsageData usage = Usage({
        {L"2024-03-01", 10},
        {L"2024-02-29", 20},
        {L"2024-02-24", 30},
        {L"2024-02-23", 40},
        {L"2024-03-02", 999},
    });
    const UsageCalendarTotals totals = Require(
        CalculateUsageCalendarTotals(usage, L"2024-03-01"),
        "leap result");
    Expect(totals.last7DaysTokens == 60, "leap seven day boundary");
    Expect(totals.previous7DaysTokens == 40, "leap prior window boundary");
    Expect(totals.thirtyDayTokens == 100, "leap thirty day total");
    Expect(totals.monthToDateTokens == 10, "March month to date only");
    Expect(totals.monthForecastTokens == 310, "March linear forecast");

    const UsageCalendarTotals priorMonth = Require(
        CalculateUsageCalendarTotals(
            Usage({{L"2024-02-29", 290}}), L"2024-03-01"),
        "prior month result");
    Expect(!priorMonth.monthForecastTokens,
           "forecast unavailable when latest bucket is in prior month");
}

void TestBadAndFutureDates() {
    const UsageData usage = Usage({
        {L"2026-02-28", 20},
        {L"2026-02-29", 100},
        {L"2026-13-01", 100},
        {L"2026-2-28", 100},
        {L"2026-03-01", 100},
        {L"2026-02-27", -100},
    });
    const UsageCalendarTotals totals = Require(
        CalculateUsageCalendarTotals(usage, L"2026-02-28"),
        "invalid buckets result");
    Expect(totals.todayTokens == 20 && totals.latestTokens == 20,
           "bad future and negative buckets ignored");
    Expect(totals.thirtyDayTokens == 20 && totals.monthToDateTokens == 20,
           "only valid bucket contributes");
    Expect(!CalculateUsageCalendarTotals(usage, L"2026-02-29"),
           "invalid reference date rejected");
}

void TestEmptyAndUnavailableData() {
    UsageData unavailable;
    const UsageCalendarTotals missing = Require(
        CalculateUsageCalendarTotals(unavailable, L"2026-08-10"),
        "unavailable result");
    Expect(!missing.sourceAvailable && !missing.todayTokens && !missing.latestDate &&
               !missing.monthForecastTokens,
           "omitted buckets stay unavailable");

    const UsageCalendarTotals empty = Require(
        CalculateUsageCalendarTotals(Usage({}), L"2026-08-10"),
        "empty result");
    Expect(empty.sourceAvailable && !empty.todayAvailable && !empty.todayTokens &&
               !empty.latestDate && empty.last7DaysTokens == 0 &&
               empty.thirtyDayTokens == 0 && !empty.monthForecastTokens,
           "engaged empty buckets are available but do not fabricate a day");
}

void TestSaturatingLargeIntegers() {
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    const UsageData usage = Usage({
        {L"2026-08-10", maximum},
        {L"2026-08-10", 1},
        {L"2026-08-09", maximum},
    });
    const UsageCalendarTotals totals = Require(
        CalculateUsageCalendarTotals(usage, L"2026-08-10"),
        "large result");
    Expect(totals.saturated, "large calculations report saturation");
    Expect(totals.todayTokens == maximum && totals.latestTokens == maximum,
           "duplicate bucket sum saturates");
    Expect(totals.last7DaysTokens == maximum &&
               totals.thirtyDayTokens == maximum &&
               totals.monthToDateTokens == maximum &&
               totals.monthForecastTokens == maximum,
           "period totals and forecast saturate without overflow");
}

}  // namespace

int main() {
    TestDelayedSettlementAndAnchoredWindows();
    TestDuplicateBuckets();
    TestLeapYearAndMonthBoundary();
    TestBadAndFutureDates();
    TestEmptyAndUnavailableData();
    TestSaturatingLargeIntegers();
    std::cout << "codex_usage_math_test: pass\n";
    return 0;
}
