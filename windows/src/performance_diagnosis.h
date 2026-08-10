#pragma once

#include "performance_snapshot.h"

#include <optional>

namespace codex_monitor {

enum class SystemPressure {
    kUnavailable,
    kComfortable,
    kElevated,
    kHigh,
};

enum class SystemBottleneck {
    kUnavailable,
    kNone,
    kCpu,
    kMemory,
    kMixed,
};

enum class TargetImpact {
    kUnavailable,
    kNotDetected,
    kLow,
    kPossible,
    kHigh,
};

enum class DiagnosisConfidence {
    kLow,
    kMedium,
    kHigh,
};

struct PerformanceDiagnosis {
    SystemPressure pressure = SystemPressure::kUnavailable;
    SystemBottleneck bottleneck = SystemBottleneck::kUnavailable;
    TargetImpact targetImpact = TargetImpact::kUnavailable;
    DiagnosisConfidence confidence = DiagnosisConfidence::kLow;
    std::optional<double> cpuPercent;
    std::optional<double> physicalMemoryPercent;
    std::optional<double> commitPercent;
};

// Produces a trend-oriented diagnosis from metrics already collected by the
// sampler. It performs no I/O and never starts additional sampling work.
PerformanceDiagnosis DiagnosePerformance(
    const PerformanceSnapshot& snapshot) noexcept;

}  // namespace codex_monitor
