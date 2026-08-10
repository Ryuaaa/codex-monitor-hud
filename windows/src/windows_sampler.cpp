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

void CaptureProcessMetrics(ProcessSnapshot& process) {
    if (process.processId == 0) return;

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.processId);
    if (!handle) return;

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
        process.cpuTimeAvailable = true;
        process.creationTime100ns = FileTimeToUint64(creation);
        process.cpuTime100ns = SaturatingAdd(FileTimeToUint64(kernel), FileTimeToUint64(user));
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = static_cast<DWORD>(sizeof(memory));
    if (GetProcessMemoryInfo(handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                             static_cast<DWORD>(sizeof(memory)))) {
        process.workingSetAvailable = true;
        process.workingSetBytes = static_cast<std::uint64_t>(memory.WorkingSetSize);
    }
    CloseHandle(handle);
}

void CaptureProcesses(RawPerformanceSnapshot& destination) {
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
        CaptureProcessMetrics(process);
    }
}

}  // namespace

RawPerformanceSnapshot WindowsSampler::CaptureRawSnapshot() const {
    RawPerformanceSnapshot snapshot{};
    CaptureSystemCpu(snapshot.systemCpu);
    CapturePhysicalMemory(snapshot);
    CaptureCommitMemory(snapshot);
    CapturePageFiles(snapshot);
    CaptureProcesses(snapshot);
    return snapshot;
}

PerformanceSnapshot WindowsSampler::BuildPerformanceSnapshot(RawPerformanceSnapshot raw) {
    PerformanceSnapshot snapshot{};
    snapshot.systemCpuNeedsBaseline = raw.systemCpu.available && !previousSystemCpu_.has_value();

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
        if (process.workingSetAvailable) {
            ++snapshot.readableWorkingSetProcessCount;
        }
        if (process.processId != 0 && !process.cpuTimeAvailable && !process.workingSetAvailable) {
            ++snapshot.unreadableProcessMetricCount;
        }

        if (process.isTargetRoot) ++snapshot.targetRootCount;
        if (process.isTargetTree) {
            ++snapshot.targetProcessCount;
            if (process.workingSetAvailable) {
                snapshot.targetWorkingSetAvailable = true;
                snapshot.targetWorkingSetBytes =
                    SaturatingAdd(snapshot.targetWorkingSetBytes, process.workingSetBytes);
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

        if (process.cpuTimeAvailable) {
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

    snapshot.topMemoryProcesses = SelectTopMemoryProcesses(raw.processes, 5);
    snapshot.raw = std::move(raw);

    if (snapshot.raw.systemCpu.available) previousSystemCpu_ = snapshot.raw.systemCpu;
    previousProcessCpu_ = std::move(nextProcessCpu);
    return snapshot;
}

PerformanceSnapshot WindowsSampler::Sample() {
    return BuildPerformanceSnapshot(CaptureRawSnapshot());
}

void WindowsSampler::ResetCpuBaseline() {
    previousSystemCpu_.reset();
    previousProcessCpu_.clear();
}

}  // namespace codex_monitor
