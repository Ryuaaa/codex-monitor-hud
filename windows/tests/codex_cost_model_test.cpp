#include "codex/codex_cost_model.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using codex_monitor::codex::CodexCostEstimate;
using codex_monitor::codex::CodexTokenUsage;
using codex_monitor::codex::EstimateCodexApiEquivalentCost;
using codex_monitor::codex::NormalizeCodexCostModel;
using codex_monitor::codex::kCodexCostPricingVersion;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectNear(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 1e-12) {
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        std::exit(1);
    }
}

CodexCostEstimate RequireEstimate(std::string model,
                                  CodexTokenUsage usage,
                                  const char* message) {
    CodexCostEstimate result =
        EstimateCodexApiEquivalentCost(model, usage);
    Expect(result.available, message);
    Expect(result.pricingVersion == kCodexCostPricingVersion,
           "available result carries frozen pricing version");
    Expect(std::isfinite(result.estimatedUsd),
           "available estimate must be finite");
    return result;
}

void TestGpt56PricesAndCacheWrite() {
    const CodexTokenUsage usage{200000, 40000, 20000, 10000};

    const CodexCostEstimate sol =
        RequireEstimate("gpt-5.6-sol", usage, "gpt-5.6-sol available");
    ExpectNear(sol.estimatedUsd, 1.145, "gpt-5.6-sol cache-write price");

    const CodexCostEstimate terra =
        RequireEstimate("gpt-5.6-terra", usage, "gpt-5.6-terra available");
    ExpectNear(terra.estimatedUsd, 0.458, "gpt-5.6-terra price");

    const CodexCostEstimate luna =
        RequireEstimate("gpt-5.6-luna", usage, "gpt-5.6-luna available");
    ExpectNear(luna.estimatedUsd, 0.0458, "gpt-5.6-luna price");
}

void TestGpt54AndLongContextBoundary() {
    const CodexCostEstimate standard = RequireEstimate(
        "gpt-5.4", {272000, 72000, 50000, 10000},
        "gpt-5.4 threshold available");
    Expect(!standard.longContext,
           "exactly 272000 input tokens use standard context pricing");
    // gpt-5.4 has no distinct cache-write entry, so writes use input price.
    ExpectNear(standard.estimatedUsd, 0.668,
               "gpt-5.4 cache write falls back to input price");

    const CodexCostEstimate longContext = RequireEstimate(
        "gpt-5.4", {272001, 72000, 50000, 10000},
        "gpt-5.4 long context available");
    Expect(longContext.longContext,
           "more than 272000 input tokens use long-context pricing");
    ExpectNear(longContext.estimatedUsd, 1.261005,
               "gpt-5.4 long-context rates");
}

void TestNormalizationAndAliases() {
    Expect(NormalizeCodexCostModel("  openai/gpt-5.6  ") ==
               "gpt-5.6-sol",
           "trim, OpenAI prefix, and gpt-5.6 default alias");
    Expect(NormalizeCodexCostModel("gpt-5.1-codex-max") == "gpt-5.1",
           "codex max alias");
    Expect(NormalizeCodexCostModel("gpt-5.1-codex-mini") == "gpt-5-mini",
           "codex mini alias");
    Expect(NormalizeCodexCostModel("gpt-5.2-codex") == "gpt-5.2",
           "gpt-5.2 codex alias");
    Expect(NormalizeCodexCostModel("openai/gpt-5.4-2026-08-08") ==
               "gpt-5.4",
           "provider prefix and date suffix");

    const CodexCostEstimate alias = RequireEstimate(
        "openai/gpt-5.4-2026-08-08", {1000000, 0, 0, 0},
        "normalized dated model available");
    ExpectNear(alias.estimatedUsd, 5.0,
               "dated gpt-5.4 uses long-context price");
}

void TestUnknownAndInvalidCounts() {
    const CodexCostEstimate unknown = EstimateCodexApiEquivalentCost(
        "openai/not-a-priced-model-2026-08-08", {1000, 0, 0, 0});
    Expect(!unknown.available && unknown.normalizedModel == "not-a-priced-model" &&
               unknown.pricingVersion.empty(),
           "unknown model remains unavailable after normalization");

    const CodexCostEstimate negative = RequireEstimate(
        "gpt-5.6-sol", {-1, -2, -3, -4}, "negative counts are safe");
    ExpectNear(negative.estimatedUsd, 0.0,
               "negative token counts clamp to zero");

    const CodexCostEstimate clipped = RequireEstimate(
        "gpt-5.6-sol", {100, 90, 90, 0}, "cache counts are clipped");
    ExpectNear(clipped.estimatedUsd, 0.0001075,
               "cached and write input cannot exceed total input");
}

void TestExtremeCountsRemainFinite() {
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    const CodexCostEstimate extreme = RequireEstimate(
        "gpt-5.4-pro", {maximum, maximum, maximum, maximum},
        "maximum integer counts are safe");
    Expect(extreme.longContext, "extreme input selects long-context pricing");
    Expect(extreme.estimatedUsd > 0.0 && std::isfinite(extreme.estimatedUsd),
           "extreme estimate is positive and finite");
}

}  // namespace

int main() {
    TestGpt56PricesAndCacheWrite();
    TestGpt54AndLongContextBoundary();
    TestNormalizationAndAliases();
    TestUnknownAndInvalidCounts();
    TestExtremeCountsRemainFinite();
    std::cout << "codex_cost_model_test: pass\n";
    return 0;
}
