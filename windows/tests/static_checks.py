#!/usr/bin/env python3
"""Source-level contracts for the native Windows product-shell milestone."""

import re
from pathlib import Path


WINDOWS_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = WINDOWS_ROOT.parent
MAIN = (WINDOWS_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
MODULE_STATE = (WINDOWS_ROOT / "src" / "module_state.cpp").read_text(encoding="utf-8")
MODULE_STATE_HEADER = (WINDOWS_ROOT / "src" / "module_state.h").read_text(encoding="utf-8")
WORKER = (WINDOWS_ROOT / "src" / "performance_worker.cpp").read_text(encoding="utf-8")
WORKER_HEADER = (WINDOWS_ROOT / "src" / "performance_worker.h").read_text(encoding="utf-8")
SCHEDULE = (WINDOWS_ROOT / "src" / "sampling_schedule.cpp").read_text(encoding="utf-8")
SCHEDULE_HEADER = (WINDOWS_ROOT / "src" / "sampling_schedule.h").read_text(encoding="utf-8")
SETTINGS_STORE = (WINDOWS_ROOT / "src" / "settings_store_win32.cpp").read_text(encoding="utf-8")
SAMPLER = (WINDOWS_ROOT / "src" / "windows_sampler.cpp").read_text(encoding="utf-8")
SAMPLER_HEADER = (WINDOWS_ROOT / "src" / "windows_sampler.h").read_text(encoding="utf-8")
SNAPSHOT = (WINDOWS_ROOT / "src" / "performance_snapshot.h").read_text(encoding="utf-8")
MATH = (WINDOWS_ROOT / "src" / "snapshot_math.h").read_text(encoding="utf-8")
TEST = (WINDOWS_ROOT / "tests" / "snapshot_math_test.cpp").read_text(encoding="utf-8")
UI_LAYOUT_MATH = (WINDOWS_ROOT / "src" / "ui_layout_math.cpp").read_text(
    encoding="utf-8"
)
UI_LAYOUT_MATH_HEADER = (WINDOWS_ROOT / "src" / "ui_layout_math.h").read_text(
    encoding="utf-8"
)
UI_LAYOUT_MATH_TEST = (
    WINDOWS_ROOT / "tests" / "ui_layout_math_test.cpp"
).read_text(encoding="utf-8")
SYSTEM_IO_RATE = (WINDOWS_ROOT / "src" / "system_io_rate.cpp").read_text(
    encoding="utf-8"
)
SYSTEM_IO_RATE_HEADER = (WINDOWS_ROOT / "src" / "system_io_rate.h").read_text(
    encoding="utf-8"
)
SYSTEM_IO_RATE_TEST = (WINDOWS_ROOT / "tests" / "system_io_rate_test.cpp").read_text(
    encoding="utf-8"
)
SYSTEM_IO_DISPLAY = (WINDOWS_ROOT / "src" / "system_io_display.cpp").read_text(
    encoding="utf-8"
)
SYSTEM_IO_DISPLAY_HEADER = (
    WINDOWS_ROOT / "src" / "system_io_display.h"
).read_text(encoding="utf-8")
SYSTEM_IO_DISPLAY_TEST = (
    WINDOWS_ROOT / "tests" / "system_io_display_test.cpp"
).read_text(encoding="utf-8")
SYSTEM_IO_SAMPLER = (
    WINDOWS_ROOT / "src" / "system_io_sampler_win32.cpp"
).read_text(encoding="utf-8")
SYSTEM_IO_SAMPLER_HEADER = (
    WINDOWS_ROOT / "src" / "system_io_sampler_win32.h"
).read_text(encoding="utf-8")
SYSTEM_IO_SAMPLER_TEST = (
    WINDOWS_ROOT / "tests" / "system_io_sampler_win32_test.cpp"
).read_text(encoding="utf-8")
STATE_TEST = (WINDOWS_ROOT / "tests" / "module_state_test.cpp").read_text(encoding="utf-8")
DIAGNOSIS = (WINDOWS_ROOT / "src" / "performance_diagnosis.cpp").read_text(encoding="utf-8")
DIAGNOSIS_TEST = (WINDOWS_ROOT / "tests" / "performance_diagnosis_test.cpp").read_text(
    encoding="utf-8"
)
SCHEDULE_TEST = (WINDOWS_ROOT / "tests" / "sampling_schedule_test.cpp").read_text(encoding="utf-8")
CMAKE = (WINDOWS_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
PACKAGE_SCRIPT = (WINDOWS_ROOT / "package-release.ps1").read_text(encoding="utf-8")
INSTALLER_SCRIPT = (WINDOWS_ROOT / "build-installer.ps1").read_text(encoding="utf-8")
INSTALLER_SOURCE = (
    WINDOWS_ROOT / "installer" / "CodexMonitorHUD.wxs"
).read_text(encoding="utf-8")
WINDOWS_WORKFLOW = (
    REPOSITORY_ROOT / ".github" / "workflows" / "windows-ci.yml"
).read_text(encoding="utf-8")
MSI_MAJOR_UPGRADE_TEST = (
    WINDOWS_ROOT / "tests" / "test-msi-major-upgrade.ps1"
).read_text(encoding="utf-8")
CODEX_TYPES = (WINDOWS_ROOT / "src" / "codex" / "codex_types.h").read_text(
    encoding="utf-8"
)
CODEX_EXECUTABLE_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_executable.h"
).read_text(encoding="utf-8")
CODEX_EXECUTABLE = (
    WINDOWS_ROOT / "src" / "codex" / "codex_executable.cpp"
).read_text(encoding="utf-8")
CODEX_PROCESS_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_process.h"
).read_text(encoding="utf-8")
CODEX_PROCESS = (WINDOWS_ROOT / "src" / "codex" / "codex_process.cpp").read_text(
    encoding="utf-8"
)
CODEX_APP_SERVER_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_app_server_client.h"
).read_text(encoding="utf-8")
CODEX_APP_SERVER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_app_server_client.cpp"
).read_text(encoding="utf-8")
CODEX_WORKER_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_worker.h"
).read_text(encoding="utf-8")
CODEX_WORKER = (WINDOWS_ROOT / "src" / "codex" / "codex_worker.cpp").read_text(
    encoding="utf-8"
)
CODEX_ACTIVITY_SCAN_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_activity_scan.h"
).read_text(encoding="utf-8")
CODEX_ACTIVITY_SCAN = (
    WINDOWS_ROOT / "src" / "codex" / "codex_activity_scan.cpp"
).read_text(encoding="utf-8")
CODEX_ACTIVITY_WORKER_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_activity_worker.h"
).read_text(encoding="utf-8")
CODEX_ACTIVITY_WORKER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_activity_worker.cpp"
).read_text(encoding="utf-8")
CODEX_ACTIVITY_SCAN_TEST = (
    WINDOWS_ROOT / "tests" / "codex_activity_scan_test.cpp"
).read_text(encoding="utf-8")
CODEX_ACTIVITY_WORKER_TEST = (
    WINDOWS_ROOT / "tests" / "codex_activity_worker_test.cpp"
).read_text(encoding="utf-8")
CODEX_USAGE_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_usage_math.h"
).read_text(encoding="utf-8")
CODEX_USAGE = (
    WINDOWS_ROOT / "src" / "codex" / "codex_usage_math.cpp"
).read_text(encoding="utf-8")
CODEX_PROCESS_TEST = (
    WINDOWS_ROOT / "tests" / "codex_process_test.cpp"
).read_text(encoding="utf-8")
CODEX_APP_SERVER_TEST = (
    WINDOWS_ROOT / "tests" / "codex_app_server_client_test.cpp"
).read_text(encoding="utf-8")
CODEX_USAGE_TEST = (
    WINDOWS_ROOT / "tests" / "codex_usage_math_test.cpp"
).read_text(encoding="utf-8")
CODEX_COST_MODEL = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_model.cpp"
).read_text(encoding="utf-8")
CODEX_COST_EVENT_PARSER = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_event_parser.cpp"
).read_text(encoding="utf-8")
CODEX_COST_FILE_SCAN = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_file_scan.cpp"
).read_text(encoding="utf-8")
CODEX_COST_HISTORY_STATE = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_history_state.cpp"
).read_text(encoding="utf-8")
CODEX_COST_HISTORY_STORE = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_history_store.cpp"
).read_text(encoding="utf-8")
CODEX_COST_SUMMARY = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_summary.cpp"
).read_text(encoding="utf-8")
CODEX_COST_HYBRID = (
    WINDOWS_ROOT / "src" / "codex" / "codex_cost_hybrid.cpp"
).read_text(encoding="utf-8")
CODEX_WORKER_TEST = (
    WINDOWS_ROOT / "tests" / "codex_worker_test.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT = (
    WINDOWS_ROOT / "src" / "codex" / "weekly_quota_alert.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "weekly_quota_alert.h"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_STATE_STORE = (
    WINDOWS_ROOT / "src" / "codex" / "weekly_quota_alert_state_store.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_TEST = (
    WINDOWS_ROOT / "tests" / "weekly_quota_alert_test.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_STATE_STORE_TEST = (
    WINDOWS_ROOT / "tests" / "weekly_quota_alert_state_store_test.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_DELIVERY = (
    WINDOWS_ROOT / "src" / "codex" / "weekly_quota_alert_delivery.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_DELIVERY_HEADER = (
    WINDOWS_ROOT / "src" / "codex" / "weekly_quota_alert_delivery.h"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_NOTIFICATION = (
    WINDOWS_ROOT / "src" / "codex" / "weekly_quota_notification_win32.cpp"
).read_text(encoding="utf-8")
WEEKLY_QUOTA_ALERT_DELIVERY_TEST = (
    WINDOWS_ROOT / "tests" / "weekly_quota_alert_delivery_test.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_MODEL = (
    WINDOWS_ROOT / "src" / "service_status_model.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_JSON = (
    WINDOWS_ROOT / "src" / "service_status_json_win32.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_FETCH = (
    WINDOWS_ROOT / "src" / "service_status_fetch_win32.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_WORKER = (
    WINDOWS_ROOT / "src" / "service_status_worker.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_WORKER_HEADER = (
    WINDOWS_ROOT / "src" / "service_status_worker.h"
).read_text(encoding="utf-8")
SERVICE_STATUS_MODEL_TEST = (
    WINDOWS_ROOT / "tests" / "service_status_model_test.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_JSON_TEST = (
    WINDOWS_ROOT / "tests" / "service_status_json_test.cpp"
).read_text(encoding="utf-8")
SERVICE_STATUS_WORKER_TEST = (
    WINDOWS_ROOT / "tests" / "service_status_worker_test.cpp"
).read_text(encoding="utf-8")
UPDATE_SELECTOR = (
    WINDOWS_ROOT / "src" / "update" / "github_release_selector.cpp"
).read_text(encoding="utf-8")
UPDATE_FETCH = (
    WINDOWS_ROOT / "src" / "update" / "update_fetch_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_ASSET_DOWNLOAD = (
    WINDOWS_ROOT / "src" / "update" / "update_asset_download_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_ASSET_DOWNLOAD_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_asset_download_win32.h"
).read_text(encoding="utf-8")
UPDATE_INSTALLER_VERIFIER = (
    WINDOWS_ROOT / "src" / "update" / "update_installer_verifier_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_INSTALLER_VERIFIER_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_installer_verifier_win32.h"
).read_text(encoding="utf-8")
UPDATE_MSI_IDENTITY = (
    WINDOWS_ROOT / "src" / "update" / "update_msi_identity_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_MSI_IDENTITY_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_msi_identity_win32.h"
).read_text(encoding="utf-8")
UPDATE_APPLY_TRANSACTION = (
    WINDOWS_ROOT / "src" / "update" / "update_apply_transaction_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_APPLY_TRANSACTION_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_apply_transaction_win32.h"
).read_text(encoding="utf-8")
UPDATE_APPLY_TRANSACTION_TEST = (
    WINDOWS_ROOT / "tests" / "update_apply_transaction_test.cpp"
).read_text(encoding="utf-8")
UPDATE_HELPER = (
    WINDOWS_ROOT / "src" / "update" / "update_helper_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_HELPER_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_helper_win32.h"
).read_text(encoding="utf-8")
UPDATE_HELPER_LAUNCHER = (
    WINDOWS_ROOT / "src" / "update" / "update_helper_launcher_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_HELPER_LAUNCHER_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_helper_launcher_win32.h"
).read_text(encoding="utf-8")
UPDATE_INSTALL = (
    WINDOWS_ROOT / "src" / "update" / "update_install_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_INSTALL_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_install_win32.h"
).read_text(encoding="utf-8")
UPDATE_DIRECTORY_CLEANUP = (
    WINDOWS_ROOT / "src" / "update" / "update_directory_cleanup_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_DIRECTORY_CLEANUP_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_directory_cleanup_win32.h"
).read_text(encoding="utf-8")
UPDATE_DIRECTORY_CLEANUP_TEST = (
    WINDOWS_ROOT / "tests" / "update_directory_cleanup_test.cpp"
).read_text(encoding="utf-8")
UPDATE_INSTALL_WORKER = (
    WINDOWS_ROOT / "src" / "update" / "update_install_worker.cpp"
).read_text(encoding="utf-8")
UPDATE_INSTALL_WORKER_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_install_worker.h"
).read_text(encoding="utf-8")
UPDATE_INSTALLED_HUD = (
    WINDOWS_ROOT / "src" / "update" / "update_installed_hud_win32.cpp"
).read_text(encoding="utf-8")
UPDATE_INSTALLED_HUD_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_installed_hud_win32.h"
).read_text(encoding="utf-8")
UPDATE_HELPER_TEST = (
    WINDOWS_ROOT / "tests" / "update_helper_test.cpp"
).read_text(encoding="utf-8")
UPDATE_STATE = (
    WINDOWS_ROOT / "src" / "update" / "update_state_store.cpp"
).read_text(encoding="utf-8")
UPDATE_WORKER = (
    WINDOWS_ROOT / "src" / "update" / "update_worker.cpp"
).read_text(encoding="utf-8")
UPDATE_WORKER_HEADER = (
    WINDOWS_ROOT / "src" / "update" / "update_worker.h"
).read_text(encoding="utf-8")
RESOURCE_SCRIPT = (WINDOWS_ROOT / "resources" / "CodexMonitorHUD.rc").read_text(
    encoding="utf-8"
)


