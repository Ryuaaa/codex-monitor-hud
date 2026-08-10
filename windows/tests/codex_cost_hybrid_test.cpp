#include "codex/codex_cost_hybrid.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using codex_monitor::codex::CalculateCodexCostHybridSummary;
using codex_monitor::codex::CodexCostHybridSummary;
using codex_monitor::codex::CodexCostSummary;
using codex_monitor::codex::UsageCalendarTotals;

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

CodexCostSummary LocalSample() {
    CodexCostSummary local;
    local.available = true;
    local.today = {100, 2.0};
    local.last7Days = {1000, 10.0};
    local.last30Days = {3000, 24.0};
    local.monthToDate = {2000, 20.0};
    local.monthForecastEstimatedUsd = 50.0;
    local.pricedTokenPercent = 62.5;
    local.topModel = "gpt-5.6-sol";
    local.topModelTokens = 2400;
    return local;
}

UsageCalendarTotals OfficialSample() {
    UsageCalendarTotals official;
    official.sourceAvailable = true;
    official.todayAvailable = true;
    official.todayTokens = 200;
    official.last7DaysTokens = 2000;
    official.thirtyDayTokens = 6000;
    official.monthToDateTokens = 3000;
    official.monthForecastTokens = 9000;
    return official;
}

void TestOfficialTotalsAndPeriodPrices() {
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(OfficialSample(), LocalSample());
    Expect(result.available && result.usedOfficialTotals,
           "official result is available and marked");
    Expect(result.today.usedOfficialTokens && result.today.tokens == 200,
           "official today total wins");
    Expect(result.last7Days.usedOfficialTokens &&
               result.last7Days.tokens == 2000,
           "official seven-day total wins");
    Expect(result.last30Days.tokens == 6000 &&
               result.monthToDate.tokens == 3000,
           "official 30-day and month totals win");
    ExpectNear(*result.today.estimatedUsd, 4.0,
               "today uses today's local average price");
    ExpectNear(*result.last7Days.estimatedUsd, 20.0,
               "seven days use seven-day average price");
    ExpectNear(*result.last30Days.estimatedUsd, 48.0,
               "30 days use 30-day average price");
    ExpectNear(*result.monthToDate.estimatedUsd, 30.0,
               "month uses month average price");
    Expect(result.monthForecastUsedOfficialTokens,
           "official month token forecast is preferred");
    ExpectNear(*result.monthForecastEstimatedUsd, 90.0,
               "official month forecast is scaled by local month price");
    ExpectNear(result.pricedTokenPercent, 62.5,
               "unknown-model price coverage is preserved");
    Expect(result.topModel == "gpt-5.6-sol" &&
               result.topModelTokens == 2400,
           "local top model is preserved");
}

void TestMissingOfficialTodayFallsBackLocally() {
    UsageCalendarTotals official = OfficialSample();
    official.todayAvailable = false;
    official.todayTokens.reset();
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(official, LocalSample());
    Expect(result.today.tokensAvailable && !result.today.usedOfficialTokens &&
               result.today.tokens == 100,
           "missing official today uses local today");
    ExpectNear(*result.today.estimatedUsd, 2.0,
               "local fallback keeps its frozen estimate");
    Expect(result.last7Days.usedOfficialTokens,
           "other official periods remain independent");
}

void TestAllOfficialDataMissingFallsBackLocally() {
    UsageCalendarTotals official;
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(official, LocalSample());
    Expect(result.available && !result.usedOfficialTotals,
           "local-only result remains available");
    Expect(result.today.tokens == 100 && result.last7Days.tokens == 1000 &&
               result.last30Days.tokens == 3000 &&
               result.monthToDate.tokens == 2000,
           "all period tokens fall back locally");
    ExpectNear(*result.last30Days.estimatedUsd, 24.0,
               "local cost remains unchanged without scaling");
    Expect(!result.monthForecastUsedOfficialTokens,
           "local month forecast is identified");
    ExpectNear(*result.monthForecastEstimatedUsd, 50.0,
               "local month forecast is the fallback");
}

