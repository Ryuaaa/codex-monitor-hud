#pragma once

#include <windows.h>

#include "codex_app_server_client.h"
#include "codex_cost_summary.h"
#include "quota_forecast.h"
#include "codex_refresh_schedule.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace codex_monitor::codex {

struct QuotaWindowForecastRefresh {
    bool windowReturned = false;
    QuotaForecast forecast;
};

struct QuotaForecastRefresh {
    QuotaWindowForecastRefresh fiveHour;
    QuotaWindowForecastRefresh weekly;
    bool historyStored = false;
    bool historySaveFailed = false;
};

enum class CodexCostRefreshStatus {
    kAvailable,
    kCodexHomeUnavailable,
    kScanFailed,
    kNoTokenEvents,
};

struct CodexCostRefresh {
    CodexCostRefreshStatus status = CodexCostRefreshStatus::kScanFailed;
    std::optional<CodexCostSummary> localSummary;
    bool showingLastKnown = false;
    bool historyStateChanged = false;
    bool historyCacheSaveFailed = false;
    bool coverageIncomplete = false;
    std::size_t skippedCompressedFiles = 0;
    std::size_t skippedOversizedLines = 0;
    std::size_t rejectedUnsafeEntries = 0;
    std::size_t malformedLineCount = 0;
};

struct CompletedCodexRefresh {
    // CodexDataState is already the privacy-trimmed product model: it contains
    // no raw protocol lines, stderr, account identity, paths, thread IDs, or
    // session content.
    CodexDataState data;
    AppServerRefreshReport report;
    std::optional<QuotaForecastRefresh> quotaForecastUpdate;
    std::optional<CodexCostRefresh> costHistoryUpdate;
    bool succeeded = false;
    std::chrono::seconds nextRefreshDelay{60};
};

class CodexWorker {
public:
    CodexWorker() = default;
    ~CodexWorker();

    CodexWorker(const CodexWorker&) = delete;
    CodexWorker& operator=(const CodexWorker&) = delete;

    bool Start(HWND completionWindow,
               UINT completionMessage,
               std::string_view clientVersion,
               std::filesystem::path quotaHistoryPath = {},
               std::filesystem::path costHistoryCachePath = {});
    bool SetQuotaForecastEnabled(bool enabled);
    bool SetCostHistoryEnabled(bool enabled);
    bool ActivateAndRefresh();
    bool RequestRefresh();
    void PauseAndInvalidate();
    [[nodiscard]] std::optional<CompletedCodexRefresh> TakeLatest();
    [[nodiscard]] bool IsBusy() const;
    void StopAndJoin();

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    ::codex_monitor::CodexRefreshSchedule schedule_;
    std::optional<CompletedCodexRefresh> latest_;
    std::optional<std::chrono::steady_clock::time_point> nextAutomaticRefresh_;
    std::atomic<std::uint64_t> cancellationEpoch_{1};
    std::atomic<std::uint64_t> quotaForecastEpoch_{1};
    std::atomic<std::uint64_t> costHistoryEpoch_{1};
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    std::string clientVersion_;
    std::filesystem::path quotaHistoryPath_;
    std::filesystem::path costHistoryCachePath_;
    bool quotaForecastEnabled_ = false;
    bool costHistoryEnabled_ = false;
    bool started_ = false;
    std::thread thread_;
};

}  // namespace codex_monitor::codex
