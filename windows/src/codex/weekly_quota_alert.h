#pragma once

#include "codex/quota_history_store.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace codex_monitor::codex {

enum class WeeklyQuotaAlertMode {
    kNaturalDay,
    kRolling24Hours,
};

// The feature is intentionally disabled by default. Settings/UI code owns the
// user's choice and only needs to pass the resulting policy to the evaluator.
struct WeeklyQuotaAlertPolicy {
    bool enabled = false;
    double thresholdPercent = 15.0;
    WeeklyQuotaAlertMode mode = WeeklyQuotaAlertMode::kRolling24Hours;
};

// Only anti-duplication and clock-safety state is persisted. Quota samples
// remain in QuotaHistoryStore and are never copied into this state.
struct WeeklyQuotaAlertState {
    std::optional<std::int64_t> weeklyResetAtUnixSeconds;
    WeeklyQuotaAlertMode mode = WeeklyQuotaAlertMode::kRolling24Hours;
    std::optional<std::int64_t> lastEvaluatedAtUnixSeconds;
    std::optional<std::int64_t> lastNotifiedPeriodStartUnixSeconds;
    std::optional<std::int64_t> lastNotifiedAtUnixSeconds;
};

enum class WeeklyQuotaAlertOutcome {
    kDisabled,
    kShouldNotify,
    kInvalidInput,
    kInsufficientHistory,
    kAlreadyNotified,
    kNoConsumption,
    kBelowThreshold,
    kRemainingIncreased,
    kClockMovedBackward,
};

struct WeeklyQuotaAlertEvaluation {
    WeeklyQuotaAlertOutcome outcome = WeeklyQuotaAlertOutcome::kInvalidInput;
    WeeklyQuotaAlertState nextState;
    std::optional<double> consumedPercent;
    std::optional<std::int64_t> baselineAtUnixSeconds;
    std::optional<std::int64_t> periodStartUnixSeconds;

    [[nodiscard]] bool shouldNotify() const noexcept {
        return outcome == WeeklyQuotaAlertOutcome::kShouldNotify;
    }
};

[[nodiscard]] bool IsValidWeeklyQuotaAlertPolicy(
    const WeeklyQuotaAlertPolicy& policy) noexcept;
[[nodiscard]] bool IsValidWeeklyQuotaAlertState(
    const WeeklyQuotaAlertState& state) noexcept;

// localDayStartUnixSeconds must be the local start of the current calendar day
// when mode is kNaturalDay. It is ignored for kRolling24Hours. The evaluator is
// platform-independent and does not read the clock, locale, settings, or disk.
//
// Conservative baseline rules deliberately prefer a missed notification over
// a false one:
// - natural day: first same-cycle sample in [local midnight, +30 minutes];
// - rolling 24h: first same-cycle sample in [now - 24h, +30 minutes];
// - a baseline must be at least 60 seconds old;
// - any increase in same-cycle remaining quota between baseline and now makes
//   the whole observation unsafe and suppresses the notification.
//
// When shouldNotify() is true, the caller must atomically save nextState before
// displaying a system notification. If that save fails, suppress the
// notification so a restart cannot bypass the single-period guarantee.
[[nodiscard]] WeeklyQuotaAlertEvaluation EvaluateWeeklyQuotaAlert(
    const std::vector<QuotaHistorySample>& history,
    const WeeklyQuotaAlertPolicy& policy,
    double currentWeeklyRemainingPercent,
    std::int64_t currentWeeklyResetAtUnixSeconds,
    std::int64_t nowUnixSeconds,
    std::optional<std::int64_t> localDayStartUnixSeconds,
    const WeeklyQuotaAlertState& priorState) noexcept;

}  // namespace codex_monitor::codex
