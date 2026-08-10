#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "codex/codex_cost_summary.h"
#include "codex/codex_usage_math.h"

namespace codex_monitor::codex {

inline constexpr double kMinimumCodexCostPricingCoveragePercent = 20.0;

struct CodexHybridPeriodSummary {
    bool tokensAvailable = false;
    std::int64_t tokens = 0;
    bool usedOfficialTokens = false;
    double pricingCoveragePercent = 0.0;

    // API-equivalent estimate inferred from the same period's local average
    // price per token. nullopt means there was no usable local price sample.
    std::optional<double> estimatedUsd;
};

struct CodexCostHybridSummary {
    bool available = false;
    CodexHybridPeriodSummary today;
    CodexHybridPeriodSummary last7Days;
    CodexHybridPeriodSummary last30Days;
    CodexHybridPeriodSummary monthToDate;

    std::optional<double> monthForecastEstimatedUsd;
    bool monthForecastUsedOfficialTokens = false;

    // These retain the quality and model information of the local pricing
    // sample; scaling to official totals does not pretend unknown models were
    // priced.
    double pricedTokenPercent = 0.0;
    std::string topModel;
    std::int64_t topModelTokens = 0;

    bool usedOfficialTotals = false;
    bool saturated = false;
};

// Official period totals take precedence when that period is available.
// Otherwise the corresponding local total is used. Estimated cost always
// needs a non-zero local token sample from the same period.
[[nodiscard]] CodexCostHybridSummary CalculateCodexCostHybridSummary(
    const UsageCalendarTotals& officialUsage,
    const CodexCostSummary& localCost);

}  // namespace codex_monitor::codex
