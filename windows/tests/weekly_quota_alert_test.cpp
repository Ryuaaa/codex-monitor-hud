#include "codex/weekly_quota_alert.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using codex_monitor::codex::EvaluateWeeklyQuotaAlert;
using codex_monitor::codex::IsValidWeeklyQuotaAlertPolicy;
using codex_monitor::codex::QuotaHistorySample;
using codex_monitor::codex::WeeklyQuotaAlertEvaluation;
using codex_monitor::codex::WeeklyQuotaAlertMode;
using codex_monitor::codex::WeeklyQuotaAlertOutcome;
using codex_monitor::codex::WeeklyQuotaAlertPolicy;
using codex_monitor::codex::WeeklyQuotaAlertState;

constexpr std::int64_t kNow = 2'000'000'000;
constexpr std::int64_t kDayStart = kNow - 12 * 60 * 60;
constexpr std::int64_t kReset = kNow + 4 * 24 * 60 * 60;

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

QuotaHistorySample Sample(std::int64_t capturedAt,
                          double remaining,
                          std::int64_t resetAt = kReset) {
    QuotaHistorySample sample;
    sample.capturedAtUnixSeconds = capturedAt;
    sample.weekly.remainingPercent = remaining;
    sample.weekly.resetsAtUnixSeconds = resetAt;
    return sample;
}

WeeklyQuotaAlertPolicy NaturalPolicy(double threshold = 20.0) {
    WeeklyQuotaAlertPolicy policy;
    policy.enabled = true;
    policy.thresholdPercent = threshold;
    policy.mode = WeeklyQuotaAlertMode::kNaturalDay;
    return policy;
}

WeeklyQuotaAlertPolicy RollingPolicy(double threshold = 20.0) {
    WeeklyQuotaAlertPolicy policy = NaturalPolicy(threshold);
    policy.mode = WeeklyQuotaAlertMode::kRolling24Hours;
    return policy;
}

WeeklyQuotaAlertEvaluation Natural(
    const std::vector<QuotaHistorySample>& history,
    double remaining,
    const WeeklyQuotaAlertState& state = {},
    std::int64_t now = kNow,
    std::int64_t dayStart = kDayStart,
    std::int64_t reset = kReset,
    double threshold = 20.0) {
    return EvaluateWeeklyQuotaAlert(history, NaturalPolicy(threshold),
                                    remaining, reset, now, dayStart, state);
}

WeeklyQuotaAlertEvaluation Rolling(
    const std::vector<QuotaHistorySample>& history,
    double remaining,
    const WeeklyQuotaAlertState& state = {},
    std::int64_t now = kNow,
    std::int64_t reset = kReset,
    double threshold = 20.0) {
    return EvaluateWeeklyQuotaAlert(history, RollingPolicy(threshold),
                                    remaining, reset, now, std::nullopt,
                                    state);
}

void TestDisabledByDefaultAndThresholdBounds() {
    const WeeklyQuotaAlertPolicy defaults;
    Require(!defaults.enabled && defaults.thresholdPercent == 15.0 &&
                defaults.mode == WeeklyQuotaAlertMode::kRolling24Hours &&
                IsValidWeeklyQuotaAlertPolicy(defaults),
            "alerts default to disabled, fifteen percent, and rolling twenty-four hours");
    const auto disabled = EvaluateWeeklyQuotaAlert(
        {Sample(kDayStart + 5 * 60, 90.0)}, defaults, 60.0, kReset,
        kNow, std::nullopt, {});
    Require(disabled.outcome == WeeklyQuotaAlertOutcome::kDisabled &&
                !disabled.shouldNotify(),
            "a default policy must never notify");

    Require(IsValidWeeklyQuotaAlertPolicy(NaturalPolicy(5.0)) &&
                IsValidWeeklyQuotaAlertPolicy(RollingPolicy(100.0)),
            "inclusive five-to-one-hundred threshold bounds must be valid");
    Require(!IsValidWeeklyQuotaAlertPolicy(NaturalPolicy(4.999)) &&
                !IsValidWeeklyQuotaAlertPolicy(RollingPolicy(100.001)) &&
                !IsValidWeeklyQuotaAlertPolicy(NaturalPolicy(
                    std::numeric_limits<double>::quiet_NaN())),
            "out-of-range and non-finite thresholds must be invalid");
}

