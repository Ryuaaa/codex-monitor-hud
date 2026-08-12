#include "update/update_worker.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;
using codex_monitor::update::CompletedWindowsUpdateCheck;
using codex_monitor::update::GitHubReleaseAsset;
using codex_monitor::update::ParseSemVerTag;
using codex_monitor::update::SelectedWindowsRelease;
using codex_monitor::update::WindowsUpdateCheckResult;
using codex_monitor::update::WindowsUpdateCheckStatus;
using codex_monitor::update::WindowsUpdateWorker;
using codex_monitor::update::WindowsUpdateWorkerTiming;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        wchar_t root[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, root) > 0,
                "Windows temporary path must be available");
        path_ = std::filesystem::path(root) /
                (L"codex-update-worker-" +
                 std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        Require(!error, "temporary update-worker directory must be created");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

HWND CreateMessageWindow() {
    return CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                           nullptr);
}

std::optional<CompletedWindowsUpdateCheck> WaitForCompletion(
    WindowsUpdateWorker& worker,
    std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (auto result = worker.TakeLatest()) return result;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return worker.TakeLatest();
}

WindowsUpdateCheckResult AvailableResult() {
    WindowsUpdateCheckResult result;
    result.status = WindowsUpdateCheckStatus::kUpdateAvailable;
    const auto version = ParseSemVerTag("1.2.0");
    Require(version.has_value(), "the update worker fixture version must parse");
    result.release = SelectedWindowsRelease{
        *version,
        "windows-v1.2.0",
        GitHubReleaseAsset{
            "CodexMonitorHUD-windows-x64-1.2.0.msi",
            "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/windows-v1.2.0/CodexMonitorHUD-windows-x64-1.2.0.msi"},
        GitHubReleaseAsset{
            "CodexMonitorHUD-windows-x64-1.2.0.msi.sha256",
            "https://github.com/Ryuaaa/codex-monitor-hud/releases/download/windows-v1.2.0/CodexMonitorHUD-windows-x64-1.2.0.msi.sha256"},
    };
    return result;
}

void TestAutomaticPersistenceCacheAndManualBypass() {
    TemporaryDirectory temporary;
    const auto statePath = temporary.path() / L"update-state.ini";
    HWND window = CreateMessageWindow();
    Require(window != nullptr, "the update worker message window must be created");

    std::atomic<int> firstChecks{0};
    WindowsUpdateWorker first(
        [&firstChecks](std::string_view, const auto&) {
            ++firstChecks;
            return AvailableResult();
        },
        WindowsUpdateWorkerTiming{0s, 60s});
    Require(first.Start(window, WM_APP + 70, "1.0.0", statePath),
            "the first update worker must start");
    const auto automatic = WaitForCompletion(first);
    Require(automatic && !automatic->manual && !automatic->fromCache &&
                automatic->availableVersion == "1.2.0" &&
                automatic->result.status ==
                    WindowsUpdateCheckStatus::kUpdateAvailable &&
                firstChecks == 1 && !automatic->stateSaveFailed,
            "the due automatic check must publish and persist its update");
    first.StopAndJoin();

    std::atomic<int> secondChecks{0};
    WindowsUpdateWorker second(
        [&secondChecks](std::string_view, const auto&) {
            ++secondChecks;
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kUpToDate;
            return result;
        },
        WindowsUpdateWorkerTiming{0s, 60s});
    Require(second.Start(window, WM_APP + 71, "1.0.0", statePath),
            "the restarted update worker must start");
    const auto cached = WaitForCompletion(second, 500ms);
    Require(cached && cached->fromCache &&
                cached->availableVersion == "1.2.0" && secondChecks == 0,
            "a restart inside the daily interval must reuse the public cached version without networking");

    Require(second.RequestManualCheck(),
            "a manual check must bypass the daily interval");
    const auto manual = WaitForCompletion(second);
    Require(manual && manual->manual && !manual->fromCache &&
                manual->result.status == WindowsUpdateCheckStatus::kUpToDate &&
                manual->availableVersion.empty() && secondChecks == 1,
            "a manual up-to-date result must clear the retained offer");
    second.StopAndJoin();
    DestroyWindow(window);
}

