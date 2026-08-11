#include "codex/weekly_quota_alert_delivery.h"

#include "codex/quota_history_store.h"

#include <utility>

namespace codex_monitor::codex {
namespace {

[[nodiscard]] bool SameState(const WeeklyQuotaAlertState& left,
                             const WeeklyQuotaAlertState& right) noexcept {
    return left.weeklyResetAtUnixSeconds == right.weeklyResetAtUnixSeconds &&
           left.mode == right.mode &&
           left.lastEvaluatedAtUnixSeconds == right.lastEvaluatedAtUnixSeconds &&
           left.lastNotifiedPeriodStartUnixSeconds ==
               right.lastNotifiedPeriodStartUnixSeconds &&
           left.lastNotifiedAtUnixSeconds == right.lastNotifiedAtUnixSeconds;
}

[[nodiscard]] WeeklyQuotaAlertDeliveryResult Finish(
    WeeklyQuotaAlertDeliveryStatus status,
    WeeklyQuotaAlertOutcome outcome = WeeklyQuotaAlertOutcome::kInvalidInput,
    std::optional<double> consumedPercent = std::nullopt) noexcept {
    WeeklyQuotaAlertDeliveryResult result;
    result.status = status;
    result.evaluationOutcome = outcome;
    result.consumedPercent = consumedPercent;
    return result;
}

[[nodiscard]] bool RecoverMalformedStateForCurrentPeriod(
    const WeeklyQuotaAlertDeliveryRequest& request,
    const WeeklyQuotaAlertStateStore& stateStore) noexcept {
    const WeeklyQuotaAlertEvaluation evaluation = EvaluateWeeklyQuotaAlert(
        {}, request.policy, request.currentWeeklyRemainingPercent,
        request.currentWeeklyResetAtUnixSeconds, request.nowUnixSeconds,
        request.localDayStartUnixSeconds, {});
    if (!evaluation.periodStartUnixSeconds ||
        evaluation.outcome == WeeklyQuotaAlertOutcome::kInvalidInput ||
        evaluation.outcome == WeeklyQuotaAlertOutcome::kDisabled ||
        evaluation.outcome == WeeklyQuotaAlertOutcome::kClockMovedBackward) {
        return false;
    }

    // The damaged file cannot prove whether this period already delivered.
    // Conservatively suppress only the current period, then let the normal
    // natural-day, rolling-24-hour, or weekly-reset boundary start fresh.
    WeeklyQuotaAlertState recovered = evaluation.nextState;
    recovered.lastNotifiedPeriodStartUnixSeconds =
        evaluation.periodStartUnixSeconds;
    recovered.lastNotifiedAtUnixSeconds = request.nowUnixSeconds;
    return stateStore.Save(recovered).written();
}

}  // namespace

WeeklyQuotaAlertDeliveryResult EvaluateAndDeliverWeeklyQuotaAlert(
    const WeeklyQuotaAlertDeliveryRequest& request,
    const WeeklyQuotaAlertNotificationSender& sender) noexcept {
    try {
        if (!IsValidWeeklyQuotaAlertPolicy(request.policy)) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kInvalidRequest);
        }
        if (!request.policy.enabled) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kDisabled,
                          WeeklyQuotaAlertOutcome::kDisabled);
        }
        if (!request.notificationFacilityAvailable || !sender) {
            return Finish(
                WeeklyQuotaAlertDeliveryStatus::kNotificationUnavailable);
        }
        if (request.quotaHistoryPath.empty() ||
            request.alertStatePath.empty()) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kInvalidRequest);
        }

        const QuotaHistoryLoadResult history =
            QuotaHistoryStore(request.quotaHistoryPath)
                .Load(request.nowUnixSeconds);
        if (!history.ok() || history.skippedMalformedLines != 0) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kHistoryUnavailable);
        }

        WeeklyQuotaAlertStateStore stateStore(
            request.alertStatePath, request.stateAtomicReplace);
        const WeeklyQuotaAlertStateLoadResult loadedState = stateStore.Load();
        if (!loadedState.ok()) {
            // A known malformed file is replaced with conservative
            // suppression for only the current period. I/O failures and
            // unknown newer versions remain untouched.
            if (loadedState.status ==
                WeeklyQuotaAlertStateLoadStatus::kMalformed) {
                (void)RecoverMalformedStateForCurrentPeriod(request,
                                                            stateStore);
            }
            return Finish(WeeklyQuotaAlertDeliveryStatus::kStateUnavailable);
        }

        const WeeklyQuotaAlertState priorState = loadedState.state;
        const WeeklyQuotaAlertEvaluation evaluation =
            EvaluateWeeklyQuotaAlert(
                history.samples, request.policy,
                request.currentWeeklyRemainingPercent,
                request.currentWeeklyResetAtUnixSeconds,
                request.nowUnixSeconds,
                request.localDayStartUnixSeconds,
                priorState);
        if (evaluation.outcome == WeeklyQuotaAlertOutcome::kInvalidInput ||
            evaluation.outcome == WeeklyQuotaAlertOutcome::kDisabled ||
            evaluation.outcome == WeeklyQuotaAlertOutcome::kClockMovedBackward) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kNoNotification,
                          evaluation.outcome, evaluation.consumedPercent);
        }

        if (!evaluation.shouldNotify()) {
            if (!SameState(priorState, evaluation.nextState) &&
                !stateStore.Save(evaluation.nextState).written()) {
                return Finish(
                    WeeklyQuotaAlertDeliveryStatus::kStateSaveFailed,
                    evaluation.outcome, evaluation.consumedPercent);
            }
            return Finish(WeeklyQuotaAlertDeliveryStatus::kNoNotification,
                          evaluation.outcome, evaluation.consumedPercent);
        }

        if (!stateStore.Save(evaluation.nextState).written()) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kStateSaveFailed,
                          evaluation.outcome, evaluation.consumedPercent);
        }

        const WeeklyQuotaAlertNotification notification{
            evaluation.consumedPercent.value_or(0.0),
            request.policy.thresholdPercent,
            request.policy.mode,
        };
        bool accepted = false;
        try {
            accepted = sender(notification);
        } catch (...) {
            accepted = false;
        }
        if (accepted) {
            return Finish(WeeklyQuotaAlertDeliveryStatus::kDelivered,
                          evaluation.outcome, evaluation.consumedPercent);
        }

        const bool restored = stateStore.Save(priorState).written();
        return Finish(
            restored
                ? WeeklyQuotaAlertDeliveryStatus::
                      kNotificationFailedStateRestored
                : WeeklyQuotaAlertDeliveryStatus::
                      kNotificationFailedStateRestoreFailed,
            evaluation.outcome, evaluation.consumedPercent);
    } catch (...) {
        return Finish(WeeklyQuotaAlertDeliveryStatus::kInvalidRequest);
    }
}

}  // namespace codex_monitor::codex
