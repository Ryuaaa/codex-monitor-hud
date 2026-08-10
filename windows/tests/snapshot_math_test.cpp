#include "snapshot_math.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using codex_monitor::CpuTimes;
using codex_monitor::ProcessSnapshot;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void ExpectNear(double actual, double expected, const char* message) {
    Expect(std::fabs(actual - expected) < 0.0001, message);
}

ProcessSnapshot MakeProcess(std::uint32_t processId,
                            std::uint32_t parentProcessId,
                            const wchar_t* name,
                            std::uint64_t workingSetBytes = 0,
                            bool workingSetAvailable = false) {
    ProcessSnapshot process{};
    process.processId = processId;
    process.parentProcessId = parentProcessId;
    process.executableName = name;
    process.workingSetBytes = workingSetBytes;
    process.workingSetAvailable = workingSetAvailable;
    return process;
}

void TestCpuDeltas() {
    const CpuTimes first{true, 100, 300, 200};
    const CpuTimes second{true, 140, 420, 280};
    const auto systemPercent = codex_monitor::ComputeSystemCpuPercent(first, second);
    Expect(systemPercent.has_value(), "a monotonic second CPU snapshot should be available");
    ExpectNear(*systemPercent, 80.0, "kernel includes idle, so busy CPU should be 80 percent");

    const auto totalDelta = codex_monitor::SystemCpuTotalDelta(first, second);
    Expect(totalDelta && *totalDelta == 200, "system denominator should include kernel and user time");
    const auto processShare = codex_monitor::ComputeWholeMachineCpuShare(50, *totalDelta);
    Expect(processShare.has_value(), "process CPU share should have a denominator");
    ExpectNear(*processShare, 25.0, "process CPU must be normalized to whole-machine 0-100 percent");

    Expect(!codex_monitor::ComputeSystemCpuPercent(CpuTimes{}, second),
           "the first CPU frame must be unavailable");
    const CpuTimes rolledBack{true, 90, 250, 190};
    Expect(!codex_monitor::ComputeSystemCpuPercent(first, rolledBack),
           "counter rollback must degrade to unavailable");
    const auto clamped = codex_monitor::ComputeWholeMachineCpuShare(300, 200);
    Expect(clamped && *clamped == 100.0, "whole-machine share must never exceed 100 percent");
}

void TestTargetProcessTree() {
    const std::vector<ProcessSnapshot> processes = {
        MakeProcess(1, 0, L"explorer.exe"),
        MakeProcess(10, 1, L"ChatGPT.exe"),
        MakeProcess(11, 10, L"msedgewebview2.exe"),
        MakeProcess(12, 11, L"helper.exe"),
        MakeProcess(20, 1, L"CODEX.EXE"),
        MakeProcess(30, 1, L"CodexMonitorHUD.exe"),
        MakeProcess(40, 999, L"renderer.exe"),
    };
    const std::unordered_set<std::uint32_t> targets =
        codex_monitor::BuildTargetProcessSet(processes);
    Expect(targets.size() == 4, "two roots and both ChatGPT descendants should be selected");
    Expect(targets.count(10) == 1 && targets.count(11) == 1 && targets.count(12) == 1 &&
               targets.count(20) == 1,
           "target roots and transitive descendants should be present");
    Expect(targets.count(30) == 0, "the monitor itself must not be mistaken for Codex");
    Expect(targets.count(40) == 0, "an orphan process must not inherit target membership");
}

void TestMemoryRanking() {
    const std::vector<ProcessSnapshot> processes = {
        MakeProcess(1, 0, L"one.exe", 10, true),
        MakeProcess(2, 0, L"two.exe", 80, true),
        MakeProcess(3, 0, L"three.exe", 40, true),
        MakeProcess(4, 0, L"four.exe", 90, false),
        MakeProcess(5, 0, L"five.exe", 60, true),
        MakeProcess(6, 0, L"six.exe", 20, true),
        MakeProcess(7, 0, L"seven.exe", 100, true),
    };
    const auto top = codex_monitor::SelectTopMemoryProcesses(processes, 5);
    Expect(top.size() == 5, "ranking should return exactly five readable processes");
    Expect(top[0].processId == 7 && top[1].processId == 2 && top[2].processId == 5 &&
               top[3].processId == 3 && top[4].processId == 6,
           "ranking should sort working sets descending and skip unreadable values");
}

void TestSaturatingArithmetic() {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    Expect(codex_monitor::SaturatingAdd(maximum - 1, 5) == maximum,
           "byte aggregation should saturate on addition overflow");
    Expect(codex_monitor::SaturatingMultiply(maximum, 2) == maximum,
           "page conversion should saturate on multiplication overflow");
}

}  // namespace

int main() {
    TestCpuDeltas();
    TestTargetProcessTree();
    TestMemoryRanking();
    TestSaturatingArithmetic();
    std::cout << "portable_snapshot_math_tests=pass\n";
    return 0;
}
