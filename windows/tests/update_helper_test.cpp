#include "update/update_helper_win32.h"
#include "update/update_helper_launcher_win32.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using codex_monitor::update::PublisherCertificateSha256;
using codex_monitor::update::LaunchPreparedWindowsUpdateHelper;
using codex_monitor::update::RunWindowsUpdateHelper;
using codex_monitor::update::RunWindowsUpdateHelperSequenceForTesting;
using codex_monitor::update::ValidateWindowsHudExecutableIdentity;
using codex_monitor::update::VerifyAndLaunchWindowsUpdateHelperCopy;
using codex_monitor::update::WindowsHudExecutableIdentity;
using codex_monitor::update::WindowsHudExecutableIdentityStatus;
using codex_monitor::update::WindowsInstalledHudLaunchResult;
using codex_monitor::update::WindowsInstalledHudLaunchStatus;
using codex_monitor::update::WindowsUpdateApplyResult;
using codex_monitor::update::WindowsUpdateApplyStatus;
using codex_monitor::update::WindowsUpdateHelperRequest;
using codex_monitor::update::WindowsUpdateHelperChildRequest;
using codex_monitor::update::WindowsUpdateHelperLauncherRequest;
using codex_monitor::update::WindowsUpdateHelperLauncherStatus;
using codex_monitor::update::WindowsUpdateHelperStatus;
using codex_monitor::update::WindowsUpdateHelperWaitStatus;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

WindowsUpdateApplyResult Installed() {
    WindowsUpdateApplyResult result;
    result.status = WindowsUpdateApplyStatus::kInstalled;
    result.installAttempted = true;
    result.installerExitCode = 0;
    return result;
}

WindowsUpdateApplyResult InstallRejected() {
    WindowsUpdateApplyResult result;
    result.status = WindowsUpdateApplyStatus::kChecksumRejected;
    return result;
}

WindowsInstalledHudLaunchResult Launched() {
    WindowsInstalledHudLaunchResult result;
    result.status = WindowsInstalledHudLaunchStatus::kStarted;
    return result;
}

WindowsInstalledHudLaunchResult LaunchFailure(
    WindowsInstalledHudLaunchStatus status) {
    WindowsInstalledHudLaunchResult result;
    result.status = status;
    return result;
}

void TestExecutableIdentityPolicy() {
    const WindowsHudExecutableIdentity valid{
        L"Codex Monitor HUD", L"CodexMonitorHUD.exe", L"1.2.3",
        L"1.2.3", 1U, 2U, 3U};
    Require(ValidateWindowsHudExecutableIdentity(valid, "1.2.3") ==
                WindowsHudExecutableIdentityStatus::kValid,
            "the signed HUD identity should accept the exact version");
    Require(ValidateWindowsHudExecutableIdentity(valid, "01.2.3") ==
                WindowsHudExecutableIdentityStatus::
                    kInvalidExpectedVersion,
            "non-canonical expected versions must fail closed");

    auto changed = valid;
    changed.productName = L"Another product";
    Require(ValidateWindowsHudExecutableIdentity(changed, "1.2.3") ==
                WindowsHudExecutableIdentityStatus::kProductNameMismatch,
            "a same-version executable from another product must reject");
    changed = valid;
    changed.originalFilename = L"helper.exe";
    Require(ValidateWindowsHudExecutableIdentity(changed, "1.2.3") ==
                WindowsHudExecutableIdentityStatus::
                    kOriginalFilenameMismatch,
            "the signed original filename is part of product identity");
    changed = valid;
    changed.fileVersion = L"1.2.4";
    Require(ValidateWindowsHudExecutableIdentity(changed, "1.2.3") ==
                WindowsHudExecutableIdentityStatus::kFileVersionMismatch,
            "the signed file version must match the installed release");
    changed = valid;
    changed.productVersion = L"1.2.4";
    Require(ValidateWindowsHudExecutableIdentity(changed, "1.2.3") ==
                WindowsHudExecutableIdentityStatus::kProductVersionMismatch,
            "the signed product version must match the installed release");
    changed = valid;
    changed.patchVersion = 4U;
    Require(ValidateWindowsHudExecutableIdentity(changed, "1.2.3") ==
                WindowsHudExecutableIdentityStatus::kFixedVersionMismatch,
            "the fixed PE version must match the signed version strings");
}

