#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace codex_monitor::codex {

inline constexpr std::string_view kCodexCostPricingVersion =
    "OpenAI 2026-08-08";

struct CodexTokenUsage {
    std::int64_t inputTokens = 0;
    std::int64_t cachedInputTokens = 0;
    std::int64_t cacheWriteInputTokens = 0;
    std::int64_t outputTokens = 0;
};

struct CodexCostEstimate {
    // This is an API-equivalent estimate using the frozen public price table,
    // not a Codex subscription charge or an actual invoice amount.
    bool available = false;
    std::string normalizedModel;
    double estimatedUsd = 0.0;
    bool longContext = false;
    std::string pricingVersion;
};

// Matches the model cleanup and aliases used by the frozen macOS 1.0 cost
// model. Unknown names are normalized but deliberately remain unpriced.
[[nodiscard]] std::string NormalizeCodexCostModel(std::string_view rawModel);

// Negative token values are treated as zero. Cached and cache-write input are
// clipped to the total input count so the same input token cannot be charged
// twice. The result is unavailable for an unknown model or a non-finite cost.
[[nodiscard]] CodexCostEstimate EstimateCodexApiEquivalentCost(
    std::string_view model,
    const CodexTokenUsage& usage) noexcept;

}  // namespace codex_monitor::codex
