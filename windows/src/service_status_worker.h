#pragma once

#include <windows.h>

#include "service_status_fetch_win32.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace codex_monitor {

struct CompletedServiceStatusRefresh {
    std::optional<OpenAIServiceStatusModel> status;
    std::optional<std::chrono::system_clock::time_point> lastSuccessfulRefresh;
    bool succeeded = false;
    bool showingLastKnown = false;
    std::chrono::seconds nextRefreshDelay{60};
};

using OpenAIServiceStatusFetcher = std::function<
    OpenAIServiceStatusFetchResult(const ServiceStatusCancellationCheck&)>;

class OpenAIServiceStatusWorker {
public:
    explicit OpenAIServiceStatusWorker(
        OpenAIServiceStatusFetcher fetcher = FetchOpenAIServiceStatus);
    ~OpenAIServiceStatusWorker();

    OpenAIServiceStatusWorker(const OpenAIServiceStatusWorker&) = delete;
    OpenAIServiceStatusWorker& operator=(
        const OpenAIServiceStatusWorker&) = delete;

    bool Start(HWND completionWindow, UINT completionMessage);
    bool ActivateAndRefresh();
    bool RequestRefresh();
    void PauseAndInvalidate();
    [[nodiscard]] std::optional<CompletedServiceStatusRefresh> TakeLatest();
    [[nodiscard]] bool IsBusy() const;
    void StopAndJoin();

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    OpenAIServiceStatusFetcher fetcher_;
    std::optional<OpenAIServiceStatusModel> retainedStatus_;
    std::optional<std::chrono::system_clock::time_point> lastSuccessfulRefresh_;
    std::optional<CompletedServiceStatusRefresh> latest_;
    std::optional<std::chrono::steady_clock::time_point> nextAutomaticRefresh_;
    std::optional<std::chrono::steady_clock::time_point> resumeNotBefore_;
    std::atomic<std::uint64_t> cancellationEpoch_{1};
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    bool started_ = false;
    bool stopRequested_ = false;
    bool active_ = false;
    bool pending_ = false;
    bool busy_ = false;
    std::thread thread_;
};

}  // namespace codex_monitor
