#include "codex/codex_cost_summary.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using codex_monitor::codex::CalculateCodexCostSummary;
using codex_monitor::codex::CodexCostEvent;
using codex_monitor::codex::CodexCostSummary;
using codex_monitor::codex::CodexTokenUsage;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectNear(double actual,
                double expected,
                double tolerance,
                const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        std::exit(1);
    }
}

CodexCostEvent Event(std::string fingerprint,
                     std::string date,
                     std::string model,
                     std::int64_t input,
                     std::int64_t output) {
    CodexCostEvent event;
    event.fingerprint = std::move(fingerprint);
    event.localDate = std::move(date);
    event.model = std::move(model);
    event.usage = CodexTokenUsage{input, 0, 0, output};
    return event;
}

CodexCostSummary Require(
    std::optional<CodexCostSummary> result,
    const char* message) {
    Expect(result.has_value(), message);
    return *result;
}

void TestWindowsAndDuplicateFingerprint() {
    std::vector<CodexCostEvent> events{
        Event("today-a", "2026-08-11", "gpt-5.6-sol", 100000, 10000),
        Event("today-a", "2026-08-11", "gpt-5.6-sol", 900000, 90000),
        Event("six-days", "2026-08-05", "gpt-5.6-sol", 200000, 20000),
        Event("seven-days", "2026-08-04", "gpt-5.6-sol", 300000, 30000),
        Event("old-edge", "2026-07-13", "gpt-5.6-sol", 400000, 40000),
        Event("too-old", "2026-07-12", "gpt-5.6-sol", 500000, 50000),
    };

    const CodexCostSummary result = Require(
        CalculateCodexCostSummary(events, "2026-08-11"),
        "window result");
    Expect(result.available && result.eventCount == 4,
           "only unique events in the 30-day window count");
    Expect(result.today.tokens == 110000, "today token total");
    Expect(result.last7Days.tokens == 330000,
           "seven-day window includes reference day and prior six");
    Expect(result.last30Days.tokens == 1100000,
           "30-day window includes its oldest boundary");
    Expect(result.monthToDate.tokens == 660000,
           "month-to-date excludes July boundary event");
    Expect(result.topModel == "gpt-5.6-sol" &&
               result.topModelTokens == 1100000,
           "top model aggregates duplicate dates and models");
    ExpectNear(result.pricedTokenPercent, 100.0, 1e-12,
               "known models have full price coverage");
}

void TestUnknownModelAndCachedHistory() {
    CodexCostEvent unknown =
        Event("unknown", "2026-08-11", "future-unpriced-model", 900, 100);
    CodexCostEvent frozen =
        Event("cached", "2026-08-10", "removed-old-model", 1800, 200);
    frozen.cachedEstimatedUsd = 12.5;
    frozen.cachedPricedTokens = 1500;
    CodexCostEvent known =
        Event("known", "2026-08-09", "gpt-5.6-luna", 900, 100);

    const CodexCostSummary result = Require(
        CalculateCodexCostSummary({unknown, frozen, known}, "2026-08-11"),
        "coverage result");
    Expect(result.last30Days.tokens == 4000,
           "unknown and cached models remain in token totals");
    Expect(result.pricedTokens == 2500,
           "cached priced tokens and live-priced tokens add");
    ExpectNear(result.pricedTokenPercent, 62.5, 1e-12,
               "unknown tokens remain in coverage denominator");
    Expect(result.last30Days.estimatedUsd > 12.5,
           "cached and live estimates add");
    ExpectNear(result.last30Days.estimatedUsd, 12.5003, 1e-12,
               "cached historical cost is used exactly");
    Expect(result.topModel == "removed-old-model" &&
               result.topModelTokens == 2000,
           "top model includes cached models even if now unpriced");
}

void TestLeapYearMonthBoundaryAndForecast() {
    const std::vector<CodexCostEvent> events{
        Event("leap", "2024-02-29", "gpt-5.6-luna", 1000000, 0),
        Event("march-a", "2024-03-01", "gpt-5.6-luna", 1000000, 0),
        Event("march-b", "2024-03-02", "gpt-5.6-luna", 1000000, 0),
    };
    const CodexCostSummary result = Require(
        CalculateCodexCostSummary(events, "2024-03-02"),
        "leap month result");
    Expect(result.last7Days.tokens == 3000000,
           "leap day crosses into March correctly");
    Expect(result.monthToDate.tokens == 2000000,
           "month-to-date excludes leap-day event");
    ExpectNear(result.monthToDate.estimatedUsd, 0.8, 1e-12,
               "March cost total");
    ExpectNear(result.monthForecastEstimatedUsd, 12.4, 1e-12,
               "31-day March linear forecast from two elapsed days");

    Expect(!CalculateCodexCostSummary(events, "2023-02-29"),
           "non-leap reference date is rejected");
}

