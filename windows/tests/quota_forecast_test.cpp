#include "codex/quota_forecast.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using codex_monitor::codex::CalculateQuotaForecast;
using codex_monitor::codex::QuotaForecast;
using codex_monitor::codex::QuotaForecastConfidence;
using codex_monitor::codex::QuotaForecastSample;
using codex_monitor::codex::QuotaForecastState;
using codex_monitor::codex::QuotaForecastUnavailableReason;

constexpr double kNow = 1770000000.0;
constexpr double kReset = kNow + 4.0 * 3600.0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectNear(double actual, double expected, double tolerance,
                const char* message) {
    Expect(std::isfinite(actual) && std::fabs(actual - expected) <= tolerance,
           message);
}

QuotaForecast Forecast(std::vector<QuotaForecastSample> samples,
                       double remaining = 40.0,
                       double reset = kReset,
                       double now = kNow) {
    return CalculateQuotaForecast(samples, remaining, reset, now);
}

void TestInsufficientHistory() {
    const QuotaForecast tooFew = Forecast({
        {kNow - 1800.0, 60.0, kReset},
        {kNow, 40.0, kReset},
    });
    Expect(!tooFew.available() && tooFew.sampleCount == 2,
           "two points are insufficient");
    Expect(tooFew.unavailableReason ==
               QuotaForecastUnavailableReason::kInsufficientHistory,
           "too few points report history reason");

    const QuotaForecast tooShort = Forecast({
        {kNow - 899.0, 60.0, kReset},
        {kNow - 400.0, 50.0, kReset},
        {kNow, 40.0, kReset},
    });
    Expect(!tooShort.available() && tooShort.sampleCount == 3,
           "three points under fifteen minutes are insufficient");
}

void TestResetStartsANewCycle() {
    const double previousReset = kReset - 5.0 * 3600.0;
    const QuotaForecast result = Forecast({
        {kNow - 7200.0, 90.0, previousReset},
        {kNow - 5400.0, 80.0, previousReset},
        {kNow - 3600.0, 70.0, previousReset},
        {kNow - 900.0, 50.0, kReset},
        {kNow, 40.0, kReset},
    });
    Expect(!result.available() && result.sampleCount == 2,
           "previous reset cycle must not contribute");
}

void TestStableUsage() {
    const QuotaForecast result = Forecast({
        {kNow - 1800.0, 80.0, kReset},
        {kNow - 900.0, 80.0, kReset},
        {kNow, 80.0, kReset},
    }, 80.0);
    Expect(result.state == QuotaForecastState::kStable,
           "flat history is stable");
    Expect(result.confidence == QuotaForecastConfidence::kLow,
           "short valid history has low confidence");
    ExpectNear(result.consumptionPercentPerHour, 0.0, 1e-12,
               "stable rate is zero");
    Expect(result.projectedRemainingAtResetPercent.has_value() &&
               !result.projectedExhaustAtUnixSeconds.has_value(),
           "stable result exposes remaining but no exhaustion time");
    ExpectNear(*result.projectedRemainingAtResetPercent, 80.0, 1e-12,
               "stable projection mirrors current remaining");
}

void TestLastsToResetAndMediumConfidence() {
    const QuotaForecast result = Forecast({
        {kNow - 3600.0, 80.0, kReset},
        {kNow - 2880.0, 79.0, kReset},
        {kNow - 2160.0, 78.0, kReset},
        {kNow - 1440.0, 77.0, kReset},
        {kNow - 720.0, 76.0, kReset},
        {kNow, 75.0, kReset},
    }, 75.0);
    Expect(result.state == QuotaForecastState::kLastsToReset,
           "positive reset projection lasts to reset");
    Expect(result.confidence == QuotaForecastConfidence::kMedium,
           "six samples over one hour have medium confidence");
    ExpectNear(result.consumptionPercentPerHour, 5.0, 1e-10,
               "linear rate is five percent per hour");
    ExpectNear(*result.projectedRemainingAtResetPercent, 55.0, 1e-9,
               "reset projection uses current remaining");
    ExpectNear(*result.projectedExhaustAtUnixSeconds,
               kNow + 15.0 * 3600.0, 1e-6,
               "exhaustion timestamp follows current rate");
}

