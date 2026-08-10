#include "codex/codex_cost_hybrid.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace codex_monitor::codex {
namespace {

struct TokenTotal {
    bool available = false;
    std::int64_t tokens = 0;
};

[[nodiscard]] bool IsValidCost(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] std::optional<double> ScaledCost(
    const CodexCostPeriodSummary& local,
    std::int64_t targetTokens,
    bool& saturated) noexcept {
    if (local.tokens <= 0 || local.pricedTokens <= 0 || targetTokens < 0 ||
        !IsValidCost(local.estimatedUsd)) {
        return std::nullopt;
    }

    const long double coverage =
        static_cast<long double>(local.pricedTokens) /
        static_cast<long double>(local.tokens) * 100.0L;
    if (!std::isfinite(coverage) ||
        coverage < kMinimumCodexCostPricingCoveragePercent) {
        return std::nullopt;
    }

    const long double averagePrice =
        static_cast<long double>(local.estimatedUsd) /
        static_cast<long double>(local.pricedTokens);
    const long double estimate =
        averagePrice * static_cast<long double>(targetTokens);
    if (!std::isfinite(estimate) ||
        estimate > static_cast<long double>(
                       std::numeric_limits<double>::max())) {
        saturated = true;
        return std::numeric_limits<double>::max();
    }
    if (estimate < 0.0L) {
        return std::nullopt;
    }
    return static_cast<double>(estimate);
}

[[nodiscard]] double PeriodCoverage(
    const CodexCostPeriodSummary& local) noexcept {
    if (local.tokens <= 0 || local.pricedTokens <= 0) return 0.0;
    const long double percent =
        static_cast<long double>(local.pricedTokens) /
        static_cast<long double>(local.tokens) * 100.0L;
    if (!std::isfinite(percent)) return 0.0;
    return static_cast<double>(std::clamp(percent, 0.0L, 100.0L));
}

[[nodiscard]] std::optional<double> ExpandedForecastCost(
    const CodexCostPeriodSummary& local,
    double rawForecast,
    bool& saturated) noexcept {
    if (!IsValidCost(rawForecast) ||
        PeriodCoverage(local) < kMinimumCodexCostPricingCoveragePercent) {
        return std::nullopt;
    }
    const long double expanded =
        static_cast<long double>(rawForecast) *
        static_cast<long double>(local.tokens) /
        static_cast<long double>(local.pricedTokens);
    if (!std::isfinite(expanded) ||
        expanded > static_cast<long double>(
                       std::numeric_limits<double>::max())) {
        saturated = true;
        return std::numeric_limits<double>::max();
    }
    return static_cast<double>(expanded);
}

[[nodiscard]] CodexHybridPeriodSummary MergePeriod(
    TokenTotal official,
    const CodexCostPeriodSummary& local,
    bool localAvailable,
    bool& saturated) noexcept {
    CodexHybridPeriodSummary output;
    output.pricingCoveragePercent = PeriodCoverage(local);
    if (official.available && official.tokens >= 0) {
        output.tokensAvailable = true;
        output.tokens = official.tokens;
        output.usedOfficialTokens = true;
        if (localAvailable) {
            output.estimatedUsd =
                ScaledCost(local, official.tokens, saturated);
        }
        return output;
    }

    if (!localAvailable || local.tokens < 0) {
        return output;
    }
    output.tokensAvailable = true;
    output.tokens = local.tokens;
    output.estimatedUsd = ScaledCost(local, local.tokens, saturated);
    return output;
}

[[nodiscard]] TokenTotal OfficialPeriod(bool sourceAvailable,
                                        std::int64_t tokens) noexcept {
    return TokenTotal{sourceAvailable && tokens >= 0, tokens};
}

}  // namespace

CodexCostHybridSummary CalculateCodexCostHybridSummary(
    const UsageCalendarTotals& officialUsage,
    const CodexCostSummary& localCost) {
    CodexCostHybridSummary output;
    output.saturated = officialUsage.saturated || localCost.saturated;

    const bool officialTodayAvailable =
        officialUsage.sourceAvailable && officialUsage.todayAvailable &&
        officialUsage.todayTokens.has_value() &&
        *officialUsage.todayTokens >= 0;
    const TokenTotal officialToday{
        officialTodayAvailable,
        officialTodayAvailable ? *officialUsage.todayTokens : 0,
    };

    output.today = MergePeriod(officialToday, localCost.today,
                               localCost.available, output.saturated);
    output.last7Days = MergePeriod(
        OfficialPeriod(officialUsage.sourceAvailable,
                       officialUsage.last7DaysTokens),
        localCost.last7Days, localCost.available, output.saturated);
    output.last30Days = MergePeriod(
        OfficialPeriod(officialUsage.sourceAvailable,
                       officialUsage.thirtyDayTokens),
        localCost.last30Days, localCost.available, output.saturated);
    output.monthToDate = MergePeriod(
        OfficialPeriod(officialUsage.sourceAvailable,
                       officialUsage.monthToDateTokens),
        localCost.monthToDate, localCost.available, output.saturated);

    output.available = output.today.tokensAvailable ||
                       output.last7Days.tokensAvailable ||
                       output.last30Days.tokensAvailable ||
                       output.monthToDate.tokensAvailable;
    output.usedOfficialTotals = output.today.usedOfficialTokens ||
                                output.last7Days.usedOfficialTokens ||
                                output.last30Days.usedOfficialTokens ||
                                output.monthToDate.usedOfficialTokens;

    const bool officialForecastAvailable =
        officialUsage.sourceAvailable &&
        officialUsage.monthForecastTokens.has_value() &&
        *officialUsage.monthForecastTokens >= 0;
    if (officialForecastAvailable && localCost.available) {
        output.monthForecastEstimatedUsd = ScaledCost(
            localCost.monthToDate, *officialUsage.monthForecastTokens,
            output.saturated);
        output.monthForecastUsedOfficialTokens =
            output.monthForecastEstimatedUsd.has_value();
    }
    if (!output.monthForecastEstimatedUsd.has_value() &&
        localCost.available && localCost.monthToDate.tokens > 0 &&
        IsValidCost(localCost.monthForecastEstimatedUsd)) {
        output.monthForecastEstimatedUsd = ExpandedForecastCost(
            localCost.monthToDate, localCost.monthForecastEstimatedUsd,
            output.saturated);
        output.monthForecastUsedOfficialTokens = false;
    }
    output.usedOfficialTotals =
        output.usedOfficialTotals || output.monthForecastUsedOfficialTokens;

    if (localCost.available) {
        if (std::isfinite(localCost.pricedTokenPercent)) {
            output.pricedTokenPercent =
                std::clamp(localCost.pricedTokenPercent, 0.0, 100.0);
        }
        output.topModel = localCost.topModel;
        output.topModelTokens =
            std::max<std::int64_t>(0, localCost.topModelTokens);
    }
    return output;
}

}  // namespace codex_monitor::codex
