#include "windows_sampler.h"

#include "snapshot_math.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace codex_monitor {
namespace {

std::uint64_t FileTimeToUint64(const FILETIME& value) {
    ULARGE_INTEGER combined{};
    combined.LowPart = value.dwLowDateTime;
    combined.HighPart = value.dwHighDateTime;
    return combined.QuadPart;
}

struct PageFileTotals {
    std::uint64_t pageSizeBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t usedBytes = 0;
    std::uint64_t peakBytes = 0;
};

BOOL CALLBACK CollectPageFileTotals(void* context,
                                    PENUM_PAGE_FILE_INFORMATION information,
                                    const wchar_t*) {
    if (!context || !information) return TRUE;
    auto* totals = static_cast<PageFileTotals*>(context);
    totals->totalBytes = SaturatingAdd(
        totals->totalBytes,
        SaturatingMultiply(static_cast<std::uint64_t>(information->TotalSize),
                           totals->pageSizeBytes));
    totals->usedBytes = SaturatingAdd(
        totals->usedBytes,
        SaturatingMultiply(static_cast<std::uint64_t>(information->TotalInUse),
                           totals->pageSizeBytes));
    totals->peakBytes = SaturatingAdd(
        totals->peakBytes,
        SaturatingMultiply(static_cast<std::uint64_t>(information->PeakUsage),
                           totals->pageSizeBytes));
    return TRUE;
}

void CaptureSystemCpu(CpuTimes& destination) {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) return;

    destination.available = true;
    destination.idle100ns = FileTimeToUint64(idle);
    destination.kernel100ns = FileTimeToUint64(kernel);
    destination.user100ns = FileTimeToUint64(user);
}

void CapturePhysicalMemory(RawPerformanceSnapshot& destination) {
    MEMORYSTATUSEX memory{};
    memory.dwLength = static_cast<DWORD>(sizeof(memory));
    if (!GlobalMemoryStatusEx(&memory)) return;

    destination.physicalMemoryAvailable = true;
    destination.physicalTotalBytes = static_cast<std::uint64_t>(memory.ullTotalPhys);
    destination.physicalAvailableBytes = static_cast<std::uint64_t>(memory.ullAvailPhys);
    destination.physicalMemoryLoadPercent = memory.dwMemoryLoad;
}

void CaptureCommitMemory(RawPerformanceSnapshot& destination) {
    PERFORMANCE_INFORMATION performance{};
    performance.cb = static_cast<DWORD>(sizeof(performance));
    if (!GetPerformanceInfo(&performance, static_cast<DWORD>(sizeof(performance)))) return;

    const std::uint64_t pageSize = static_cast<std::uint64_t>(performance.PageSize);
    destination.commitAvailable = true;
    destination.commitTotalBytes =
        SaturatingMultiply(static_cast<std::uint64_t>(performance.CommitTotal), pageSize);
    destination.commitLimitBytes =
        SaturatingMultiply(static_cast<std::uint64_t>(performance.CommitLimit), pageSize);
    destination.commitPeakBytes =
        SaturatingMultiply(static_cast<std::uint64_t>(performance.CommitPeak), pageSize);
}

void CapturePageFiles(RawPerformanceSnapshot& destination) {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    PageFileTotals totals{};
    totals.pageSizeBytes = static_cast<std::uint64_t>(systemInfo.dwPageSize);
    if (totals.pageSizeBytes == 0 || !EnumPageFilesW(CollectPageFileTotals, &totals)) return;

    destination.pageFileAvailable = true;
    destination.pageFileTotalBytes = totals.totalBytes;
    destination.pageFileUsedBytes = totals.usedBytes;
    destination.pageFilePeakBytes = totals.peakBytes;
}

