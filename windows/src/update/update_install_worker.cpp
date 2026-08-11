#include "update/update_install_worker.h"

#include <utility>

namespace codex_monitor::update {

WindowsUpdateInstallWorker::WindowsUpdateInstallWorker(
    WindowsUpdateInstallOperation operation)
    : operation_(std::move(operation)) {
    if (!operation_) operation_ = PrepareAndLaunchWindowsUpdate;
}

WindowsUpdateInstallWorker::~WindowsUpdateInstallWorker() {
    StopAndJoin();
}

bool WindowsUpdateInstallWorker::Start(
    HWND completionWindow,
    UINT completionMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || thread_.joinable() || stopRequested_ ||
        !completionWindow || completionMessage < WM_APP || !operation_) {
        return false;
    }
    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    try {
        thread_ = std::thread(&WindowsUpdateInstallWorker::Run, this);
    } catch (...) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        return false;
    }
    started_ = true;
    return true;
}

bool WindowsUpdateInstallWorker::Request(
    WindowsUpdateInstallRequest request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_ || busy_ || pending_.has_value()) {
            return false;
        }
        pending_ = std::move(request);
        latest_.reset();
    }
    wake_.notify_one();
    return true;
}

std::optional<WindowsUpdateInstallResult>
WindowsUpdateInstallWorker::TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<WindowsUpdateInstallResult> result = std::move(latest_);
    latest_.reset();
    return result;
}

bool WindowsUpdateInstallWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && (busy_ || pending_.has_value());
}

void WindowsUpdateInstallWorker::RequestStop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopRequested_ && !started_) return;
        stopRequested_ = true;
        started_ = false;
        pending_.reset();
        latest_.reset();
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_.notify_one();
}

void WindowsUpdateInstallWorker::StopAndJoin() {
    RequestStop();
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        threadToJoin = std::move(thread_);
    }
    if (threadToJoin.joinable()) threadToJoin.join();
}

void WindowsUpdateInstallWorker::Run() {
    for (;;) {
        WindowsUpdateInstallRequest request;
        std::uint64_t requestEpoch = 0;
        HWND busyWindow = nullptr;
        UINT busyMessage = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] {
                return stopRequested_ || pending_.has_value();
            });
            if (stopRequested_) return;
            request = std::move(*pending_);
            pending_.reset();
            busy_ = true;
            requestEpoch = cancellationEpoch_.load(
                std::memory_order_acquire);
            busyWindow = completionWindow_;
            busyMessage = completionMessage_;
        }
        if (busyWindow && busyMessage != 0U) {
            PostMessageW(busyWindow, busyMessage, 0, 0);
        }

        WindowsUpdateInstallResult completed;
        try {
            completed = operation_(
                request, [this, requestEpoch] {
                    return cancellationEpoch_.load(
                               std::memory_order_acquire) != requestEpoch;
                });
        } catch (...) {
            completed.status = WindowsUpdateInstallStatus::kUnexpected;
            completed.targetVersion = request.release.version.canonical;
        }

        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (stopRequested_ ||
                cancellationEpoch_.load(std::memory_order_acquire) !=
                    requestEpoch) {
                continue;
            }
            latest_ = std::move(completed);
            notifyWindow = completionWindow_;
            notifyMessage = completionMessage_;
        }
        if (notifyWindow && notifyMessage != 0U) {
            PostMessageW(notifyWindow, notifyMessage, 0, 0);
        }
    }
}

}  // namespace codex_monitor::update
