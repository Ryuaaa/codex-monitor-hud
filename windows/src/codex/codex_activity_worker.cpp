#include "codex/codex_activity_worker.h"

#include <ctime>
#include <utility>

namespace codex_monitor::codex {
namespace {

constexpr std::chrono::seconds kActiveRefreshDelay{5};
constexpr std::chrono::seconds kIdleRefreshDelay{20};

}  // namespace

CodexActivityWorker::CodexActivityWorker(CodexActivityScanner scanner)
    : scanner_(std::move(scanner)) {
    if (!scanner_) scanner_ = ScanRecentCodexActivity;
}

CodexActivityWorker::~CodexActivityWorker() {
    StopAndJoin();
}

bool CodexActivityWorker::Start(HWND completionWindow,
                                UINT completionMessage,
                                std::filesystem::path sessionsRoot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || thread_.joinable() || stopRequested_ ||
        !completionWindow || completionMessage < WM_APP || !scanner_) {
        return false;
    }
    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    sessionsRoot_ = std::move(sessionsRoot);
    try {
        thread_ = std::thread(&CodexActivityWorker::Run, this);
    } catch (...) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        sessionsRoot_.clear();
        return false;
    }
    started_ = true;
    return true;
}

bool CodexActivityWorker::ActivateAndRefresh() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_) return false;
        if (!active_) {
            active_ = true;
            pending_ = true;
            nextRefresh_.reset();
            changed = true;
        } else if (!pending_ && !busy_) {
            pending_ = true;
            nextRefresh_.reset();
            changed = true;
        }
    }
    if (changed) wake_.notify_one();
    return changed;
}

bool CodexActivityWorker::RequestRefresh() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_ || !active_ || pending_) return false;
        pending_ = true;
        nextRefresh_.reset();
        changed = true;
    }
    if (changed) wake_.notify_one();
    return changed;
}

void CodexActivityWorker::PauseAndInvalidate() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stopRequested_) return;
        active_ = false;
        pending_ = false;
        nextRefresh_.reset();
        latest_.reset();
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_.notify_one();
}

std::optional<CompletedCodexActivityRefresh>
CodexActivityWorker::TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<CompletedCodexActivityRefresh> result = std::move(latest_);
    latest_.reset();
    return result;
}

bool CodexActivityWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && (pending_ || busy_);
}

void CodexActivityWorker::StopAndJoin() {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !thread_.joinable()) {
            latest_.reset();
            return;
        }
        stopRequested_ = true;
        active_ = false;
        pending_ = false;
        busy_ = false;
        nextRefresh_.reset();
        latest_.reset();
        cancellationEpoch_.fetch_add(1, std::memory_order_acq_rel);
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        sessionsRoot_.clear();
        started_ = false;
        if (thread_.joinable()) threadToJoin = std::move(thread_);
    }
    wake_.notify_one();
    if (threadToJoin.joinable()) threadToJoin.join();
}

void CodexActivityWorker::Run() {
    for (;;) {
        std::uint64_t epoch = 0;
        std::filesystem::path sessionsRoot;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stopRequested_) return;
                const auto now = std::chrono::steady_clock::now();
                if (active_ && nextRefresh_ && now >= *nextRefresh_) {
                    nextRefresh_.reset();
                    pending_ = true;
                }
                if (active_ && pending_ && !busy_) {
                    pending_ = false;
                    busy_ = true;
                    epoch =
                        cancellationEpoch_.load(std::memory_order_acquire);
                    sessionsRoot = sessionsRoot_;
                    break;
                }
                if (active_ && nextRefresh_) {
                    wake_.wait_until(lock, *nextRefresh_);
                } else {
                    wake_.wait(lock);
                }
            }
        }

        CodexActivityScanRequest request;
        request.sessionsRoot = std::move(sessionsRoot);
        request.nowUnixSeconds =
            static_cast<std::int64_t>(std::time(nullptr));
        request.shouldCancel = [this, epoch] {
            return cancellationEpoch_.load(std::memory_order_acquire) !=
                   epoch;
        };
        CodexActivityScanResult scanned;
        try {
            scanned = scanner_(request);
        } catch (...) {
            scanned.status = CodexActivityScanStatus::kIoError;
            scanned.error = std::make_error_code(std::errc::io_error);
        }

        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (stopRequested_) return;
            const bool accept = active_ &&
                cancellationEpoch_.load(std::memory_order_acquire) == epoch &&
                scanned.status != CodexActivityScanStatus::kCancelled;
            if (!accept) {
                wake_.notify_one();
                continue;
            }
            const std::chrono::seconds delay =
                scanned.available() && scanned.activeTaskCount > 0
                    ? kActiveRefreshDelay
                    : kIdleRefreshDelay;
            latest_ = CompletedCodexActivityRefresh{std::move(scanned), delay};
            nextRefresh_ = pending_
                               ? std::optional<
                                     std::chrono::steady_clock::time_point>{}
                               : std::optional{
                                     std::chrono::steady_clock::now() + delay};
            notifyWindow = completionWindow_;
            notifyMessage = completionMessage_;
        }
        if (notifyWindow && notifyMessage != 0) {
            PostMessageW(notifyWindow, notifyMessage, 0, 0);
        }
        wake_.notify_one();
    }
}

}  // namespace codex_monitor::codex