void TestSuccessfulSequenceIsStrictlyOrdered() {
    std::vector<int> order;
    int recoveryCalls = 0;
    const auto result = RunWindowsUpdateHelperSequenceForTesting(
        [&] {
            order.push_back(1);
            return WindowsUpdateHelperWaitStatus::kExited;
        },
        [&] {
            order.push_back(2);
            return Installed();
        },
        [&] {
            order.push_back(3);
            return Launched();
        },
        [&] {
            ++recoveryCalls;
            return Launched();
        });
    Require(result.completedAndRestarted() &&
                order == std::vector<int>({1, 2, 3}) &&
                recoveryCalls == 0,
            "wait, verified install, and verified restart must be ordered");
}

void TestWaitFailuresNeverReachInstaller() {
    const WindowsUpdateHelperWaitStatus failures[] = {
        WindowsUpdateHelperWaitStatus::kInvalidHandle,
        WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch,
        WindowsUpdateHelperWaitStatus::kTimedOut,
        WindowsUpdateHelperWaitStatus::kWaitFailed,
        WindowsUpdateHelperWaitStatus::kUnsupportedPlatform,
    };
    for (const WindowsUpdateHelperWaitStatus failure : failures) {
        int installCalls = 0;
        int launchCalls = 0;
        int recoveryCalls = 0;
        const auto result = RunWindowsUpdateHelperSequenceForTesting(
            [failure] { return failure; },
            [&] {
                ++installCalls;
                return Installed();
            },
            [&] {
                ++launchCalls;
                return Launched();
            },
            [&] {
                ++recoveryCalls;
                return Launched();
            });
        Require(!result.completedAndRestarted() && installCalls == 0 &&
                    launchCalls == 0 && recoveryCalls == 0,
                "an unproven old-process exit must stop before installation");
    }
}

void TestInstallFailureRestartsOnlyVerifiedPreviousVersion() {
    int newVersionLaunchCalls = 0;
    int previousVersionLaunchCalls = 0;
    const auto result = RunWindowsUpdateHelperSequenceForTesting(
        [] { return WindowsUpdateHelperWaitStatus::kExited; },
        [] { return InstallRejected(); },
        [&] {
            ++newVersionLaunchCalls;
            return Launched();
        },
        [&] {
            ++previousVersionLaunchCalls;
            return Launched();
        });
    Require(result.status ==
                WindowsUpdateHelperStatus::
                    kInstallFailedAndPreviousVersionRestarted &&
                newVersionLaunchCalls == 0 &&
                previousVersionLaunchCalls == 1,
            "a failed MSI should restart only the verified previous version");

    const auto recoveryRejected =
        RunWindowsUpdateHelperSequenceForTesting(
            [] { return WindowsUpdateHelperWaitStatus::kExited; },
            [] { return InstallRejected(); },
            [] { return Launched(); },
            [] {
                return LaunchFailure(
                    WindowsInstalledHudLaunchStatus::
                        kSignatureVerificationFailed);
            });
    Require(recoveryRejected.status ==
                WindowsUpdateHelperStatus::kInstallRejectedOrFailed,
            "an unverified previous executable must remain stopped");
}

void TestUnverifiedExecutableIsNeverReportedAsRestarted() {
    const auto result = RunWindowsUpdateHelperSequenceForTesting(
        [] { return WindowsUpdateHelperWaitStatus::kExited; },
        [] { return Installed(); },
        [] {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::
                    kSignatureVerificationFailed);
        },
        [] {
            return Launched();
        });
    Require(result.status ==
                WindowsUpdateHelperStatus::kInstalledExecutableRejected &&
                !result.completedAndRestarted(),
            "an unverified installed executable must not be treated as started");
}

void TestVerifiedProcessStartFailureIsDistinct() {
    const auto result = RunWindowsUpdateHelperSequenceForTesting(
        [] { return WindowsUpdateHelperWaitStatus::kExited; },
        [] { return Installed(); },
        [] {
            return LaunchFailure(
                WindowsInstalledHudLaunchStatus::kProcessStartFailed);
        },
        [] {
            return Launched();
        });
    Require(result.status == WindowsUpdateHelperStatus::kRestartFailed,
            "a verified executable start failure should remain diagnosable");
}

