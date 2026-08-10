#include "performance_diagnosis.h"

#include <iostream>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

codex_monitor::PerformanceSnapshot Snapshot(double cpu,
                                            std::uint32_t memoryLoad,
                                            std::uint64_t commitUsed,
                                            std::uint64_t commitLimit) {
    codex_monitor::PerformanceSnapshot snapshot;
    snapshot.systemCpuPercent = cpu;
    snapshot.raw.physicalMemoryAvailable = true;
    snapshot.raw.physicalTotalBytes = 16ULL * 1024 * 1024 * 1024;
    snapshot.raw.physicalMemoryLoadPercent = memoryLoad;
    snapshot.raw.physicalAvailableBytes =
        snapshot.raw.physicalTotalBytes * (100 - memoryLoad) / 100;
    snapshot.raw.commitAvailable = true;
    snapshot.raw.commitTotalBytes = commitUsed;
    snapshot.raw.commitLimitBytes = commitLimit;
    snapshot.raw.processListAvailable = true;
    return snapshot;
}

void MarkTargetPresent(codex_monitor::PerformanceSnapshot& snapshot) {
    snapshot.targetRootCount = 1;
    snapshot.targetProcessCount = 1;
}

void TestUnavailableDataDoesNotGuess() {
    const auto result = codex_monitor::DiagnosePerformance({});
    Expect(result.pressure == codex_monitor::SystemPressure::kUnavailable &&
               result.bottleneck == codex_monitor::SystemBottleneck::kUnavailable &&
               result.targetImpact == codex_monitor::TargetImpact::kUnavailable &&
               result.confidence == codex_monitor::DiagnosisConfidence::kLow,
           "missing metrics must remain unavailable rather than simulated");
}

void TestComfortableComputerDoesNotBlameTargetApps() {
    auto snapshot = Snapshot(32.0, 55, 8, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetCpuPercent = 12.0;
    snapshot.targetWorkingSetAvailable = true;
    snapshot.targetWorkingSetBytes = 3ULL * 1024 * 1024 * 1024;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.pressure == codex_monitor::SystemPressure::kComfortable &&
               result.bottleneck == codex_monitor::SystemBottleneck::kNone &&
               result.targetImpact == codex_monitor::TargetImpact::kLow &&
               result.confidence == codex_monitor::DiagnosisConfidence::kHigh,
           "complete comfortable evidence must not blame the target apps");
}

void TestCpuPressureCanIdentifyHighTargetAppImpact() {
    auto snapshot = Snapshot(92.0, 60, 9, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetCpuPercent = 48.0;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.pressure == codex_monitor::SystemPressure::kHigh &&
               result.bottleneck == codex_monitor::SystemBottleneck::kCpu &&
               result.targetImpact == codex_monitor::TargetImpact::kHigh &&
               result.confidence == codex_monitor::DiagnosisConfidence::kHigh,
           "a large target-app CPU share must be identified during CPU pressure");
}

void TestMemoryPressureCanIdentifyPossibleTargetAppImpact() {
    auto snapshot = Snapshot(40.0, 90, 18, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetWorkingSetAvailable = true;
    snapshot.targetWorkingSetBytes = 3ULL * 1024 * 1024 * 1024;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.pressure == codex_monitor::SystemPressure::kHigh &&
               result.bottleneck == codex_monitor::SystemBottleneck::kMemory &&
               result.targetImpact == codex_monitor::TargetImpact::kPossible &&
               result.confidence == codex_monitor::DiagnosisConfidence::kMedium,
           "working set may show a memory contribution but cannot prove ownership");
}

void TestCommitOnlyPressureDoesNotMisattributeWorkingSet() {
    auto snapshot = Snapshot(35.0, 60, 19, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetWorkingSetAvailable = true;
    snapshot.targetWorkingSetBytes = 6ULL * 1024 * 1024 * 1024;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.pressure == codex_monitor::SystemPressure::kElevated &&
               result.bottleneck == codex_monitor::SystemBottleneck::kMemory &&
               result.targetImpact == codex_monitor::TargetImpact::kUnavailable &&
               result.confidence == codex_monitor::DiagnosisConfidence::kLow,
           "working set must not be treated as process commit attribution");
}

void TestMixedPressureAndPartialMetricsReduceConfidence() {
    auto snapshot = Snapshot(97.0, 96, 18, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetCpuPercent = 22.0;
    snapshot.targetCpuPartial = true;
    snapshot.targetWorkingSetAvailable = true;
    snapshot.targetWorkingSetBytes = 7ULL * 1024 * 1024 * 1024;
    snapshot.targetWorkingSetPartial = true;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.pressure == codex_monitor::SystemPressure::kHigh &&
               result.bottleneck == codex_monitor::SystemBottleneck::kMixed &&
               result.targetImpact == codex_monitor::TargetImpact::kHigh &&
               result.confidence == codex_monitor::DiagnosisConfidence::kMedium,
           "partial target metrics may preserve an alert but must lower confidence");
}

void TestNoTargetProcessIsReportedDirectly() {
    const auto result = codex_monitor::DiagnosePerformance(
        Snapshot(88.0, 82, 10, 20));
    Expect(result.targetImpact == codex_monitor::TargetImpact::kNotDetected,
           "an available process list with no target must report not detected");
}

void TestIncompleteLowSystemEvidenceDoesNotClaimComfortable() {
    codex_monitor::PerformanceSnapshot snapshot;
    snapshot.systemCpuPercent = 20.0;
    snapshot.raw.processListAvailable = true;
    MarkTargetPresent(snapshot);
    snapshot.targetCpuPercent = 2.0;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.pressure == codex_monitor::SystemPressure::kUnavailable &&
               result.targetImpact == codex_monitor::TargetImpact::kUnavailable,
           "a low CPU sample alone cannot prove that the computer is comfortable");
}

void TestPartialLowTargetMetricStaysUnknown() {
    auto snapshot = Snapshot(92.0, 60, 9, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetCpuPercent = 4.0;
    snapshot.targetCpuPartial = true;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.targetImpact == codex_monitor::TargetImpact::kUnavailable &&
               result.confidence == codex_monitor::DiagnosisConfidence::kLow,
           "a low partial process total is a lower bound, not proof of low impact");
}

void TestInvalidTargetCpuIsNotSilentlyClamped() {
    auto snapshot = Snapshot(92.0, 60, 9, 20);
    MarkTargetPresent(snapshot);
    snapshot.targetCpuPercent = 99.0;
    const auto result = codex_monitor::DiagnosePerformance(snapshot);
    Expect(result.targetImpact == codex_monitor::TargetImpact::kUnavailable,
           "target CPU above system CPU must be rejected as inconsistent evidence");
}

}  // namespace

int main() {
    TestUnavailableDataDoesNotGuess();
    TestComfortableComputerDoesNotBlameTargetApps();
    TestCpuPressureCanIdentifyHighTargetAppImpact();
    TestMemoryPressureCanIdentifyPossibleTargetAppImpact();
    TestCommitOnlyPressureDoesNotMisattributeWorkingSet();
    TestMixedPressureAndPartialMetricsReduceConfidence();
    TestNoTargetProcessIsReportedDirectly();
    TestIncompleteLowSystemEvidenceDoesNotClaimComfortable();
    TestPartialLowTargetMetricStaysUnknown();
    TestInvalidTargetCpuIsNotSilentlyClamped();
    if (failures != 0) return 1;
    std::cout << "performance_diagnosis_tests=pass\n";
    return 0;
}