void TestInvalidFutureAndMalformedEvents() {
    const std::vector<CodexCostEvent> events{
        Event("future", "2026-08-12", "gpt-5.6-luna", 100, 100),
        Event("bad-date", "2026-02-29", "gpt-5.6-luna", 100, 100),
        Event("short-date", "2026-8-11", "gpt-5.6-luna", 100, 100),
        Event("", "2026-08-11", "gpt-5.6-luna", 100, 100),
        Event("negative", "2026-08-11", "gpt-5.6-luna", -100, -100),
        Event("valid", "2026-08-11", "gpt-5.6-luna", 100, 100),
    };
    const CodexCostSummary result = Require(
        CalculateCodexCostSummary(events, "2026-08-11"),
        "invalid event result");
    Expect(result.eventCount == 1 && result.last30Days.tokens == 200,
           "invalid, future, empty, and zero-token events are ignored");
}

void TestInvalidDuplicateDoesNotPoisonValidEvent() {
    const std::vector<CodexCostEvent> events{
        Event("same", "bad-date", "gpt-5.6-luna", 100, 100),
        Event("same", "2026-08-11", "gpt-5.6-luna", 300, 200),
    };
    const CodexCostSummary result = Require(
        CalculateCodexCostSummary(events, "2026-08-11"),
        "valid duplicate result");
    Expect(result.eventCount == 1 && result.today.tokens == 500,
           "invalid rows do not reserve a fingerprint");
}

void TestCachedValueValidationAndClipping() {
    CodexCostEvent event =
        Event("cached", "2026-08-11", "unknown", 80, 20);
    event.cachedEstimatedUsd = std::numeric_limits<double>::quiet_NaN();
    event.cachedPricedTokens = 1000;
    const CodexCostSummary result = Require(
        CalculateCodexCostSummary({event}, "2026-08-11"),
        "cached validation result");
    Expect(result.pricedTokens == 0 &&
               result.pricedTokenPercent == 0.0,
           "an invalid cached cost also invalidates its priced-token count");
    Expect(result.today.estimatedUsd == 0.0,
           "invalid cached cost is ignored for an unknown model");

    event.model = "gpt-5.6-luna";
    const CodexCostSummary fallback = Require(
        CalculateCodexCostSummary({event}, "2026-08-11"),
        "cached known-model fallback result");
    Expect(fallback.pricedTokens == 100 &&
               fallback.pricedTokenPercent == 100.0 &&
               fallback.today.estimatedUsd > 0.0,
           "invalid cached pricing falls back atomically to the current known-model estimate");
}

void TestSaturation() {
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    CodexCostEvent first =
        Event("max-a", "2026-08-11", "unknown-a", maximum, maximum);
    first.cachedEstimatedUsd = std::numeric_limits<double>::max();
    first.cachedPricedTokens = maximum;
    CodexCostEvent second =
        Event("max-b", "2026-08-11", "unknown-a", maximum, maximum);
    second.cachedEstimatedUsd = std::numeric_limits<double>::max();
    second.cachedPricedTokens = maximum;

    const CodexCostSummary result = Require(
        CalculateCodexCostSummary({first, second}, "2026-08-11"),
        "saturation result");
    Expect(result.saturated, "integer and cost overflow sets saturation");
    Expect(result.today.tokens == maximum &&
               result.last30Days.tokens == maximum &&
               result.monthToDate.tokens == maximum,
           "period token totals clamp at int64 maximum");
    Expect(result.today.estimatedUsd == std::numeric_limits<double>::max() &&
               result.monthForecastEstimatedUsd ==
                   std::numeric_limits<double>::max(),
           "cost sums and forecast clamp at double maximum");
    Expect(result.topModelTokens == maximum,
           "top-model sum clamps at int64 maximum");
    ExpectNear(result.pricedTokenPercent, 50.0, 1e-12,
               "coverage uses unclamped event totals when exposed sums saturate");
}

void TestAliasesAggregateIntoOneTopModel() {
    const std::vector<CodexCostEvent> events{
        Event("alias-a", "2026-08-11", "openai/gpt-5.6", 500, 0),
        Event("alias-b", "2026-08-10", "gpt-5.6-sol", 600, 0),
        Event("other", "2026-08-10", "gpt-5.6-terra", 1000, 0),
    };
    const CodexCostSummary result = Require(
        CalculateCodexCostSummary(events, "2026-08-11"),
        "alias aggregation result");
    Expect(result.topModel == "gpt-5.6-sol" &&
               result.topModelTokens == 1100,
           "normalized aliases aggregate into one model");
}

}  // namespace

int main() {
    TestWindowsAndDuplicateFingerprint();
    TestUnknownModelAndCachedHistory();
    TestLeapYearMonthBoundaryAndForecast();
    TestInvalidFutureAndMalformedEvents();
    TestInvalidDuplicateDoesNotPoisonValidEvent();
    TestCachedValueValidationAndClipping();
    TestSaturation();
    TestAliasesAggregateIntoOneTopModel();
    std::cout << "codex_cost_summary_test: pass\n";
    return 0;
}
