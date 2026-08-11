#pragma once

#include "codex/weekly_quota_alert.h"
#include "codex/weekly_quota_alert_state_store.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace codex_monitor::codex {

struct WeeklyQuotaAlertNotification {
    double consumedPercent = 0.0;
    double thresholdPercent = 0.0;
    WeeklyQuotaAlertMode mode = WeeklyQuotaAlertMode::kRolling24Hours;
};

using WeeklyQuotaAlertNotificationSender =
    std::function<bool(const WeeklyQuotaAlertNotification& notification)>;

struct WeeklyQuotaAlertDeliveryRequest {
    WeeklyQuotaAlertPolicy policy;
    std::filesystem::path quotaHistoryPath;
    std::filesystem::path alertStatePath;
    double currentWeeklyRemainingPercent = 0.0;
    std::int64_t currentWeeklyResetAtUnixSeconds = 0;
    std::int64_t nowUnixSeconds = 0;
    std::optional<std::int64_t> localDayStartUnixSeconds;

    // The caller must prove that its non-activating system-notification
    // facility is registered before setting this flag. If it is false, this
    // function does not read or write either state file and never invokes the
    // sender, so an unavailable facility cannot be recorded as a notification.
    bool notificationFacilityAvailable = false;

    // Optional seam for deterministic atomic-write failure tests.
    WeeklyQuotaAlertStateAtomicReplace stateAtomicReplace;
};

enum class WeeklyQuotaAlertDeliveryStatus {
    kDisabled,
    kNotificationUnavailable,
    kInvalidRequest,
    kHistoryUnavailable,
    kStateUnavailable,
    kNoNotification,
    kStateSaveFailed,
    kDelivered,
    kNotificationFailedStateRestored,
    kNotificationFailedStateRestoreFailed,
};

struct WeeklyQuotaAlertDeliveryResult {
    WeeklyQuotaAlertDeliveryStatus status =
        WeeklyQuotaAlertDeliveryStatus::kInvalidRequest;
    WeeklyQuotaAlertOutcome evaluationOutcome =
        WeeklyQuotaAlertOutcome::kInvalidInput;
    std::optional<double> consumedPercent;
};

// This is the only product-level delivery sequence:
//   1. require an already available notification facility;
//   2. evaluate only validated local history and current weekly quota;
//   3. atomically save the evaluator's nextState;
//   4. invoke the non-activating system notification sender once.
// If step 4 fails, the prior state is atomically restored on a best-effort
// basis so a notification that was not accepted by Windows is not normally
// remembered as delivered.
[[nodiscard]] WeeklyQuotaAlertDeliveryResult EvaluateAndDeliverWeeklyQuotaAlert(
    const WeeklyQuotaAlertDeliveryRequest& request,
    const WeeklyQuotaAlertNotificationSender& sender) noexcept;

}  // namespace codex_monitor::codex
