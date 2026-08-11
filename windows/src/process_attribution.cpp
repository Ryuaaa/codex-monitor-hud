#include "process_attribution.h"

#include "snapshot_math.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace codex_monitor {
namespace {

std::optional<double> CounterRate(std::uint64_t previous,
                                  std::uint64_t current,
                                  std::uint64_t elapsed100ns) noexcept {
    if (current < previous || elapsed100ns == 0 ||
        elapsed100ns > kMaximumTargetIoInterval100ns) {
        return std::nullopt;
    }
    const double seconds = static_cast<double>(elapsed100ns) /
        static_cast<double>(kProcessAttributionHundredNanosecondsPerSecond);
    const double rate = static_cast<double>(current - previous) / seconds;
    return std::isfinite(rate) && rate >= 0.0
        ? std::optional<double>(rate)
        : std::nullopt;
}

void SortAndLimitCpu(std::vector<RankedCpuProcess>& ranked,
                     std::size_t limit) {
    std::sort(ranked.begin(), ranked.end(),
              [](const RankedCpuProcess& left,
                 const RankedCpuProcess& right) {
        if (left.wholeMachineCpuPercent != right.wholeMachineCpuPercent) {
            return left.wholeMachineCpuPercent >
                   right.wholeMachineCpuPercent;
        }
        const std::wstring leftName =
            NormalizedExecutableName(left.executableName);
        const std::wstring rightName =
            NormalizedExecutableName(right.executableName);
        if (leftName != rightName) return leftName < rightName;
        return left.processId < right.processId;
    });
    if (ranked.size() > limit) ranked.resize(limit);
}

}  // namespace

void ProcessAttributionTracker::Apply(
    const RawPerformanceSnapshot& raw,
    bool updateWholeMachineCpuRanking,
    PerformanceSnapshot& destination) {
    std::unordered_map<std::uint32_t, CounterBaseline> nextTargetIo;
    nextTargetIo.reserve(raw.processes.size());
    std::uint64_t readDelta = 0;
    std::uint64_t writeDelta = 0;
    std::size_t targetIoReadable = 0;
    std::size_t targetIoMatched = 0;
    std::size_t targetIoMissing = 0;

    const bool timeUsable = raw.capturedAtUnbiasedTimeAvailable &&
        (!previousTargetIoTime100ns_ ||
         raw.capturedAtUnbiased100ns > *previousTargetIoTime100ns_);
    const std::uint64_t elapsed100ns =
        timeUsable && previousTargetIoTime100ns_
            ? raw.capturedAtUnbiased100ns - *previousTargetIoTime100ns_
            : 0;
    const bool intervalUsable = elapsed100ns > 0 &&
        elapsed100ns <= kMaximumTargetIoInterval100ns;

    if (raw.processListAvailable) {
        for (const ProcessSnapshot& process : raw.processes) {
            if (!process.isTargetTree) continue;
            if (!process.ioAvailable) {
                ++targetIoMissing;
                continue;
            }
            ++targetIoReadable;
            nextTargetIo.emplace(
                process.processId,
                CounterBaseline{process.creationTime100ns,
                                process.ioReadTransferBytes,
                                process.ioWriteTransferBytes});
            if (!intervalUsable) continue;
            const auto previous = previousTargetIo_.find(process.processId);
            if (previous == previousTargetIo_.end() ||
                previous->second.creationTime100ns !=
                    process.creationTime100ns) {
                ++targetIoMissing;
                continue;
            }
            const auto readRate = CounterRate(
                previous->second.firstCounter,
                process.ioReadTransferBytes, elapsed100ns);
            const auto writeRate = CounterRate(
                previous->second.secondCounter,
                process.ioWriteTransferBytes, elapsed100ns);
            if (!readRate || !writeRate) {
                ++targetIoMissing;
                continue;
            }
            readDelta = SaturatingAdd(
                readDelta, process.ioReadTransferBytes -
                    previous->second.firstCounter);
            writeDelta = SaturatingAdd(
                writeDelta, process.ioWriteTransferBytes -
                    previous->second.secondCounter);
            ++targetIoMatched;
        }
    }

    if (raw.processListAvailable && destination.targetProcessCount == 0) {
        destination.targetIoReadBytesPerSecond = 0.0;
        destination.targetIoWriteBytesPerSecond = 0.0;
    } else if (targetIoMatched > 0 && intervalUsable) {
        const double seconds = static_cast<double>(elapsed100ns) /
            static_cast<double>(kProcessAttributionHundredNanosecondsPerSecond);
        destination.targetIoReadBytesPerSecond =
            static_cast<double>(readDelta) / seconds;
        destination.targetIoWriteBytesPerSecond =
            static_cast<double>(writeDelta) / seconds;
        destination.targetIoPartial = targetIoMissing > 0 ||
            targetIoMatched < previousTargetIo_.size();
    } else if (targetIoReadable > 0) {
        destination.targetIoNeedsBaseline = true;
    }

    if (raw.processListAvailable && raw.capturedAtUnbiasedTimeAvailable) {
        previousTargetIo_ = std::move(nextTargetIo);
        previousTargetIoTime100ns_ = raw.capturedAtUnbiased100ns;
    } else {
        previousTargetIo_.clear();
        previousTargetIoTime100ns_.reset();
    }

    if (!updateWholeMachineCpuRanking) return;

    std::unordered_map<std::uint32_t, CounterBaseline> nextSlowCpu;
    nextSlowCpu.reserve(raw.processes.size());
    std::vector<RankedCpuProcess> ranked;
    ranked.reserve(raw.processes.size());
    const std::optional<std::uint64_t> systemDelta = previousSlowSystemCpu_
        ? SystemCpuTotalDelta(*previousSlowSystemCpu_, raw.systemCpu)
        : std::nullopt;

    for (const ProcessSnapshot& process : raw.processes) {
        if (process.cpuTimeAttempted && !process.cpuTimeAvailable &&
            process.processId != 0) {
            ++destination.unreadableCpuProcessCount;
        }
        if (!process.cpuTimeAvailable) continue;
        nextSlowCpu.emplace(
            process.processId,
            CounterBaseline{process.creationTime100ns,
                            process.cpuTime100ns, 0});
        if (!systemDelta) continue;
        const auto previous = previousSlowCpu_.find(process.processId);
        if (previous == previousSlowCpu_.end() ||
            previous->second.creationTime100ns != process.creationTime100ns ||
            process.cpuTime100ns < previous->second.firstCounter) {
            continue;
        }
        const auto percent = ComputeWholeMachineCpuShare(
            process.cpuTime100ns - previous->second.firstCounter,
            *systemDelta);
        if (!percent) continue;
        ranked.push_back({process.processId, process.executableName, *percent});
    }

    if (systemDelta && !ranked.empty()) {
        SortAndLimitCpu(ranked, 5);
        destination.topCpuRankingAvailable = true;
        destination.topCpuProcesses = std::move(ranked);
    }
    if (raw.processListAvailable && raw.systemCpu.available) {
        previousSlowCpu_ = std::move(nextSlowCpu);
        previousSlowSystemCpu_ = raw.systemCpu;
    } else {
        previousSlowCpu_.clear();
        previousSlowSystemCpu_.reset();
    }
}

void ProcessAttributionTracker::Reset() noexcept {
    previousTargetIo_.clear();
    previousTargetIoTime100ns_.reset();
    previousSlowCpu_.clear();
    previousSlowSystemCpu_.reset();
}

}  // namespace codex_monitor
