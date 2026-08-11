#pragma once

#include <windows.h>

#include "codex_activity_scan.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace codex_monitor::codex {

struct CompletedCodexActivityRefresh {
    CodexActivityScanResult activity;
    std::chrono::seconds nextRefreshDelay{20};
};

using CodexActivityScanner =
    std::function<CodexActivityScanResult(const CodexActivityScanRequest&)>;

class CodexActivityWorker {
public:
    explicit CodexActivityWorker(
        CodexActivityScanner scanner = ScanRecentCodexActivity);
    ~CodexActivityWorker();

    CodexActivityWorker(const CodexActivityWorker&) = delete;
    CodexActivityWorker& operator=(const CodexActivityWorker&) = delete;

    bool Start(HWND completionWindow,
               UINT completionMessage,
               std::filesystem::path sessionsRoot);
    bool ActivateAndRefresh();
    bool RequestRefresh();
    void PauseAndInvalidate();
    [[nodiscard]] std::optional<CompletedCodexActivityRefresh> TakeLatest();
    [[nodiscard]] bool IsBusy() const;
    void StopAndJoin();

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    CodexActivityScanner scanner_;
    std::filesystem::path sessionsRoot_;
    std::optional<CompletedCodexActivityRefresh> latest_;
    std::optional<std::chrono::steady_clock::time_point> nextRefresh_;
    std::atomic<std::uint64_t> cancellationEpoch_{1};
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    bool started_ = false;
    bool active_ = false;
    bool pending_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    std::thread thread_;
};

}  // namespace codex_monitor::codex
