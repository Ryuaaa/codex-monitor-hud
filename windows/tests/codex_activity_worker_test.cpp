#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "codex/codex_activity_worker.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>

namespace {

using codex_monitor::codex::CodexActivityScanRequest;
using codex_monitor::codex::CodexActivityScanResult;
using codex_monitor::codex::CodexActivityScanStatus;
using codex_monitor::codex::CodexActivityWorker;
using codex_monitor::codex::CompletedCodexActivityRefresh;
using namespace std::chrono_literals;

constexpr UINT kWorkerReadyMessage = WM_APP + 47;
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::optional<CompletedCodexActivityRefresh> WaitForRefresh(
    HWND window,
    CodexActivityWorker& worker,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        if (PeekMessageW(&message, window, kWorkerReadyMessage,
                         kWorkerReadyMessage, PM_REMOVE)) {
            return worker.TakeLatest();
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

void DrainMessages(HWND window) {
    MSG message{};
    while (PeekMessageW(&message, window, kWorkerReadyMessage,
                        kWorkerReadyMessage, PM_REMOVE)) {
    }
}

CodexActivityScanResult Available(std::size_t activeCount) {
    CodexActivityScanResult result;
    result.status = CodexActivityScanStatus::kAvailable;
    result.activeTaskCount = activeCount;
    result.longestActiveTaskSeconds = activeCount > 0 ? 42 : 0;
    result.readableRecentFileCount = activeCount > 0 ? activeCount : 0;
    return result;
}

CodexActivityScanResult Unavailable() {
    CodexActivityScanResult result;
    result.status = CodexActivityScanStatus::kRecentFilesUnresolved;
    result.unresolvedRecentFileCount = 1;
    return result;
}

void TestCadenceAndRepeatedRequestCoalescing(HWND window) {
    std::atomic<int> calls{0};
    std::atomic<bool> releaseFirst{false};
    std::atomic<bool> releaseSecond{false};
    CodexActivityWorker worker(
        [&](const CodexActivityScanRequest& request) {
            const int call = ++calls;
            if (call == 1) {
                while (!releaseFirst.load(std::memory_order_acquire) &&
                       !(request.shouldCancel && request.shouldCancel())) {
                    std::this_thread::sleep_for(2ms);
                }
                return Available(2);
            }
            if (call == 2) {
                while (!releaseSecond.load(std::memory_order_acquire) &&
                       !(request.shouldCancel && request.shouldCancel())) {
                    std::this_thread::sleep_for(2ms);
                }
                return Available(0);
            }
            return Unavailable();
        });
    Expect(worker.Start(window, kWorkerReadyMessage,
                        L"C:\\Users\\test\\.codex\\sessions"),
           "the activity worker must start");
    Expect(worker.ActivateAndRefresh(),
           "activation must queue an immediate scan");
    Expect(WaitUntil([&] { return calls.load() == 1; }),
           "the first scan must enter");
    Expect(worker.RequestRefresh(),
           "one request while busy must queue a follow-up");
    Expect(!worker.RequestRefresh(),
           "repeated requests while one is queued must be coalesced");
    releaseFirst.store(true, std::memory_order_release);

    const auto active = WaitForRefresh(window, worker);
    Expect(active && active->activity.activeTaskCount == 2 &&
               active->nextRefreshDelay == 5s,
           "an active result must use the five-second cadence");
    releaseSecond.store(true, std::memory_order_release);
    const auto idle = WaitForRefresh(window, worker);
    Expect(idle && idle->activity.available() &&
               idle->activity.activeTaskCount == 0 &&
               idle->nextRefreshDelay == 20s,
           "the coalesced idle follow-up must use the twenty-second cadence");
    Expect(calls.load() == 2,
           "repeated busy requests must produce only one follow-up scan");

    Expect(worker.RequestRefresh(),
           "an explicit request must exercise unavailable cadence");
    const auto unavailable = WaitForRefresh(window, worker);
    Expect(unavailable && !unavailable->activity.available() &&
               unavailable->nextRefreshDelay == 20s,
           "an unavailable result must use the twenty-second cadence");
    worker.StopAndJoin();
}

void TestPauseDiscardsInflightResult(HWND window) {
    std::atomic<int> calls{0};
    std::atomic<bool> firstEntered{false};
    CodexActivityWorker worker(
        [&](const CodexActivityScanRequest& request) {
            const int call = ++calls;
            if (call == 1) {
                firstEntered.store(true, std::memory_order_release);
                while (!(request.shouldCancel && request.shouldCancel())) {
                    std::this_thread::sleep_for(2ms);
                }
                return Available(99);
            }
            return Available(1);
        });
    Expect(worker.Start(window, kWorkerReadyMessage,
                        L"C:\\Users\\test\\.codex\\sessions"),
           "the pause activity worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the pause test must start a scan");
    Expect(WaitUntil(
               [&] { return firstEntered.load(std::memory_order_acquire); }),
           "the cancellable scan must enter before pause");
    worker.PauseAndInvalidate();
    Expect(WaitUntil([&] { return !worker.IsBusy(); }),
           "pause must cancel the in-flight scan promptly");
    DrainMessages(window);
    Expect(!worker.TakeLatest(),
           "a hidden or paused generation must not write back stale activity");

    Expect(worker.ActivateAndRefresh(),
           "restoring visibility must start a fresh generation");
    const auto resumed = WaitForRefresh(window, worker);
    Expect(resumed && resumed->activity.activeTaskCount == 1,
           "resume must publish only the fresh result");
    Expect(calls.load() == 2,
           "pause and resume must run one discarded and one fresh scan");
    worker.StopAndJoin();
}

void TestStopAndJoinLeavesNoThreadOrResult(HWND window) {
    std::atomic<bool> entered{false};
    CodexActivityWorker worker(
        [&](const CodexActivityScanRequest& request) {
            entered.store(true, std::memory_order_release);
            while (!(request.shouldCancel && request.shouldCancel())) {
                std::this_thread::sleep_for(2ms);
            }
            return Available(4);
        });
    Expect(worker.Start(window, kWorkerReadyMessage,
                        L"C:\\Users\\test\\.codex\\sessions"),
           "the stop activity worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the stop test must start a scan");
    Expect(WaitUntil([&] { return entered.load(std::memory_order_acquire); }),
           "the stop test scan must enter");
    const auto start = std::chrono::steady_clock::now();
    worker.StopAndJoin();
    Expect(std::chrono::steady_clock::now() - start < 2s,
           "StopAndJoin must cancel and join promptly");
    Expect(!worker.IsBusy() && !worker.TakeLatest(),
           "a stopped activity worker must leave no thread-visible work or result");
}

}  // namespace

int wmain() {
    HWND window = CreateWindowExW(0, L"STATIC", L"ActivityWorkerTest", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr,
                                  nullptr);
    if (!window) {
        std::cerr << "FAIL: could not create completion window\n";
        return 1;
    }
    TestCadenceAndRepeatedRequestCoalescing(window);
    DrainMessages(window);
    TestPauseDiscardsInflightResult(window);
    DrainMessages(window);
    TestStopAndJoinLeavesNoThreadOrResult(window);
    DestroyWindow(window);
    if (failures != 0) return 1;
    std::cout << "codex_activity_worker_tests=pass\n";
    return 0;
}
