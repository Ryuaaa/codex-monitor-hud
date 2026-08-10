#include "sampling_schedule.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void FinishInitialFullSample(codex_monitor::SamplingSchedule& schedule) {
    Require(schedule.ActivateAndRequestFullSample(), "activation must queue a full sample");
    Require(schedule.TakeBaselineReset(), "activation must reset the CPU baseline first");
    const auto item = schedule.TakeNext();
    Require(item.has_value(), "activation must expose a work item after reset");
    Require(item->mode == codex_monitor::SampleMode::kFastAndSlow,
            "activation work must include slow metrics");
    Require(schedule.Finish(*item), "the active generation must accept its result");
}

void TestBusyRequestsCoalesceWithSlowPriority() {
    codex_monitor::SamplingSchedule schedule;
    FinishInitialFullSample(schedule);

    Require(schedule.Request(codex_monitor::SampleMode::kFast),
            "an idle fast request must be queued");
    const auto running = schedule.TakeNext();
    Require(running.has_value(), "the fast request must start");
    Require(!schedule.Request(codex_monitor::SampleMode::kFast),
            "a running fast sample must cover another fast request");
    Require(schedule.Request(codex_monitor::SampleMode::kFastAndSlow),
            "a slow request must remain pending behind a running fast sample");
    Require(!schedule.Request(codex_monitor::SampleMode::kFast),
            "a pending slow request must not be downgraded by a fast request");
    Require(schedule.Finish(*running), "the current fast result must remain valid");

    const auto pending = schedule.TakeNext();
    Require(pending.has_value(), "the coalesced pending request must run");
    Require(pending->mode == codex_monitor::SampleMode::kFastAndSlow,
            "slow sampling must win request coalescing");
    Require(!schedule.Request(codex_monitor::SampleMode::kFastAndSlow),
            "a running full sample must cover all same-generation requests");
    Require(schedule.Finish(*pending), "the coalesced slow result must be accepted");
    Require(!schedule.IsBusy(), "the schedule must become idle after the result");
}

void TestPauseInvalidatesAndResumeResetsBaseline() {
    codex_monitor::SamplingSchedule schedule;
    FinishInitialFullSample(schedule);
    Require(schedule.Request(codex_monitor::SampleMode::kFast), "fast request must queue");
    const auto stale = schedule.TakeNext();
    Require(stale.has_value(), "fast request must start");
    Require(schedule.Request(codex_monitor::SampleMode::kFastAndSlow),
            "a slow request must be pending before the pause");

    schedule.PauseAndInvalidate();
    Require(!schedule.Request(codex_monitor::SampleMode::kFastAndSlow),
            "paused schedules must reject requests");
    Require(!schedule.Finish(*stale), "an in-flight pre-pause result must be rejected");
    Require(schedule.TakeBaselineReset(), "pause must request a CPU baseline reset");
    Require(!schedule.TakeNext(), "pause must cancel all pending work");

    Require(schedule.ActivateAndRequestFullSample(), "resume must queue a full first sample");
    Require(schedule.TakeBaselineReset(), "resume must reset the baseline before sampling");
    const auto resumed = schedule.TakeNext();
    Require(resumed.has_value(), "resume must expose a work item");
    Require(resumed->mode == codex_monitor::SampleMode::kFastAndSlow,
            "resume must request complete metrics");
    Require(resumed->generation != stale->generation,
            "resume must use a generation that cannot accept the stale result");
    Require(schedule.Finish(*resumed), "the resumed generation must accept its result");
}

void TestStopCancelsAndRejectsFutureWork() {
    codex_monitor::SamplingSchedule schedule;
    FinishInitialFullSample(schedule);
    Require(schedule.Request(codex_monitor::SampleMode::kFast), "fast request must queue");
    schedule.Stop();
    Require(schedule.IsStopped(), "stop must be observable by the worker");
    Require(schedule.HasAction(), "stop must wake a waiting worker");
    Require(!schedule.TakeNext(), "stop must cancel pending work");
    Require(!schedule.Request(codex_monitor::SampleMode::kFastAndSlow),
            "stop must reject future work");
}

}  // namespace

int main() {
    TestBusyRequestsCoalesceWithSlowPriority();
    TestPauseInvalidatesAndResumeResetsBaseline();
    TestStopCancelsAndRejectsFutureWork();
    std::cout << "sampling_schedule_tests=pass\n";
    return 0;
}
