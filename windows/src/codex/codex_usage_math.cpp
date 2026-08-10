#include "codex/codex_usage_math.h"

#include <limits>
#include <map>

namespace codex_monitor::codex {
namespace {

struct CalendarDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

[[nodiscard]] bool IsLeapYear(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

[[nodiscard]] int DaysInMonth(int year, int month) noexcept {
    static constexpr int kDays[] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && IsLeapYear(year)) {
        return 29;
    }
    return kDays[month];
}

[[nodiscard]] bool ParseCanonicalDate(std::wstring_view text,
                                      CalendarDate& output) noexcept {
    if (text.size() != 10 || text[4] != L'-' || text[7] != L'-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (text[index] < L'0' || text[index] > L'9') {
            return false;
        }
    }

    const auto digit = [text](std::size_t index) noexcept {
        return static_cast<int>(text[index] - L'0');
    };
    const int year = digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
    const int month = digit(5) * 10 + digit(6);
    const int day = digit(8) * 10 + digit(9);
    if (year < 1 || month < 1 || month > 12 ||
        day < 1 || day > DaysInMonth(year, month)) {
        return false;
    }
    output = CalendarDate{year, month, day};
    return true;
}

[[nodiscard]] std::int64_t SerialDay(const CalendarDate& date) noexcept {
    const std::int64_t priorYears = static_cast<std::int64_t>(date.year - 1);
    std::int64_t days = priorYears * 365 + priorYears / 4 - priorYears / 100 +
                        priorYears / 400;
    for (int month = 1; month < date.month; ++month) {
        days += DaysInMonth(date.year, month);
    }
    return days + date.day - 1;
}

void SaturatingAdd(std::int64_t value,
                   std::int64_t& total,
                   bool& saturated) noexcept {
    // Token buckets are filtered to non-negative values before this helper.
    if (value > std::numeric_limits<std::int64_t>::max() - total) {
        total = std::numeric_limits<std::int64_t>::max();
        saturated = true;
        return;
    }
    total += value;
}

[[nodiscard]] std::int64_t SumRange(
    const std::map<std::int64_t, std::int64_t>& tokensByDay,
    std::int64_t firstDay,
    std::int64_t lastDay,
    bool& saturated) noexcept {
    std::int64_t total = 0;
    auto iterator = tokensByDay.lower_bound(firstDay);
    while (iterator != tokensByDay.end() && iterator->first <= lastDay) {
        SaturatingAdd(iterator->second, total, saturated);
        ++iterator;
    }
    return total;
}

[[nodiscard]] std::int64_t SaturatingLinearForecast(
    std::int64_t monthToDate,
    int elapsedDays,
    int totalDays,
    bool& saturated) noexcept {
    const std::int64_t quotient = monthToDate / elapsedDays;
    const std::int64_t remainder = monthToDate % elapsedDays;
    const std::int64_t days = totalDays;

    std::int64_t whole = 0;
    if (quotient > std::numeric_limits<std::int64_t>::max() / days) {
        whole = std::numeric_limits<std::int64_t>::max();
        saturated = true;
    } else {
        whole = quotient * days;
    }

    // elapsedDays and totalDays are at most 31, so this multiplication cannot
    // overflow. Adding half the divisor implements llround for non-negative
    // values without converting a potentially huge integer to floating point.
    const std::int64_t roundedFraction =
        (remainder * days + elapsedDays / 2) / elapsedDays;
    SaturatingAdd(roundedFraction, whole, saturated);
    return whole;
}

}  // namespace

std::optional<UsageCalendarTotals> CalculateUsageCalendarTotals(
    const UsageData& usage,
    std::wstring_view referenceLocalDate) {
    CalendarDate reference;
    if (!ParseCanonicalDate(referenceLocalDate, reference)) {
        return std::nullopt;
    }

    UsageCalendarTotals output;
    if (!usage.dailyUsageBuckets.has_value()) {
        return output;
    }
    output.sourceAvailable = true;

    const std::int64_t referenceDay = SerialDay(reference);
    std::map<std::int64_t, std::int64_t> tokensByDay;
    std::map<std::int64_t, CalendarDate> datesByDay;
    for (const DailyUsageBucket& bucket : *usage.dailyUsageBuckets) {
        CalendarDate date;
        if (!ParseCanonicalDate(bucket.startDate, date) || bucket.tokens < 0) {
            continue;
        }
        const std::int64_t serialDay = SerialDay(date);
        if (serialDay > referenceDay) {
            continue;
        }
        std::int64_t& total = tokensByDay[serialDay];
        SaturatingAdd(bucket.tokens, total, output.saturated);
        datesByDay[serialDay] = date;
    }

    const auto today = tokensByDay.find(referenceDay);
    if (today != tokensByDay.end()) {
        output.todayAvailable = true;
        output.todayTokens = today->second;
    }

    if (tokensByDay.empty()) {
        return output;
    }

    const auto latest = tokensByDay.rbegin();
    const CalendarDate& latestCalendarDate = datesByDay.at(latest->first);
    output.latestDate = std::wstring(referenceLocalDate.size(), L'0');
    const auto twoDigits = [](int value, wchar_t* destination) noexcept {
        destination[0] = static_cast<wchar_t>(L'0' + value / 10);
        destination[1] = static_cast<wchar_t>(L'0' + value % 10);
    };
    (*output.latestDate)[0] = static_cast<wchar_t>(L'0' + latestCalendarDate.year / 1000);
    (*output.latestDate)[1] = static_cast<wchar_t>(L'0' + latestCalendarDate.year / 100 % 10);
    (*output.latestDate)[2] = static_cast<wchar_t>(L'0' + latestCalendarDate.year / 10 % 10);
    (*output.latestDate)[3] = static_cast<wchar_t>(L'0' + latestCalendarDate.year % 10);
    (*output.latestDate)[4] = L'-';
    twoDigits(latestCalendarDate.month, &(*output.latestDate)[5]);
    (*output.latestDate)[7] = L'-';
    twoDigits(latestCalendarDate.day, &(*output.latestDate)[8]);
    output.latestTokens = latest->second;

    const std::int64_t anchorDay = latest->first;
    output.last7DaysTokens =
        SumRange(tokensByDay, anchorDay - 6, anchorDay, output.saturated);
    output.previous7DaysTokens =
        SumRange(tokensByDay, anchorDay - 13, anchorDay - 7, output.saturated);
    output.thirtyDayTokens =
        SumRange(tokensByDay, anchorDay - 29, anchorDay, output.saturated);

    const CalendarDate monthStartDate{reference.year, reference.month, 1};
    output.monthToDateTokens =
        SumRange(tokensByDay, SerialDay(monthStartDate), referenceDay, output.saturated);

    if (latestCalendarDate.year == reference.year &&
        latestCalendarDate.month == reference.month) {
        output.monthForecastTokens = SaturatingLinearForecast(
            output.monthToDateTokens,
            latestCalendarDate.day,
            DaysInMonth(reference.year, reference.month),
            output.saturated);
    }

    return output;
}

}  // namespace codex_monitor::codex
