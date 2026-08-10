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
CODEX_WORKER_TEST = (
    WINDOWS_ROOT / "tests" / "codex_worker_test.cpp"
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
        "Page nativePage": "each module declares its own native page",
        "requiresPerformanceSampling": "sampling demand is declared per module",
        "requiresCodexData": "Codex demand is independent from performance demand",
        "nativePageVisible": "own-page visibility is independent from Home visibility",
        "kCodexFiveHourQuota": "the five-hour quota has its own module identity",
        "kCodexWeeklyQuota": "the weekly quota has its own module identity",
        "kSystemDiagnosis": "the system diagnosis has its own module identity",
        "SanitizeHomeOrder": "saved order is repaired against the current registry",
        "VisibleHomeModules": "homepage visibility is derived in the portable model",
        "VisibleModulesForNativePage": "native pages are filtered by registry metadata",
        "HomeNeedsCodexData": "homepage Codex demand follows visible modules",
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
        "TestVersionThreeRoundTripAndIndependentQuotaSwitches":
            "the two quota switches and version 3 schema have a round-trip test",
        "TestVersionOneMigrationPreservesOldHomeChoices":
            "version 1 settings migrate without enabling new Home work",
        '"version=3\\n"': "the current persisted settings schema is version 3",
        '"version=1\\n"': "the previous settings schema has an explicit migration fixture",
    }
    for token, reason in state_test_contracts.items():
        require(STATE_TEST, token, reason)

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
        "AllowSameVersionUpgrades": "development builds can replace an earlier same-version MSI",
        'Id="CodexMonitorExecutable"': "the tested HUD executable is the MSI key file",
        'Id="LicenseFile"': "the installed application carries its license",
        'Id="WindowsReadme"': "the installed application carries Windows instructions",
        'Id="ApplicationStartMenuShortcut"': "the MSI creates a Start menu entry",
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
    }
    app_server_sources = CODEX_APP_SERVER_HEADER + CODEX_APP_SERVER
    for token, reason in app_server_contracts.items():
        require(app_server_sources, token, reason)
    require(CODEX_APP_SERVER_TEST, "TestResponseLineLimitStopsFlood",
            "the response-line cap is covered by an integration test")
    require(CODEX_APP_SERVER_TEST, "TestCancellationStopsTheJobWithoutMethodFailures",
            "process cancellation is covered by an integration test")

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
    for forbidden in ("raw", "stderr", "path"):
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
    }.items():
        require(CODEX_WORKER_TEST, token, reason)

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

    main_codex_contracts = {
        "CurrentPageNeedsCodexData": "Codex work follows visible module demand",
        "HomeNeedsCodexData": "Home can request Codex independently from performance data",
        "NativePageNeedsCodexData": "the Codex native page follows its own switches",
        "UpdateCodexDemand": "all demand transitions share one start/stop gate",
        "codexWorker.PauseAndInvalidate": "minimize and hidden modules stop Codex work",
        "codexWorker.ActivateAndRefresh": "new demand refreshes immediately",
        "codexWorker.Start": "the app-server worker starts with the window",
        "kCodexReadyMessage": "worker completion has a dedicated UI message",
        "codexWorker.TakeLatest": "the UI takes a copied privacy-trimmed result",
        "codexWorker.StopAndJoin": "window teardown reaps the Codex worker",
        "ApplyCodexRefresh": "real returned data is applied to cards",
        "BuildQuotaCardText": "quota cards use parsed rate-limit data",
        "BuildSubscriptionCardText": "subscription cards use parsed account data",
        "CalculateUsageCalendarTotals": "Token cards use official daily totals",
        "BuildRecentTasksCardText": "recent-task cards use official task history",
        "app-server 进程范围": "task status is visibly scoped to this app-server process",
        "WM_VSCROLL": "overflowing cards support scrollbar navigation",
        "WM_MOUSEWHEEL": "overflowing cards support wheel navigation",
        "UpdateContentScrollBar": "scroll range follows visible card rows",
        "homeVisibleCheck": "settings expose the Home visibility checkbox",
        "nativeVisibleCheck": "settings expose the own-page visibility checkbox",
        "kSettingsVisibleBaseId": "Home checkbox commands are independently routed",
        "kSettingsNativeVisibleBaseId": "own-page checkbox commands are independently routed",
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
    if MAIN.count("UpdateCodexDemand(") < 5:
        raise AssertionError(
            "Codex demand must be reconsidered on startup, page/settings changes, and minimize/restore"
        )

    hud_sources = cmake_call_body(CMAKE, "add_executable", "CodexMonitorHUD")
    hud_source_contracts = {
        "resources/CodexMonitorHUD.rc": "the application icon resource is compiled into the HUD",
        "src/codex/codex_app_server_client.cpp":
            "the official protocol client is compiled into the HUD",
        "src/codex/codex_executable.cpp": "safe codex.exe discovery is compiled into the HUD",
        "src/codex/codex_json_win32.cpp": "the privacy-trimmed parser is compiled into the HUD",
        "src/codex/codex_process.cpp": "the bounded child process transport is compiled into the HUD",
        "src/codex/codex_refresh_schedule.cpp": "Codex request coalescing is compiled into the HUD",
        "src/codex/codex_usage_math.cpp": "official daily usage math is compiled into the HUD",
        "src/codex/codex_worker.cpp": "the serial Codex worker is compiled into the HUD",
        "src/main.cpp": "the product window is compiled into the HUD",
    }
    for token, reason in hud_source_contracts.items():
        require(hud_sources, token, reason)

    hud_libraries = cmake_call_body(CMAKE, "target_link_libraries", "CodexMonitorHUD")
    require(hud_libraries, "windowsapp",
            "the HUD itself links Windows.Data.Json rather than only its tests")

    cmake_contracts = {
        "add_executable(CodexMonitorHUD WIN32": "the executable uses the Windows GUI subsystem",
        "src/windows_sampler.cpp": "the native sampler is compiled into the HUD",
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
        "cxx_std_17": "the implementation has an explicit language baseline",
        "MSVC_RUNTIME_LIBRARY": "the release binary does not require a separate VC runtime install",
        "PSAPI_VERSION=1": "PSAPI names resolve consistently through Psapi.lib",
        "NOMINMAX": "the existing Win32 shell's std::max calls compile under Windows headers",
        "$<$<COMPILE_LANGUAGE:CXX>:/W4>":
            "C++ compiler flags are not forwarded to the resource compiler",
        "windowsapp": "Windows.Data.Json is linked for the official protocol parser",
        "user32 gdi32 psapi shell32 ole32": "only documented Windows system libraries are linked",
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
