#include "process_attribution.h"

#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool Near(const std::optional<double>& value, double expected) {
    return value && std::fabs(*value - expected) < 0.0001;
}

constexpr std::uint64_t Seconds(std::uint64_t seconds) {
    return seconds *
        codex_monitor::kProcessAttributionHundredNanosecondsPerSecond;
}

codex_monitor::ProcessSnapshot Process(
    std::uint32_t id,
    const wchar_t* name,
    bool target,
    std::uint64_t creation,
    std::uint64_t cpu,
    std::uint64_t read,
    std::uint64_t write) {
    codex_monitor::ProcessSnapshot process;
    process.processId = id;
    process.executableName = name;
    process.isTargetRoot = target;
    process.isTargetTree = target;
    process.cpuTimeAttempted = true;
    process.cpuTimeAvailable = true;
    process.creationTime100ns = creation;
    process.cpuTime100ns = cpu;
    process.ioAttempted = target;
    process.ioAvailable = target;
    process.ioReadTransferBytes = read;
    process.ioWriteTransferBytes = write;
    return process;
}

codex_monitor::RawPerformanceSnapshot Snapshot(
    std::uint64_t second,
    std::uint64_t idle,
    std::uint64_t kernel,
    std::uint64_t user) {
    codex_monitor::RawPerformanceSnapshot raw;
    raw.capturedAtUnbiasedTimeAvailable = true;
    raw.capturedAtUnbiased100ns = Seconds(second);
    raw.systemCpu = {true, idle, kernel, user};
    raw.processListAvailable = true;
    return raw;
}

void TestTargetIoNeedsBaselineThenUsesFiveSecondDelta() {
    codex_monitor::ProcessAttributionTracker tracker;
    auto first = Snapshot(10, 100, 400, 200);
    first.processes.push_back(Process(
        10, L"codex.exe", true, 7, 100, 1'000, 2'000));
    codex_monitor::PerformanceSnapshot initial;
    initial.targetProcessCount = 1;
    tracker.Apply(first, false, initial);
    Expect(initial.targetIoNeedsBaseline &&
               !initial.targetIoReadBytesPerSecond,
           "the first target I/O sample must request a baseline");

    auto second = Snapshot(15, 150, 650, 300);
    second.processes.push_back(Process(
        10, L"codex.exe", true, 7, 150, 6'000, 12'000));
    codex_monitor::PerformanceSnapshot current;
    current.targetProcessCount = 1;
    tracker.Apply(second, false, current);
    Expect(Near(current.targetIoReadBytesPerSecond, 1'000.0) &&
               Near(current.targetIoWriteBytesPerSecond, 2'000.0) &&
               !current.targetIoPartial,
           "target process I/O must use cumulative transfer deltas and elapsed time");
}

void TestTargetIoChurnAndCounterResetStayConservative() {
    codex_monitor::ProcessAttributionTracker tracker;
    auto first = Snapshot(1, 10, 30, 20);
    first.processes.push_back(Process(
        10, L"codex.exe", true, 7, 10, 1'000, 1'000));
    first.processes.push_back(Process(
        11, L"helper.exe", true, 8, 10, 2'000, 2'000));
    codex_monitor::PerformanceSnapshot initial;
    initial.targetProcessCount = 2;
    tracker.Apply(first, false, initial);

    auto churned = Snapshot(6, 20, 60, 40);
    churned.processes.push_back(Process(
        10, L"codex.exe", true, 7, 20, 2'000, 2'000));
    churned.processes.push_back(Process(
        12, L"new.exe", true, 9, 1, 100, 100));
    codex_monitor::PerformanceSnapshot current;
    current.targetProcessCount = 2;
    tracker.Apply(churned, false, current);
    Expect(Near(current.targetIoReadBytesPerSecond, 200.0) &&
               current.targetIoPartial,
           "new and exited target processes must make the aggregate a partial lower bound");

    auto reset = Snapshot(11, 30, 90, 60);
    reset.processes.push_back(Process(
        10, L"codex.exe", true, 7, 30, 1, 1));
    codex_monitor::PerformanceSnapshot resetResult;
    resetResult.targetProcessCount = 1;
    tracker.Apply(reset, false, resetResult);
    Expect(resetResult.targetIoNeedsBaseline &&
               !resetResult.targetIoReadBytesPerSecond,
           "a rolled-back process I/O counter must not fabricate a negative rate");
}

void TestNoTargetIsARealZeroAndResetDropsContinuity() {
    codex_monitor::ProcessAttributionTracker tracker;
    auto raw = Snapshot(1, 10, 30, 20);
    codex_monitor::PerformanceSnapshot none;
    tracker.Apply(raw, false, none);
    Expect(Near(none.targetIoReadBytesPerSecond, 0.0) &&
               Near(none.targetIoWriteBytesPerSecond, 0.0) &&
               !none.targetIoNeedsBaseline,
           "an available process list with no target is a real zero");

    tracker.Reset();
    raw.processes.push_back(Process(
        10, L"codex.exe", true, 7, 10, 1'000, 1'000));
    codex_monitor::PerformanceSnapshot afterReset;
    afterReset.targetProcessCount = 1;
    tracker.Apply(raw, false, afterReset);
    Expect(afterReset.targetIoNeedsBaseline,
           "pause or reset must discard target I/O continuity");
}

void TestSlowCpuRankingUsesWholeMachineShare() {
    codex_monitor::ProcessAttributionTracker tracker;
    auto first = Snapshot(1, 100, 500, 300);
    first.processes.push_back(Process(
        10, L"small.exe", false, 1, 100, 0, 0));
    first.processes.push_back(Process(
        20, L"large.exe", false, 2, 200, 0, 0));
    codex_monitor::PerformanceSnapshot initial;
    tracker.Apply(first, true, initial);
    Expect(!initial.topCpuRankingAvailable,
           "the first slow process CPU sample must establish a baseline");

    auto second = Snapshot(21, 200, 900, 500);
    second.processes.push_back(Process(
        10, L"small.exe", false, 1, 150, 0, 0));
    second.processes.push_back(Process(
        20, L"large.exe", false, 2, 400, 0, 0));
    second.processes.push_back(Process(
        30, L"new.exe", false, 3, 900, 0, 0));
    codex_monitor::PerformanceSnapshot ranked;
    tracker.Apply(second, true, ranked);
    Expect(ranked.topCpuRankingAvailable &&
               ranked.topCpuProcesses.size() == 2 &&
               ranked.topCpuProcesses[0].processId == 20 &&
               ranked.topCpuProcesses[1].processId == 10,
           "slow CPU ranking must order only identity-matched processes");
    Expect(std::fabs(ranked.topCpuProcesses[0].wholeMachineCpuPercent -
                     (200.0 / 600.0 * 100.0)) < 0.0001,
           "process CPU ranking must use the whole-machine CPU denominator");
}

}  // namespace

int main() {
    TestTargetIoNeedsBaselineThenUsesFiveSecondDelta();
    TestTargetIoChurnAndCounterResetStayConservative();
    TestNoTargetIsARealZeroAndResetDropsContinuity();
    TestSlowCpuRankingUsesWholeMachineShare();
    if (failures != 0) return 1;
    std::cout << "process_attribution_tests=pass\n";
    return 0;
}
