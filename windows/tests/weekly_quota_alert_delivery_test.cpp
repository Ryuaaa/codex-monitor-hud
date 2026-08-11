#include "codex/quota_history_store.h"
#include "codex/weekly_quota_alert_delivery.h"
#include "codex/weekly_quota_alert_state_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using codex_monitor::codex::QuotaHistorySample;
using codex_monitor::codex::QuotaHistoryStore;
using codex_monitor::codex::WeeklyQuotaAlertDeliveryRequest;
using codex_monitor::codex::WeeklyQuotaAlertDeliveryStatus;
using codex_monitor::codex::WeeklyQuotaAlertMode;
using codex_monitor::codex::WeeklyQuotaAlertOutcome;
using codex_monitor::codex::WeeklyQuotaAlertStateLoadStatus;
using codex_monitor::codex::WeeklyQuotaAlertStateStore;

constexpr std::int64_t kNow = 2'000'000'000;
constexpr std::int64_t kReset = kNow + 4 * 24 * 60 * 60;
constexpr std::int64_t kRollingStart = kNow - 24 * 60 * 60;

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto sequence = std::chrono::high_resolution_clock::now()
                                  .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("codex-monitor-alert-delivery-" +
                 std::to_string(sequence));
        std::error_code error;
        Require(std::filesystem::create_directories(path_, error) && !error,
                "delivery test directory must be created");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

QuotaHistorySample Sample(std::int64_t capturedAt,
                          double remaining,
                          std::int64_t resetAt = kReset) {
    QuotaHistorySample sample;
    sample.capturedAtUnixSeconds = capturedAt;
    sample.weekly.remainingPercent = remaining;
    sample.weekly.resetsAtUnixSeconds = resetAt;
    return sample;
}

WeeklyQuotaAlertDeliveryRequest Request(const TemporaryDirectory& temporary) {
    WeeklyQuotaAlertDeliveryRequest request;
    request.policy.enabled = true;
    request.policy.thresholdPercent = 15.0;
    request.policy.mode = WeeklyQuotaAlertMode::kRolling24Hours;
    request.quotaHistoryPath = temporary.path() / "quota-history.txt";
    request.alertStatePath = temporary.path() / "weekly-alert-state.txt";
    request.currentWeeklyRemainingPercent = 70.0;
    request.currentWeeklyResetAtUnixSeconds = kReset;
    request.nowUnixSeconds = kNow;
    request.notificationFacilityAvailable = true;
    return request;
}

void SeedBaseline(const WeeklyQuotaAlertDeliveryRequest& request,
                  double remaining = 90.0,
                  std::int64_t resetAt = kReset) {
    Require(QuotaHistoryStore(request.quotaHistoryPath)
                .Update(Sample(kRollingStart + 5 * 60,
                               remaining, resetAt))
                .written(),
            "quota history fixture must be saved atomically");
}

void TestUnavailableFacilityDoesNotWriteOrNotify() {
    TemporaryDirectory temporary;
    auto request = Request(temporary);
    request.notificationFacilityAvailable = false;
    int sends = 0;
    const auto result = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [&](const auto&) {
            ++sends;
            return true;
        });
    Require(result.status ==
                WeeklyQuotaAlertDeliveryStatus::kNotificationUnavailable &&
                sends == 0 &&
                !std::filesystem::exists(request.alertStatePath) &&
                !std::filesystem::exists(request.quotaHistoryPath),
            "an unavailable notification facility must not pop up or be recorded as reminded");
}

void TestAtomicStatePrecedesOneNotification() {
    TemporaryDirectory temporary;
    auto request = Request(temporary);
    SeedBaseline(request);
    int sends = 0;
    const auto first = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [&](const auto& notification) {
            ++sends;
            const auto duringSend =
                WeeklyQuotaAlertStateStore(request.alertStatePath).Load();
            Require(duringSend.status == WeeklyQuotaAlertStateLoadStatus::kOk &&
                        duringSend.state.lastNotifiedAtUnixSeconds == kNow,
                    "the next anti-duplication state must be atomically saved before Windows is asked to notify");
            Require(notification.consumedPercent == 20.0 &&
                        notification.thresholdPercent == 15.0,
                    "the notification must contain only the calculated percentage and configured threshold");
            return true;
        });
    Require(first.status == WeeklyQuotaAlertDeliveryStatus::kDelivered &&
                first.evaluationOutcome ==
                    WeeklyQuotaAlertOutcome::kShouldNotify &&
                sends == 1,
            "a valid threshold crossing must deliver exactly once");

    request.currentWeeklyRemainingPercent = 60.0;
    request.nowUnixSeconds += 5 * 60;
    const auto duplicate = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [&](const auto&) {
            ++sends;
            return true;
        });
    Require(duplicate.status ==
                WeeklyQuotaAlertDeliveryStatus::kNoNotification &&
                duplicate.evaluationOutcome ==
                    WeeklyQuotaAlertOutcome::kAlreadyNotified &&
                sends == 1,
            "the same rolling period must not deliver a duplicate notification");
}

