#pragma once

#include "update/update_check_win32.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace codex_monitor::update {

struct WindowsUpdateWorkerTiming {
    std::chrono::seconds startupDelay{5};
    std::chrono::seconds automaticCheckInterval{24 * 60 * 60};
};

struct CompletedWindowsUpdateCheck {
    WindowsUpdateCheckResult result;
    std::string availableVersion;
    std::int64_t checkedAtUnixSeconds = 0;
    bool manual = false;
    bool fromCache = false;
    bool stateSaveFailed = false;
};

using WindowsUpdateChecker = std::function<WindowsUpdateCheckResult(
    std::string_view currentVersion,
    const WindowsUpdateCancellationCheck& cancelled)>;

class WindowsUpdateWorker {
public:
    explicit WindowsUpdateWorker(
        WindowsUpdateChecker checker = CheckForWindowsUpdate,
        WindowsUpdateWorkerTiming timing = {});
    ~WindowsUpdateWorker();

    WindowsUpdateWorker(const WindowsUpdateWorker&) = delete;
    WindowsUpdateWorker& operator=(const WindowsUpdateWorker&) = delete;

    bool Start(HWND completionWindow,
               UINT completionMessage,
               std::string_view currentVersion,
               std::filesystem::path statePath);
    bool RequestManualCheck();
    [[nodiscard]] std::optional<CompletedWindowsUpdateCheck> TakeLatest();
    [[nodiscard]] bool IsBusy() const;
    void RequestStop() noexcept;
    void StopAndJoin();

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    WindowsUpdateChecker checker_;
    WindowsUpdateWorkerTiming timing_;
    std::optional<CompletedWindowsUpdateCheck> latest_;
    std::optional<std::chrono::steady_clock::time_point>
        nextAutomaticCheck_;
    std::atomic<std::uint64_t> cancellationEpoch_{1};
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    std::string currentVersion_;
    std::filesystem::path statePath_;
    bool manualPending_ = false;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool started_ = false;
    std::thread thread_;
};

}  // namespace codex_monitor::update
