#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace codex_monitor::codex {

// All timestamps are Unix seconds and all percentages are in the inclusive
// range [0, 100]. Invalid samples are ignored by CalculateQuotaForecast.
struct QuotaForecastSample {
    double timestamp = 0.0;
    double remainingPercent = 0.0;
    double resetAt = 0.0;
};

enum class QuotaForecastState {
    kUnavailable,
    kStable,
    kLastsToReset,
    kMayExhaustEarly,
};

enum class QuotaForecastConfidence {
    kUnavailable,
    kLow,
    kMedium,
    kHigh,
};

enum class QuotaForecastUnavailableReason {
    kNone,
    kInvalidCurrentState,
    kInsufficientHistory,
    kUnsafeArithmetic,
};

struct QuotaForecast {
    QuotaForecastState state = QuotaForecastState::kUnavailable;
    QuotaForecastConfidence confidence =
        QuotaForecastConfidence::kUnavailable;
    QuotaForecastUnavailableReason unavailableReason =
        QuotaForecastUnavailableReason::kInsufficientHistory;

    // The number and span of samples retained after validation and filtering.
    std::size_t sampleCount = 0;
    double sampleSpanSeconds = 0.0;

    double consumptionPercentPerHour = 0.0;
    std::optional<double> projectedRemainingAtResetPercent;
    std::optional<double> projectedExhaustAtUnixSeconds;

    [[nodiscard]] bool available() const noexcept {
        return state != QuotaForecastState::kUnavailable;
    }
};

// Mirrors the validated macOS forecast policy:
// - samples must belong to the current reset cycle within 300 seconds;
// - only the latest 24 hours are used, with at most 60 seconds of clock skew;
// - at least 3 samples spanning at least 15 minutes are required;
// - consumption is the non-negative slope from a linear regression;
// - confidence is high at 12 samples/6 hours, medium at 6 samples/1 hour,
//   and low otherwise.
//
// The function has no platform dependencies and never returns NaN or infinity.
QuotaForecast CalculateQuotaForecast(
    const std::vector<QuotaForecastSample>& samples,
    double currentRemainingPercent,
    double currentResetAtUnixSeconds,
    double nowUnixSeconds);

}  // namespace codex_monitor::codex