void TestNaturalDayTriggersOnceAndRestartsNextDay() {
    const std::vector<QuotaHistorySample> firstDay = {
        Sample(kDayStart + 5 * 60, 90.0),
        Sample(kDayStart + 6 * 60 * 60, 80.0),
    };
    const auto first = Natural(firstDay, 70.0);
    Require(first.shouldNotify() && first.consumedPercent &&
                std::fabs(*first.consumedPercent - 20.0) < 1e-9 &&
                first.baselineAtUnixSeconds == kDayStart + 5 * 60 &&
                first.periodStartUnixSeconds == kDayStart,
            "a natural-day threshold crossing must notify from local midnight");

    const auto duplicate = Natural(firstDay, 60.0, first.nextState,
                                   kNow + 5 * 60, kDayStart);
    Require(duplicate.outcome ==
                WeeklyQuotaAlertOutcome::kAlreadyNotified,
            "the same local calendar day must notify only once");

    const std::int64_t nextDayStart = kDayStart + 24 * 60 * 60;
    const std::int64_t nextNow = kNow + 24 * 60 * 60;
    const std::vector<QuotaHistorySample> nextDay = {
        Sample(nextDayStart + 5 * 60, 70.0),
    };
    const auto next = Natural(nextDay, 50.0, duplicate.nextState, nextNow,
                              nextDayStart);
    Require(next.shouldNotify(),
            "a new local calendar day must allow one new notification");
}

void TestRollingWindowUsesANearbyConservativeBaseline() {
    const std::int64_t target = kNow - 24 * 60 * 60;
    const auto result = Rolling({
        Sample(target - 1, 100.0),
        Sample(target + 5 * 60, 85.0),
        Sample(target + 10 * 60, 84.0),
    }, 65.0);
    Require(result.shouldNotify() && result.baselineAtUnixSeconds ==
                                        target + 5 * 60 &&
                result.consumedPercent &&
                std::fabs(*result.consumedPercent - 20.0) < 1e-9,
            "rolling use must ignore pre-window data and use the first nearby safe baseline");

    const auto tooLate = Rolling({Sample(target + 30 * 60 + 1, 90.0)},
                                 60.0);
    Require(tooLate.outcome ==
                WeeklyQuotaAlertOutcome::kInsufficientHistory,
            "a rolling baseline more than thirty minutes late is insufficient");

    const auto futureOnly = Rolling({Sample(kNow + 1, 90.0)}, 60.0);
    Require(futureOnly.outcome ==
                WeeklyQuotaAlertOutcome::kInsufficientHistory,
            "future samples must never become a rolling baseline");
}

void TestRollingWindowSuppressesForTwentyFourHours() {
    const std::int64_t target = kNow - 24 * 60 * 60;
    const auto first = Rolling({Sample(target + 5 * 60, 90.0)}, 70.0);
    Require(first.shouldNotify(), "rolling fixture must first notify");

    const auto duplicate = Rolling(
        {Sample(target + 10 * 60, 90.0)}, 60.0, first.nextState,
        kNow + 60 * 60);
    Require(duplicate.outcome ==
                WeeklyQuotaAlertOutcome::kAlreadyNotified,
            "overlapping rolling windows must not repeat within twenty-four hours");

    const std::int64_t later = kNow + 24 * 60 * 60;
    const auto newWindow = Rolling({Sample(kNow, 70.0)}, 50.0,
                                   duplicate.nextState, later);
    Require(newWindow.shouldNotify(),
            "a full twenty-four hours after notification starts a new rolling period");
}

void TestOnlyTheCurrentWeeklyResetCycleCounts() {
    const auto wrongCycle = Natural(
        {Sample(kDayStart + 5 * 60, 90.0, kReset - 1)}, 60.0);
    Require(wrongCycle.outcome ==
                WeeklyQuotaAlertOutcome::kInsufficientHistory,
            "a baseline from another weekly reset cycle must be ignored");

    const auto first = Natural({Sample(kDayStart + 5 * 60, 90.0)}, 60.0);
    Require(first.shouldNotify(), "old cycle state fixture must notify");
    const std::int64_t newReset = kReset + 24 * 60 * 60;
    const auto newCycle = Natural(
        {Sample(kDayStart + 5 * 60, 80.0, newReset)}, 60.0,
        first.nextState, kNow + 60, kDayStart, newReset);
    Require(newCycle.shouldNotify() &&
                newCycle.nextState.weeklyResetAtUnixSeconds == newReset,
            "a changed weekly reset must clear prior-cycle suppression");
}

