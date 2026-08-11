#pragma once

#include "system_io_rate.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace codex_monitor {

struct CpuTimes {
    bool available = false;
    std::uint64_t idle100ns = 0;
    std::uint64_t kernel100ns = 0;
    std::uint64_t user100ns = 0;
};

struct ProcessSnapshot {
    std::uint32_t processId = 0;
    std::uint32_t parentProcessId = 0;
    std::wstring executableName;
    bool isTargetRoot = false;
    bool isTargetTree = false;

    bool cpuTimeAttempted = false;
    bool cpuTimeAvailable = false;
    std::uint64_t creationTime100ns = 0;
    std::uint64_t cpuTime100ns = 0;

    bool workingSetAttempted = false;
    bool workingSetAvailable = false;
    std::uint64_t workingSetBytes = 0;

    // IO_COUNTERS is cumulative process I/O. It includes more than physical
    // disk traffic, so the UI must label derived rates as process I/O.
    bool ioAttempted = false;
    bool ioAvailable = false;
    std::uint64_t ioReadTransferBytes = 0;
    std::uint64_t ioWriteTransferBytes = 0;
};

struct RawPerformanceSnapshot {
    bool capturedAtUnbiasedTimeAvailable = false;
    std::uint64_t capturedAtUnbiased100ns = 0;
    CpuTimes systemCpu;
    SystemIoCounters systemIo;

    bool physicalMemoryAvailable = false;
    std::uint64_t physicalTotalBytes = 0;
    std::uint64_t physicalAvailableBytes = 0;
    std::uint32_t physicalMemoryLoadPercent = 0;

    bool commitAvailable = false;
    std::uint64_t commitTotalBytes = 0;
    std::uint64_t commitLimitBytes = 0;
    std::uint64_t commitPeakBytes = 0;

    bool pageFileAvailable = false;
    std::uint64_t pageFileTotalBytes = 0;
    std::uint64_t pageFileUsedBytes = 0;
    std::uint64_t pageFilePeakBytes = 0;

    bool processListAvailable = false;
    std::vector<ProcessSnapshot> processes;
};

struct RankedProcess {
    std::uint32_t processId = 0;
    std::wstring executableName;
    std::uint64_t workingSetBytes = 0;
};

struct RankedCpuProcess {
    std::uint32_t processId = 0;
    std::wstring executableName;
    double wholeMachineCpuPercent = 0.0;
};

struct PerformanceSnapshot {
    RawPerformanceSnapshot raw;

    std::optional<double> systemCpuPercent;
    bool systemCpuNeedsBaseline = false;
    SystemIoRates systemIoRates;

    std::optional<double> targetCpuPercent;
    bool targetCpuPartial = false;
    std::uint64_t targetWorkingSetBytes = 0;
    bool targetWorkingSetAvailable = false;
    bool targetWorkingSetPartial = false;
    std::optional<RankedProcess> largestTargetWorkingSetProcess;
    std::uint32_t targetRootCount = 0;
    std::uint32_t targetProcessCount = 0;

    std::optional<double> targetIoReadBytesPerSecond;
    std::optional<double> targetIoWriteBytesPerSecond;
    bool targetIoNeedsBaseline = false;
    bool targetIoPartial = false;

    std::uint32_t readableWorkingSetProcessCount = 0;
    std::uint32_t unreadableProcessMetricCount = 0;
    bool topMemoryRankingAvailable = false;
    std::vector<RankedProcess> topMemoryProcesses;

    bool topCpuRankingAvailable = false;
    std::uint32_t unreadableCpuProcessCount = 0;
    std::vector<RankedCpuProcess> topCpuProcesses;
};

}  // namespace codex_monitor