void CaptureProcessMetrics(ProcessSnapshot& process,
                           bool captureAllProcessMemory,
                           bool captureAllProcessCpu) {
    if (process.processId == 0) return;

    const bool captureCpu = process.isTargetTree || captureAllProcessCpu;
    const bool captureMemory = process.isTargetTree || captureAllProcessMemory;
    const bool captureIo = process.isTargetTree;
    if (!captureCpu && !captureMemory && !captureIo) return;

    process.cpuTimeAttempted = captureCpu;
    process.workingSetAttempted = captureMemory;
    process.ioAttempted = captureIo;

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.processId);
    if (!handle) return;

    if (captureCpu) {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
            process.cpuTimeAvailable = true;
            process.creationTime100ns = FileTimeToUint64(creation);
            process.cpuTime100ns =
                SaturatingAdd(FileTimeToUint64(kernel), FileTimeToUint64(user));
        }
    }

    if (captureMemory) {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = static_cast<DWORD>(sizeof(memory));
        if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 static_cast<DWORD>(sizeof(memory)))) {
            process.workingSetAvailable = true;
            process.workingSetBytes = static_cast<std::uint64_t>(memory.WorkingSetSize);
        }
    }
    if (captureIo && process.cpuTimeAvailable) {
        IO_COUNTERS counters{};
        if (GetProcessIoCounters(handle, &counters)) {
            process.ioAvailable = true;
            process.ioReadTransferBytes =
                static_cast<std::uint64_t>(counters.ReadTransferCount);
            process.ioWriteTransferBytes =
                static_cast<std::uint64_t>(counters.WriteTransferCount);
        }
    }
    CloseHandle(handle);
}

void CaptureProcesses(RawPerformanceSnapshot& destination,
                      bool captureAllProcessMemory,
                      bool captureAllProcessCpu) {
    HANDLE processList = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processList == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    if (Process32FirstW(processList, &entry)) {
        do {
            ProcessSnapshot process{};
            process.processId = entry.th32ProcessID;
            process.parentProcessId = entry.th32ParentProcessID;
            process.executableName = entry.szExeFile;
            destination.processes.push_back(std::move(process));
        } while (Process32NextW(processList, &entry));
        destination.processListAvailable = true;
    }
    CloseHandle(processList);

    if (!destination.processListAvailable) return;
    const std::unordered_set<std::uint32_t> targetIds =
        BuildTargetProcessSet(destination.processes);
    for (ProcessSnapshot& process : destination.processes) {
        process.isTargetRoot = IsTargetRootExecutable(process.executableName);
        process.isTargetTree = targetIds.find(process.processId) != targetIds.end();
        CaptureProcessMetrics(
            process, captureAllProcessMemory, captureAllProcessCpu);
    }
}

}  // namespace

RawPerformanceSnapshot WindowsSampler::CaptureRawSnapshot(SampleMode mode) {
    RawPerformanceSnapshot snapshot{};
    ULONGLONG capturedAtUnbiased100ns = 0;
    if (QueryUnbiasedInterruptTime(&capturedAtUnbiased100ns)) {
        snapshot.capturedAtUnbiasedTimeAvailable = true;
        snapshot.capturedAtUnbiased100ns =
            static_cast<std::uint64_t>(capturedAtUnbiased100ns);
    }
    CaptureSystemCpu(snapshot.systemCpu);
    snapshot.systemIo = systemIoSampler_.Capture();
    CapturePhysicalMemory(snapshot);
    if (mode == SampleMode::kFastAndSlow) {
        CaptureCommitMemory(snapshot);
        CapturePageFiles(snapshot);
    }
    const bool slowSample = mode == SampleMode::kFastAndSlow;
    CaptureProcesses(snapshot, slowSample, slowSample);
    return snapshot;
}

