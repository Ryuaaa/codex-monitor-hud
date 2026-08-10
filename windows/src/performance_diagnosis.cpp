#include "performance_diagnosis.h"

#include <algorithm>
#include <cmath>

namespace codex_monitor {
namespace {

std::optional<double> ValidPercent(std::optional<double> value) noexcept {
    if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 100.0) {
        return std::nullopt;
    }
    return value;
}

int CpuSeverity(double percent) noexcept {
    if (percent >= 90.0) return 3;
    if (percent >= 75.0) return 2;
    return 1;
}

int MemorySeverity(std::optional<double> physical,
                   std::optional<double> commit) noexcept {
    if (physical && commit) {
        if (*physical >= 90.0 && *commit >= 90.0) return 3;
        if (*physical >= 85.0 || *commit >= 85.0) return 2;
        return 1;
    }
    const auto single = physical ? physical : commit;
    if (single && *single >= 90.0) return 2;
    if (!single) return 0;
    return 1;
}

SystemPressure PressureFromSeverity(int severity) noexcept {
    switch (severity) {
        case 1:
            return SystemPressure::kComfortable;
        case 2:
            return SystemPressure::kElevated;
        case 3:
            return SystemPressure::kHigh;
        default:
            return SystemPressure::kUnavailable;
    }
}

DiagnosisConfidence CapAtMedium(DiagnosisConfidence confidence) noexcept {
    if (confidence == DiagnosisConfidence::kHigh) {
        return DiagnosisConfidence::kMedium;
    }
    return confidence;
}

int CpuImpactScore(const PerformanceSnapshot& snapshot,
                   const PerformanceDiagnosis& diagnosis,
                   bool& evaluated,
                   bool& inconclusiveLowerBound) noexcept {
    const auto target = ValidPercent(snapshot.targetCpuPercent);
    if (!diagnosis.cpuPercent || !target || *diagnosis.cpuPercent <= 0.0 ||
        *target > *diagnosis.cpuPercent + 0.5) {
        return 0;
    }
    evaluated = true;
    const double shareOfBusyCpu = *target / *diagnosis.cpuPercent;
    if (*target >= 25.0 && shareOfBusyCpu >= 0.40) return 2;
    if (*target >= 10.0 && shareOfBusyCpu >= 0.15) return 1;
    inconclusiveLowerBound = snapshot.targetCpuPartial;
    return 0;
}

int MemoryImpactScore(const PerformanceSnapshot& snapshot,
                      const PerformanceDiagnosis& diagnosis,
                      bool& evaluated,
                      bool& inconclusiveLowerBound) noexcept {
    if (!snapshot.targetWorkingSetAvailable ||
        !snapshot.raw.physicalMemoryAvailable ||
        snapshot.raw.physicalTotalBytes == 0 ||
        !diagnosis.physicalMemoryPercent ||
        *diagnosis.physicalMemoryPercent < 85.0) {
        return 0;
    }
    evaluated = true;
    const double shareOfPhysicalMemory =
        static_cast<double>(snapshot.targetWorkingSetBytes) /
        static_cast<double>(snapshot.raw.physicalTotalBytes);
    if (shareOfPhysicalMemory >= 0.30) return 2;
    if (shareOfPhysicalMemory >= 0.15) return 1;
    inconclusiveLowerBound = snapshot.targetWorkingSetPartial;
    return 0;
}

}  // namespace

