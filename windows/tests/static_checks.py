#!/usr/bin/env python3
"""Source-level contracts for the native Windows product-shell milestone."""

from pathlib import Path


WINDOWS_ROOT = Path(__file__).resolve().parents[1]
MAIN = (WINDOWS_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
MODULE_STATE = (WINDOWS_ROOT / "src" / "module_state.cpp").read_text(encoding="utf-8")
MODULE_STATE_HEADER = (WINDOWS_ROOT / "src" / "module_state.h").read_text(encoding="utf-8")
SETTINGS_STORE = (WINDOWS_ROOT / "src" / "settings_store_win32.cpp").read_text(encoding="utf-8")
SAMPLER = (WINDOWS_ROOT / "src" / "windows_sampler.cpp").read_text(encoding="utf-8")
SAMPLER_HEADER = (WINDOWS_ROOT / "src" / "windows_sampler.h").read_text(encoding="utf-8")
SNAPSHOT = (WINDOWS_ROOT / "src" / "performance_snapshot.h").read_text(encoding="utf-8")
MATH = (WINDOWS_ROOT / "src" / "snapshot_math.h").read_text(encoding="utf-8")
TEST = (WINDOWS_ROOT / "tests" / "snapshot_math_test.cpp").read_text(encoding="utf-8")
STATE_TEST = (WINDOWS_ROOT / "tests" / "module_state_test.cpp").read_text(encoding="utf-8")
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
        "kFastSampleIntervalMs = 5000": "system and target sampling uses five seconds",
        "kSlowSampleIntervalMs = 20000": "commit, paging, and ranking use twenty seconds",
        "GetTickCount64() >= state->nextSlowSampleTick": "slow sampling is scheduled by elapsed time",
        "WM_TIMER": "the HUD refreshes from the sampler",
        "SIZE_MINIMIZED": "minimization has an explicit low-burden path",
        "KillTimer(window, kSampleTimerId)": "minimization stops periodic sampling",
        "ResetCpuBaseline": "restoring cannot report a stale long-interval CPU delta",
        "HomeNeedsPerformance": "homepage sampling follows visible module dependencies",
        "CurrentPageNeedsPerformance": "page-level sampling demand is centralized",
        "case codex_monitor::Page::kCodex:\n            return false":
            "the disconnected Codex page stops performance sampling",
        "本机 Codex 数据尚未连接": "the Codex page states the real connection boundary",
        "does not simulate account, usage, task, token":
            "the disconnected Codex page cannot imply fabricated data",
        "kHomePageButtonId": "the shell exposes a homepage",
        "kCodexPageButtonId": "the shell exposes a Codex page",
        "kComputerPageButtonId": "the shell exposes a computer-performance page",
        "kSettingsButtonId": "the shell exposes settings",
        "ModuleRegistry()": "module controls are created from the registry",
        "VisibleHomeModules": "homepage cards follow saved visibility and order",
        "WM_EXITSIZEMOVE": "window placement is persisted after interactive movement",
        "WM_DISPLAYCHANGE": "saved placement is repaired after monitor changes",
        "RecreateSettingsFonts(window, *state)":
            "the settings window owns fonts at its current monitor DPI",
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
        "const bool captureCpu = process.isTargetTree": "non-target processes do not receive CPU queries",
        "process.isTargetTree || captureAllProcessMemory": "all-process memory is limited to slow samples",
        "if (mode == SampleMode::kFastAndSlow)": "slow APIs have an explicit cadence gate",
        "slowMetricsCache_.commitAvailable": "last valid commit values survive fast samples or failures",
        "slowMetricsCache_.pageFileAvailable": "last valid page-file values survive fast samples or failures",
        "slowMetricsCache_.rankingAvailable": "last valid ranking survives fast samples or failures",
    }
    for token, reason in sampler_contracts.items():
        require(SAMPLER, token, reason)

    snapshot_contracts = {
        "struct RawPerformanceSnapshot": "raw native readings have a clear snapshot boundary",
        "struct PerformanceSnapshot": "derived UI readings have a clear snapshot boundary",
        "std::optional<double> systemCpuPercent": "unavailable CPU is represented, not replaced by zero",
        "bool workingSetAvailable": "permission and exit races can degrade per process",
        "bool workingSetAttempted": "intentional fast-sample omissions are not permission failures",
        "bool processListAvailable": "process enumeration failure is represented explicitly",
        "bool topMemoryRankingAvailable": "a cached empty state is distinguishable from unavailable data",
    }
    for token, reason in snapshot_contracts.items():
        require(SNAPSHOT, token, reason)
    require(SAMPLER_HEADER, "class WindowsSampler", "native collection has a sampler module")
    require(SAMPLER_HEADER, "enum class SampleMode", "fast and slow sampling are explicit")

    if MAIN.count("SampleMode::kFastAndSlow") < 2:
        raise AssertionError("scheduled full refresh and demand restart must request slow metrics")
    if SAMPLER.count("GetProcessTimes") != 1:
        raise AssertionError("process CPU should have one target-gated native query site")

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

    state_contracts = {
        "struct ModuleDefinition": "module metadata has a single registry model",
        "requiresPerformanceSampling": "sampling demand is declared per module",
        "SanitizeHomeOrder": "saved order is repaired against the current registry",
        "VisibleHomeModules": "homepage visibility is derived in the portable model",
        "MoveHomeModule": "settings reorder behavior is portable and testable",
        "SerializeSettings": "settings use an explicit whitelist serializer",
        "ParseSettings": "settings parsing has a portable boundary",
        "ClampWindowPlacement": "off-screen recovery is portable and testable",
    }
    for token, reason in state_contracts.items():
        require(MODULE_STATE_HEADER + MODULE_STATE, token, reason)
    reject(MODULE_STATE, "windows.h", "the state model must remain portable")

    state_test_contracts = {
        "duplicate order entries must be removed": "registry migration behavior is fixed",
        "intentionally empty homepage": "all-hidden homepage intent is fixed",
        "unknown page must fall back to home": "damaged settings have a safe fallback",
        "off-screen saved window must return": "monitor removal recovery is fixed",
    }
    for token, reason in state_test_contracts.items():
        require(STATE_TEST, token, reason)

    persistence_contracts = {
        "FOLDERID_LocalAppData": "settings live in the per-user Windows data directory",
        "CodexMonitorHUD": "settings have an app-specific directory",
        "settings.ini": "settings use a deterministic filename",
        "kMaximumSettingsBytes = 64 * 1024":
            "a damaged settings file cannot consume unbounded memory at startup",
        "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH":
            "settings replacement is atomic and flushed",
    }
    for token, reason in persistence_contracts.items():
        require(SETTINGS_STORE, token, reason)
    reject(SETTINGS_STORE, "GetEnvironmentVariable", "settings paths must not be environment-spliced")

    require(MAIN, "if (wasMinimized)",
            "restoring after a display change revalidates the saved window position")

    cmake_contracts = {
        "add_executable(CodexMonitorHUD WIN32": "the executable uses the Windows GUI subsystem",
        "src/windows_sampler.cpp": "the native sampler is compiled into the HUD",
        "src/module_state.cpp": "the module state model is compiled into the HUD",
        "src/settings_store_win32.cpp": "the per-user settings store is compiled into the HUD",
        "add_executable(CodexMonitorSnapshotMathTests": "portable fixed tests are buildable",
        "add_test(NAME windows_snapshot_math": "portable tests are registered with CTest",
        "add_executable(CodexMonitorModuleStateTests": "portable state tests are buildable",
        "add_test(NAME windows_module_state": "portable state tests are registered with CTest",
        "cxx_std_17": "the implementation has an explicit language baseline",
        "MSVC_RUNTIME_LIBRARY": "the release binary does not require a separate VC runtime install",
        "PSAPI_VERSION=1": "PSAPI names resolve consistently through Psapi.lib",
        "NOMINMAX": "the existing Win32 shell's std::max calls compile under Windows headers",
        "user32 gdi32 psapi shell32 ole32": "only documented Windows system libraries are linked",
    }
    for token, reason in cmake_contracts.items():
        require(CMAKE, token, reason)

    print("windows_static_contracts=pass")
    print(f"checked_root={WINDOWS_ROOT}")


if __name__ == "__main__":
    main()