void TestMissingOrThrowingOperationsFailClosed() {
    int laterCalls = 0;
    const auto missing = RunWindowsUpdateHelperSequenceForTesting(
        {}, [] { return Installed(); }, [] { return Launched(); },
        [] { return Launched(); });
    Require(missing.status == WindowsUpdateHelperStatus::kInvalidInput,
            "a missing helper operation must fail closed");

    const auto waitThrows = RunWindowsUpdateHelperSequenceForTesting(
        []() -> WindowsUpdateHelperWaitStatus {
            throw std::runtime_error("wait");
        },
        [&] {
            ++laterCalls;
            return Installed();
        },
        [&] {
            ++laterCalls;
            return Launched();
        },
        [&] {
            ++laterCalls;
            return Launched();
        });
    Require(waitThrows.status == WindowsUpdateHelperStatus::kUnexpected &&
                laterCalls == 0,
            "an unexpected wait failure must not continue the transaction");

    const auto installThrows = RunWindowsUpdateHelperSequenceForTesting(
        [] { return WindowsUpdateHelperWaitStatus::kExited; },
        []() -> WindowsUpdateApplyResult {
            throw std::runtime_error("install");
        },
        [&] {
            ++laterCalls;
            return Launched();
        },
        [&] {
            ++laterCalls;
            return Launched();
        });
    Require(installThrows.status == WindowsUpdateHelperStatus::kUnexpected &&
                laterCalls == 0,
            "an unexpected install failure must not launch anything");
}

#ifdef _WIN32

std::uint64_t FileTimeTicks(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) |
           static_cast<std::uint64_t>(value.dwLowDateTime);
}

struct ChildProcess {
    HANDLE process = nullptr;
    DWORD processId = 0;
    std::uint64_t creationTime = 0;
    std::filesystem::path executablePath;
};

ChildProcess StartWaitChild(unsigned int milliseconds) {
    std::vector<wchar_t> executable(32768U, L'\0');
    const DWORD written = GetModuleFileNameW(
        nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Require(written > 0U && written < executable.size(),
            "the helper test executable path must be available");
    const std::filesystem::path executablePath(
        std::wstring(executable.data(), written));

    std::wstring commandLine = L"\"";
    commandLine.append(executablePath.native());
    commandLine.append(L"\" --wait-child ");
    commandLine.append(std::to_wstring(milliseconds));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(
                executablePath.c_str(), commandLine.data(), nullptr, nullptr,
                FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                &process) != FALSE,
            "the native old-HUD wait fixture must start");
    CloseHandle(process.hThread);

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    Require(GetProcessTimes(process.hProcess, &creation, &exit, &kernel,
                            &user) != FALSE,
            "the child creation time must be readable");
    return {process.hProcess, process.dwProcessId,
            FileTimeTicks(creation), executablePath};
}

void TestNativeProcessWaitUsesExactIdentity() {
    using codex_monitor::update::
        WaitForOldWindowsHudProcessForTesting;

    ChildProcess exits = StartWaitChild(50U);
    const auto exited = WaitForOldWindowsHudProcessForTesting(
        reinterpret_cast<std::uintptr_t>(exits.process), exits.processId,
        exits.creationTime, exits.executablePath, std::chrono::seconds(5));
    Require(exited == WindowsUpdateHelperWaitStatus::kExited,
            "the exact old process handle should authorize only after exit");

    ChildProcess mismatch = StartWaitChild(100U);
    const auto rejected = WaitForOldWindowsHudProcessForTesting(
        reinterpret_cast<std::uintptr_t>(mismatch.process),
        mismatch.processId, mismatch.creationTime + 1U,
        mismatch.executablePath,
        std::chrono::seconds(5));
    Require(rejected ==
                WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch,
            "a creation-time mismatch must reject a reused or wrong process");

    ChildProcess wrongImage = StartWaitChild(100U);
    const auto imageRejected = WaitForOldWindowsHudProcessForTesting(
        reinterpret_cast<std::uintptr_t>(wrongImage.process),
        wrongImage.processId, wrongImage.creationTime,
        wrongImage.executablePath.parent_path() / L"NotTheHud.exe",
        std::chrono::seconds(5));
    Require(imageRejected ==
                WindowsUpdateHelperWaitStatus::kProcessIdentityMismatch,
            "an inherited handle for another executable must not authorize installation");

    ChildProcess slow = StartWaitChild(200U);
    const auto timedOut = WaitForOldWindowsHudProcessForTesting(
        reinterpret_cast<std::uintptr_t>(slow.process), slow.processId,
        slow.creationTime, slow.executablePath,
        std::chrono::milliseconds(5));
    Require(timedOut == WindowsUpdateHelperWaitStatus::kTimedOut,
            "a live old process must stop the update at the bounded timeout");
    Sleep(250U);
}

