#pragma once

#include "performance_snapshot.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace codex_monitor {

inline std::wstring NormalizedExecutableName(const std::wstring& rawName) {
    const std::size_t separator = rawName.find_last_of(L"\\/");
    std::wstring name = separator == std::wstring::npos ? rawName : rawName.substr(separator + 1);
    for (wchar_t& character : name) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    return name;
}

inline bool IsTargetRootExecutable(const std::wstring& rawName) {
    constexpr std::array<const wchar_t*, 6> kTargetExecutables = {
        L"codex.exe",
        L"codex-cli.exe",
        L"codex-app-server.exe",
        L"chatgpt.exe",
        L"chatgptdesktop.exe",
        L"openai.chatgpt.exe",
    };
    const std::wstring normalized = NormalizedExecutableName(rawName);
    return std::find_if(kTargetExecutables.begin(), kTargetExecutables.end(),
                        [&normalized](const wchar_t* candidate) {
                            return normalized == candidate;
                        }) != kTargetExecutables.end();
}

inline std::unordered_set<std::uint32_t> BuildTargetProcessSet(
    const std::vector<ProcessSnapshot>& processes) {
    std::unordered_set<std::uint32_t> targetProcessIds;
    targetProcessIds.reserve(processes.size());

    for (const ProcessSnapshot& process : processes) {
        if (process.processId != 0 && IsTargetRootExecutable(process.executableName)) {
            targetProcessIds.insert(process.processId);
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const ProcessSnapshot& process : processes) {
            if (process.processId == 0 || process.processId == process.parentProcessId ||
                targetProcessIds.find(process.processId) != targetProcessIds.end()) {
                continue;
            }
            if (targetProcessIds.find(process.parentProcessId) != targetProcessIds.end()) {
                targetProcessIds.insert(process.processId);
                changed = true;
            }
        }
    }
    return targetProcessIds;
}

inline std::optional<std::uint64_t> SystemCpuTotalDelta(const CpuTimes& previous,
                                                        const CpuTimes& current) {
    if (!previous.available || !current.available || current.kernel100ns < previous.kernel100ns ||
        current.user100ns < previous.user100ns || current.idle100ns < previous.idle100ns) {
        return std::nullopt;
    }
    const std::uint64_t kernelDelta = current.kernel100ns - previous.kernel100ns;
    const std::uint64_t userDelta = current.user100ns - previous.user100ns;
    if (kernelDelta > std::numeric_limits<std::uint64_t>::max() - userDelta) {
        return std::nullopt;
    }
    const std::uint64_t totalDelta = kernelDelta + userDelta;
    if (totalDelta == 0) return std::nullopt;
    return totalDelta;
}

inline std::optional<double> ComputeSystemCpuPercent(const CpuTimes& previous,
                                                     const CpuTimes& current) {
    const std::optional<std::uint64_t> totalDelta = SystemCpuTotalDelta(previous, current);
    if (!totalDelta) return std::nullopt;

    const std::uint64_t idleDelta = current.idle100ns - previous.idle100ns;
    if (idleDelta > *totalDelta) return std::nullopt;
    const double percent = 100.0 * static_cast<double>(*totalDelta - idleDelta) /
                           static_cast<double>(*totalDelta);
    return std::clamp(percent, 0.0, 100.0);
}

inline std::optional<double> ComputeWholeMachineCpuShare(std::uint64_t processCpuDelta100ns,
                                                         std::uint64_t systemCpuTotalDelta100ns) {
    if (systemCpuTotalDelta100ns == 0) return std::nullopt;
    const double percent = 100.0 * static_cast<double>(processCpuDelta100ns) /
                           static_cast<double>(systemCpuTotalDelta100ns);
    return std::clamp(percent, 0.0, 100.0);
}

inline std::vector<RankedProcess> SelectTopMemoryProcesses(
    const std::vector<ProcessSnapshot>& processes, std::size_t limit) {
    std::vector<RankedProcess> ranked;
    ranked.reserve(processes.size());
    for (const ProcessSnapshot& process : processes) {
        if (!process.workingSetAvailable) continue;
        ranked.push_back({process.processId, process.executableName, process.workingSetBytes});
    }

    std::sort(ranked.begin(), ranked.end(), [](const RankedProcess& left,
                                                const RankedProcess& right) {
        if (left.workingSetBytes != right.workingSetBytes) {
            return left.workingSetBytes > right.workingSetBytes;
        }
        const std::wstring leftName = NormalizedExecutableName(left.executableName);
        const std::wstring rightName = NormalizedExecutableName(right.executableName);
        if (leftName != rightName) return leftName < rightName;
        return left.processId < right.processId;
    });
    if (ranked.size() > limit) ranked.resize(limit);
    return ranked;
}

inline std::uint64_t SaturatingAdd(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

inline std::uint64_t SaturatingMultiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

}  // namespace codex_monitor