PerformanceDiagnosis DiagnosePerformance(
    const PerformanceSnapshot& snapshot) noexcept {
    PerformanceDiagnosis diagnosis;
    diagnosis.cpuPercent = ValidPercent(snapshot.systemCpuPercent);
    if (snapshot.raw.physicalMemoryAvailable &&
        snapshot.raw.physicalMemoryLoadPercent <= 100) {
        diagnosis.physicalMemoryPercent =
            static_cast<double>(snapshot.raw.physicalMemoryLoadPercent);
    }
    if (snapshot.raw.commitAvailable && snapshot.raw.commitLimitBytes > 0) {
        const double percent = 100.0 *
            static_cast<double>(snapshot.raw.commitTotalBytes) /
            static_cast<double>(snapshot.raw.commitLimitBytes);
        diagnosis.commitPercent = ValidPercent(percent);
    }

    const int cpu = diagnosis.cpuPercent ? CpuSeverity(*diagnosis.cpuPercent) : 0;
    const int memory = MemorySeverity(diagnosis.physicalMemoryPercent,
                                      diagnosis.commitPercent);
    const int maximum = std::max(cpu, memory);
    const bool completeSystemEvidence = diagnosis.cpuPercent &&
                                        diagnosis.physicalMemoryPercent &&
                                        diagnosis.commitPercent;

    // Missing a low metric cannot prove that the computer is comfortable.
    // Incomplete evidence may still raise an alert when an available metric is high.
    if (maximum == 0 || (!completeSystemEvidence && maximum < 2)) {
        diagnosis.pressure = SystemPressure::kUnavailable;
        diagnosis.bottleneck = SystemBottleneck::kUnavailable;
        diagnosis.confidence = DiagnosisConfidence::kLow;
    } else {
        diagnosis.pressure = PressureFromSeverity(maximum);
        if (completeSystemEvidence) {
            diagnosis.confidence = maximum == 2
                ? DiagnosisConfidence::kMedium
                : DiagnosisConfidence::kHigh;
        } else {
            diagnosis.confidence = DiagnosisConfidence::kMedium;
        }

        if (maximum == 1) {
            diagnosis.bottleneck = SystemBottleneck::kNone;
        } else if (cpu == memory) {
            diagnosis.bottleneck = SystemBottleneck::kMixed;
        } else if (cpu > memory) {
            diagnosis.bottleneck = SystemBottleneck::kCpu;
        } else {
            diagnosis.bottleneck = SystemBottleneck::kMemory;
        }
    }

    if (!snapshot.raw.processListAvailable) {
        diagnosis.targetImpact = TargetImpact::kUnavailable;
        diagnosis.confidence = DiagnosisConfidence::kLow;
        return diagnosis;
    }
    if (snapshot.targetProcessCount == 0) {
        diagnosis.targetImpact = TargetImpact::kNotDetected;
        return diagnosis;
    }
    if (diagnosis.pressure == SystemPressure::kComfortable) {
        diagnosis.targetImpact = TargetImpact::kLow;
        return diagnosis;
    }
    if (diagnosis.pressure == SystemPressure::kUnavailable) {
        diagnosis.targetImpact = TargetImpact::kUnavailable;
        diagnosis.confidence = DiagnosisConfidence::kLow;
        return diagnosis;
    }

    bool evaluated = false;
    bool inconclusiveLowerBound = false;
    int impactScore = 0;
    if (diagnosis.bottleneck == SystemBottleneck::kCpu ||
        diagnosis.bottleneck == SystemBottleneck::kMixed) {
        bool cpuEvaluated = false;
        bool cpuInconclusive = false;
        impactScore = std::max(impactScore, CpuImpactScore(
            snapshot, diagnosis, cpuEvaluated, cpuInconclusive));
        evaluated = evaluated || cpuEvaluated;
        inconclusiveLowerBound = inconclusiveLowerBound || cpuInconclusive;
        if (cpuEvaluated && snapshot.targetCpuPartial) {
            diagnosis.confidence = CapAtMedium(diagnosis.confidence);
        }
    }
    if (diagnosis.bottleneck == SystemBottleneck::kMemory ||
        diagnosis.bottleneck == SystemBottleneck::kMixed) {
        bool memoryEvaluated = false;
        bool memoryInconclusive = false;
        impactScore = std::max(impactScore, MemoryImpactScore(
            snapshot, diagnosis, memoryEvaluated, memoryInconclusive));
        evaluated = evaluated || memoryEvaluated;
        inconclusiveLowerBound = inconclusiveLowerBound || memoryInconclusive;
        if (memoryEvaluated) {
            // A working set contains shared pages, so it is trend evidence only.
            diagnosis.confidence = CapAtMedium(diagnosis.confidence);
        }
    }

    if (!evaluated || (impactScore == 0 && inconclusiveLowerBound)) {
        diagnosis.targetImpact = TargetImpact::kUnavailable;
        diagnosis.confidence = DiagnosisConfidence::kLow;
    } else if (impactScore >= 2) {
        diagnosis.targetImpact = TargetImpact::kHigh;
    } else if (impactScore == 1) {
        diagnosis.targetImpact = TargetImpact::kPossible;
    } else {
        diagnosis.targetImpact = TargetImpact::kLow;
    }
    return diagnosis;
}

}  // namespace codex_monitor