void TestStateFailureSuppressesNotification() {
    TemporaryDirectory temporary;
    auto request = Request(temporary);
    SeedBaseline(request);
    request.stateAtomicReplace = [](const auto&, const auto&) {
        return std::make_error_code(std::errc::permission_denied);
    };
    int sends = 0;
    const auto result = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [&](const auto&) {
            ++sends;
            return true;
        });
    Require(result.status ==
                WeeklyQuotaAlertDeliveryStatus::kStateSaveFailed &&
                sends == 0,
            "an atomic state failure must suppress the notification");
}

void TestRejectedNotificationRestoresPriorState() {
    TemporaryDirectory temporary;
    auto request = Request(temporary);
    SeedBaseline(request);
    const auto result = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [](const auto&) { return false; });
    const auto restored =
        WeeklyQuotaAlertStateStore(request.alertStatePath).Load();
    Require(result.status == WeeklyQuotaAlertDeliveryStatus::
                                 kNotificationFailedStateRestored &&
                restored.status == WeeklyQuotaAlertStateLoadStatus::kOk &&
                !restored.state.lastNotifiedAtUnixSeconds,
            "a notification rejected by Windows must not remain recorded as delivered");
}

void TestInsufficientHistoryNeverNotifies() {
    TemporaryDirectory temporary;
    auto request = Request(temporary);
    Require(QuotaHistoryStore(request.quotaHistoryPath)
                .Update(Sample(kRollingStart + 31 * 60, 90.0))
                .written(),
            "late baseline fixture must be stored");
    int sends = 0;
    const auto result = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [&](const auto&) {
            ++sends;
            return true;
        });
    Require(result.status == WeeklyQuotaAlertDeliveryStatus::kNoNotification &&
                result.evaluationOutcome ==
                    WeeklyQuotaAlertOutcome::kInsufficientHistory &&
                sends == 0,
            "insufficient history must never produce a notification");
}

void TestWeeklyResetStartsANewNotificationCycle() {
    TemporaryDirectory temporary;
    auto request = Request(temporary);
    SeedBaseline(request);
    int sends = 0;
    Require(EvaluateAndDeliverWeeklyQuotaAlert(
                request, [&](const auto&) {
                    ++sends;
                    return true;
                })
                .status == WeeklyQuotaAlertDeliveryStatus::kDelivered,
            "old weekly cycle must deliver its first notification");

    const std::int64_t newNow = kNow + 60 * 60;
    const std::int64_t newReset = kReset + 7 * 24 * 60 * 60;
    request.nowUnixSeconds = newNow;
    request.currentWeeklyResetAtUnixSeconds = newReset;
    request.currentWeeklyRemainingPercent = 60.0;
    Require(QuotaHistoryStore(request.quotaHistoryPath)
                .Update(Sample(newNow - 24 * 60 * 60 + 5 * 60,
                               80.0, newReset))
                .written(),
            "new weekly cycle baseline must be stored");
    const auto nextCycle = EvaluateAndDeliverWeeklyQuotaAlert(
        request, [&](const auto&) {
            ++sends;
            return true;
        });
    Require(nextCycle.status == WeeklyQuotaAlertDeliveryStatus::kDelivered &&
                sends == 2,
            "a changed weekly reset must start one fresh notification cycle");
}

}  // namespace

int main() {
    TestUnavailableFacilityDoesNotWriteOrNotify();
    TestAtomicStatePrecedesOneNotification();
    TestStateFailureSuppressesNotification();
    TestRejectedNotificationRestoresPriorState();
    TestInsufficientHistoryNeverNotifies();
    TestWeeklyResetStartsANewNotificationCycle();
    std::cout << "weekly_quota_alert_delivery_test: pass\n";
    return 0;
}