def require(text: str, token: str, reason: str) -> None:
    if token not in text:
        raise AssertionError(f"missing {token!r}: {reason}")


def reject(text: str, token: str, reason: str) -> None:
    if token in text:
        raise AssertionError(f"unexpected {token!r}: {reason}")


def require_regex(text: str, pattern: str, reason: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE | re.DOTALL) is None:
        raise AssertionError(f"missing pattern {pattern!r}: {reason}")


def reject_regex(text: str, pattern: str, reason: str) -> None:
    if re.search(pattern, text, flags=re.MULTILINE | re.DOTALL) is not None:
        raise AssertionError(f"unexpected pattern {pattern!r}: {reason}")


def without_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def struct_body(text: str, name: str) -> str:
    match = re.search(
        rf"\bstruct\s+{re.escape(name)}\s*\{{(?P<body>.*?)\n\}};",
        without_cpp_comments(text),
        flags=re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing struct {name!r}")
    return match.group("body")


def cmake_call_body(text: str, command: str, target: str) -> str:
    match = re.search(
        rf"\b{re.escape(command)}\s*\(\s*{re.escape(target)}\b(?P<body>.*?)\)",
        text,
        flags=re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing CMake call {command}({target} ...)")
    return match.group("body")


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
        "GetDpiForWindow(window)": "startup placement uses the actual target monitor DPI",
        "showCommand == SW_SHOWMAXIMIZED": "startup cannot bypass uniform HUD sizing through maximize",
        "command == SC_MAXIMIZE": "the fixed-aspect HUD cannot be maximized after startup",
        "kFastSampleIntervalMs = 5000": "system and target sampling uses five seconds",
        "kSlowSampleIntervalMs = 20000": "commit, paging, and ranking use twenty seconds",
        "GetTickCount64() >= state->nextSlowSampleTick": "slow sampling is scheduled by elapsed time",
        "WM_TIMER": "the HUD refreshes from the sampler",
        "SIZE_MINIMIZED": "minimization has an explicit low-burden path",
        "KillTimer(window, kSampleTimerId)": "minimization stops periodic sampling",
        "PauseAndInvalidate": "hidden performance pages invalidate outstanding samples",
        "HomeNeedsPerformance": "homepage sampling follows visible module dependencies",
        "CurrentPageNeedsPerformance": "page-level sampling demand is centralized",
        "NativePageNeedsPerformanceData":
            "native-page sampling follows module data dependencies",
        "kHomePageButtonId": "the shell exposes a homepage",
        "kCodexPageButtonId": "the shell exposes a Codex page",
        "kComputerPageButtonId": "the shell exposes a computer-performance page",
        "kSettingsButtonId": "the shell exposes settings",
        "ModuleRegistry()": "module controls are created from the registry",
        "VisibleHomeModules": "homepage cards follow saved visibility and order",
        "SYSTEM + CODEX/CHATGPT":
            "the first performance card names the full process-tree scope",
        "WM_EXITSIZEMOVE": "window placement is persisted after interactive movement",
        "WM_DISPLAYCHANGE": "saved placement is repaired after monitor changes",
        "RecreateSettingsFonts(window, *state)":
            "the settings window owns fonts at its current monitor DPI",
        "CODEX / CHATGPT PROCESS TREE": "the first card displays target aggregation",
        "SYSTEM CPU & PHYSICAL MEMORY": "the second card displays system metrics",
        "COMMIT & PAGE FILE": "the third card displays commit and page-file metrics",
        "TOP 5 PROCESSES BY WORKING SET": "the fourth card displays the memory ranking",
        "BuildSystemIoThroughputCardText":
            "the live network and disk rates are connected to a performance card",
        "Thermal pressure: system not provided": "unsupported thermal pressure is explicit",
        "waiting for next sample": "the first CPU frame is not fabricated",
        "#define NOMINMAX": "Windows min/max macros cannot break standard-library calls",
    }
    for token, reason in window_contracts.items():
        require(MAIN, token, reason)
    reject(MAIN, "Placeholder", "milestone 2 must not retain placeholder cards")
    reject(MAIN, ".Sample(", "the window thread must not execute native sampling")

    worker_contracts = {
        "std::thread": "native sampling runs away from the window message thread",
        "std::condition_variable": "the worker sleeps instead of polling for requests",
        "WindowsSampler sampler_": "the single serial worker owns the native sampler",
        "std::optional<CompletedSample> latest_":
            "completed data is retained in worker-owned storage",
        "PostMessageW(notifyWindow, notifyMessage, 0, 0)":
            "completion notification carries no owning raw pointer",
        "schedule_.PauseAndInvalidate()":
            "pausing cancels pending work and invalidates an in-flight result",
        "sampler_.ResetCpuBaseline()":
            "baseline reset runs on the sampler-owning thread",
        "schedule_.Finish(*item)": "generation validation gates publication",
        "thread_.join()": "shutdown waits for the sole worker thread",
    }
    for token, reason in worker_contracts.items():
        require(WORKER_HEADER + WORKER, token, reason)

    schedule_contracts = {
        "pendingMode_": "busy requests occupy one coalescing slot",
        "Covers": "a running full sample covers concurrent fast requests",
        "generation_": "pause and stop can invalidate old work",
        "baselineResetPending_": "resume sampling is ordered behind a baseline reset",
        "ActivateAndRequestFullSample": "resume always requests full metrics",
        "PauseAndInvalidate": "pause semantics are centralized and portable",
    }
    for token, reason in schedule_contracts.items():
        require(SCHEDULE_HEADER + SCHEDULE, token, reason)
    reject(SCHEDULE_HEADER + SCHEDULE, "windows.h",
           "request scheduling must remain portable")

    schedule_test_contracts = {
        "slow sampling must win request coalescing": "slow priority is fixed by a test",
        "an in-flight pre-pause result must be rejected":
            "generation invalidation is fixed by a test",
        "resume must reset the baseline before sampling":
            "resume ordering is fixed by a test",
        "stop must reject future work": "shutdown request handling is fixed by a test",
    }
    for token, reason in schedule_test_contracts.items():
        require(SCHEDULE_TEST, token, reason)

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
        "systemIoSampler_.Capture()": "network and disk counters share the serial native sampler",
        "ComputeSystemIoRates(previousSystemIo_": "whole-machine byte rates use cumulative deltas",
        "previousSystemIo_ = {}": "pause and resume require fresh I/O rate baselines",
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
        "SystemIoCounters systemIo": "raw network and disk counters cross the snapshot boundary",
        "SystemIoRates systemIoRates": "derived byte-per-second results cross the UI boundary",
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

    ui_layout_math_contracts = {
        "kMinimumUniformScale = 0.75":
            "interactive HUD resizing has a fixed 75 percent minimum",
        "ComputeUniformScaleLimits":
            "the maximum scale follows the current monitor work area",
        "minimumFitsWorkArea":
            "a work area smaller than the minimum is represented explicitly",
        "ComputeUniformScaleForProposedFrame":
            "side and corner proposals select one uniform scale",
        "FrameForUniformScale":
            "uniform dimensions are rebuilt around a fixed drag anchor",
        "CenteredOrigin":
            "side drags retain the fixed opposite edge and orthogonal center",
        "PlaceFrameInWorkAreaCorner":
            "all four work-area corner placements share one safe primitive",
        "ClampFrameToWorkArea":
            "restored and resized frames can be moved back onscreen",
        "ScaleLogicalSizeForDpi":
            "DPI conversion is checked independently of Win32 window state",
        "BuildVariableHeightGridRows":
            "variable-height cards share portable checked row geometry",
        "ClampPixelScrollOffset":
            "pixel scroll bounds are portable and deterministic",
        "kInt32Maximum":
            "coordinate arithmetic has explicit signed 32-bit bounds",
    }
    for token, reason in ui_layout_math_contracts.items():
        require(UI_LAYOUT_MATH + UI_LAYOUT_MATH_HEADER, token, reason)
    reject(UI_LAYOUT_MATH_HEADER, "windows.h",
           "uniform resize math must remain portable and deterministic")

    ui_layout_math_test_contracts = {
        "the minimum drag scale must remain 75 percent":
            "the public minimum is fixed by a sample",
        "the maximum must follow the limiting work-area axis":
            "dynamic work-area maximum behavior is fixed",
        "all four work-area corners must be placeable":
            "corner placement behavior is fixed",
        "must keep the opposite corner fixed":
            "corner drag anchoring is fixed",
        "must fix the right edge and vertical center":
            "side drag anchoring is fixed",
        "DPI multiplication must reject signed-coordinate overflow":
            "DPI overflow behavior is fixed",
        "a RECT whose mathematical width exceeds signed range must be rejected":
            "coordinate overflow behavior is fixed",
        "each grid row must use its tallest card":
            "variable-height two-column layout is fixed by a sample",
        "pixel scrolling must stop at the content bottom":
            "long-card scrolling has a fixed pixel boundary sample",
    }
    for token, reason in ui_layout_math_test_contracts.items():
        require(UI_LAYOUT_MATH_TEST, token, reason)

    system_io_rate_contracts = {
        "networkReceiveBytesPerSecond": "network receive rate has an explicit optional byte-per-second field",
        "networkSendBytesPerSecond": "network send rate has an explicit optional byte-per-second field",
        "diskReadBytesPerSecond": "disk read rate has an explicit optional byte-per-second field",
        "diskWriteBytesPerSecond": "disk write rate has an explicit optional byte-per-second field",
        "currentTime100ns <= previousTime100ns": "zero and backwards elapsed time are rejected",
        "elapsed100ns > kMaximumSystemIoRateInterval100ns": "stale intervals are not presented as current rates",
        "currentFirst < previousFirst": "counter reset or wraparound is rejected",
        "previousSourceIdentity != currentSourceIdentity": "adapter or counter-source changes require a new baseline",
        "result.networkNeedsBaseline = true": "network reset and time anomalies request a new baseline",
        "result.diskNeedsBaseline = true": "disk reset and time anomalies request a new baseline",
    }
    for token, reason in system_io_rate_contracts.items():
        require(SYSTEM_IO_RATE + SYSTEM_IO_RATE_HEADER, token, reason)
    reject(SYSTEM_IO_RATE_HEADER, "windows.h",
           "fixed I/O rate algorithms must not depend on Windows headers")

    system_io_native_contracts = {
        "#include <sdkddkver.h>":
            "the Windows target version is visible before IP Helper gates netioapi declarations",
        "GetIfTable2": "network bytes come from documented cumulative interface counters",
        "FreeMibTable": "the native interface table is always released",
        "IF_TYPE_SOFTWARE_LOOPBACK": "loopback traffic is not counted as external machine traffic",
        "row.InterfaceAndOperStatusFlags.FilterInterface":
            "filter interfaces are excluded from aggregate traffic",
        "row.InterfaceAndOperStatusFlags.HardwareInterface":
            "VPN and virtual interfaces are not double-counted with physical adapters",
        "PdhAddEnglishCounterW": "localized systems use stable English physical-disk counter paths",
        "PdhCollectQueryData": "physical-disk cumulative counters are sampled in one persistent query",
        "PdhGetRawCounterValue": "disk rates are derived from raw cumulative byte counters",
        "PERF_COUNTER_BULK_COUNT": "disk raw values are accepted only with cumulative-byte semantics",
        "raw FirstValue is the": "the misleading Bytes/sec display name is documented as raw cumulative data",
        "PdhCloseQuery": "the persistent native disk query is released",
        "kDiskQueryRetryInterval100ns":
            "a transient PDH startup failure is retried with a bounded low-frequency interval",
        "ResetDiskQueryAfterFailure":
            "a runtime PDH failure cannot leave the 24-hour monitor permanently unavailable",
        "QueryUnbiasedInterruptTime":
            "low-overhead rate timing excludes suspended time without unnecessary precision",
    }
    for token, reason in system_io_native_contracts.items():
        require(SYSTEM_IO_SAMPLER, token, reason)
    require(CMAKE, "iphlpapi mincore pdh",
            "native I/O targets link IP Helper, the precise timer import, and PDH")
    require(SYSTEM_IO_SAMPLER,
            "#include <sdkddkver.h>\n#include <winsock2.h>\n#include <ws2tcpip.h>\n#include <windows.h>\n#include <iphlpapi.h>",
            "the SDK target gate and Winsock IP definitions must precede the IP Helper API")
    require(SYSTEM_IO_SAMPLER, "#include <iphlpapi.h>",
            "the supported IP Helper umbrella header exposes GetIfTable2 and MIB_IF_TABLE2")
    reject(SYSTEM_IO_SAMPLER_HEADER, "winsock2.h",
           "the sampler header can be included after windows.h and must not change Winsock include order")

    system_io_test_contracts = {
        "the first sample must be unavailable rather than reported as zero":
            "first-sample behavior is fixed",
        "counter rollback or reset must invalidate": "counter reset behavior is fixed",
        "a backwards monotonic timestamp must be unavailable": "time anomaly behavior is fixed",
        "GetIfTable2 must provide whole-machine network counters":
            "the Windows-native sampling path executes in CI",
    }
    for token, reason in system_io_test_contracts.items():
        require(SYSTEM_IO_RATE_TEST + SYSTEM_IO_SAMPLER_TEST, token, reason)

    system_io_display_contracts = {
        "FormatSystemIoByteRate":
            "byte-rate formatting has a portable presentation boundary",
        "BuildSystemIoThroughputCardText":
            "all four rates are composed in one tested card builder",
        "正在建立基线":
            "first and reset samples are not fabricated as zero throughput",
        "当前未取得网络或磁盘计数":
            "a fully unavailable sample is explained honestly",
        "复用现有 5 秒性能采样":
            "the card documents its existing low-burden cadence",
        "networkReceiveBytesPerSecond": "the card displays download throughput",
        "networkSendBytesPerSecond": "the card displays upload throughput",
        "diskReadBytesPerSecond": "the card displays disk-read throughput",
        "diskWriteBytesPerSecond": "the card displays disk-write throughput",
    }
    for token, reason in system_io_display_contracts.items():
        require(SYSTEM_IO_DISPLAY + SYSTEM_IO_DISPLAY_HEADER, token, reason)
    reject(SYSTEM_IO_DISPLAY_HEADER + SYSTEM_IO_DISPLAY, "windows.h",
           "fixed I/O display formatting must stay independent of Win32")
    reject(SYSTEM_IO_DISPLAY_HEADER + SYSTEM_IO_DISPLAY, "std::thread",
           "display formatting must not create another worker")

    system_io_display_test_contracts = {
        "a first or reset sample must say that it is establishing a baseline":
            "baseline wording is fixed by a sample",
        "a valid idle rate must be shown as zero rather than unavailable":
            "zero and unavailable remain distinct",
        "the card must display network receive as download":
            "download mapping is fixed",
        "the card must preserve a valid idle disk-write rate":
            "disk-write mapping is fixed",
        "an entirely unavailable sample must not look idle or healthy":
            "the all-unavailable state is fixed",
    }
    for token, reason in system_io_display_test_contracts.items():
        require(SYSTEM_IO_DISPLAY_TEST, token, reason)

    state_contracts = {
        "struct ModuleDefinition": "module metadata has a single registry model",
        "Page nativePage": "each module declares its own native page",
        "requiresPerformanceSampling": "sampling demand is declared per module",
        "requiresCodexData": "Codex demand is independent from performance demand",
        "requiresCodexActivity": "local task activity has an independent demand gate",
        "requiresServiceStatus": "official service-status demand is an independent dependency",
        "nativePageVisible": "own-page visibility is independent from Home visibility",
        "kCodexFiveHourQuota": "the five-hour quota has its own module identity",
        "kCodexWeeklyQuota": "the weekly quota has its own module identity",
        "kCodexQuotaForecast": "quota trend forecasting has its own module identity",
        "kCodexTokenCostEstimate": "Token and API-equivalent cost has its own Beta module identity",
        "kCodexTaskActivity": "current local task activity has its own module identity",
        "kSystemDiagnosis": "the system diagnosis has its own module identity",
        "kSystemIoThroughput":
            "live network and disk throughput has its own module identity",
        "kOpenAIServiceStatus": "official service status has its own module identity",
        "SanitizeHomeOrder": "saved order is repaired against the current registry",
        "VisibleHomeModules": "homepage visibility is derived in the portable model",
        "VisibleModulesForNativePage": "native pages are filtered by registry metadata",
        "HomeNeedsCodexData": "homepage Codex demand follows visible modules",
        "HomeNeedsCodexActivity": "homepage activity scanning follows visible modules",
        "HomeNeedsServiceStatus": "homepage service-status demand follows visible modules",
        "MoveHomeModule": "settings reorder behavior is portable and testable",
        "SerializeSettings": "settings use an explicit whitelist serializer",
        "ParseSettings": "settings parsing has a portable boundary",
        "version == 10":
            "only the complete current schema may restore background quota alerts",
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
        "TestVersionTenRoundTripAndIndependentQuotaSwitches":
            "the independent quota, appearance, and alert settings have a version 10 round-trip test",
        "TestVersionSevenIoMigrationPreservesExplicitChoices":
            "the new I/O module migrates version 7 settings without coupling quotas",
        "TestLegacyShapeMigrationKeepsOriginAndResetsUniformScale":
            "old independently shaped windows migrate to a uniform frame",
        "TestWeeklyQuotaAlertDefaultsAndMigrationAreOptIn":
            "weekly alert settings remain opt-in across migration",
        "version 9 appearance settings must migrate without loss":
            "the previous appearance schema is retained without enabling alerts",
        "TestVersionThreeMigrationPreservesExplicitVisibility":
            "version 3 settings retain explicit visibility when the service module is added",
        "TestVersionOneMigrationPreservesOldHomeChoices":
            "version 1 settings migrate without enabling new Home work",
        '"version=10\\n"': "the current persisted settings schema is version 10",
        '"version=9\\n"': "the previous appearance schema has an explicit migration fixture",
        '"version=1\\n"': "the previous settings schema has an explicit migration fixture",
    }
    for token, reason in state_test_contracts.items():
        require(STATE_TEST, token, reason)

    cost_contracts = {
        "kCodexCostPricingVersion": "the frozen price table has an explicit version",
        "ParseCodexCostJsonlLine": "local rollout parsing has a narrow boundary",
        "kCodexCostMaximumScanBytes": "local history scanning has a hard byte budget",
        "kCodexCostMaximumLineBytes": "one damaged line cannot grow memory without bound",
        "resetAfterTruncation": "rewritten rollout files invalidate retained parser state",
        "kCodexCostHistoryCacheMaximumBytes":
            "restart cache size is bounded independently from source scanning",
        'kVersionLine = "version=1"':
            "restart cache has an explicit format version",
        "CodexCostHistoryAtomicReplace":
            "restart cache commits through an atomic replacement boundary",
        "CalculateCodexCostSummary": "cost periods and coverage are aggregated independently",
        "CalculateCodexCostHybridSummary": "official Token totals remain the primary display source",
    }
    cost_sources = (CODEX_COST_MODEL + CODEX_COST_EVENT_PARSER +
                    CODEX_COST_FILE_SCAN + CODEX_COST_HISTORY_STATE +
                    CODEX_COST_HISTORY_STORE + CODEX_COST_SUMMARY +
                    CODEX_COST_HYBRID)
    for token, reason in cost_contracts.items():
        require(cost_sources, token, reason)
    reject(struct_body(CODEX_WORKER_HEADER, "CodexCostRefresh"),
           "path", "cost refresh results must not expose local source paths")
    reject(struct_body(CODEX_WORKER_HEADER, "CodexCostRefresh"),
           "raw", "cost refresh results must not expose raw session JSON")

    update_contracts = {
        "CodexMonitorHUD-windows-x64-":
            "Windows updates require the exact architecture-specific MSI name",
        "draft || release.prerelease":
            "draft and preview releases cannot become automatic updates",
        "IsAllowedDownloadUrl":
            "selected installers must use the exact repository release URL",
        "WINHTTP_DISABLE_COOKIES":
            "update checks send no browser cookies",
        "WINHTTP_DISABLE_AUTHENTICATION":
            "update checks send no automatic credentials",
        "WINHTTP_DISABLE_REDIRECTS":
            "the release-list request cannot redirect to another host",
        "kMaximumResponseBytes = 2 * 1024 * 1024":
            "the public release response is bounded",
        "automaticCheckInterval{24 * 60 * 60}":
            "automatic Windows update checks run at most daily",
        "RequestManualCheck":
            "the user can explicitly bypass the daily cadence",
        "checked_version=":
            "an app upgrade invalidates stale cached update offers",
        '"version=2\\nlast_check="':
            "the checked-version schema has an explicit format revision",
        "RequestStop":
            "visible window shutdown never waits for synchronous network work",
        "update-state.ini":
            "the UI supplies an independent update-state file",
    }
    update_sources = (UPDATE_SELECTOR + UPDATE_FETCH +
                      UPDATE_ASSET_DOWNLOAD_HEADER + UPDATE_ASSET_DOWNLOAD +
                      UPDATE_STATE +
                      UPDATE_INSTALLER_VERIFIER_HEADER +
                      UPDATE_INSTALLER_VERIFIER + UPDATE_MSI_IDENTITY_HEADER +
                      UPDATE_MSI_IDENTITY + UPDATE_APPLY_TRANSACTION_HEADER +
                      UPDATE_APPLY_TRANSACTION + UPDATE_HELPER_HEADER +
                      UPDATE_HELPER + UPDATE_HELPER_LAUNCHER_HEADER +
                      UPDATE_HELPER_LAUNCHER + UPDATE_INSTALL_HEADER +
                      UPDATE_INSTALL + UPDATE_DIRECTORY_CLEANUP_HEADER +
                      UPDATE_DIRECTORY_CLEANUP + UPDATE_INSTALL_WORKER_HEADER +
                      UPDATE_INSTALL_WORKER + UPDATE_INSTALLED_HUD_HEADER +
                      UPDATE_INSTALLED_HUD + UPDATE_WORKER_HEADER +
                      UPDATE_WORKER + MAIN)
    for token, reason in update_contracts.items():
        require(update_sources, token, reason)
    reject(UPDATE_FETCH, "Authorization", "public update checks must not send a token")
    reject(UPDATE_FETCH, "Cookie:", "public update checks must not send cookies")
    for token, reason in {
        "release-assets.githubusercontent.com":
            "asset redirects are restricted to GitHub's release CDN",
        "WINHTTP_DISABLE_REDIRECTS":
            "asset redirects are followed only after explicit validation",
        "WINHTTP_DISABLE_COOKIES":
            "asset downloads never send browser cookies",
        "WINHTTP_DISABLE_AUTHENTICATION":
            "asset downloads never send automatic credentials",
        "CREATE_NEW":
            "downloaded assets cannot overwrite an existing path",
        "FILE_FLAG_OPEN_REPARSE_POINT":
            "download destinations reject filesystem redirection",
        "LockDirectoryChain":
            "the complete local directory chain stays fixed during download",
        "DRIVE_FIXED":
            "update assets cannot be written through UNC or network paths",
        "GENERIC_WRITE | DELETE":
            "failed partial downloads can be deleted by their open handle",
        "maximumBytes":
            "each downloaded asset has a caller-supplied hard size limit",
    }.items():
        require(UPDATE_ASSET_DOWNLOAD, token, reason)
    reject(UPDATE_ASSET_DOWNLOAD, "Authorization:",
           "asset downloads must not send a token")
    reject(UPDATE_ASSET_DOWNLOAD, "Cookie:",
           "asset downloads must not send cookies")
    installer_verifier_sources = (
        UPDATE_INSTALLER_VERIFIER_HEADER + UPDATE_INSTALLER_VERIFIER
    )
    for token, reason in {
        "kMaximumWindowsInstallerBytes":
            "downloaded installers have a hard file-size limit",
        "FILE_FLAG_OPEN_REPARSE_POINT":
            "installer verification refuses redirected filesystem targets",
        "BCryptHashData":
            "MSI SHA-256 is calculated through Windows CNG",
        "ConstantTimeSha256Equals":
            "the complete digest is compared without early exit",
        "It must never authorize":
            "checksum equality cannot be mistaken for publisher trust",
    }.items():
        require(installer_verifier_sources, token, reason)
    msi_identity_sources = UPDATE_MSI_IDENTITY_HEADER + UPDATE_MSI_IDENTITY
    for token, reason in {
        "WinVerifyTrust":
            "a checksum cannot replace Windows publisher trust validation",
        "WTD_REVOKE_WHOLECHAIN":
            "the Authenticode chain is revocation checked",
        "CERT_SHA256_HASH_PROP_ID":
            "the actual signing certificate is pinned by SHA-256",
        "kMissingTrustedPublisherFingerprint":
            "one-click installation fails closed until a publisher pin exists",
        "MsiOpenDatabaseW":
            "the MSI identity is inspected through the Windows Installer API",
        "kWindowsMsiUpgradeCode":
            "the package must belong to the expected upgrade family",
        "kWindowsMsiTemplate":
            "the package architecture and language are fixed",
    }.items():
        require(msi_identity_sources, token, reason)
    update_apply_sources = (
        UPDATE_APPLY_TRANSACTION_HEADER + UPDATE_APPLY_TRANSACTION
    )
    for token, reason in {
        "LockCanonicalInstallerPath":
            "the apply transaction locks the complete canonical MSI path",
        "FILE_READ_ATTRIBUTES | DELETE":
            "writable ancestors are opened with active rename protection",
        "FILE_SHARE_READ, nullptr":
            "the locked MSI denies write and delete replacement",
        "VerifyDownloadedWindowsInstallerChecksum":
            "the locked transaction includes the SHA-256 gate",
        "VerifyLockedWindowsMsiIdentityAndPublisher":
            "the locked transaction includes Authenticode and MSI identity without reacquiring its locks",
        "installCallback(locked.canonicalPath)":
            "the install callback runs before the path locks leave scope",
        "MsiInstallProductW":
            "the production transaction performs a synchronous Windows Installer call",
        "MsiSetInternalUI":
            "the production transaction suppresses installer UI without spawning an async process",
        "installAttempted = true":
            "the production install boundary is explicitly observable in transaction tests",
    }.items():
        require(update_apply_sources, token, reason)
    require(msi_identity_sources, "WTD_REVOKE_WHOLECHAIN",
            "the production trust path cannot disable revocation checks")
    require(msi_identity_sources, "CERT_E_UNTRUSTEDROOT",
            "the isolated signed fixture has an explicit test-only trust result")
    require(msi_identity_sources,
            "CODEX_MONITOR_UPDATE_APPLY_TRANSACTION_TESTING",
            "the untrusted CI signer exception is unavailable to production")
    reject(update_apply_sources, "revocationChecks",
           "the transaction API must not expose a revocation bypass")
    require(UPDATE_APPLY_TRANSACTION_TEST, "callbackCount == 0",
            "verification failures assert that installer launch is impossible")
    require(UPDATE_APPLY_TRANSACTION_TEST,
            "ProbePathAndAncestorLocks",
            "the signed fixture proves locks survive through the callback")
    require(UPDATE_APPLY_TRANSACTION_TEST, "--install-signed-msi",
            "the signed fixture can execute the production synchronous installer path")
    test_apply_macro = "CODEX_MONITOR_UPDATE_APPLY_TRANSACTION_TESTING"
    if CMAKE.count(test_apply_macro) != 1:
        raise AssertionError(
            "the injectable update callback macro must belong to exactly one test target"
        )
    require(UPDATE_APPLY_TRANSACTION_HEADER, test_apply_macro,
            "the injectable callback API is hidden from production builds")
    require(UPDATE_APPLY_TRANSACTION, test_apply_macro,
            "the injectable callback implementation is hidden from production builds")
    require(WINDOWS_WORKFLOW,
            "--verify-apply-signed-msi",
            "Windows CI runs the transaction against a real signed MSI")
    require(WINDOWS_WORKFLOW,
            "publisher-rejected",
            "Windows CI proves publisher failure invokes no callback")
    require(WINDOWS_WORKFLOW,
            "checksum-rejected",
            "Windows CI proves checksum failure invokes no callback")
    require(WINDOWS_WORKFLOW,
            "signature-rejected",
            "Windows CI proves a tampered signed MSI invokes no callback")
    require(UPDATE_APPLY_TRANSACTION_TEST,
            "--tamper-msi-product-name",
            "the signed fixture mutates a signed MSI database stream")
    require(WINDOWS_WORKFLOW,
            "--tamper-msi-product-name",
            "Windows CI invalidates signed MSI content deterministically")
    for forbidden_store_write in (
        "Import-Certificate",
        "Cert:\\CurrentUser\\Root",
        "Cert:\\CurrentUser\\TrustedPublisher",
        "$store.Add($publicCertificate)",
    ):
        reject(WINDOWS_WORKFLOW, forbidden_store_write,
               "Windows CI must not mutate hosted-runner trust stores")
    helper_sources = (
        UPDATE_HELPER_HEADER + UPDATE_HELPER +
        UPDATE_HELPER_LAUNCHER_HEADER + UPDATE_HELPER_LAUNCHER +
        UPDATE_INSTALLED_HUD_HEADER + UPDATE_INSTALLED_HUD
    )
    for token, reason in {
        "--codex-monitor-update-helper-v1":
            "the internal helper mode has a versioned command-line contract",
        "GetProcessTimes":
            "the inherited old-process handle is checked against creation time",
        "QueryFullProcessImageNameW":
            "the inherited process must be the installed HUD executable",
        "WaitForSingleObject":
            "the helper blocks on the exact inherited process handle",
        "ApplyVerifiedWindowsMsiUpdate":
            "the helper reuses the continuously locked production installer transaction",
        "VerifyAndLaunchInstalledWindowsHud":
            "restart is gated by post-install executable verification",
        "WTD_REVOKE_WHOLECHAIN":
            "the restarted executable receives whole-chain revocation checking",
        "CERT_SHA256_HASH_PROP_ID":
            "the restarted executable is pinned to the compiled publisher",
        "GetFileVersionInfoW":
            "the restarted executable carries the selected release identity",
        "CreateProcessW":
            "the verified installed HUD is restarted directly without a shell",
        "FILE_READ_ATTRIBUTES | DELETE":
            "the restarted executable's writable ancestors remain rename-locked",
        "PROC_THREAD_ATTRIBUTE_HANDLE_LIST":
            "the helper child inherits only the explicit old-process handle",
        "HANDLE_FLAG_INHERIT":
            "the helper launcher rejects a non-inheritable process handle",
    }.items():
        require(helper_sources, token, reason)
    reject(helper_sources, "DeleteFileW(",
           "the helper must never delete an installed application file")
    reject(helper_sources, "ShellExecute",
           "restart must not delegate an unverified path to the shell")
    for token, reason in {
        "CopyFileW":
            "the running signed HUD is copied out of the MSI replacement path",
        "DuplicateHandle":
            "the launcher creates a real inheritable handle for the exact HUD process",
        "PROCESS_QUERY_LIMITED_INFORMATION":
            "the inherited process handle exposes only wait and identity access",
        "VerifyAndLaunchWindowsUpdateHelperCopy":
            "the copied helper is re-verified before its internal mode starts",
        "InstalledWindowsHudExecutablePath":
            "portable or unexpected launch locations cannot self-update",
    }.items():
        require(UPDATE_HELPER_LAUNCHER, token, reason)
    for token, reason in {
        "ConfiguredWindowsUpdatePublisherFingerprint":
            "the publisher pin is checked before update assets are downloaded",
        "BCryptGenRandom":
            "each update preparation uses a fresh unpredictable directory",
        "kMaximumSha256ManifestBytes":
            "the checksum download and read remain tightly bounded",
        "kMaximumWindowsInstallerBytes":
            "the MSI download keeps the fixed hard size limit",
        "ParseWindowsInstallerSha256Manifest":
            "the downloaded checksum must name the exact selected MSI",
        "LaunchPreparedWindowsUpdateHelper":
            "only the verified helper boundary can start installation",
        "BestEffortRemoveFailedPreparation":
            "failed preparation removes only its exact bounded files",
    }.items():
        require(UPDATE_INSTALL, token, reason)
    cleanup_sources = (
        UPDATE_DIRECTORY_CLEANUP_HEADER + UPDATE_DIRECTORY_CLEANUP
    )
    for token, reason in {
        "kMinimumRetainedUpdateDirectories = 2U":
            "maintenance always preserves the two newest managed directories",
        "7ULL * 24ULL * 60ULL * 60ULL":
            "maintenance never removes a directory from the last seven days",
        "IsManagedWindowsUpdateDirectoryName":
            "cleanup owns only the exact random update-directory namespace",
        "FILE_ATTRIBUTE_REPARSE_POINT":
            "cleanup rejects redirected directories and contents",
        "FILE_FLAG_OPEN_REPARSE_POINT":
            "cleanup opens deletion targets without following filesystem redirection",
        "kMaximumRootEntriesToInspect":
            "maintenance work stays bounded on an abnormal root",
        "kMaximumFilesPerManagedDirectory":
            "per-directory maintenance work stays bounded",
        "BestEffortCleanupWindowsUpdateDirectories":
            "cleanup failure cannot become an update failure",
    }.items():
        require(cleanup_sources, token, reason)
    reject(cleanup_sources, "remove_all",
           "cleanup must not recursively follow or erase an unchecked tree")
    for token, reason in {
        "TestExactManagedNameContract":
            "fixed tests pin the lowercase 32-hex namespace",
        "TestSevenDayAndNewestTwoRetention":
            "fixed tests pin both retention safeguards",
        "TestFilesReparsePointsAndUnknownTimesAreNeverSelected":
            "fixed tests prove unsafe entries are skipped",
        "TestFutureTimesFailClosed":
            "clock rollback cannot select a directory for deletion",
        "TestWindowsFilesystemCleanup":
            "Windows CI removes only the stale third managed directory",
    }.items():
        require(UPDATE_DIRECTORY_CLEANUP_TEST, token, reason)
    for token, reason in {
        "cancellationEpoch_":
            "window shutdown cancels an in-flight download worker",
        "pending_.has_value()":
            "one-click update requests are serialized",
        "PostMessageW":
            "download completion returns to the UI without blocking it",
    }.items():
        require(UPDATE_INSTALL_WORKER_HEADER + UPDATE_INSTALL_WORKER,
                token, reason)
    reject(UPDATE_INSTALL, "ShellExecute",
           "one-click update must not delegate an unchecked path to the shell")
    require_regex(
        MAIN,
        r"const bool helperStarted = completed->helperStarted\(\);.*?"
        r"if \(helperStarted\) \{\s*PostMessageW\(window, WM_CLOSE",
        "the running HUD may close only after the verified helper has started",
    )
    require_regex(
        MAIN,
        r"TryRunWindowsUpdateHelperCommandLine\(\).*?SetProcessDpiAwarenessContext",
        "helper mode must run before singleton, workers, or visible UI initialization",
    )
    for token, reason in {
        "an unproven old-process exit must stop before installation":
            "every wait failure is fixed as a no-install path",
        "a failed MSI should restart only the verified previous version":
            "installer failure recovers only through pinned old-version verification",
        "an unverified previous executable must remain stopped":
            "failure recovery cannot launch an unchecked old executable",
        "an unverified installed executable must not be treated as started":
            "post-install signature failure remains fail closed",
        "wait, verified install, and verified restart must be ordered":
            "the complete helper sequence has a fixed ordering test",
    }.items():
        require(UPDATE_HELPER_TEST, token, reason)
    test_helper_macro = "CODEX_MONITOR_UPDATE_HELPER_TESTING"
    if CMAKE.count(test_helper_macro) != 1:
        raise AssertionError(
            "the injectable helper sequencing macro must belong to exactly one test target"
        )
    require(UPDATE_HELPER_HEADER, test_helper_macro,
            "the helper callback seam is hidden from production builds")
    require(UPDATE_HELPER, test_helper_macro,
            "the helper callback implementation is hidden from production builds")
    require(RESOURCE_SCRIPT, "CODEX_MONITOR_WINDOWS_VERSION_MAJOR",
            "the executable resource version follows the CMake project version")
    reject(RESOURCE_SCRIPT, "FILEVERSION 0,3,0,0",
           "the executable resource version must not be hard-coded separately")
    require(CMAKE, "CODEX_MONITOR_WINDOWS_VERSION_PATCH=${PROJECT_VERSION_PATCH}",
            "all executable version components come from one CMake source")
    require((WINDOWS_ROOT / "installer" / "CodexMonitorHUD.wxs").read_text(
                encoding="utf-8"),
            'Schedule="afterInstallInitialize"',
            "major upgrades keep rollback protection after the old version is removed")
    require((WINDOWS_ROOT / "build-installer.ps1").read_text(encoding="utf-8"),
            "$patchVersion -gt 65535",
            "MSI versions are rejected before exceeding Windows Installer limits")
    require((WINDOWS_ROOT / "build-installer.ps1").read_text(encoding="utf-8"),
            "$binaryVersionInfo.ProductVersion -ne $sourceVersion",
            "the MSI cannot wrap an executable carrying a different version")

    diagnosis_contracts = {
        "Missing a low metric cannot prove":
            "incomplete low readings cannot claim that the computer is comfortable",
        "shareOfBusyCpu":
            "target-app CPU impact is related to current whole-machine pressure",
        "shareOfPhysicalMemory":
            "target-app working set is compared with physical memory capacity",
        "working set contains shared pages":
            "memory attribution is explicitly confidence-limited",
    }
    for token, reason in diagnosis_contracts.items():
        require(DIAGNOSIS, token, reason)

    diagnosis_test_contracts = {
        "missing metrics must remain unavailable": "missing evidence has a fixed safe result",
        "a low CPU sample alone cannot prove": "partial system evidence cannot report comfort",
        "a low partial process total is a lower bound":
            "partial target readings cannot prove low impact",
        "target CPU above system CPU must be rejected":
            "inconsistent attribution data is not silently corrected",
    }
    for token, reason in diagnosis_test_contracts.items():
        require(DIAGNOSIS_TEST, token, reason)

    package_contracts = {
        "Set-StrictMode -Version Latest": "packaging fails on undeclared PowerShell state",
        "Resolve-Path -LiteralPath $BinaryPath": "the package input must exist",
        "CodexMonitorHUD-windows-x64": "the portable package has a stable platform name",
        "Copy-Item -LiteralPath $licensePath": "the portable package carries its license",
        "Copy-Item -LiteralPath $readmePath": "the portable package carries Windows instructions",
        "Compress-Archive -LiteralPath $stagingDirectory":
            "the package preserves a single top-level directory",
        "Get-FileHash -LiteralPath $archivePath -Algorithm SHA256":
            "the package receives a SHA-256 checksum",
        "[System.Text.Encoding]::ASCII": "the checksum file has deterministic text encoding",
    }
    for token, reason in package_contracts.items():
        require(PACKAGE_SCRIPT, token, reason)

    package_workflow_contracts = {
        "./windows/package-release.ps1": "Windows CI creates the portable package",
        "Build output is not an x64 PE executable":
            "Windows CI verifies that the release binary is really x64",
        "Windows archive SHA-256 verification failed":
            "Windows CI independently verifies the package checksum",
        "Expand-Archive -LiteralPath $archive":
            "Windows CI opens the package before publishing it",
        "Windows archive contains an unexpected file set":
            "Windows CI fixes the portable archive contents",
        "Packaged executable does not match the tested build output":
            "Windows CI compares the packaged EXE with the tested EXE",
        "CodexMonitorHUD-windows-x64-portable":
            "Windows CI publishes the archive and checksum together",
    }
    for token, reason in package_workflow_contracts.items():
        require(WINDOWS_WORKFLOW, token, reason)

    installer_contracts = {
        'InstallScope="perUser"': "the MSI defaults to a no-admin per-user install",
        'Platform="x64"': "the MSI declares its x64 platform",
        'Id="CodexMonitorExecutable"': "the tested HUD executable is the MSI key file",
        'Id="LicenseFile"': "the installed application carries its license",
        'Id="WindowsReadme"': "the installed application carries Windows instructions",
        'Id="ApplicationStartMenuShortcut"': "the MSI creates a Start menu entry",
        'Name="InstallFolder"':
            "a per-user registry value is the application component key path",
        'Id="PreviousInstallFolderSearch"':
            "maintenance and uninstall recover a customized installation directory",
        'Id="RemoveInstallFolder"': "uninstall removes the empty application directory",
        'Id="RemoveLocalProgramsFolder"':
            "the per-user parent directory is represented in the RemoveFile table",
        'On="uninstall"': "the MSI removes its Start menu directory",
    }
    for token, reason in installer_contracts.items():
        require(INSTALLER_SOURCE, token, reason)

    installer_script_contracts = {
        "Could not read the Windows app version from CMakeLists.txt":
            "the MSI version is derived from the Windows project version",
        "candle.exe": "the MSI source is compiled with the pinned runner WiX toolset",
        "light.exe": "the MSI object is linked with the pinned runner WiX toolset",
        "Get-FileHash -LiteralPath $installerPath -Algorithm SHA256":
            "the MSI receives a SHA-256 checksum",
    }
    for token, reason in installer_script_contracts.items():
        require(INSTALLER_SCRIPT, token, reason)

    installer_workflow_contracts = {
        "./windows/build-installer.ps1": "Windows CI builds the MSI",
        "Test MSI install and uninstall": "Windows CI exercises the real MSI lifecycle",
        "Installed executable does not match the tested build output":
            "Windows CI verifies the installed EXE identity",
        "MSI uninstall left the application executable behind":
            "Windows CI verifies that uninstall removes the app",
        "CodexMonitorHUD-windows-x64-msi-unsigned":
            "the unsigned status is explicit on the development artifact",
    }
    for token, reason in installer_workflow_contracts.items():
        require(WINDOWS_WORKFLOW, token, reason)

    upgrade_workflow_contracts = {
        "fetch-depth: 0":
            "Windows CI keeps the pinned previous source available",
        "Test real MSI major upgrade from 0.2.0":
            "Windows CI exercises a real previous-to-current upgrade",
        "./windows/tests/test-msi-major-upgrade.ps1":
            "the fixed major-upgrade test is part of the release gate",
    }
    for token, reason in upgrade_workflow_contracts.items():
        require(WINDOWS_WORKFLOW, token, reason)

    upgrade_test_contracts = {
        "2c2a103534596b1f191d6e9475b32738794bf9a2":
            "the previous 0.2.0 source baseline cannot drift",
        'expectedPreviousVersion = "0.2.0"':
            "the previous MSI identity is explicit",
        "MsiEnumRelatedProducts":
            "the test proves only one related product remains registered",
        "Major-upgrade MSI files must have different ProductCodes":
            "a same-product reinstall cannot masquerade as an upgrade",
        "Previous and current MSI UpgradeCodes do not match":
            "the major-upgrade family identity must remain stable",
        "preserve across upgrade":
            "the customized install directory keeps user-created content",
        "Previous MSI unexpectedly downgraded":
            "the current install is protected from rollback",
        "WIX_DOWNGRADE_DETECTED":
            "downgrade rejection is distinguished from unrelated MSI failures",
        "reason other than the expected downgrade gate":
            "an arbitrary MSI failure cannot masquerade as rollback protection",
        "Related products remain registered after cleanup":
            "the real MSI test fails when it leaves runner state behind",
    }
    for token, reason in upgrade_test_contracts.items():
        require(MSI_MAJOR_UPGRADE_TEST, token, reason)
    reject(MSI_MAJOR_UPGRADE_TEST, "Win32_Product",
           "upgrade verification must not trigger unrelated MSI self-repair")

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

    codex_process_contracts = {
        "IsSafeCodexExecutable": "the launcher validates the executable before creation",
        "path.is_absolute()": "automatic executable discovery rejects relative paths",
        "kMaximumDirectPathDirectories":
            "PATH discovery reserves candidate budget for official user layouts",
        "CreateProcessW": "the app-server starts directly through the Win32 process API",
        "executable.c_str()": "the absolute executable is supplied as lpApplicationName",
        "PROC_THREAD_ATTRIBUTE_HANDLE_LIST":
            "the child inherits only the three intended pipe handles",
        "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE":
            "closing the Job cannot orphan app-server descendants",
        "AssignProcessToJobObject": "the suspended child joins the Job before it runs",
        "CREATE_NO_WINDOW": "the background app-server does not flash a console",
        "CREATE_SUSPENDED": "Job assignment happens before child execution",
        "kMaximumLineBytes = 1024 * 1024": "one NDJSON line is capped at one MiB",
        "kMaximumStderrBytes = 16 * 1024": "retained stderr is capped at sixteen KiB",
        "kDefaultTotalTimeout{15000}": "one app-server process has a hard fifteen-second cap",
    }
    codex_process_sources = (
        CODEX_EXECUTABLE_HEADER
        + CODEX_EXECUTABLE
        + CODEX_PROCESS_HEADER
        + CODEX_PROCESS
    )
    for token, reason in codex_process_contracts.items():
        require(codex_process_sources, token, reason)
    reject(CODEX_PROCESS, "ShellExecute", "the app-server must not launch through a shell")
    reject(CODEX_PROCESS, "system(", "the app-server must not launch through a command shell")

    codex_process_test_contracts = {
        "TestExecutableDiscovery": "current-directory executable injection is covered",
        "TestFragmentedAndMultipleLines": "direct NDJSON process transport is covered",
        "TestOversizedLine": "the NDJSON line cap is covered",
        "TestStderrMemoryLimit": "the stderr memory cap is covered",
        "TestStopKillsJobTree": "descendant cleanup is covered",
        "TestTotalTimeoutKillsSilentServer": "the hard process timeout is covered",
    }
    for token, reason in codex_process_test_contracts.items():
        require(CODEX_PROCESS_TEST, token, reason)

    app_server_contracts = {
        'L"initialize"': "the official initialization request is sent first",
        'L"initialized"': "the initialization notification completes the handshake",
        'L"account/rateLimits/read"': "rate limits use the official method",
        'L"account/read"': "subscription type uses the official method",
        'L"account/usage/read"': "daily usage uses the official method",
        'L"thread/list"': "recent task history uses the official method",
        'L"app-server"': "each refresh launches app-server mode",
        'L"--stdio"': "the official newline-delimited stdio transport is selected",
        "kMaximumResponseLines = 512": "one refresh cannot parse unbounded response lines",
        "isCancelled": "the caller can cancel an in-flight refresh",
        "kCancellationPollInterval{200}": "blocked reads notice cancellation promptly",
        "ParseInitializeCodexHomeResultJson":
            "initialize can expose only a validated thread-confined Codex home",
    }
    app_server_sources = CODEX_APP_SERVER_HEADER + CODEX_APP_SERVER
    for token, reason in app_server_contracts.items():
        require(app_server_sources, token, reason)
    require(CODEX_APP_SERVER_TEST, "TestResponseLineLimitStopsFlood",
            "the response-line cap is covered by an integration test")
    require(CODEX_APP_SERVER_TEST, "TestCancellationStopsTheJobWithoutMethodFailures",
            "process cancellation is covered by an integration test")
    require(CODEX_APP_SERVER_TEST,
            "TestCodexHomeMissingAndUntrustedValuesAreNotRetained",
            "missing and untrusted initialize Codex-home values are covered")
    reject(struct_body(CODEX_APP_SERVER_HEADER, "AppServerRefreshReport"),
           "codexHome", "Codex home must not enter app-server refresh reports")
    reject(MAIN, "codexHome", "Codex home must not enter UI code")

    privacy_model = without_cpp_comments(CODEX_TYPES)
    for forbidden in ("email", "preview", "id", "cwd", "path"):
        reject_regex(
            privacy_model,
            rf"\b{forbidden}\b",
            f"the retained Codex product model must not contain {forbidden}",
        )
    require(CODEX_TYPES, "ProcessLocalThreadStatus",
            "thread status is named as process-local in the product model")
    for assertion in (
        "!HasEmailMember<AccountData>::value",
        "!HasPreviewMember<ProcessLocalThread>::value",
        "!HasPathMember<ProcessLocalThread>::value",
        "!HasIdMember<ProcessLocalThread>::value",
    ):
        require(CODEX_APP_SERVER_TEST, assertion,
                "privacy exclusions are protected by compile-time tests")

    codex_worker_contracts = {
        "std::thread thread_": "the worker owns one serial background thread",
        "CodexAppServerClient client": "the protocol client is owned on that worker thread",
        "std::condition_variable": "the worker sleeps between demand and refresh deadlines",
        "kSuccessfulRefreshDelay{300}": "normal refreshes wait five minutes",
        "kFailedRefreshDelays": "failed refreshes use a bounded backoff sequence",
        "std::chrono::seconds{900}": "failure backoff is capped at fifteen minutes",
        "cancellationEpoch_": "pause uses a monotonic cancellation generation",
        "quotaForecastEpoch_": "hiding forecast invalidates a pending history commit",
        "SetQuotaForecastEnabled": "quota history work follows forecast-card visibility",
        "SetCostHistoryEnabled":
            "local cost history is gated independently by visible-card demand",
        "costHistoryEpoch_":
            "disabled or hidden cost results cannot be published later",
        "!scan.discoveryIncomplete && previous.size() != scan.files.size()":
            "partial discovery does not rewrite an unchanged preserved cache",
        "costHistoryCacheDirty":
            "a failed restart-cache write remains pending for a later retry",
        "costHistoryCacheWriteBlocked":
            "a newer cache version is not retried or downgraded every refresh",
        "MoveFileExW": "Windows history replacement is atomic and replaces an old file",
        "fetch_add": "pause and stop permanently invalidate an old generation",
        "PauseAndInvalidate": "no-demand states invalidate in-flight work",
        "ActivateAndRefresh": "resume immediately requests fresh data",
        "FindCodexExecutable": "executable discovery happens inside the short refresh",
        "init_apartment": "the worker initializes its C++/WinRT apartment",
        "uninit_apartment": "the worker releases its C++/WinRT apartment",
        "StopAndJoin": "destruction waits for the worker and child process",
    }
    worker_sources = CODEX_WORKER_HEADER + CODEX_WORKER
    for token, reason in codex_worker_contracts.items():
        require(worker_sources, token, reason)
    if CODEX_WORKER_HEADER.count("std::thread thread_;") != 1:
        raise AssertionError("the Codex worker must own exactly one serial worker thread")
    require_regex(
        CODEX_WORKER,
        r"PostMessageW\s*\(\s*notifyWindow\s*,\s*notifyMessage\s*,\s*0\s*,\s*0\s*\)",
        "the UI notification must carry no owning raw pointer",
    )
    completed_refresh = struct_body(CODEX_WORKER_HEADER, "CompletedCodexRefresh")
    for token in (
        "CodexDataState data",
        "AppServerRefreshReport report",
        "bool succeeded",
        "nextRefreshDelay",
    ):
        require(completed_refresh, token,
                "the completed refresh contains only the trimmed model and refresh metadata")
    for forbidden in ("raw", "stderr", "path", "codexHome"):
        reject_regex(
            completed_refresh,
            rf"\b{forbidden}\b",
            f"completed refreshes must not retain {forbidden}",
        )
    for token, reason in {
        "TestSuccessfulBackgroundRefresh":
            "the complete worker-to-window success path is covered",
        "TestPauseCancelsAndResumeRefreshes":
            "pause, child cleanup, generation invalidation, and resume are covered",
        "TestFailureBackoffAndRecovery":
            "consecutive failures back off and a success restores normal cadence",
        "ProcessWithImagePathExists":
            "the worker integration test checks for orphaned app-server processes",
        "TestQuotaForecastDemandGating":
            "forecast history is covered across disabled, enabled, failure, and cancellation paths",
        "a refresh that observed malformed history must publish suppression metadata to the UI":
            "worker repair cannot hide same-refresh history damage from alert delivery",
    }.items():
        require(CODEX_WORKER_TEST, token, reason)

    weekly_quota_alert_sources = (
        WEEKLY_QUOTA_ALERT_HEADER + WEEKLY_QUOTA_ALERT
    )
    for token, reason in {
        "bool enabled = false":
            "weekly quota alerts remain opt-in",
        "double thresholdPercent = 15.0":
            "the inactive default threshold matches the macOS policy",
        "WeeklyQuotaAlertMode::kRolling24Hours":
            "rolling twenty-four hours is the default mode",
        "policy.thresholdPercent >= 5.0":
            "the minimum user threshold is enforced",
        "policy.thresholdPercent <= 100.0":
            "the maximum user threshold is enforced",
        "kMaximumBaselineLatenessSeconds = 30LL * 60LL":
            "baselines too far from midnight or twenty-four hours ago are rejected",
        "*sample.weekly.resetsAtUnixSeconds == resetAtUnixSeconds":
            "only one exact weekly reset cycle contributes",
        "sample.capturedAtUnixSeconds <= nowUnixSeconds":
            "future samples cannot trigger an alert",
        "kClockMovedBackward":
            "wall-clock rollback has an explicit non-alert outcome",
        "kRemainingIncreased":
            "quota replenishment or non-monotonic data suppresses alerts",
        "nowUnixSeconds - notifiedAt <":
            "overlapping rolling windows cannot repeat within twenty-four hours",
        "localDayStartUnixSeconds":
            "natural-day evaluation receives the platform-derived local midnight",
    }.items():
        require(weekly_quota_alert_sources, token, reason)
    reject(WEEKLY_QUOTA_ALERT_HEADER, "windows.h",
           "quota alert decisions must remain portable and deterministic")

    for token, reason in {
        "alerts default to disabled, fifteen percent, and rolling twenty-four hours":
            "default settings are fixed by a sample",
        "the same local calendar day must notify only once":
            "natural-day anti-duplication is covered",
        "overlapping rolling windows must not repeat within twenty-four hours":
            "rolling anti-duplication is covered",
        "a changed weekly reset must clear prior-cycle suppression":
            "weekly reset rollover is covered",
        "future samples must never become a rolling baseline":
            "future history is covered",
        "any same-cycle increase inside the period makes the data unsafe":
            "non-monotonic remaining percentages are covered",
    }.items():
        require(WEEKLY_QUOTA_ALERT_TEST, token, reason)

    for token, reason in {
        "kMaximumStateBytes = 1024":
            "persistent anti-duplication state has a hard byte limit",
        "kMaximumLineBytes = 128":
            "persistent fields have a hard line limit",
        "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH":
            "Windows state replacement is atomic and flushed",
        "kUnsupportedVersion":
            "newer state formats are never overwritten",
        "TemporaryPathFor":
            "state writes use a same-directory temporary file",
    }.items():
        require(WEEKLY_QUOTA_ALERT_STATE_STORE, token, reason)
    for token, reason in {
        "a damaged file must expose only a safe empty fallback state":
            "corrupt-state fallback is fixed",
        "a newer unknown state format must never be overwritten":
            "unknown-version preservation is covered",
        "replacement failure must preserve old bytes":
            "atomic failure preservation is covered",
    }.items():
        require(WEEKLY_QUOTA_ALERT_STATE_STORE_TEST, token, reason)

    weekly_alert_delivery_sources = (
        WEEKLY_QUOTA_ALERT_DELIVERY_HEADER + WEEKLY_QUOTA_ALERT_DELIVERY +
        WEEKLY_QUOTA_NOTIFICATION + MAIN + MODULE_STATE
    )
    for token, reason in {
        "notificationFacilityAvailable":
            "an unavailable Windows notification facility short-circuits before persistence",
        "stateStore.Save(evaluation.nextState)":
            "the anti-duplication state is committed before notification delivery",
        "sender(notification)":
            "notification delivery has a narrow percentage-only callback",
        "stateStore.Save(priorState)":
            "a rejected notification restores the prior anti-duplication state",
        "history.skippedMalformedLines != 0":
            "damaged history suppresses rather than estimating a notification",
        "RecoverMalformedStateForCurrentPeriod":
            "known damaged state recovers conservatively without permanent disablement",
        "quotaHistoryHadMalformedLinesThisRefresh":
            "worker-detected history damage suppresses the same UI refresh",
        "Shell_NotifyIconW(NIM_MODIFY":
            "Windows receives a non-activating Shell notification",
        "NIIF_RESPECT_QUIET_TIME":
            "Windows quiet-time policy is respected",
        "weeklyAlertEnabled ||":
            "enabled alerts retain the existing Codex quota refresh while hidden",
        "weekly_quota_alert_enabled=":
            "alert enablement uses the versioned settings whitelist",
        "weekly_quota_alert_threshold=":
            "the user threshold is persisted independently",
        "weekly_quota_alert_mode=":
            "natural-day and rolling mode are persisted independently",
    }.items():
        require(weekly_alert_delivery_sources, token, reason)
    reject(WEEKLY_QUOTA_ALERT_DELIVERY_HEADER, "windows.h",
           "delivery sequencing remains portable and fixed-testable")
    for token, reason in {
        "an unavailable notification facility must not pop up or be recorded as reminded":
            "unavailable notification handling is covered",
        "the next anti-duplication state must be atomically saved before Windows is asked to notify":
            "save-before-notify ordering is covered",
        "the same rolling period must not deliver a duplicate notification":
            "product-level single-period suppression is covered",
        "an atomic state failure must suppress the notification":
            "persistence failure cannot notify",
        "a notification rejected by Windows must not remain recorded as delivered":
            "notification rejection rollback is covered",
        "insufficient history must never produce a notification":
            "insufficient product history is covered",
        "a changed weekly reset must start one fresh notification cycle":
            "weekly reset rollover is covered at the delivery boundary",
        "any skipped malformed history line must suppress rather than estimate a notification":
            "partial history cannot cause a false alert",
        "malformed-state recovery must allow one notification in the next rolling period":
            "known damaged state recovers after a safe suppression period",
    }.items():
        require(WEEKLY_QUOTA_ALERT_DELIVERY_TEST, token, reason)

    activity_sources = (
        CODEX_ACTIVITY_SCAN_HEADER + CODEX_ACTIVITY_SCAN +
        CODEX_ACTIVITY_WORKER_HEADER + CODEX_ACTIVITY_WORKER
    )
    for token, reason in {
        "kCodexActivityWindowSeconds = 120":
            "only sessions written in the bounded two-minute window are considered",
        "kCodexActivityMaximumTailBytes":
            "one recent session read is capped at one MiB",
        "kCodexActivityMaximumCandidateFiles = 64":
            "the number of recent session candidates is bounded",
        "ParseCodexActivityJsonlLine":
            "activity parsing has a narrow metadata-only boundary",
        "root.payload->role":
            "only the documented message role participates in inference",
        "root.payload->phase":
            "only the documented final-answer phase completes a turn",
        "kRecentFilesUnresolved":
            "compressed-only or unreadable recent state cannot become a zero",
        "unresolvedRecentFileCount":
            "partial coverage remains visible to presentation",
        "FILE_FLAG_OPEN_REPARSE_POINT":
            "Windows file reads do not follow redirected candidates",
        "O_NOFOLLOW":
            "portable tests exercise the same no-follow file boundary",
        "kActiveRefreshDelay{5}":
            "active sessions use the five-second cadence",
        "kIdleRefreshDelay{20}":
            "idle and unavailable sessions use the low-burden twenty-second cadence",
        "cancellationEpoch_":
            "hidden or paused activity results cannot write back later",
        "PauseAndInvalidate":
            "activity scanning stops when its module is not visible",
        "StopAndJoin":
            "activity worker teardown joins its background thread",
    }.items():
        require(activity_sources, token, reason)
    for token, reason in {
        "TestPureInferenceMatchesMacSafetyRules":
            "start, finish, staleness, and documented event forms have fixed samples",
        "TestPrivateTextCannotImpersonateMetadata":
            "prompt and response text cannot impersonate structural metadata",
        "TestBoundedFilesystemScanAndHonestDegradation":
            "compressed and unreadable recent coverage has fixed degradation tests",
        "TestCandidateAndTailLimits":
            "the 64-file and one-MiB limits are executed",
        "far-future event timestamp must not invent activity":
            "clock anomalies cannot manufacture activity",
    }.items():
        require(CODEX_ACTIVITY_SCAN_TEST, token, reason)
    for token, reason in {
        "TestCadenceAndRepeatedRequestCoalescing":
            "five/twenty-second cadence and request merging are fixed",
        "TestPauseDiscardsInflightResult":
            "an in-flight hidden result is discarded before resume",
        "TestStopAndJoinLeavesNoThreadOrResult":
            "worker shutdown leaves no publishable result or thread work",
    }.items():
        require(CODEX_ACTIVITY_WORKER_TEST, token, reason)

    codex_usage_contracts = {
        "bool todayAvailable": "today has an explicit availability bit",
        "optional<std::int64_t> todayTokens":
            "a missing today bucket cannot be represented as a fabricated zero",
        "latestDate": "delayed settlement still exposes the latest reported date",
        "latestTokens": "delayed settlement still exposes the latest reported amount",
        "last7DaysTokens": "the recent seven-day total is available",
        "previous7DaysTokens": "the preceding seven-day comparison is available",
        "thirtyDayTokens": "the thirty-day total is available",
        "monthToDateTokens": "the reference month's usage is available",
        "monthForecastTokens": "the monthly linear projection is explicitly optional",
        "SaturatingLinearForecast": "monthly projection cannot overflow",
        "anchorDay - 29": "rolling totals anchor to the latest valid day",
    }
    usage_sources = CODEX_USAGE_HEADER + CODEX_USAGE
    for token, reason in codex_usage_contracts.items():
        require(usage_sources, token, reason)
    codex_usage_test_contracts = {
        "TestDelayedSettlementAndAnchoredWindows": "delayed daily settlement is covered",
        "TestDuplicateBuckets": "same-day buckets are combined",
        "TestLeapYearAndMonthBoundary": "Gregorian month and leap boundaries are covered",
        "TestBadAndFutureDates": "future and invalid inputs cannot inflate totals",
        "TestEmptyAndUnavailableData": "missing source data stays unavailable",
        "TestSaturatingLargeIntegers": "large integer behavior is covered",
    }
    for token, reason in codex_usage_test_contracts.items():
        require(CODEX_USAGE_TEST, token, reason)

    service_status_contracts = {
        "Codex in ChatGPT Desktop":
            "the public Statuspage component is identified explicitly",
        "MapOpenAIServiceStatus":
            "provider strings are mapped through a stable presentation model",
        'kStatusHost[] = L"status.openai.com"':
            "the network target is fixed to OpenAI's official status host",
        'kStatusPath[] = L"/api/v2/summary.json"':
            "the client reads only the public summary endpoint",
        "kMaximumResponseBytes = 1024 * 1024":
            "the public response has a strict memory bound",
        "WINHTTP_DISABLE_COOKIES":
            "the public status request never uses browser cookies",
        "WINHTTP_DISABLE_AUTHENTICATION":
            "the public status request disables automatic account authentication",
        "WINHTTP_DISABLE_REDIRECTS":
            "the fixed official request cannot redirect to another host",
        "cancellationEpoch_":
            "pause and stop invalidate an in-flight status result",
        "kSuccessfulRefreshDelay{900}":
            "healthy status checks use the low-burden fifteen-minute cadence",
        "std::chrono::seconds{120}":
            "status failures use bounded progressive backoff",
        "retainedStatus_":
            "a transient failure can keep the last verified status visible",
        "resumeNotBefore_":
            "page switching cannot bypass the low-burden refresh cadence",
        "PostMessageW(notifyWindow, notifyMessage, 0, 0)":
            "status completion carries no owning pointer across threads",
    }
    service_sources = (
        SERVICE_STATUS_MODEL + SERVICE_STATUS_JSON + SERVICE_STATUS_FETCH +
        SERVICE_STATUS_WORKER_HEADER + SERVICE_STATUS_WORKER
    )
    for token, reason in service_status_contracts.items():
        require(service_sources, token, reason)
    for token, reason in {
        "TestCodexComponentMappings": "all documented Codex health values are fixed",
        "TestOverallFallbackMappings": "missing Codex status safely falls back to overall",
    }.items():
        require(SERVICE_STATUS_MODEL_TEST, token, reason)
    for token, reason in {
        "TestOperationalSummary": "the normal public payload is parsed",
        "TestMalformedJsonFails": "malformed public payloads stay unavailable",
        "TestKnownFieldTypeErrorsFail":
            "known status fields with wrong types stay unavailable",
    }.items():
        require(SERVICE_STATUS_JSON_TEST, token, reason)
    for token, reason in {
        "TestActivationRefreshesImmediatelyAndUsesSuccessCadence":
            "visibility demand triggers one immediate status refresh",
        "TestFailureRetainsLastSuccess":
            "a transient status failure retains the last verified result",
        "TestFailureBackoffCapsAndResetsAfterSuccess":
            "the full failure backoff and recovery reset are fixed",
        "TestResumeReusesFreshResultWithoutExtraFetch":
            "restoring a fresh status does not trigger a duplicate request",
        "TestPauseDiscardsInflightResultAndResumeRefreshes":
            "hidden-module cancellation and resume are fixed",
        "TestStopAndJoinCompletes":
            "window teardown cannot leave the status worker behind",
    }.items():
        require(SERVICE_STATUS_WORKER_TEST, token, reason)

    main_codex_contracts = {
        "CurrentPageNeedsCodexData": "Codex work follows visible module demand",
        "HomeNeedsCodexData": "Home can request Codex independently from performance data",
        "NativePageNeedsCodexData": "the Codex native page follows its own switches",
        "CurrentPageNeedsServiceStatus":
            "official status work follows only a currently visible status module",
        "UpdateServiceStatusDemand":
            "official status lifecycle transitions share one demand gate",
        "serviceStatusWorker.PauseAndInvalidate":
            "hidden or minimized status cards stop future network work",
        "WM_SHOWWINDOW": "hiding the HUD re-evaluates official status demand",
        "UpdateCodexDemand": "all demand transitions share one start/stop gate",
        "codexWorker.PauseAndInvalidate": "minimize and hidden modules stop Codex work",
        "totals->monthForecastTokens":
            "the tested monthly Token projection is displayed in the HUD",
        "月末约：当前数据不足":
            "the HUD does not fabricate a monthly projection when evidence is missing",
        "codexWorker.ActivateAndRefresh": "new demand refreshes immediately",
        "codexWorker.Start": "the app-server worker starts with the window",
        "codex-cost-history-cache.txt":
            "the worker receives an independent restart cache path",
        "kCodexReadyMessage": "worker completion has a dedicated UI message",
        "codexWorker.TakeLatest": "the UI takes a copied privacy-trimmed result",
        "codexWorker.StopAndJoin": "window teardown reaps the Codex worker",
        "ApplyCodexRefresh": "real returned data is applied to cards",
        "BuildQuotaCardText": "quota cards use parsed rate-limit data",
        "BuildQuotaForecastCardText": "the HUD shows the tested quota trend forecast",
        "CurrentPageShowsQuotaForecast":
            "forecast history work follows only a currently visible forecast card",
        "BuildSubscriptionCardText": "subscription cards use parsed account data",
        "CalculateUsageCalendarTotals": "Token cards use official daily totals",
        "BuildRecentTasksCardText": "recent-task cards use official task history",
        "app-server 进程范围": "task status is visibly scoped to this app-server process",
        "WM_VSCROLL": "overflowing cards support scrollbar navigation",
        "WM_MOUSEWHEEL": "overflowing cards support wheel navigation",
        "UpdateContentScrollBar": "scroll range follows measured card pixels",
        "MeasureCardHeight": "card height follows its wrapped display text",
        "BuildVariableHeightGridRows":
            "dynamic card rows use the tested portable layout model",
        "SetContentScrollOffset":
            "wheel and native scrollbar input share pixel clamping",
        "WM_ENTERSIZEMOVE":
            "interactive resize captures a stable opposite-edge anchor",
        "WM_SIZING": "corner and side dragging are constrained uniformly",
        "ConstrainUniformResize":
            "interactive Win32 sizing uses the tested portable math",
        "windowScale": "uniform visual scale is persisted and applied",
        "kSettingsWindowLockId":
            "settings expose a position-and-size lock",
        "kSettingsCornerBaseId": "settings expose four corner presets",
        "kSettingsOpacityBaseId": "settings expose bounded whole-window opacity",
        "kSettingsThemeBaseId": "settings expose four low-saturation themes",
        "kWindowLockButtonId": "the main window exposes a direct lock toggle",
        "kUpdateBannerButtonId":
            "an available update is a visible user-clicked path",
        "homeVisibleCheck": "settings expose the Home visibility checkbox",
        "nativeVisibleCheck": "settings expose the own-page visibility checkbox",
        "kSettingsVisibleBaseId": "Home checkbox commands are independently routed",
        "kSettingsNativeVisibleBaseId": "own-page checkbox commands are independently routed",
        "kSettingsCheckUpdatesId": "settings expose an explicit update check button",
        "kSettingsInstallUpdateId": "settings expose an explicit one-click update button",
        "kUpdateReadyMessage": "update completion has a dedicated UI message",
        "kUpdateInstallReadyMessage":
            "download and helper completion has a dedicated UI message",
        "updateWorker.StopAndJoin": "window teardown stops the update worker",
        "updateInstallWorker.StopAndJoin":
            "window teardown joins the cancellable install worker",
        "FreshWindowsReleaseForInstall":
            "cached version notices cannot install without a fresh checked release",
        "IsRunningFromMsiInstalledWindowsHud":
            "the settings UI identifies portable launches before download",
        "CanRequestWindowsUpdateInstall":
            "button and command entry share the same MSI-only preflight",
        "便携版请下载 MSI 安装包更新":
            "portable users receive a clear update path instead of a late failure",
        "便携版不可安装":
            "the disabled one-click button explains why it is unavailable",
        "BS_AUTOCHECKBOX": "visibility controls have visible native check states",
        "BM_GETCHECK": "visibility changes read the actual checkbox state",
        "settingsScrollMaximum": "small high-DPI screens have bounded settings scrolling",
        "SetSettingsScrollOffset": "settings wheel and scrollbar input share one clamp",
        "WS_VSCROLL": "the settings window exposes native vertical scrolling",
        "MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)":
            "the settings window is initially constrained to its monitor work area",
    }
    for token, reason in main_codex_contracts.items():
        require(MAIN, token, reason)
    reject(MAIN, "SetForegroundWindow",
           "settings and automatic update paths must not force foreground focus")
    reject(MAIN, "MessageBox",
           "background and duplicate-instance paths must not interrupt the user")
    if MAIN.count("UpdateCodexDemand(") < 5:
        raise AssertionError(
            "Codex demand must be reconsidered on startup, page/settings changes, and minimize/restore"
        )
    if MAIN.count("UpdateCodexActivityDemand(") < 7:
        raise AssertionError(
            "activity demand must be reconsidered on show, startup, page/settings, and size changes"
        )
    if MAIN.count("UpdateServiceStatusDemand(") < 6:
        raise AssertionError(
            "service-status demand must be reconsidered on show, startup, page/settings, and size changes"
        )

    hud_sources = cmake_call_body(CMAKE, "add_executable", "CodexMonitorHUD")
    hud_source_contracts = {
        "resources/CodexMonitorHUD.rc": "the application icon resource is compiled into the HUD",
        "src/codex/codex_activity_scan.cpp":
            "bounded local activity inference is compiled into the HUD",
        "src/codex/codex_activity_worker.cpp":
            "visibility-driven activity scanning is compiled into the HUD",
        "src/codex/codex_app_server_client.cpp":
            "the official protocol client is compiled into the HUD",
        "src/codex/codex_executable.cpp": "safe codex.exe discovery is compiled into the HUD",
        "src/codex/codex_json_win32.cpp": "the privacy-trimmed parser is compiled into the HUD",
        "src/codex/codex_process.cpp": "the bounded child process transport is compiled into the HUD",
        "src/codex/codex_refresh_schedule.cpp": "Codex request coalescing is compiled into the HUD",
        "src/codex/codex_usage_math.cpp": "official daily usage math is compiled into the HUD",
        "src/codex/codex_worker.cpp": "the serial Codex worker is compiled into the HUD",
        "src/codex/codex_cost_history_store.cpp":
            "the bounded restart cache is compiled into the HUD",
        "src/service_status_fetch_win32.cpp":
            "the bounded official status transport is compiled into the HUD",
        "src/service_status_json_win32.cpp":
            "the official status parser is compiled into the HUD",
        "src/service_status_model.cpp":
            "the stable status presentation model is compiled into the HUD",
        "src/service_status_worker.cpp":
            "the visibility-driven status worker is compiled into the HUD",
        "src/update/github_release_selector.cpp":
            "strict Windows release selection is compiled into the HUD",
        "src/update/update_fetch_win32.cpp":
            "the bounded public GitHub transport is compiled into the HUD",
        "src/update/update_asset_download_win32.cpp":
            "the allow-listed update asset downloader is compiled into the HUD",
        "src/update/update_installer_verifier_win32.cpp":
            "strict MSI SHA-256 verification is compiled into the HUD",
        "src/update/update_msi_identity_win32.cpp":
            "publisher trust and MSI identity verification are compiled into the HUD",
        "src/update/update_apply_transaction_win32.cpp":
            "the continuous-lock update apply transaction is compiled into the HUD",
        "src/update/update_helper_win32.cpp":
            "the controlled exit/install/restart helper mode is compiled into the HUD",
        "src/update/update_helper_launcher_win32.cpp":
            "the verified temporary helper launcher is compiled into the HUD",
        "src/update/update_directory_cleanup_win32.cpp":
            "bounded old update-directory cleanup is compiled into the HUD",
        "src/update/update_install_win32.cpp":
            "the bounded one-click update preparation is compiled into the HUD",
        "src/update/update_install_worker.cpp":
            "the cancellable update preparation worker is compiled into the HUD",
        "src/update/update_installed_hud_win32.cpp":
            "post-install executable verification is compiled into the HUD",
        "src/update/update_worker.cpp":
            "daily and manual update scheduling is compiled into the HUD",
        "src/system_io_display.cpp":
            "tested I/O throughput presentation is compiled into the HUD",
        "src/main.cpp": "the product window is compiled into the HUD",
    }
    for token, reason in hud_source_contracts.items():
        require(hud_sources, token, reason)

    hud_libraries = cmake_call_body(CMAKE, "target_link_libraries", "CodexMonitorHUD")
    require(hud_libraries, "windowsapp",
            "the HUD itself links Windows.Data.Json rather than only its tests")
    require(hud_libraries, "winhttp",
            "the HUD links the documented native HTTP client library")
    require(hud_libraries, "iphlpapi",
            "the HUD links documented native network interface counters")
    require(hud_libraries, "pdh",
            "the HUD links documented physical-disk performance counters")

    cmake_contracts = {
        "add_executable(CodexMonitorHUD WIN32": "the executable uses the Windows GUI subsystem",
        "src/windows_sampler.cpp": "the native sampler is compiled into the HUD",
        "src/system_io_rate.cpp": "portable network and disk rate math is compiled into the HUD",
        "src/system_io_display.cpp":
            "portable network and disk presentation is compiled into the HUD",
        "src/system_io_sampler_win32.cpp": "native network and disk counters are compiled into the HUD",
        "src/ui_layout_math.cpp": "portable uniform resize math is compiled into the HUD",
        "src/performance_worker.cpp": "the HUD builds its serial background worker",
        "src/performance_diagnosis.cpp": "the HUD builds the pure diagnosis model",
        "src/sampling_schedule.cpp": "the request scheduler is shared with fixed tests",
        "src/module_state.cpp": "the module state model is compiled into the HUD",
        "src/settings_store_win32.cpp": "the per-user settings store is compiled into the HUD",
        "add_executable(CodexMonitorSnapshotMathTests": "portable fixed tests are buildable",
        "add_executable(CodexMonitorPerformanceDiagnosisTests":
            "portable diagnosis tests are buildable",
        "add_test(NAME windows_performance_diagnosis":
            "portable diagnosis tests are registered with CTest",
        "add_test(NAME windows_snapshot_math": "portable tests are registered with CTest",
        "add_executable(CodexMonitorUILayoutMathTests":
            "portable uniform resize tests are buildable",
        "add_test(NAME windows_ui_layout_math":
            "portable uniform resize tests are registered with CTest",
        "add_executable(CodexMonitorSystemIoRateTests":
            "portable network and disk rate tests are buildable",
        "add_test(NAME windows_system_io_rate":
            "portable network and disk rate tests are registered with CTest",
        "add_executable(CodexMonitorSystemIoDisplayTests":
            "portable network and disk display tests are buildable",
        "add_test(NAME windows_system_io_display":
            "portable network and disk display samples are registered with CTest",
        "add_executable(CodexMonitorSystemIoSamplerTests":
            "Windows-native network and disk sampling tests are buildable",
        "add_test(NAME windows_system_io_sampler":
            "Windows-native network and disk sampling tests are registered with CTest",
        "add_executable(CodexMonitorModuleStateTests": "portable state tests are buildable",
        "add_test(NAME windows_module_state": "portable state tests are registered with CTest",
        "add_executable(CodexMonitorSamplingScheduleTests":
            "portable sampling scheduling tests are buildable",
        "add_test(NAME windows_sampling_schedule":
            "sampling scheduling tests are registered with CTest",
        "add_executable(CodexMonitorCodexRefreshScheduleTests":
            "Codex request coalescing tests are buildable",
        "add_test(NAME windows_codex_refresh_schedule":
            "Codex request coalescing tests are registered with CTest",
        "add_executable(CodexMonitorCodexUsageMathTests":
            "official daily usage math tests are buildable",
        "add_test(NAME windows_codex_usage_math":
            "official daily usage math tests are registered with CTest",
        "add_executable(CodexMonitorCodexCostSummaryTests":
            "portable Codex cost summary tests are buildable",
        "add_test(NAME windows_codex_cost_summary":
            "portable Codex cost summary tests are registered with CTest",
        "add_executable(CodexMonitorCodexCostHybridTests":
            "official/local cost merge tests are buildable",
        "add_test(NAME windows_codex_cost_hybrid":
            "official/local cost merge tests are registered with CTest",
        "add_executable(CodexMonitorCodexCostEventParserTests":
            "privacy-trimmed rollout parser tests are buildable",
        "add_test(NAME windows_codex_cost_file_scan":
            "bounded local file scan tests are registered with CTest",
        "add_test(NAME windows_codex_cost_history_state":
            "incremental history state tests are registered with CTest",
        "add_test(NAME windows_codex_cost_history_store":
            "restart cache persistence tests are registered with CTest",
        "add_executable(CodexMonitorQuotaForecastTests":
            "portable quota forecast tests are buildable",
        "add_test(NAME windows_quota_forecast":
            "portable quota forecast tests are registered with CTest",
        "add_executable(CodexMonitorQuotaHistoryStoreTests":
            "portable quota history tests are buildable",
        "add_test(NAME windows_quota_history_store":
            "portable quota history tests are registered with CTest",
        "add_executable(CodexMonitorWeeklyQuotaAlertTests":
            "portable weekly quota alert tests are buildable",
        "add_test(NAME windows_weekly_quota_alert":
            "weekly quota alert fixed samples are registered with CTest",
        "add_executable(CodexMonitorWeeklyQuotaAlertStateStoreTests":
            "portable weekly alert state-store tests are buildable",
        "add_test(NAME windows_weekly_quota_alert_state_store":
            "weekly alert persistence tests are registered with CTest",
        "add_executable(CodexMonitorWeeklyQuotaAlertDeliveryTests":
            "product-level weekly alert delivery tests are buildable",
        "add_test(NAME windows_weekly_quota_alert_delivery":
            "weekly alert delivery sequencing is registered with CTest",
        "add_test(NAME windows_github_release_selector":
            "strict release selection tests are registered with CTest",
        "add_test(NAME windows_update_state_store":
            "persistent daily cadence tests are registered with CTest",
        "add_test(NAME windows_github_release_json":
            "GitHub release JSON tests are registered with CTest",
        "add_test(NAME windows_update_check":
            "Windows-only release evaluation tests are registered with CTest",
        "add_test(NAME windows_update_worker":
            "daily and manual update worker tests are registered with CTest",
        "add_test(NAME windows_update_installer_verifier":
            "downloaded MSI checksum verification tests are registered with CTest",
        "add_test(NAME windows_update_asset_download":
            "update asset URL and download policy tests are registered with CTest",
        "add_test(NAME windows_update_msi_identity":
            "publisher and MSI identity policy tests are registered with CTest",
        "add_test(NAME windows_update_apply_transaction":
            "the continuous-lock update transaction is registered with CTest",
        "add_test(NAME windows_update_helper":
            "the controlled helper sequencing tests are registered with CTest",
        "add_test(NAME windows_update_directory_cleanup":
            "the bounded retention policy is registered with CTest",
        "add_test(NAME windows_update_install":
            "the one-click preparation order is registered with CTest",
        "add_test(NAME windows_update_install_worker":
            "the serialized cancellable install worker is registered with CTest",
        "add_executable(CodexMonitorCodexProcessTests":
            "bounded process integration tests are buildable",
        "add_test(NAME windows_codex_process":
            "bounded process integration tests are registered with CTest",
        "add_executable(CodexMonitorCodexJsonTests":
            "privacy parser tests are buildable",
        "add_test(NAME windows_codex_json":
            "privacy parser tests are registered with CTest",
        "add_executable(CodexMonitorCodexAppServerClientTests":
            "official protocol integration tests are buildable",
        "add_test(NAME windows_codex_app_server_client":
            "official protocol integration tests are registered with CTest",
        "add_executable(CodexMonitorCodexWorkerTests":
            "the background Codex worker integration test is buildable",
        "add_test(NAME windows_codex_worker":
            "the background Codex worker integration test is registered with CTest",
        "add_test(NAME windows_codex_worker_quota":
            "quota demand gating has a focused Windows worker test",
        "add_test(NAME windows_codex_worker_cost":
            "cost-history demand gating has a focused Windows worker test",
        "add_executable(CodexMonitorServiceStatusModelTests":
            "portable service-status mapping tests are buildable",
        "add_test(NAME windows_service_status_model":
            "portable service-status mapping tests are registered with CTest",
        "add_executable(CodexMonitorServiceStatusJsonTests":
            "Windows status JSON tests are buildable",
        "add_test(NAME windows_service_status_json":
            "Windows status JSON tests are registered with CTest",
        "add_executable(CodexMonitorServiceStatusWorkerTests":
            "the no-network service worker test is buildable",
        "add_test(NAME windows_service_status_worker":
            "the no-network service worker test is registered with CTest",
        "cxx_std_17": "the implementation has an explicit language baseline",
        "MSVC_RUNTIME_LIBRARY": "the release binary does not require a separate VC runtime install",
        "PSAPI_VERSION=1": "PSAPI names resolve consistently through Psapi.lib",
        "NOMINMAX": "the existing Win32 shell's std::max calls compile under Windows headers",
        "$<$<COMPILE_LANGUAGE:CXX>:/W4>":
            "C++ compiler flags are not forwarded to the resource compiler",
        "windowsapp": "Windows.Data.Json is linked for the official protocol parser",
        "user32 gdi32 comctl32 psapi shell32 ole32 winhttp windowsapp":
            "only documented Windows system libraries are linked",
    }
    for token, reason in cmake_contracts.items():
        require(CMAKE, token, reason)

    require(RESOURCE_SCRIPT, "IDI_CODEX_MONITOR_HUD",
            "the resource script exposes the application icon identifier")
    require(RESOURCE_SCRIPT, "CodexMonitorHUD.ico",
            "the resource script embeds the multi-size application icon")

    print("windows_static_contracts=pass")
    print(f"checked_root={WINDOWS_ROOT}")


if __name__ == "__main__":
    main()
