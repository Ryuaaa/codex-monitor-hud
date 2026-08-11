#include "codex/weekly_quota_alert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::int64_t kMaximumUnixSeconds = 253402300799LL;
constexpr std::int64_t kRollingWindowSeconds = 24LL * 60LL * 60LL;
constexpr std::int64_t kMaximumNaturalDaySeconds = 26LL * 60LL * 60LL;
constexpr std::int64_t kMaximumBaselineLatenessSeconds = 30LL * 60LL;
constexpr std::int64_t kMinimumBaselineAgeSeconds = 60;
constexpr double kPercentComparisonTolerance = 1e-9;

struct Observation {
    std::int64_t capturedAtUnixSeconds = 0;
    double remainingPercent = 0.0;
};

[[nodiscard]] bool IsValidUnixSeconds(std::int64_t value) noexcept {
    return value >= 0 && value <= kMaximumUnixSeconds;
}

[[nodiscard]] bool IsValidPercent(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

[[nodiscard]] bool IsValidMode(WeeklyQuotaAlertMode mode) noexcept {
    return mode == WeeklyQuotaAlertMode::kNaturalDay ||
           mode == WeeklyQuotaAlertMode::kRolling24Hours;
}

[[nodiscard]] bool IsSameCycleSample(
    const QuotaHistorySample& sample,
    std::int64_t resetAtUnixSeconds,
    std::int64_t nowUnixSeconds) noexcept {
    return IsValidUnixSeconds(sample.capturedAtUnixSeconds) &&
           sample.capturedAtUnixSeconds <= nowUnixSeconds &&
           sample.weekly.remainingPercent.has_value() &&
           IsValidPercent(*sample.weekly.remainingPercent) &&
           sample.weekly.resetsAtUnixSeconds.has_value() &&
           *sample.weekly.resetsAtUnixSeconds == resetAtUnixSeconds;
}

[[nodiscard]] WeeklyQuotaAlertEvaluation Finish(
    WeeklyQuotaAlertOutcome outcome,
    WeeklyQuotaAlertState state) noexcept {
    WeeklyQuotaAlertEvaluation result;
    result.outcome = outcome;
    result.nextState = std::move(state);
    return result;
}

}  // namespace

bool IsValidWeeklyQuotaAlertPolicy(
    const WeeklyQuotaAlertPolicy& policy) noexcept {
    return IsValidMode(policy.mode) &&
           std::isfinite(policy.thresholdPercent) &&
           policy.thresholdPercent >= 5.0 &&
           policy.thresholdPercent <= 100.0;
}

bool IsValidWeeklyQuotaAlertState(
    const WeeklyQuotaAlertState& state) noexcept {
    if (!IsValidMode(state.mode)) return false;

    const bool hasReset = state.weeklyResetAtUnixSeconds.has_value();
    const bool hasEvaluation = state.lastEvaluatedAtUnixSeconds.has_value();
    const bool hasNotifiedPeriod =
        state.lastNotifiedPeriodStartUnixSeconds.has_value();
    const bool hasNotifiedAt = state.lastNotifiedAtUnixSeconds.has_value();

    if (!hasReset) {
        return !hasEvaluation && !hasNotifiedPeriod && !hasNotifiedAt;
    }
    if (!hasEvaluation || hasNotifiedPeriod != hasNotifiedAt ||
        !IsValidUnixSeconds(*state.weeklyResetAtUnixSeconds) ||
        !IsValidUnixSeconds(*state.lastEvaluatedAtUnixSeconds) ||
        *state.lastEvaluatedAtUnixSeconds >=
            *state.weeklyResetAtUnixSeconds) {
        return false;
    }
    if (!hasNotifiedAt) return true;

    return IsValidUnixSeconds(*state.lastNotifiedPeriodStartUnixSeconds) &&
           IsValidUnixSeconds(*state.lastNotifiedAtUnixSeconds) &&
           *state.lastNotifiedPeriodStartUnixSeconds <=
               *state.lastNotifiedAtUnixSeconds &&
           *state.lastNotifiedAtUnixSeconds <=
               *state.lastEvaluatedAtUnixSeconds &&
           *state.lastNotifiedAtUnixSeconds <
               *state.weeklyResetAtUnixSeconds;
}

