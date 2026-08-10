#include "codex/codex_refresh_schedule.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void TestActivationImmediatelyQueuesRefresh() {
    codex_monitor::CodexRefreshSchedule schedule;
    Require(!schedule.IsActive(), "new schedule must be inactive");
    Require(schedule.Activate(), "activation must change inactive state");
    Require(schedule.IsActive(), "activation must make the schedule active");
    Require(schedule.HasPending(), "activation must immediately queue a refresh");
    Require(!schedule.Activate(), "repeated activation must not queue duplicate work");

    const auto work = schedule.TakeNext();
    Require(work.has_value(), "activated refresh must be available to the worker");
    Require(!schedule.TakeNext(), "only one refresh may run at a time");
}

void TestBusyRequestsMergeIntoOnePendingRound() {
    codex_monitor::CodexRefreshSchedule schedule;
    Require(schedule.Activate(), "activation must queue initial work");
    const auto running = schedule.TakeNext();
    Require(running.has_value(), "initial work must start");

    Require(schedule.Request(), "first busy request must create one pending round");
    Require(!schedule.Request(), "repeated busy requests must merge into that pending round");
    Require(schedule.HasPending(), "the merged pending round must remain visible");
    Require(schedule.Finish(*running, codex_monitor::CodexRefreshOutcome::kSuccess),
            "current result must remain valid");
    Require(!schedule.RecommendedDelay(),
            "an immediate pending round must suppress a redundant timer delay");

    const auto merged = schedule.TakeNext();
    Require(merged.has_value(), "exactly one merged round must follow the running round");
    Require(!schedule.TakeNext(), "the merged round must not be duplicated");
    Require(schedule.Finish(*merged, codex_monitor::CodexRefreshOutcome::kSuccess),
            "merged round must finish normally");
    Require(!schedule.HasPending(), "no third round may remain after merged work");
}

void TestPauseInvalidatesRunningAndPendingWork() {
    codex_monitor::CodexRefreshSchedule schedule;
    Require(schedule.Activate(), "activation must queue initial work");
    const auto stale = schedule.TakeNext();
    Require(stale.has_value(), "initial work must start");
    Require(schedule.Request(), "busy request must queue pending work before pause");

    schedule.PauseAndInvalidate();
    Require(!schedule.IsActive(), "pause must deactivate refreshes");
    Require(!schedule.HasPending(), "pause must cancel pending refreshes");
    Require(!schedule.Request(), "paused schedules must reject refresh requests");
    Require(!schedule.Finish(*stale, codex_monitor::CodexRefreshOutcome::kSuccess),
            "a pre-pause in-flight result must be invalid");
    Require(!schedule.RecommendedDelay(), "an invalid old result must not schedule a timer");
}

void TestResumeQueuesNewGeneration() {
    codex_monitor::CodexRefreshSchedule schedule;
    Require(schedule.Activate(), "initial activation must succeed");
    const auto stale = schedule.TakeNext();
    Require(stale.has_value(), "initial work must start");
    schedule.PauseAndInvalidate();

    Require(schedule.Activate(), "resume must reactivate the schedule");
    Require(schedule.HasPending(), "resume must immediately queue a refresh");
    Require(!schedule.TakeNext(), "old in-flight work must finish before resumed work starts");
    Require(!schedule.Finish(*stale, codex_monitor::CodexRefreshOutcome::kFailure),
            "old generation must remain invalid after resume");

    const auto resumed = schedule.TakeNext();
    Require(resumed.has_value(), "resumed work must start after old work retires");
    Require(resumed->generation != stale->generation,
            "resumed work must use a new generation");
    Require(schedule.Finish(*resumed, codex_monitor::CodexRefreshOutcome::kSuccess),
            "resumed result must be accepted");
}

void TestSuccessAndFailureIntervals() {
    using namespace std::chrono_literals;

    codex_monitor::CodexRefreshSchedule schedule;
    Require(schedule.Activate(), "activation must queue success case");
    const auto success = schedule.TakeNext();
    Require(success.has_value(), "success case must start");
    Require(schedule.Finish(*success, codex_monitor::CodexRefreshOutcome::kSuccess),
            "success result must be accepted");
    Require(schedule.RecommendedDelay() == 300s,
            "success must recommend the normal 300 second interval");

    Require(schedule.Request(), "explicit retry request must queue failure case");
    const auto failure = schedule.TakeNext();
    Require(failure.has_value(), "failure case must start");
    Require(schedule.Finish(*failure, codex_monitor::CodexRefreshOutcome::kFailure),
            "failure result must be accepted");
    Require(schedule.RecommendedDelay() == 60s,
            "failure must recommend the 60 second retry interval");
}

void TestStopRejectsAllFutureWork() {
    codex_monitor::CodexRefreshSchedule schedule;
    Require(schedule.Activate(), "activation must queue initial work");
    const auto stale = schedule.TakeNext();
    Require(stale.has_value(), "initial work must start");
    schedule.Stop();

    Require(schedule.IsStopped(), "stop must be observable");
    Require(!schedule.Activate(), "stopped schedule must reject activation");
    Require(!schedule.Request(), "stopped schedule must reject requests");
    Require(!schedule.TakeNext(), "stopped schedule must not start work");
    Require(!schedule.Finish(*stale, codex_monitor::CodexRefreshOutcome::kSuccess),
            "stop must invalidate an in-flight result");
    Require(!schedule.RecommendedDelay(), "stop must clear timer recommendations");
}

}  // namespace

int main() {
    TestActivationImmediatelyQueuesRefresh();
    TestBusyRequestsMergeIntoOnePendingRound();
    TestPauseInvalidatesRunningAndPendingWork();
    TestResumeQueuesNewGeneration();
    TestSuccessAndFailureIntervals();
    TestStopRejectsAllFutureWork();
    std::cout << "codex_refresh_schedule_tests=pass\n";
    return 0;
}