void TestOfficialTokensWithoutLocalPriceSample() {
    CodexCostSummary local;
    UsageCalendarTotals official = OfficialSample();
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(official, local);
    Expect(result.available && result.usedOfficialTotals,
           "official tokens remain available without local history");
    Expect(result.today.tokens == 200 && !result.today.estimatedUsd,
           "cost is unavailable without a local sample");
    Expect(!result.monthForecastEstimatedUsd &&
               !result.monthForecastUsedOfficialTokens,
           "month cost forecast is unavailable without local price");
}

void TestZeroLocalTokensCannotEstimate() {
    CodexCostSummary local = LocalSample();
    local.today = {0, 0.0};
    local.monthToDate = {0, 0.0};
    local.monthForecastEstimatedUsd = 25.0;
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(OfficialSample(), local);
    Expect(result.today.tokens == 200 && !result.today.estimatedUsd,
           "zero local today tokens cannot infer a unit price");
    Expect(!result.monthForecastEstimatedUsd,
           "zero local month tokens reject both forecast paths");
}

void TestMissingOfficialForecastUsesLocalForecast() {
    UsageCalendarTotals official = OfficialSample();
    official.monthForecastTokens.reset();
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(official, LocalSample());
    Expect(result.monthForecastEstimatedUsd.has_value() &&
               !result.monthForecastUsedOfficialTokens,
           "missing official forecast selects local forecast");
    ExpectNear(*result.monthForecastEstimatedUsd, 50.0,
               "local forecast value is retained");
}

void TestInvalidOfficialTotalsFallBack() {
    UsageCalendarTotals official = OfficialSample();
    official.todayTokens = -1;
    official.last7DaysTokens = -1;
    official.monthForecastTokens = -1;
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(official, LocalSample());
    Expect(!result.today.usedOfficialTokens && result.today.tokens == 100,
           "negative official today is rejected");
    Expect(!result.last7Days.usedOfficialTokens &&
               result.last7Days.tokens == 1000,
           "negative official period is rejected");
    Expect(!result.monthForecastUsedOfficialTokens &&
               result.monthForecastEstimatedUsd == 50.0,
           "negative official forecast falls back locally");
}

void TestOverflowAndNonFiniteValues() {
    CodexCostSummary local = LocalSample();
    local.today = {1, std::numeric_limits<double>::max()};
    local.monthToDate = {1, std::numeric_limits<double>::max()};
    local.pricedTokenPercent = std::numeric_limits<double>::quiet_NaN();
    UsageCalendarTotals official = OfficialSample();
    official.todayTokens = std::numeric_limits<std::int64_t>::max();
    official.monthForecastTokens = std::numeric_limits<std::int64_t>::max();

    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary(official, local);
    Expect(result.saturated, "scaled double overflow is reported");
    Expect(result.today.estimatedUsd == std::numeric_limits<double>::max() &&
               result.monthForecastEstimatedUsd ==
                   std::numeric_limits<double>::max(),
           "overflowing costs clamp at double maximum");
    Expect(std::isfinite(result.pricedTokenPercent) &&
               result.pricedTokenPercent == 0.0,
           "non-finite coverage never escapes the model");
    Expect(result.topModel == "gpt-5.6-sol" &&
               result.topModelTokens == 2400,
           "invalid coverage does not discard independent top-model data");
}

void TestNoSourcesIsUnavailable() {
    const CodexCostHybridSummary result =
        CalculateCodexCostHybridSummary({}, {});
    Expect(!result.available && !result.usedOfficialTotals &&
               !result.today.tokensAvailable &&
               !result.monthForecastEstimatedUsd,
           "no official or local source is unavailable");
}

}  // namespace

int main() {
    TestOfficialTotalsAndPeriodPrices();
    TestMissingOfficialTodayFallsBackLocally();
    TestAllOfficialDataMissingFallsBackLocally();
    TestOfficialTokensWithoutLocalPriceSample();
    TestZeroLocalTokensCannotEstimate();
    TestMissingOfficialForecastUsesLocalForecast();
    TestInvalidOfficialTotalsFallBack();
    TestOverflowAndNonFiniteValues();
    TestNoSourcesIsUnavailable();
    std::cout << "codex_cost_hybrid_test: pass\n";
    return 0;
}
