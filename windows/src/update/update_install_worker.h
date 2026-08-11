#pragma once

#include "update/update_install_win32.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace codex_monitor::update {

using WindowsUpdateInstallOperation = std::function<
    WindowsUpdateInstallResult(
        const WindowsUpdateInstallRequest& request,
        const WindowsUpdateInstallCancellationCheck& cancelled)>;

class WindowsUpdateInstallWorker {
public:
    explicit WindowsUpdateInstallWorker(
        WindowsUpdateInstallOperation operation =
            PrepareAndLaunchWindowsUpdate);
    ~WindowsUpdateInstallWorker();

    WindowsUpdateInstallWorker(const WindowsUpdateInstallWorker&) = delete;
    WindowsUpdateInstallWorker& operator=(
        const WindowsUpdateInstallWorker&) = delete;

    bool Start(HWND completionWindow, UINT completionMessage);
    bool Request(WindowsUpdateInstallRequest request);
    [[nodiscard]] std::optional<WindowsUpdateInstallResult> TakeLatest();
    [[nodiscard]] bool IsBusy() const;
    void RequestStop() noexcept;
    void StopAndJoin();

private:
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    WindowsUpdateInstallOperation operation_;
    std::optional<WindowsUpdateInstallRequest> pending_;
    std::optional<WindowsUpdateInstallResult> latest_;
    std::atomic<std::uint64_t> cancellationEpoch_{1};
    HWND completionWindow_ = nullptr;
    UINT completionMessage_ = 0;
    bool busy_ = false;
    bool stopRequested_ = false;
    bool started_ = false;
    std::thread thread_;
};

}  // namespace codex_monitor::update
