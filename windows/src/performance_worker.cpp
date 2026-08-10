#include "performance_worker.h"

#include <system_error>
#include <utility>

namespace codex_monitor {

PerformanceWorker::~PerformanceWorker() {
    StopAndJoin();
}

bool PerformanceWorker::Start(HWND completionWindow, UINT completionMessage) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || thread_.joinable() || !completionWindow || completionMessage < WM_APP) {
        return false;
    }

    completionWindow_ = completionWindow;
    completionMessage_ = completionMessage;
    try {
        thread_ = std::thread(&PerformanceWorker::Run, this);
    } catch (const std::system_error&) {
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        return false;
    }
    started_ = true;
    return true;
}

bool PerformanceWorker::ActivateAndRequestFullSample() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        changed = schedule_.ActivateAndRequestFullSample();
    }
    if (changed) wake_.notify_one();
    return changed;
}

bool PerformanceWorker::Request(SampleMode mode) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return false;
        changed = schedule_.Request(mode);
    }
    if (changed) wake_.notify_one();
    return changed;
}

void PerformanceWorker::PauseAndInvalidate() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        schedule_.PauseAndInvalidate();
        latest_.reset();
    }
    wake_.notify_one();
}

bool PerformanceWorker::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ && schedule_.IsBusy();
}

std::optional<CompletedSample> PerformanceWorker::TakeLatest() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::optional<CompletedSample> result = std::move(latest_);
    latest_.reset();
    return result;
}

void PerformanceWorker::StopAndJoin() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && !thread_.joinable()) {
            latest_.reset();
            return;
        }
        schedule_.Stop();
        latest_.reset();
        completionWindow_ = nullptr;
        completionMessage_ = 0;
        started_ = false;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void PerformanceWorker::Run() {
    for (;;) {
        std::optional<SamplingWorkItem> item;
        bool resetBaseline = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return schedule_.HasAction(); });
            if (schedule_.IsStopped()) return;
            resetBaseline = schedule_.TakeBaselineReset();
            if (!resetBaseline) item = schedule_.TakeNext();
        }

        if (resetBaseline) {
            sampler_.ResetCpuBaseline();
            continue;
        }
        if (!item) continue;

        std::optional<PerformanceSnapshot> snapshot;
        try {
            snapshot = sampler_.Sample(item->mode);
        } catch (...) {
            // A transient allocation or native-wrapper failure must not terminate the UI process.
        }

        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool acceptResult = schedule_.Finish(*item);
            if (acceptResult && snapshot) {
                latest_ = CompletedSample{item->mode, std::move(*snapshot)};
                notifyWindow = completionWindow_;
                notifyMessage = completionMessage_;
            }
        }
        if (notifyWindow && notifyMessage != 0) {
            PostMessageW(notifyWindow, notifyMessage, 0, 0);
        }
    }
}

}  // namespace codex_monitor