void TestUpgradeDiscardsStaleCacheAndChecksAgain() {
    TemporaryDirectory temporary;
    const auto statePath = temporary.path() / L"update-state.ini";
    HWND window = CreateMessageWindow();
    Require(window != nullptr, "the upgrade cache window must be created");

    std::atomic<int> firstChecks{0};
    WindowsUpdateWorker first(
        [&firstChecks](std::string_view, const auto&) {
            ++firstChecks;
            return AvailableResult();
        },
        WindowsUpdateWorkerTiming{0s, 24h});
    Require(first.Start(window, WM_APP + 73, "1.0.0", statePath),
            "the pre-upgrade worker must start");
    Require(WaitForCompletion(first).has_value() && firstChecks == 1,
            "the pre-upgrade worker must persist the available version");
    first.StopAndJoin();

    std::atomic<int> upgradedChecks{0};
    WindowsUpdateWorker upgraded(
        [&upgradedChecks](std::string_view, const auto&) {
            ++upgradedChecks;
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kUpToDate;
            return result;
        },
        WindowsUpdateWorkerTiming{0s, 24h});
    Require(upgraded.Start(window, WM_APP + 74, "1.2.0", statePath),
            "the upgraded worker must start");
    const auto refreshed = WaitForCompletion(upgraded);
    Require(refreshed && !refreshed->fromCache &&
                refreshed->availableVersion.empty() && upgradedChecks == 1,
            "an upgraded app must discard its stale offer and check again");
    upgraded.StopAndJoin();
    DestroyWindow(window);
}

void TestBusyCheckRejectsDuplicateManualRequest() {
    TemporaryDirectory temporary;
    HWND window = CreateMessageWindow();
    Require(window != nullptr, "the busy update window must be created");
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    WindowsUpdateWorker worker(
        [&entered, &release](std::string_view, const auto&) {
            entered = true;
            while (!release) std::this_thread::sleep_for(5ms);
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kUpToDate;
            return result;
        },
        WindowsUpdateWorkerTiming{0s, 60s});
    Require(worker.Start(window, WM_APP + 75, "1.0.0",
                         temporary.path() / L"update-state.ini"),
            "the busy update worker must start");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!entered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    Require(entered && worker.IsBusy(),
            "the automatic check must report its busy state");
    Require(!worker.RequestManualCheck(),
            "a busy automatic check must reject a duplicate manual request");
    release = true;
    Require(WaitForCompletion(worker).has_value(),
            "the original automatic check must still complete");
    worker.StopAndJoin();
    DestroyWindow(window);
}

void TestStopCancelsInFlightCheck() {
    TemporaryDirectory temporary;
    HWND window = CreateMessageWindow();
    Require(window != nullptr, "the cancellation message window must be created");
    std::atomic<bool> entered{false};
    std::atomic<bool> observedCancellation{false};
    WindowsUpdateWorker worker(
        [&entered, &observedCancellation](std::string_view,
                                           const auto& cancelled) {
            entered = true;
            while (!cancelled()) std::this_thread::sleep_for(5ms);
            observedCancellation = true;
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kFetchFailed;
            return result;
        },
        WindowsUpdateWorkerTiming{0s, 60s});
    Require(worker.Start(window, WM_APP + 72, "1.0.0",
                         temporary.path() / L"update-state.ini"),
            "the cancellable update worker must start");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!entered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    Require(entered, "the automatic check must enter its fetcher");
    worker.StopAndJoin();
    Require(observedCancellation && !worker.TakeLatest(),
            "stopping must cancel and suppress the in-flight result");
    DestroyWindow(window);
}

void TestRequestStopDoesNotWaitForNetworkWork() {
    TemporaryDirectory temporary;
    HWND window = CreateMessageWindow();
    Require(window != nullptr, "the nonblocking stop window must be created");
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    WindowsUpdateWorker worker(
        [&entered, &release](std::string_view, const auto&) {
            entered = true;
            while (!release) std::this_thread::sleep_for(5ms);
            WindowsUpdateCheckResult result;
            result.status = WindowsUpdateCheckStatus::kFetchFailed;
            return result;
        },
        WindowsUpdateWorkerTiming{0s, 60s});
    Require(worker.Start(window, WM_APP + 76, "1.0.0",
                         temporary.path() / L"update-state.ini"),
            "the nonblocking stop worker must start");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!entered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    Require(entered, "the deliberately blocked checker must start");
    const auto before = std::chrono::steady_clock::now();
    worker.RequestStop();
    const auto elapsed = std::chrono::steady_clock::now() - before;
    Require(elapsed < 50ms,
            "requesting shutdown must not wait for synchronous network work");
    release = true;
    worker.StopAndJoin();
    Require(!worker.TakeLatest(),
            "a result completed after shutdown must not reach the UI");
    DestroyWindow(window);
}

}  // namespace

int main() {
    TestAutomaticPersistenceCacheAndManualBypass();
    TestUpgradeDiscardsStaleCacheAndChecksAgain();
    TestBusyCheckRejectsDuplicateManualRequest();
    TestStopCancelsInFlightCheck();
    TestRequestStopDoesNotWaitForNetworkWork();
    std::cout << "update_worker_tests=pass\n";
    return 0;
}
