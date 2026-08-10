#include "codex/codex_cost_summary.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_set>

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
    return month == 2 && IsLeapYear(year) ? 29 : kDays[month];
}

[[nodiscard]] bool ParseCanonicalDate(std::string_view text,
                                      CalendarDate& output) noexcept {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }

    const auto digit = [text](std::size_t index) noexcept {
        return static_cast<int>(text[index] - '0');
    };
    const int year =
        digit(0) * 1000 + digit(1) * 100 + digit(2) * 10 + digit(3);
    const int month = digit(5) * 10 + digit(6);
    const int day = digit(8) * 10 + digit(9);
    if (year < 1 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month)) {
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

[[nodiscard]] std::int64_t NonNegative(std::int64_t value) noexcept {
    return value < 0 ? 0 : value;
}

void SaturatingAdd(std::int64_t value,
                   std::int64_t& total,
                   bool& saturated) noexcept {
    if (value > std::numeric_limits<std::int64_t>::max() - total) {
        total = std::numeric_limits<std::int64_t>::max();
        saturated = true;
        return;
    }
    total += value;
}

void SaturatingAdd(double value, double& total, bool& saturated) noexcept {
    if (!std::isfinite(value) || value < 0.0) {
        return;
    }
    const double maximum = std::numeric_limits<double>::max();
    if (value > maximum - total) {
        total = maximum;
        saturated = true;
        return;
    }
    total += value;
}

struct EventTokenCount {
    std::int64_t clamped = 0;
    long double exact = 0.0L;
};

[[nodiscard]] EventTokenCount CountEventTokens(
    const CodexTokenUsage& usage,
    bool& saturated) noexcept {
    const std::int64_t input = NonNegative(usage.inputTokens);
    const std::int64_t output = NonNegative(usage.outputTokens);
    EventTokenCount result;
    result.clamped = input;
    SaturatingAdd(output, result.clamped, saturated);
    result.exact = static_cast<long double>(input) +
                   static_cast<long double>(output);
    return result;
}

void AddToPeriod(std::int64_t tokens,
                 double cost,
                 std::int64_t pricedTokens,
                 CodexCostPeriodSummary& period,
                 bool& saturated) noexcept {
    SaturatingAdd(tokens, period.tokens, saturated);
    SaturatingAdd(cost, period.estimatedUsd, saturated);
    SaturatingAdd(pricedTokens, period.pricedTokens, saturated);
}

[[nodiscard]] double LinearForecast(double monthToDate,
                                    int elapsedDays,
                                    int totalDays,
                                    bool& saturated) noexcept {
    if (monthToDate <= 0.0) {
        return 0.0;
    }
    const long double forecast =
        static_cast<long double>(monthToDate) /
        static_cast<long double>(elapsedDays) *
        static_cast<long double>(totalDays);
    if (!std::isfinite(forecast) ||
        forecast > static_cast<long double>(
                       std::numeric_limits<double>::max())) {
        saturated = true;
        return std::numeric_limits<double>::max();
    }
    return static_cast<double>(forecast);
}

}  // namespace