void WindowsSampler::UpdateAndApplySlowMetrics(RawPerformanceSnapshot& raw,
                                               PerformanceSnapshot& snapshot,
                                               SampleMode mode) {
    if (mode == SampleMode::kFastAndSlow) {
        if (raw.commitAvailable) {
            slowMetricsCache_.commitAvailable = true;
            slowMetricsCache_.commitTotalBytes = raw.commitTotalBytes;
            slowMetricsCache_.commitLimitBytes = raw.commitLimitBytes;
            slowMetricsCache_.commitPeakBytes = raw.commitPeakBytes;
        }
        if (raw.pageFileAvailable) {
            slowMetricsCache_.pageFileAvailable = true;
            slowMetricsCache_.pageFileTotalBytes = raw.pageFileTotalBytes;
            slowMetricsCache_.pageFileUsedBytes = raw.pageFileUsedBytes;
            slowMetricsCache_.pageFilePeakBytes = raw.pageFilePeakBytes;
        }

        if (raw.processListAvailable) {
            std::uint32_t readableCount = 0;
            std::uint32_t unreadableCount = 0;
            for (const ProcessSnapshot& process : raw.processes) {
                if (!process.workingSetAttempted) continue;
                if (process.workingSetAvailable) {
                    ++readableCount;
                } else if (process.processId != 0) {
                    ++unreadableCount;
                }
            }
            if (readableCount > 0) {
                slowMetricsCache_.rankingAvailable = true;
                slowMetricsCache_.readableWorkingSetProcessCount = readableCount;
                slowMetricsCache_.unreadableProcessMetricCount = unreadableCount;
                slowMetricsCache_.topMemoryProcesses =
                    SelectTopMemoryProcesses(raw.processes, 5);
            } else if (!slowMetricsCache_.rankingAvailable) {
                snapshot.unreadableProcessMetricCount = unreadableCount;
            }

            if (snapshot.topCpuRankingAvailable) {
                slowMetricsCache_.cpuRankingAvailable = true;
                slowMetricsCache_.unreadableCpuProcessCount =
                    snapshot.unreadableCpuProcessCount;
                slowMetricsCache_.topCpuProcesses = snapshot.topCpuProcesses;
            } else if (!slowMetricsCache_.cpuRankingAvailable) {
                slowMetricsCache_.unreadableCpuProcessCount =
                    snapshot.unreadableCpuProcessCount;
            }
        }
    }

    if (slowMetricsCache_.commitAvailable) {
        raw.commitAvailable = true;
        raw.commitTotalBytes = slowMetricsCache_.commitTotalBytes;
        raw.commitLimitBytes = slowMetricsCache_.commitLimitBytes;
        raw.commitPeakBytes = slowMetricsCache_.commitPeakBytes;
    }
    if (slowMetricsCache_.pageFileAvailable) {
        raw.pageFileAvailable = true;
        raw.pageFileTotalBytes = slowMetricsCache_.pageFileTotalBytes;
        raw.pageFileUsedBytes = slowMetricsCache_.pageFileUsedBytes;
        raw.pageFilePeakBytes = slowMetricsCache_.pageFilePeakBytes;
    }
    if (slowMetricsCache_.rankingAvailable) {
        snapshot.topMemoryRankingAvailable = true;
        snapshot.readableWorkingSetProcessCount =
            slowMetricsCache_.readableWorkingSetProcessCount;
        snapshot.unreadableProcessMetricCount = slowMetricsCache_.unreadableProcessMetricCount;
        snapshot.topMemoryProcesses = slowMetricsCache_.topMemoryProcesses;
    }
    if (slowMetricsCache_.cpuRankingAvailable) {
        snapshot.topCpuRankingAvailable = true;
        snapshot.unreadableCpuProcessCount =
            slowMetricsCache_.unreadableCpuProcessCount;
        snapshot.topCpuProcesses = slowMetricsCache_.topCpuProcesses;
    } else if (snapshot.unreadableCpuProcessCount == 0) {
        snapshot.unreadableCpuProcessCount =
            slowMetricsCache_.unreadableCpuProcessCount;
    }
}

