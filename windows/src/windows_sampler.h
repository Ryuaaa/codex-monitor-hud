#pragma once

#include "performance_snapshot.h"
#include "system_io_sampler_win32.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace codex_monitor {

enum class SampleMode {
    kFast,
    kFastAndSlow,
};

class WindowsSampler {
public:
    PerformanceSnapshot Sample(SampleMode mode);
    void ResetCpuBaseline();

private:
    struct ProcessCpuBaseline {
        std::uint64_t creationTime100ns = 0;
        std::uint64_t cpuTime100ns = 0;
    };

    struct SlowMetricsCache {
        bool commitAvailable = false;
        std::uint64_t commitTotalBytes = 0;
        std::uint64_t commitLimitBytes = 0;
        std::uint64_t commitPeakBytes = 0;

        bool pageFileAvailable = false;
        std::uint64_t pageFileTotalBytes = 0;
        std::uint64_t pageFileUsedBytes = 0;
        std::uint64_t pageFilePeakBytes = 0;

        bool rankingAvailable = false;
        std::uint32_t readableWorkingSetProcessCount = 0;
        std::uint32_t unreadableProcessMetricCount = 0;
        std::vector<RankedProcess> topMemoryProcesses;
    };

    RawPerformanceSnapshot CaptureRawSnapshot(SampleMode mode);
    PerformanceSnapshot BuildPerformanceSnapshot(RawPerformanceSnapshot raw, SampleMode mode);
    void UpdateAndApplySlowMetrics(RawPerformanceSnapshot& raw,
                                   PerformanceSnapshot& snapshot,
                                   SampleMode mode);

    std::optional<CpuTimes> previousSystemCpu_;
    std::unordered_map<std::uint32_t, ProcessCpuBaseline> previousProcessCpu_;
    SystemIoCounters previousSystemIo_;
    WindowsSystemIoCounterSampler systemIoSampler_;
    SlowMetricsCache slowMetricsCache_;
};

}  // namespace codex_monitor