std::optional<CodexCostSummary> CalculateCodexCostSummary(
    const std::vector<CodexCostEvent>& events,
    std::string_view referenceLocalDate) {
    CalendarDate reference;
    if (!ParseCanonicalDate(referenceLocalDate, reference)) {
        return std::nullopt;
    }

    CodexCostSummary output;
    const std::int64_t referenceDay = SerialDay(reference);
    const std::int64_t earliestDay = referenceDay - 29;
    std::unordered_set<std::string> seenFingerprints;
    std::map<std::string, std::int64_t> tokensByModel;
    long double coverageTokens = 0.0L;
    long double coveragePricedTokens = 0.0L;

    for (const CodexCostEvent& event : events) {
        if (event.fingerprint.empty()) {
            continue;
        }

        CalendarDate eventDate;
        if (!ParseCanonicalDate(event.localDate, eventDate)) {
            continue;
        }
        const std::int64_t eventDay = SerialDay(eventDate);
        if (eventDay < earliestDay || eventDay > referenceDay) {
            continue;
        }

        bool eventSaturated = false;
        const EventTokenCount eventTokenCount =
            CountEventTokens(event.usage, eventSaturated);
        const std::int64_t tokens = eventTokenCount.clamped;
        if (eventTokenCount.exact == 0.0L) {
            continue;
        }
        if (!seenFingerprints.insert(event.fingerprint).second) {
            continue;
        }
        output.saturated = output.saturated || eventSaturated;

        const CodexCostEstimate estimate =
            EstimateCodexApiEquivalentCost(event.model, event.usage);
        const bool validCachedCost = event.cachedEstimatedUsd.has_value() &&
                                     std::isfinite(*event.cachedEstimatedUsd) &&
                                     *event.cachedEstimatedUsd >= 0.0;
        // The cached amount and its priced-token denominator are one atomic
        // estimate. Accepting only one side can turn an invalid cache entry
        // into an apparently complete zero-dollar sample.
        const bool validCachedPricing =
            validCachedCost && event.cachedPricedTokens.has_value() &&
            *event.cachedPricedTokens >= 0;
        const double cost = validCachedPricing
                                ? *event.cachedEstimatedUsd
                                : (estimate.available ? estimate.estimatedUsd
                                                      : 0.0);

        std::int64_t eventPricedTokens = 0;
        long double exactEventPricedTokens = 0.0L;
        if (validCachedPricing) {
            eventPricedTokens = std::min(
                tokens, NonNegative(*event.cachedPricedTokens));
            exactEventPricedTokens =
                std::min(eventTokenCount.exact,
                         static_cast<long double>(eventPricedTokens));
        } else if (estimate.available) {
            eventPricedTokens = tokens;
            exactEventPricedTokens = eventTokenCount.exact;
        }

        const std::string normalizedModel =
            estimate.normalizedModel.empty()
                ? NormalizeCodexCostModel(event.model)
                : estimate.normalizedModel;
        const std::string modelKey =
            normalizedModel.empty() ? "unknown" : normalizedModel;
        SaturatingAdd(tokens, tokensByModel[modelKey], output.saturated);
        SaturatingAdd(eventPricedTokens, output.pricedTokens,
                      output.saturated);

        AddToPeriod(tokens, cost, eventPricedTokens,
                    output.last30Days, output.saturated);
        if (eventDay >= referenceDay - 6) {
            AddToPeriod(tokens, cost, eventPricedTokens,
                        output.last7Days, output.saturated);
        }
        if (eventDay == referenceDay) {
            AddToPeriod(tokens, cost, eventPricedTokens,
                        output.today, output.saturated);
        }
        if (eventDate.year == reference.year &&
            eventDate.month == reference.month) {
            AddToPeriod(tokens, cost, eventPricedTokens,
                        output.monthToDate, output.saturated);
        }
        coverageTokens += eventTokenCount.exact;
        coveragePricedTokens += exactEventPricedTokens;
        if (!std::isfinite(coverageTokens) ||
            !std::isfinite(coveragePricedTokens)) {
            output.saturated = true;
        }

        if (output.eventCount == std::numeric_limits<std::size_t>::max()) {
            output.saturated = true;
        } else {
            ++output.eventCount;
        }
    }

    output.available = output.eventCount > 0;
    output.monthForecastEstimatedUsd = LinearForecast(
        output.monthToDate.estimatedUsd,
        reference.day,
        DaysInMonth(reference.year, reference.month),
        output.saturated);

    if (coverageTokens > 0.0L && std::isfinite(coverageTokens) &&
        std::isfinite(coveragePricedTokens)) {
        long double percent = coveragePricedTokens / coverageTokens * 100.0L;
        percent = std::clamp(percent, 0.0L, 100.0L);
        output.pricedTokenPercent = static_cast<double>(percent);
    }

    for (const auto& [model, tokens] : tokensByModel) {
        if (tokens > output.topModelTokens) {
            output.topModel = model;
            output.topModelTokens = tokens;
        }
    }
    return output;
}

}  // namespace codex_monitor::codex