PerformanceSnapshot WindowsSampler::BuildPerformanceSnapshot(RawPerformanceSnapshot raw,
                                                               SampleMode mode) {
    PerformanceSnapshot snapshot{};
    snapshot.systemCpuNeedsBaseline = raw.systemCpu.available && !previousSystemCpu_.has_value();
    snapshot.systemIoRates = ComputeSystemIoRates(previousSystemIo_, raw.systemIo);

    std::optional<std::uint64_t> systemCpuDelta;
    if (previousSystemCpu_) {
        systemCpuDelta = SystemCpuTotalDelta(*previousSystemCpu_, raw.systemCpu);
        snapshot.systemCpuPercent = ComputeSystemCpuPercent(*previousSystemCpu_, raw.systemCpu);
    }

    std::unordered_map<std::uint32_t, ProcessCpuBaseline> nextProcessCpu;
    nextProcessCpu.reserve(raw.processes.size());
    std::uint64_t targetCpuDelta = 0;
    std::uint32_t targetCpuDeltaCount = 0;
    std::uint32_t targetCpuMissingCount = 0;

    for (const ProcessSnapshot& process : raw.processes) {
        if (process.isTargetRoot) ++snapshot.targetRootCount;
        if (process.isTargetTree) {
            ++snapshot.targetProcessCount;
            if (process.workingSetAvailable) {
                snapshot.targetWorkingSetAvailable = true;
                snapshot.targetWorkingSetBytes =
                    SaturatingAdd(snapshot.targetWorkingSetBytes, process.workingSetBytes);
                if (!snapshot.largestTargetWorkingSetProcess ||
                    process.workingSetBytes >
                        snapshot.largestTargetWorkingSetProcess->workingSetBytes) {
                    snapshot.largestTargetWorkingSetProcess = RankedProcess{
                        process.processId, process.executableName,
                        process.workingSetBytes};
                }
            } else {
                snapshot.targetWorkingSetPartial = true;
            }

            bool hasCpuDelta = false;
            if (process.cpuTimeAvailable) {
                const auto previous = previousProcessCpu_.find(process.processId);
                if (previous != previousProcessCpu_.end() &&
                    previous->second.creationTime100ns == process.creationTime100ns &&
                    process.cpuTime100ns >= previous->second.cpuTime100ns) {
                    targetCpuDelta = SaturatingAdd(
                        targetCpuDelta, process.cpuTime100ns - previous->second.cpuTime100ns);
                    ++targetCpuDeltaCount;
                    hasCpuDelta = true;
                }
            }
            if (!hasCpuDelta) ++targetCpuMissingCount;
        }

        if (process.isTargetTree && process.cpuTimeAvailable) {
            nextProcessCpu.emplace(
                process.processId,
                ProcessCpuBaseline{process.creationTime100ns, process.cpuTime100ns});
        }
    }

    if (raw.processListAvailable && snapshot.targetProcessCount == 0) {
        snapshot.targetCpuPercent = 0.0;
    } else if (systemCpuDelta && targetCpuDeltaCount > 0) {
        snapshot.targetCpuPercent = ComputeWholeMachineCpuShare(targetCpuDelta, *systemCpuDelta);
        snapshot.targetCpuPartial = targetCpuMissingCount > 0;
    }

    processAttributionTracker_.Apply(
        raw, mode == SampleMode::kFastAndSlow, snapshot);
    UpdateAndApplySlowMetrics(raw, snapshot, mode);
    snapshot.raw = std::move(raw);

    if (snapshot.raw.systemCpu.available) previousSystemCpu_ = snapshot.raw.systemCpu;
    if (snapshot.raw.systemIo.network.available) {
        previousSystemIo_.network = snapshot.raw.systemIo.network;
    }
    if (snapshot.raw.systemIo.disk.available) {
        previousSystemIo_.disk = snapshot.raw.systemIo.disk;
    }
    previousProcessCpu_ = std::move(nextProcessCpu);
    return snapshot;
}

PerformanceSnapshot WindowsSampler::Sample(SampleMode mode) {
    return BuildPerformanceSnapshot(CaptureRawSnapshot(mode), mode);
}

void WindowsSampler::ResetCpuBaseline() {
    previousSystemCpu_.reset();
    previousProcessCpu_.clear();
    previousSystemIo_ = {};
    processAttributionTracker_.Reset();
}

}  // namespace codex_monitor
