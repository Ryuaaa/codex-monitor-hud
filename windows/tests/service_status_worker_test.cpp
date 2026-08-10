#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "service_status_worker.h"

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace codex_monitor {

// The worker target deliberately excludes the real WinHTTP transport. Every
// test injects a fake fetcher; this fallback only satisfies the default
// constructor symbol and makes accidental network access impossible.
OpenAIServiceStatusFetchResult FetchOpenAIServiceStatus(
    const ServiceStatusCancellationCheck&) noexcept {
    OpenAIServiceStatusFetchResult result;
    result.failure = OpenAIServiceStatusFailureKind::kNetwork;
    result.error = L"Network transport is disabled in worker tests";
    return result;
}

}  // namespace codex_monitor

namespace {

using codex_monitor::CompletedServiceStatusRefresh;
using codex_monitor::OpenAIServiceHealth;
using codex_monitor::OpenAIServiceStatusFetchResult;
using codex_monitor::OpenAIServiceStatusFailureKind;
using codex_monitor::OpenAIServiceStatusWorker;
using namespace std::chrono_literals;

constexpr UINT kWorkerReadyMessage = WM_APP + 43;
int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::optional<CompletedServiceStatusRefresh> WaitForRefresh(
    HWND window, OpenAIServiceStatusWorker& worker,
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

void DrainRefreshMessages(HWND window) {
    MSG message{};
    while (PeekMessageW(&message, window, kWorkerReadyMessage,
                        kWorkerReadyMessage, PM_REMOVE)) {
    }
}

OpenAIServiceStatusFetchResult SuccessfulResult(std::string headline) {
    OpenAIServiceStatusFetchResult result;
    result.succeeded = true;
    result.failure = OpenAIServiceStatusFailureKind::kNone;
    result.status.health = OpenAIServiceHealth::kOperational;
    result.status.headline = std::move(headline);
    result.status.detail = "Official OpenAI status";
    return result;
}

OpenAIServiceStatusFetchResult FailedResult(
    OpenAIServiceStatusFailureKind failure =
        OpenAIServiceStatusFailureKind::kNetwork) {
    OpenAIServiceStatusFetchResult result;
    result.failure = failure;
    result.error = L"Injected failure";
    return result;
}

void TestActivationRefreshesImmediatelyAndUsesSuccessCadence(HWND window) {
    std::atomic<int> calls{0};
    OpenAIServiceStatusWorker worker(
        [&calls](const codex_monitor::ServiceStatusCancellationCheck&) {
            ++calls;
            return SuccessfulResult("Operational");
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "a valid message-only window must start the worker");
    Expect(worker.ActivateAndRefresh(),
           "activation must queue an immediate refresh");

    const auto result = WaitForRefresh(window, worker);
    Expect(result.has_value(), "activation must publish a refresh promptly");
    if (result) {
        Expect(result->succeeded,
               "the injected successful fetch must be marked successful");
        Expect(result->status && result->status->headline == "Operational",
               "the successful status must reach the UI result");
        Expect(result->lastSuccessfulRefresh.has_value(),
               "success must record a last-success timestamp");
        Expect(!result->showingLastKnown,
               "fresh success must not be marked as last-known data");
        Expect(result->nextRefreshDelay == 900s,
               "success must schedule the fifteen-minute cadence");
    }
    Expect(calls.load() == 1,
           "activation must perform exactly one immediate fetch");
    worker.StopAndJoin();
}

void TestFirstFailureUsesOneMinuteBackoff(HWND window) {
    OpenAIServiceStatusWorker worker(
        [](const codex_monitor::ServiceStatusCancellationCheck&) {
            return FailedResult();
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "the failure worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the failure worker must refresh immediately");

    const auto result = WaitForRefresh(window, worker);
    Expect(result.has_value(), "a failed fetch must still publish UI state");
    if (result) {
        Expect(!result->succeeded,
               "the injected failure must be marked unsuccessful");
        Expect(!result->status,
               "a first failure must not manufacture a service status");
        Expect(!result->showingLastKnown,
               "a first failure has no last-known status to show");
        Expect(result->nextRefreshDelay == 60s,
               "the first failure must retry after one minute");
    }
    worker.StopAndJoin();
}

void TestFailureBackoffCapsAndResetsAfterSuccess(HWND window) {
    std::atomic<int> calls{0};
    OpenAIServiceStatusWorker worker(
        [&calls](const codex_monitor::ServiceStatusCancellationCheck&) {
            const int call = ++calls;
            if (call == 7) return SuccessfulResult("Recovered");
            return FailedResult();
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "the backoff-sequence worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the backoff-sequence worker must refresh immediately");

    constexpr std::array expectedFailureDelays{
        60s, 120s, 300s, 600s, 900s, 900s,
    };
    for (const auto expectedDelay : expectedFailureDelays) {
        const auto failed = WaitForRefresh(window, worker);
        Expect(failed && !failed->succeeded &&
                   failed->nextRefreshDelay == expectedDelay,
               "consecutive failures must follow the capped retry sequence");
        Expect(worker.RequestRefresh(),
               "an explicit refresh must advance the backoff sequence");
    }

    const auto recovered = WaitForRefresh(window, worker);
    Expect(recovered && recovered->succeeded &&
               recovered->nextRefreshDelay == 900s,
           "success after backoff must restore the fifteen-minute cadence");
    Expect(worker.RequestRefresh(),
           "an explicit refresh must test the post-recovery failure");
    const auto failedAfterRecovery = WaitForRefresh(window, worker);
    Expect(failedAfterRecovery && !failedAfterRecovery->succeeded &&
               failedAfterRecovery->nextRefreshDelay == 60s,
           "the first failure after recovery must restart at one minute");
    Expect(calls.load() == 8,
           "the backoff reset scenario must perform exactly eight fetches");
    worker.StopAndJoin();
}

void TestFailureRetainsLastSuccess(HWND window) {
    std::atomic<int> calls{0};
    OpenAIServiceStatusWorker worker(
        [&calls](const codex_monitor::ServiceStatusCancellationCheck&) {
            if (++calls == 1) return SuccessfulResult("Last known normal");
            return FailedResult();
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "the retained-status worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the retained-status worker must refresh immediately");

    const auto first = WaitForRefresh(window, worker);
    Expect(first && first->succeeded && first->status,
           "the setup refresh must retain a successful status");
    Expect(worker.RequestRefresh(),
           "an explicit refresh must bypass the success timer");
    const auto failed = WaitForRefresh(window, worker);
    Expect(failed.has_value(), "the follow-up failure must be published");
    if (first && failed) {
        Expect(!failed->succeeded,
               "the follow-up injected failure must stay unsuccessful");
        Expect(failed->showingLastKnown,
               "failure after success must identify last-known data");
        Expect(failed->status &&
                   failed->status->headline == "Last known normal",
               "failure must preserve the last successful status");
        Expect(failed->lastSuccessfulRefresh == first->lastSuccessfulRefresh,
               "failure must preserve the last successful timestamp");
        Expect(failed->nextRefreshDelay == 60s,
               "the first failure after success must restart backoff at one minute");
    }
    worker.StopAndJoin();
}

void TestResumeReusesFreshResultWithoutExtraFetch(HWND window) {
    std::atomic<int> calls{0};
    OpenAIServiceStatusWorker worker(
        [&calls](const codex_monitor::ServiceStatusCancellationCheck&) {
            ++calls;
            return SuccessfulResult("Still fresh");
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "the freshness worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the freshness worker must fetch its first status");
    const auto first = WaitForRefresh(window, worker);
    Expect(first && first->succeeded,
           "the freshness setup refresh must succeed");

    worker.PauseAndInvalidate();
    DrainRefreshMessages(window);
    Expect(worker.ActivateAndRefresh(),
           "restoring visibility must reactivate the worker");
    std::this_thread::sleep_for(100ms);
    Expect(calls.load() == 1,
           "restoring a fresh result must not bypass the fifteen-minute cadence");
    Expect(!worker.IsBusy(),
           "a fresh retained result must wait without occupying the worker");
    Expect(!worker.TakeLatest(),
           "reactivation within the cadence must not republish fake data");
    worker.StopAndJoin();
}

void TestPauseDiscardsInflightResultAndResumeRefreshes(HWND window) {
    std::atomic<int> calls{0};
    std::atomic<bool> firstFetchEntered{false};
    OpenAIServiceStatusWorker worker(
        [&calls, &firstFetchEntered](
            const codex_monitor::ServiceStatusCancellationCheck& cancelled) {
            const int call = ++calls;
            if (call == 1) {
                firstFetchEntered.store(true, std::memory_order_release);
                while (!cancelled()) std::this_thread::sleep_for(2ms);
                return SuccessfulResult("Stale in-flight result");
            }
            return SuccessfulResult("Fresh after resume");
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "the pause worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the pause worker must queue an immediate refresh");
    Expect(WaitUntil(
               [&] { return firstFetchEntered.load(std::memory_order_acquire); },
               2s),
           "the first fake fetch must enter before pause");

    worker.PauseAndInvalidate();
    Expect(WaitUntil([&] { return !worker.IsBusy(); }, 2s),
           "pause cancellation must let the in-flight fake fetch finish");
    DrainRefreshMessages(window);
    Expect(!worker.TakeLatest(),
           "an in-flight result invalidated by pause must be discarded");

    Expect(worker.ActivateAndRefresh(),
           "resuming must queue a new generation immediately");
    const auto resumed = WaitForRefresh(window, worker);
    Expect(resumed && resumed->succeeded && resumed->status &&
               resumed->status->headline == "Fresh after resume",
           "resume must publish only the fresh generation");
    Expect(calls.load() == 2,
           "pause and resume must produce one discarded and one fresh fetch");
    worker.StopAndJoin();
}

void TestStopAndJoinCompletes(HWND window) {
    std::atomic<bool> fetchEntered{false};
    OpenAIServiceStatusWorker worker(
        [&fetchEntered](
            const codex_monitor::ServiceStatusCancellationCheck& cancelled) {
            fetchEntered.store(true, std::memory_order_release);
            while (!cancelled()) std::this_thread::sleep_for(2ms);
            return FailedResult(OpenAIServiceStatusFailureKind::kCancelled);
        });
    Expect(worker.Start(window, kWorkerReadyMessage),
           "the stop worker must start");
    Expect(worker.ActivateAndRefresh(),
           "the stop worker must enter its fake fetch");
    Expect(WaitUntil([&] { return fetchEntered.load(std::memory_order_acquire); },
                     2s),
           "the cancellable fake fetch must start before stopping");

    const auto started = std::chrono::steady_clock::now();
    worker.StopAndJoin();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Expect(elapsed < 2s,
           "StopAndJoin must cancel and join the worker promptly");
    Expect(!worker.IsBusy(), "a stopped worker must not remain busy");
    Expect(!worker.TakeLatest(), "stopping must leave no publishable result");
}

}  // namespace

int wmain() {
    HWND window = CreateWindowExW(0, L"STATIC", L"ServiceStatusWorkerTest", 0,
                                  0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr,
                                  nullptr);
    if (!window) {
        std::cerr << "FAIL: could not create completion window\n";
        return 1;
    }

    TestActivationRefreshesImmediatelyAndUsesSuccessCadence(window);
    DrainRefreshMessages(window);
    TestFirstFailureUsesOneMinuteBackoff(window);
    DrainRefreshMessages(window);
    TestFailureBackoffCapsAndResetsAfterSuccess(window);
    DrainRefreshMessages(window);
    TestFailureRetainsLastSuccess(window);
    DrainRefreshMessages(window);
    TestResumeReusesFreshResultWithoutExtraFetch(window);
    DrainRefreshMessages(window);
    TestPauseDiscardsInflightResultAndResumeRefreshes(window);
    DrainRefreshMessages(window);
    TestStopAndJoinCompletes(window);

    DestroyWindow(window);
    if (failures != 0) return 1;
    std::cout << "service_status_worker_tests=pass\n";
    return 0;
}