void TestClockRollbackAndRemainingIncreasesNeverNotify() {
    WeeklyQuotaAlertState futureState;
    futureState.weeklyResetAtUnixSeconds = kReset;
    futureState.mode = WeeklyQuotaAlertMode::kNaturalDay;
    futureState.lastEvaluatedAtUnixSeconds = kNow + 1;
    const auto rollback = Natural(
        {Sample(kDayStart + 5 * 60, 90.0)}, 60.0, futureState);
    Require(rollback.outcome ==
                WeeklyQuotaAlertOutcome::kClockMovedBackward &&
                rollback.nextState.lastEvaluatedAtUnixSeconds == kNow + 1,
            "clock rollback must not lower persisted time or notify");

    const auto currentIncrease = Natural(
        {Sample(kDayStart + 5 * 60, 70.0)}, 80.0);
    Require(currentIncrease.outcome ==
                WeeklyQuotaAlertOutcome::kRemainingIncreased,
            "a higher current remaining percentage must suppress notification");

    const auto intermediateIncrease = Natural({
        Sample(kDayStart + 5 * 60, 90.0),
        Sample(kDayStart + 60 * 60, 85.0),
        Sample(kDayStart + 2 * 60 * 60, 88.0),
    }, 60.0);
    Require(intermediateIncrease.outcome ==
                WeeklyQuotaAlertOutcome::kRemainingIncreased,
            "any same-cycle increase inside the period makes the data unsafe");
}

void TestInsufficientAndInvalidDataDoNotNotify() {
    const auto lateNatural = Natural(
        {Sample(kDayStart + 30 * 60 + 1, 90.0)}, 60.0);
    Require(lateNatural.outcome ==
                WeeklyQuotaAlertOutcome::kInsufficientHistory,
            "a natural-day baseline too far from midnight is insufficient");

    const auto tooYoung = Natural(
        {Sample(kNow - 59, 90.0)}, 60.0, {}, kNow, kNow - 10 * 60);
    Require(tooYoung.outcome ==
                WeeklyQuotaAlertOutcome::kInsufficientHistory,
            "a baseline younger than one minute is insufficient");

    const auto below = Natural(
        {Sample(kDayStart + 5 * 60, 90.0)}, 70.001);
    Require(below.outcome == WeeklyQuotaAlertOutcome::kBelowThreshold,
            "usage below the configured threshold must not notify");

    const auto expiredReset = Natural(
        {Sample(kDayStart + 5 * 60, 90.0, kNow)}, 60.0, {}, kNow,
        kDayStart, kNow);
    Require(expiredReset.outcome == WeeklyQuotaAlertOutcome::kInvalidInput,
            "an elapsed weekly reset is not a current cycle");

    const auto wrongDayStart = EvaluateWeeklyQuotaAlert(
        {Sample(kDayStart + 5 * 60, 90.0)}, NaturalPolicy(), 60.0,
        kReset, kNow, kNow - 27 * 60 * 60, {});
    Require(wrongDayStart.outcome ==
                WeeklyQuotaAlertOutcome::kInvalidInput,
            "a stale value cannot masquerade as today's local midnight");
}

}  // namespace

int main() {
    TestDisabledByDefaultAndThresholdBounds();
    TestNaturalDayTriggersOnceAndRestartsNextDay();
    TestRollingWindowUsesANearbyConservativeBaseline();
    TestRollingWindowSuppressesForTwentyFourHours();
    TestOnlyTheCurrentWeeklyResetCycleCounts();
    TestClockRollbackAndRemainingIncreasesNeverNotify();
    TestInsufficientAndInvalidDataDoNotNotify();
    std::cout << "weekly_quota_alert_test: pass\n";
    return 0;
}
