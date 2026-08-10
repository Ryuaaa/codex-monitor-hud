#pragma once

#include "performance_snapshot.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace codex_monitor {

class WindowsSampler {
public:
    PerformanceSnapshot Sample();
    void ResetCpuBaseline();

private:
    struct ProcessCpuBaseline {
        std::uint64_t creationTime100ns = 0;
        std::uint64_t cpuTime100ns = 0;
    };

    RawPerformanceSnapshot CaptureRawSnapshot() const;
    PerformanceSnapshot BuildPerformanceSnapshot(RawPerformanceSnapshot raw);

    std::optional<CpuTimes> previousSystemCpu_;
    std::unordered_map<std::uint32_t, ProcessCpuBaseline> previousProcessCpu_;
};

}  // namespace codex_monitor