void TestMayExhaustEarly() {
    const QuotaForecast result = Forecast({
        {kNow - 1800.0, 60.0, kReset},
        {kNow - 900.0, 50.0, kReset},
        {kNow, 40.0, kReset},
    });
    Expect(result.state == QuotaForecastState::kMayExhaustEarly,
           "negative reset projection warns of early exhaustion");
    Expect(result.confidence == QuotaForecastConfidence::kLow,
           "three samples have low confidence");
    ExpectNear(result.consumptionPercentPerHour, 40.0, 1e-10,
               "rapid usage rate");
    ExpectNear(*result.projectedRemainingAtResetPercent, -120.0, 1e-9,
               "early exhaustion projected remaining");
    ExpectNear(*result.projectedExhaustAtUnixSeconds, kNow + 3600.0, 1e-6,
               "early exhaustion timestamp");
}

void TestHighConfidence() {
    std::vector<QuotaForecastSample> samples;
    for (int index = 0; index < 12; ++index) {
        const double hours = 6.0 * static_cast<double>(index) / 11.0;
        samples.push_back(
            {kNow - 6.0 * 3600.0 + hours * 3600.0,
             90.0 - 2.0 * hours, kReset});
    }
    const QuotaForecast result = Forecast(samples, 78.0);
    Expect(result.available() &&
               result.confidence == QuotaForecastConfidence::kHigh,
           "twelve points over six hours have high confidence");
    ExpectNear(result.consumptionPercentPerHour, 2.0, 1e-10,
               "high-confidence rate");
}

void TestUnorderedBadAndOutOfWindowSamples() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const QuotaForecast result = Forecast({
        {kNow, 40.0, kReset},
        {nan, 55.0, kReset},
        {kNow - 900.0, 50.0, kReset + 300.0},
        {kNow - 1800.0, 60.0, kReset},
        {kNow - 400.0, infinity, kReset},
        {kNow - 300.0, -1.0, kReset},
        {kNow - 300.0, 101.0, kReset},
        {kNow - 300.0, 45.0, infinity},
        {kNow - 300.0, 45.0, kReset + 301.0},
        {kNow - 24.0 * 3600.0 - 1.0, 90.0, kReset},
        {kNow + 61.0, 39.0, kReset},
    });
    Expect(result.state == QuotaForecastState::kMayExhaustEarly &&
               result.sampleCount == 3,
           "unordered valid points survive while bad points are ignored");
    ExpectNear(result.consumptionPercentPerHour, 40.0, 1e-10,
               "sorting restores correct regression");
}

void TestWindowAndClockSkewBoundaries() {
    const QuotaForecast result = Forecast({
        {kNow - 24.0 * 3600.0, 60.0, kReset - 300.0},
        {kNow - 840.0, 50.0, kReset},
        {kNow + 60.0, 40.0, kReset + 300.0},
    });
    Expect(result.available() && result.sampleCount == 3,
           "inclusive age skew and reset tolerance boundaries are accepted");
}

void TestInvalidAndOverflowingCurrentState() {
    const std::vector<QuotaForecastSample> samples = {
        {kNow - 1800.0, 60.0, kReset},
        {kNow - 900.0, 50.0, kReset},
        {kNow, 40.0, kReset},
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Expect(!CalculateQuotaForecast(samples, nan, kReset, kNow).available(),
           "NaN current remaining is rejected");
    Expect(!CalculateQuotaForecast(samples, 101.0, kReset, kNow).available(),
           "out-of-range current remaining is rejected");
    Expect(!CalculateQuotaForecast(samples, 40.0, kNow, kNow).available(),
           "elapsed reset is rejected");

    const double maximum = std::numeric_limits<double>::max();
    const QuotaForecast overflow =
        CalculateQuotaForecast({}, 40.0, maximum, -maximum);
    Expect(!overflow.available() &&
               overflow.unavailableReason ==
                   QuotaForecastUnavailableReason::kUnsafeArithmetic,
           "overflowing reset interval is rejected without infinity");
}

}  // namespace

int main() {
    TestInsufficientHistory();
    TestResetStartsANewCycle();
    TestStableUsage();
    TestLastsToResetAndMediumConfidence();
    TestMayExhaustEarly();
    TestHighConfidence();
    TestUnorderedBadAndOutOfWindowSamples();
    TestWindowAndClockSkewBoundaries();
    TestInvalidAndOverflowingCurrentState();
    std::cout << "quota_forecast_test: pass\n";
    return 0;
}
