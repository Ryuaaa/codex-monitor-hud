#!/usr/bin/env python3
"""Source-level contracts for the native Windows performance milestone."""

from pathlib import Path


WINDOWS_ROOT = Path(__file__).resolve().parents[1]
MAIN = (WINDOWS_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
SAMPLER = (WINDOWS_ROOT / "src" / "windows_sampler.cpp").read_text(encoding="utf-8")
SAMPLER_HEADER = (WINDOWS_ROOT / "src" / "windows_sampler.h").read_text(encoding="utf-8")
SNAPSHOT = (WINDOWS_ROOT / "src" / "performance_snapshot.h").read_text(encoding="utf-8")
MATH = (WINDOWS_ROOT / "src" / "snapshot_math.h").read_text(encoding="utf-8")
TEST = (WINDOWS_ROOT / "tests" / "snapshot_math_test.cpp").read_text(encoding="utf-8")
CMAKE = (WINDOWS_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")


def require(text: str, token: str, reason: str) -> None:
    if token not in text:
        raise AssertionError(f"missing {token!r}: {reason}")


def reject(text: str, token: str, reason: str) -> None:
    if token in text:
        raise AssertionError(f"unexpected {token!r}: {reason}")


def main() -> None:
    window_contracts = {
        "WS_CAPTION": "the standard caption provides native dragging",
        "WS_THICKFRAME": "the standard sizing frame provides resizing",
        "WS_MINIMIZEBOX": "the standard frame exposes native minimization",
        "ShowWindow(window, SW_MINIMIZE)": "the in-content minimize button works",
        "HWND_TOPMOST": "the window can become topmost",
        "HWND_NOTOPMOST": "topmost mode can be disabled",
        "WM_GETMINMAXINFO": "resizing has a usable minimum size",
        "WM_DPICHANGED": "layout follows per-monitor DPI changes",
        "kSampleIntervalMs = 5000": "visible sampling uses the requested five-second cadence",
        "WM_TIMER": "the HUD refreshes from the sampler",
        "SIZE_MINIMIZED": "minimization has an explicit low-burden path",
        "KillTimer(window, kSampleTimerId)": "minimization stops periodic sampling",
        "ResetCpuBaseline": "restoring cannot report a stale long-interval CPU delta",
        "CODEX / CHATGPT PROCESS TREE": "the first card displays target aggregation",
        "SYSTEM CPU & PHYSICAL MEMORY": "the second card displays system metrics",
        "COMMIT & PAGE FILE": "the third card displays commit and page-file metrics",
        "TOP 5 PROCESSES BY WORKING SET": "the fourth card displays the memory ranking",
        "Thermal pressure: system not provided": "unsupported thermal pressure is explicit",
        "waiting for next sample": "the first CPU frame is not fabricated",
        "#define NOMINMAX": "Windows min/max macros cannot break standard-library calls",
    }
    for token, reason in window_contracts.items():
        require(MAIN, token, reason)
    reject(MAIN, "Placeholder", "milestone 2 must not retain placeholder cards")

    sampler_contracts = {
        "GetSystemTimes": "whole-machine CPU comes from documented system counters",
        "GlobalMemoryStatusEx": "physical memory comes from the Windows memory API",
        "GetPerformanceInfo": "system commit totals and limit are collected",
        "EnumPageFilesW": "installed page-file totals and use are collected",
        "CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS": "the process list uses one system snapshot",
        "Process32FirstW": "process identity and parent IDs are enumerated",
        "OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION": "per-process access stays read-only and limited",
        "GetProcessTimes": "process CPU uses cumulative native counters",
        "GetProcessMemoryInfo": "working-set memory uses the native process API",
        "BuildTargetProcessSet": "Codex and ChatGPT descendants are resolved",
        "ComputeSystemCpuPercent": "system CPU is calculated from two snapshots",
        "ComputeWholeMachineCpuShare": "target CPU is normalized to the machine denominator",
        "SelectTopMemoryProcesses(raw.processes, 5)": "exactly five top-memory entries are requested",
    }
    for token, reason in sampler_contracts.items():
        require(SAMPLER, token, reason)

    snapshot_contracts = {
        "struct RawPerformanceSnapshot": "raw native readings have a clear snapshot boundary",
        "struct PerformanceSnapshot": "derived UI readings have a clear snapshot boundary",
        "std::optional<double> systemCpuPercent": "unavailable CPU is represented, not replaced by zero",
        "bool workingSetAvailable": "permission and exit races can degrade per process",
        "bool processListAvailable": "process enumeration failure is represented explicitly",
    }
    for token, reason in snapshot_contracts.items():
        require(SNAPSHOT, token, reason)
    require(SAMPLER_HEADER, "class WindowsSampler", "native collection has a sampler module")

    math_contracts = {
        "BuildTargetProcessSet": "process-tree selection is portable and testable",
        "IsTargetRootExecutable": "root executable matching is centralized",
        "codex.exe": "Codex is a recognized root",
        "chatgpt.exe": "ChatGPT is a recognized root",
        "SystemCpuTotalDelta": "CPU denominator math is centralized",
        "std::clamp(percent, 0.0, 100.0)": "CPU values stay in the whole-machine range",
        "SelectTopMemoryProcesses": "ranking is deterministic and portable",
    }
    for token, reason in math_contracts.items():
        require(MATH, token, reason)
    reject(MATH, "windows.h", "fixed algorithm tests must not depend on Windows headers")

    test_contracts = {
        "the first CPU frame must be unavailable": "first-frame behavior is fixed by a test",
        "normalized to whole-machine 0-100 percent": "CPU normalization is fixed by a test",
        "transitive descendants": "nested target process membership is fixed by a test",
        "monitor itself must not be mistaken": "root matching avoids a nearby false positive",
        "ranking should sort working sets descending": "top-five ordering is fixed by a test",
    }
    for token, reason in test_contracts.items():
        require(TEST, token, reason)

    cmake_contracts = {
        "add_executable(CodexMonitorHUD WIN32": "the executable uses the Windows GUI subsystem",
        "src/windows_sampler.cpp": "the native sampler is compiled into the HUD",
        "add_executable(CodexMonitorSnapshotMathTests": "portable fixed tests are buildable",
        "add_test(NAME windows_snapshot_math": "portable tests are registered with CTest",
        "cxx_std_17": "the implementation has an explicit language baseline",
        "MSVC_RUNTIME_LIBRARY": "the release binary does not require a separate VC runtime install",
        "PSAPI_VERSION=1": "PSAPI names resolve consistently through Psapi.lib",
        "NOMINMAX": "the existing Win32 shell's std::max calls compile under Windows headers",
        "user32 gdi32 psapi": "only documented Windows system libraries are linked",
    }
    for token, reason in cmake_contracts.items():
        require(CMAKE, token, reason)

    print("windows_static_contracts=pass")
    print(f"checked_root={WINDOWS_ROOT}")


if __name__ == "__main__":
    main()