WeeklyQuotaAlertEvaluation EvaluateWeeklyQuotaAlert(
    const std::vector<QuotaHistorySample>& history,
    const WeeklyQuotaAlertPolicy& policy,
    double currentWeeklyRemainingPercent,
    std::int64_t currentWeeklyResetAtUnixSeconds,
    std::int64_t nowUnixSeconds,
    std::optional<std::int64_t> localDayStartUnixSeconds,
    const WeeklyQuotaAlertState& priorState) noexcept {
    try {
        if (!IsValidWeeklyQuotaAlertPolicy(policy) ||
            !IsValidWeeklyQuotaAlertState(priorState)) {
            return Finish(WeeklyQuotaAlertOutcome::kInvalidInput, priorState);
        }
        if (!policy.enabled) {
            return Finish(WeeklyQuotaAlertOutcome::kDisabled, priorState);
        }
        if (!IsValidPercent(currentWeeklyRemainingPercent) ||
            !IsValidUnixSeconds(nowUnixSeconds) ||
            !IsValidUnixSeconds(currentWeeklyResetAtUnixSeconds) ||
            currentWeeklyResetAtUnixSeconds <= nowUnixSeconds) {
            return Finish(WeeklyQuotaAlertOutcome::kInvalidInput, priorState);
        }
        if (priorState.lastEvaluatedAtUnixSeconds &&
            nowUnixSeconds < *priorState.lastEvaluatedAtUnixSeconds) {
            return Finish(WeeklyQuotaAlertOutcome::kClockMovedBackward,
                          priorState);
        }

        std::int64_t periodStartUnixSeconds = 0;
        if (policy.mode == WeeklyQuotaAlertMode::kNaturalDay) {
            if (!localDayStartUnixSeconds ||
                !IsValidUnixSeconds(*localDayStartUnixSeconds) ||
                *localDayStartUnixSeconds > nowUnixSeconds ||
                nowUnixSeconds - *localDayStartUnixSeconds >
                    kMaximumNaturalDaySeconds) {
                return Finish(WeeklyQuotaAlertOutcome::kInvalidInput,
                              priorState);
            }
            periodStartUnixSeconds = *localDayStartUnixSeconds;
        } else {
            if (nowUnixSeconds < kRollingWindowSeconds) {
                return Finish(WeeklyQuotaAlertOutcome::kInsufficientHistory,
                              priorState);
            }
            periodStartUnixSeconds = nowUnixSeconds - kRollingWindowSeconds;
        }

        WeeklyQuotaAlertState nextState = priorState;
        const bool sameCycle =
            priorState.weeklyResetAtUnixSeconds &&
            *priorState.weeklyResetAtUnixSeconds ==
                currentWeeklyResetAtUnixSeconds &&
            priorState.mode == policy.mode;
        if (!sameCycle) {
            nextState = {};
            nextState.weeklyResetAtUnixSeconds =
                currentWeeklyResetAtUnixSeconds;
            nextState.mode = policy.mode;
        }
        nextState.lastEvaluatedAtUnixSeconds = nowUnixSeconds;

        bool alreadyNotified = false;
        if (sameCycle && nextState.lastNotifiedAtUnixSeconds) {
            if (policy.mode == WeeklyQuotaAlertMode::kNaturalDay) {
                alreadyNotified =
                    nextState.lastNotifiedPeriodStartUnixSeconds &&
                    *nextState.lastNotifiedPeriodStartUnixSeconds ==
                        periodStartUnixSeconds;
            } else {
                const std::int64_t notifiedAt =
                    *nextState.lastNotifiedAtUnixSeconds;
                alreadyNotified = nowUnixSeconds >= notifiedAt &&
                                  nowUnixSeconds - notifiedAt <
                                      kRollingWindowSeconds;
            }
        }
        if (alreadyNotified) {
            WeeklyQuotaAlertEvaluation result = Finish(
                WeeklyQuotaAlertOutcome::kAlreadyNotified, nextState);
            result.periodStartUnixSeconds = periodStartUnixSeconds;
            return result;
        }

        std::optional<Observation> baseline;
        for (const QuotaHistorySample& sample : history) {
            if (!IsSameCycleSample(sample, currentWeeklyResetAtUnixSeconds,
                                   nowUnixSeconds) ||
                sample.capturedAtUnixSeconds < periodStartUnixSeconds ||
                sample.capturedAtUnixSeconds - periodStartUnixSeconds >
                    kMaximumBaselineLatenessSeconds) {
                continue;
            }
            if (!baseline || sample.capturedAtUnixSeconds <
                                 baseline->capturedAtUnixSeconds) {
                baseline = Observation{sample.capturedAtUnixSeconds,
                                       *sample.weekly.remainingPercent};
            }
        }

        if (!baseline || baseline->capturedAtUnixSeconds >= nowUnixSeconds ||
            nowUnixSeconds - baseline->capturedAtUnixSeconds <
                kMinimumBaselineAgeSeconds) {
            WeeklyQuotaAlertEvaluation result = Finish(
                WeeklyQuotaAlertOutcome::kInsufficientHistory, nextState);
            result.periodStartUnixSeconds = periodStartUnixSeconds;
            return result;
        }

        std::vector<Observation> observations;
        observations.reserve(history.size() + 1);
        for (const QuotaHistorySample& sample : history) {
            if (IsSameCycleSample(sample, currentWeeklyResetAtUnixSeconds,
                                  nowUnixSeconds) &&
                sample.capturedAtUnixSeconds >=
                    baseline->capturedAtUnixSeconds) {
                observations.push_back(
                    {sample.capturedAtUnixSeconds,
                     *sample.weekly.remainingPercent});
            }
        }
        observations.push_back({nowUnixSeconds,
                                currentWeeklyRemainingPercent});
        std::sort(observations.begin(), observations.end(),
                  [](const Observation& left,
                     const Observation& right) noexcept {
                      if (left.capturedAtUnixSeconds !=
                          right.capturedAtUnixSeconds) {
                          return left.capturedAtUnixSeconds <
                                 right.capturedAtUnixSeconds;
                      }
                      return left.remainingPercent < right.remainingPercent;
                  });

        double previousRemaining = observations.front().remainingPercent;
        for (std::size_t index = 1; index < observations.size(); ++index) {
            const Observation& previous = observations[index - 1];
            const Observation& current = observations[index];
            if (current.capturedAtUnixSeconds ==
                    previous.capturedAtUnixSeconds &&
                std::fabs(current.remainingPercent -
                          previous.remainingPercent) >
                    kPercentComparisonTolerance) {
                WeeklyQuotaAlertEvaluation result = Finish(
                    WeeklyQuotaAlertOutcome::kRemainingIncreased, nextState);
                result.baselineAtUnixSeconds =
                    baseline->capturedAtUnixSeconds;
                result.periodStartUnixSeconds = periodStartUnixSeconds;
                return result;
            }
            if (current.capturedAtUnixSeconds !=
                previous.capturedAtUnixSeconds) {
                if (current.remainingPercent > previousRemaining +
                                                   kPercentComparisonTolerance) {
                    WeeklyQuotaAlertEvaluation result = Finish(
                        WeeklyQuotaAlertOutcome::kRemainingIncreased,
                        nextState);
                    result.baselineAtUnixSeconds =
                        baseline->capturedAtUnixSeconds;
                    result.periodStartUnixSeconds = periodStartUnixSeconds;
                    return result;
                }
                previousRemaining = current.remainingPercent;
            }
        }

        const double consumed =
            baseline->remainingPercent - currentWeeklyRemainingPercent;
        WeeklyQuotaAlertEvaluation result;
        result.nextState = nextState;
        result.consumedPercent = consumed;
        result.baselineAtUnixSeconds = baseline->capturedAtUnixSeconds;
        result.periodStartUnixSeconds = periodStartUnixSeconds;
        if (consumed <= kPercentComparisonTolerance) {
            result.outcome = WeeklyQuotaAlertOutcome::kNoConsumption;
            return result;
        }
        if (consumed + kPercentComparisonTolerance <
            policy.thresholdPercent) {
            result.outcome = WeeklyQuotaAlertOutcome::kBelowThreshold;
            return result;
        }

        result.outcome = WeeklyQuotaAlertOutcome::kShouldNotify;
        result.nextState.lastNotifiedPeriodStartUnixSeconds =
            periodStartUnixSeconds;
        result.nextState.lastNotifiedAtUnixSeconds = nowUnixSeconds;
        return result;
    } catch (...) {
        return Finish(WeeklyQuotaAlertOutcome::kInvalidInput, priorState);
    }
}

}  // namespace codex_monitor::codex
