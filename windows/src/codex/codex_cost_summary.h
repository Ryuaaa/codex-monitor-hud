#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codex/codex_cost_model.h"

namespace codex_monitor::codex {

struct CodexCostEvent {
    // A stable identity for one counted token event. Empty identities are
    // invalid; when identities repeat, the first valid event wins.
    std::string fingerprint;
    std::string localDate;
    std::string model;
    CodexTokenUsage usage;

    // Compacted caches can freeze the estimate that was calculated when the
    // event was first seen. Valid cached values take precedence over the
    // current price table so later price changes do not rewrite history.
    std::optional<double> cachedEstimatedUsd;
    std::optional<std::int64_t> cachedPricedTokens;
};

struct CodexCostPeriodSummary {
    std::int64_t tokens = 0;
    double estimatedUsd = 0.0;
    std::int64_t pricedTokens = 0;
};

struct CodexCostSummary {
    bool available = false;
    CodexCostPeriodSummary today;
    CodexCostPeriodSummary last7Days;
    CodexCostPeriodSummary last30Days;
    CodexCostPeriodSummary monthToDate;

    // A calendar-day linear extrapolation of month-to-date estimated cost.
    // It remains an API-equivalent estimate, not a subscription bill.
    double monthForecastEstimatedUsd = 0.0;

    // Coverage and the top model are calculated over the accepted 30-day
    // event set. Unknown models count in the denominator but are not priced.
    std::int64_t pricedTokens = 0;
    double pricedTokenPercent = 0.0;
    std::string topModel;
    std::int64_t topModelTokens = 0;
    std::size_t eventCount = 0;

    // Any overflowing token/cost aggregation or forecast is clamped to its
    // destination type's maximum and sets this flag.
    bool saturated = false;
};

// referenceLocalDate must be a real Gregorian date in canonical YYYY-MM-DD
// form. Invalid references return nullopt. Events with invalid/empty fields,
// zero input+output tokens, future dates, or dates outside the inclusive
// 30-day window ending on the reference date are ignored.
[[nodiscard]] std::optional<CodexCostSummary> CalculateCodexCostSummary(
    const std::vector<CodexCostEvent>& events,
    std::string_view referenceLocalDate,
    std::string_view trackingStartLocalDate = {});

}  // namespace codex_monitor::codex
