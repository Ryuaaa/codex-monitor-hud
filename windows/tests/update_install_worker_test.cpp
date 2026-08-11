#include "update/update_install_worker.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace codex_monitor::update {

// The worker test always injects its operation. This definition keeps the
// worker test isolated from networking, Authenticode, and MSI implementation.
WindowsUpdateInstallResult PrepareAndLaunchWindowsUpdate(
    const WindowsUpdateInstallRequest& request,
    const WindowsUpdateInstallCancellationCheck&) noexcept {
    WindowsUpdateInstallResult result;
    result.status = WindowsUpdateInstallStatus::kUnexpected;
    result.targetVersion = request.release.version.canonical;
    return result;
}

}  // namespace codex_monitor::update

namespace {

using namespace std::chrono_literals;
using namespace codex_monitor::update;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

WindowsUpdateInstallRequest Request() {
    WindowsUpdateInstallRequest request;
    request.currentVersion = "1.0.0";
    request.updatesRoot = L"C:\\CodexMonitorUpdates";
    request.release.version.canonical = "1.1.0";
    return request;
}

std::optional<WindowsUpdateInstallResult> WaitForCompletion(
    WindowsUpdateInstallWorker& worker,
    std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (auto completed = worker.TakeLatest()) return completed;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return worker.TakeLatest();
}

void TestSerialCompletionAndFailurePublication() {
    HWND window = CreateWindowExW(
        0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "message-only test window must be created");

    std::atomic<int> calls{0};
    WindowsUpdateInstallWorker worker(
        [&calls](const WindowsUpdateInstallRequest& request, const auto&) {
            ++calls;
            std::this_thread::sleep_for(40ms);
            WindowsUpdateInstallResult result;
            result.status = WindowsUpdateInstallStatus::kHelperStarted;
            result.targetVersion = request.release.version.canonical;
            return result;
        });
    Require(worker.Start(window, WM_APP + 81), "install worker must start");
    Require(worker.Request(Request()), "first install request must queue");
    Require(!worker.Request(Request()),
            "a second install request cannot overlap the first");
    const auto completed = WaitForCompletion(worker);
    Require(completed && completed->helperStarted() && calls == 1 &&
                !worker.IsBusy(),
            "one helper-start completion must be published exactly once");
    worker.StopAndJoin();

    WindowsUpdateInstallWorker failed(
        [](const WindowsUpdateInstallRequest& request, const auto&) {
            WindowsUpdateInstallResult result;
            result.status =
                WindowsUpdateInstallStatus::kChecksumDownloadFailed;
            result.targetVersion = request.release.version.canonical;
            return result;
        });
    Require(failed.Start(window, WM_APP + 82),
            "failure worker must start");
    Require(failed.Request(Request()), "failure request must queue");
    const auto failure = WaitForCompletion(failed);
    Require(failure && !failure->helperStarted() &&
                failure->status ==
                    WindowsUpdateInstallStatus::kChecksumDownloadFailed,
            "ordinary preparation failure is published without helper success");
    failed.StopAndJoin();
    DestroyWindow(window);
}

void TestStopCancelsAndJoinsInFlightWork() {
    HWND window = CreateWindowExW(
        0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "cancellation window must be created");

    std::atomic<bool> entered{false};
    std::atomic<bool> observedCancellation{false};
    WindowsUpdateInstallWorker worker(
        [&entered, &observedCancellation](const auto&, const auto& cancelled) {
            entered = true;
            while (!cancelled()) std::this_thread::sleep_for(2ms);
            observedCancellation = true;
            WindowsUpdateInstallResult result;
            result.status = WindowsUpdateInstallStatus::kCancelled;
            return result;
        });
    Require(worker.Start(window, WM_APP + 83),
            "cancellation worker must start");
    Require(worker.Request(Request()), "cancellation request must queue");
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!entered && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    Require(entered, "in-flight operation must begin");
    worker.StopAndJoin();
    Require(observedCancellation && !worker.TakeLatest().has_value(),
            "shutdown cancellation must be observed and discarded");
    DestroyWindow(window);
}

}  // namespace

int main() {
    TestSerialCompletionAndFailurePublication();
    TestStopCancelsAndJoinsInFlightWork();
    std::cout << "update_install_worker_tests=pass\n";
    return 0;
}
