#include "codex/quota_forecast.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace codex_monitor::codex {
namespace {

constexpr double kResetCycleToleranceSeconds = 300.0;
constexpr double kHistoryWindowSeconds = 24.0 * 60.0 * 60.0;
constexpr double kFutureClockSkewSeconds = 60.0;
constexpr double kMinimumHistorySpanSeconds = 15.0 * 60.0;
constexpr double kStableRatePercentPerHour = 0.02;

[[nodiscard]] bool IsValidPercentage(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

[[nodiscard]] bool IsRepresentableDouble(long double value) noexcept {
    const long double limit =
        static_cast<long double>(std::numeric_limits<double>::max());
    return std::isfinite(value) && value >= -limit && value <= limit;
}

QuotaForecast Unavailable(QuotaForecastUnavailableReason reason) noexcept {
    QuotaForecast result;
    result.unavailableReason = reason;
    return result;
}

}  // namespace

QuotaForecast CalculateQuotaForecast(
    const std::vector<QuotaForecastSample>& samples,
    double currentRemainingPercent,
    double currentResetAtUnixSeconds,
    double nowUnixSeconds) {
    if (!IsValidPercentage(currentRemainingPercent) ||
        !std::isfinite(currentResetAtUnixSeconds) ||
        !std::isfinite(nowUnixSeconds)) {
        return Unavailable(
            QuotaForecastUnavailableReason::kInvalidCurrentState);
    }

    const double secondsToReset = currentResetAtUnixSeconds - nowUnixSeconds;
    if (!std::isfinite(secondsToReset)) {
        return Unavailable(QuotaForecastUnavailableReason::kUnsafeArithmetic);
    }
    if (secondsToReset <= 0.0) {
        return Unavailable(
            QuotaForecastUnavailableReason::kInvalidCurrentState);
    }

    std::vector<QuotaForecastSample> valid;
    valid.reserve(samples.size());
    for (const QuotaForecastSample& sample : samples) {
        if (!std::isfinite(sample.timestamp) ||
            !IsValidPercentage(sample.remainingPercent) ||
            !std::isfinite(sample.resetAt)) {
            continue;
        }

        const double resetDifference =
            sample.resetAt - currentResetAtUnixSeconds;
        const double age = nowUnixSeconds - sample.timestamp;
        if (!std::isfinite(resetDifference) || !std::isfinite(age) ||
            std::fabs(resetDifference) > kResetCycleToleranceSeconds ||
            age > kHistoryWindowSeconds || age < -kFutureClockSkewSeconds) {
            continue;
        }
        valid.push_back(sample);
    }

    std::sort(valid.begin(), valid.end(),
              [](const QuotaForecastSample& left,
                 const QuotaForecastSample& right) noexcept {
                  return left.timestamp < right.timestamp;
              });

    if (valid.size() < 3) {
        QuotaForecast result =
            Unavailable(QuotaForecastUnavailableReason::kInsufficientHistory);
        result.sampleCount = valid.size();
        return result;
    }

    const double spanSeconds = valid.back().timestamp - valid.front().timestamp;
    if (!std::isfinite(spanSeconds)) {
        return Unavailable(QuotaForecastUnavailableReason::kUnsafeArithmetic);
    }
    if (spanSeconds < kMinimumHistorySpanSeconds) {
        QuotaForecast result =
            Unavailable(QuotaForecastUnavailableReason::kInsufficientHistory);
        result.sampleCount = valid.size();
        result.sampleSpanSeconds = spanSeconds;
        return result;
    }

    // Online covariance keeps the regression stable without summing absolute
    // Unix timestamps or relying on potentially overflowing double totals.
    long double meanX = 0.0L;
    long double meanY = 0.0L;
    long double sumXX = 0.0L;
    long double sumXY = 0.0L;
    std::size_t count = 0;
    const long double firstTimestamp =
        static_cast<long double>(valid.front().timestamp);
    for (const QuotaForecastSample& sample : valid) {
        const long double x =
            (static_cast<long double>(sample.timestamp) - firstTimestamp) /
            3600.0L;
        const long double y =
            static_cast<long double>(sample.remainingPercent);
        ++count;
        const long double divisor = static_cast<long double>(count);
        const long double deltaX = x - meanX;
        const long double deltaY = y - meanY;
        meanX += deltaX / divisor;
        meanY += deltaY / divisor;
        sumXX += deltaX * (x - meanX);
        sumXY += deltaX * (y - meanY);
    }

    if (!std::isfinite(sumXX) || !std::isfinite(sumXY) || sumXX <= 0.0L) {
        return Unavailable(QuotaForecastUnavailableReason::kUnsafeArithmetic);
    }
    const long double slope = sumXY / sumXX;
    const long double rateLong = std::max(0.0L, -slope);
    if (!IsRepresentableDouble(rateLong)) {
        return Unavailable(QuotaForecastUnavailableReason::kUnsafeArithmetic);
    }

    QuotaForecast result;
    result.unavailableReason = QuotaForecastUnavailableReason::kNone;
    result.sampleCount = valid.size();
    result.sampleSpanSeconds = spanSeconds;
    result.consumptionPercentPerHour = static_cast<double>(rateLong);
    if (spanSeconds >= 6.0 * 3600.0 && valid.size() >= 12) {
        result.confidence = QuotaForecastConfidence::kHigh;
    } else if (spanSeconds >= 3600.0 && valid.size() >= 6) {
        result.confidence = QuotaForecastConfidence::kMedium;
    } else {
        result.confidence = QuotaForecastConfidence::kLow;
    }

    if (result.consumptionPercentPerHour <
        kStableRatePercentPerHour) {
        result.state = QuotaForecastState::kStable;
        result.projectedRemainingAtResetPercent = currentRemainingPercent;
        return result;
    }

    const long double projectedRemaining =
        static_cast<long double>(currentRemainingPercent) -
        rateLong * static_cast<long double>(secondsToReset) / 3600.0L;
    const long double secondsUntilExhaustion =
        static_cast<long double>(currentRemainingPercent) / rateLong * 3600.0L;
    const long double exhaustAt =
        static_cast<long double>(nowUnixSeconds) + secondsUntilExhaustion;
    if (!IsRepresentableDouble(projectedRemaining) ||
        !IsRepresentableDouble(exhaustAt)) {
        return Unavailable(QuotaForecastUnavailableReason::kUnsafeArithmetic);
    }

    result.projectedRemainingAtResetPercent =
        static_cast<double>(projectedRemaining);
    result.projectedExhaustAtUnixSeconds = static_cast<double>(exhaustAt);
    result.state = projectedRemaining > 0.0L
                       ? QuotaForecastState::kLastsToReset
                       : QuotaForecastState::kMayExhaustEarly;
    return result;
}

}  // namespace codex_monitor::codex