#endif

void TestProductionBoundaryIsUnsupportedOffWindows() {
#ifndef _WIN32
    WindowsUpdateHelperRequest request;
    request.inheritedOldProcessHandle = 1U;
    request.expectedOldProcessId = 1U;
    request.expectedOldProcessCreationTime = 1U;
    request.installerPath = "/tmp/CodexMonitorHUD-windows-x64-1.2.3.msi";
    request.expectedInstallerFileName =
        "CodexMonitorHUD-windows-x64-1.2.3.msi";
    request.sha256Manifest = std::string(64U, '0') + "  " +
        request.expectedInstallerFileName + "\n";
    request.expectedVersion = "1.2.3";
    request.previousVersion = "1.2.2";
    request.trustedPublisherFingerprint = PublisherCertificateSha256{};
    request.installedExecutablePath = "/tmp/CodexMonitorHUD.exe";
    const auto result = RunWindowsUpdateHelper(request);
    Require(result.status == WindowsUpdateHelperStatus::kUnsupportedPlatform,
            "the production helper cannot install on a non-Windows host");

    WindowsUpdateHelperChildRequest child;
    child.inheritedOldProcessHandle = 1U;
    child.expectedOldProcessId = 1U;
    child.expectedOldProcessCreationTime = 1U;
    child.installerPath = request.installerPath;
    child.installerSha256 = std::string(64U, '0');
    child.targetVersion = "1.2.3";
    child.previousVersion = "1.2.2";
    const auto launch = VerifyAndLaunchWindowsUpdateHelperCopy(
        "/tmp/CodexMonitorHUD.exe", "1.2.2",
        PublisherCertificateSha256{}, child);
    Require(launch.status ==
                WindowsInstalledHudLaunchStatus::kUnsupportedPlatform,
            "the verified helper launcher cannot start off Windows");

    WindowsUpdateHelperLauncherRequest prepared;
    prepared.privateUpdateDirectory = "/tmp/update";
    prepared.installerPath =
        "/tmp/update/CodexMonitorHUD-windows-x64-1.2.3.msi";
    prepared.installerSha256 = std::string(64U, '0');
    prepared.currentVersion = "1.2.2";
    prepared.targetVersion = "1.2.3";
    const auto preparedLaunch =
        LaunchPreparedWindowsUpdateHelper(prepared);
    Require(preparedLaunch.status ==
                WindowsUpdateHelperLauncherStatus::kUnsupportedPlatform,
            "a prepared update cannot launch a helper off Windows");

    prepared.installerPath =
        "/tmp/other/CodexMonitorHUD-windows-x64-1.2.3.msi";
    const auto mismatchedDirectory =
        LaunchPreparedWindowsUpdateHelper(prepared);
    Require(mismatchedDirectory.status ==
                WindowsUpdateHelperLauncherStatus::kInvalidInput,
            "the MSI must remain inside the prepared private directory");
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    if (argc == 3 && std::string(argv[1]) == "--wait-child") {
        const unsigned long milliseconds = std::strtoul(argv[2], nullptr, 10);
        Sleep(static_cast<DWORD>(milliseconds));
        return 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    TestExecutableIdentityPolicy();
    TestSuccessfulSequenceIsStrictlyOrdered();
    TestWaitFailuresNeverReachInstaller();
    TestInstallFailureRestartsOnlyVerifiedPreviousVersion();
    TestUnverifiedExecutableIsNeverReportedAsRestarted();
    TestVerifiedProcessStartFailureIsDistinct();
    TestMissingOrThrowingOperationsFailClosed();
#ifdef _WIN32
    TestNativeProcessWaitUsesExactIdentity();
#endif
    TestProductionBoundaryIsUnsupportedOffWindows();
    std::cout << "update_helper_tests=pass\n";
    return 0;
}
